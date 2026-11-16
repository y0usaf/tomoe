#include "portal.h"

#include <drm/drm_fourcc.h>
#include <errno.h>
#include <linux/udmabuf.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <spa/param/video/raw-utils.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

struct stream {
    struct portal *portal;
    struct session *session;
    uint32_t source_type;
    struct wl_output *output;
    struct ext_foreign_toplevel_handle_v1 *toplevel;
    struct ext_image_capture_source_v1 *source;
    struct ext_image_copy_capture_session_v1 *capture;
    int width;
    int height;
    uint32_t format;
    uint32_t shm_format;
    dev_t device;
    bool have_device;
    bool constrained;
    bool stopped;
    bool dying;
    size_t refresh;
    bool dmabuf;
    int alloc;
    int udmabuf;
    int drm_fd;
    struct gbm_device *gbm;
    uint64_t modifier;
    uint64_t *modifiers;
    size_t modifier_count;
    int planes;
    bool wants_cursor;
    struct ext_image_copy_capture_cursor_session_v1 *cursor;
    struct ext_image_copy_capture_session_v1 *cursor_capture;
    struct ext_image_copy_capture_frame_v1 *cursor_frame;
    struct spa_source *cursor_retry;
    int cursor_failures;
    struct wl_shm_pool *cursor_pool;
    struct wl_buffer *cursor_buffer;
    void *cursor_map;
    size_t cursor_map_size;
    int bitmap_width;
    int bitmap_height;
    int bitmap_stride;
    uint32_t bitmap_format;
    void *bitmap;
    uint64_t bitmap_generation;
    uint64_t bitmap_sent;
    bool cursor_visible;
    int cursor_x;
    int cursor_y;
    int cursor_hotspot_x;
    int cursor_hotspot_y;
    struct ext_image_copy_capture_frame_v1 *frame;
    struct pw_buffer *frame_pw;
    struct pw_stream *pw;
    struct spa_hook pw_listener;
    uint32_t node_id;
    struct slot *slots[SLOTS_MAX];
    int slot_count;
    uint32_t spa_format;
    uint64_t submitted_at;
    int rejections;
    struct spa_source *retry;
    bool started;
    bool arming;
    bool noted_meta;
    bool renegotiating;
};

static void stream_arm(struct stream *s);
static int stream_push_formats(struct stream *s);
static void cursor_arm(struct stream *s);
static void stream_fail(struct stream *s, const char *why);

static uint32_t spa_format_of(uint32_t fourcc) {
    switch (fourcc) {
    case DRM_FORMAT_ARGB8888: return SPA_VIDEO_FORMAT_BGRA;
    case DRM_FORMAT_XRGB8888: return SPA_VIDEO_FORMAT_BGRx;
    case DRM_FORMAT_ABGR8888: return SPA_VIDEO_FORMAT_RGBA;
    case DRM_FORMAT_XBGR8888: return SPA_VIDEO_FORMAT_RGBx;
    default: return SPA_VIDEO_FORMAT_UNKNOWN;
    }
}

static uint32_t spa_format_of_shm(uint32_t format) {
    switch (format) {
    case WL_SHM_FORMAT_ARGB8888: return SPA_VIDEO_FORMAT_BGRA;
    case WL_SHM_FORMAT_XRGB8888: return SPA_VIDEO_FORMAT_BGRx;
    case WL_SHM_FORMAT_ABGR8888: return SPA_VIDEO_FORMAT_RGBA;
    case WL_SHM_FORMAT_XBGR8888: return SPA_VIDEO_FORMAT_RGBx;
    default: return SPA_VIDEO_FORMAT_UNKNOWN;
    }
}

static uint64_t monotonic_ms(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000 + (uint64_t)now.tv_nsec / 1000000;
}

static int open_render_node(dev_t wanted) {
    const char *named = getenv("TOMOE_SCREENCAST_RENDER_NODE");
    if (named && *named) return open(named, O_RDWR | O_CLOEXEC);
    char path[64];
    int fallback = -1;
    for (int index = 128; index < 200; index++) {
        snprintf(path, sizeof(path), "/dev/dri/renderD%d", index);
        struct stat status;
        if (stat(path, &status) < 0) continue;
        int fd = open(path, O_RDWR | O_CLOEXEC);
        if (fd < 0) continue;
        if (status.st_rdev == wanted) return fd;
        if (fallback < 0) fallback = fd;
        else close(fd);
    }
    return fallback;
}

static bool udmabuf_setup(struct stream *s) {
    if (!s->portal->dmabuf || !s->have_device || !s->modifier_count) return false;
    s->udmabuf = open("/dev/udmabuf", O_RDWR | O_CLOEXEC);
    if (s->udmabuf < 0) return false;
    s->modifier = DRM_FORMAT_MOD_LINEAR;
    s->planes = 1;
    s->alloc = ALLOC_UDMABUF;
    return true;
}

