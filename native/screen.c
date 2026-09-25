#include "internal.h"
#include <wlr/render/drm_syncobj.h>
#include "xdg-output-unstable-v1-protocol.h"

#define OUTPUT_VERSION 4
#define XDG_OUTPUT_VERSION 3

static void destroy_resource(struct wl_client *client, struct wl_resource *resource) {
    wl_resource_destroy(resource);
}

void screen_state_init(struct screen_state *state) {
    *state = (struct screen_state){ .scale = 1 };
}

void screen_state_finish(struct screen_state *state) {
    wlr_buffer_unlock(state->buffer);
    wlr_drm_syncobj_timeline_unref(state->wait_timeline);
    free(state->gamma);
    screen_state_init(state);
}

bool screen_state_copy(struct screen_state *dst, const struct screen_state *src) {
    uint16_t *gamma = NULL;
    if (src->gamma && !(gamma = malloc(src->gamma_size * 3 * sizeof(*gamma)))) return false;
    if (gamma) memcpy(gamma, src->gamma, src->gamma_size * 3 * sizeof(*gamma));
    screen_state_finish(dst);
    *dst = *src;
    dst->gamma = gamma;
    if (dst->buffer) wlr_buffer_lock(dst->buffer);
    if (dst->wait_timeline) wlr_drm_syncobj_timeline_ref(dst->wait_timeline);
    return true;
}

void screen_state_set_enabled(struct screen_state *state, bool enabled) {
    state->committed |= SCREEN_ENABLED;
    state->enabled = enabled;
}

void screen_state_set_mode(struct screen_state *state, struct screen_mode *mode) {
    state->committed |= SCREEN_MODE;
    state->mode_type = SCREEN_MODE_FIXED;
    state->mode = mode;
}

void screen_state_set_custom_mode(struct screen_state *state, int32_t width, int32_t height,
        int32_t refresh) {
    state->committed |= SCREEN_MODE;
    state->mode_type = SCREEN_MODE_CUSTOM;
    state->mode = NULL;
    state->custom_mode.width = width;
    state->custom_mode.height = height;
    state->custom_mode.refresh = refresh;
}

void screen_state_set_scale(struct screen_state *state, float scale) {
    state->committed |= SCREEN_SCALE;
    state->scale = scale;
}

void screen_state_set_transform(struct screen_state *state, enum wl_output_transform transform) {
    state->committed |= SCREEN_TRANSFORM;
    state->transform = transform;
}

void screen_state_set_adaptive_sync_enabled(struct screen_state *state, bool enabled) {
    state->committed |= SCREEN_VRR;
    state->adaptive_sync_enabled = enabled;
}

void screen_state_set_buffer(struct screen_state *state, struct wlr_buffer *buffer) {
    state->committed |= SCREEN_BUFFER;
    wlr_buffer_unlock(state->buffer);
    state->buffer = wlr_buffer_lock(buffer);
}

void screen_state_set_wait_timeline(struct screen_state *state,
        struct wlr_drm_syncobj_timeline *timeline, uint64_t point) {
    state->committed |= SCREEN_WAIT;
    wlr_drm_syncobj_timeline_unref(state->wait_timeline);
    state->wait_timeline = wlr_drm_syncobj_timeline_ref(timeline);
    state->wait_point = point;
}

bool screen_state_set_gamma(struct screen_state *state, const uint16_t *ramps, size_t size) {
    uint16_t *copy = ramps ? malloc(size * 3 * sizeof(*copy)) : NULL;
    if (ramps && !copy) return false;
    if (copy) memcpy(copy, ramps, size * 3 * sizeof(*copy));
    free(state->gamma);
    state->gamma = copy;
    state->gamma_size = copy ? size : 0;
    state->committed |= SCREEN_GAMMA;
    return true;
}

void screen_transformed_resolution(struct screen *screen, int *width, int *height) {
    *width = screen->width;
    *height = screen->height;
    if (screen->transform % 2) {
        *width = screen->height;
        *height = screen->width;
    }
}

void screen_effective_resolution(struct screen *screen, int *width, int *height) {
    screen_transformed_resolution(screen, width, height);
    float scale = screen->scale > 0 ? screen->scale : 1;
    *width = (int)round(*width / scale);
    *height = (int)round(*height / scale);
}

struct screen_mode *screen_preferred_mode(struct screen *screen) {
    struct screen_mode *mode;
    wl_list_for_each(mode, &screen->modes, link) if (mode->preferred) return mode;
    return wl_list_empty(&screen->modes) ? NULL :
        wl_container_of(screen->modes.next, mode, link);
}

struct screen *screen_from_resource(struct wl_resource *resource) {
    return resource ? wl_resource_get_user_data(resource) : NULL;
}

