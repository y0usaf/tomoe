#include "internal.h"
#include <drm_fourcc.h>
#include <sys/stat.h>
#include "ext-image-capture-source-v1-protocol.h"
#include "ext-image-copy-capture-v1-protocol.h"
#include "wlr-screencopy-unstable-v1-protocol.h"

struct copy_frame {
    struct wl_resource *resource;
    struct wl_list link;
    struct wlr_output *output;
    struct wlr_box box;
    uint32_t shm_format, dmabuf_format;
    bool cursor, damage, cursor_locked;
    struct wlr_buffer *buffer;
};

static const struct zwlr_screencopy_frame_v1_interface frame_impl;

static struct copy_frame *frame_from(struct wl_resource *resource) {
    return wl_resource_get_user_data(resource);
}

static void frame_finish(struct copy_frame *frame) {
    if (!frame) return;
    wl_resource_set_user_data(frame->resource, NULL);
    wl_list_remove(&frame->link);
    if (frame->cursor_locked) wlr_output_lock_software_cursors(frame->output, false);
    wlr_buffer_unlock(frame->buffer);
    free(frame);
}

static void frame_fail(struct copy_frame *frame) {
    zwlr_screencopy_frame_v1_send_failed(frame->resource);
    frame_finish(frame);
}

static void frame_resource_destroy(struct wl_resource *resource) {
    frame_finish(frame_from(resource));
}

static void frame_destroy(struct wl_client *client, struct wl_resource *resource) {
    wl_resource_destroy(resource);
}

static void frame_request(struct wl_resource *resource, struct wl_resource *buffer_resource,
        bool damage) {
    struct copy_frame *frame = frame_from(resource);
    if (!frame) return;
    if (frame->buffer) {
        wl_resource_post_error(resource, ZWLR_SCREENCOPY_FRAME_V1_ERROR_ALREADY_USED,
            "frame already used");
        return;
    }
    struct wlr_buffer *buffer = wlr_buffer_try_from_resource(buffer_resource);
    struct wlr_dmabuf_attributes dmabuf;
    void *data;
    uint32_t format = DRM_FORMAT_INVALID;
    size_t stride = 0;
    bool valid = buffer && buffer->width == frame->box.width &&
        buffer->height == frame->box.height;
    if (valid && wlr_buffer_get_dmabuf(buffer, &dmabuf)) {
        valid = dmabuf.format == frame->dmabuf_format;
    } else if (valid && wlr_buffer_begin_data_ptr_access(buffer,
            WLR_BUFFER_DATA_PTR_ACCESS_WRITE, &data, &format, &stride)) {
        wlr_buffer_end_data_ptr_access(buffer);
        valid = format == frame->shm_format && stride == (size_t)frame->box.width * 4;
    } else {
        valid = false;
    }
    if (!valid) {
        wlr_buffer_unlock(buffer);
        wl_resource_post_error(resource, ZWLR_SCREENCOPY_FRAME_V1_ERROR_INVALID_BUFFER,
            "invalid buffer");
        return;
    }
    if (!frame->output->enabled) {
        wlr_buffer_unlock(buffer);
        frame_fail(frame);
        return;
    }
    frame->buffer = buffer;
    frame->damage = damage;
    if (frame->cursor) {
        wlr_output_lock_software_cursors(frame->output, true);
        frame->cursor_locked = true;
    }
    if (!damage) wlr_output_schedule_frame(frame->output);
}

static void frame_copy(struct wl_client *client, struct wl_resource *resource,
        struct wl_resource *buffer) {
    frame_request(resource, buffer, false);
}

static void frame_copy_with_damage(struct wl_client *client, struct wl_resource *resource,
        struct wl_resource *buffer) {
    frame_request(resource, buffer, true);
}

static const struct zwlr_screencopy_frame_v1_interface frame_impl = {
    .copy = frame_copy,
    .destroy = frame_destroy,
    .copy_with_damage = frame_copy_with_damage,
};