static bool dmabuf_setup(struct stream *s) {
    if (!s->portal->dmabuf || !s->have_device) return false;
    s->drm_fd = open_render_node(s->device);
    if (s->drm_fd < 0) {
        portal_log("no render node for the compositor's device; capturing through memory");
        s->drm_fd = -1;
        return false;
    }
    s->gbm = gbm_create_device(s->drm_fd);
    if (!s->gbm) {
        portal_log("cannot open the render node through GBM; capturing through memory");
        close(s->drm_fd);
        s->drm_fd = -1;
        return false;
    }
    uint64_t chosen = UINT64_MAX;
    bool have = false;
    for (size_t index = 0; index < s->modifier_count; index++) {
        uint64_t modifier = s->modifiers[index];
        if (modifier == DRM_FORMAT_MOD_INVALID) continue;
        struct gbm_bo *bo = gbm_bo_create_with_modifiers2(s->gbm, s->width, s->height,
            s->format, &modifier, 1, GBM_BO_USE_RENDERING);
        if (!bo) continue;
        bool linear = modifier == DRM_FORMAT_MOD_LINEAR;
        gbm_bo_destroy(bo);
        if (linear) {
            chosen = modifier;
            have = true;
            break;
        }
        if (!have) {
            chosen = modifier;
            have = true;
        }
    }
    if (!have) {
        portal_log("no offered modifier allocates on this device; capturing through memory");
        gbm_device_destroy(s->gbm);
        s->gbm = NULL;
        close(s->drm_fd);
        s->drm_fd = -1;
        return false;
    }
    s->modifier = chosen;
    s->alloc = ALLOC_GBM;
    struct gbm_bo *probe = gbm_bo_create_with_modifiers2(s->gbm, s->width, s->height, s->format,
        &s->modifier, 1, GBM_BO_USE_RENDERING);
    if (!probe) {
        gbm_device_destroy(s->gbm);
        s->gbm = NULL;
        close(s->drm_fd);
        s->drm_fd = -1;
        return false;
    }
    s->planes = gbm_bo_get_plane_count(probe);
    gbm_bo_destroy(probe);
    if (s->planes < 1 || s->planes > PLANES_MAX) {
        gbm_device_destroy(s->gbm);
        s->gbm = NULL;
        close(s->drm_fd);
        s->drm_fd = -1;
        s->planes = 0;
        return false;
    }
    return true;
}

static void slot_free(struct stream *s, struct slot *slot) {
    if (slot->buffer) wl_buffer_destroy(slot->buffer);
    if (slot->bo) gbm_bo_destroy(slot->bo);
    for (int plane = 0; plane < slot->planes; plane++)
        if (slot->fds[plane] >= 0) close(slot->fds[plane]);
    if (slot->memfd >= 0) close(slot->memfd);
    free(slot);
}

static struct slot *slot_create(struct stream *s, bool want_dmabuf) {
    struct slot *slot = calloc(1, sizeof(*slot));
    if (!slot) return NULL;
    slot->memfd = -1;
    slot->fds[0] = -1;
    slot->width = s->width;
    slot->height = s->height;
    if (!want_dmabuf) goto memory;
    if (s->alloc == ALLOC_UDMABUF) {
        long page = sysconf(_SC_PAGESIZE);
        size_t stride = (size_t)s->width * 4;
        size_t size = stride * (size_t)s->height;
        size_t mapped = page > 0 ? (size + (size_t)page - 1) / (size_t)page * (size_t)page : size;
        slot->memfd = memfd_create("tomoe-portal-udmabuf", MFD_CLOEXEC);
        if (slot->memfd < 0 || ftruncate(slot->memfd, (off_t)mapped) < 0) goto failed;
        void *pages = mmap(NULL, mapped, PROT_READ | PROT_WRITE, MAP_SHARED, slot->memfd, 0);
        if (pages == MAP_FAILED) goto failed;
        memset(pages, 0, mapped);
        munmap(pages, mapped);
        struct udmabuf_create create = {
            .memfd = (uint32_t)slot->memfd,
            .flags = UDMABUF_FLAGS_CLOEXEC,
            .offset = 0,
            .size = mapped,
        };
        slot->fds[0] = ioctl(s->udmabuf, UDMABUF_CREATE, &create);
        if (slot->fds[0] < 0) goto failed;
        struct zwp_linux_buffer_params_v1 *params = zwp_linux_dmabuf_v1_create_params(
            s->portal->dmabuf);
        zwp_linux_buffer_params_v1_add(params, slot->fds[0], 0, 0, (int32_t)stride,
            (uint32_t)(s->modifier >> 32), (uint32_t)s->modifier);
        slot->buffer = zwp_linux_buffer_params_v1_create_immed(params, s->width, s->height,
            s->format, 0);
        zwp_linux_buffer_params_v1_destroy(params);
        if (!slot->buffer) goto failed;
        slot->planes = 1;
        slot->offset[0] = 0;
        slot->stride[0] = (uint32_t)stride;
        slot->size[0] = (uint32_t)size;
        slot->kind = SPA_DATA_DmaBuf;
        return slot;
    }
    if (s->alloc == ALLOC_GBM) {
        struct gbm_bo *bo = gbm_bo_create_with_modifiers2(s->gbm, s->width, s->height, s->format,
            &s->modifier, 1, GBM_BO_USE_RENDERING);
        if (!bo) goto failed;
        int planes = gbm_bo_get_plane_count(bo);
        if (planes < 1 || planes > PLANES_MAX) {
            gbm_bo_destroy(bo);
            goto failed;
        }
        struct zwp_linux_buffer_params_v1 *params = zwp_linux_dmabuf_v1_create_params(
            s->portal->dmabuf);
        for (int plane = 0; plane < planes; plane++) {
            slot->fds[plane] = gbm_bo_get_fd_for_plane(bo, plane);
            if (slot->fds[plane] < 0) {
                zwp_linux_buffer_params_v1_destroy(params);
                goto failed;
            }
            slot->offset[plane] = gbm_bo_get_offset(bo, plane);
            slot->stride[plane] = gbm_bo_get_stride_for_plane(bo, plane);
            slot->size[plane] = slot->stride[plane] * (uint32_t)s->height;
            zwp_linux_buffer_params_v1_add(params, slot->fds[plane], plane, slot->offset[plane],
                slot->stride[plane], (uint32_t)(s->modifier >> 32), (uint32_t)s->modifier);
        }
        slot->buffer = zwp_linux_buffer_params_v1_create_immed(params, s->width, s->height,
            s->format, 0);
        zwp_linux_buffer_params_v1_destroy(params);
        if (!slot->buffer) goto failed;
        slot->bo = bo;
        slot->planes = planes;
        slot->kind = SPA_DATA_DmaBuf;
        return slot;
    }
memory:
    size_t stride = (size_t)s->width * 4;
    size_t size = stride * (size_t)s->height;
    slot->memfd = memfd_create("tomoe-portal-pwbuf", MFD_CLOEXEC);
    if (slot->memfd < 0 || ftruncate(slot->memfd, (off_t)size) < 0) goto failed;
    struct wl_shm_pool *pool = wl_shm_create_pool(s->portal->shm, slot->memfd, (int32_t)size);
    if (!pool) goto failed;
    slot->buffer = wl_shm_pool_create_buffer(pool, 0, s->width, s->height, (int32_t)stride,
        s->shm_format);
    wl_shm_pool_destroy(pool);
    if (!slot->buffer) goto failed;
    slot->planes = 1;
    slot->fds[0] = slot->memfd;
    slot->stride[0] = (uint32_t)stride;
    slot->size[0] = (uint32_t)size;
    slot->kind = SPA_DATA_MemFd;
    return slot;

failed:
    slot_free(s, slot);
    return NULL;
}

