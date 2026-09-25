#include "internal.h"
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <drm_fourcc.h>
#include <xf86drm.h>
#include "linux-dmabuf-v1-protocol.h"

struct shm_buffer {
    struct buffer base;
    struct wl_resource *resource;
    struct wl_shm_buffer *shm;
    struct wl_shm_pool *pool;
    void *data;
    uint32_t format;
    size_t stride;
    struct wl_listener resource_destroy, release;
};

struct dmabuf_buffer {
    struct buffer base;
    struct wl_resource *resource;
    struct dmabuf_attributes attributes;
    struct wl_listener release;
};

struct dmabuf_params {
    struct dmabuf_attributes attributes;
    struct tomoe *server;
    bool has_modifier;
};

struct table_entry {
    uint32_t format, pad;
    uint64_t modifier;
};

static struct {
    int drm_fd, table_fd;
    size_t table_size, count;
    dev_t device;
} dmabuf = { -1, -1, 0, 0, 0 };

static uint32_t shm_to_drm(uint32_t format) {
    return format == WL_SHM_FORMAT_ARGB8888 ? DRM_FORMAT_ARGB8888 :
        format == WL_SHM_FORMAT_XRGB8888 ? DRM_FORMAT_XRGB8888 : format;
}

static void shm_buffer_destroy(struct buffer *base) {
    struct shm_buffer *b = wl_container_of(base, b, base);
    detach(&b->release);
    detach(&b->resource_destroy);
    buffer_finish(base);
    wl_shm_pool_unref(b->pool);
    free(b);
}

static bool shm_buffer_begin(struct buffer *base, uint32_t flags, void **data,
        uint32_t *format, size_t *stride) {
    struct shm_buffer *b = wl_container_of(base, b, base);
    if (b->shm) wl_shm_buffer_begin_access(b->shm);
    *data = b->data;
    *format = b->format;
    *stride = b->stride;
    return true;
}

static void shm_buffer_end(struct buffer *base) {
    struct shm_buffer *b = wl_container_of(base, b, base);
    if (b->shm) wl_shm_buffer_end_access(b->shm);
}

static const struct buffer_impl shm_buffer_impl = {
    .destroy = shm_buffer_destroy,
    .begin_access = shm_buffer_begin,
    .end_access = shm_buffer_end,
};

static void shm_resource_destroyed(struct wl_listener *listener, void *data) {
    struct shm_buffer *b = wl_container_of(listener, b, resource_destroy);
    detach(&b->resource_destroy);
    b->resource = NULL;
    b->shm = NULL;
    buffer_drop(&b->base);
}

static void shm_released(struct wl_listener *listener, void *data) {
    struct shm_buffer *b = wl_container_of(listener, b, release);
    if (b->resource) wl_buffer_send_release(b->resource);
}

static bool shm_is_instance(struct wl_resource *resource) {
    return wl_shm_buffer_get(resource) != NULL;
}

static struct buffer *shm_from_resource(struct wl_resource *resource) {
    struct wl_listener *existing =
        wl_resource_get_destroy_listener(resource, shm_resource_destroyed);
    if (existing) {
        struct shm_buffer *b = wl_container_of(existing, b, resource_destroy);
        return &b->base;
    }
    struct wl_shm_buffer *shm = wl_shm_buffer_get(resource);
    struct shm_buffer *b = calloc(1, sizeof(*b));
    if (!b) return NULL;
    buffer_init(&b->base, &shm_buffer_impl, wl_shm_buffer_get_width(shm),
        wl_shm_buffer_get_height(shm));
    b->resource = resource;
    b->shm = shm;
    b->pool = wl_shm_buffer_ref_pool(shm);
    b->data = wl_shm_buffer_get_data(shm);
    b->format = shm_to_drm(wl_shm_buffer_get_format(shm));
    b->stride = (size_t)wl_shm_buffer_get_stride(shm);
    b->resource_destroy.notify = shm_resource_destroyed;
    wl_resource_add_destroy_listener(resource, &b->resource_destroy);
    listen(&b->release, &b->base.events.release, shm_released);
    return &b->base;
}

static void destroy_resource(struct wl_client *client, struct wl_resource *resource) {
    wl_resource_destroy(resource);
}

static const struct wl_buffer_interface dmabuf_wl_buffer_impl = {
    .destroy = destroy_resource,
};

static void dmabuf_buffer_destroy(struct buffer *base) {
    struct dmabuf_buffer *b = wl_container_of(base, b, base);
    detach(&b->release);
    buffer_finish(base);
    if (b->resource) wl_resource_set_user_data(b->resource, NULL);
    dmabuf_attributes_finish(&b->attributes);
    free(b);
}

static bool dmabuf_buffer_get(struct buffer *base, struct dmabuf_attributes *attributes) {
    struct dmabuf_buffer *b = wl_container_of(base, b, base);
    *attributes = b->attributes;
    return true;
}

