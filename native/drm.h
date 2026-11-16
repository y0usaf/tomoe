#ifndef TOMOE_DRM_H
#define TOMOE_DRM_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include <drm/drm.h>
#include <drm/drm_mode.h>
#include <drm/drm_fourcc.h>

typedef struct drm_mode_modeinfo drm_mode_info;

#ifndef DRM_MODE_CONNECTED
#define DRM_MODE_CONNECTED 1
#define DRM_MODE_DISCONNECTED 2
#define DRM_MODE_UNKNOWNCONNECTION 3
#endif
#ifndef DRM_PLANE_TYPE_PRIMARY
#define DRM_PLANE_TYPE_OVERLAY 0
#define DRM_PLANE_TYPE_PRIMARY 1
#define DRM_PLANE_TYPE_CURSOR 2
#endif

struct drm_res {
    int count_crtcs, count_connectors, count_encoders;
    uint32_t *crtcs, *connectors, *encoders;
};

struct drm_plane_res {
    int count;
    uint32_t *planes;
};

struct drm_crtc {
    uint32_t id, gamma_size;
    drm_mode_info mode;
};

struct drm_plane_info {
    uint32_t id, possible_crtcs;
    int count_formats;
    uint32_t *formats;
};

struct drm_connector {
    uint32_t id, encoder_id, type, type_id;
    int connection, count_encoders, count_modes;
    uint32_t mm_width, mm_height;
    uint32_t *encoders;
    drm_mode_info *modes;
};

struct drm_encoder {
    uint32_t id, crtc_id, possible_crtcs;
};

struct drm_prop {
    uint32_t id, flags;
    int count_values, count_enums;
    char name[DRM_PROP_NAME_LEN];
    uint64_t *values;
    struct drm_mode_property_enum *enums;
};

struct drm_object_props {
    uint32_t count;
    uint32_t *ids;
    uint64_t *values;
};

struct drm_blob {
    uint32_t length;
    void *data;
};

enum { DRM_ATOMIC_INLINE_ITEMS = 32, DRM_ATOMIC_INLINE_OBJECTS = 16 };

struct drm_atomic_item {
    uint32_t object, property;
    uint64_t value;
};

struct drm_atomic {
    uint32_t count, capacity, objects_used, objects_capacity;
    struct drm_atomic_item *items;
    uint32_t *out_properties;
    uint64_t *out_values;
    uint32_t *objects, *object_counts, *object_offsets;
    struct drm_atomic_item inline_items[DRM_ATOMIC_INLINE_ITEMS];
    uint32_t inline_out_properties[DRM_ATOMIC_INLINE_ITEMS];
    uint64_t inline_out_values[DRM_ATOMIC_INLINE_ITEMS];
    uint32_t inline_objects[DRM_ATOMIC_INLINE_OBJECTS];
    uint32_t inline_object_counts[DRM_ATOMIC_INLINE_OBJECTS];
    uint32_t inline_object_offsets[DRM_ATOMIC_INLINE_OBJECTS];
};

int drm_ioctl(int fd, unsigned long request, void *arg);

struct drm_res *drm_res_get(int fd);
void drm_res_free(struct drm_res *res);
struct drm_plane_res *drm_plane_res_get(int fd);
void drm_plane_res_free(struct drm_plane_res *res);
struct drm_crtc *drm_crtc_get(int fd, uint32_t id);
void drm_crtc_free(struct drm_crtc *crtc);
struct drm_plane_info *drm_plane_get(int fd, uint32_t id);
void drm_plane_free(struct drm_plane_info *plane);
struct drm_connector *drm_connector_get(int fd, uint32_t id, bool probe);
void drm_connector_free(struct drm_connector *connector);
struct drm_encoder *drm_encoder_get(int fd, uint32_t id);
void drm_encoder_free(struct drm_encoder *encoder);
const char *drm_connector_type_name(uint32_t type);