static bool slot_matches(struct stream *s, const struct slot *slot) {
    return slot->buffer && slot->width == s->width && slot->height == s->height;
}

static void write_cursor_meta(struct stream *s, struct pw_buffer *buffer) {
    if (!buffer || !buffer->buffer) return;
    struct spa_meta *meta = spa_buffer_find_meta(buffer->buffer, SPA_META_Cursor);
    if (!meta || !meta->data || meta->size < sizeof(struct spa_meta_cursor)) {
        if (!s->noted_meta) {
            portal_log("the PipeWire buffer carries no cursor metadata");
            s->noted_meta = true;
        }
        return;
    }
    struct spa_meta_cursor *cursor = meta->data;
    memset(cursor, 0, sizeof(*cursor));
    cursor->id = s->cursor_visible ? (uint32_t)(s->bitmap_generation ? s->bitmap_generation : 1)
                                  : 0;
    cursor->position = (struct spa_point){ .x = s->cursor_x, .y = s->cursor_y };
    cursor->hotspot = (struct spa_point){ .x = s->cursor_hotspot_x, .y = s->cursor_hotspot_y };
    if (!s->cursor_visible || !s->bitmap || s->bitmap_generation == s->bitmap_sent) return;
    size_t cursor_size = sizeof(struct spa_meta_cursor);
    size_t bitmap_size = sizeof(struct spa_meta_bitmap);
    size_t data_size = (size_t)s->bitmap_stride * (size_t)s->bitmap_height;
    uint32_t format = spa_format_of_shm(s->bitmap_format);
    if (!data_size || !format || cursor_size + bitmap_size + data_size > meta->size) return;
    struct spa_meta_bitmap *bitmap = (struct spa_meta_bitmap *)((uint8_t *)cursor + cursor_size);
    memcpy((uint8_t *)cursor + cursor_size + bitmap_size, s->bitmap, data_size);
    *bitmap = (struct spa_meta_bitmap){
        .format = format,
        .size = { .width = (uint32_t)s->bitmap_width, .height = (uint32_t)s->bitmap_height },
        .stride = s->bitmap_stride,
        .offset = (uint32_t)bitmap_size,
    };
    cursor->bitmap_offset = (uint32_t)cursor_size;
    s->bitmap_sent = s->bitmap_generation;
}

static void frame_ready(void *data, struct ext_image_copy_capture_frame_v1 *frame) {
    struct stream *s = data;
    if (s->dying) return;
    s->rejections = 0;
    s->submitted_at = 0;
    struct pw_buffer *buffer = s->frame_pw;
    if (buffer && s->pw) {
        write_cursor_meta(s, buffer);
        pw_stream_queue_buffer(s->pw, buffer);
    }
    s->frame_pw = NULL;
    if (s->frame) ext_image_copy_capture_frame_v1_destroy(s->frame);
    s->frame = NULL;
    stream_arm(s);
}

static void frame_retry(void *data, uint64_t expirations) {
    struct stream *s = data;
    pw_loop_destroy_source(s->portal->loop, s->retry);
    s->retry = NULL;
    stream_arm(s);
}

static void frame_failed(void *data, struct ext_image_copy_capture_frame_v1 *frame,
        uint32_t reason) {
    struct stream *s = data;
    if (s->dying) return;
    if (s->frame) ext_image_copy_capture_frame_v1_destroy(s->frame);
    s->frame = NULL;
    if (s->frame_pw && s->pw) pw_stream_queue_buffer(s->pw, s->frame_pw);
    s->frame_pw = NULL;
    uint64_t elapsed = s->submitted_at ? monotonic_ms() - s->submitted_at : 0;
    s->submitted_at = 0;
    if (elapsed < 100) {
        s->rejections++;
    } else {
        s->rejections = 0;
    }
    if (s->rejections >= 3) {
        s->rejections = 0;
        if (s->alloc != ALLOC_SHM) {
            portal_log("the compositor keeps refusing our buffers; capturing through memory");
            s->alloc = ALLOC_SHM;
            s->dmabuf = false;
            s->renegotiating = true;
            if (s->pw) stream_push_formats(s);
            return;
        }
        stream_fail(s, "the compositor keeps refusing capture buffers");
        return;
    }
    if (!s->retry) {
        s->retry = pw_loop_add_timer(s->portal->loop, frame_retry, s);
        if (s->retry) {
            struct timespec retry = { .tv_sec = 0, .tv_nsec = FRAME_RETRY_MS * 1000000L };
            pw_loop_update_timer(s->portal->loop, s->retry, &retry, NULL, false);
        }
    }
}

static void frame_transform(void *data, struct ext_image_copy_capture_frame_v1 *frame,
        uint32_t transform) {
}

static void frame_damage(void *data, struct ext_image_copy_capture_frame_v1 *frame, int32_t x,
        int32_t y, int32_t width, int32_t height) {
}