static void capture(struct wl_client *client, struct wl_resource *manager, uint32_t id,
        int32_t cursor, struct wl_resource *output_resource, const struct wlr_box *region) {
    struct tomoe *s = wl_resource_get_user_data(manager);
    struct wl_resource *resource = wl_resource_create(client, &zwlr_screencopy_frame_v1_interface,
        wl_resource_get_version(manager), id);
    if (!resource) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(resource, &frame_impl, NULL, frame_resource_destroy);
    struct wlr_output *output = wlr_output_from_resource(output_resource);
    struct output *o = NULL, *candidate;
    wl_list_for_each(candidate, &s->outputs, link)
        if (candidate->wlr == output && output_is_active(candidate)) o = candidate;
    struct wlr_box box = { 0, 0, output ? output->width : 0, output ? output->height : 0 };
    if (o && region) {
        int width, height;
        wlr_output_effective_resolution(output, &width, &height);
        wlr_box_transform(&box, region, wlr_output_transform_invert(output->transform),
            width, height);
        box = (struct wlr_box){ pixel_round(box.x * output->scale),
            pixel_round(box.y * output->scale), pixel_round(box.width * output->scale),
            pixel_round(box.height * output->scale) };
        struct wlr_box bounds = { 0, 0, output->width, output->height };
        if (!wlr_box_intersection(&box, &box, &bounds)) o = NULL;
    }
    struct copy_frame *frame = o ? calloc(1, sizeof(*frame)) : NULL;
    if (!frame) {
        zwlr_screencopy_frame_v1_send_failed(resource);
        return;
    }
    frame->resource = resource;
    frame->output = output;
    frame->box = box;
    frame->cursor = cursor != 0;
    frame->shm_format = render_read_format(s->renderer);
    frame->dmabuf_format = o->ring.format.len ? o->ring.format.formats[0].format :
        DRM_FORMAT_XRGB8888;
    wl_resource_set_user_data(resource, frame);
    wl_list_insert(&s->copy_frames, &frame->link);
    zwlr_screencopy_frame_v1_send_buffer(resource,
        frame->shm_format == DRM_FORMAT_XRGB8888 ? WL_SHM_FORMAT_XRGB8888 : frame->shm_format,
        box.width, box.height, box.width * 4);
    if (wl_resource_get_version(resource) >= 3) {
        zwlr_screencopy_frame_v1_send_linux_dmabuf(resource, frame->dmabuf_format,
            box.width, box.height);
        zwlr_screencopy_frame_v1_send_buffer_done(resource);
    }
}

static void capture_output(struct wl_client *client, struct wl_resource *manager, uint32_t id,
        int32_t cursor, struct wl_resource *output) {
    capture(client, manager, id, cursor, output, NULL);
}

static void capture_output_region(struct wl_client *client, struct wl_resource *manager,
        uint32_t id, int32_t cursor, struct wl_resource *output, int32_t x, int32_t y,
        int32_t width, int32_t height) {
    capture(client, manager, id, cursor, output, &(struct wlr_box){ x, y, width, height });
}

static void manager_destroy(struct wl_client *client, struct wl_resource *resource) {
    wl_resource_destroy(resource);
}

static const struct zwlr_screencopy_manager_v1_interface manager_impl = {
    .capture_output = capture_output,
    .capture_output_region = capture_output_region,
    .destroy = manager_destroy,
};

static void bind(struct wl_client *client, void *data, uint32_t version, uint32_t id) {
    struct wl_resource *resource = wl_resource_create(client,
        &zwlr_screencopy_manager_v1_interface, version, id);
    if (!resource) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(resource, &manager_impl, data, NULL);
}

static bool blit(struct tomoe *s, struct wlr_buffer *target, struct wlr_buffer *source,
        struct wlr_box box) {
    struct wlr_texture *texture = wlr_texture_from_buffer(s->renderer, source);
    if (!texture) return false;
    struct wlr_dmabuf_attributes dmabuf;
    void *data;
    uint32_t format;
    size_t stride;
    bool ok = false;
    if (wlr_buffer_get_dmabuf(target, &dmabuf)) {
        struct wlr_render_pass *pass = wlr_renderer_begin_buffer_pass(s->renderer, target, NULL);
        if (pass) {
            wlr_render_pass_add_texture(pass, &(struct wlr_render_texture_options){
                .texture = texture, .src_box = { box.x, box.y, box.width, box.height },
                .dst_box = { 0, 0, box.width, box.height },
                .blend_mode = WLR_RENDER_BLEND_MODE_NONE });
            ok = wlr_render_pass_submit(pass);
        }
    } else if (wlr_buffer_begin_data_ptr_access(target, WLR_BUFFER_DATA_PTR_ACCESS_WRITE,
            &data, &format, &stride)) {
        ok = wlr_texture_read_pixels(texture, &(struct wlr_texture_read_pixels_options){
            .data = data, .format = format, .stride = stride, .src_box = box });
        wlr_buffer_end_data_ptr_access(target);
    }
    wlr_texture_destroy(texture);
    return ok;
}

