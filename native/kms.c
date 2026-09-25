#include "internal.h"
#include <fcntl.h>
#include <poll.h>
#include <sys/stat.h>
#include <unistd.h>
#include <drm_fourcc.h>
#include <gbm.h>
#include <libudev.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <wlr/render/drm_syncobj.h>
#include <wlr/util/addon.h>
#include "presentation-time-protocol.h"

enum { PROP_CRTC_ID, PROP_MODE_ID, PROP_ACTIVE, PROP_GAMMA_LUT, PROP_GAMMA_LUT_SIZE,
    PROP_VRR_ENABLED, PROP_FB_ID, PROP_SRC_X, PROP_SRC_Y, PROP_SRC_W, PROP_SRC_H, PROP_CRTC_X,
    PROP_CRTC_Y, PROP_CRTC_W, PROP_CRTC_H, PROP_IN_FENCE_FD, PROP_TYPE, PROP_IN_FORMATS,
    PROP_VRR_CAPABLE, PROP_NON_DESKTOP, PROP_EDID, PROP_LINK_STATUS, PROP_COUNT };

static const char *const prop_names[PROP_COUNT] = { "CRTC_ID", "MODE_ID", "ACTIVE", "GAMMA_LUT",
    "GAMMA_LUT_SIZE", "VRR_ENABLED", "FB_ID", "SRC_X", "SRC_Y", "SRC_W", "SRC_H", "CRTC_X",
    "CRTC_Y", "CRTC_W", "CRTC_H", "IN_FENCE_FD", "type", "IN_FORMATS", "vrr_capable",
    "non-desktop", "EDID", "link-status" };

struct props {
    uint32_t id[PROP_COUNT];
    uint64_t value[PROP_COUNT];
};

struct plane {
    uint32_t id, type, possible_crtcs;
    struct props props;
    struct wlr_drm_format_set formats;
};

struct crtc {
    uint32_t id;
    int index;
    struct plane *primary, *cursor;
    struct props props;
    struct connector *owner;
    uint32_t mode_blob, gamma_blob;
    drmModeModeInfo mode;
    bool active;
    size_t gamma_size;
};

struct connector {
    struct screen screen;
    struct kms *kms;
    uint32_t id;
    struct props props;
    uint32_t possible_crtcs;
    struct crtc *crtc, *kernel_crtc;
    struct wlr_buffer *queued, *current;
    bool flip_pending, present_pending;
    size_t flip_seq;
    struct gbm_bo *cursor_bo[2];
    uint32_t cursor_fb[2];
    int cursor_front, cursor_x, cursor_y;
    bool cursor_on, cursor_committed;
    struct wl_list link;
};

struct kms {
    struct tomoe *server;
    int fd, device;
    dev_t devnum;
    bool atomic, async_atomic, modifiers;
    uint64_t cursor_width, cursor_height;
    struct gbm_device *gbm;
    struct wl_event_source *source, *monitor_source;
    struct udev *udev;
    struct udev_monitor *monitor;
    struct crtc *crtcs;
    size_t crtc_count;
    struct plane *planes;
    size_t plane_count;
    struct wl_list connectors;
};

struct fb {
    struct wlr_addon addon;
    struct kms *kms;
    uint32_t id;
};

static const struct screen_impl connector_impl;

static void props_read(int fd, uint32_t object, uint32_t type, struct props *props) {
    drmModeObjectProperties *list = drmModeObjectGetProperties(fd, object, type);
    if (!list) return;
    for (uint32_t i = 0; i < list->count_props; i++) {
        drmModePropertyRes *prop = drmModeGetProperty(fd, list->props[i]);
        if (!prop) continue;
        for (int p = 0; p < PROP_COUNT; p++) {
            if (strcmp(prop->name, prop_names[p])) continue;
            props->id[p] = prop->prop_id;
            props->value[p] = list->prop_values[i];
        }
        drmModeFreeProperty(prop);
    }
    drmModeFreeObjectProperties(list);
}

static void fb_destroy(struct wlr_addon *addon) {
    struct fb *fb = wl_container_of(addon, fb, addon);
    drmModeRmFB(fb->kms->fd, fb->id);
    wlr_addon_finish(addon);
    free(fb);
}

static const struct wlr_addon_interface fb_addon = { .name = "tomoe-kms-fb", .destroy = fb_destroy };

static uint32_t fb_for(struct kms *kms, struct wlr_buffer *buffer) {
    struct wlr_addon *addon = wlr_addon_find(&buffer->addons, kms, &fb_addon);
    if (addon) {
        struct fb *fb = wl_container_of(addon, fb, addon);
        return fb->id;
    }
    struct wlr_dmabuf_attributes dmabuf;
    if (!wlr_buffer_get_dmabuf(buffer, &dmabuf)) return 0;
    uint32_t handles[4] = {0}, pitches[4] = {0}, offsets[4] = {0};
    uint64_t modifiers[4] = {0};
    for (int i = 0; i < dmabuf.n_planes; i++) {
        if (drmPrimeFDToHandle(kms->fd, dmabuf.fd[i], &handles[i])) return 0;
        pitches[i] = dmabuf.stride[i];
        offsets[i] = dmabuf.offset[i];
        modifiers[i] = dmabuf.modifier;
    }
    uint32_t id = 0;
    bool explicit = dmabuf.modifier != DRM_FORMAT_MOD_INVALID;
    int ret = explicit && kms->modifiers ?
        drmModeAddFB2WithModifiers(kms->fd, dmabuf.width, dmabuf.height, dmabuf.format, handles,
            pitches, offsets, modifiers, &id, DRM_MODE_FB_MODIFIERS) :
        explicit && dmabuf.modifier != DRM_FORMAT_MOD_LINEAR ? -1 :
        drmModeAddFB2(kms->fd, dmabuf.width, dmabuf.height, dmabuf.format, handles, pitches,
            offsets, &id, 0);
    for (int i = 0; i < dmabuf.n_planes; i++) {
        bool shared = false;
        for (int j = 0; j < i; j++) shared |= handles[j] == handles[i];
        if (!shared && handles[i]) drmCloseBufferHandle(kms->fd, handles[i]);
    }
    struct fb *fb = ret ? NULL : calloc(1, sizeof(*fb));
    if (!fb) {
        if (!ret) drmModeRmFB(kms->fd, id);
        return 0;
    }
    fb->kms = kms;
    fb->id = id;
    wlr_addon_init(&fb->addon, &buffer->addons, kms, &fb_addon);
    return id;
}