static void frame_presentation(void *data, struct ext_image_copy_capture_frame_v1 *frame,
        uint32_t tv_sec_hi, uint32_t tv_sec_lo, uint32_t tv_nsec) {
}

static const struct ext_image_copy_capture_frame_v1_listener frame_listener = {
    .transform = frame_transform,
    .damage = frame_damage,
    .presentation_time = frame_presentation,
    .ready = frame_ready,
    .failed = frame_failed,
};

static void stream_arm(struct stream *s) {
    if (!s->pw || !s->constrained || s->dying || s->frame || s->stopped) return;
    if (s->renegotiating) return;
    if (s->slot_count == 0) return;
    struct pw_buffer *buffer = pw_stream_dequeue_buffer(s->pw);
    if (!buffer) return;
    struct slot *slot = buffer->user_data;
    if (!slot || !slot_matches(s, slot)) {
        pw_stream_queue_buffer(s->pw, buffer);
        return;
    }
    struct ext_image_copy_capture_frame_v1 *frame =
        ext_image_copy_capture_session_v1_create_frame(s->capture);
    if (!frame) {
        pw_stream_queue_buffer(s->pw, buffer);
        return;
    }
    ext_image_copy_capture_frame_v1_add_listener(frame, &frame_listener, s);
    s->frame = frame;
    s->frame_pw = buffer;
    ext_image_copy_capture_frame_v1_attach_buffer(frame, slot->buffer);
    ext_image_copy_capture_frame_v1_capture(frame);
    s->submitted_at = monotonic_ms();
    wl_display_flush(s->portal->display);
}

static void stream_push_buffers(struct stream *s);

static void stream_state_changed(void *data, enum pw_stream_state old, enum pw_stream_state state,
        const char *error) {
    struct stream *s = data;
    if (s->dying) return;
    if (state == PW_STREAM_STATE_PAUSED || state == PW_STREAM_STATE_STREAMING) {
        s->node_id = pw_stream_get_node_id(s->pw);
        if (!s->started) {
            s->started = true;
            portal_start_reply(s->session, 0, s->node_id, (uint32_t)s->width, (uint32_t)s->height);
        }
        stream_arm(s);
    } else if (state == PW_STREAM_STATE_ERROR || state == PW_STREAM_STATE_UNCONNECTED) {
        stream_fail(s, error ? error : "the PipeWire stream disconnected");
    }
}

static void stream_param_changed(void *data, uint32_t id, const struct spa_pod *param) {
    struct stream *s = data;
    if (s->dying || id != SPA_PARAM_Format || !param) return;
    struct spa_video_info_raw raw;
    if (spa_format_video_raw_parse(param, &raw) < 0) return;
    bool dmabuf = (raw.flags & SPA_VIDEO_FLAG_MODIFIER) && (s->gbm || s->udmabuf >= 0);
    s->dmabuf = dmabuf;
    if (dmabuf) s->modifier = raw.modifier;
    stream_push_buffers(s);
}

static void stream_process(void *data) {
    stream_arm(data);
}

static void arm_deferred(void *data) {
    stream_arm(data);
}

static void stream_add_buffer(void *data, struct pw_buffer *buffer) {
    struct stream *s = data;
    if (s->dying || s->slot_count >= SLOTS_MAX || !buffer->buffer) return;
    uint32_t negotiated = buffer->buffer->n_datas ? buffer->buffer->datas[0].type : 0;
    bool want_dmabuf = s->alloc != ALLOC_SHM && (negotiated & (1u << SPA_DATA_DmaBuf));
    struct slot *slot = slot_create(s, want_dmabuf);
    if (!slot) {
        if (s->alloc != ALLOC_SHM) {
            portal_log("cannot allocate a DMA-BUF capture buffer; capturing through memory");
            s->alloc = ALLOC_SHM;
            s->dmabuf = false;
            s->renegotiating = true;
            stream_push_formats(s);
        }
        return;
    }
    if ((int)buffer->buffer->n_datas < slot->planes) {
        slot_free(s, slot);
        return;
    }
    for (int plane = 0; plane < slot->planes; plane++) {
        struct spa_data *spa = &buffer->buffer->datas[plane];
        spa->type = (uint32_t)slot->kind;
        spa->flags = SPA_DATA_FLAG_READWRITE;
        spa->fd = slot->fds[plane];
        spa->mapoffset = 0;
        spa->maxsize = slot->size[plane];
        spa->data = NULL;
        *spa->chunk = (struct spa_chunk){
            .offset = slot->offset[plane],
            .size = slot->size[plane],
            .stride = (int32_t)slot->stride[plane],
        };
    }
    buffer->user_data = slot;
    s->slots[s->slot_count++] = slot;
    s->dmabuf = slot->kind == SPA_DATA_DmaBuf;
    s->renegotiating = false;
    portal_defer(s->portal, arm_deferred, s);
}

static void stream_remove_buffer(void *data, struct pw_buffer *buffer) {
    struct stream *s = data;
    struct slot *slot = buffer->user_data;
    buffer->user_data = NULL;
    if (!slot) return;
    for (int index = 0; index < s->slot_count; index++)
        if (s->slots[index] == slot) {
            memmove(&s->slots[index], &s->slots[index + 1],
                (size_t)(s->slot_count - index - 1) * sizeof(s->slots[0]));
            s->slot_count--;
            break;
        }
    if (s->frame_pw == buffer) {
        if (s->frame) ext_image_copy_capture_frame_v1_destroy(s->frame);
        s->frame = NULL;
        s->frame_pw = NULL;
    }
    slot_free(s, slot);
}

static const struct pw_stream_events stream_events = {
    PW_VERSION_STREAM_EVENTS,
    .state_changed = stream_state_changed,
    .param_changed = stream_param_changed,
    .add_buffer = stream_add_buffer,
    .remove_buffer = stream_remove_buffer,
    .process = stream_process,
};

