#include "drm.h"
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>

int drm_ioctl(int fd, unsigned long request, void *arg) {
    int ret;
    do ret = ioctl(fd, request, arg);
    while (ret == -1 && errno == EINTR);
    return ret == -1 ? -errno : 0;
}

static uint64_t pointer_of(const void *pointer) {
    return (uint64_t)(uintptr_t)pointer;
}

struct drm_res *drm_res_get(int fd) {
    struct drm_mode_card_res res = {0};
    if (drm_ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res)) return NULL;
    struct drm_res *out = calloc(1, sizeof(*out));
    if (!out) return NULL;
    for (int tries = 0; tries < 4; tries++) {
        uint32_t crtcs = res.count_crtcs, connectors = res.count_connectors;
        uint32_t encoders = res.count_encoders;
        free(out->crtcs);
        free(out->connectors);
        free(out->encoders);
        out->crtcs = crtcs ? calloc(crtcs, sizeof(*out->crtcs)) : NULL;
        out->connectors = connectors ? calloc(connectors, sizeof(*out->connectors)) : NULL;
        out->encoders = encoders ? calloc(encoders, sizeof(*out->encoders)) : NULL;
        if ((crtcs && !out->crtcs) || (connectors && !out->connectors) ||
                (encoders && !out->encoders)) break;
        res.fb_id_ptr = 0;
        res.count_fbs = 0;
        res.crtc_id_ptr = pointer_of(out->crtcs);
        res.connector_id_ptr = pointer_of(out->connectors);
        res.encoder_id_ptr = pointer_of(out->encoders);
        res.count_crtcs = crtcs;
        res.count_connectors = connectors;
        res.count_encoders = encoders;
        if (drm_ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res)) break;
        if (res.count_crtcs <= crtcs && res.count_connectors <= connectors &&
                res.count_encoders <= encoders) {
            out->count_crtcs = (int)res.count_crtcs;
            out->count_connectors = (int)res.count_connectors;
            out->count_encoders = (int)res.count_encoders;
            return out;
        }
    }
    drm_res_free(out);
    return NULL;
}

void drm_res_free(struct drm_res *res) {
    if (!res) return;
    free(res->crtcs);
    free(res->connectors);
    free(res->encoders);
    free(res);
}

struct drm_plane_res *drm_plane_res_get(int fd) {
    struct drm_mode_get_plane_res res = {0};
    if (drm_ioctl(fd, DRM_IOCTL_MODE_GETPLANERESOURCES, &res)) return NULL;
    struct drm_plane_res *out = calloc(1, sizeof(*out));
    if (!out) return NULL;
    for (int tries = 0; tries < 4; tries++) {
        uint32_t planes = res.count_planes;
        free(out->planes);
        out->planes = planes ? calloc(planes, sizeof(*out->planes)) : NULL;
        if (planes && !out->planes) break;
        res.plane_id_ptr = pointer_of(out->planes);
        res.count_planes = planes;
        if (drm_ioctl(fd, DRM_IOCTL_MODE_GETPLANERESOURCES, &res)) break;
        if (res.count_planes <= planes) {
            out->count = (int)res.count_planes;
            return out;
        }
    }
    drm_plane_res_free(out);
    return NULL;
}

void drm_plane_res_free(struct drm_plane_res *res) {
    if (!res) return;
    free(res->planes);
    free(res);
}

struct drm_crtc *drm_crtc_get(int fd, uint32_t id) {
    struct drm_mode_crtc info = { .crtc_id = id };
    if (drm_ioctl(fd, DRM_IOCTL_MODE_GETCRTC, &info)) return NULL;
    struct drm_crtc *crtc = calloc(1, sizeof(*crtc));
    if (!crtc) return NULL;
    crtc->id = info.crtc_id;
    crtc->gamma_size = info.gamma_size;
    crtc->mode = info.mode;
    return crtc;
}

void drm_crtc_free(struct drm_crtc *crtc) {
    free(crtc);
}