static int32_t refresh_mhz(const drmModeModeInfo *mode) {
    if (!mode->htotal || !mode->vtotal) return 0;
    int64_t refresh = ((int64_t)mode->clock * 1000000 / mode->htotal + mode->vtotal / 2) /
        mode->vtotal;
    if (mode->flags & DRM_MODE_FLAG_INTERLACE) refresh *= 2;
    if (mode->flags & DRM_MODE_FLAG_DBLSCAN) refresh /= 2;
    if (mode->vscan > 1) refresh /= mode->vscan;
    return (int32_t)refresh;
}

static char *edid_string(const uint8_t *text) {
    char buffer[14] = {0};
    for (int i = 0; i < 13 && text[i] != '\n'; i++) buffer[i] = (char)text[i];
    for (int i = (int)strlen(buffer) - 1; i >= 0 && buffer[i] == ' '; i--) buffer[i] = '\0';
    return buffer[0] ? strdup(buffer) : NULL;
}

static void parse_edid(struct screen *screen, const uint8_t *edid, size_t size) {
    if (size < 128) return;
    char make[4] = { (char)('A' + ((edid[8] >> 2) & 0x1f) - 1),
        (char)('A' + (((edid[8] & 0x3) << 3) | (edid[9] >> 5)) - 1),
        (char)('A' + (edid[9] & 0x1f) - 1), 0 };
    screen->make = strdup(make);
    for (int i = 0; i < 4; i++) {
        const uint8_t *d = edid + 54 + i * 18;
        if (d[0] || d[1]) continue;
        if (d[3] == 0xfc && !screen->model) screen->model = edid_string(d + 5);
        if (d[3] == 0xff && !screen->serial) screen->serial = edid_string(d + 5);
    }
    if (!screen->model && asprintf(&screen->model, "0x%04X", edid[10] | edid[11] << 8) < 0)
        screen->model = NULL;
}

static void read_formats(struct kms *kms, struct plane *plane) {
    drmModePropertyBlobRes *blob = plane->props.id[PROP_IN_FORMATS] && kms->modifiers ?
        drmModeGetPropertyBlob(kms->fd, (uint32_t)plane->props.value[PROP_IN_FORMATS]) : NULL;
    if (blob) {
        drmModeFormatModifierIterator iter = {0};
        while (drmModeFormatModifierBlobIterNext(blob, &iter))
            wlr_drm_format_set_add(&plane->formats, iter.fmt, iter.mod);
        drmModeFreePropertyBlob(blob);
        return;
    }
    drmModePlane *info = drmModeGetPlane(kms->fd, plane->id);
    for (uint32_t i = 0; info && i < info->count_formats; i++) {
        wlr_drm_format_set_add(&plane->formats, info->formats[i], DRM_FORMAT_MOD_INVALID);
        wlr_drm_format_set_add(&plane->formats, info->formats[i], DRM_FORMAT_MOD_LINEAR);
    }
    drmModeFreePlane(info);
}

static bool read_resources(struct kms *kms) {
    drmModeRes *res = drmModeGetResources(kms->fd);
    drmModePlaneRes *planes = res ? drmModeGetPlaneResources(kms->fd) : NULL;
    if (!planes) {
        drmModeFreeResources(res);
        return false;
    }
    kms->crtc_count = (size_t)res->count_crtcs;
    kms->crtcs = calloc(kms->crtc_count, sizeof(*kms->crtcs));
    kms->plane_count = planes->count_planes;
    kms->planes = calloc(kms->plane_count, sizeof(*kms->planes));
    bool ok = kms->crtcs && kms->planes;
    for (size_t i = 0; ok && i < kms->crtc_count; i++) {
        struct crtc *crtc = &kms->crtcs[i];
        crtc->id = res->crtcs[i];
        crtc->index = (int)i;
        props_read(kms->fd, crtc->id, DRM_MODE_OBJECT_CRTC, &crtc->props);
        crtc->gamma_size = (size_t)crtc->props.value[PROP_GAMMA_LUT_SIZE];
        if (!crtc->gamma_size) {
            drmModeCrtc *info = drmModeGetCrtc(kms->fd, crtc->id);
            crtc->gamma_size = info ? (size_t)info->gamma_size : 0;
            drmModeFreeCrtc(info);
        }
    }
    for (size_t i = 0; ok && i < kms->plane_count; i++) {
        struct plane *plane = &kms->planes[i];
        drmModePlane *info = drmModeGetPlane(kms->fd, planes->planes[i]);
        if (!info) continue;
        plane->id = info->plane_id;
        plane->possible_crtcs = info->possible_crtcs;
        drmModeFreePlane(info);
        props_read(kms->fd, plane->id, DRM_MODE_OBJECT_PLANE, &plane->props);
        plane->type = (uint32_t)plane->props.value[PROP_TYPE];
        read_formats(kms, plane);
        for (size_t c = 0; c < kms->crtc_count; c++) {
            struct crtc *crtc = &kms->crtcs[c];
            if (!(plane->possible_crtcs & (1u << c))) continue;
            if (plane->type == DRM_PLANE_TYPE_PRIMARY && !crtc->primary) {
                crtc->primary = plane;
                break;
            }
            if (plane->type == DRM_PLANE_TYPE_CURSOR && !crtc->cursor) {
                crtc->cursor = plane;
                break;
            }
        }
    }
    drmModeFreePlaneResources(planes);
    drmModeFreeResources(res);
    return ok;
}