static int build_formats(struct stream *s, struct spa_pod_builder *builder,
        const struct spa_pod **params) {
    int count = 0;
    struct spa_rectangle size = SPA_RECTANGLE((uint32_t)s->width, (uint32_t)s->height);
    struct spa_fraction rate = SPA_FRACTION(0, 1);
    struct spa_fraction limit = SPA_FRACTION(s->refresh ? (uint32_t)s->refresh : 60u, 1);
    struct spa_fraction zero = SPA_FRACTION(0, 1);
    if (s->alloc != ALLOC_SHM)
        params[count++] = spa_pod_builder_add_object(builder,
            SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat,
            SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_video),
            SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
            SPA_FORMAT_VIDEO_format, SPA_POD_Id(s->spa_format),
            SPA_FORMAT_VIDEO_modifier, SPA_POD_Long((long long)s->modifier),
            SPA_FORMAT_VIDEO_size, SPA_POD_Rectangle(&size),
            SPA_FORMAT_VIDEO_framerate, SPA_POD_Fraction(&rate),
            SPA_FORMAT_VIDEO_maxFramerate, SPA_POD_CHOICE_RANGE_Fraction(&limit, &zero, &limit));
    params[count++] = spa_pod_builder_add_object(builder,
        SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat,
        SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_video),
        SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
        SPA_FORMAT_VIDEO_format, SPA_POD_Id(s->spa_format),
        SPA_FORMAT_VIDEO_size, SPA_POD_Rectangle(&size),
        SPA_FORMAT_VIDEO_framerate, SPA_POD_Fraction(&rate),
        SPA_FORMAT_VIDEO_maxFramerate, SPA_POD_CHOICE_RANGE_Fraction(&limit, &zero, &limit));
    return count;
}

static int stream_push_formats(struct stream *s) {
    if (!s->pw) return -1;
    uint8_t buffer[1024];
    struct spa_pod_builder builder = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
    const struct spa_pod *params[2];
    int count = build_formats(s, &builder, params);
    return pw_stream_update_params(s->pw, params, (uint32_t)count);
}

static void stream_push_buffers(struct stream *s) {
    if (!s->pw || s->dying) return;
    uint8_t buffer[512];
    struct spa_pod_builder builder = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
    const struct spa_pod *params[1];
    size_t stride = (size_t)s->width * 4;
    size_t size = stride * (size_t)s->height;
    int types = 1 << SPA_DATA_MemFd;
    if (s->alloc != ALLOC_SHM && s->planes == 1) types |= 1 << SPA_DATA_DmaBuf;
    if (s->alloc != ALLOC_SHM && s->planes > 1)
        params[0] = spa_pod_builder_add_object(&builder,
            SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers,
            SPA_PARAM_BUFFERS_buffers, SPA_POD_CHOICE_RANGE_Int(8, 2, 16),
            SPA_PARAM_BUFFERS_blocks, SPA_POD_Int(s->planes),
            SPA_PARAM_BUFFERS_dataType, SPA_POD_CHOICE_FLAGS_Int(1 << SPA_DATA_DmaBuf));
    else if (s->alloc != ALLOC_SHM)
        params[0] = spa_pod_builder_add_object(&builder,
            SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers,
            SPA_PARAM_BUFFERS_buffers, SPA_POD_CHOICE_RANGE_Int(8, 2, 16),
            SPA_PARAM_BUFFERS_blocks, SPA_POD_Int(1),
            SPA_PARAM_BUFFERS_dataType, SPA_POD_CHOICE_FLAGS_Int(types));
    else
        params[0] = spa_pod_builder_add_object(&builder,
            SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers,
            SPA_PARAM_BUFFERS_buffers, SPA_POD_CHOICE_RANGE_Int(8, 2, 16),
            SPA_PARAM_BUFFERS_blocks, SPA_POD_Int(1),
            SPA_PARAM_BUFFERS_size, SPA_POD_Int((int)size),
            SPA_PARAM_BUFFERS_stride, SPA_POD_Int((int)stride),
            SPA_PARAM_BUFFERS_dataType, SPA_POD_CHOICE_FLAGS_Int(1 << SPA_DATA_MemFd));
    if (pw_stream_update_params(s->pw, params, 1) < 0)
        portal_log("cannot publish the capture buffer shape");
}

static size_t cursor_meta_size(void) {
    return sizeof(struct spa_meta_cursor) + sizeof(struct spa_meta_bitmap) +
        (size_t)CURSOR_EDGE * CURSOR_EDGE * 4;
}

static int stream_connect(struct stream *s) {
    struct portal *p = s->portal;
    struct pw_core *core = portal_core(p);
    if (!core) {
        portal_log("no PipeWire daemon");
        return -1;
    }
    const char *name = s->source_type == SOURCE_WINDOW ? "tomoe-portal-toplevel-stream"
                                                      : "tomoe-portal-stream";
    struct pw_properties *properties = pw_properties_new(
        PW_KEY_MEDIA_TYPE, "Video",
        PW_KEY_MEDIA_CATEGORY, "Source",
        PW_KEY_MEDIA_ROLE, "Screen",
        PW_KEY_NODE_NAME, name,
        PW_KEY_NODE_DESCRIPTION, "tomoe portal screencast",
        NULL);
    s->pw = pw_stream_new(core, "tomoe-screencast", properties);
    if (!s->pw) return -1;
    pw_stream_add_listener(s->pw, &s->pw_listener, &stream_events, s);
    uint8_t pod_buffer[2048];
    struct spa_pod_builder builder = SPA_POD_BUILDER_INIT(pod_buffer, sizeof(pod_buffer));
    const struct spa_pod *params[3];
    int count = build_formats(s, &builder, params);
    if (s->wants_cursor)
        params[count++] = spa_pod_builder_add_object(&builder,
            SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta,
            SPA_PARAM_META_type, SPA_POD_Id(SPA_META_Cursor),
            SPA_PARAM_META_size, SPA_POD_Int((int)cursor_meta_size()));
    return pw_stream_connect(s->pw, PW_DIRECTION_OUTPUT, PW_ID_ANY,
        PW_STREAM_FLAG_DRIVER | PW_STREAM_FLAG_ALLOC_BUFFERS, params, (uint32_t)count);
}

