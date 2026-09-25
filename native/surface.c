#include "internal.h"
#include <unistd.h>
#include <drm_fourcc.h>
#include <wlr/render/drm_syncobj.h>
#include <wlr/util/region.h>
#include "fractional-scale-v1-protocol.h"
#include "linux-drm-syncobj-v1-protocol.h"
#include "presentation-time-protocol.h"
#include "viewporter-protocol.h"

#define COMPOSITOR_VERSION 6

enum {
    STATE_BUFFER = 1 << 0, STATE_SURFACE_DAMAGE = 1 << 1, STATE_BUFFER_DAMAGE = 1 << 2,
    STATE_INPUT = 1 << 4, STATE_TRANSFORM = 1 << 5, STATE_SCALE = 1 << 6,
    STATE_FRAME = 1 << 7, STATE_VIEWPORT = 1 << 8, STATE_OFFSET = 1 << 9,
};

struct release {
    struct wlr_drm_syncobj_timeline *timeline;
    uint64_t point;
    int refs;
};

struct release_hold {
    struct release *release;
    struct wl_listener buffer_release;
};

struct feedback {
    struct wl_list resources, link;
    struct wlr_output *output;
    bool zero_copy, committed;
    size_t commit_seq;
    struct wl_listener commit, present, output_destroy;
};

struct acquire_wait {
    struct surface *surface;
    struct surface_state *state;
    struct wl_list link;
    struct wlr_drm_syncobj_timeline_waiter waiter;
};

struct surface_output {
    struct wlr_output *output;
    struct surface *surface;
    struct wl_list link;
    struct wl_listener bind, destroy;
};

struct subsurface {
    struct wl_resource *resource;
    struct surface *surface, *parent;
    struct wl_list link, pending_link;
    int x, y, pending_x, pending_y;
    bool synchronized, has_cache, added;
    struct surface_state *cache;
    struct wl_listener parent_destroy;
};

struct synced_entry {
    struct surface_synced *owner;
    struct wl_list link;
    max_align_t data[];
};

struct syncobj_surface {
    struct wl_resource *resource;
    struct surface *surface;
};

static const struct wl_surface_interface surface_impl;
static const struct wl_region_interface region_impl;
static const struct surface_role subsurface_role;
static int drm_fd = -1;

static void destroy_resource(struct wl_client *client, struct wl_resource *resource) {
    wl_resource_destroy(resource);
}

static void release_unref(struct release *r) {
    if (!r || --r->refs > 0) return;
    wlr_drm_syncobj_timeline_signal(r->timeline, r->point);
    wlr_drm_syncobj_timeline_unref(r->timeline);
    free(r);
}

static void hold_released(struct wl_listener *listener, void *data) {
    struct release_hold *hold = wl_container_of(listener, hold, buffer_release);
    detach(&hold->buffer_release);
    release_unref(hold->release);
    free(hold);
}

void surface_release_after(struct surface *surface, struct wlr_buffer *consumer) {
    if (!surface->release || !consumer) return;
    struct release_hold *hold = calloc(1, sizeof(*hold));
    if (!hold) return;
    hold->release = surface->release;
    hold->release->refs++;
    listen(&hold->buffer_release, &consumer->events.release, hold_released);
}

pixman_region32_t *region_from_resource(struct wl_resource *resource) {
    return wl_resource_get_user_data(resource);
}

static void region_add(struct wl_client *client, struct wl_resource *resource, int32_t x,
        int32_t y, int32_t width, int32_t height) {
    pixman_region32_union_rect(region_from_resource(resource), region_from_resource(resource),
        x, y, width, height);
}

static void region_subtract(struct wl_client *client, struct wl_resource *resource, int32_t x,
        int32_t y, int32_t width, int32_t height) {
    pixman_region32_t *region = region_from_resource(resource), rect;
    pixman_region32_init_rect(&rect, x, y, width, height);
    pixman_region32_subtract(region, region, &rect);
    pixman_region32_fini(&rect);
}

static const struct wl_region_interface region_impl = {
    .destroy = destroy_resource, .add = region_add, .subtract = region_subtract,
};

static void region_free(struct wl_resource *resource) {
    pixman_region32_t *region = wl_resource_get_user_data(resource);
    pixman_region32_fini(region);
    free(region);
}

static void state_init(struct surface_state *state) {
    *state = (struct surface_state){ .scale = 1, .transform = WL_OUTPUT_TRANSFORM_NORMAL };
    pixman_region32_init(&state->surface_damage);
    pixman_region32_init(&state->buffer_damage);
    pixman_region32_init_rect(&state->input, INT32_MIN, INT32_MIN, UINT32_MAX, UINT32_MAX);
    wl_list_init(&state->frames);
    wl_list_init(&state->feedbacks);
    wl_list_init(&state->waits);
    wl_list_init(&state->synced);
    wl_list_init(&state->link);
}

static void entry_free(struct synced_entry *entry) {
    if (entry->owner->impl->finish) entry->owner->impl->finish(entry->data);
    wl_list_remove(&entry->link);
    free(entry);
}

static void synced_move(struct surface_synced *synced, void *dst, void *src) {
    if (synced->impl->move) synced->impl->move(dst, src);
    else memcpy(dst, src, synced->impl->size);
}

static struct synced_entry *entry_create(struct surface_synced *synced, struct surface_state *state,
        bool copy) {
    struct synced_entry *entry = calloc(1, sizeof(*entry) + synced->impl->size);
    if (!entry) return NULL;
    entry->owner = synced;
    if (synced->impl->init) synced->impl->init(entry->data);
    else if (copy) memcpy(entry->data, synced->pending, synced->impl->size);
    wl_list_insert(state->synced.prev, &entry->link);
    return entry;
}

bool surface_synced_init(struct surface_synced *synced, struct surface *surface,
        const struct surface_synced_impl *impl, void *pending, void *current) {
    *synced = (struct surface_synced){ .surface = surface, .impl = impl, .pending = pending,
        .current = current };
    if (impl->init) {
        impl->init(pending);
        impl->init(current);
    }
    struct surface_state *state;
    wl_list_for_each(state, &surface->cached, link)
        if (!entry_create(synced, state, true)) return false;
    wl_list_insert(surface->synced.prev, &synced->link);
    return true;
}

void surface_synced_finish(struct surface_synced *synced) {
    if (!synced->surface) return;
    struct surface_state *state;
    wl_list_for_each(state, &synced->surface->cached, link) {
        struct synced_entry *entry, *next;
        wl_list_for_each_safe(entry, next, &state->synced, link)
            if (entry->owner == synced) entry_free(entry);
    }
    wl_list_remove(&synced->link);
    if (synced->impl->finish) {
        synced->impl->finish(synced->pending);
        synced->impl->finish(synced->current);
    }
    synced->surface = NULL;
}

static void resources_destroy(struct wl_list *list) {
    struct wl_resource *resource, *next;
    wl_resource_for_each_safe(resource, next, list) wl_resource_destroy(resource);
}

static void feedbacks_discard(struct wl_list *list) {
    struct wl_resource *resource, *next;
    wl_resource_for_each_safe(resource, next, list) {
        wp_presentation_feedback_send_discarded(resource);
        wl_resource_destroy(resource);
    }
}

static void state_finish(struct surface_state *state) {
    struct synced_entry *entry, *next;
    wl_list_for_each_safe(entry, next, &state->synced, link) entry_free(entry);
    wlr_buffer_unlock(state->buffer);
    state->buffer = NULL;
    pixman_region32_fini(&state->surface_damage);
    pixman_region32_fini(&state->buffer_damage);
    pixman_region32_fini(&state->input);
    resources_destroy(&state->frames);
    feedbacks_discard(&state->feedbacks);
    wlr_drm_syncobj_timeline_unref(state->acquire);
    wlr_drm_syncobj_timeline_unref(state->release);
    state->acquire = state->release = NULL;
}