struct drm_plane_info *drm_plane_get(int fd, uint32_t id) {
    struct drm_mode_get_plane info = { .plane_id = id };
    if (drm_ioctl(fd, DRM_IOCTL_MODE_GETPLANE, &info)) return NULL;
    struct drm_plane_info *plane = calloc(1, sizeof(*plane));
    if (!plane) return NULL;
    plane->id = info.plane_id;
    plane->possible_crtcs = info.possible_crtcs;
    for (int tries = 0; tries < 4; tries++) {
        uint32_t count = info.count_format_types;
        free(plane->formats);
        plane->formats = count ? calloc(count, sizeof(*plane->formats)) : NULL;
        if (count && !plane->formats) break;
        info.format_type_ptr = pointer_of(plane->formats);
        info.count_format_types = count;
        if (drm_ioctl(fd, DRM_IOCTL_MODE_GETPLANE, &info)) break;
        if (info.count_format_types <= count) {
            plane->count_formats = (int)info.count_format_types;
            return plane;
        }
    }
    drm_plane_free(plane);
    return NULL;
}

void drm_plane_free(struct drm_plane_info *plane) {
    if (!plane) return;
    free(plane->formats);
    free(plane);
}

struct drm_connector *drm_connector_get(int fd, uint32_t id, bool probe) {
    drm_mode_info scratch;
    struct drm_mode_get_connector counts = { .connector_id = id };
    if (!probe) {
        counts.count_modes = 1;
        counts.modes_ptr = pointer_of(&scratch);
    }
    if (drm_ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &counts)) return NULL;
    for (int tries = 0; tries < 4; tries++) {
        uint32_t mode_count = counts.count_modes, encoder_count = counts.count_encoders;
        drm_mode_info *modes = mode_count ? calloc(mode_count, sizeof(*modes)) : NULL;
        uint32_t *encoders = encoder_count ? calloc(encoder_count, sizeof(*encoders)) : NULL;
        struct drm_mode_get_connector info = { .connector_id = id,
            .modes_ptr = pointer_of(modes), .encoders_ptr = pointer_of(encoders),
            .count_modes = mode_count, .count_encoders = encoder_count };
        bool allocated = (!mode_count || modes) && (!encoder_count || encoders);
        int ret = allocated ? drm_ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &info) : -ENOMEM;
        if (!ret && info.count_modes <= mode_count && info.count_encoders <= encoder_count) {
            struct drm_connector *connector = calloc(1, sizeof(*connector));
            if (!connector) {
                free(modes);
                free(encoders);
                return NULL;
            }
            connector->id = info.connector_id;
            connector->encoder_id = info.encoder_id;
            connector->type = info.connector_type;
            connector->type_id = info.connector_type_id;
            connector->connection = info.connection;
            connector->mm_width = info.mm_width;
            connector->mm_height = info.mm_height;
            connector->modes = modes;
            connector->count_modes = (int)info.count_modes;
            connector->encoders = encoders;
            connector->count_encoders = (int)info.count_encoders;
            return connector;
        }
        free(modes);
        free(encoders);
        if (!ret) {
            counts = info;
            counts.modes_ptr = 0;
            counts.encoders_ptr = 0;
            continue;
        }
        if (ret != -EINVAL) return NULL;
        counts = (struct drm_mode_get_connector){ .connector_id = id };
        if (drm_ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &counts)) return NULL;
    }
    return NULL;
}

void drm_connector_free(struct drm_connector *connector) {
    if (!connector) return;
    free(connector->modes);
    free(connector->encoders);
    free(connector);
}

static const char *const connector_types[] = { "Unknown", "VGA", "DVI-I", "DVI-D", "DVI-A",
    "Composite", "SVIDEO", "LVDS", "Component", "DIN", "DP", "HDMI-A", "HDMI-B", "TV", "eDP",
    "Virtual", "DSI", "DPI", "Writeback", "SPI", "USB" };

const char *drm_connector_type_name(uint32_t type) {
    return type < sizeof(connector_types) / sizeof(connector_types[0]) ?
        connector_types[type] : NULL;
}