static void send_geometry(struct screen *screen, struct wl_resource *resource) {
    wl_output_send_geometry(resource, 0, 0, screen->phys_width, screen->phys_height,
        WL_OUTPUT_SUBPIXEL_UNKNOWN, screen->make ? screen->make : "Unknown",
        screen->model ? screen->model : "Unknown", screen->transform);
}

static void send_mode(struct screen *screen, struct wl_resource *resource) {
    uint32_t flags = WL_OUTPUT_MODE_CURRENT;
    if (screen->current_mode && screen->current_mode->preferred) flags |= WL_OUTPUT_MODE_PREFERRED;
    wl_output_send_mode(resource, flags, screen->width, screen->height, screen->refresh);
}

static void send_done(struct wl_resource *resource) {
    if (wl_resource_get_version(resource) >= WL_OUTPUT_DONE_SINCE_VERSION)
        wl_output_send_done(resource);
}

static void send_xdg(struct screen *screen, struct wl_resource *resource) {
    int width, height;
    screen_effective_resolution(screen, &width, &height);
    zxdg_output_v1_send_logical_position(resource, screen->lx, screen->ly);
    zxdg_output_v1_send_logical_size(resource, width, height);
    if (wl_resource_get_version(resource) < 3) zxdg_output_v1_send_done(resource);
}

static void resource_unlink(struct wl_resource *resource) {
    wl_list_remove(wl_resource_get_link(resource));
    wl_list_init(wl_resource_get_link(resource));
}

static const struct wl_output_interface output_impl = { .release = destroy_resource };

static void bind_output(struct wl_client *client, void *data, uint32_t version, uint32_t id) {
    struct screen *screen = data;
    struct wl_resource *resource = wl_resource_create(client, &wl_output_interface, version, id);
    if (!resource) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(resource, &output_impl, screen, resource_unlink);
    wl_list_insert(&screen->resources, wl_resource_get_link(resource));
    send_geometry(screen, resource);
    send_mode(screen, resource);
    if (version >= WL_OUTPUT_SCALE_SINCE_VERSION)
        wl_output_send_scale(resource, (int32_t)ceil(screen->scale));
    if (version >= WL_OUTPUT_NAME_SINCE_VERSION) wl_output_send_name(resource, screen->name);
    if (version >= WL_OUTPUT_DESCRIPTION_SINCE_VERSION)
        wl_output_send_description(resource, screen->description);
    send_done(resource);
    wl_signal_emit_mutable(&screen->events.bind, &(struct screen_bind){ screen, resource });
}

static void global_update(struct screen *screen) {
    if (screen->enabled && !screen->global) {
        screen->global = wl_global_create(screen->server->display, &wl_output_interface,
            OUTPUT_VERSION, screen, bind_output);
        if (!screen->global) fail(screen->server, "wl_output global allocation failed");
    } else if (!screen->enabled && screen->global) {
        struct wl_resource *resource, *next;
        wl_resource_for_each_safe(resource, next, &screen->resources) {
            wl_resource_set_user_data(resource, NULL);
            resource_unlink(resource);
        }
        wl_resource_for_each_safe(resource, next, &screen->xdg_resources) {
            wl_resource_set_user_data(resource, NULL);
            resource_unlink(resource);
        }
        wl_global_destroy(screen->global);
        screen->global = NULL;
    }
}

static void resources_update(struct screen *screen, bool geometry) {
    struct wl_resource *resource;
    wl_resource_for_each(resource, &screen->resources) {
        if (geometry) send_geometry(screen, resource);
        send_mode(screen, resource);
        if (wl_resource_get_version(resource) >= WL_OUTPUT_SCALE_SINCE_VERSION)
            wl_output_send_scale(resource, (int32_t)ceil(screen->scale));
    }
    wl_resource_for_each(resource, &screen->xdg_resources) send_xdg(screen, resource);
    wl_resource_for_each(resource, &screen->resources) send_done(resource);
}

void screen_set_position(struct screen *screen, int lx, int ly) {
    if (screen->lx == lx && screen->ly == ly) return;
    screen->lx = lx;
    screen->ly = ly;
    resources_update(screen, false);
}