static void state_move(struct surface_state *dst, struct surface_state *src) {
    dst->width = src->width;
    dst->height = src->height;
    dst->buffer_width = src->buffer_width;
    dst->buffer_height = src->buffer_height;
    if (src->committed & STATE_SCALE) dst->scale = src->scale;
    if (src->committed & STATE_TRANSFORM) dst->transform = src->transform;
    if (src->committed & STATE_OFFSET) {
        dst->dx = src->dx;
        dst->dy = src->dy;
    } else {
        dst->dx = dst->dy = 0;
    }
    src->dx = src->dy = 0;
    if (src->committed & STATE_BUFFER) {
        wlr_buffer_unlock(dst->buffer);
        dst->buffer = src->buffer;
        src->buffer = NULL;
        wlr_drm_syncobj_timeline_unref(dst->acquire);
        wlr_drm_syncobj_timeline_unref(dst->release);
        dst->acquire = src->acquire;
        dst->release = src->release;
        dst->acquire_point = src->acquire_point;
        dst->release_point = src->release_point;
        src->acquire = src->release = NULL;
    }
    pixman_region32_clear(&dst->surface_damage);
    pixman_region32_clear(&dst->buffer_damage);
    if (src->committed & STATE_SURFACE_DAMAGE) {
        pixman_region32_copy(&dst->surface_damage, &src->surface_damage);
        pixman_region32_clear(&src->surface_damage);
    }
    if (src->committed & STATE_BUFFER_DAMAGE) {
        pixman_region32_copy(&dst->buffer_damage, &src->buffer_damage);
        pixman_region32_clear(&src->buffer_damage);
    }
    if (src->committed & STATE_INPUT) pixman_region32_copy(&dst->input, &src->input);
    if (src->committed & STATE_VIEWPORT) dst->viewport = src->viewport;
    wl_list_insert_list(dst->frames.prev, &src->frames);
    wl_list_init(&src->frames);
    wl_list_insert_list(dst->feedbacks.prev, &src->feedbacks);
    wl_list_init(&src->feedbacks);
    dst->committed = src->committed;
    src->committed = 0;
}

struct surface *surface_from_resource(struct wl_resource *resource) {
    return wl_resource_get_user_data(resource);
}

bool surface_state_has_buffer(const struct surface_state *state) {
    return state->buffer_width > 0 && state->buffer_height > 0;
}

bool surface_has_buffer(struct surface *surface) {
    return surface->texture != NULL;
}

static bool state_buffer_size(struct surface_state *state, int *width, int *height) {
    *width = state->buffer_width;
    *height = state->buffer_height;
    wlr_output_transform_coords(state->transform, width, height);
    return *width && *height;
}

static void finalize_pending(struct surface *surface) {
    struct surface_state *p = &surface->pending;
    if (p->committed & STATE_BUFFER) {
        p->buffer_width = p->buffer ? p->buffer->width : 0;
        p->buffer_height = p->buffer ? p->buffer->height : 0;
    }
    if (!p->viewport.has_src && (p->buffer_width % p->scale || p->buffer_height % p->scale) &&
            surface->role && strcmp(surface->role->name, "wl_pointer-cursor")) {
        surface_reject_pending(surface, surface->resource, WL_SURFACE_ERROR_INVALID_SIZE,
            "buffer size is not divisible by scale");
        return;
    }
    int width, height;
    if (!state_buffer_size(p, &width, &height)) {
        p->width = p->height = 0;
    } else if (p->viewport.has_dst) {
        p->width = p->viewport.dst_width;
        p->height = p->viewport.dst_height;
    } else if (p->viewport.has_src) {
        p->width = (int)p->viewport.src.width;
        p->height = (int)p->viewport.src.height;
    } else {
        p->width = width / p->scale;
        p->height = height / p->scale;
    }
    if (p->viewport.has_src && width && height &&
            (p->viewport.src.x + p->viewport.src.width > width / p->scale ||
             p->viewport.src.y + p->viewport.src.height > height / p->scale)) {
        surface_reject_pending(surface, surface->viewport ? surface->viewport : surface->resource,
            WP_VIEWPORT_ERROR_OUT_OF_BUFFER, "viewport source is outside the buffer");
        return;
    }
    if (p->viewport.has_src && !p->viewport.has_dst &&
            (p->viewport.src.width != (int)p->viewport.src.width ||
             p->viewport.src.height != (int)p->viewport.src.height)) {
        surface_reject_pending(surface, surface->viewport ? surface->viewport : surface->resource,
            WP_VIEWPORT_ERROR_BAD_SIZE, "viewport source size is not integer");
        return;
    }
}

static void buffer_damage(struct surface *surface, pixman_region32_t *out) {
    struct surface_state *s = &surface->current;
    pixman_region32_copy(out, &s->surface_damage);
    int width, height;
    state_buffer_size(s, &width, &height);
    if (s->viewport.has_dst && s->width && s->height) {
        double src_w = s->viewport.has_src ? s->viewport.src.width : (double)width / s->scale;
        double src_h = s->viewport.has_src ? s->viewport.src.height : (double)height / s->scale;
        wlr_region_scale_xy(out, out, src_w / s->width, src_h / s->height);
    }
    if (s->viewport.has_src)
        pixman_region32_translate(out, (int)floor(s->viewport.src.x), (int)floor(s->viewport.src.y));
    wlr_region_scale(out, out, s->scale);
    wlr_region_transform(out, out, wlr_output_transform_invert(s->transform), width, height);
    pixman_region32_union(out, out, &s->buffer_damage);
    pixman_region32_intersect_rect(out, out, 0, 0, s->buffer_width, s->buffer_height);
}

static void apply_buffer(struct surface *surface) {
    struct wlr_buffer *next = surface->current.buffer;
    release_unref(surface->release);
    surface->release = NULL;
    if (!next) {
        wlr_texture_destroy(surface->texture);
        surface->texture = NULL;
        wlr_buffer_unlock(surface->buffer);
        surface->buffer = NULL;
        return;
    }
    struct wlr_dmabuf_attributes dmabuf;
    bool gpu = wlr_buffer_get_dmabuf(next, &dmabuf);
    pixman_region32_t damage;
    pixman_region32_init(&damage);
    buffer_damage(surface, &damage);
    bool reused = !gpu && surface->texture &&
        wlr_texture_update_from_buffer(surface->texture, next, &damage);
    pixman_region32_fini(&damage);
    if (!reused) {
        struct wlr_texture *texture = wlr_texture_from_buffer(surface->server->renderer, next);
        if (!texture) wlr_log(WLR_ERROR, "tomoe: client buffer upload failed");
        wlr_texture_destroy(surface->texture);
        surface->texture = texture;
    }
    wlr_buffer_unlock(surface->buffer);
    bool keep = gpu || !surface->role || surface->role->keep_buffer;
    surface->buffer = keep ? wlr_buffer_lock(next) : NULL;
    if (surface->current.release) {
        surface->release = calloc(1, sizeof(*surface->release));
        if (surface->release) {
            surface->release->timeline = wlr_drm_syncobj_timeline_ref(surface->current.release);
            surface->release->point = surface->current.release_point;
            surface->release->refs = 1;
        }
    }
}