static struct crtc *pick_crtc(struct connector *c) {
    if (c->crtc) return c->crtc;
    if (c->kernel_crtc && !c->kernel_crtc->owner && c->kernel_crtc->primary) return c->kernel_crtc;
    for (size_t i = 0; i < c->kms->crtc_count; i++) {
        struct crtc *crtc = &c->kms->crtcs[i];
        if (!crtc->owner && crtc->primary && (c->possible_crtcs & (1u << i))) return crtc;
    }
    return NULL;
}

static bool mode_for(struct connector *c, const struct screen_state *state, drmModeModeInfo *out) {
    if (!(state->committed & SCREEN_MODE)) {
        if (c->crtc && c->crtc->active) {
            *out = c->crtc->mode;
            return true;
        }
        if (!c->screen.current_mode) return false;
        *out = *(drmModeModeInfo *)c->screen.current_mode->data;
        return true;
    }
    if (state->mode_type == SCREEN_MODE_FIXED && state->mode) {
        *out = *(drmModeModeInfo *)state->mode->data;
        return true;
    }
    struct screen_mode *mode;
    wl_list_for_each(mode, &c->screen.modes, link) {
        if (mode->width != state->custom_mode.width || mode->height != state->custom_mode.height ||
                (state->custom_mode.refresh && abs(mode->refresh - state->custom_mode.refresh) > 1000))
            continue;
        *out = *(drmModeModeInfo *)mode->data;
        return true;
    }
    return false;
}

static int sync_file(const struct screen_state *state) {
    if (!(state->committed & SCREEN_WAIT) || !state->wait_timeline) return -1;
    return wlr_drm_syncobj_timeline_export_sync_file(state->wait_timeline, state->wait_point);
}

static bool enabled_after(const struct connector *c, const struct screen_state *state) {
    return (state->committed & SCREEN_ENABLED) ? state->enabled : c->screen.enabled;
}

static uint32_t gamma_blob(struct kms *kms, const struct screen_state *state) {
    if (!state->gamma) return 0;
    struct drm_color_lut *lut = calloc(state->gamma_size, sizeof(*lut));
    if (!lut) return 0;
    for (size_t i = 0; i < state->gamma_size; i++) {
        lut[i].red = state->gamma[i];
        lut[i].green = state->gamma[state->gamma_size + i];
        lut[i].blue = state->gamma[state->gamma_size * 2 + i];
    }
    uint32_t blob = 0;
    drmModeCreatePropertyBlob(kms->fd, lut, state->gamma_size * sizeof(*lut), &blob);
    free(lut);
    return blob;
}

static void add(drmModeAtomicReq *req, uint32_t object, const struct props *props, int prop,
        uint64_t value) {
    if (props->id[prop]) drmModeAtomicAddProperty(req, object, props->id[prop], value);
}

static void plane_set(drmModeAtomicReq *req, struct plane *plane, uint32_t crtc, uint32_t fb,
        int x, int y, int width, int height, int src_width, int src_height) {
    add(req, plane->id, &plane->props, PROP_FB_ID, fb);
    add(req, plane->id, &plane->props, PROP_CRTC_ID, fb ? crtc : 0);
    add(req, plane->id, &plane->props, PROP_SRC_X, 0);
    add(req, plane->id, &plane->props, PROP_SRC_Y, 0);
    add(req, plane->id, &plane->props, PROP_SRC_W, (uint64_t)src_width << 16);
    add(req, plane->id, &plane->props, PROP_SRC_H, (uint64_t)src_height << 16);
    add(req, plane->id, &plane->props, PROP_CRTC_X, (uint64_t)(int64_t)x);
    add(req, plane->id, &plane->props, PROP_CRTC_Y, (uint64_t)(int64_t)y);
    add(req, plane->id, &plane->props, PROP_CRTC_W, (uint64_t)width);
    add(req, plane->id, &plane->props, PROP_CRTC_H, (uint64_t)height);
}

struct plan {
    struct crtc *crtc;
    drmModeModeInfo mode;
    uint32_t mode_blob, gamma_blob, fb;
    int fence;
    bool enable, modeset, gamma;
};

static int drm_event(int fd, uint32_t mask, void *data);