struct drm_encoder *drm_encoder_get(int fd, uint32_t id) {
    struct drm_mode_get_encoder info = { .encoder_id = id };
    if (drm_ioctl(fd, DRM_IOCTL_MODE_GETENCODER, &info)) return NULL;
    struct drm_encoder *encoder = calloc(1, sizeof(*encoder));
    if (!encoder) return NULL;
    encoder->id = info.encoder_id;
    encoder->crtc_id = info.crtc_id;
    encoder->possible_crtcs = info.possible_crtcs;
    return encoder;
}

void drm_encoder_free(struct drm_encoder *encoder) {
    free(encoder);
}

struct drm_prop *drm_prop_get(int fd, uint32_t id) {
    struct drm_mode_get_property info = { .prop_id = id };
    if (drm_ioctl(fd, DRM_IOCTL_MODE_GETPROPERTY, &info)) return NULL;
    struct drm_prop *prop = calloc(1, sizeof(*prop));
    if (!prop) return NULL;
    prop->id = info.prop_id;
    prop->flags = info.flags;
    memcpy(prop->name, info.name, sizeof(prop->name));
    if (info.count_values) {
        prop->values = calloc(info.count_values, sizeof(*prop->values));
        if (!prop->values) goto failed;
    }
    if (info.count_enum_blobs) {
        prop->enums = calloc(info.count_enum_blobs, sizeof(*prop->enums));
        if (!prop->enums) goto failed;
    }
    info.values_ptr = pointer_of(prop->values);
    info.enum_blob_ptr = pointer_of(prop->enums);
    if (drm_ioctl(fd, DRM_IOCTL_MODE_GETPROPERTY, &info)) goto failed;
    prop->count_values = (int)info.count_values;
    prop->count_enums = (int)info.count_enum_blobs;
    return prop;
failed:
    drm_prop_free(prop);
    return NULL;
}

void drm_prop_free(struct drm_prop *prop) {
    if (!prop) return;
    free(prop->values);
    free(prop->enums);
    free(prop);
}

struct drm_object_props *drm_object_props_get(int fd, uint32_t object, uint32_t type) {
    struct drm_mode_obj_get_properties info = { .obj_id = object, .obj_type = type };
    if (drm_ioctl(fd, DRM_IOCTL_MODE_OBJ_GETPROPERTIES, &info)) return NULL;
    struct drm_object_props *props = calloc(1, sizeof(*props));
    if (!props) return NULL;
    if (!info.count_props) return props;
    props->ids = calloc(info.count_props, sizeof(*props->ids));
    props->values = calloc(info.count_props, sizeof(*props->values));
    if (!props->ids || !props->values) goto failed;
    info.props_ptr = pointer_of(props->ids);
    info.prop_values_ptr = pointer_of(props->values);
    if (drm_ioctl(fd, DRM_IOCTL_MODE_OBJ_GETPROPERTIES, &info)) goto failed;
    props->count = info.count_props;
    return props;
failed:
    drm_object_props_free(props);
    return NULL;
}

void drm_object_props_free(struct drm_object_props *props) {
    if (!props) return;
    free(props->ids);
    free(props->values);
    free(props);
}

struct drm_blob *drm_blob_get(int fd, uint32_t id) {
    struct drm_mode_get_blob info = { .blob_id = id };
    if (drm_ioctl(fd, DRM_IOCTL_MODE_GETPROPBLOB, &info)) return NULL;
    struct drm_blob *blob = malloc(sizeof(*blob) + info.length);
    if (!blob) return NULL;
    blob->length = info.length;
    blob->data = blob + 1;
    if (info.length) {
        info.data = pointer_of(blob->data);
        if (drm_ioctl(fd, DRM_IOCTL_MODE_GETPROPBLOB, &info)) {
            free(blob);
            return NULL;
        }
    }
    return blob;
}

void drm_blob_free(struct drm_blob *blob) {
    free(blob);
}