static void feedback_free(struct feedback *f) {
    feedbacks_discard(&f->resources);
    detach(&f->commit);
    detach(&f->present);
    detach(&f->output_destroy);
    wl_list_remove(&f->link);
    free(f);
}

static void subsurface_consider_map(struct subsurface *sub);
static void surface_unlock(struct surface *surface, struct surface_state *state);

static void apply_state(struct surface *surface, struct surface_state *next) {
    bool buffer = next->committed & STATE_BUFFER;
    if (buffer && !next->buffer) surface_unmap(surface);
    feedbacks_discard(&surface->current.feedbacks);
    state_move(&surface->current, next);
    struct surface_synced *synced;
    if (next == &surface->pending) {
        wl_list_for_each(synced, &surface->synced, link)
            synced_move(synced, synced->current, synced->pending);
    } else {
        struct synced_entry *entry, *next_entry;
        wl_list_for_each_safe(entry, next_entry, &next->synced, link) {
            synced_move(entry->owner, entry->owner->current, entry->data);
            entry_free(entry);
        }
    }
    if (buffer) apply_buffer(surface);
    pixman_region32_intersect_rect(&surface->input_region, &surface->current.input, 0, 0,
        surface->current.width, surface->current.height);
    struct subsurface *sub, *next_sub;
    wl_list_for_each(sub, &surface->pending_below, pending_link) {
        wl_list_remove(&sub->link);
        wl_list_insert(surface->below.prev, &sub->link);
    }
    wl_list_for_each(sub, &surface->pending_above, pending_link) {
        wl_list_remove(&sub->link);
        wl_list_insert(surface->above.prev, &sub->link);
    }
    struct wl_list *lists[] = { &surface->below, &surface->above };
    for (int i = 0; i < 2; i++) wl_list_for_each_safe(sub, next_sub, lists[i], link) {
        sub->x = sub->pending_x;
        sub->y = sub->pending_y;
        if (sub->has_cache && sub->synchronized) {
            sub->has_cache = false;
            surface_unlock(sub->surface, sub->cache);
        }
        if (!sub->added) {
            sub->added = true;
            subsurface_consider_map(sub);
        }
    }
    wl_list_for_each(synced, &surface->synced, link)
        if (synced->impl->commit) synced->impl->commit(synced);
    if (surface->role && surface->role->commit && (surface->role_resource || surface->role->no_object))
        surface->role->commit(surface);
    wl_signal_emit_mutable(&surface->events.commit, surface);
    schedule_scene(surface->server);
    wlr_buffer_unlock(surface->current.buffer);
    surface->current.buffer = NULL;
}

static void flush_cached(struct surface *surface) {
    while (!wl_list_empty(&surface->cached)) {
        struct surface_state *state = wl_container_of(surface->cached.next, state, link);
        if (state->locks) return;
        wl_list_remove(&state->link);
        apply_state(surface, state);
        state_finish(state);
        free(state);
    }
}

static void surface_unlock(struct surface *surface, struct surface_state *state) {
    if (--state->locks == 0) flush_cached(surface);
}

static bool cache_pending(struct surface *surface) {
    struct surface_state *cached = calloc(1, sizeof(*cached));
    if (!cached) {
        wl_resource_post_no_memory(surface->resource);
        return false;
    }
    state_init(cached);
    state_move(cached, &surface->pending);
    struct surface_synced *synced;
    wl_list_for_each(synced, &surface->synced, link) {
        struct synced_entry *entry = entry_create(synced, cached, false);
        if (!entry) {
            state_finish(cached);
            free(cached);
            wl_resource_post_no_memory(surface->resource);
            return false;
        }
        synced_move(synced, entry->data, synced->pending);
    }
    cached->locks = surface->pending.locks;
    surface->pending.locks = 0;
    wl_list_insert(surface->cached.prev, &cached->link);
    surface->last_cached = cached;
    return true;
}

static bool subsurface_synchronized(struct subsurface *sub) {
    for (; sub; sub = sub->parent ? sub->parent->subsurface : NULL)
        if (sub->synchronized) return true;
    return false;
}

static void acquire_ready(struct wlr_drm_syncobj_timeline_waiter *waiter) {
    struct acquire_wait *wait = wl_container_of(waiter, wait, waiter);
    struct surface *surface = wait->surface;
    struct surface_state *state = wait->state;
    wlr_drm_syncobj_timeline_waiter_finish(&wait->waiter);
    wl_list_remove(&wait->link);
    free(wait);
    surface_unlock(surface, state);
}

static bool syncobj_check(struct surface *surface) {
    struct surface_state *p = &surface->pending;
    bool attached = (p->committed & STATE_BUFFER) && p->buffer;
    if (!surface->syncobj) return true;
    struct wl_resource *resource = surface->syncobj;
    const char *error = NULL;
    uint32_t code = 0;
    if ((p->acquire || p->release) && !attached) {
        error = "sync point set but no buffer attached";
        code = WP_LINUX_DRM_SYNCOBJ_SURFACE_V1_ERROR_NO_BUFFER;
    } else if (attached && !p->acquire) {
        error = "buffer attached but no acquire point set";
        code = WP_LINUX_DRM_SYNCOBJ_SURFACE_V1_ERROR_NO_ACQUIRE_POINT;
    } else if (attached && !p->release) {
        error = "buffer attached but no release point set";
        code = WP_LINUX_DRM_SYNCOBJ_SURFACE_V1_ERROR_NO_RELEASE_POINT;
    } else if (p->acquire && p->acquire == p->release && p->acquire_point >= p->release_point) {
        error = "acquire and release points conflict";
        code = WP_LINUX_DRM_SYNCOBJ_SURFACE_V1_ERROR_CONFLICTING_POINTS;
    } else if (attached) {
        struct wlr_dmabuf_attributes dmabuf;
        if (!wlr_buffer_get_dmabuf(p->buffer, &dmabuf)) {
            error = "buffer is not a dmabuf";
            code = WP_LINUX_DRM_SYNCOBJ_SURFACE_V1_ERROR_UNSUPPORTED_BUFFER;
        }
    }
    if (error) {
        surface_reject_pending(surface, resource, code, error);
        return false;
    }
    return true;
}

static void surface_commit(struct wl_client *client, struct wl_resource *resource) {
    struct surface *surface = surface_from_resource(resource);
    surface->pending_rejected = false;
    finalize_pending(surface);
    if (!surface->pending_rejected && surface->role && surface->role->client_commit &&
            (surface->role_resource || surface->role->no_object))
        surface->role->client_commit(surface);
    if (!surface->pending_rejected) wl_signal_emit_mutable(&surface->events.client_commit, NULL);
    if (surface->pending_rejected || !syncobj_check(surface)) return;
    struct surface_state *p = &surface->pending;
    bool wait = false;
    if (p->acquire) {
        bool ready = false;
        if (!wlr_drm_syncobj_timeline_check(p->acquire, p->acquire_point,
                DRM_SYNCOBJ_WAIT_FLAGS_WAIT_AVAILABLE, &ready)) {
            wl_resource_post_no_memory(resource);
            return;
        }
        wait = !ready;
    }
    struct subsurface *sub = surface->subsurface;
    bool synced = sub && subsurface_synchronized(sub);
    bool lock_parent = synced && !sub->has_cache;
    if (sub && !synced && sub->has_cache) {
        sub->has_cache = false;
        surface_unlock(surface, sub->cache);
    }
    if (!wait && !lock_parent && !synced && wl_list_empty(&surface->cached)) {
        apply_state(surface, p);
        return;
    }
    if (lock_parent) p->locks++;
    if (wait) p->locks++;
    if (!cache_pending(surface)) return;
    struct surface_state *cached = surface->last_cached;
    if (lock_parent) {
        sub->has_cache = true;
        sub->cache = cached;
    }
    if (wait) {
        struct acquire_wait *w = calloc(1, sizeof(*w));
        if (!w || !wlr_drm_syncobj_timeline_waiter_init(&w->waiter, cached->acquire,
                cached->acquire_point, DRM_SYNCOBJ_WAIT_FLAGS_WAIT_AVAILABLE,
                wl_display_get_event_loop(surface->server->display), acquire_ready)) {
            free(w);
            wl_resource_post_no_memory(resource);
            return;
        }
        w->surface = surface;
        w->state = cached;
        wl_list_insert(&cached->waits, &w->link);
    }
    flush_cached(surface);
}