static void apply(struct screen *screen, const struct screen_state *state) {
    bool geometry = false;
    if (state->committed & SCREEN_ENABLED) screen->enabled = state->enabled;
    if (state->committed & SCREEN_MODE) {
        if (state->mode_type == SCREEN_MODE_FIXED && state->mode) {
            screen->current_mode = state->mode;
            screen->width = state->mode->width;
            screen->height = state->mode->height;
            screen->refresh = state->mode->refresh;
        } else {
            screen->current_mode = NULL;
            screen->width = state->custom_mode.width;
            screen->height = state->custom_mode.height;
            screen->refresh = state->custom_mode.refresh;
        }
    }
    if (state->committed & SCREEN_SCALE) screen->scale = state->scale;
    if (state->committed & SCREEN_TRANSFORM) {
        geometry = screen->transform != state->transform;
        screen->transform = state->transform;
    }
    if (state->committed & SCREEN_VRR)
        screen->adaptive_sync = screen->adaptive_sync_supported && state->adaptive_sync_enabled;
    if (!screen->enabled) screen->adaptive_sync = false;
    if (state->committed & SCREEN_BUFFER) screen->frame_pending = true;
    screen->commit_seq++;
    global_update(screen);
    if (state->committed & (SCREEN_MODE | SCREEN_SCALE | SCREEN_TRANSFORM))
        resources_update(screen, geometry);
    wl_signal_emit_mutable(&screen->events.commit, screen);
}

bool screens_test(struct screen_update *updates, size_t count) {
    return !count || updates[0].output->impl->test(updates, count);
}

bool screens_commit(struct screen_update *updates, size_t count) {
    if (count && !updates[0].output->impl->commit(updates, count)) return false;
    for (size_t i = 0; i < count; i++) apply(updates[i].output, &updates[i].base);
    return true;
}

bool screen_test(struct screen *screen, const struct screen_state *state) {
    struct screen_update update = { screen, *state };
    return screens_test(&update, 1);
}

bool screen_commit(struct screen *screen, const struct screen_state *state) {
    struct screen_update update = { screen, *state };
    return screens_commit(&update, 1);
}

const struct wlr_drm_format_set *screen_primary_formats(struct screen *screen) {
    return screen->impl->formats ? screen->impl->formats(screen) : NULL;
}

size_t screen_gamma_size(struct screen *screen) {
    return screen->impl->gamma_size ? screen->impl->gamma_size(screen) : 0;
}

void screen_send_frame(struct screen *screen) {
    screen->frame_pending = false;
    if (screen->enabled) wl_signal_emit_mutable(&screen->events.frame, screen);
}

static void idle_frame(void *data) {
    struct screen *screen = data;
    screen->idle_frame = NULL;
    if (!screen->frame_pending) screen_send_frame(screen);
}

void screen_schedule_frame(struct screen *screen) {
    if (screen->frame_pending || screen->idle_frame) return;
    screen->idle_frame = wl_event_loop_add_idle(
        wl_display_get_event_loop(screen->server->display), idle_frame, screen);
}

void screen_send_present(struct screen *screen, struct screen_present *present) {
    present->output = screen;
    wl_signal_emit_mutable(&screen->events.present, present);
}

void screen_request_state(struct screen *screen, struct screen_state *state) {
    wl_signal_emit_mutable(&screen->events.request_state, state);
}

void screen_init(struct screen *screen, struct tomoe *s, const struct screen_impl *impl,
        enum screen_kind kind, const char *name) {
    screen->server = s;
    screen->impl = impl;
    screen->kind = kind;
    screen->name = strdup(name);
    screen->scale = 1;
    wl_list_init(&screen->modes);
    wl_list_init(&screen->resources);
    wl_list_init(&screen->xdg_resources);
    wl_signal_init(&screen->events.frame);
    wl_signal_init(&screen->events.needs_frame);
    wl_signal_init(&screen->events.present);
    wl_signal_init(&screen->events.request_state);
    wl_signal_init(&screen->events.commit);
    wl_signal_init(&screen->events.destroy);
    wl_signal_init(&screen->events.bind);
}

void screen_describe(struct screen *screen) {
    free(screen->description);
    screen->description = NULL;
    if (asprintf(&screen->description, "%s %s %s (%s)", screen->make ? screen->make : "Unknown",
            screen->model ? screen->model : "Unknown", screen->serial ? screen->serial : "Unknown",
            screen->name) < 0)
        screen->description = NULL;
}

struct screen_mode *screen_add_mode(struct screen *screen, int32_t width, int32_t height,
        int32_t refresh, bool preferred) {
    struct screen_mode *mode = calloc(1, sizeof(*mode));
    if (!mode) return NULL;
    *mode = (struct screen_mode){ .width = width, .height = height, .refresh = refresh,
        .preferred = preferred };
    wl_list_insert(screen->modes.prev, &mode->link);
    return mode;
}