enum { SOURCE_OUTPUT, SOURCE_WINDOW };

struct source {
    int kind;
    uint64_t id;
};

struct session {
    struct wl_resource *resource;
    struct wl_list link;
    struct tomoe *server;
    struct source source;
    bool cursors;
    int width, height;
    uint32_t format;
    struct image_frame *frame;
};

struct image_frame {
    struct wl_resource *resource;
    struct session *session;
    struct wlr_buffer *buffer;
    bool capturing;
};

static struct output *source_output(struct tomoe *s, const struct source *source) {
    struct output *o;
    wl_list_for_each(o, &s->outputs, link)
        if (source->kind == SOURCE_OUTPUT && o->id == source->id && output_is_active(o)) return o;
    return NULL;
}

static bool source_size(struct tomoe *s, const struct source *source, int *width, int *height,
        uint32_t *format) {
    struct output *o = source_output(s, source);
    if (o) {
        *width = o->wlr->width;
        *height = o->wlr->height;
        *format = o->ring.format.len ? o->ring.format.formats[0].format : DRM_FORMAT_XRGB8888;
        return true;
    }
    *format = DRM_FORMAT_ARGB8888;
    return source->kind == SOURCE_WINDOW && window_capture_size(s, (uint32_t)source->id,
        width, height);
}

static void image_frame_fail(struct image_frame *frame, uint32_t reason) {
    ext_image_copy_capture_frame_v1_send_failed(frame->resource, reason);
    wlr_buffer_unlock(frame->buffer);
    frame->buffer = NULL;
    frame->capturing = false;
}

static void session_constraints(struct tomoe *s, struct session *session) {
    struct wl_resource *resource = session->resource;
    ext_image_copy_capture_session_v1_send_buffer_size(resource, session->width, session->height);
    uint32_t shm = session->format == DRM_FORMAT_ARGB8888 ?
        (render_read_format(s->renderer) == DRM_FORMAT_XRGB8888 ? DRM_FORMAT_ARGB8888 :
            DRM_FORMAT_ABGR8888) : render_read_format(s->renderer);
    ext_image_copy_capture_session_v1_send_shm_format(resource,
        shm == DRM_FORMAT_ARGB8888 ? WL_SHM_FORMAT_ARGB8888 :
        shm == DRM_FORMAT_XRGB8888 ? WL_SHM_FORMAT_XRGB8888 : shm);
    struct stat st;
    if (fstat(wlr_renderer_get_drm_fd(s->renderer), &st) == 0) {
        struct wl_array device;
        wl_array_init(&device);
        dev_t *slot = wl_array_add(&device, sizeof(st.st_rdev));
        if (slot) *slot = st.st_rdev;
        ext_image_copy_capture_session_v1_send_dmabuf_device(resource, &device);
        wl_array_release(&device);
        const struct wlr_drm_format *format = wlr_drm_format_set_get(
            render_formats(s->renderer), session->format);
        struct wl_array modifiers;
        wl_array_init(&modifiers);
        for (size_t i = 0; format && i < format->len; i++) {
            uint64_t *modifier = wl_array_add(&modifiers, sizeof(*modifier));
            if (modifier) *modifier = format->modifiers[i];
        }
        ext_image_copy_capture_session_v1_send_dmabuf_format(resource, session->format,
            &modifiers);
        wl_array_release(&modifiers);
    }
    ext_image_copy_capture_session_v1_send_done(resource);
}

static void session_stop(struct session *session) {
    if (session->frame && session->frame->capturing)
        image_frame_fail(session->frame, EXT_IMAGE_COPY_CAPTURE_FRAME_V1_FAILURE_REASON_STOPPED);
    ext_image_copy_capture_session_v1_send_stopped(session->resource);
    wl_list_remove(&session->link);
    wl_list_init(&session->link);
}