static void surface_attach(struct wl_client *client, struct wl_resource *resource,
        struct wl_resource *buffer_resource, int32_t dx, int32_t dy) {
    struct surface *surface = surface_from_resource(resource);
    struct surface_state *p = &surface->pending;
    if ((dx || dy) && wl_resource_get_version(resource) >= WL_SURFACE_OFFSET_SINCE_VERSION) {
        wl_resource_post_error(resource, WL_SURFACE_ERROR_INVALID_OFFSET,
            "non-zero attach offset is not allowed");
        return;
    }
    struct wlr_buffer *buffer = NULL;
    if (buffer_resource && !(buffer = wlr_buffer_try_from_resource(buffer_resource))) {
        wl_resource_post_error(resource, 0, "unknown buffer type");
        return;
    }
    wlr_buffer_unlock(p->buffer);
    p->buffer = buffer;
    p->committed |= STATE_BUFFER;
    if (dx || dy) {
        p->dx = dx;
        p->dy = dy;
        p->committed |= STATE_OFFSET;
    }
}

static void surface_damage(struct wl_client *client, struct wl_resource *resource, int32_t x,
        int32_t y, int32_t width, int32_t height) {
    struct surface *surface = surface_from_resource(resource);
    if (width < 0 || height < 0) return;
    surface->pending.committed |= STATE_SURFACE_DAMAGE;
    pixman_region32_union_rect(&surface->pending.surface_damage, &surface->pending.surface_damage,
        x, y, width, height);
}

static void surface_damage_buffer(struct wl_client *client, struct wl_resource *resource,
        int32_t x, int32_t y, int32_t width, int32_t height) {
    struct surface *surface = surface_from_resource(resource);
    if (width < 0 || height < 0) return;
    surface->pending.committed |= STATE_BUFFER_DAMAGE;
    pixman_region32_union_rect(&surface->pending.buffer_damage, &surface->pending.buffer_damage,
        x, y, width, height);
}

static void callback_free(struct wl_resource *resource) {
    wl_list_remove(wl_resource_get_link(resource));
}

static void surface_frame(struct wl_client *client, struct wl_resource *resource, uint32_t id) {
    struct surface *surface = surface_from_resource(resource);
    struct wl_resource *callback = wl_resource_create(client, &wl_callback_interface, 1, id);
    if (!callback) {
        wl_resource_post_no_memory(resource);
        return;
    }
    wl_resource_set_implementation(callback, NULL, NULL, callback_free);
    wl_list_insert(surface->pending.frames.prev, wl_resource_get_link(callback));
    surface->pending.committed |= STATE_FRAME;
}

static void surface_set_opaque_region(struct wl_client *client, struct wl_resource *resource,
        struct wl_resource *region) {
}

static void surface_set_input_region(struct wl_client *client, struct wl_resource *resource,
        struct wl_resource *region) {
    struct surface *surface = surface_from_resource(resource);
    surface->pending.committed |= STATE_INPUT;
    if (region) {
        pixman_region32_copy(&surface->pending.input, region_from_resource(region));
    } else {
        pixman_region32_fini(&surface->pending.input);
        pixman_region32_init_rect(&surface->pending.input, INT32_MIN, INT32_MIN, UINT32_MAX,
            UINT32_MAX);
    }
}

static void surface_set_buffer_transform(struct wl_client *client, struct wl_resource *resource,
        int32_t transform) {
    if (transform < WL_OUTPUT_TRANSFORM_NORMAL || transform > WL_OUTPUT_TRANSFORM_FLIPPED_270) {
        wl_resource_post_error(resource, WL_SURFACE_ERROR_INVALID_TRANSFORM,
            "invalid transform %d", transform);
        return;
    }
    struct surface *surface = surface_from_resource(resource);
    surface->pending.committed |= STATE_TRANSFORM;
    surface->pending.transform = transform;
}

static void surface_set_buffer_scale(struct wl_client *client, struct wl_resource *resource,
        int32_t scale) {
    if (scale <= 0) {
        wl_resource_post_error(resource, WL_SURFACE_ERROR_INVALID_SCALE, "invalid scale %d", scale);
        return;
    }
    struct surface *surface = surface_from_resource(resource);
    surface->pending.committed |= STATE_SCALE;
    surface->pending.scale = scale;
}

static void surface_offset(struct wl_client *client, struct wl_resource *resource, int32_t x,
        int32_t y) {
    struct surface *surface = surface_from_resource(resource);
    surface->pending.committed |= STATE_OFFSET;
    surface->pending.dx = x;
    surface->pending.dy = y;
}

static const struct wl_surface_interface surface_impl = {
    .destroy = destroy_resource, .attach = surface_attach, .damage = surface_damage,
    .frame = surface_frame, .set_opaque_region = surface_set_opaque_region,
    .set_input_region = surface_set_input_region, .commit = surface_commit,
    .set_buffer_transform = surface_set_buffer_transform,
    .set_buffer_scale = surface_set_buffer_scale, .damage_buffer = surface_damage_buffer,
    .offset = surface_offset,
};

static void surface_output_free(struct surface_output *so) {
    detach(&so->bind);
    detach(&so->destroy);
    wl_list_remove(&so->link);
    free(so);
}

static void surface_state_waits_free(struct surface_state *state) {
    struct acquire_wait *w, *next;
    wl_list_for_each_safe(w, next, &state->waits, link) {
        wlr_drm_syncobj_timeline_waiter_finish(&w->waiter);
        wl_list_remove(&w->link);
        free(w);
    }
}

static void surface_free(struct wl_resource *resource) {
    struct surface *surface = surface_from_resource(resource);
    surface_unmap(surface);
    detach(&surface->role_resource_destroy);
    if (surface->role && surface->role->destroy &&
            (surface->role_resource || surface->role->no_object))
        surface->role->destroy(surface);
    wl_signal_emit_mutable(&surface->events.destroy, surface);
    struct surface_state *state, *next;
    wl_list_for_each_safe(state, next, &surface->cached, link) {
        surface_state_waits_free(state);
        wl_list_remove(&state->link);
        state_finish(state);
        free(state);
    }
    state_finish(&surface->pending);
    state_finish(&surface->current);
    struct surface_output *so, *so_next;
    wl_list_for_each_safe(so, so_next, &surface->outputs, link) surface_output_free(so);
    release_unref(surface->release);
    wlr_texture_destroy(surface->texture);
    wlr_buffer_unlock(surface->buffer);
    pixman_region32_fini(&surface->input_region);
    if (surface->viewport) wl_resource_set_user_data(surface->viewport, NULL);
    if (surface->fractional) wl_resource_set_user_data(surface->fractional, NULL);
    if (surface->syncobj) {
        struct syncobj_surface *sync = wl_resource_get_user_data(surface->syncobj);
        if (sync) sync->surface = NULL;
    }
    wl_list_remove(&surface->link);
    schedule_scene(surface->server);
    free(surface);
}

