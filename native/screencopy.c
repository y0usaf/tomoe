#include "internal.h"
#include <drm_fourcc.h>
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

bool screencopy_listen(struct tomoe *s) {
    wl_list_init(&s->copy_frames);
    return wl_global_create(s->display, &zwlr_screencopy_manager_v1_interface, 3, s, bind);
}

bool screencopy_wants_cursorless(struct output *o) {
    struct copy_frame *frame;
    wl_list_for_each(frame, &o->server->copy_frames, link)
        if (frame->output == o->wlr && frame->buffer && !frame->cursor) return true;
    return false;
}

static bool blit(struct tomoe *s, struct copy_frame *frame, struct wlr_buffer *source) {
    struct wlr_texture *texture = wlr_texture_from_buffer(s->renderer, source);
    if (!texture) return false;
    struct wlr_dmabuf_attributes dmabuf;
    void *data;
    uint32_t format;
    size_t stride;
    bool ok = false;
    if (wlr_buffer_get_dmabuf(frame->buffer, &dmabuf)) {
        struct wlr_render_pass *pass = wlr_renderer_begin_buffer_pass(s->renderer,
            frame->buffer, NULL);
        if (pass) {
            wlr_render_pass_add_texture(pass, &(struct wlr_render_texture_options){
                .texture = texture,
                .src_box = { frame->box.x, frame->box.y, frame->box.width, frame->box.height },
                .dst_box = { 0, 0, frame->box.width, frame->box.height },
                .blend_mode = WLR_RENDER_BLEND_MODE_NONE });
            ok = wlr_render_pass_submit(pass);
        }
    } else if (wlr_buffer_begin_data_ptr_access(frame->buffer, WLR_BUFFER_DATA_PTR_ACCESS_WRITE,
            &data, &format, &stride)) {
        ok = wlr_texture_read_pixels(texture, &(struct wlr_texture_read_pixels_options){
            .data = data, .format = format, .stride = stride, .src_box = frame->box });
        wlr_buffer_end_data_ptr_access(frame->buffer);
    }
    wlr_texture_destroy(texture);
    return ok;
}

void screencopy_serve(struct output *o, struct wlr_buffer *committed, bool scanout) {
    struct tomoe *s = o->server;
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    struct copy_frame *frame, *next;
    wl_list_for_each_safe(frame, next, &s->copy_frames, link) {
        if (frame->output != o->wlr || !frame->buffer) continue;
        struct wlr_buffer *source = frame->cursor || scanout ? committed : o->capture_buffer;
        if (!source) {
            wlr_output_schedule_frame(o->wlr);
            continue;
        }
        if (!blit(s, frame, source)) {
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

void screencopy_output_gone(struct tomoe *s, struct wlr_output *output) {
    struct copy_frame *frame, *next;
    wl_list_for_each_safe(frame, next, &s->copy_frames, link)
        if (frame->output == output) frame_fail(frame);
}