struct drm_prop *drm_prop_get(int fd, uint32_t id);
void drm_prop_free(struct drm_prop *prop);
struct drm_object_props *drm_object_props_get(int fd, uint32_t object, uint32_t type);
void drm_object_props_free(struct drm_object_props *props);
struct drm_blob *drm_blob_get(int fd, uint32_t id);
void drm_blob_free(struct drm_blob *blob);
bool drm_blob_create(int fd, const void *data, size_t size, uint32_t *id);
void drm_blob_destroy(int fd, uint32_t id);
void drm_formats_blob_walk(const struct drm_blob *blob,
    void (*visit)(void *data, uint32_t format, uint64_t modifier), void *data);

bool drm_fb_add(int fd, uint32_t width, uint32_t height, uint32_t format,
    const uint32_t handles[4], const uint32_t pitches[4], const uint32_t offsets[4],
    const uint64_t modifiers[4], uint32_t *id, bool explicit_modifiers);
void drm_fb_remove(int fd, uint32_t id);
bool drm_dumb_create(int fd, uint32_t width, uint32_t height, uint32_t bpp, uint32_t *handle,
    uint32_t *pitch, uint64_t *size);
bool drm_dumb_map(int fd, uint32_t handle, uint64_t *offset);
void drm_dumb_destroy(int fd, uint32_t handle);
bool drm_prime_fd_to_handle(int fd, int prime_fd, uint32_t *handle);
void drm_prime_close_handle(int fd, uint32_t handle);

bool drm_syncobj_create(int fd, uint32_t flags, uint32_t *handle);
void drm_syncobj_destroy(int fd, uint32_t handle);
bool drm_syncobj_fd_to_handle(int fd, int syncobj_fd, uint32_t *handle);
bool drm_syncobj_export_sync_file(int fd, uint32_t handle, int *sync_file_fd);
bool drm_syncobj_import_sync_file(int fd, uint32_t handle, int sync_file_fd);
bool drm_syncobj_transfer(int fd, uint32_t destination, uint64_t destination_point,
    uint32_t source, uint64_t source_point, uint32_t flags);
int drm_syncobj_timeline_wait(int fd, uint32_t *handles, uint64_t *points, uint32_t count,
    int64_t timeout_ns, uint32_t flags, uint32_t *first_signaled);
bool drm_syncobj_timeline_signal(int fd, uint32_t handle, uint64_t point);

bool drm_client_cap(int fd, uint64_t capability, uint64_t value);
bool drm_cap(int fd, uint64_t capability, uint64_t *value);

bool drm_set_crtc(int fd, uint32_t crtc, uint32_t fb, int32_t x, int32_t y,
    const uint32_t *connectors, uint32_t count, const drm_mode_info *mode);
bool drm_page_flip(int fd, uint32_t crtc, uint32_t fb, uint32_t flags, void *user_data);
bool drm_set_cursor(int fd, uint32_t crtc, uint32_t handle, uint32_t width, uint32_t height);
bool drm_set_cursor2(int fd, uint32_t crtc, uint32_t handle, uint32_t width, uint32_t height,
    int32_t hot_x, int32_t hot_y);
bool drm_move_cursor(int fd, uint32_t crtc, int32_t x, int32_t y);
bool drm_crtc_set_gamma(int fd, uint32_t crtc, uint32_t size, const uint16_t *red,
    const uint16_t *green, const uint16_t *blue);

void drm_atomic_init(struct drm_atomic *request);
void drm_atomic_reset(struct drm_atomic *request);
void drm_atomic_finish(struct drm_atomic *request);
bool drm_atomic_add(struct drm_atomic *request, uint32_t object, uint32_t property,
    uint64_t value);
int drm_atomic_commit(int fd, struct drm_atomic *request, uint32_t flags, void *user_data);

void drm_events(int fd, void (*flip)(void *data, uint32_t sequence, uint32_t tv_sec,
    uint32_t tv_usec, uint32_t crtc_id));

char *drm_format_name(uint32_t format);
char *drm_primary_node(void);
char *drm_first_render_node(void);
char *drm_device_node(int fd);
char *drm_render_node(int fd);
char *drm_render_node_for_device(dev_t device);
#endif