static void create_surface(struct wl_client *client, struct wl_resource *compositor, uint32_t id) {
    struct tomoe *s = wl_resource_get_user_data(compositor);
    struct surface *surface = calloc(1, sizeof(*surface));
    struct wl_resource *resource = surface ? wl_resource_create(client, &wl_surface_interface,
        wl_resource_get_version(compositor), id) : NULL;
    if (!resource) {
        free(surface);
        wl_client_post_no_memory(client);
        return;
    }
    surface->resource = resource;
    surface->server = s;
    surface->preferred_scale = 1;
    state_init(&surface->pending);
    state_init(&surface->current);
    wl_list_init(&surface->cached);
    wl_list_init(&surface->synced);
    wl_list_init(&surface->outputs);
    wl_list_init(&surface->below);
    wl_list_init(&surface->above);
    wl_list_init(&surface->pending_below);
    wl_list_init(&surface->pending_above);
    pixman_region32_init(&surface->input_region);
    wl_signal_init(&surface->events.client_commit);
    wl_signal_init(&surface->events.commit);
    wl_signal_init(&surface->events.map);
    wl_signal_init(&surface->events.unmap);
    wl_signal_init(&surface->events.destroy);
    wl_list_insert(s->surfaces.prev, &surface->link);
    wl_resource_set_implementation(resource, &surface_impl, surface, surface_free);
}

static void create_region(struct wl_client *client, struct wl_resource *compositor, uint32_t id) {
    pixman_region32_t *region = calloc(1, sizeof(*region));
    struct wl_resource *resource = region ? wl_resource_create(client, &wl_region_interface, 1, id) :
        NULL;
    if (!resource) {
        free(region);
        wl_client_post_no_memory(client);
        return;
    }
    pixman_region32_init(region);
    wl_resource_set_implementation(resource, &region_impl, region, region_free);
}

static const struct wl_compositor_interface compositor_impl = {
    .create_surface = create_surface, .create_region = create_region,
};

void surface_map(struct surface *surface) {
    if (surface->mapped) return;
    surface->mapped = true;
    if (surface->role && surface->role->map && (surface->role_resource || surface->role->no_object))
        surface->role->map(surface);
    wl_signal_emit_mutable(&surface->events.map, NULL);
    struct wl_list *lists[] = { &surface->below, &surface->above };
    struct subsurface *sub;
    for (int i = 0; i < 2; i++) wl_list_for_each(sub, lists[i], link) subsurface_consider_map(sub);
}

void surface_unmap(struct surface *surface) {
    if (!surface->mapped) return;
    surface->mapped = false;
    wl_signal_emit_mutable(&surface->events.unmap, NULL);
    if (surface->role && surface->role->unmap && (surface->role_resource || surface->role->no_object))
        surface->role->unmap(surface);
    struct wl_list *lists[] = { &surface->below, &surface->above };
    struct subsurface *sub;
    for (int i = 0; i < 2; i++) wl_list_for_each(sub, lists[i], link) surface_unmap(sub->surface);
}

void surface_reject_pending(struct surface *surface, struct wl_resource *resource, uint32_t code,
        const char *message) {
    wl_resource_post_error(resource, code, "%s", message);
    surface->pending_rejected = true;
}

bool surface_set_role(struct surface *surface, const struct surface_role *role,
        struct wl_resource *error_resource, uint32_t error_code) {
    if (surface->role && surface->role != role) {
        wl_resource_post_error(error_resource, error_code, "surface already has role %s",
            surface->role->name);
        return false;
    }
    if (surface->role_resource) {
        wl_resource_post_error(error_resource, error_code, "surface already has a role object");
        return false;
    }
    surface->role = role;
    return true;
}

static void role_resource_destroyed(struct wl_listener *listener, void *data) {
    struct surface *surface = wl_container_of(listener, surface, role_resource_destroy);
    detach(&surface->role_resource_destroy);
    surface_unmap(surface);
    if (surface->role->destroy) surface->role->destroy(surface);
    surface->role_resource = NULL;
}

void surface_set_role_object(struct surface *surface, struct wl_resource *resource) {
    surface->role_resource = resource;
    surface->role_resource_destroy.notify = role_resource_destroyed;
    wl_resource_add_destroy_listener(resource, &surface->role_resource_destroy);
}

struct surface *surface_root(struct surface *surface) {
    while (surface->subsurface && surface->subsurface->parent)
        surface = surface->subsurface->parent;
    return surface;
}

static void extents(struct surface *surface, int x, int y, struct wlr_box *box, bool *first) {
    if (!surface->mapped && !*first) return;
    struct wlr_box own = { x, y, surface->current.width, surface->current.height };
    if (*first) {
        *box = own;
    } else {
        int x2 = fmax(box->x + box->width, own.x + own.width);
        int y2 = fmax(box->y + box->height, own.y + own.height);
        box->x = box->x < own.x ? box->x : own.x;
        box->y = box->y < own.y ? box->y : own.y;
        box->width = x2 - box->x;
        box->height = y2 - box->y;
    }
    *first = false;
    struct wl_list *lists[] = { &surface->below, &surface->above };
    struct subsurface *sub;
    for (int i = 0; i < 2; i++) wl_list_for_each(sub, lists[i], link)
        extents(sub->surface, x + sub->x, y + sub->y, box, first);
}

void surface_extents(struct surface *surface, struct wlr_box *box) {
    bool first = true;
    *box = (struct wlr_box){0};
    extents(surface, 0, 0, box, &first);
}

static bool walk_list(struct wl_list *list, int x, int y, bool reverse,
        surface_iterator iterator, void *data) {
    struct subsurface *sub;
    if (reverse) {
        wl_list_for_each_reverse(sub, list, link)
            if (sub->surface->mapped &&
                    surface_walk(sub->surface, x + sub->x, y + sub->y, reverse, iterator, data))
                return true;
    } else {
        wl_list_for_each(sub, list, link)
            if (sub->surface->mapped &&
                    surface_walk(sub->surface, x + sub->x, y + sub->y, reverse, iterator, data))
                return true;
    }
    return false;
}

bool surface_walk(struct surface *surface, int x, int y, bool reverse, surface_iterator iterator,
        void *data) {
    struct wl_list *first = reverse ? &surface->above : &surface->below;
    struct wl_list *last = reverse ? &surface->below : &surface->above;
    return walk_list(first, x, y, reverse, iterator, data) || iterator(surface, x, y, data) ||
        walk_list(last, x, y, reverse, iterator, data);
}

void surface_source_box(struct surface *surface, struct wlr_fbox *box) {
    struct surface_state *s = &surface->current;
    *box = (struct wlr_fbox){ 0, 0, s->buffer_width, s->buffer_height };
    if (!s->viewport.has_src) return;
    *box = (struct wlr_fbox){ s->viewport.src.x * s->scale, s->viewport.src.y * s->scale,
        s->viewport.src.width * s->scale, s->viewport.src.height * s->scale };
    int width, height;
    state_buffer_size(s, &width, &height);
    wlr_fbox_transform(box, box, wlr_output_transform_invert(s->transform), width, height);
}

bool surface_accepts_input(struct surface *surface, double sx, double sy) {
    return sx >= 0 && sy >= 0 && sx < surface->current.width && sy < surface->current.height &&
        pixman_region32_contains_point(&surface->input_region, (int)floor(sx), (int)floor(sy), NULL);
}