bool drm_blob_create(int fd, const void *data, size_t size, uint32_t *id) {
    struct drm_mode_create_blob blob = { .data = pointer_of(data), .length = (uint32_t)size };
    if (drm_ioctl(fd, DRM_IOCTL_MODE_CREATEPROPBLOB, &blob)) return false;
    *id = blob.blob_id;
    return true;
}

void drm_blob_destroy(int fd, uint32_t id) {
    struct drm_mode_destroy_blob blob = { .blob_id = id };
    drm_ioctl(fd, DRM_IOCTL_MODE_DESTROYPROPBLOB, &blob);
}

void drm_formats_blob_walk(const struct drm_blob *blob,
        void (*visit)(void *data, uint32_t format, uint64_t modifier), void *data) {
    if (!blob || blob->length < sizeof(struct drm_format_modifier_blob)) return;
    const struct drm_format_modifier_blob *header = blob->data;
    if (header->version != FORMAT_BLOB_CURRENT) return;
    if (header->formats_offset > blob->length || header->modifiers_offset > blob->length) return;
    const uint32_t *formats = (const uint32_t *)((const uint8_t *)blob->data +
        header->formats_offset);
    const struct drm_format_modifier *modifiers =
        (const struct drm_format_modifier *)((const uint8_t *)blob->data +
            header->modifiers_offset);
    for (uint32_t format = 0; format < header->count_formats; format++) {
        for (uint32_t m = 0; m < header->count_modifiers; m++) {
            uint32_t bit = format - modifiers[m].offset;
            if (bit >= 64 || !(modifiers[m].formats & (1ull << bit))) continue;
            visit(data, formats[format], modifiers[m].modifier);
        }
    }
}

bool drm_fb_add(int fd, uint32_t width, uint32_t height, uint32_t format,
        const uint32_t handles[4], const uint32_t pitches[4], const uint32_t offsets[4],
        const uint64_t modifiers[4], uint32_t *id, bool explicit_modifiers) {
    struct drm_mode_fb_cmd2 fb = { .width = width, .height = height, .pixel_format = format,
        .flags = explicit_modifiers ? DRM_MODE_FB_MODIFIERS : 0 };
    for (int i = 0; i < 4; i++) {
        fb.handles[i] = handles[i];
        fb.pitches[i] = pitches[i];
        fb.offsets[i] = offsets[i];
        fb.modifier[i] = modifiers ? modifiers[i] : 0;
    }
    if (drm_ioctl(fd, DRM_IOCTL_MODE_ADDFB2, &fb)) return false;
    *id = fb.fb_id;
    return true;
}

void drm_fb_remove(int fd, uint32_t id) {
    drm_ioctl(fd, DRM_IOCTL_MODE_RMFB, &id);
}

bool drm_dumb_create(int fd, uint32_t width, uint32_t height, uint32_t bpp, uint32_t *handle,
        uint32_t *pitch, uint64_t *size) {
    struct drm_mode_create_dumb dumb = { .width = width, .height = height, .bpp = bpp };
    if (drm_ioctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &dumb)) return false;
    *handle = dumb.handle;
    *pitch = dumb.pitch;
    *size = dumb.size;
    return true;
}

bool drm_dumb_map(int fd, uint32_t handle, uint64_t *offset) {
    struct drm_mode_map_dumb dumb = { .handle = handle };
    if (drm_ioctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &dumb)) return false;
    *offset = dumb.offset;
    return true;
}

void drm_dumb_destroy(int fd, uint32_t handle) {
    struct drm_mode_destroy_dumb dumb = { .handle = handle };
    drm_ioctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dumb);
}

bool drm_prime_fd_to_handle(int fd, int prime_fd, uint32_t *handle) {
    struct drm_prime_handle args = { .fd = prime_fd };
    if (drm_ioctl(fd, DRM_IOCTL_PRIME_FD_TO_HANDLE, &args)) return false;
    *handle = args.handle;
    return true;
}