static void drain_flips(struct kms *kms, struct screen_update *updates, size_t count) {
    for (size_t i = 0; i < count; i++) {
        struct connector *c = wl_container_of(updates[i].output, c, screen);
        for (int tries = 0; c->flip_pending && tries < 10; tries++) {
            struct pollfd pfd = { .fd = kms->fd, .events = POLLIN };
            if (poll(&pfd, 1, 100) > 0) drm_event(kms->fd, 0, kms);
        }
    }
}

static bool atomic_commit(struct kms *kms, struct screen_update *updates, size_t count, bool test) {
    if (!test) drain_flips(kms, updates, count);
    struct plan *plans = calloc(count, sizeof(*plans));
    drmModeAtomicReq *req = drmModeAtomicAlloc();
    bool ok = plans && req, modeset = false, flip = false, tearing = false;
    for (size_t i = 0; i < count; i++) plans[i].fence = -1;
    for (size_t i = 0; ok && i < count; i++) {
        struct connector *c = wl_container_of(updates[i].output, c, screen);
        const struct screen_state *state = &updates[i].base;
        struct plan *p = &plans[i];
        p->enable = enabled_after(c, state);
        if (!p->enable) {
            if (!c->crtc) continue;
            p->crtc = c->crtc;
            modeset = true;
            add(req, c->id, &c->props, PROP_CRTC_ID, 0);
            add(req, c->crtc->id, &c->crtc->props, PROP_ACTIVE, 0);
            add(req, c->crtc->id, &c->crtc->props, PROP_MODE_ID, 0);
            plane_set(req, c->crtc->primary, 0, 0, 0, 0, 0, 0, 0, 0);
            if (c->crtc->cursor) plane_set(req, c->crtc->cursor, 0, 0, 0, 0, 0, 0, 0, 0);
            continue;
        }
        p->crtc = pick_crtc(c);
        ok = p->crtc && mode_for(c, state, &p->mode);
        if (!ok) break;
        p->modeset = !p->crtc->active || p->crtc->owner != c ||
            memcmp(&p->mode, &p->crtc->mode, sizeof(p->mode));
        modeset |= p->modeset;
        struct wlr_buffer *buffer = (state->committed & SCREEN_BUFFER) ? state->buffer : c->queued ?
            c->queued : c->current;
        p->fb = buffer ? fb_for(kms, buffer) : 0;
        ok = p->fb != 0;
        if (!ok) break;
        flip |= (state->committed & SCREEN_BUFFER) != 0;
        tearing |= state->tearing_page_flip;
        if (p->modeset) {
            ok = !drmModeCreatePropertyBlob(kms->fd, &p->mode, sizeof(p->mode), &p->mode_blob);
            add(req, c->id, &c->props, PROP_CRTC_ID, p->crtc->id);
            add(req, p->crtc->id, &p->crtc->props, PROP_MODE_ID, p->mode_blob);
            add(req, p->crtc->id, &p->crtc->props, PROP_ACTIVE, 1);
        }
        plane_set(req, p->crtc->primary, p->crtc->id, p->fb, 0, 0, p->mode.hdisplay,
            p->mode.vdisplay, buffer->width, buffer->height);
        p->fence = sync_file(state);
        if (p->fence >= 0)
            add(req, p->crtc->primary->id, &p->crtc->primary->props, PROP_IN_FENCE_FD,
                (uint64_t)p->fence);
        if (state->committed & SCREEN_VRR)
            add(req, p->crtc->id, &p->crtc->props, PROP_VRR_ENABLED,
                state->adaptive_sync_enabled && c->screen.adaptive_sync_supported);
        p->gamma = (state->committed & SCREEN_GAMMA) != 0;
        if (p->gamma) {
            p->gamma_blob = gamma_blob(kms, state);
            add(req, p->crtc->id, &p->crtc->props, PROP_GAMMA_LUT, p->gamma_blob);
        }
        if (p->crtc->cursor) {
            if (c->cursor_on)
                plane_set(req, p->crtc->cursor, p->crtc->id, c->cursor_fb[c->cursor_front],
                    c->cursor_x, c->cursor_y, (int)kms->cursor_width, (int)kms->cursor_height,
                    (int)kms->cursor_width, (int)kms->cursor_height);
            else
                plane_set(req, p->crtc->cursor, 0, 0, 0, 0, 0, 0, 0, 0);
        }
    }
    uint32_t flags = test ? DRM_MODE_ATOMIC_TEST_ONLY : DRM_MODE_ATOMIC_NONBLOCK;
    if (modeset) flags |= DRM_MODE_ATOMIC_ALLOW_MODESET;
    if (!test && flip) flags |= DRM_MODE_PAGE_FLIP_EVENT;
    if (tearing && !modeset) {
        ok = ok && kms->async_atomic;
        flags |= DRM_MODE_PAGE_FLIP_ASYNC;
    }
    if (modeset && !test) flags &= ~DRM_MODE_ATOMIC_NONBLOCK;
    ok = ok && drmModeAtomicCommit(kms->fd, req, flags, kms) == 0;
    for (size_t i = 0; plans && i < count; i++) {
        struct plan *p = &plans[i];
        if (p->fence >= 0) close(p->fence);
        struct connector *c = wl_container_of(updates[i].output, c, screen);
        const struct screen_state *state = &updates[i].base;
        if (!ok || test) {
            if (p->mode_blob) drmModeDestroyPropertyBlob(kms->fd, p->mode_blob);
            if (p->gamma_blob) drmModeDestroyPropertyBlob(kms->fd, p->gamma_blob);
            continue;
        }
        if (!p->enable) {
            if (p->crtc) {
                if (p->crtc->mode_blob) drmModeDestroyPropertyBlob(kms->fd, p->crtc->mode_blob);
                p->crtc->mode_blob = 0;
                p->crtc->active = false;
                p->crtc->owner = NULL;
            }
            c->crtc = NULL;
            wlr_buffer_unlock(c->queued);
            wlr_buffer_unlock(c->current);
            c->queued = c->current = NULL;
            c->flip_pending = false;
            continue;
        }
        c->crtc = p->crtc;
        p->crtc->owner = c;
        p->crtc->active = true;
        if (p->modeset) {
            if (p->crtc->mode_blob) drmModeDestroyPropertyBlob(kms->fd, p->crtc->mode_blob);
            p->crtc->mode_blob = p->mode_blob;
            p->crtc->mode = p->mode;
        }
        if (p->gamma) {
            if (p->crtc->gamma_blob) drmModeDestroyPropertyBlob(kms->fd, p->crtc->gamma_blob);
            p->crtc->gamma_blob = p->gamma_blob;
        }
        if (state->committed & SCREEN_BUFFER) {
            wlr_buffer_unlock(c->queued);
            c->queued = wlr_buffer_lock(state->buffer);
            c->flip_pending = true;
            c->flip_seq = c->screen.commit_seq + 1;
        }
        c->cursor_committed = c->cursor_on;
    }
    if (req) drmModeAtomicFree(req);
    free(plans);
    return ok;
}