static void cursor_ensure_buffer(struct stream *s) {
    if (!s->portal->shm || s->bitmap_width <= 0 || s->bitmap_height <= 0) return;
    size_t stride = (size_t)s->bitmap_width * 4;
    size_t size = stride * (size_t)s->bitmap_height;
    if (s->cursor_buffer && s->cursor_map_size == size) return;
    if (s->cursor_buffer) {
        wl_buffer_destroy(s->cursor_buffer);
        s->cursor_buffer = NULL;
    }
    if (s->cursor_pool) {
        wl_shm_pool_destroy(s->cursor_pool);
        s->cursor_pool = NULL;
    }
    if (s->cursor_map) {
        munmap(s->cursor_map, s->cursor_map_size);
        s->cursor_map = NULL;
    }
    s->cursor_map_size = 0;
    int fd = memfd_create("tomoe-portal-cursor", MFD_CLOEXEC);
    if (fd < 0) return;
    if (ftruncate(fd, (off_t)size) < 0) {
        close(fd);
        return;
    }
    void *map = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (map == MAP_FAILED) {
        close(fd);
        return;
    }
    s->cursor_pool = wl_shm_create_pool(s->portal->shm, fd, (int32_t)size);
    close(fd);
    if (!s->cursor_pool) {
        munmap(map, size);
        return;
    }
    s->cursor_buffer = wl_shm_pool_create_buffer(s->cursor_pool, 0, s->bitmap_width,
        s->bitmap_height, (int32_t)stride, s->bitmap_format);
    if (!s->cursor_buffer) {
        wl_shm_pool_destroy(s->cursor_pool);
        s->cursor_pool = NULL;
        munmap(map, size);
        return;
    }
    s->cursor_map = map;
    s->cursor_map_size = size;
    s->bitmap_stride = (int)stride;
}

static void cursor_frame_ready(void *data, struct ext_image_copy_capture_frame_v1 *frame) {
    struct stream *s = data;
    if (s->dying) return;
    s->cursor_failures = 0;
    if (s->cursor_map && s->bitmap_width > 0 && s->bitmap_height > 0) {
        size_t size = (size_t)s->bitmap_stride * (size_t)s->bitmap_height;
        uint8_t *pixels = malloc(size);
        if (pixels) {
            memcpy(pixels, s->cursor_map, size);
            free(s->bitmap);
            s->bitmap = pixels;
            s->bitmap_generation++;
            portal_log("cursor bitmap %dx%d", s->bitmap_width, s->bitmap_height);
        }
    }
    if (s->cursor_frame) ext_image_copy_capture_frame_v1_destroy(s->cursor_frame);
    s->cursor_frame = NULL;
    cursor_arm(s);
}

static void cursor_retry(void *data, uint64_t expirations) {
    struct stream *s = data;
    pw_loop_destroy_source(s->portal->loop, s->cursor_retry);
    s->cursor_retry = NULL;
    cursor_arm(s);
}

static void cursor_frame_failed(void *data, struct ext_image_copy_capture_frame_v1 *frame,
        uint32_t reason) {
    struct stream *s = data;
    if (s->cursor_frame) ext_image_copy_capture_frame_v1_destroy(s->cursor_frame);
    s->cursor_frame = NULL;
    if (++s->cursor_failures <= 3)
        portal_log("cursor frame failed with reason %u", reason);
    if (!s->cursor_retry) {
        s->cursor_retry = pw_loop_add_timer(s->portal->loop, cursor_retry, s);
        if (s->cursor_retry) {
            struct timespec retry = { .tv_sec = 0, .tv_nsec = 100000000L };
            pw_loop_update_timer(s->portal->loop, s->cursor_retry, &retry, NULL, false);
        }
    }
}

static const struct ext_image_copy_capture_frame_v1_listener cursor_frame_listener = {
    .transform = frame_transform,
    .damage = frame_damage,
    .presentation_time = frame_presentation,
    .ready = cursor_frame_ready,
    .failed = cursor_frame_failed,
};

static void cursor_arm(struct stream *s) {
    if (!s->cursor_capture || s->cursor_frame || s->dying || s->stopped) return;
    if (s->bitmap_width <= 0 || s->bitmap_height <= 0) return;
    cursor_ensure_buffer(s);
    if (!s->cursor_buffer) {
        portal_log("no cursor buffer for %dx%d", s->bitmap_width, s->bitmap_height);
        return;
    }
    struct ext_image_copy_capture_frame_v1 *frame =
        ext_image_copy_capture_session_v1_create_frame(s->cursor_capture);
    if (!frame) return;
    ext_image_copy_capture_frame_v1_add_listener(frame, &cursor_frame_listener, s);
    s->cursor_frame = frame;
    ext_image_copy_capture_frame_v1_attach_buffer(frame, s->cursor_buffer);
    ext_image_copy_capture_frame_v1_capture(frame);
    wl_display_flush(s->portal->display);
}

static void cursor_buffer_size(void *data, struct ext_image_copy_capture_session_v1 *session,
        uint32_t width, uint32_t height) {
    struct stream *s = data;
    if (width <= 0 || height <= 0 || width > CURSOR_EDGE || height > CURSOR_EDGE) return;
    if ((int)width != s->bitmap_width || (int)height != s->bitmap_height) {
        if (s->cursor_frame) {
            ext_image_copy_capture_frame_v1_destroy(s->cursor_frame);
            s->cursor_frame = NULL;
        }
    }
    s->bitmap_width = (int)width;
    s->bitmap_height = (int)height;
}