static void output_bound(struct wl_listener *listener, void *data) {
    struct surface_output *so = wl_container_of(listener, so, bind);
    struct wlr_output_event_bind *event = data;
    if (wl_resource_get_client(event->resource) == wl_resource_get_client(so->surface->resource))
        wl_surface_send_enter(so->surface->resource, event->resource);
}

static void output_gone(struct wl_listener *listener, void *data) {
    struct surface_output *so = wl_container_of(listener, so, destroy);
    surface_output_free(so);
}

static void output_resources(struct surface *surface, struct wlr_output *output, bool enter) {
    struct wl_client *client = wl_resource_get_client(surface->resource);
    struct wl_resource *resource;
    wl_resource_for_each(resource, &output->resources) {
        if (wl_resource_get_client(resource) != client) continue;
        if (enter) wl_surface_send_enter(surface->resource, resource);
        else wl_surface_send_leave(surface->resource, resource);
    }
}

void surface_send_enter(struct surface *surface, struct wlr_output *output) {
    struct surface_output *so;
    wl_list_for_each(so, &surface->outputs, link) if (so->output == output) return;
    so = calloc(1, sizeof(*so));
    if (!so) return;
    so->output = output;
    so->surface = surface;
    wl_list_insert(&surface->outputs, &so->link);
    listen(&so->bind, &output->events.bind, output_bound);
    listen(&so->destroy, &output->events.destroy, output_gone);
    output_resources(surface, output, true);
}

void surface_send_leave(struct surface *surface, struct wlr_output *output) {
    struct surface_output *so, *next;
    wl_list_for_each_safe(so, next, &surface->outputs, link) {
        if (so->output != output) continue;
        output_resources(surface, output, false);
        surface_output_free(so);
    }
}

void surface_leave_all(struct surface *surface) {
    struct surface_output *so, *next;
    wl_list_for_each_safe(so, next, &surface->outputs, link) {
        output_resources(surface, so->output, false);
        surface_output_free(so);
    }
}

bool surface_on_output(struct surface *surface, struct wlr_output *output) {
    struct surface_output *so;
    wl_list_for_each(so, &surface->outputs, link) if (so->output == output) return true;
    return false;
}

void surface_frame_done(struct surface *surface, const struct timespec *when) {
    uint32_t ms = (uint32_t)(when->tv_sec * 1000 + when->tv_nsec / 1000000);
    struct wl_resource *resource, *next;
    wl_resource_for_each_safe(resource, next, &surface->current.frames) {
        wl_callback_send_done(resource, ms);
        wl_resource_destroy(resource);
    }
}

void surface_set_scale(struct surface *surface, double scale) {
    int integer = (int)ceil(scale);
    if (wl_resource_get_version(surface->resource) >= WL_SURFACE_PREFERRED_BUFFER_SCALE_SINCE_VERSION) {
        if (surface->preferred_scale != integer)
            wl_surface_send_preferred_buffer_scale(surface->resource, integer);
        if (!surface->sent_transform)
            wl_surface_send_preferred_buffer_transform(surface->resource,
                WL_OUTPUT_TRANSFORM_NORMAL);
        surface->sent_transform = true;
    }
    surface->preferred_scale = integer;
    uint32_t fractional = (uint32_t)round(scale * 120);
    if (surface->fractional && surface->fractional_scale != fractional)
        wp_fractional_scale_v1_send_preferred_scale(surface->fractional, fractional);
    surface->fractional_scale = fractional;
}

static void feedback_committed(struct wl_listener *listener, void *data) {
    struct feedback *f = wl_container_of(listener, f, commit);
    detach(&f->commit);
    f->committed = true;
    f->commit_seq = f->output->commit_seq;
}

static void feedback_presented(struct wl_listener *listener, void *data) {
    struct feedback *f = wl_container_of(listener, f, present);
    struct wlr_output_event_present *event = data;
    if (!f->committed || event->commit_seq != f->commit_seq) return;
    if (event->presented) {
        uint32_t flags = event->flags;
        if (!f->zero_copy) flags &= ~WP_PRESENTATION_FEEDBACK_KIND_ZERO_COPY;
        struct wl_resource *resource, *next;
        wl_resource_for_each_safe(resource, next, &f->resources) {
            struct wl_resource *output;
            wl_resource_for_each(output, &event->output->resources)
                if (wl_resource_get_client(output) == wl_resource_get_client(resource))
                    wp_presentation_feedback_send_sync_output(resource, output);
            wp_presentation_feedback_send_presented(resource, (uint32_t)((uint64_t)event->when.tv_sec >> 32),
                (uint32_t)event->when.tv_sec, (uint32_t)event->when.tv_nsec,
                (uint32_t)event->refresh, (uint32_t)((uint64_t)event->seq >> 32),
                (uint32_t)event->seq, flags);
            wl_resource_destroy(resource);
        }
    }
    feedback_free(f);
}

static void feedback_output_destroyed(struct wl_listener *listener, void *data) {
    struct feedback *f = wl_container_of(listener, f, output_destroy);
    feedback_free(f);
}

void surface_presented(struct surface *surface, struct wlr_output *output, bool zero_copy) {
    if (wl_list_empty(&surface->current.feedbacks)) return;
    struct feedback *f = calloc(1, sizeof(*f));
    if (!f) return;
    wl_list_init(&f->resources);
    wl_list_insert_list(&f->resources, &surface->current.feedbacks);
    wl_list_init(&surface->current.feedbacks);
    wl_list_insert(&surface->server->feedbacks, &f->link);
    f->output = output;
    f->zero_copy = zero_copy;
    listen(&f->commit, &output->events.commit, feedback_committed);
    listen(&f->present, &output->events.present, feedback_presented);
    listen(&f->output_destroy, &output->events.destroy, feedback_output_destroyed);
}

static void subsurface_consider_map(struct subsurface *sub) {
    if (sub->added && sub->parent && sub->parent->mapped && surface_has_buffer(sub->surface))
        surface_map(sub->surface);
}

static void subsurface_role_commit(struct surface *surface) {
    if (surface->subsurface) subsurface_consider_map(surface->subsurface);
}

static void subsurface_free(struct subsurface *sub) {
    if (sub->has_cache) {
        sub->has_cache = false;
        surface_unlock(sub->surface, sub->cache);
    }
    surface_unmap(sub->surface);
    detach(&sub->parent_destroy);
    wl_list_remove(&sub->link);
    wl_list_remove(&sub->pending_link);
    sub->surface->subsurface = NULL;
    wl_resource_set_user_data(sub->resource, NULL);
    free(sub);
}

static void subsurface_role_destroy(struct surface *surface) {
    if (surface->subsurface) subsurface_free(surface->subsurface);
}

static const struct surface_role subsurface_role = {
    .name = "wl_subsurface",
    .commit = subsurface_role_commit,
    .destroy = subsurface_role_destroy,
};

static void subsurface_parent_destroyed(struct wl_listener *listener, void *data) {
    struct subsurface *sub = wl_container_of(listener, sub, parent_destroy);
    subsurface_free(sub);
}

static void subsurface_set_position(struct wl_client *client, struct wl_resource *resource,
        int32_t x, int32_t y) {
    struct subsurface *sub = wl_resource_get_user_data(resource);
    if (!sub) return;
    sub->pending_x = x;
    sub->pending_y = y;
}

static struct subsurface *sibling(struct subsurface *sub, struct surface *surface) {
    struct wl_list *lists[] = { &sub->parent->pending_below, &sub->parent->pending_above };
    struct subsurface *other;
    for (int i = 0; i < 2; i++) wl_list_for_each(other, lists[i], pending_link)
        if (other->surface == surface && other != sub) return other;
    return NULL;
}