static void image_frame_resource_destroy(struct wl_resource *resource) {
    struct image_frame *frame = wl_resource_get_user_data(resource);
    if (frame->session) frame->session->frame = NULL;
    wlr_buffer_unlock(frame->buffer);
    free(frame);
}

static void image_frame_attach(struct wl_client *client, struct wl_resource *resource,
        struct wl_resource *buffer) {
    struct image_frame *frame = wl_resource_get_user_data(resource);
    if (frame->capturing) {
        wl_resource_post_error(resource, EXT_IMAGE_COPY_CAPTURE_FRAME_V1_ERROR_ALREADY_CAPTURED,
            "frame already captured");
        return;
    }
    wlr_buffer_unlock(frame->buffer);
    frame->buffer = wlr_buffer_try_from_resource(buffer);
}

static void image_frame_damage(struct wl_client *client, struct wl_resource *resource,
        int32_t x, int32_t y, int32_t width, int32_t height) {
    if (x < 0 || y < 0 || width <= 0 || height <= 0)
        wl_resource_post_error(resource, EXT_IMAGE_COPY_CAPTURE_FRAME_V1_ERROR_INVALID_BUFFER_DAMAGE,
            "invalid buffer damage");
}

static bool buffer_fits(struct tomoe *s, struct session *session, struct wlr_buffer *buffer) {
    struct wlr_dmabuf_attributes dmabuf;
    void *data;
    uint32_t format;
    size_t stride;
    if (buffer->width != session->width || buffer->height != session->height) return false;
    if (wlr_buffer_get_dmabuf(buffer, &dmabuf)) return dmabuf.format == session->format;
    if (!wlr_buffer_begin_data_ptr_access(buffer, WLR_BUFFER_DATA_PTR_ACCESS_WRITE,
            &data, &format, &stride)) return false;
    wlr_buffer_end_data_ptr_access(buffer);
    return stride == (size_t)buffer->width * 4;
}

static void image_frame_capture(struct wl_client *client, struct wl_resource *resource) {
    struct image_frame *frame = wl_resource_get_user_data(resource);
    if (frame->capturing) {
        wl_resource_post_error(resource, EXT_IMAGE_COPY_CAPTURE_FRAME_V1_ERROR_ALREADY_CAPTURED,
            "frame already captured");
        return;
    }
    if (!frame->buffer) {
        wl_resource_post_error(resource, EXT_IMAGE_COPY_CAPTURE_FRAME_V1_ERROR_NO_BUFFER,
            "no buffer attached");
        return;
    }
    frame->capturing = true;
    struct session *session = frame->session;
    if (!session || wl_list_empty(&session->link)) {
        image_frame_fail(frame, EXT_IMAGE_COPY_CAPTURE_FRAME_V1_FAILURE_REASON_STOPPED);
        return;
    }
    struct output *o = source_output(session->server, &session->source);
    if (o) wlr_output_schedule_frame(o->wlr);
    else schedule_scene(session->server);
}

static void image_frame_destroy(struct wl_client *client, struct wl_resource *resource) {
    wl_resource_destroy(resource);
}

static const struct ext_image_copy_capture_frame_v1_interface image_frame_impl = {
    .destroy = image_frame_destroy,
    .attach_buffer = image_frame_attach,
    .damage_buffer = image_frame_damage,
    .capture = image_frame_capture,
};