void drm_prime_close_handle(int fd, uint32_t handle) {
    struct drm_gem_close args = { .handle = handle };
    drm_ioctl(fd, DRM_IOCTL_GEM_CLOSE, &args);
}

bool drm_syncobj_create(int fd, uint32_t flags, uint32_t *handle) {
    struct drm_syncobj_create args = { .flags = flags };
    if (drm_ioctl(fd, DRM_IOCTL_SYNCOBJ_CREATE, &args)) return false;
    *handle = args.handle;
    return true;
}

void drm_syncobj_destroy(int fd, uint32_t handle) {
    struct drm_syncobj_destroy args = { .handle = handle };
    drm_ioctl(fd, DRM_IOCTL_SYNCOBJ_DESTROY, &args);
}

bool drm_syncobj_fd_to_handle(int fd, int syncobj_fd, uint32_t *handle) {
    struct drm_syncobj_handle args = { .fd = syncobj_fd };
    if (drm_ioctl(fd, DRM_IOCTL_SYNCOBJ_FD_TO_HANDLE, &args)) return false;
    *handle = args.handle;
    return true;
}

bool drm_syncobj_export_sync_file(int fd, uint32_t handle, int *sync_file_fd) {
    struct drm_syncobj_handle args = { .handle = handle, .fd = -1,
        .flags = DRM_SYNCOBJ_HANDLE_TO_FD_FLAGS_EXPORT_SYNC_FILE };
    if (drm_ioctl(fd, DRM_IOCTL_SYNCOBJ_HANDLE_TO_FD, &args)) return false;
    *sync_file_fd = args.fd;
    return true;
}

bool drm_syncobj_import_sync_file(int fd, uint32_t handle, int sync_file_fd) {
    struct drm_syncobj_handle args = { .handle = handle, .fd = sync_file_fd,
        .flags = DRM_SYNCOBJ_FD_TO_HANDLE_FLAGS_IMPORT_SYNC_FILE };
    return drm_ioctl(fd, DRM_IOCTL_SYNCOBJ_FD_TO_HANDLE, &args) == 0;
}

bool drm_syncobj_transfer(int fd, uint32_t destination, uint64_t destination_point,
        uint32_t source, uint64_t source_point, uint32_t flags) {
    struct drm_syncobj_transfer args = { .src_handle = source, .dst_handle = destination,
        .src_point = source_point, .dst_point = destination_point, .flags = flags };
    return drm_ioctl(fd, DRM_IOCTL_SYNCOBJ_TRANSFER, &args) == 0;
}

int drm_syncobj_timeline_wait(int fd, uint32_t *handles, uint64_t *points, uint32_t count,
        int64_t timeout_ns, uint32_t flags, uint32_t *first_signaled) {
    struct drm_syncobj_timeline_wait args = { .handles = pointer_of(handles),
        .points = pointer_of(points), .count_handles = count, .timeout_nsec = timeout_ns,
        .flags = flags };
    int ret = drm_ioctl(fd, DRM_IOCTL_SYNCOBJ_TIMELINE_WAIT, &args);
    if (!ret && first_signaled) *first_signaled = args.first_signaled;
    return ret;
}

bool drm_syncobj_timeline_signal(int fd, uint32_t handle, uint64_t point) {
    struct drm_syncobj_timeline_array args = { .handles = pointer_of(&handle),
        .points = pointer_of(&point), .count_handles = 1 };
    return drm_ioctl(fd, DRM_IOCTL_SYNCOBJ_TIMELINE_SIGNAL, &args) == 0;
}

bool drm_client_cap(int fd, uint64_t capability, uint64_t value) {
    struct drm_set_client_cap args = { .capability = capability, .value = value };
    return drm_ioctl(fd, DRM_IOCTL_SET_CLIENT_CAP, &args) == 0;
}

bool drm_cap(int fd, uint64_t capability, uint64_t *value) {
    struct drm_get_cap args = { .capability = capability };
    if (drm_ioctl(fd, DRM_IOCTL_GET_CAP, &args)) return false;
    *value = args.value;
    return true;
}