static const struct buffer_impl dmabuf_buffer_impl = {
    .destroy = dmabuf_buffer_destroy,
    .get_dmabuf = dmabuf_buffer_get,
};

static bool dmabuf_is_instance(struct wl_resource *resource) {
    return wl_resource_instance_of(resource, &wl_buffer_interface, &dmabuf_wl_buffer_impl) &&
        wl_resource_get_user_data(resource);
}

static struct buffer *dmabuf_from_resource(struct wl_resource *resource) {
    struct dmabuf_buffer *b = wl_resource_get_user_data(resource);
    return &b->base;
}

static void dmabuf_resource_destroyed(struct wl_resource *resource) {
    struct dmabuf_buffer *b = wl_resource_get_user_data(resource);
    if (!b) return;
    b->resource = NULL;
    buffer_drop(&b->base);
}

static void dmabuf_released(struct wl_listener *listener, void *data) {
    struct dmabuf_buffer *b = wl_container_of(listener, b, release);
    if (b->resource) wl_buffer_send_release(b->resource);
}

static void params_free(struct wl_resource *resource) {
    struct dmabuf_params *p = wl_resource_get_user_data(resource);
    if (!p) return;
    dmabuf_attributes_finish(&p->attributes);
    free(p);
}

static void params_add(struct wl_client *client, struct wl_resource *resource, int32_t fd,
        uint32_t plane, uint32_t offset, uint32_t stride, uint32_t modifier_hi,
        uint32_t modifier_lo) {
    struct dmabuf_params *p = wl_resource_get_user_data(resource);
    uint64_t modifier = ((uint64_t)modifier_hi << 32) | modifier_lo;
    const char *error = !p ? "params already used" : plane >= DMABUF_MAX_PLANES ?
        "plane index out of range" : p->attributes.fd[plane] != -1 ? "plane already set" :
        p->has_modifier && modifier != p->attributes.modifier ? "modifier differs between planes" :
        NULL;
    if (error) {
        uint32_t code = !p ? ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_ALREADY_USED :
            plane >= DMABUF_MAX_PLANES ? ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_PLANE_IDX :
            p->attributes.fd[plane] != -1 ? ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_PLANE_SET :
            ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_INVALID_FORMAT;
        wl_resource_post_error(resource, code, "%s", error);
        close(fd);
        return;
    }
    p->attributes.modifier = modifier;
    p->has_modifier = true;
    p->attributes.fd[plane] = fd;
    p->attributes.offset[plane] = offset;
    p->attributes.stride[plane] = stride;
    p->attributes.n_planes++;
}

static const char *params_invalid(struct dmabuf_attributes *a, uint32_t *code) {
    *code = ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_INCOMPLETE;
    if (!a->n_planes || a->fd[0] == -1) return "no dmabuf for plane 0";
    for (int i = 1; i < a->n_planes; i++) if (a->fd[i] == -1) return "gap in dmabuf planes";
    *code = ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_INVALID_DIMENSIONS;
    if (a->width < 1 || a->height < 1) return "invalid dimensions";
    *code = ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_OUT_OF_BOUNDS;
    for (int i = 0; i < a->n_planes; i++) {
        if ((uint64_t)a->offset[i] + (uint64_t)a->stride[i] * (uint64_t)a->height > UINT32_MAX)
            return "plane size overflow";
        off_t size = lseek(a->fd[i], 0, SEEK_END);
        if (size == -1) continue;
        if (!a->stride[i] || (off_t)a->offset[i] + a->stride[i] > size ||
                (i == 0 && (off_t)a->offset[i] + (off_t)a->stride[i] * a->height > size))
            return "plane out of bounds";
    }
    return NULL;
}

static bool importable(const struct dmabuf_attributes *a) {
    for (int i = 0; dmabuf.drm_fd >= 0 && i < a->n_planes; i++) {
        uint32_t handle = 0;
        if (drmPrimeFDToHandle(dmabuf.drm_fd, a->fd[i], &handle) != 0) return false;
        drmCloseBufferHandle(dmabuf.drm_fd, handle);
    }
    return true;
}