static void subsurface_place(struct wl_resource *resource, struct wl_resource *sibling_resource,
        bool above) {
    struct subsurface *sub = wl_resource_get_user_data(resource);
    if (!sub) return;
    struct surface *target = surface_from_resource(sibling_resource);
    struct wl_list *node;
    if (target == sub->parent) {
        node = above ? &sub->parent->pending_above : sub->parent->pending_below.prev;
    } else {
        struct subsurface *other = sibling(sub, target);
        if (!other) {
            wl_resource_post_error(resource, WL_SUBSURFACE_ERROR_BAD_SURFACE,
                "surface is not a parent or sibling");
            return;
        }
        node = above ? &other->pending_link : other->pending_link.prev;
    }
    wl_list_remove(&sub->pending_link);
    wl_list_insert(node, &sub->pending_link);
}

static void subsurface_place_above(struct wl_client *client, struct wl_resource *resource,
        struct wl_resource *sibling_resource) {
    subsurface_place(resource, sibling_resource, true);
}

static void subsurface_place_below(struct wl_client *client, struct wl_resource *resource,
        struct wl_resource *sibling_resource) {
    subsurface_place(resource, sibling_resource, false);
}

static void subsurface_set_sync(struct wl_client *client, struct wl_resource *resource) {
    struct subsurface *sub = wl_resource_get_user_data(resource);
    if (sub) sub->synchronized = true;
}

static void subsurface_set_desync(struct wl_client *client, struct wl_resource *resource) {
    struct subsurface *sub = wl_resource_get_user_data(resource);
    if (!sub || !sub->synchronized) return;
    sub->synchronized = false;
    if (!subsurface_synchronized(sub) && sub->has_cache) {
        sub->has_cache = false;
        surface_unlock(sub->surface, sub->cache);
    }
}

static const struct wl_subsurface_interface subsurface_impl = {
    .destroy = destroy_resource, .set_position = subsurface_set_position,
    .place_above = subsurface_place_above, .place_below = subsurface_place_below,
    .set_sync = subsurface_set_sync, .set_desync = subsurface_set_desync,
};

static void get_subsurface(struct wl_client *client, struct wl_resource *resource, uint32_t id,
        struct wl_resource *surface_resource, struct wl_resource *parent_resource) {
    struct surface *surface = surface_from_resource(surface_resource);
    struct surface *parent = surface_from_resource(parent_resource);
    if (!surface_set_role(surface, &subsurface_role, resource, WL_SUBCOMPOSITOR_ERROR_BAD_SURFACE))
        return;
    if (surface_root(parent) == surface) {
        wl_resource_post_error(resource, WL_SUBCOMPOSITOR_ERROR_BAD_PARENT,
            "subsurface cannot be a parent of itself or its ancestor");
        return;
    }
    struct subsurface *sub = calloc(1, sizeof(*sub));
    struct wl_resource *sub_resource = sub ? wl_resource_create(client, &wl_subsurface_interface,
        wl_resource_get_version(resource), id) : NULL;
    if (!sub_resource) {
        free(sub);
        wl_client_post_no_memory(client);
        return;
    }
    sub->resource = sub_resource;
    sub->surface = surface;
    sub->parent = parent;
    sub->synchronized = true;
    wl_resource_set_implementation(sub_resource, &subsurface_impl, sub, NULL);
    surface->subsurface = sub;
    surface_set_role_object(surface, sub_resource);
    listen(&sub->parent_destroy, &parent->events.destroy, subsurface_parent_destroyed);
    wl_list_init(&sub->link);
    wl_list_insert(parent->pending_above.prev, &sub->pending_link);
}

static const struct wl_subcompositor_interface subcompositor_impl = {
    .destroy = destroy_resource, .get_subsurface = get_subsurface,
};

static void viewport_set_source(struct wl_client *client, struct wl_resource *resource,
        wl_fixed_t x, wl_fixed_t y, wl_fixed_t width, wl_fixed_t height) {
    struct surface *surface = wl_resource_get_user_data(resource);
    if (!surface) {
        wl_resource_post_error(resource, WP_VIEWPORT_ERROR_NO_SURFACE, "surface destroyed");
        return;
    }
    struct surface_state *p = &surface->pending;
    double fx = wl_fixed_to_double(x), fy = wl_fixed_to_double(y);
    double fw = wl_fixed_to_double(width), fh = wl_fixed_to_double(height);
    if (fx == -1 && fy == -1 && fw == -1 && fh == -1) {
        p->viewport.has_src = false;
    } else if (fx < 0 || fy < 0 || fw <= 0 || fh <= 0) {
        wl_resource_post_error(resource, WP_VIEWPORT_ERROR_BAD_VALUE, "invalid source rectangle");
        return;
    } else {
        p->viewport.has_src = true;
        p->viewport.src = (struct wlr_fbox){ fx, fy, fw, fh };
    }
    p->committed |= STATE_VIEWPORT;
}

static void viewport_set_destination(struct wl_client *client, struct wl_resource *resource,
        int32_t width, int32_t height) {
    struct surface *surface = wl_resource_get_user_data(resource);
    if (!surface) {
        wl_resource_post_error(resource, WP_VIEWPORT_ERROR_NO_SURFACE, "surface destroyed");
        return;
    }
    struct surface_state *p = &surface->pending;
    if (width == -1 && height == -1) {
        p->viewport.has_dst = false;
    } else if (width <= 0 || height <= 0) {
        wl_resource_post_error(resource, WP_VIEWPORT_ERROR_BAD_VALUE, "invalid destination size");
        return;
    } else {
        p->viewport.has_dst = true;
        p->viewport.dst_width = width;
        p->viewport.dst_height = height;
    }
    p->committed |= STATE_VIEWPORT;
}

static void viewport_free(struct wl_resource *resource) {
    struct surface *surface = wl_resource_get_user_data(resource);
    if (!surface) return;
    surface->viewport = NULL;
    surface->pending.viewport.has_src = surface->pending.viewport.has_dst = false;
    surface->pending.committed |= STATE_VIEWPORT;
}

static const struct wp_viewport_interface viewport_impl = {
    .destroy = destroy_resource, .set_source = viewport_set_source,
    .set_destination = viewport_set_destination,
};

static void get_viewport(struct wl_client *client, struct wl_resource *manager, uint32_t id,
        struct wl_resource *surface_resource) {
    struct surface *surface = surface_from_resource(surface_resource);
    if (surface->viewport) {
        wl_resource_post_error(manager, WP_VIEWPORTER_ERROR_VIEWPORT_EXISTS,
            "surface already has a viewport");
        return;
    }
    struct wl_resource *resource = wl_resource_create(client, &wp_viewport_interface,
        wl_resource_get_version(manager), id);
    if (!resource) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(resource, &viewport_impl, surface, viewport_free);
    surface->viewport = resource;
}

static const struct wp_viewporter_interface viewporter_impl = {
    .destroy = destroy_resource, .get_viewport = get_viewport,
};

static void fractional_free(struct wl_resource *resource) {
    struct surface *surface = wl_resource_get_user_data(resource);
    if (surface) surface->fractional = NULL;
}

static const struct wp_fractional_scale_v1_interface fractional_impl = {
    .destroy = destroy_resource,
};