bool drm_set_crtc(int fd, uint32_t crtc, uint32_t fb, int32_t x, int32_t y,
        const uint32_t *connectors, uint32_t count, const drm_mode_info *mode) {
    struct drm_mode_crtc args = { .set_connectors_ptr = pointer_of(connectors),
        .count_connectors = count, .crtc_id = crtc, .fb_id = fb, .x = (uint32_t)x,
        .y = (uint32_t)y, .mode_valid = mode ? 1 : 0 };
    if (mode) args.mode = *mode;
    return drm_ioctl(fd, DRM_IOCTL_MODE_SETCRTC, &args) == 0;
}

bool drm_page_flip(int fd, uint32_t crtc, uint32_t fb, uint32_t flags, void *user_data) {
    struct drm_mode_crtc_page_flip args = { .crtc_id = crtc, .fb_id = fb, .flags = flags,
        .user_data = pointer_of(user_data) };
    return drm_ioctl(fd, DRM_IOCTL_MODE_PAGE_FLIP, &args) == 0;
}

bool drm_set_cursor(int fd, uint32_t crtc, uint32_t handle, uint32_t width, uint32_t height) {
    struct drm_mode_cursor args = { .flags = DRM_MODE_CURSOR_BO, .crtc_id = crtc,
        .width = width, .height = height, .handle = handle };
    return drm_ioctl(fd, DRM_IOCTL_MODE_CURSOR, &args) == 0;
}

bool drm_set_cursor2(int fd, uint32_t crtc, uint32_t handle, uint32_t width, uint32_t height,
        int32_t hot_x, int32_t hot_y) {
    struct drm_mode_cursor2 args = { .flags = DRM_MODE_CURSOR_BO, .crtc_id = crtc,
        .width = width, .height = height, .handle = handle, .hot_x = hot_x, .hot_y = hot_y };
    return drm_ioctl(fd, DRM_IOCTL_MODE_CURSOR2, &args) == 0;
}

bool drm_move_cursor(int fd, uint32_t crtc, int32_t x, int32_t y) {
    struct drm_mode_cursor args = { .flags = DRM_MODE_CURSOR_MOVE, .crtc_id = crtc,
        .x = x, .y = y };
    return drm_ioctl(fd, DRM_IOCTL_MODE_CURSOR, &args) == 0;
}

bool drm_crtc_set_gamma(int fd, uint32_t crtc, uint32_t size, const uint16_t *red,
        const uint16_t *green, const uint16_t *blue) {
    struct drm_mode_crtc_lut args = { .crtc_id = crtc, .gamma_size = size,
        .red = pointer_of(red), .green = pointer_of(green), .blue = pointer_of(blue) };
    return drm_ioctl(fd, DRM_IOCTL_MODE_SETGAMMA, &args) == 0;
}

void drm_atomic_init(struct drm_atomic *request) {
    memset(request, 0, sizeof(*request));
    request->capacity = DRM_ATOMIC_INLINE_ITEMS;
    request->items = request->inline_items;
    request->out_properties = request->inline_out_properties;
    request->out_values = request->inline_out_values;
    request->objects_capacity = DRM_ATOMIC_INLINE_OBJECTS;
    request->objects = request->inline_objects;
    request->object_counts = request->inline_object_counts;
    request->object_offsets = request->inline_object_offsets;
}

void drm_atomic_reset(struct drm_atomic *request) {
    request->count = 0;
    request->objects_used = 0;
}

void drm_atomic_finish(struct drm_atomic *request) {
    if (request->items != request->inline_items) free(request->items);
    if (request->out_properties != request->inline_out_properties) free(request->out_properties);
    if (request->out_values != request->inline_out_values) free(request->out_values);
    if (request->objects != request->inline_objects) free(request->objects);
    if (request->object_counts != request->inline_object_counts) free(request->object_counts);
    if (request->object_offsets != request->inline_object_offsets) free(request->object_offsets);
    request->items = request->inline_items;
    request->out_properties = request->inline_out_properties;
    request->out_values = request->inline_out_values;
    request->objects = request->inline_objects;
    request->object_counts = request->inline_object_counts;
    request->object_offsets = request->inline_object_offsets;
    request->capacity = DRM_ATOMIC_INLINE_ITEMS;
    request->objects_capacity = DRM_ATOMIC_INLINE_OBJECTS;
    request->count = request->objects_used = 0;
}