static void params_create_common(struct wl_resource *resource, uint32_t id, int32_t width,
        int32_t height, uint32_t format, uint32_t flags) {
    struct dmabuf_params *p = wl_resource_get_user_data(resource);
    if (!p) {
        wl_resource_post_error(resource, ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_ALREADY_USED,
            "params already used");
        return;
    }
    struct dmabuf_attributes attributes = p->attributes;
    wl_resource_set_user_data(resource, NULL);
    free(p);
    attributes.width = width;
    attributes.height = height;
    attributes.format = format;
    uint32_t code;
    const char *invalid = flags ? NULL : params_invalid(&attributes, &code);
    if (invalid) {
        wl_resource_post_error(resource, code, "%s", invalid);
        dmabuf_attributes_finish(&attributes);
        return;
    }
    struct dmabuf_buffer *b = !flags && importable(&attributes) ? calloc(1, sizeof(*b)) : NULL;
    struct wl_resource *buffer = b ? wl_resource_create(wl_resource_get_client(resource),
        &wl_buffer_interface, 1, id) : NULL;
    if (!buffer) {
        free(b);
        dmabuf_attributes_finish(&attributes);
        if (!id) zwp_linux_buffer_params_v1_send_failed(resource);
        else wl_resource_post_error(resource, ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_INVALID_WL_BUFFER,
            "dmabuf import failed");
        return;
    }
    buffer_init(&b->base, &dmabuf_buffer_impl, width, height);
    b->resource = buffer;
    b->attributes = attributes;
    wl_resource_set_implementation(buffer, &dmabuf_wl_buffer_impl, b, dmabuf_resource_destroyed);
    listen(&b->release, &b->base.events.release, dmabuf_released);
    if (!id) zwp_linux_buffer_params_v1_send_created(resource, buffer);
}

static void params_create(struct wl_client *client, struct wl_resource *resource, int32_t width,
        int32_t height, uint32_t format, uint32_t flags) {
    params_create_common(resource, 0, width, height, format, flags);
}

static void params_create_immed(struct wl_client *client, struct wl_resource *resource,
        uint32_t id, int32_t width, int32_t height, uint32_t format, uint32_t flags) {
    params_create_common(resource, id, width, height, format, flags);
}

static const struct zwp_linux_buffer_params_v1_interface params_impl = {
    .destroy = destroy_resource,
    .add = params_add,
    .create = params_create,
    .create_immed = params_create_immed,
};

static void create_params(struct wl_client *client, struct wl_resource *manager, uint32_t id) {
    struct dmabuf_params *p = calloc(1, sizeof(*p));
    struct wl_resource *resource = p ? wl_resource_create(client,
        &zwp_linux_buffer_params_v1_interface, wl_resource_get_version(manager), id) : NULL;
    if (!resource) {
        free(p);
        wl_client_post_no_memory(client);
        return;
    }
    for (int i = 0; i < DMABUF_MAX_PLANES; i++) p->attributes.fd[i] = -1;
    wl_resource_set_implementation(resource, &params_impl, p, params_free);
}

static const struct zwp_linux_dmabuf_feedback_v1_interface feedback_impl = {
    .destroy = destroy_resource,
};

static void send_feedback(struct wl_client *client, struct wl_resource *manager, uint32_t id) {
    struct wl_resource *resource = wl_resource_create(client,
        &zwp_linux_dmabuf_feedback_v1_interface, wl_resource_get_version(manager), id);
    if (!resource) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(resource, &feedback_impl, NULL, NULL);
    struct wl_array device = { .size = sizeof(dmabuf.device), .data = &dmabuf.device };
    zwp_linux_dmabuf_feedback_v1_send_format_table(resource, dmabuf.table_fd,
        (uint32_t)dmabuf.table_size);
    zwp_linux_dmabuf_feedback_v1_send_main_device(resource, &device);
    zwp_linux_dmabuf_feedback_v1_send_tranche_target_device(resource, &device);
    struct wl_array indices;
    wl_array_init(&indices);
    for (size_t i = 0; i < dmabuf.count; i++) {
        uint16_t *index = wl_array_add(&indices, sizeof(*index));
        if (index) *index = (uint16_t)i;
    }
    zwp_linux_dmabuf_feedback_v1_send_tranche_formats(resource, &indices);
    wl_array_release(&indices);
    zwp_linux_dmabuf_feedback_v1_send_tranche_flags(resource, 0);
    zwp_linux_dmabuf_feedback_v1_send_tranche_done(resource);
    zwp_linux_dmabuf_feedback_v1_send_done(resource);
}

static void get_default_feedback(struct wl_client *client, struct wl_resource *manager,
        uint32_t id) {
    send_feedback(client, manager, id);
}

static void get_surface_feedback(struct wl_client *client, struct wl_resource *manager,
        uint32_t id, struct wl_resource *surface) {
    send_feedback(client, manager, id);
}

static const struct zwp_linux_dmabuf_v1_interface dmabuf_impl = {
    .destroy = destroy_resource,
    .create_params = create_params,
    .get_default_feedback = get_default_feedback,
    .get_surface_feedback = get_surface_feedback,
};