static void get_fractional_scale(struct wl_client *client, struct wl_resource *manager,
        uint32_t id, struct wl_resource *surface_resource) {
    struct surface *surface = surface_from_resource(surface_resource);
    if (surface->fractional) {
        wl_resource_post_error(manager, WP_FRACTIONAL_SCALE_MANAGER_V1_ERROR_FRACTIONAL_SCALE_EXISTS,
            "surface already has a fractional scale");
        return;
    }
    struct wl_resource *resource = wl_resource_create(client, &wp_fractional_scale_v1_interface,
        wl_resource_get_version(manager), id);
    if (!resource) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(resource, &fractional_impl, surface, fractional_free);
    surface->fractional = resource;
    if (surface->fractional_scale)
        wp_fractional_scale_v1_send_preferred_scale(resource, surface->fractional_scale);
}

static const struct wp_fractional_scale_manager_v1_interface fractional_manager_impl = {
    .destroy = destroy_resource, .get_fractional_scale = get_fractional_scale,
};

static void feedback_resource_free(struct wl_resource *resource) {
    wl_list_remove(wl_resource_get_link(resource));
}

static void presentation_feedback(struct wl_client *client, struct wl_resource *manager,
        struct wl_resource *surface_resource, uint32_t id) {
    struct surface *surface = surface_from_resource(surface_resource);
    struct wl_resource *resource = wl_resource_create(client, &wp_presentation_feedback_interface,
        wl_resource_get_version(manager), id);
    if (!resource) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(resource, NULL, NULL, feedback_resource_free);
    wl_list_insert(surface->pending.feedbacks.prev, wl_resource_get_link(resource));
}

static const struct wp_presentation_interface presentation_impl = {
    .destroy = destroy_resource, .feedback = presentation_feedback,
};

static void syncobj_surface_free(struct wl_resource *resource) {
    struct syncobj_surface *sync = wl_resource_get_user_data(resource);
    if (sync->surface) sync->surface->syncobj = NULL;
    free(sync);
}

static void set_point(struct wl_resource *resource, struct wl_resource *timeline_resource,
        uint32_t hi, uint32_t lo, bool acquire) {
    struct syncobj_surface *sync = wl_resource_get_user_data(resource);
    if (!sync->surface) {
        wl_resource_post_error(resource, WP_LINUX_DRM_SYNCOBJ_SURFACE_V1_ERROR_NO_SURFACE,
            "surface destroyed");
        return;
    }
    struct surface_state *p = &sync->surface->pending;
    struct wlr_drm_syncobj_timeline *timeline = wl_resource_get_user_data(timeline_resource);
    struct wlr_drm_syncobj_timeline **slot = acquire ? &p->acquire : &p->release;
    wlr_drm_syncobj_timeline_unref(*slot);
    *slot = wlr_drm_syncobj_timeline_ref(timeline);
    *(acquire ? &p->acquire_point : &p->release_point) = ((uint64_t)hi << 32) | lo;
}

static void set_acquire_point(struct wl_client *client, struct wl_resource *resource,
        struct wl_resource *timeline, uint32_t hi, uint32_t lo) {
    set_point(resource, timeline, hi, lo, true);
}

static void set_release_point(struct wl_client *client, struct wl_resource *resource,
        struct wl_resource *timeline, uint32_t hi, uint32_t lo) {
    set_point(resource, timeline, hi, lo, false);
}

static const struct wp_linux_drm_syncobj_surface_v1_interface syncobj_surface_impl = {
    .destroy = destroy_resource, .set_acquire_point = set_acquire_point,
    .set_release_point = set_release_point,
};

static void syncobj_get_surface(struct wl_client *client, struct wl_resource *manager,
        uint32_t id, struct wl_resource *surface_resource) {
    struct surface *surface = surface_from_resource(surface_resource);
    if (surface->syncobj) {
        wl_resource_post_error(manager, WP_LINUX_DRM_SYNCOBJ_MANAGER_V1_ERROR_SURFACE_EXISTS,
            "surface already has a syncobj surface");
        return;
    }
    struct syncobj_surface *sync = calloc(1, sizeof(*sync));
    struct wl_resource *resource = sync ? wl_resource_create(client,
        &wp_linux_drm_syncobj_surface_v1_interface, wl_resource_get_version(manager), id) : NULL;
    if (!resource) {
        free(sync);
        wl_client_post_no_memory(client);
        return;
    }
    sync->resource = resource;
    sync->surface = surface;
    wl_resource_set_implementation(resource, &syncobj_surface_impl, sync, syncobj_surface_free);
    surface->syncobj = resource;
}

static void timeline_free(struct wl_resource *resource) {
    wlr_drm_syncobj_timeline_unref(wl_resource_get_user_data(resource));
}

static const struct wp_linux_drm_syncobj_timeline_v1_interface timeline_impl = {
    .destroy = destroy_resource,
};

static void import_timeline(struct wl_client *client, struct wl_resource *manager, uint32_t id,
        int32_t fd) {
    struct wlr_drm_syncobj_timeline *timeline = wlr_drm_syncobj_timeline_import(drm_fd, fd);
    close(fd);
    if (!timeline) {
        wl_resource_post_error(manager, WP_LINUX_DRM_SYNCOBJ_MANAGER_V1_ERROR_INVALID_TIMELINE,
            "timeline import failed");
        return;
    }
    struct wl_resource *resource = wl_resource_create(client,
        &wp_linux_drm_syncobj_timeline_v1_interface, wl_resource_get_version(manager), id);
    if (!resource) {
        wlr_drm_syncobj_timeline_unref(timeline);
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(resource, &timeline_impl, timeline, timeline_free);
}

static const struct wp_linux_drm_syncobj_manager_v1_interface syncobj_impl = {
    .destroy = destroy_resource, .get_surface = syncobj_get_surface,
    .import_timeline = import_timeline,
};

#define BIND(name, interface, impl, extra) \
    static void name(struct wl_client *client, void *data, uint32_t version, uint32_t id) { \
        struct wl_resource *resource = wl_resource_create(client, interface, version, id); \
        if (!resource) { \
            wl_client_post_no_memory(client); \
            return; \
        } \
        wl_resource_set_implementation(resource, impl, data, NULL); \
        extra; \
    }

BIND(bind_compositor, &wl_compositor_interface, &compositor_impl, )
BIND(bind_subcompositor, &wl_subcompositor_interface, &subcompositor_impl, )
BIND(bind_viewporter, &wp_viewporter_interface, &viewporter_impl, )
BIND(bind_fractional, &wp_fractional_scale_manager_v1_interface, &fractional_manager_impl, )
BIND(bind_presentation, &wp_presentation_interface, &presentation_impl,
    wp_presentation_send_clock_id(resource, CLOCK_MONOTONIC))
BIND(bind_syncobj, &wp_linux_drm_syncobj_manager_v1_interface, &syncobj_impl, )

bool surfaces_listen(struct tomoe *s) {
    bool ok = wl_global_create(s->display, &wl_compositor_interface, COMPOSITOR_VERSION, s,
            bind_compositor) &&
        wl_global_create(s->display, &wl_subcompositor_interface, 1, s, bind_subcompositor) &&
        wl_global_create(s->display, &wp_viewporter_interface, 1, s, bind_viewporter) &&
        wl_global_create(s->display, &wp_fractional_scale_manager_v1_interface, 1, s,
            bind_fractional) &&
        wl_global_create(s->display, &wp_presentation_interface, 2, s, bind_presentation);
    if (!ok) return false;
    drm_fd = wlr_renderer_get_drm_fd(s->renderer);
    if (!s->renderer->features.timeline || !s->backend->features.timeline || drm_fd < 0) return true;
    return wl_global_create(s->display, &wp_linux_drm_syncobj_manager_v1_interface, 1, s,
        bind_syncobj);
}

void surfaces_finish(struct tomoe *s) {
    struct feedback *f, *next;
    wl_list_for_each_safe(f, next, &s->feedbacks, link) feedback_free(f);
}