void screen_destroy(struct screen *screen) {
    wl_signal_emit_mutable(&screen->events.destroy, screen);
    screen->enabled = false;
    global_update(screen);
    if (screen->idle_frame) wl_event_source_remove(screen->idle_frame);
    struct screen_mode *mode, *next;
    wl_list_for_each_safe(mode, next, &screen->modes, link) {
        wl_list_remove(&mode->link);
        free(mode->data);
        free(mode);
    }
    free(screen->name);
    free(screen->description);
    free(screen->make);
    free(screen->model);
    free(screen->serial);
    if (screen->impl->destroy) screen->impl->destroy(screen);
}

static const struct zxdg_output_v1_interface xdg_output_impl = { .destroy = destroy_resource };

static void get_xdg_output(struct wl_client *client, struct wl_resource *manager, uint32_t id,
        struct wl_resource *output) {
    struct screen *screen = screen_from_resource(output);
    struct wl_resource *resource = wl_resource_create(client, &zxdg_output_v1_interface,
        wl_resource_get_version(manager), id);
    if (!resource) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(resource, &xdg_output_impl, screen, resource_unlink);
    if (!screen) {
        wl_list_init(wl_resource_get_link(resource));
        return;
    }
    wl_list_insert(&screen->xdg_resources, wl_resource_get_link(resource));
    if (wl_resource_get_version(resource) >= ZXDG_OUTPUT_V1_NAME_SINCE_VERSION) {
        zxdg_output_v1_send_name(resource, screen->name);
        zxdg_output_v1_send_description(resource, screen->description);
    }
    send_xdg(screen, resource);
    if (wl_resource_get_version(resource) >= 3) send_done(output);
}

static const struct zxdg_output_manager_v1_interface xdg_manager_impl = {
    .destroy = destroy_resource, .get_xdg_output = get_xdg_output,
};

static void bind_xdg_manager(struct wl_client *client, void *data, uint32_t version, uint32_t id) {
    struct wl_resource *resource = wl_resource_create(client, &zxdg_output_manager_v1_interface,
        version, id);
    if (!resource) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(resource, &xdg_manager_impl, data, NULL);
}

bool screens_listen(struct tomoe *s) {
    return wl_global_create(s->display, &zxdg_output_manager_v1_interface, XDG_OUTPUT_VERSION, s,
        bind_xdg_manager);
}

static bool hardware_allowed(struct screen *screen) {
    const char *off = getenv("WLR_NO_HARDWARE_CURSORS");
    struct cursor_image *image = &screen->server->cursor_image;
    return screen->impl->cursor && !screen->software_cursor_locks && !(off && !strcmp(off, "1")) &&
        (!image->buffer || image->scale == screen->scale);
}

static void cursor_update(struct screen *screen) {
    struct cursor_image *image = &screen->server->cursor_image;
    bool hardware = screen->enabled && hardware_allowed(screen) &&
        screen->impl->cursor(screen, image->buffer, image->hotspot_x, image->hotspot_y);
    if (!hardware && screen->hardware_cursor && screen->impl->cursor)
        screen->impl->cursor(screen, NULL, 0, 0);
    screen->hardware_cursor = hardware;
    if (!hardware) screen_schedule_frame(screen);
}

void cursor_show(struct tomoe *s, struct wlr_buffer *buffer, int hotspot_x, int hotspot_y,
        float scale) {
    struct cursor_image *image = &s->cursor_image;
    if (buffer != image->buffer) {
        wlr_texture_destroy(image->texture);
        image->texture = buffer ? wlr_texture_from_buffer(s->renderer, buffer) : NULL;
        wlr_buffer_unlock(image->buffer);
        image->buffer = buffer ? wlr_buffer_lock(buffer) : NULL;
    }
    image->hotspot_x = hotspot_x;
    image->hotspot_y = hotspot_y;
    image->scale = scale > 0 ? scale : 1;
    struct output *o;
    wl_list_for_each(o, &s->outputs, link) cursor_update(o->screen);
}

void cursor_finish(struct tomoe *s) {
    wlr_texture_destroy(s->cursor_image.texture);
    wlr_buffer_unlock(s->cursor_image.buffer);
    s->cursor_image = (struct cursor_image){0};
}

void screen_cursor_move(struct screen *screen, double x, double y) {
    struct cursor_image *image = &screen->server->cursor_image;
    if (screen->cursor_x == x && screen->cursor_y == y) return;
    screen->cursor_x = x;
    screen->cursor_y = y;
    if (screen->hardware_cursor && screen->impl->move_cursor)
        screen->impl->move_cursor(screen, (int)round(x) - image->hotspot_x,
            (int)round(y) - image->hotspot_y);
    else if (image->buffer)
        screen_schedule_frame(screen);
}

void screen_lock_software_cursors(struct screen *screen, bool lock) {
    screen->software_cursor_locks += lock ? 1 : -1;
    cursor_update(screen);
}