static void wait_fence(int fd) {
    if (fd < 0) return;
    struct pollfd pfd = { .fd = fd, .events = POLLIN };
    poll(&pfd, 1, 1000);
    close(fd);
}

static void present(struct connector *c, unsigned seq, unsigned sec, unsigned usec);

static void legacy_modeset_done(void *data) {
    struct connector *c = data;
    if (!c->present_pending) return;
    c->present_pending = false;
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    present(c, 0, (unsigned)now.tv_sec, (unsigned)(now.tv_nsec / 1000));
    screen_send_frame(&c->screen);
}

static bool legacy_commit(struct kms *kms, struct screen_update *updates, size_t count, bool test) {
    if (!test) drain_flips(kms, updates, count);
    for (size_t i = 0; i < count; i++) {
        struct connector *c = wl_container_of(updates[i].output, c, screen);
        const struct screen_state *state = &updates[i].base;
        if (!enabled_after(c, state)) {
            if (test || !c->crtc) continue;
            drmModeSetCrtc(kms->fd, c->crtc->id, 0, 0, 0, NULL, 0, NULL);
            c->crtc->active = false;
            c->crtc->owner = NULL;
            c->crtc = NULL;
            wlr_buffer_unlock(c->queued);
            wlr_buffer_unlock(c->current);
            c->queued = c->current = NULL;
            continue;
        }
        struct crtc *crtc = pick_crtc(c);
        drmModeModeInfo mode;
        if (!crtc || !mode_for(c, state, &mode)) return false;
        struct wlr_buffer *buffer = (state->committed & SCREEN_BUFFER) ? state->buffer : c->current;
        uint32_t fb = buffer ? fb_for(kms, buffer) : 0;
        if (!fb || state->tearing_page_flip) return false;
        if (test) continue;
        wait_fence(sync_file(state));
        bool modeset = !crtc->active || crtc->owner != c || memcmp(&mode, &crtc->mode, sizeof(mode));
        if (modeset) {
            if (drmModeSetCrtc(kms->fd, crtc->id, fb, 0, 0, &c->id, 1, &mode)) return false;
            crtc->mode = mode;
            crtc->active = true;
            crtc->owner = c;
            c->crtc = crtc;
            wlr_buffer_unlock(c->current);
            c->current = wlr_buffer_lock(buffer);
            c->present_pending = true;
            c->flip_seq = c->screen.commit_seq + 1;
            wl_event_loop_add_idle(wl_display_get_event_loop(kms->server->display),
                legacy_modeset_done, c);
        } else if (state->committed & SCREEN_BUFFER) {
            if (drmModePageFlip(kms->fd, crtc->id, fb, DRM_MODE_PAGE_FLIP_EVENT, kms)) return false;
            wlr_buffer_unlock(c->queued);
            c->queued = wlr_buffer_lock(buffer);
            c->flip_pending = true;
            c->flip_seq = c->screen.commit_seq + 1;
        }
        if ((state->committed & SCREEN_GAMMA) && state->gamma)
            drmModeCrtcSetGamma(kms->fd, crtc->id, (uint32_t)state->gamma_size, state->gamma,
                state->gamma + state->gamma_size, state->gamma + state->gamma_size * 2);
    }
    return true;
}

static bool commit(struct screen_update *updates, size_t count, bool test) {
    struct connector *first = wl_container_of(updates[0].output, first, screen);
    struct kms *kms = first->kms;
    if (!session_active(kms->server)) return test ? false : true;
    return kms->atomic ? atomic_commit(kms, updates, count, test) :
        legacy_commit(kms, updates, count, test);
}

static bool connector_test(struct screen_update *updates, size_t count) {
    return commit(updates, count, true);
}

static bool connector_commit(struct screen_update *updates, size_t count) {
    return commit(updates, count, false);
}

static const struct wlr_drm_format_set *connector_formats(struct screen *screen) {
    struct connector *c = wl_container_of(screen, c, screen);
    struct crtc *crtc = pick_crtc(c);
    return crtc && crtc->primary ? &crtc->primary->formats : NULL;
}