static void cursor_shm_format(void *data, struct ext_image_copy_capture_session_v1 *session,
        uint32_t format) {
    struct stream *s = data;
    s->bitmap_format = format;
}

static void cursor_dmabuf_device(void *data, struct ext_image_copy_capture_session_v1 *session,
        struct wl_array *device) {
}

static void cursor_dmabuf_format(void *data, struct ext_image_copy_capture_session_v1 *session,
        uint32_t format, struct wl_array *modifiers) {
    portal_log("cursor session offers format 0x%x with %zu modifiers", format,
        modifiers->size / sizeof(uint64_t));
}

static void cursor_constraints_done(void *data, struct ext_image_copy_capture_session_v1 *session) {
    struct stream *s = data;
    portal_log("cursor session %dx%d format 0x%x", s->bitmap_width, s->bitmap_height,
        s->bitmap_format);
    cursor_arm(s);
}

static void cursor_constraints_stopped(void *data,
        struct ext_image_copy_capture_session_v1 *session) {
}

static const struct ext_image_copy_capture_session_v1_listener cursor_session_listener = {
    .buffer_size = cursor_buffer_size,
    .shm_format = cursor_shm_format,
    .dmabuf_device = cursor_dmabuf_device,
    .dmabuf_format = cursor_dmabuf_format,
    .done = cursor_constraints_done,
    .stopped = cursor_constraints_stopped,
};

static void cursor_enter(void *data, struct ext_image_copy_capture_cursor_session_v1 *cursor) {
    struct stream *s = data;
    s->cursor_visible = true;
}

static void cursor_leave(void *data, struct ext_image_copy_capture_cursor_session_v1 *cursor) {
    struct stream *s = data;
    s->cursor_visible = false;
}

static void cursor_position(void *data, struct ext_image_copy_capture_cursor_session_v1 *cursor,
        int32_t x, int32_t y) {
    struct stream *s = data;
    s->cursor_x = x;
    s->cursor_y = y;
}

static void cursor_hotspot(void *data, struct ext_image_copy_capture_cursor_session_v1 *cursor,
        int32_t x, int32_t y) {
    struct stream *s = data;
    s->cursor_hotspot_x = x;
    s->cursor_hotspot_y = y;
}

static const struct ext_image_copy_capture_cursor_session_v1_listener cursor_listener = {
    .enter = cursor_enter,
    .leave = cursor_leave,
    .position = cursor_position,
    .hotspot = cursor_hotspot,
};

static void capture_buffer_size(void *data, struct ext_image_copy_capture_session_v1 *session,
        uint32_t width, uint32_t height) {
    struct stream *s = data;
    if (width <= 0 || height <= 0) return;
    if (s->constrained && ((int)width != s->width || (int)height != s->height)) {
        portal_log("the source changed size to %dx%d", width, height);
        s->width = (int)width;
        s->height = (int)height;
        s->renegotiating = true;
        if (s->pw) stream_push_formats(s);
        return;
    }
    s->width = (int)width;
    s->height = (int)height;
}

static void capture_shm_format(void *data, struct ext_image_copy_capture_session_v1 *session,
        uint32_t format) {
    struct stream *s = data;
    if (format == WL_SHM_FORMAT_ARGB8888 || format == WL_SHM_FORMAT_XRGB8888 ||
            format == WL_SHM_FORMAT_ABGR8888 || format == WL_SHM_FORMAT_XBGR8888)
        s->shm_format = format;
    else if (!s->shm_format)
        s->shm_format = format;
}

static void capture_dmabuf_device(void *data, struct ext_image_copy_capture_session_v1 *session,
        struct wl_array *device) {
    struct stream *s = data;
    if (device->size < sizeof(dev_t)) return;
    memcpy(&s->device, device->data, sizeof(dev_t));
    s->have_device = true;
}

static void capture_dmabuf_format(void *data, struct ext_image_copy_capture_session_v1 *session,
        uint32_t format, struct wl_array *modifiers) {
    struct stream *s = data;
    free(s->modifiers);
    s->modifier_count = modifiers->size / sizeof(uint64_t);
    s->modifiers = NULL;
    s->format = format;
    if (!s->modifier_count) return;
    s->modifiers = malloc(modifiers->size);
    if (!s->modifiers) {
        s->modifier_count = 0;
        return;
    }
    memcpy(s->modifiers, modifiers->data, modifiers->size);
}

static void capture_done(void *data, struct ext_image_copy_capture_session_v1 *session) {
    struct stream *s = data;
    s->constrained = true;
}

static void capture_stopped(void *data, struct ext_image_copy_capture_session_v1 *session) {
    struct stream *s = data;
    s->stopped = true;
    if (!s->arming) stream_fail(s, "the compositor stopped the capture");
}

static const struct ext_image_copy_capture_session_v1_listener capture_listener = {
    .buffer_size = capture_buffer_size,
    .shm_format = capture_shm_format,
    .dmabuf_device = capture_dmabuf_device,
    .dmabuf_format = capture_dmabuf_format,
    .done = capture_done,
    .stopped = capture_stopped,
};

static void stream_fail_deferred(void *data) {
    struct stream *s = data;
    if (s->session->start_call) {
        s->session->stream = NULL;
        portal_start_reply(s->session, 2, 0, 0, 0);
        stream_free(s);
        return;
    }
    portal_defer_close(s->session, true);
}

static void stream_fail(struct stream *s, const char *why) {
    if (s->dying) return;
    s->dying = true;
    portal_log("%s", why);
    if (s->arming) return;
    portal_defer(s->portal, stream_fail_deferred, s);
}