static void serve_session(struct tomoe *s, struct session *session, struct output *o,
        struct wlr_buffer *committed, bool scanout, const struct timespec *now) {
    struct image_frame *frame = session->frame;
    if (!frame || !frame->capturing) return;
    int width, height;
    uint32_t format;
    if (!source_size(s, &session->source, &width, &height, &format)) return;
    if (width != session->width || height != session->height || format != session->format) {
        session->width = width;
        session->height = height;
        session->format = format;
        session_constraints(s, session);
    }
    if (!buffer_fits(s, session, frame->buffer)) {
        image_frame_fail(frame, EXT_IMAGE_COPY_CAPTURE_FRAME_V1_FAILURE_REASON_BUFFER_CONSTRAINTS);
        return;
    }
    bool ok;
    enum wl_output_transform transform = WL_OUTPUT_TRANSFORM_NORMAL;
    struct wlr_box box = { 0, 0, width, height };
    if (session->source.kind == SOURCE_OUTPUT) {
        struct wlr_buffer *source = session->cursors || scanout ? committed : o->capture_buffer;
        if (!source) {
            wlr_output_schedule_frame(o->wlr);
            return;
        }
        transform = o->wlr->transform;
        ok = blit(s, frame->buffer, source, box);
    } else {
        struct wlr_dmabuf_attributes dmabuf;
        if (wlr_buffer_get_dmabuf(frame->buffer, &dmabuf)) {
            ok = render_window_buffer(s, (uint32_t)session->source.id, frame->buffer);
        } else {
            const struct wlr_drm_format *render = wlr_drm_format_set_get(
                render_formats(s->renderer), format);
            struct wlr_buffer *scratch = render ?
                wlr_allocator_create_buffer(s->allocator, width, height, render) : NULL;
            ok = scratch && render_window_buffer(s, (uint32_t)session->source.id, scratch) &&
                blit(s, frame->buffer, scratch, box);
            if (scratch) wlr_buffer_drop(scratch);
        }
    }
    if (!ok) {
        image_frame_fail(frame, EXT_IMAGE_COPY_CAPTURE_FRAME_V1_FAILURE_REASON_UNKNOWN);
        return;
    }
    ext_image_copy_capture_frame_v1_send_transform(frame->resource, transform);
    ext_image_copy_capture_frame_v1_send_damage(frame->resource, 0, 0, width, height);
    ext_image_copy_capture_frame_v1_send_presentation_time(frame->resource,
        (uint32_t)((uint64_t)now->tv_sec >> 32), (uint32_t)now->tv_sec, (uint32_t)now->tv_nsec);
    ext_image_copy_capture_frame_v1_send_ready(frame->resource);
    wlr_buffer_unlock(frame->buffer);
    frame->buffer = NULL;
    frame->capturing = false;
}

static void session_resource_destroy(struct wl_resource *resource) {
    struct session *session = wl_resource_get_user_data(resource);
    if (!session) return;
    if (session->frame) session->frame->session = NULL;
    wl_list_remove(&session->link);
    free(session);
}

static void session_create_frame(struct wl_client *client, struct wl_resource *resource,
        uint32_t id) {
    struct session *session = wl_resource_get_user_data(resource);
    if (session && session->frame) {
        wl_resource_post_error(resource, EXT_IMAGE_COPY_CAPTURE_SESSION_V1_ERROR_DUPLICATE_FRAME,
            "session already has a frame");
        return;
    }
    struct wl_resource *frame_resource = wl_resource_create(client,
        &ext_image_copy_capture_frame_v1_interface, 1, id);
    struct image_frame *frame = frame_resource ? calloc(1, sizeof(*frame)) : NULL;
    if (!frame) {
        if (frame_resource) wl_resource_destroy(frame_resource);
        wl_client_post_no_memory(client);
        return;
    }
    frame->resource = frame_resource;
    frame->session = session;
    if (session) session->frame = frame;
    wl_resource_set_implementation(frame_resource, &image_frame_impl, frame,
        image_frame_resource_destroy);
}

static void session_destroy(struct wl_client *client, struct wl_resource *resource) {
    wl_resource_destroy(resource);
}

static const struct ext_image_copy_capture_session_v1_interface session_impl = {
    .create_frame = session_create_frame,
    .destroy = session_destroy,
};

static struct wl_resource *session_resource(struct wl_client *client, uint32_t id) {
    struct wl_resource *resource = wl_resource_create(client,
        &ext_image_copy_capture_session_v1_interface, 1, id);
    if (!resource) {
        wl_client_post_no_memory(client);
        return NULL;
    }
    wl_resource_set_implementation(resource, &session_impl, NULL, session_resource_destroy);
    return resource;
}