static size_t connector_gamma_size(struct screen *screen) {
    struct connector *c = wl_container_of(screen, c, screen);
    struct crtc *crtc = pick_crtc(c);
    return crtc ? crtc->gamma_size : 0;
}

static bool cursor_upload(struct connector *c, struct wlr_buffer *buffer) {
    struct kms *kms = c->kms;
    void *data;
    uint32_t format;
    size_t stride;
    if (buffer->width > (int)kms->cursor_width || buffer->height > (int)kms->cursor_height ||
            !wlr_buffer_begin_data_ptr_access(buffer, WLR_BUFFER_DATA_PTR_ACCESS_READ, &data,
                &format, &stride)) return false;
    bool ok = format == DRM_FORMAT_ARGB8888;
    int back = c->cursor_front ^ 1;
    if (ok && !c->cursor_bo[back]) {
        c->cursor_bo[back] = gbm_bo_create(kms->gbm, (uint32_t)kms->cursor_width,
            (uint32_t)kms->cursor_height, GBM_FORMAT_ARGB8888, GBM_BO_USE_CURSOR | GBM_BO_USE_WRITE);
        uint32_t handle = c->cursor_bo[back] ? gbm_bo_get_handle(c->cursor_bo[back]).u32 : 0;
        uint32_t pitch = c->cursor_bo[back] ? gbm_bo_get_stride(c->cursor_bo[back]) : 0;
        ok = handle && !drmModeAddFB2(kms->fd, (uint32_t)kms->cursor_width,
            (uint32_t)kms->cursor_height, DRM_FORMAT_ARGB8888, (uint32_t[4]){ handle },
            (uint32_t[4]){ pitch }, (uint32_t[4]){ 0 }, &c->cursor_fb[back], 0);
    }
    if (ok) {
        size_t size = kms->cursor_width * kms->cursor_height * 4;
        uint8_t *pixels = calloc(1, size);
        ok = pixels != NULL;
        for (int y = 0; ok && y < buffer->height; y++)
            memcpy(pixels + (size_t)y * kms->cursor_width * 4, (uint8_t *)data + (size_t)y * stride,
                (size_t)buffer->width * 4);
        ok = ok && gbm_bo_write(c->cursor_bo[back], pixels, size) == 0;
        free(pixels);
    }
    wlr_buffer_end_data_ptr_access(buffer);
    if (ok) c->cursor_front = back;
    return ok;
}

static bool connector_cursor(struct screen *screen, struct wlr_buffer *buffer, int hotspot_x,
        int hotspot_y) {
    struct connector *c = wl_container_of(screen, c, screen);
    if (!c->crtc || (c->kms->atomic && !c->crtc->cursor)) return false;
    if (!buffer) {
        c->cursor_on = false;
        if (!c->kms->atomic) drmModeSetCursor(c->kms->fd, c->crtc->id, 0, 0, 0);
        screen_schedule_frame(screen);
        return true;
    }
    if (!cursor_upload(c, buffer)) return false;
    c->cursor_on = true;
    if (!c->kms->atomic)
        return !drmModeSetCursor2(c->kms->fd, c->crtc->id,
            gbm_bo_get_handle(c->cursor_bo[c->cursor_front]).u32, (uint32_t)c->kms->cursor_width,
            (uint32_t)c->kms->cursor_height, hotspot_x, hotspot_y);
    screen_schedule_frame(screen);
    return true;
}

static void connector_move_cursor(struct screen *screen, int x, int y) {
    struct connector *c = wl_container_of(screen, c, screen);
    c->cursor_x = x;
    c->cursor_y = y;
    if (!c->crtc) return;
    if (c->kms->atomic) screen_schedule_frame(screen);
    else drmModeMoveCursor(c->kms->fd, c->crtc->id, x, y);
}

static void connector_destroy(struct screen *screen) {
    struct connector *c = wl_container_of(screen, c, screen);
    c->present_pending = false;
    if (c->crtc) {
        c->crtc->owner = NULL;
        c->crtc->active = false;
    }
    wlr_buffer_unlock(c->queued);
    wlr_buffer_unlock(c->current);
    for (int i = 0; i < 2; i++) {
        if (c->cursor_fb[i]) drmModeRmFB(c->kms->fd, c->cursor_fb[i]);
        if (c->cursor_bo[i]) gbm_bo_destroy(c->cursor_bo[i]);
    }
    wl_list_remove(&c->link);
    free(c);
}

static const struct screen_impl connector_impl = {
    .test = connector_test,
    .commit = connector_commit,
    .formats = connector_formats,
    .gamma_size = connector_gamma_size,
    .cursor = connector_cursor,
    .move_cursor = connector_move_cursor,
    .destroy = connector_destroy,
};

static void present(struct connector *c, unsigned seq, unsigned sec, unsigned usec) {
    struct screen_present event = {
        .commit_seq = c->flip_seq, .presented = true, .seq = seq,
        .when = { .tv_sec = sec, .tv_nsec = (long)usec * 1000 },
        .refresh = c->screen.refresh ? (int)(1000000000000LL / c->screen.refresh) : 0,
        .flags = WP_PRESENTATION_FEEDBACK_KIND_VSYNC | WP_PRESENTATION_FEEDBACK_KIND_HW_CLOCK |
            WP_PRESENTATION_FEEDBACK_KIND_HW_COMPLETION,
    };
    screen_send_present(&c->screen, &event);
}