void stream_free(struct stream *s) {
    if (!s) return;
    s->dying = true;
    struct portal *p = s->portal;
    struct pw_stream *pw = s->pw;
    s->pw = NULL;
    if (s->retry) pw_loop_destroy_source(p->loop, s->retry);
    if (s->cursor_retry) pw_loop_destroy_source(p->loop, s->cursor_retry);
    if (pw) pw_stream_destroy(pw);
    for (int index = 0; index < s->slot_count; index++)
        if (s->slots[index]) slot_free(s, s->slots[index]);
    s->slot_count = 0;
    if (s->frame) ext_image_copy_capture_frame_v1_destroy(s->frame);
    if (s->cursor_frame) ext_image_copy_capture_frame_v1_destroy(s->cursor_frame);
    if (s->cursor_capture) ext_image_copy_capture_session_v1_destroy(s->cursor_capture);
    if (s->cursor) ext_image_copy_capture_cursor_session_v1_destroy(s->cursor);
    if (s->capture) ext_image_copy_capture_session_v1_destroy(s->capture);
    if (s->source) ext_image_capture_source_v1_destroy(s->source);
    if (s->cursor_buffer) wl_buffer_destroy(s->cursor_buffer);
    if (s->cursor_pool) wl_shm_pool_destroy(s->cursor_pool);
    if (s->cursor_map) munmap(s->cursor_map, s->cursor_map_size);
    if (s->gbm) gbm_device_destroy(s->gbm);
    if (s->udmabuf >= 0) close(s->udmabuf);
    if (s->drm_fd >= 0) close(s->drm_fd);
    free(s->bitmap);
    free(s->modifiers);
    free(s);
}

struct stream *stream_start(struct session *session) {
    struct portal *p = session->portal;
    if (!p->capture || !p->shm) {
        portal_log("the compositor cannot be captured from");
        return NULL;
    }
    struct stream *s = calloc(1, sizeof(*s));
    if (!s) return NULL;
    s->portal = p;
    s->session = session;
    s->arming = true;
    s->source_type = session->choice.type;
    s->drm_fd = -1;
    s->udmabuf = -1;
    s->wants_cursor = session->cursor_mode == CURSOR_METADATA;
    if (s->source_type == SOURCE_MONITOR) {
        struct output_info *info = portal_output_by_name(p, session->choice.output);
        if (!info) {
            portal_log("holding an output that is no longer connected");
            free(s);
            return NULL;
        }
        s->output = info->output;
        s->refresh = info->refresh_mhz > 0 ? (size_t)((info->refresh_mhz + 500) / 1000) : 60;
        s->source = ext_output_image_capture_source_manager_v1_create_source(p->output_sources,
            s->output);
    } else {
        struct toplevel_info *info = portal_toplevel_by_identifier(p, session->choice.identifier);
        if (!info) {
            portal_log("holding a window that is no longer open");
            free(s);
            return NULL;
        }
        s->toplevel = info->handle;
        s->refresh = 60;
        if (!p->window_sources) {
            portal_log("the compositor cannot capture windows");
            free(s);
            return NULL;
        }
        s->source = ext_foreign_toplevel_image_capture_source_manager_v1_create_source(
            p->window_sources, s->toplevel);
    }
    if (!s->source) {
        free(s);
        return NULL;
    }
    uint32_t options = session->cursor_mode == CURSOR_EMBEDDED
        ? EXT_IMAGE_COPY_CAPTURE_MANAGER_V1_OPTIONS_PAINT_CURSORS : 0;
    s->capture = ext_image_copy_capture_manager_v1_create_session(p->capture, s->source,
        options);
    if (!s->capture) {
        stream_free(s);
        return NULL;
    }
    ext_image_copy_capture_session_v1_add_listener(s->capture, &capture_listener, s);
    for (int attempt = 0; attempt < 3 && !s->constrained && !s->stopped; attempt++)
        wl_display_roundtrip(p->display);
    if (!s->constrained || s->stopped || s->width <= 0 || s->height <= 0) {
        portal_log("the compositor never advertised capture constraints");
        stream_free(s);
        return NULL;
    }
    if (!spa_format_of(s->format)) s->format = s->shm_format;
    s->spa_format = spa_format_of(s->format);
    if (!s->spa_format) s->spa_format = spa_format_of_shm(s->shm_format);
    if (!s->spa_format) {
        portal_log("the compositor offers no streamable pixel format");
        stream_free(s);
        return NULL;
    }
    if (!s->shm_format) s->shm_format = s->format;
    if (!dmabuf_setup(s) && !udmabuf_setup(s)) s->alloc = ALLOC_SHM;
    s->dmabuf = s->alloc != ALLOC_SHM;
    if (s->dmabuf)
        portal_log("capturing %dx%d through DMA-BUF (%s) with modifier 0x%llx", s->width,
            s->height, s->alloc == ALLOC_GBM ? "GBM" : "udmabuf",
            (unsigned long long)s->modifier);
    else
        portal_log("capturing %dx%d through memory", s->width, s->height);
    if (s->wants_cursor && s->output) {
        s->cursor = ext_image_copy_capture_manager_v1_create_pointer_cursor_session(p->capture,
            s->source, p->pointer);
        if (s->cursor) {
            ext_image_copy_capture_cursor_session_v1_add_listener(s->cursor, &cursor_listener, s);
            s->cursor_capture = ext_image_copy_capture_cursor_session_v1_get_capture_session(
                s->cursor);
            if (s->cursor_capture)
                ext_image_copy_capture_session_v1_add_listener(s->cursor_capture,
                    &cursor_session_listener, s);
        }
        for (int attempt = 0; attempt < 3 && !s->bitmap_width; attempt++)
            wl_display_roundtrip(p->display);
    }
    if (stream_connect(s) < 0 || s->dying) {
        if (!s->dying) portal_log("cannot open the PipeWire stream");
        stream_free(s);
        return NULL;
    }
    s->arming = false;
    return s;
}