static bool atomic_reserve_items(struct drm_atomic *request, uint32_t count) {
    if (count <= request->capacity) return true;
    uint32_t capacity = request->capacity * 2;
    while (capacity < count) capacity *= 2;
    struct drm_atomic_item *items = malloc(capacity * sizeof(*items));
    uint32_t *properties = malloc(capacity * sizeof(*properties));
    uint64_t *values = malloc(capacity * sizeof(*values));
    if (!items || !properties || !values) {
        free(items);
        free(properties);
        free(values);
        return false;
    }
    memcpy(items, request->items, request->count * sizeof(*items));
    if (request->items != request->inline_items) free(request->items);
    if (request->out_properties != request->inline_out_properties)
        free(request->out_properties);
    if (request->out_values != request->inline_out_values) free(request->out_values);
    request->items = items;
    request->out_properties = properties;
    request->out_values = values;
    request->capacity = capacity;
    return true;
}

static bool atomic_reserve_objects(struct drm_atomic *request, uint32_t count) {
    if (count <= request->objects_capacity) return true;
    uint32_t capacity = request->objects_capacity * 2;
    while (capacity < count) capacity *= 2;
    uint32_t *objects = malloc(capacity * sizeof(*objects));
    uint32_t *counts = malloc(capacity * sizeof(*counts));
    uint32_t *offsets = malloc(capacity * sizeof(*offsets));
    if (!objects || !counts || !offsets) {
        free(objects);
        free(counts);
        free(offsets);
        return false;
    }
    if (request->objects != request->inline_objects) free(request->objects);
    if (request->object_counts != request->inline_object_counts) free(request->object_counts);
    if (request->object_offsets != request->inline_object_offsets)
        free(request->object_offsets);
    request->objects = objects;
    request->object_counts = counts;
    request->object_offsets = offsets;
    request->objects_capacity = capacity;
    return true;
}

bool drm_atomic_add(struct drm_atomic *request, uint32_t object, uint32_t property,
        uint64_t value) {
    if (!atomic_reserve_items(request, request->count + 1) ||
            !atomic_reserve_objects(request, request->count + 1)) return false;
    request->items[request->count++] = (struct drm_atomic_item){ .object = object,
        .property = property, .value = value };
    return true;
}

int drm_atomic_commit(int fd, struct drm_atomic *request, uint32_t flags, void *user_data) {
    if (!request->count) return 0;
    request->objects_used = 0;
    for (uint32_t i = 0; i < request->count; i++) {
        uint32_t object = request->items[i].object, group = 0;
        while (group < request->objects_used && request->objects[group] != object) group++;
        if (group == request->objects_used) {
            request->objects[request->objects_used++] = object;
            request->object_counts[group] = 0;
        }
        request->object_counts[group]++;
    }
    uint32_t offset = 0;
    for (uint32_t group = 0; group < request->objects_used; group++) {
        request->object_offsets[group] = offset;
        offset += request->object_counts[group];
    }
    for (uint32_t i = 0; i < request->count; i++) {
        uint32_t object = request->items[i].object, group = 0;
        while (request->objects[group] != object) group++;
        uint32_t slot = request->object_offsets[group]++;
        request->out_properties[slot] = request->items[i].property;
        request->out_values[slot] = request->items[i].value;
    }
    struct drm_mode_atomic atomic = { .flags = flags, .count_objs = request->objects_used,
        .objs_ptr = pointer_of(request->objects),
        .count_props_ptr = pointer_of(request->object_counts),
        .props_ptr = pointer_of(request->out_properties),
        .prop_values_ptr = pointer_of(request->out_values),
        .user_data = pointer_of(user_data) };
    return drm_ioctl(fd, DRM_IOCTL_MODE_ATOMIC, &atomic);
}