static void bind_dmabuf(struct wl_client *client, void *data, uint32_t version, uint32_t id) {
    struct tomoe *s = data;
    struct wl_resource *resource = wl_resource_create(client, &zwp_linux_dmabuf_v1_interface,
        version, id);
    if (!resource) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(resource, &dmabuf_impl, s, NULL);
    if (version >= ZWP_LINUX_DMABUF_V1_GET_DEFAULT_FEEDBACK_SINCE_VERSION) return;
    const struct format_set *formats =
        render_texture_formats(s->renderer);
    for (size_t i = 0; i < formats->len; i++) {
        const struct format *f = &formats->formats[i];
        if (version < ZWP_LINUX_DMABUF_V1_MODIFIER_SINCE_VERSION) {
            zwp_linux_dmabuf_v1_send_format(resource, f->format);
            continue;
        }
        for (size_t j = 0; j < f->len; j++)
            zwp_linux_dmabuf_v1_send_modifier(resource, f->format,
                (uint32_t)(f->modifiers[j] >> 32), (uint32_t)f->modifiers[j]);
    }
}

static bool dmabuf_table(const struct format_set *formats) {
    size_t count = 0;
    for (size_t i = 0; i < formats->len; i++) count += formats->formats[i].len;
    if (!count || count > UINT16_MAX) return false;
    struct table_entry *table = calloc(count, sizeof(*table));
    if (!table) return false;
    size_t n = 0;
    for (size_t i = 0; i < formats->len; i++)
        for (size_t j = 0; j < formats->formats[i].len; j++)
            table[n++] = (struct table_entry){ .format = formats->formats[i].format,
                .modifier = formats->formats[i].modifiers[j] };
    size_t size = count * sizeof(*table);
    int fd = memfd_create("tomoe-dmabuf-formats", MFD_CLOEXEC | MFD_ALLOW_SEALING);
    bool ok = fd >= 0 && write(fd, table, size) == (ssize_t)size &&
        fcntl(fd, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_WRITE | F_SEAL_SEAL) == 0;
    free(table);
    if (!ok) {
        if (fd >= 0) close(fd);
        return false;
    }
    dmabuf.table_fd = fd;
    dmabuf.table_size = size;
    dmabuf.count = count;
    return true;
}

bool buffers_listen(struct tomoe *s) {
    if (wl_display_init_shm(s->display) != 0) return false;
    const struct format_set *shm =
        render_shm_formats(s->renderer);
    for (size_t i = 0; shm && i < shm->len; i++) {
        uint32_t format = shm->formats[i].format;
        if (format != DRM_FORMAT_ARGB8888 && format != DRM_FORMAT_XRGB8888)
            wl_display_add_shm_format(s->display, format);
    }
    const struct format_set *formats =
        render_texture_formats(s->renderer);
    int fd = render_drm_fd(s->renderer);
    struct stat st;
    if (!formats || !formats->len || fd < 0 || fstat(fd, &st) != 0) return true;
    dmabuf.drm_fd = fd;
    dmabuf.device = st.st_rdev;
    if (!dmabuf_table(formats)) return false;
    return wl_global_create(s->display, &zwp_linux_dmabuf_v1_interface, 4, s, bind_dmabuf);
}

void buffers_finish(void) {
    if (dmabuf.table_fd >= 0) close(dmabuf.table_fd);
    dmabuf.table_fd = dmabuf.drm_fd = -1;
}

struct pixel_buffer {
    struct buffer base;
    uint32_t format;
    size_t stride;
    uint8_t data[];
};

struct buffer *buffer_from_resource(struct wl_resource *resource) {
    struct buffer *buffer = shm_is_instance(resource) ? shm_from_resource(resource) :
        dmabuf_is_instance(resource) ? dmabuf_from_resource(resource) : NULL;
    if (!buffer) tomoe_log(LOG_ERROR, "tomoe: unknown buffer type");
    return buffer ? buffer_lock(buffer) : NULL;
}

static void pixel_buffer_destroy(struct buffer *base) {
    struct pixel_buffer *b = wl_container_of(base, b, base);
    buffer_finish(base);
    free(b);
}

static bool pixel_buffer_begin(struct buffer *base, uint32_t flags, void **data,
        uint32_t *format, size_t *stride) {
    struct pixel_buffer *b = wl_container_of(base, b, base);
    *data = b->data;
    *format = b->format;
    *stride = b->stride;
    return true;
}

static void pixel_buffer_end(struct buffer *base) {
}

static const struct buffer_impl pixel_buffer_impl = {
    .destroy = pixel_buffer_destroy,
    .begin_access = pixel_buffer_begin,
    .end_access = pixel_buffer_end,
};

struct buffer *pixel_buffer_create(int width, int height, size_t stride, uint32_t format,
        const void *pixels) {
    struct pixel_buffer *b = calloc(1, sizeof(*b) + stride * (size_t)height);
    if (!b) return NULL;
    buffer_init(&b->base, &pixel_buffer_impl, width, height);
    b->format = format;
    b->stride = stride;
    memcpy(b->data, pixels, stride * (size_t)height);
    return &b->base;
}