static void page_flip(int fd, unsigned seq, unsigned sec, unsigned usec, unsigned crtc_id,
        void *data) {
    struct kms *kms = data;
    struct connector *c;
    wl_list_for_each(c, &kms->connectors, link) {
        if (!c->crtc || c->crtc->id != crtc_id || !c->flip_pending) continue;
        c->flip_pending = false;
        wlr_buffer_unlock(c->current);
        c->current = c->queued;
        c->queued = NULL;
        present(c, seq, sec, usec);
        if (session_active(kms->server)) screen_send_frame(&c->screen);
        return;
    }
}

static int drm_event(int fd, uint32_t mask, void *data) {
    drmEventContext context = { .version = 3, .page_flip_handler2 = page_flip };
    drmHandleEvent(fd, &context);
    return 0;
}

static void add_modes(struct connector *c, drmModeConnector *info) {
    for (int i = 0; i < info->count_modes; i++) {
        drmModeModeInfo *copy = malloc(sizeof(*copy));
        struct screen_mode *mode = copy ? screen_add_mode(&c->screen, info->modes[i].hdisplay,
            info->modes[i].vdisplay, refresh_mhz(&info->modes[i]),
            info->modes[i].type & DRM_MODE_TYPE_PREFERRED) : NULL;
        if (!mode) {
            free(copy);
            continue;
        }
        *copy = info->modes[i];
        mode->data = copy;
        mode->interlaced = info->modes[i].flags & DRM_MODE_FLAG_INTERLACE;
    }
}

static struct connector *connector_find(struct kms *kms, uint32_t id) {
    struct connector *c;
    wl_list_for_each(c, &kms->connectors, link) if (c->id == id) return c;
    return NULL;
}

static void connector_add(struct kms *kms, drmModeConnector *info) {
    struct connector *c = calloc(1, sizeof(*c));
    if (!c) return;
    c->kms = kms;
    c->id = info->connector_id;
    props_read(kms->fd, c->id, DRM_MODE_OBJECT_CONNECTOR, &c->props);
    if (c->props.value[PROP_NON_DESKTOP]) {
        free(c);
        return;
    }
    char name[64];
    const char *type = drmModeGetConnectorTypeName(info->connector_type);
    snprintf(name, sizeof(name), "%s-%u", type ? type : "Unknown", info->connector_type_id);
    screen_init(&c->screen, kms->server, &connector_impl, SCREEN_DRM, name);
    c->screen.phys_width = (int32_t)info->mmWidth;
    c->screen.phys_height = (int32_t)info->mmHeight;
    c->screen.adaptive_sync_supported = c->props.value[PROP_VRR_CAPABLE] != 0;
    for (int i = 0; i < info->count_encoders; i++) {
        drmModeEncoder *encoder = drmModeGetEncoder(kms->fd, info->encoders[i]);
        if (encoder) c->possible_crtcs |= encoder->possible_crtcs;
        for (size_t j = 0; encoder && info->encoders[i] == info->encoder_id && j < kms->crtc_count; j++)
            if (kms->crtcs[j].id == encoder->crtc_id) c->kernel_crtc = &kms->crtcs[j];
        drmModeFreeEncoder(encoder);
    }
    drmModePropertyBlobRes *edid = c->props.value[PROP_EDID] ?
        drmModeGetPropertyBlob(kms->fd, (uint32_t)c->props.value[PROP_EDID]) : NULL;
    if (edid) parse_edid(&c->screen, edid->data, edid->length);
    drmModeFreePropertyBlob(edid);
    screen_describe(&c->screen);
    add_modes(c, info);
    struct screen_mode *preferred = screen_preferred_mode(&c->screen);
    if (preferred) {
        c->screen.width = preferred->width;
        c->screen.height = preferred->height;
        c->screen.refresh = preferred->refresh;
    }
    wl_list_insert(kms->connectors.prev, &c->link);
    wlr_log(WLR_INFO, "tomoe: connector %s (%s)", c->screen.name, c->screen.description);
    output_added(kms->server, &c->screen);
}

static void scan_connectors(struct kms *kms) {
    drmModeRes *res = drmModeGetResources(kms->fd);
    if (!res) return;
    struct connector *c, *next;
    wl_list_for_each_safe(c, next, &kms->connectors, link) {
        drmModeConnector *info = drmModeGetConnector(kms->fd, c->id);
        bool gone = !info || info->connection != DRM_MODE_CONNECTED;
        drmModeFreeConnector(info);
        if (gone) screen_destroy(&c->screen);
    }
    for (int i = 0; i < res->count_connectors; i++) {
        if (connector_find(kms, res->connectors[i])) continue;
        drmModeConnector *info = drmModeGetConnector(kms->fd, res->connectors[i]);
        if (info && info->connection == DRM_MODE_CONNECTED && info->count_modes)
            connector_add(kms, info);
        drmModeFreeConnector(info);
    }
    drmModeFreeResources(res);
}

static int udev_event(int fd, uint32_t mask, void *data) {
    struct kms *kms = data;
    struct udev_device *device = udev_monitor_receive_device(kms->monitor);
    if (!device) return 0;
    const char *action = udev_device_get_action(device);
    if (action && !strcmp(action, "change") && udev_device_get_devnum(device) == kms->devnum &&
            session_active(kms->server))
        scan_connectors(kms);
    udev_device_unref(device);
    return 0;
}