void drm_events(int fd, void (*flip)(void *data, uint32_t sequence, uint32_t tv_sec,
        uint32_t tv_usec, uint32_t crtc_id)) {
    char buffer[1024];
    ssize_t length;
    do length = read(fd, buffer, sizeof(buffer));
    while (length < 0 && errno == EINTR);
    if (length < (ssize_t)sizeof(struct drm_event)) return;
    for (ssize_t offset = 0;
            offset + (ssize_t)sizeof(struct drm_event) <= length;) {
        const struct drm_event *event = (const struct drm_event *)(buffer + offset);
        if (event->length < sizeof(*event) || offset + (ssize_t)event->length > length) return;
        if (event->type == DRM_EVENT_FLIP_COMPLETE || event->type == DRM_EVENT_VBLANK) {
            const struct drm_event_vblank *vblank = (const struct drm_event_vblank *)event;
            if (event->length >= sizeof(*vblank))
                flip((void *)(uintptr_t)vblank->user_data, vblank->sequence, vblank->tv_sec,
                    vblank->tv_usec, vblank->crtc_id);
        }
        offset += (ssize_t)event->length;
    }
}

char *drm_format_name(uint32_t format) {
    uint32_t code = format & 0x7fffffffu;
    if (!code) return strdup("INVALID");
    char name[5] = { (char)(code & 0xff), (char)((code >> 8) & 0xff),
        (char)((code >> 16) & 0xff), (char)((code >> 24) & 0xff), 0 };
    for (int i = 3; i > 0 && name[i] == ' '; i--) name[i] = 0;
    char *text = NULL;
    if (asprintf(&text, "%s%s", format & 0x80000000u ? "_BE" : "", name) < 0) return NULL;
    return text;
}

static char *node_scan(const char *prefix) {
    DIR *directory = opendir("/dev/dri");
    if (!directory) return NULL;
    size_t length = strlen(prefix);
    char *node = NULL;
    struct dirent *entry;
    while (!node && (entry = readdir(directory))) {
        if (strncmp(entry->d_name, prefix, length) ||
                entry->d_name[length] < '0' || entry->d_name[length] > '9') continue;
        if (asprintf(&node, "/dev/dri/%s", entry->d_name) < 0) node = NULL;
    }
    closedir(directory);
    return node;
}

char *drm_primary_node(void) {
    return node_scan("card");
}

char *drm_first_render_node(void) {
    return node_scan("renderD");
}

char *drm_device_node(int fd) {
    char link[64], path[PATH_MAX];
    snprintf(link, sizeof(link), "/proc/self/fd/%d", fd);
    ssize_t length = readlink(link, path, sizeof(path) - 1);
    if (length <= 0) return NULL;
    path[length] = 0;
    return strdup(path);
}

static char *node_sibling(dev_t device, const char *prefix) {
    char directory[PATH_MAX];
    snprintf(directory, sizeof(directory), "/sys/dev/char/%u:%u/device/drm",
        major(device), minor(device));
    DIR *handle = opendir(directory);
    if (!handle) return NULL;
    size_t length = strlen(prefix);
    char *node = NULL;
    struct dirent *entry;
    while (!node && (entry = readdir(handle)))
        if (!strncmp(entry->d_name, prefix, length) &&
                asprintf(&node, "/dev/dri/%s", entry->d_name) < 0)
            node = NULL;
    closedir(handle);
    return node;
}

char *drm_render_node(int fd) {
    struct stat st;
    if (fstat(fd, &st)) return NULL;
    char *node = drm_device_node(fd);
    if (node && strstr(node, "renderD")) return node;
    free(node);
    return node_sibling(st.st_rdev, "renderD");
}

char *drm_render_node_for_device(dev_t device) {
    return node_sibling(device, "renderD");
}