static void create_session(struct wl_client *client, struct wl_resource *manager, uint32_t id,
        struct wl_resource *source_resource, uint32_t options) {
    struct tomoe *s = wl_resource_get_user_data(manager);
    if (options & ~EXT_IMAGE_COPY_CAPTURE_MANAGER_V1_OPTIONS_PAINT_CURSORS) {
        wl_resource_post_error(manager, EXT_IMAGE_COPY_CAPTURE_MANAGER_V1_ERROR_INVALID_OPTION,
            "invalid option");
        return;
    }
    struct wl_resource *resource = session_resource(client, id);
    if (!resource) return;
    struct source *source = wl_resource_get_user_data(source_resource);
    struct session *session = source ? calloc(1, sizeof(*session)) : NULL;
    if (!session || !source_size(s, source, &session->width, &session->height,
            &session->format)) {
        free(session);
        ext_image_copy_capture_session_v1_send_stopped(resource);
        return;
    }
    session->resource = resource;
    session->server = s;
    session->source = *source;
    session->cursors = options & EXT_IMAGE_COPY_CAPTURE_MANAGER_V1_OPTIONS_PAINT_CURSORS;
    wl_resource_set_user_data(resource, session);
    wl_list_insert(&s->capture_sessions, &session->link);
    session_constraints(s, session);
}

static void cursor_session_capture(struct wl_client *client, struct wl_resource *resource,
        uint32_t id) {
    struct wl_resource *session = session_resource(client, id);
    if (session) ext_image_copy_capture_session_v1_send_stopped(session);
}

static void cursor_session_destroy(struct wl_client *client, struct wl_resource *resource) {
    wl_resource_destroy(resource);
}

static const struct ext_image_copy_capture_cursor_session_v1_interface cursor_session_impl = {
    .destroy = cursor_session_destroy,
    .get_capture_session = cursor_session_capture,
};

static void create_cursor_session(struct wl_client *client, struct wl_resource *manager,
        uint32_t id, struct wl_resource *source, struct wl_resource *pointer) {
    struct wl_resource *resource = wl_resource_create(client,
        &ext_image_copy_capture_cursor_session_v1_interface, 1, id);
    if (!resource) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(resource, &cursor_session_impl, NULL, NULL);
}

static void resource_destroy(struct wl_client *client, struct wl_resource *resource) {
    wl_resource_destroy(resource);
}

static const struct ext_image_copy_capture_manager_v1_interface copy_manager_impl = {
    .create_session = create_session,
    .create_pointer_cursor_session = create_cursor_session,
    .destroy = resource_destroy,
};

static const struct ext_image_capture_source_v1_interface source_impl = {
    .destroy = resource_destroy,
};

static void source_resource_destroy(struct wl_resource *resource) {
    free(wl_resource_get_user_data(resource));
}

static void create_source(struct wl_client *client, struct wl_resource *manager, uint32_t id,
        int kind, uint64_t source_id) {
    struct wl_resource *resource = wl_resource_create(client, &ext_image_capture_source_v1_interface,
        1, id);
    struct source *source = resource && source_id ? calloc(1, sizeof(*source)) : NULL;
    if (!resource) {
        wl_client_post_no_memory(client);
        return;
    }
    if (source) *source = (struct source){ kind, source_id };
    wl_resource_set_implementation(resource, &source_impl, source, source_resource_destroy);
}

static void create_output_source(struct wl_client *client, struct wl_resource *manager,
        uint32_t id, struct wl_resource *output_resource) {
    struct tomoe *s = wl_resource_get_user_data(manager);
    struct wlr_output *output = wlr_output_from_resource(output_resource);
    uint64_t source_id = 0;
    struct output *o;
    wl_list_for_each(o, &s->outputs, link) if (o->wlr == output) source_id = o->id;
    create_source(client, manager, id, SOURCE_OUTPUT, source_id);
}

static const struct ext_output_image_capture_source_manager_v1_interface output_sources_impl = {
    .create_source = create_output_source,
    .destroy = resource_destroy,
};

static void create_window_source(struct wl_client *client, struct wl_resource *manager,
        uint32_t id, struct wl_resource *handle_resource) {
    create_source(client, manager, id, SOURCE_WINDOW, foreign_handle_window(handle_resource));
}

static const struct ext_foreign_toplevel_image_capture_source_manager_v1_interface
        window_sources_impl = {
    .create_source = create_window_source,
    .destroy = resource_destroy,
};