static char *primary_path(void) {
    const char *list = getenv("TOMOE_DRM_DEVICES");
    if (list && *list) {
        size_t length = strcspn(list, ":");
        return strndup(list, length);
    }
    drmDevicePtr devices[64];
    int count = drmGetDevices2(0, devices, 64);
    char *path = NULL;
    for (int i = 0; i < count; i++)
        if (!path && (devices[i]->available_nodes & (1 << DRM_NODE_PRIMARY)))
            path = strdup(devices[i]->nodes[DRM_NODE_PRIMARY]);
    drmFreeDevices(devices, count);
    return path;
}

int kms_create(struct tomoe *s) {
    struct kms *kms = calloc(1, sizeof(*kms));
    char *path = primary_path();
    if (!kms || !path) {
        free(kms);
        free(path);
        wlr_log(WLR_ERROR, "tomoe: no DRM device found");
        return -1;
    }
    kms->server = s;
    kms->fd = -1;
    wl_list_init(&kms->connectors);
    s->kms = kms;
    kms->fd = session_open(s, path, &kms->device);
    struct stat st;
    if (kms->fd < 0 || fstat(kms->fd, &st) != 0) {
        wlr_log(WLR_ERROR, "tomoe: could not open %s", path);
        free(path);
        return -1;
    }
    free(path);
    kms->devnum = st.st_rdev;
    uint64_t cap = 0;
    drmSetClientCap(kms->fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1);
    kms->atomic = !getenv("TOMOE_DRM_LEGACY") && drmSetClientCap(kms->fd, DRM_CLIENT_CAP_ATOMIC, 1) == 0;
    kms->modifiers = drmGetCap(kms->fd, DRM_CAP_ADDFB2_MODIFIERS, &cap) == 0 && cap;
    kms->async_atomic = drmGetCap(kms->fd, DRM_CAP_ATOMIC_ASYNC_PAGE_FLIP, &cap) == 0 && cap;
    kms->cursor_width = drmGetCap(kms->fd, DRM_CAP_CURSOR_WIDTH, &cap) == 0 && cap ? cap : 64;
    kms->cursor_height = drmGetCap(kms->fd, DRM_CAP_CURSOR_HEIGHT, &cap) == 0 && cap ? cap : 64;
    kms->gbm = gbm_create_device(kms->fd);
    if (!kms->gbm || !read_resources(kms)) {
        wlr_log(WLR_ERROR, "tomoe: DRM resources unavailable");
        return -1;
    }
    struct wl_event_loop *loop = wl_display_get_event_loop(s->display);
    kms->source = wl_event_loop_add_fd(loop, kms->fd, WL_EVENT_READABLE, drm_event, kms);
    kms->udev = udev_new();
    kms->monitor = kms->udev ? udev_monitor_new_from_netlink(kms->udev, "udev") : NULL;
    if (kms->monitor) {
        udev_monitor_filter_add_match_subsystem_devtype(kms->monitor, "drm", NULL);
        udev_monitor_enable_receiving(kms->monitor);
        kms->monitor_source = wl_event_loop_add_fd(loop, udev_monitor_get_fd(kms->monitor),
            WL_EVENT_READABLE, udev_event, kms);
    }
    wlr_log(WLR_INFO, "tomoe: DRM %s, modifiers %s, async flips %s",
        kms->atomic ? "atomic" : "legacy", kms->modifiers ? "yes" : "no",
        kms->async_atomic ? "yes" : "no");
    return kms->fd;
}

void kms_start(struct tomoe *s) {
    if (s->kms) scan_connectors(s->kms);
}

struct udev *kms_udev(struct tomoe *s) {
    return s->kms ? s->kms->udev : NULL;
}

void kms_pause(struct tomoe *s) {
    struct kms *kms = s->kms;
    if (!kms) return;
    struct connector *c;
    wl_list_for_each(c, &kms->connectors, link) c->flip_pending = false;
}

void kms_resume(struct tomoe *s) {
    struct kms *kms = s->kms;
    if (!kms) return;
    for (size_t i = 0; i < kms->crtc_count; i++) kms->crtcs[i].active = false;
    scan_connectors(kms);
    struct connector *c;
    wl_list_for_each(c, &kms->connectors, link) {
        if (!c->screen.enabled) continue;
        c->screen.frame_pending = false;
        screen_schedule_frame(&c->screen);
    }
}

void kms_destroy(struct tomoe *s) {
    struct kms *kms = s->kms;
    if (!kms) return;
    struct connector *c, *next;
    wl_list_for_each_safe(c, next, &kms->connectors, link) screen_destroy(&c->screen);
    if (kms->source) wl_event_source_remove(kms->source);
    if (kms->monitor_source) wl_event_source_remove(kms->monitor_source);
    if (kms->monitor) udev_monitor_unref(kms->monitor);
    if (kms->udev) udev_unref(kms->udev);
    for (size_t i = 0; i < kms->plane_count; i++) wlr_drm_format_set_finish(&kms->planes[i].formats);
    for (size_t i = 0; i < kms->crtc_count; i++) {
        if (kms->crtcs[i].mode_blob) drmModeDestroyPropertyBlob(kms->fd, kms->crtcs[i].mode_blob);
        if (kms->crtcs[i].gamma_blob) drmModeDestroyPropertyBlob(kms->fd, kms->crtcs[i].gamma_blob);
    }
    free(kms->crtcs);
    free(kms->planes);
    if (kms->gbm) gbm_device_destroy(kms->gbm);
    if (kms->fd >= 0) session_close(s, kms->device, kms->fd);
    free(kms);
    s->kms = NULL;
}