static void bind_global(struct wl_client *client, void *data, uint32_t version, uint32_t id,
        const struct wl_interface *interface, const void *impl) {
    struct wl_resource *resource = wl_resource_create(client, interface, version, id);
    if (!resource) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(resource, impl, data, NULL);
}

static void bind_copy(struct wl_client *client, void *data, uint32_t version, uint32_t id) {
    bind_global(client, data, version, id, &ext_image_copy_capture_manager_v1_interface,
        &copy_manager_impl);
}

static void bind_output_sources(struct wl_client *client, void *data, uint32_t version,
        uint32_t id) {
    bind_global(client, data, version, id, &ext_output_image_capture_source_manager_v1_interface,
        &output_sources_impl);
}

static void bind_window_sources(struct wl_client *client, void *data, uint32_t version,
        uint32_t id) {
    bind_global(client, data, version, id,
        &ext_foreign_toplevel_image_capture_source_manager_v1_interface, &window_sources_impl);
}

bool capture_listen(struct tomoe *s) {
    return wl_global_create(s->display, &zwlr_screencopy_manager_v1_interface, 3, s, bind) &&
        wl_global_create(s->display, &ext_image_copy_capture_manager_v1_interface, 1, s,
            bind_copy) &&
        wl_global_create(s->display, &ext_output_image_capture_source_manager_v1_interface, 1, s,
            bind_output_sources) &&
        wl_global_create(s->display, &ext_foreign_toplevel_image_capture_source_manager_v1_interface,
            1, s, bind_window_sources);
}

bool capture_wants_cursorless(struct output *o) {
    struct copy_frame *frame;
    wl_list_for_each(frame, &o->server->copy_frames, link)
        if (frame->output == o->wlr && frame->buffer && !frame->cursor) return true;
    struct session *session;
    wl_list_for_each(session, &o->server->capture_sessions, link)
        if (session->source.kind == SOURCE_OUTPUT && session->source.id == o->id &&
                session->frame && session->frame->capturing && !session->cursors) return true;
    return false;
}

void capture_serve(struct output *o, struct wlr_buffer *committed, bool scanout) {
    struct tomoe *s = o->server;
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    struct session *session, *next_session;
    wl_list_for_each_safe(session, next_session, &s->capture_sessions, link)
        if (session->source.kind == SOURCE_WINDOW ||
                (session->source.kind == SOURCE_OUTPUT && session->source.id == o->id))
            serve_session(s, session, o, committed, scanout, &now);
    struct copy_frame *frame, *next;
    wl_list_for_each_safe(frame, next, &s->copy_frames, link) {
        if (frame->output != o->wlr || !frame->buffer) continue;
        struct wlr_buffer *source = frame->cursor || scanout ? committed : o->capture_buffer;
        if (!source) {
            wlr_output_schedule_frame(o->wlr);
            continue;
        }
        if (!blit(s, frame->buffer, source, frame->box)) {
            frame_fail(frame);
            continue;
        }
        zwlr_screencopy_frame_v1_send_flags(frame->resource, 0);
        if (frame->damage)
            zwlr_screencopy_frame_v1_send_damage(frame->resource, 0, 0,
                frame->box.width, frame->box.height);
        zwlr_screencopy_frame_v1_send_ready(frame->resource, (uint32_t)((uint64_t)now.tv_sec >> 32),
            (uint32_t)now.tv_sec, (uint32_t)now.tv_nsec);
        frame_finish(frame);
    }
}

void capture_output_gone(struct tomoe *s, struct output *o) {
    struct copy_frame *frame, *next;
    wl_list_for_each_safe(frame, next, &s->copy_frames, link)
        if (frame->output == o->wlr) frame_fail(frame);
    struct session *session, *next_session;
    wl_list_for_each_safe(session, next_session, &s->capture_sessions, link)
        if (session->source.kind == SOURCE_OUTPUT && session->source.id == o->id)
            session_stop(session);
}

void capture_window_gone(struct tomoe *s, uint32_t id) {
    struct session *session, *next;
    wl_list_for_each_safe(session, next, &s->capture_sessions, link)
        if (session->source.kind == SOURCE_WINDOW && session->source.id == id)
            session_stop(session);
}
