#ifndef TOMOE_INTERNAL_H
#define TOMOE_INTERNAL_H
#include "backend.h"
#include "timing.h"
#include "motion.h"
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <GLES2/gl2.h>
#include "wayland.h"
#include <drm/drm_fourcc.h>
#include "region.h"
#include "policy.h"
#include "xkb.h"
#include "wlr-layer-shell-unstable-v1-protocol.h"
#include "xdg-shell-protocol.h"

enum { FRAME_TRACE = 128 };
#define TOMOE_SHADER_COPY_MS 60000
extern uint64_t tomoe_started_ns;
void tomoe_log_line(const char *line);
void tomoe_log_open(const char *name, const char *backend);
void tomoe_log_close(const char *reason);
void tomoe_log_tend(void);
const char *tomoe_log_path(void);
const char *tomoe_data_dirs(void);

struct box { int x, y, width, height; };
struct fbox { double x, y, width, height; };
enum edges { EDGE_NONE = 0, EDGE_TOP = 1, EDGE_BOTTOM = 2, EDGE_LEFT = 4, EDGE_RIGHT = 8 };
bool box_empty(const struct box *box);
bool box_equal(const struct box *a, const struct box *b);
bool box_intersection(struct box *dest, const struct box *a, const struct box *b);
void box_transform(struct box *dest, const struct box *box, enum wl_output_transform transform,
    int width, int height);
void fbox_transform(struct fbox *dest, const struct fbox *box, enum wl_output_transform transform,
    double width, double height);
enum wl_output_transform transform_invert(enum wl_output_transform transform);
enum wl_output_transform transform_compose(enum wl_output_transform a, enum wl_output_transform b);
void transform_coords(enum wl_output_transform transform, int *x, int *y);
void transform_point(double *x, double *y, enum wl_output_transform transform,
    double width, double height);
void region_scale(struct region *dst, const struct region *src, float scale);
void region_scale_xy(struct region *dst, const struct region *src, float sx, float sy);
void region_transform(struct region *dst, const struct region *src,
    enum wl_output_transform transform, int width, int height);

struct addon {
    struct wl_list link;
    const void *owner;
    void (*destroy)(struct addon *addon);
};
void addon_init(struct addon *addon, struct wl_list *addons, const void *owner,
    void (*destroy)(struct addon *addon));
void addon_finish(struct addon *addon);
struct addon *addon_find(struct wl_list *addons, const void *owner,
    void (*destroy)(struct addon *addon));

#define DMABUF_MAX_PLANES 4
struct dmabuf_attributes {
    int32_t width, height;
    uint32_t format;
    uint64_t modifier;
    int n_planes;
    uint32_t offset[DMABUF_MAX_PLANES], stride[DMABUF_MAX_PLANES];
    int fd[DMABUF_MAX_PLANES];
};
void dmabuf_attributes_finish(struct dmabuf_attributes *attributes);

enum { BUFFER_READ = 1 << 0, BUFFER_WRITE = 1 << 1 };
struct buffer;
struct buffer_impl {
    void (*destroy)(struct buffer *buffer);
    bool (*get_dmabuf)(struct buffer *buffer, struct dmabuf_attributes *attributes);
    bool (*begin_access)(struct buffer *buffer, uint32_t flags, void **data, uint32_t *format,
        size_t *stride);
    void (*end_access)(struct buffer *buffer);
};
struct buffer {
    const struct buffer_impl *impl;
    int width, height;
    bool dropped, accessing;
    size_t n_locks;
    struct wl_list addons;
    struct { struct wl_signal destroy, release; } events;
};
void buffer_init(struct buffer *buffer, const struct buffer_impl *impl, int width, int height);
void buffer_finish(struct buffer *buffer);
void buffer_drop(struct buffer *buffer);
struct buffer *buffer_lock(struct buffer *buffer);
void buffer_unlock(struct buffer *buffer);
bool buffer_get_dmabuf(struct buffer *buffer, struct dmabuf_attributes *attributes);
bool buffer_begin_access(struct buffer *buffer, uint32_t flags, void **data, uint32_t *format,
    size_t *stride);
void buffer_end_access(struct buffer *buffer);
struct buffer *buffer_from_resource(struct wl_resource *resource);

struct format {
    uint32_t format;
    size_t len;
    uint64_t *modifiers;
};
struct format_set {
    size_t len;
    struct format *formats;
};
struct format *format_set_get(const struct format_set *set, uint32_t format);
bool format_has(const struct format *format, uint64_t modifier);
bool format_set_has(const struct format_set *set, uint32_t format, uint64_t modifier);
bool format_set_add(struct format_set *set, uint32_t format, uint64_t modifier);
void format_set_finish(struct format_set *set);

struct timeline {
    int drm_fd;
    uint32_t handle;
    size_t refs;
};
struct timeline_waiter {
    int fd;
    struct wl_event_source *source;
    void (*callback)(struct timeline_waiter *waiter);
};
struct timeline *timeline_create(int drm_fd);
struct timeline *timeline_import(int drm_fd, int syncobj_fd);
struct timeline *timeline_ref(struct timeline *timeline);
void timeline_unref(struct timeline *timeline);
int timeline_export_sync_file(struct timeline *timeline, uint64_t point);
bool timeline_import_sync_file(struct timeline *timeline, uint64_t point, int fd);
bool timeline_check(struct timeline *timeline, uint64_t point, uint32_t flags, bool *ready);
bool timeline_signal(struct timeline *timeline, uint64_t point);
bool timeline_waiter_init(struct timeline_waiter *waiter, struct timeline *timeline,
    uint64_t point, uint32_t flags, struct wl_event_loop *loop,
    void (*callback)(struct timeline_waiter *waiter));
void timeline_waiter_finish(struct timeline_waiter *waiter);

struct positioner_rules {
    struct box anchor_rect;
    enum xdg_positioner_anchor anchor;
    enum xdg_positioner_gravity gravity;
    enum xdg_positioner_constraint_adjustment constraint_adjustment;
    bool reactive;
    struct { int32_t width, height; } size, parent_size;
    struct { int32_t x, y; } offset;
};
void positioner_geometry(const struct positioner_rules *rules, struct box *box);
void positioner_unconstrain(const struct positioner_rules *rules, const struct box *constraint,
    struct box *box);
struct buffer *xcursor_load(const char *name, float scale, int *hotspot_x, int *hotspot_y);
char *xcursor_path(void);

struct render;
struct ui_surface;
struct texture { uint32_t width, height; uint64_t serial; };
enum blend_mode { BLEND_PREMULTIPLIED, BLEND_NONE, BLEND_CUTOUT };
enum filter_mode { FILTER_BILINEAR, FILTER_NEAREST };
struct color { float r, g, b, a; };
struct texture_options {
    struct texture *texture;
    struct fbox src_box;
    struct box dst_box;
    const float *alpha;
    const struct region *clip;
    enum wl_output_transform transform;
    enum filter_mode filter_mode;
    enum blend_mode blend_mode;
    struct timeline *wait_timeline;
    uint64_t wait_point;
    const struct surface *surface;
    const struct ui_surface *ui;
    uint64_t content_seq;
    struct texture *lut;
    double rotation;
    const float *tint;
};
struct rect_options {
    struct box box;
    struct color color;
    const struct region *clip;
    enum blend_mode blend_mode;
};
struct read_options {
    void *data;
    uint32_t format, stride;
    struct box src_box;
};
enum { OP_RECT = 1, OP_TEXTURE, OP_BORDER, OP_SHADOW, OP_ROUNDED, OP_BLUR, OP_SHADER,
    OP_MESH = 9, OP_TRAIL = 13, OP_CURSOR = 14, OP_GLASS = 15 };
struct pass;
struct blur_grid { struct box grown; int passes; double offset; };
struct op {
    uint64_t hash, seq, backdrop, key;
    const struct surface *surface;
    const struct ui_surface *ui;
    uint64_t content_seq;
    struct box box, reach, footprint;
    struct fbox src;
    struct blur_grid grid;
    struct region opaque, visible, need;
    int kind, blur;
    bool mapped, hidden, cached;
};
struct oplist { struct op *ops; size_t len, cap; bool lost, valid; };
struct op_clip {
    struct region region;
    const struct region *saved;
    const struct op *op;
    bool limited;
};
bool op_begin(struct pass *pass, int kind, struct box box, struct op_clip *clip);
void op_end(struct pass *pass, struct op_clip *clip);
struct op *pass_recorded(struct pass *pass, size_t before);
bool pass_recording(const struct pass *pass);
size_t pass_record_count(const struct pass *pass);
void oplist_finish(struct oplist *list);
void oplist_reserve(struct oplist *list, size_t used);
bool oplist_damage(struct oplist *old, struct oplist *now, struct region *out);
void oplist_expand(const struct oplist *list, struct region *region);
void render_destroy(struct render *r);
int render_drm_fd(struct render *r);
bool render_has_timeline(struct render *r);
void render_usage(struct render *r, size_t *textures, uint64_t *texture_bytes, size_t *images);
struct render_counters {
    uint64_t gl_calls, quads, runs, fragments, uploads, upload_bytes;
};
void render_counters(struct render *r, struct render_counters *out);
const struct format_set *render_texture_formats(struct render *r);
const struct format_set *render_shm_formats(struct render *r);
struct buffer *render_allocate(struct render *r, int width, int height,
    const struct format *format);
struct texture *texture_from_buffer(struct render *r, struct buffer *buffer);
struct texture *texture_new(struct render *r, uint32_t format, uint32_t width, uint32_t height);
bool texture_upload(struct texture *texture, const void *data, uint32_t format,
    uint32_t stride, const struct rect *rects, int count);
struct texture *texture_from_pixels(struct render *r, uint32_t format, uint32_t stride,
    uint32_t width, uint32_t height, const void *data);
bool texture_update(struct texture *texture, struct buffer *buffer,
    const struct region *damage);
bool texture_read_pixels(struct texture *texture, const struct read_options *options);
void texture_destroy(struct texture *texture);
struct pass *render_begin(struct render *r, struct buffer *buffer, struct timeline *signal,
    uint64_t point);
void pass_add_texture(struct pass *pass, const struct texture_options *options);
void pass_add_rect(struct pass *pass, const struct rect_options *options);
bool pass_submit(struct pass *pass);
int pass_submit_fence(struct pass *pass);
struct pass *render_record(struct render *r, struct buffer *buffer, struct oplist *list);
bool pass_record(struct pass *pass, const struct texture_options *texture, struct box box,
    struct box reach, const double *params, size_t count);
bool pass_record_blur(struct pass *pass, struct box box, struct box reach, struct box footprint,
    const struct blur_grid *grid, const double *params, size_t count, int blur,
    const double *seed, size_t seeds);
void blur_need(const struct op *op, struct region *need);
void pass_clip(struct pass *pass, const struct region *clip);
bool pass_touches(struct pass *pass, struct box box);
void pass_ops(struct pass *pass, const struct oplist *ops);
const struct oplist *pass_oplist(const struct pass *pass);
void render_retire_fences(struct render *r);
bool render_frames_held(struct render *r);

struct screen;

struct binding;
struct pointer_latch { uint32_t button; struct binding *binding; };
struct window;
struct layer_plan;
struct presentation;
struct keyboard_profile;
struct keyboard;
struct keyboard_modifiers {
    uint32_t depressed, latched, locked, group;
};
enum {
    MOD_SHIFT = 1 << 0, MOD_CAPS = 1 << 1, MOD_CTRL = 1 << 2, MOD_ALT = 1 << 3,
    MOD_MOD2 = 1 << 4, MOD_MOD3 = 1 << 5, MOD_LOGO = 1 << 6, MOD_MOD5 = 1 << 7,
};
#define LED_COUNT 3
#define MODIFIER_COUNT 8
struct keymap_slot {
    struct txkb_keymap *keymap;
    struct txkb_state *xkb_state;
    char *keymap_string;
    size_t keymap_size;
    int keymap_fd;
    txkb_led_index_t led_indexes[LED_COUNT];
    txkb_mod_index_t mod_indexes[MODIFIER_COUNT];
    struct keyboard_modifiers modifiers;
    uint32_t leds;
    struct { int32_t rate, delay; } repeat_info;
};
enum { INPUT_KEYBOARD = 1 << 0, INPUT_POINTER = 1 << 1 };
struct input_device {
    struct wl_list link;
    struct tomoe *server;
    char *name, *output_name;
    uint32_t caps;
    struct libinput_device *libinput;
    struct keyboard *keyboard;
    struct wl_client *client;
    void *data;
    struct {
        struct wl_signal destroy;
    } events;
};
struct pointer_motion {
    struct input_device *device;
    uint32_t time_msec;
    double delta_x, delta_y, unaccel_dx, unaccel_dy;
};
struct pointer_absolute {
    struct input_device *device;
    uint32_t time_msec;
    double x, y;
};
struct pointer_button {
    struct input_device *device;
    uint32_t time_msec, button;
    enum wl_pointer_button_state state;
};
struct pointer_axis {
    struct input_device *device;
    uint32_t time_msec;
    enum wl_pointer_axis orientation;
    enum wl_pointer_axis_source source;
    enum wl_pointer_axis_relative_direction relative_direction;
    double delta;
    int32_t delta_discrete;
};
struct logical_keyboard;
struct ui_set;
struct ui_asset_pool;
struct ui_pointer;

struct timeline;
enum { PROGRAM_SCENE, PROGRAM_SDF, PROGRAM_EXTERNAL, PROGRAM_TEXTURE_LUT, PROGRAM_EXTERNAL_LUT,
    PROGRAM_DOWN, PROGRAM_UP, PROGRAM_TRAIL, PROGRAM_COUNT };
struct program {
    GLuint id;
    GLint pos, local, texcoord;
    GLint tex, alpha, opaque, size, radius, clip, width, kind, color, range, power, half_pixel, offset;
    GLint resolution, time, frame, mouse, channel, lut, trail, trail_count, tint;
    GLint box, min_ndc, max_ndc, drag, tex_prev, tex_next;
    GLint geo_to_tex, geo_to_tex_prev, geo_to_tex_next;
    GLint curr_geo_to_prev_geo, curr_geo_to_next_geo;
    GLint progress, clamped_progress, random_seed, path, path_count;
    GLint data, windows, window_count, beneath, selection, marks, marks_box;
    GLint frost, frost_box, grain, vibrancy;
};
enum { SCENE_RECT, SCENE_BORDER, SCENE_SHADOW, SCENE_TEXTURE };
struct scene_quad {
    float pos[8], local[8], tex[8];
    float color[4], tint[4];
    float alpha, radius, width, range, power, size[2];
    int kind, shader_clip, opaque, filter, blend;
    uint32_t target;
    GLuint raw;
    struct texture *texture;
    const struct region *clip;
};
struct trail_cell {
    float clip[4];
    float extent[2];
    float color[4];
    float radius, rounded;
};
void pass_scene(struct pass *pass, const struct scene_quad *quad);
void pass_trail(struct pass *pass, const struct trail_cell *cells, size_t count, uint64_t pixels);
void pass_flush(struct pass *pass);
void pass_scene_attributes_dirty(struct pass *pass);
struct program *render_shader_create(struct render *r, const char *source, bool *pending);
bool render_shader_reap(struct render *r);
struct program *render_shader_compile(struct render *r, const char *source);
void render_shader_destroy(struct program *program);
struct window_shader {
    struct program *program;
    char *path;
    char entry[64];
    long mtime;
    int grid_x, grid_y, margin;
    double settle;
    struct window_shader *next;
};
enum { WINDOW_PATH = 32 };
struct window_path_sample {
    int x, y;
    double t;
};
int window_path_samples(struct tomoe *server, uint32_t id,
    struct window_path_sample *out, int max);
struct window_shader *window_shader_for(struct tomoe *server, const char *path,
    int grid_x, int grid_y, int margin, double settle, const char *entry);
struct window_shader *window_mesh_shader(struct tomoe *server);
void window_shaders_finish(struct tomoe *server);
struct window_mesh {
    uint32_t id;
    int grid_x, grid_y;
    struct box frame;
    float *positions;
    size_t capacity;
    uint64_t digest;
    struct window_mesh *next;
};
struct window_mesh *window_mesh_for(struct tomoe *server, uint32_t id, int grid_x, int grid_y,
    const double *positions, size_t count);
void window_meshes_finish(struct tomoe *server);
struct mesh_options {
    struct texture *texture;
    struct program *shader;
    const struct window_mesh *mesh;
    struct box dst_box;
    struct fbox src_box;
    enum wl_output_transform transform;
    const float *trail, *path;
    const float *alpha;
    struct timeline *wait_timeline;
    uint64_t wait_point;
    int trail_count, path_count, grid_x, grid_y, frame, drag;
    enum wl_output_transform position_transform;
    double box[4], time, progress, seed;
    double origin[2], scale;
};
void pass_add_mesh(struct pass *pass, const struct mesh_options *options);
#define RING_SLOTS 4
#define RING_START_SLOTS 2
#define RING_FRAME_SLOTS 3
struct ring {
    struct buffer *slots[RING_SLOTS];
    uint64_t frames[RING_SLOTS];
    int width, height;
    bool implicit;
    bool grow;
    struct format_set format;
};
struct render *render_create(int drm_fd);
struct program *render_program(struct render *renderer, int kind);
void pass_quad(struct pass *pass, struct program *p, const float pos[8], const float local[8],
    const float texcoords[8]);
void pass_triangles(struct pass *pass, struct program *p, const float *pos, size_t vertices);
bool render_texture_gl(struct texture *texture, GLenum *target, GLuint *tex, bool *alpha);
GLuint render_buffer_fbo(struct render *renderer, struct buffer *buffer);
uint32_t render_read_format(struct render *renderer);
const struct format_set *render_formats(struct render *renderer);
bool render_wait(struct render *renderer, struct timeline *timeline,
    uint64_t point);
bool ring_configure(struct tomoe *s, struct ring *ring, struct screen *output,
    int width, int height, bool implicit);
struct buffer *ring_acquire(struct tomoe *s, struct ring *ring);
struct buffer *ring_create(struct tomoe *s, struct ring *ring);
void ring_reserve(struct tomoe *s, struct ring *ring);
struct buffer *ring_take(struct ring *ring);
bool ring_grow(struct tomoe *s, struct ring *ring);
void ring_finish(struct ring *ring);
bool fenced(struct screen *output);

struct screen;
struct session;
struct kms;
#define CURSOR_THEME_WATCHES 8
struct cursor_theme_watch {
    int wd;
    char *name;
};
struct cursor_image {
    struct buffer *buffer;
    struct texture *texture;
    int hotspot_x, hotspot_y;
    float scale;
};
struct screen_mode {
    int32_t width, height, refresh;
    bool preferred, interlaced;
    struct wl_list link;
    void *data;
};
enum {
    SCREEN_ENABLED = 1 << 0, SCREEN_MODE = 1 << 1, SCREEN_SCALE = 1 << 2,
    SCREEN_TRANSFORM = 1 << 3, SCREEN_VRR = 1 << 4, SCREEN_BUFFER = 1 << 5,
    SCREEN_WAIT = 1 << 6, SCREEN_GAMMA = 1 << 7, SCREEN_PROFILE = 1 << 8,
};
struct profile {
    int refs;
    char *path;
    uint16_t *degamma, *gamma;
    size_t degamma_size, gamma_size;
    uint64_t ctm[9];
};
enum screen_mode_type { SCREEN_MODE_FIXED, SCREEN_MODE_CUSTOM };
enum screen_kind { SCREEN_DRM, SCREEN_NESTED, SCREEN_HEADLESS };
enum { SCREEN_OVERLAYS = 3 };
struct screen_overlay {
    struct buffer *buffer;
    struct timeline *wait;
    uint64_t wait_point;
    int x, y, width, height;
    int src_x, src_y, src_width, src_height;
    bool underlay;
};
struct screen_state {
    uint32_t committed;
    bool enabled, adaptive_sync_enabled, tearing_page_flip;
    float scale;
    enum wl_output_transform transform;
    enum screen_mode_type mode_type;
    struct screen_mode *mode;
    struct { int32_t width, height, refresh; } custom_mode;
    struct buffer *buffer;
    struct timeline *wait_timeline;
    uint64_t wait_point;
    uint16_t *gamma;
    size_t gamma_size;
    const struct profile *profile;
    size_t overlay_count;
    struct screen_overlay overlays[SCREEN_OVERLAYS];
};
struct screen_update {
    struct screen *output;
    struct screen_state base;
};
struct ns_timer {
    int fd;
    int64_t at;
    struct wl_event_source *source;
    void (*fire)(void *data);
    void *data;
};
struct screen_impl {
    bool (*test)(struct screen_update *updates, size_t count);
    bool (*commit)(struct screen_update *updates, size_t count);
    const struct format_set *(*formats)(struct screen *screen);
    const struct format_set *(*overlay_formats)(struct screen *screen);
    dev_t (*device)(struct screen *screen);
    size_t (*gamma_size)(struct screen *screen);
    size_t (*degamma_size)(struct screen *screen);
    size_t (*overlays)(struct screen *screen);
    bool (*cursor)(struct screen *screen, struct buffer *buffer, int hotspot_x, int hotspot_y);
    void (*move_cursor)(struct screen *screen, int x, int y);
    void (*destroy)(struct screen *screen);
    bool replace_flip, late_frames;
};
struct screen_present {
    struct screen *output;
    size_t commit_seq;
    bool presented;
    struct timespec when;
    unsigned seq;
    int refresh;
    uint32_t flags;
};
enum {
    TOMOE_PENDING_INPUT = 64,
    TOMOE_COST_BUCKETS = 512,
    TOMOE_COST_GRAIN_NS = 40000,
};
struct screen_bind {
    struct screen *output;
    struct wl_resource *resource;
};
struct screen {
    struct tomoe *server;
    const struct screen_impl *impl;
    enum screen_kind kind;
    char *name, *description, *make, *model, *serial;
    int32_t phys_width, phys_height;
    struct wl_list modes;
    struct screen_mode *current_mode;
    int32_t width, height, refresh;
    float scale;
    enum wl_output_transform transform;
    bool enabled, adaptive_sync_supported, adaptive_sync, hardware_cursor, tearing;
    bool frame_after_events;
    bool frame_pending, needs_frame, animation_only, power_off, frame_replaced, deferred;
    bool suspended;
    int lx, ly, software_cursor_locks;
    uint64_t deferrals, warps;
    uint64_t plane_tests, plane_refusals;
    unsigned underlay_plan;
    double cursor_x, cursor_y;
    uint64_t cursor_seen_generation;
    int cursor_seen_hotspot_x, cursor_seen_hotspot_y;
    float cursor_seen_scale;
    size_t commit_seq;
    int64_t vblank_ns, present_ns, frame_start_ns, frame_armed_ns, frame_ready_ns, frame_target_ns, frame_cost_ns;
    struct {
        uint16_t buckets[TOMOE_COST_BUCKETS];
        uint16_t heavy[TOMOE_COST_BUCKETS];
        uint32_t samples, heavy_samples, warmup;
        bool heavy_last;
        int64_t predicted_ns, slack_ns;
    } clock;
    uint64_t input_seen, commit_seen;
    uint64_t pending_input[TOMOE_PENDING_INPUT], pending_commit[TOMOE_PENDING_INPUT];
    size_t pending_input_count, pending_commit_count;
    uint64_t missed_frames;
    int frame_fence;
    struct ns_timer frame_timer;
    bool idle_frame;
    struct wl_global *global;
    struct wl_list resources, xdg_resources;
    struct {
        struct wl_signal frame, present, request_state, commit, destroy, bind;
    } events;
    void *data;
};

struct plane_item {
    struct surface *surface;
    struct buffer *buffer;
    struct box dst;
    int src_x, src_y, src_width, src_height;
    bool underlay;
};
struct plane_assignment {
    size_t count;
    struct plane_item item[SCREEN_OVERLAYS];
    struct region excluded;
    bool changed, retest, tested;
    uint64_t signature, shape, refused;
};

enum animation_kind { ANIMATION_OFF, ANIMATION_SPRING, ANIMATION_EASE };
struct animation_config {
    int kind, duration_ms, curve;
    double damping_ratio, stiffness, epsilon, launch, bezier[4];
};
struct animation {
    double from, to, start, duration, v0, sample;
    struct animation_config config;
    bool active, advanced;
};

struct output {
    struct wl_list link;
    struct tomoe *server;
    struct screen *screen;
    struct wl_listener frame, present, request, destroy;
    struct screen_state initial, pending;
    struct screen_state deferred;
    bool configured, admitted, pending_configured, pending_positioned;
    bool request_pending, pending_hold;
    uint64_t id;
    uint64_t request_id;
    int pending_x, pending_y;
    int x, y;
    bool positioned;
    bool baseline_positioned;
    int baseline_x, baseline_y;
    char mirror[129];
    char pending_mirror[129];
    bool tearing, pending_tearing;
    struct profile *profile, *pending_profile;
    const char *profile_error, *pending_profile_error;
    struct buffer *capture_buffer;
    struct texture *gamma_lut;
    struct buffer *presented[2];
    uint64_t presented_point;
    struct ring ring;
    bool lock_rendered, gamma_dirty;
    struct surface *scanout;
    struct plane_assignment planes;
    bool damaged;
    struct wl_event_source *tick;
    double tick_at;
    double sample;
    bool animating;
    struct oplist ops, spare_ops, cursor_ops, cursor_spare_ops;
    uint64_t frames, reused, repeated;
    bool reused_present;
    struct buffer *drawn_buffer;
    uint64_t damage_seq;
    struct region damage[8];
    struct { uint64_t frame, age; int64_t pixels; size_t ops; } drawn;
    struct { uint64_t ns, counts[8]; } timing;
    struct { uint32_t us[FRAME_TRACE]; unsigned head, count; } traced;
};

static inline bool output_is_active(const struct output *output) {
    return output && output->admitted && output->screen && output->screen->enabled;
}

enum target_kind { TARGET_WINDOW, TARGET_LAYER, TARGET_UNMANAGED, TARGET_ICON };
struct window_style {
    int radius, blur, tearing;
    int64_t focused, unfocused;
};
struct visual {
    double opacity;
    double scale, rotation;
    int translate_x, translate_y;
    float tint[4];
    bool closing;
};
struct target {
    uint32_t id;
    enum target_kind kind;
    int x, y, width, height, geometry_x, geometry_y;
    double scale;
    struct screen *output;
    int client_width, client_height;
    bool fullscreen;
    double offset_x, offset_y, alpha;
    int drawn_width, drawn_height;
    struct window_style style;
    struct visual visual;
    struct window_shader *shader;
    struct window_mesh *mesh;
    double shader_time;
};
struct frame {
    struct tomoe *server;
    struct pass *pass;
    struct buffer *buffer;
    const struct plane_assignment *planes;
    int blurs;
    int x, y, width, height;
    enum wl_output_transform transform;
    double view_x, view_y, zoom;
    double sample;
    uint32_t focused;
};
struct effects;
void effect_border(struct frame *f, struct fbox geometry, double width, double radius,
    uint32_t rgba, float alpha);
void effect_shadow(struct frame *f, struct fbox geometry, double range, double radius,
    uint32_t rgba, double power, float alpha);
bool effect_texture(struct frame *f, const struct texture_options *options,
    struct fbox dst, struct fbox clip, double radius);
void effect_cutout(struct frame *f, struct fbox dst, struct fbox clip, double radius);
void effect_blur(struct frame *f, struct fbox area, double radius, int passes,
    double offset, int margin, double alpha);
struct shader_inputs {
    const float *data;
    int data_count;
    const float *windows;
    int window_count;
    const struct fbox *part;
    const float *selection;
    const struct texture *marks;
    const float *marks_box;
    bool opaque;
    const struct region *frost_ring;
    uint64_t frost_stamp;
};
uint64_t effects_frost_stamp(struct tomoe *s);
void effect_shader(struct frame *f, struct program *p, struct fbox area, double time, int frame,
    const struct shader_inputs *inputs);
enum { TRAIL_CELLS = 0, TRAIL_DOTS = 1 };
struct trail_style {
    double cell, life;
    int steps, shape;
    uint32_t rgba;
};
void effect_trail(struct frame *f, struct fbox area, const struct trail_style *style);
bool effect_cursor(struct frame *f, const struct texture_options *options,
    const struct fbox corners[4], const float texcoords[8]);
bool effect_glass(struct frame *f, struct program *p, struct texture *map, struct fbox area,
    double bend);
void effects_plan(struct tomoe *s, struct oplist *list);
void effects_forget(struct tomoe *s);
void effects_counters(struct tomoe *s, uint64_t *blurs, uint64_t *reused);
bool effects_create(struct tomoe *s);
bool effects_admit_trail(struct tomoe *s);
bool effects_trail_reserve(struct tomoe *s, double cell);
void effects_finish(struct tomoe *s);
bool background_effects_listen(struct tomoe *s);
const struct region *background_blur_region(struct tomoe *s, struct surface *surface);
struct presentation_output {
    struct output *output;
    struct box box;
    int scale_120;
};
struct output_location {
    struct output *output;
    int x, y, width, height, scale_120;
    bool active;
};
struct presentation_target {
    struct node *node;
    struct target target;
    int layer_x, layer_y;
    int band;
    size_t order;
    bool visible, desired_visible, staged;
    int width, height, fullscreen, maximize;
};
enum { ANIMATION_SETTLE_MOVE, ANIMATION_SETTLE_OPEN, ANIMATION_SETTLE_CLOSE,
    ANIMATION_SETTLE_RESIZE, ANIMATION_SETTLE_VIEW, ANIMATION_SETTLE_SWITCH };
struct animation_stats {
    uint64_t frames, gaps, dead, stale;
    int64_t first_passed_ns, last_frame_ns, lead_min_ns, lead_max_ns;
    int64_t first_frame_ns, max_interval_ns, steady_interval_ns, span_ns, intervals;
};
double animation_now(void);
void animation_start(struct animation *a, const struct animation_config *config,
    double from, double to, double now);
void animation_start_from(struct animation *a, const struct animation_config *config,
    double from, double to, double now, double velocity);
bool animation_retarget(struct animation *a, const struct animation_config *config, double to);
double animation_velocity(const struct animation *a, double now);
double animation_handoff(const struct animation *a);
double animation_at(const struct animation *a, double now);
double animation_value(struct animation *a, double now);
double animation_advance(struct animation *a, double now, struct animation_stats *stats);
double animation_reference(const struct screen *screen, int64_t start_ns);
void animation_frame_record(struct tomoe *s, const struct screen *screen,
    int64_t start_ns, double sample, bool moving);
int animation_settle_ms(const struct tomoe *s, int which);
const char *tomoe_animation(struct tomoe *s);
struct ghost {
    struct wl_list link;
    uint32_t id;
    struct texture *texture;
    struct box box;
    struct animation fade, grow;
    double alpha, scale;
};
struct input_config {
    double disabled, disabled_on_external_mouse, tap, tap_drag, tap_drag_lock, natural_scroll;
    double accel_speed, accel_profile, dwt, left_handed, middle_emulation;
    double scroll_method, scroll_button, click_method;
};
struct named_input_config {
    char *name;
    struct input_config config;
};
enum { CURSOR_HISTORY = 128 };
struct cursor_sample { float x, y; double t; };
struct cursor_motion {
    double sample;
    double presented;
    double settle;
    bool live;
    double target[4], target_offset[2];
    double current[4], current_offset[2];
    bool software, trail_enabled;
    struct cursor_sample history[CURSOR_HISTORY];
    int history_head, history_count;
    double history_at;
    double shake, shake_distance;
    double velocity[2], acceleration[2], velocity_at;
};
enum { CURSOR_GLASS_EDGE = 2 };
struct cursor_glass {
    struct wl_event_source *timer;
    struct program *program;
    struct texture *map;
    const struct screen *screen;
    uint64_t map_generation;
    int64_t moved_ns;
    double x, y, bend;
    bool armed, engaged, broken;
};

enum cursor_term_field {
    CURSOR_FIELD_SPEED,
    CURSOR_FIELD_ANGLE,
    CURSOR_FIELD_ACCELERATION,
    CURSOR_FIELD_SHAKE,
    CURSOR_FIELD_VX,
    CURSOR_FIELD_VY,
    CURSOR_FIELD_CONSTANT
};

enum { CURSOR_TERM_CLAUSES = 8, CURSOR_EFFECTS_MAX = 32 };

struct cursor_term_clause {
    int field;
    double scale, above, below;
};

struct cursor_term {
    int count;
    struct cursor_term_clause clauses[CURSOR_TERM_CLAUSES];
};

struct cursor_effect {
    struct cursor_term rotation, scale, stretch, stretch_angle, offset_x, offset_y;
    double settle;
};

struct cursor_effect_set {
    int count;
    struct cursor_effect entries[CURSOR_EFFECTS_MAX];
};
enum { SCREENSHOT_PALETTE_MAX = 32 };

struct settings {
    struct input_config touchpad, mouse;
    struct named_input_config devices[64];
    size_t device_count;
    bool force_ssd, honor_invalid_serial, tearing, wait_frame, pointer_follows_focus;
    int nested_width, nested_height;
    int border_width, border_radius;
    uint32_t border_focused, border_unfocused;
    int shadow_range;
    uint32_t shadow_color;
    double shadow_power;
    bool blur_enabled;
    int blur_passes, blur_margin;
    bool cursor_glass_enabled;
    int cursor_glass_delay_ms;
    bool cursor_trail_enabled;
    double cursor_trail_cell, cursor_trail_life;
    int cursor_trail_steps, cursor_trail_shape;
    uint32_t cursor_trail_color;
    double blur_offset, blur_noise, blur_vibrancy;
    double shader_hold;
    bool screenshot_freeze, screenshot_motion_enabled;
    double screenshot_motion_stiffness, screenshot_motion_damping;
    uint32_t screenshot_mark_ink;
    int screenshot_mark_width, screenshot_mark_head, screenshot_mark_size;
    char *screenshot_mark_font;
    char *screenshot_mark_keys[6];
    size_t screenshot_mark_key_count;
    uint32_t screenshot_mark_palette[SCREENSHOT_PALETTE_MAX];
    char *screenshot_mark_palette_keys[SCREENSHOT_PALETTE_MAX];
    size_t screenshot_mark_palette_count;
    size_t screenshot_mark_palette_key_count;
    struct animation_config window_move, window_open, window_close, window_resize, window_switch,
        window_view, window_alpha, window_tint, window_scale, window_rotation, window_translate;
    char *blur_namespaces[64];
    size_t blur_namespace_count;
};
void settings_clear(struct settings *settings);
void settings_finish(struct settings *settings);
int tomoe_test_flag(const char *name);
const char *tomoe_test_flag_value(const char *name);
int tomoe_set_test_flag(const char *name, const char *value);
void tomoe_test_gestures(struct tomoe *s);
void tomoe_select_backend(const char *kind);
void tomoe_select_drm_devices(const char *devices);
void tomoe_select_render_node(const char *node);
void input_config_unset(struct input_config *config);
int input_setting(struct settings *settings, const char *key, double value, const char *text);
void input_devices_apply(struct tomoe *s);
bool libinput_listen(struct tomoe *s);
void libinput_finish(struct tomoe *s);
void input_latch(struct tomoe *s);
struct input_device *input_device_create(struct tomoe *s, const char *name, uint32_t caps);
void input_device_destroy(struct input_device *device);
void input_device_leds(struct input_device *device, uint32_t leds);
void input_key(struct input_device *device, uint32_t time_msec, uint32_t keycode, uint32_t state);
void input_modifiers(struct input_device *device, const struct keyboard_modifiers *modifiers);
void input_pointer_motion(struct pointer_motion *event);
void input_pointer_absolute(struct pointer_absolute *event);
void input_pointer_button(struct pointer_button *event);
void input_pointer_axis(struct pointer_axis *event);
void input_pointer_frame(struct input_device *device);
bool keymap_slot_set(struct keymap_slot *slot, struct txkb_keymap *keymap);
void keymap_slot_finish(struct keymap_slot *slot);
uint32_t keymap_slot_modifiers(const struct keymap_slot *slot);
struct presentation {
    struct presentation_output *outputs;
    size_t output_count;
    struct presentation_target *targets;
    size_t target_count, next_order;
    int view_x, view_y;
    double view_zoom;
    uint32_t focused;
    uint32_t grab_id;
    int grab_mode;
    struct wl_list bindings;
    struct keyboard_profile *keyboard;
    bool outputs_changed, replace_bindings;
    struct ui_set *ui;
    struct settings *settings;
    char *grab_owner, *grab_otherwise;
    uint64_t grab_source;
    bool grab_staged;
    struct sound_bank *sounds;
    struct motion_plan motion;
};
void settings_publish(struct tomoe *s, struct presentation *plan);
enum sound_event { SOUND_KEY, SOUND_BUTTON, SOUND_OPEN, SOUND_CLOSE, SOUND_BELL, SOUND_EVENTS };
void sound_play(struct tomoe *s, enum sound_event event);
void sound_publish(struct tomoe *s, struct presentation *plan);
void sound_bank_free(struct sound_bank *bank);
void sound_finish(struct tomoe *s);
bool system_bell_listen(struct tomoe *s);
struct surface;
struct subsurface;
struct release;
struct node;
struct surface_role {
    const char *name;
    bool no_object, keep_buffer;
    void (*client_commit)(struct surface *surface);
    void (*commit)(struct surface *surface);
    void (*map)(struct surface *surface);
    void (*unmap)(struct surface *surface);
    void (*destroy)(struct surface *surface);
};
struct surface_viewport {
    bool has_src, has_dst;
    struct fbox src;
    int dst_width, dst_height;
};
struct surface_state {
    uint32_t committed, locks;
    bool fifo_set, fifo_wait;
    struct buffer *buffer;
    int dx, dy, width, height, buffer_width, buffer_height, scale;
    uint32_t alpha, content_type;
    enum wl_output_transform transform;
    struct region surface_damage, buffer_damage, input, opaque;
    struct surface_viewport viewport;
    struct wl_list frames, feedbacks, waits, synced, link;
    struct timeline *acquire, *release;
    uint64_t acquire_point, release_point;
};
struct surface {
    struct wl_resource *resource;
    struct tomoe *server;
    struct wl_list link;
    struct surface_state pending, current, *last_cached;
    struct wl_list cached, synced, outputs, below, above, pending_below, pending_above;
    struct wl_list dmabuf_feedbacks;
    struct texture *texture;
    struct buffer *buffer;
    struct release *release;
    struct region input_region;
    const struct surface_role *role;
    struct wl_resource *role_resource, *viewport, *fractional, *syncobj;
    struct wl_resource *fifo;
    bool fifo_barrier, fifo_latched;
    struct wl_resource *alpha_modifier, *content_type;
    struct wl_listener role_resource_destroy;
    struct subsurface *subsurface;
    struct screen *primary;
    struct screen *feedback_output;
    uint8_t feedback_mode, feedback_pending;
    bool feedback_pending_set;
    uint64_t feedback_pending_at;
    time_t frames_answered;
    uint64_t commit_seq, buffer_serial;
    struct { uint64_t seq, prev; struct box box; } commits[4];
    int preferred_scale;
    uint32_t fractional_scale;
    bool mapped, pending_rejected, sent_transform, seen, scene_owned;
    struct xdg_toplevel *external_toplevel;
    struct {
        struct wl_signal client_commit, commit, map, unmap, destroy;
    } events;
    void *data;
};
struct surface_synced;
struct surface_synced_impl {
    size_t size;
    void (*init)(void *state);
    void (*finish)(void *state);
    void (*move)(void *dst, void *src);
    void (*commit)(struct surface_synced *synced);
};
struct surface_synced {
    struct surface *surface;
    const struct surface_synced_impl *impl;
    void *pending, *current;
    struct wl_list link;
};
bool surface_damage_since(const struct surface *surface, uint64_t seq, struct box *out);
typedef bool (*surface_iterator)(struct surface *surface, int x, int y, void *data);
struct node {
    struct node *parent;
    struct wl_list link, children;
    bool enabled;
    int x, y;
    void *data;
    struct surface *surface;
    struct wl_listener surface_destroy;
    struct {
        struct wl_signal destroy;
    } events;
};

struct seat;
struct seat_client;
struct source;
struct drag;
struct seat_pointer_grab;
struct seat_keyboard_grab;
struct seat_pointer_grab_interface {
    void (*enter)(struct seat_pointer_grab *grab, struct surface *surface, double sx, double sy);
    void (*clear_focus)(struct seat_pointer_grab *grab);
    void (*motion)(struct seat_pointer_grab *grab, uint32_t time, double sx, double sy);
    uint32_t (*button)(struct seat_pointer_grab *grab, uint32_t time, uint32_t button,
        uint32_t state);
    void (*axis)(struct seat_pointer_grab *grab, uint32_t time, uint32_t orientation, double value,
        int32_t discrete, uint32_t source, uint32_t direction);
    void (*frame)(struct seat_pointer_grab *grab);
    void (*cancel)(struct seat_pointer_grab *grab);
};
struct seat_keyboard_grab_interface {
    void (*enter)(struct seat_keyboard_grab *grab, struct surface *surface,
        const uint32_t keys[], size_t count, const struct keyboard_modifiers *modifiers);
    void (*clear_focus)(struct seat_keyboard_grab *grab);
    void (*key)(struct seat_keyboard_grab *grab, uint32_t time, uint32_t key, uint32_t state);
    void (*modifiers)(struct seat_keyboard_grab *grab,
        const struct keyboard_modifiers *modifiers);
    void (*cancel)(struct seat_keyboard_grab *grab);
};
struct seat_pointer_grab {
    const struct seat_pointer_grab_interface *interface;
    struct seat *seat;
};
struct seat_keyboard_grab {
    const struct seat_keyboard_grab_interface *interface;
    struct seat *seat;
};
struct seat_client {
    struct wl_list link;
    struct seat *seat;
    struct wl_client *client;
    struct wl_list resources, pointers, keyboards, data_devices, primary_devices;
    int32_t acc_discrete[2], last_discrete[2];
    double acc_axis[2];
};
#define SEAT_BUTTONS 16
struct seat_button { uint32_t button, pressed; };
struct seat_pointer_state {
    struct seat_client *focused_client;
    struct surface *focused_surface;
    double sx, sy;
    struct seat_pointer_grab *grab, default_grab;
    bool sent_axis_source, frame_pending;
    struct seat_button buttons[SEAT_BUTTONS];
    size_t button_count;
    uint32_t grab_button, grab_serial, enter_serial;
    struct wl_listener surface_destroy;
};
struct seat_keyboard_state {
    struct keymap_slot *keyboard;
    struct seat_client *focused_client;
    struct surface *focused_surface;
    struct seat_keyboard_grab *grab, default_grab;
    struct wl_listener surface_destroy;
};
struct selection_slot {
    struct source *source;
    struct wl_listener destroy;
};
struct seat {
    struct tomoe *server;
    struct wl_global *global;
    uint32_t capabilities;
    struct wl_list clients, controls, drag_offers;
    struct seat_pointer_state pointer_state;
    struct seat_keyboard_state keyboard_state;
    struct selection_slot selection, primary, drag_source;
    struct drag *drag;
};

enum { POINTER_TRAIL = 64 };
struct pointer_sample { double x, y, t; };

struct tomoe {
    struct wl_display *display;
    struct output_manager *output_manager;
    struct settings settings;
    struct effects *effects;
    struct screenshot *screenshot;
    struct sound *sound;
    struct session *session;
    struct kms *kms;
    enum screen_kind backend;
    struct render *renderer;
    struct node *scene;
    struct node *window_tree, *layer_tree[4], *fullscreen_tree, *popup_tree;
    struct cursor_image cursor_image;
    struct cursor_motion cursor_motion;
    struct cursor_glass cursor_glass;
    struct cursor_effect_set cursor_effects;
    struct buffer *default_cursor;
    float default_cursor_scale;
    uint32_t default_cursor_shape;
    int default_hotspot_x, default_hotspot_y;
    struct cursor_theme_watch cursor_theme_watches[CURSOR_THEME_WATCHES];
    int cursor_theme_fd;
    struct wl_event_source *cursor_theme_source;
    struct seat *seat;
    struct logical_keyboard *logical_keyboard;
    struct keyboard_profile *keyboard_profile;
    struct ime *ime;
    struct wl_list input_devices, background_effects;
    struct wl_list shortcut_inhibitors;
    struct wl_list surfaces, feedbacks;
    struct wl_list windows, layers, outputs, keyboards, events, bindings, virtual_pointers;
    struct wl_list ghosts;
    struct wl_list activation_tokens;
    size_t activation_tracked_count;
    size_t activation_token_count;
    struct wl_list activation_pending;
    size_t activation_pending_count;
    struct wl_listener new_input;
    struct libinput *libinput;
    struct libinput_event *libinput_held;
    struct wl_event_source *libinput_source, *libinput_idle;
    struct wl_list libinput_fds;

    uint32_t next_id, focused, grab_id;
    uint64_t next_binding_id, next_device_id, next_output_id;
    uint32_t latest_keyboard_enter_serial, latest_pointer_enter_serial;
    bool have_keyboard_enter_serial, have_pointer_enter_serial;
    int grab_mode;
    double grab_x, grab_y;
    double pointer_x, pointer_y;
    struct pointer_sample pointer_trail[POINTER_TRAIL];
    double pointer_trail_next;
    int pointer_trail_count, pointer_trail_head;
    struct surface *cursor_surface;
    int32_t cursor_hotspot_x, cursor_hotspot_y;
    bool cursor_hidden;
    uint64_t cursor_generation;
    char *cursor_result;
    char *motion_result;
    uint32_t hovered;
    uint64_t ui_hovered;
    struct pointer_latch pointer_latches[32];
    char *grab_owner, *grab_otherwise;
    uint64_t grab_source;
    size_t pointer_latch_count;
    struct timeline *render_timeline;
    uint64_t render_point;
    struct wl_listener cursor_surface_destroy;
    int view_x, view_y;
    double view_zoom;
    struct animation view_x_anim, view_y_anim, view_zoom_anim;
    struct {
        uint64_t input_at, pointer_at, drag_at, key_at, commit_at, event_at, frame_at;
        uint64_t pointer_drawn, drag_drawn;
        int64_t pointer_event_ns;
        uint64_t input_ring[TOMOE_PENDING_INPUT], commit_ring[TOMOE_PENDING_INPUT];
        uint64_t input_total, commit_total;
        uint64_t scene_ns, draw_ns, raster_ns, submit_ns;
        bool pointer, drag, key, commit;
    } timing;
    struct animation_stats animation;
    uint64_t trail_steps;
    struct { int64_t ns[512]; unsigned head, count; } trail_times;
    char *hit_result, *frames_result, *animation_result;
    char *output_preview;
    char *output_current;
    uint64_t outputs_revision;
    uint64_t activity_serial;
    double input_at, shader_shift, shader_hold_at;
    bool shader_holding, shader_lifting;
    struct layer_plan *layer_preview;
    char *layer_preview_text;
    struct presentation *presentation;
    size_t event_count;
    size_t observation_barrier;
    struct tomoe_event *event_ring;
    size_t event_capacity, event_head;
    uint64_t event_read_mask, event_dropped, event_drop_log_ns;
    char *event_arena;
    size_t event_arena_used, event_arena_size;
    struct binding_index *binding_index;
    uint64_t policy_steps, policy_deadline_ns;
    struct motion_state motion;
    int press_world_x, press_world_y;
    bool grab_held;
    struct wl_event_source *grab_release;
    bool running, stopping, failed, configuring_outputs, scene_dirty, outputs_deferred;
    struct ui_set *ui;
    struct ui_pointer *ui_pointers;
    struct ui_asset_pool *ui_assets;
    struct window_shader *shaders;
    struct window_mesh *meshes;
    uint64_t next_ui_callback_id, ui_rasterizations;
    char *ui_stats_result;
    struct wl_list copy_frames, copy_damage, capture_sessions, cursor_sessions;
    int capture_inflight;
    struct wl_list capture_pending;
    struct wl_event_source *capture_fence_timer;
    bool capture_watch_fail, capture_watch_reported;
    bool input_stream, input_motion_pending, input_stream_armed;
    uint64_t input_motion_time;
    struct wl_event_source *input_stream_timer;
    struct wl_list relative_pointers;
    struct wl_list gestures;
    struct input_device *gesture_device;
    uint32_t gesture_kind;
    double gesture_scale;
    struct wl_list idle_notifications, idle_inhibitors;
    bool idle_inhibited;
    struct wl_list tearings;
    struct wl_list gammas, decorations, powers;
    struct popup_grab *popup_grab;
    struct constraint *active_constraint;
    struct wl_list foreigns, foreign_managers, foreign_lists;
    struct wl_resource *session_lock;
    bool lock_confirmed;
    int lock_state;
    struct node *drag_icon_tree, *lock_tree;
    struct wl_list constraints, lock_surfaces;
    struct wl_event_source *lock_deadline_source;
    struct target drag_icon;
    struct xwayland *xwayland;
    struct xwm *xwm;
};
struct layer_state {
    uint32_t anchor, width, height;
    int margin[4];
    int layer, exclusive_zone, keyboard;
    uint32_t desired_width, desired_height;
    int request_margin[4];
    int request_exclusive_zone;
    uint32_t exclusive_edge;
};
struct xdg_surface;
struct popup_grab;
struct toplevel_state {
    bool maximized, fullscreen, activated, resizing;
    int32_t width, height, min_width, min_height, max_width, max_height;
};
struct xdg_toplevel_request {
    uint32_t serial, edges;
};
struct xdg_toplevel {
    struct wl_resource *resource;
    struct xdg_surface *base;
    char *title, *app_id, *tag, *description;
    struct xdg_toplevel *parent;
    struct wl_listener parent_unmap, parent_destroy;
    struct wl_resource *dialog;
    bool modal;
    struct xwm_window *xwm;
    bool external_dialog;
    struct toplevel_state pending, current, scheduled;
    struct {
        bool maximized, fullscreen;
        struct screen *fullscreen_output;
        struct wl_listener fullscreen_output_destroy;
    } requested;
    struct surface_synced synced;
    struct {
        struct wl_signal destroy, set_title, set_app_id, request_maximize, request_fullscreen,
            request_minimize, request_move, request_resize, metadata;
    } events;
};
struct xdg_popup_state {
    struct box geometry;
    bool reactive;
};
struct xdg_popup {
    struct wl_resource *resource;
    struct xdg_surface *base;
    struct surface *parent;
    struct wl_list link, grab_link;
    bool grabbed;
    struct xdg_popup_state pending, current;
    struct {
        struct box geometry;
        struct positioner_rules rules;
        bool reposition;
        uint32_t token;
    } scheduled;
    struct surface_synced synced;
    struct { struct wl_signal destroy, reposition; } events;
};
struct xdg_surface_state {
    struct box geometry;
    uint32_t committed;
};
struct xdg_configure {
    uint32_t serial;
    struct toplevel_state toplevel;
    struct box popup_geometry;
    bool reactive;
};
#define XDG_CONFIGURES 16
struct xdg_surface {
    struct wl_resource *resource, *client, *role_resource;
    struct tomoe *server;
    struct wl_list link, popups;
    struct surface *surface;
    struct xdg_toplevel *toplevel;
    struct xdg_popup *popup;
    int role;
    struct wl_listener role_resource_destroy, scene_destroy;
    bool initialized, initial_commit, configured;
    struct box geometry;
    struct xdg_surface_state pending, current;
    struct surface_synced synced;
    struct xdg_configure configures[XDG_CONFIGURES];
    size_t configure_count;
    struct wl_event_source *configure_idle;
    uint32_t scheduled_serial;
    struct node *scene, *scene_surface;
    void *data;
    struct { struct wl_signal destroy, configure; } events;
};
bool xdg_shell_listen(struct tomoe *s);
void xdg_shell_finish(struct tomoe *s);
struct xdg_surface *xdg_surface_from_resource(struct wl_resource *resource);
struct xdg_toplevel *xdg_toplevel_from_resource(struct wl_resource *resource);
struct xdg_popup *xdg_popup_from_resource(struct wl_resource *resource);
struct xdg_surface *xdg_surface_from_surface(struct surface *surface);
struct xdg_toplevel *xdg_toplevel_from_surface(struct surface *surface);
bool xdg_surface_is_popup(struct surface *surface);
struct node *xdg_surface_scene(struct node *parent, struct xdg_surface *xdg);
uint32_t xdg_surface_schedule_configure(struct xdg_surface *xdg);
uint32_t xdg_toplevel_configure_size(struct xdg_toplevel *toplevel, int32_t width, int32_t height);
uint32_t xdg_toplevel_configure_activated(struct xdg_toplevel *toplevel, bool activated);
uint32_t xdg_toplevel_configure_maximized(struct xdg_toplevel *toplevel, bool maximized);
uint32_t xdg_toplevel_configure_fullscreen(struct xdg_toplevel *toplevel, bool fullscreen);
void xdg_toplevel_close(struct xdg_toplevel *toplevel);
void xdg_popup_dismiss(struct xdg_popup *popup);
void xdg_popup_unconstrain_from_box(struct xdg_popup *popup, const struct box *box);
struct window *xdg_toplevel_created(struct tomoe *s, struct xdg_toplevel *toplevel);
bool window_attach_surface(struct window *w, struct surface *surface);
void window_external_mapped(struct window *w);
void window_client_resized(struct window *w);
void window_external_activate(struct window *w);
void xdg_popup_created(struct tomoe *s, struct xdg_popup *popup);
enum { LAYER_STATE_SIZE = 1, LAYER_STATE_ANCHOR = 2, LAYER_STATE_ZONE = 4, LAYER_STATE_MARGIN = 8,
    LAYER_STATE_KEYBOARD = 16, LAYER_STATE_LAYER = 32 };
struct layer_surface_state {
    uint32_t committed, anchor;
    int32_t exclusive_zone;
    struct { int32_t top, right, bottom, left; } margin;
    uint32_t keyboard_interactive, desired_width, desired_height, layer, exclusive_edge;
    uint32_t actual_width, actual_height;
};
struct layer_surface {
    struct wl_resource *resource;
    struct tomoe *server;
    struct surface *surface;
    struct screen *output;
    char *namespace;
    bool initialized, initial_commit, configured;
    struct layer_surface_state current, pending;
    struct surface_synced synced;
    struct wl_list popups;
    void *data;
};
bool layer_shell_listen(struct tomoe *s);
uint32_t layer_surface_configure(struct layer_surface *ls, uint32_t width, uint32_t height);
void layer_created(struct tomoe *s, struct layer_surface *ls);
void layer_destroyed(struct layer_surface *ls);
void layer_popup_created(struct layer_surface *ls, struct xdg_popup *popup);
struct layer {
    struct target target;
    struct wl_list link;
    struct tomoe *server;
    struct layer_surface *wlr;
    struct node *tree;
    struct wl_listener commit, map, unmap;
    int override_layer, override_exclusive_zone, override_keyboard, override_visible;
    struct layer_state last;
    int last_configure_width, last_configure_height;
    bool configure_sent;
    bool mapped, announced;
    bool ready_to_configure;
};

static inline void listen(struct wl_listener *listener, struct wl_signal *signal,
        wl_notify_func_t notify) {
    listener->notify = notify;
    wl_signal_add(signal, listener);
}
static inline void detach(struct wl_listener *listener) {
    if (listener->link.next) wl_list_remove(&listener->link);
    wl_list_init(&listener->link);
}
static inline void fail(struct tomoe *s, const char *message) {
    tomoe_log(LOG_ERROR, "tomoe: %s", message);
    s->failed = true;
    s->running = false;
}

void quote(FILE *out, const char *text);
void policy_event_grab_hold(struct tomoe *s);
void tomoe_trim_heap(void);
void unmap_event(struct tomoe *s, uint32_t id);
void queue_grab_motion(struct tomoe *s, uint32_t id, int mode, int x, int y, int dx, int dy);
void grab_frame_drawn(struct tomoe *s);
bool grab_waiting(struct tomoe *s);
int tomoe_input_stream(struct tomoe *s, int enabled);
void input_stream_pointer(struct tomoe *s, uint32_t time_msec);
void input_stream_button(struct tomoe *s, uint32_t time_msec, uint32_t button, bool pressed);
void input_stream_key(struct tomoe *s, uint32_t time_msec, uint32_t keycode,
    txkb_keysym_t sym, bool pressed);
void input_stream_flush(struct tomoe *s);
void input_stream_suspend(struct tomoe *s);
bool lock_shields_input(struct tomoe *s);
uint32_t input_modifier_mask(struct tomoe *s);

struct screen *any_output(struct tomoe *s);
struct output *output_for_screen(struct tomoe *s, struct screen *screen);
void output_added(struct tomoe *s, struct screen *screen);
void outputs_request_nested_size(struct tomoe *s);
struct output_configuration_entry {
    uint64_t id;
    struct screen_state state;
    int x, y;
};
bool output_management_listen(struct tomoe *s);
void output_management_refresh(struct tomoe *s);
bool output_management_configure(struct tomoe *s,
    struct output_configuration_entry *entries, size_t count, bool test);
int tomoe_outputs_pending(struct tomoe *s);
bool presentation_outputs(struct tomoe *s, struct presentation *plan);
bool output_locations(struct tomoe *s, bool pending,
    struct output_location **locations, size_t *count);
size_t output_active_count(struct tomoe *s, bool pending);
const struct output_location *output_location_for(
    const struct output_location *locations, size_t count, struct output *output);

void presentation_finish(struct tomoe *s);
struct presentation_target *presentation_target_for(struct presentation *plan, uint32_t id);
const struct presentation_output *presentation_output_for(const struct presentation *plan,
    struct screen *output);
const struct presentation_output *presentation_output_at(const struct presentation *plan,
    double x, double y);
bool presentation_prepare(struct tomoe *s);
void presentation_publish(struct tomoe *s);

int pixel_round(double value);
double snapped_scale(double scale);
int logical_size(int physical, double scale);
int physical_size(int logical, double scale);
int physical_offset(double logical, double scale);
double reference_scale(struct tomoe *s);
struct output *output_at_physical(struct tomoe *s, double x, double y);
struct output *output_for_world(struct tomoe *s, double x, double y);
void world_to_screen(struct tomoe *s, double *x, double *y);
void screen_to_world(struct tomoe *s, double *x, double *y);
void physical_output_box(struct output *o, struct box *box);
void schedule_scene(struct tomoe *s);
void schedule_repaint(struct tomoe *s);
void refresh_scene(struct tomoe *s);
void forget_output(struct tomoe *s, struct screen *output);
bool render_output(struct output *o, struct screen_state *state);
bool render_presentation(struct output *o, struct screen_state *state,
    struct ring *ring, const struct presentation *plan);
bool capture_listen(struct tomoe *s);
bool capture_wants_cursorless(struct output *o);
bool capture_wants_output(struct output *o);
void capture_serve(struct output *o, struct buffer *committed, bool scanout);
void capture_output_gone(struct tomoe *s, struct output *o);
void capture_window_gone(struct tomoe *s, uint32_t id);
void capture_cursor_update(struct tomoe *s);
void finish_output_capture(struct output *o);
void finish_captures(struct tomoe *s);
void frame_done(struct output *o, const struct timespec *when);
void surfaces_textured(struct output *o);
struct surface *scanout_surface(struct output *o);
size_t planes_assign(struct output *o);
bool planes_attach(struct output *o, struct screen_state *state);
void planes_clear(struct output *o);
void planes_refuse(struct output *o);
void planes_finish(struct plane_assignment *planes);
bool planes_owned(const struct plane_assignment *planes, const struct surface *surface);
bool planes_geometry(const struct output *o, size_t index, struct box *dst);
bool surface_visible(struct tomoe *s, struct surface *surface);
double physical_hit_ratio(struct tomoe *s, double x, double y);
uint32_t physical_hit_test(struct tomoe *s, double x, double y,
    struct surface **surface, double *sx, double *sy);
bool scene_covers(struct tomoe *s, double x, double y, int layer);

struct surface *surface_of(struct window *w);
struct window *find_window(struct tomoe *s, uint32_t id);
const struct target *window_shown_target(struct tomoe *s, uint32_t id);
bool window_opaque(struct tomoe *s, uint32_t id);
bool window_drawn_box(struct tomoe *s, const struct presentation *plan, const struct frame *f,
    uint32_t id, struct fbox *out);
struct window *find_window_registered(struct tomoe *s, uint32_t id);
struct window *find_window_any(struct tomoe *s, uint32_t id);
void window_move_offset(struct tomoe *s, uint32_t id, double *x, double *y);
uint32_t find_window_id_for_surface(struct tomoe *s, struct surface *surface);
bool window_surface_mapped(struct tomoe *s, struct surface *surface);
void popup_create(struct xdg_popup *xdg, struct node *parent);
void windows_refresh(struct tomoe *s);
void foreign_toplevels_refresh(struct tomoe *s);
bool windows_animate(struct tomoe *s, double now);
void view_publish(struct tomoe *s, const struct presentation *plan);
bool view_animate(struct tomoe *s, const struct settings *settings, double now);
bool ghosts_animate(struct tomoe *s, double now);
bool ghosts_visible(struct tomoe *s);
void ghosts_draw(struct tomoe *s, struct frame *frame);
bool window_capture_size(struct tomoe *s, uint32_t id, int *width, int *height);
struct node *window_capture_node(struct tomoe *s, uint32_t id, struct target *target);
bool foreign_listen(struct tomoe *s);
bool foreign_announce(struct tomoe *s, uint32_t id);
void foreign_fields(struct tomoe *s, uint32_t id, const char *title, const char *app_id,
    uint32_t state);
void foreign_output(struct tomoe *s, uint32_t id, struct screen *output, bool enter);
void foreign_forget(struct tomoe *s, uint32_t id);
bool window_foreign_fields(struct tomoe *s, uint32_t id, const char **title,
    const char **app_id, uint32_t *state, struct screen **outputs, size_t capacity,
    size_t *output_count);
const char *foreign_identifier(struct tomoe *s, uint32_t id);
uint32_t foreign_handle_window(struct wl_resource *handle);
void window_foreign_request(struct tomoe *s, uint32_t id, const char *request, int requested,
    struct screen *output);
bool render_window_buffer(struct tomoe *s, uint32_t id, struct buffer *buffer, bool cursor,
    int *fence);
void window_frame_done(struct tomoe *s, uint32_t id, const struct timespec *when);
bool windows_want_tearing(struct tomoe *s, struct output *o);
void windows_prepare_presentation(struct tomoe *s, struct presentation *plan);
void windows_publish_presentation(struct tomoe *s, struct presentation *plan);

void layers_preview_finish(struct tomoe *s);
bool layers_prepare_presentation(struct tomoe *s, struct presentation *plan);
void layers_publish_presentation(struct tomoe *s);
struct layer *find_layer(struct tomoe *s, uint32_t id);
int layer_of(struct layer *l);
int keyboard_of(struct layer *l);
bool visible_of(struct layer *l);
void arrange_layers(struct tomoe *s);

int tomoe_present_keyboard(struct tomoe *s, const char *rules, const char *model,
    const char *layout, const char *variant, const char *options,
    int repeat_rate, int repeat_delay);
bool keyboard_profile_publish(struct tomoe *s, struct presentation *plan);
void keyboard_profile_finish(struct keyboard_profile *profile);
bool keyboard_logical_init(struct tomoe *s, struct txkb_keymap *keymap,
    int repeat_rate, int repeat_delay);
void keyboard_logical_finish(struct tomoe *s);
void keyboard_sync_leds(struct tomoe *s);
void update_keyboard_focus(struct tomoe *s);
void window_place_native(struct tomoe *s, uint32_t id, int x, int y, int width, int height);
bool window_box_native(struct tomoe *s, uint32_t id, int *x, int *y, int *width, int *height);
void grab_clear(struct tomoe *s);
void input_listen(struct tomoe *s);
void presentation_bindings_finish(struct presentation *plan);
void presentation_input_publish(struct tomoe *s, struct presentation *plan);
void pointer_refresh(struct tomoe *s);
bool pointer_trail_record(struct tomoe *s, uint32_t time_msec);
void ui_input_finish(struct tomoe *s);
void pointer_sync_cursors(struct tomoe *s);
bool virtual_input_listen(struct tomoe *s);
struct screen *virtual_pointer_output(struct tomoe *s, struct input_device *device);

bool activation_listen(struct tomoe *s);
void activation_surface_mapped(struct tomoe *s, struct surface *surface);
void activation_finish(struct tomoe *s);

bool protocols_listen(struct tomoe *s);
bool security_listen(struct tomoe *s);
void relative_motion_forward(struct tomoe *s, uint32_t time_msec,
    double dx, double dy, double dx_unaccel, double dy_unaccel);
void idle_notify_activity(struct tomoe *s);
void idle_refresh(struct tomoe *s);
bool idle_listen(struct tomoe *s);
bool pointer_protocols_listen(struct tomoe *s);
void gamma_apply(struct output *o, struct screen_state *state);
bool gamma_listen(struct tomoe *s);
bool decoration_listen(struct tomoe *s);
bool tearing_listen(struct tomoe *s);
bool tearing_async(struct tomoe *s, struct surface *surface);
void gamma_output_gone(struct output *o);
const char *profile_load(const char *path, size_t degamma_size, size_t gamma_size,
    struct profile **out);
struct profile *profile_ref(struct profile *profile);
void profile_unref(struct profile *profile);
uint16_t *profile_ramps(const struct profile *profile, const uint16_t *ramps, size_t size);
bool power_listen(struct tomoe *s);
bool output_power(struct output *o, bool on);
void power_output_gone(struct output *o);
void outputs_event(struct tomoe *s);
lisp_value outputs_event_value(struct tomoe *s, const struct tomoe_event *event);
void drag_icons_refresh(struct tomoe *s);
bool ime_listen(struct tomoe *s);
bool input_method_listen(struct tomoe *s);
void ime_focus(struct tomoe *s, struct surface *surface);
bool ime_key(struct input_device *device, uint32_t time, uint32_t key, uint32_t state);
void ime_keymap(struct tomoe *s);
bool ime_modifiers(struct tomoe *s, const struct keyboard_modifiers *modifiers);
bool ime_virtual_modifiers(struct input_device *device, const struct keyboard_modifiers *mods);
bool shortcuts_listen(struct tomoe *s);
void shortcuts_focus(struct tomoe *s, struct surface *surface);
bool shortcuts_inhibited(struct tomoe *s);
void constraint_focus(struct tomoe *s, struct surface *surface, double sx, double sy);
void constraint_refresh(struct tomoe *s);
bool constraint_allows(struct tomoe *s, double x, double y);

bool lock_listen(struct tomoe *s);
void lock_finish(struct tomoe *s);
void lock_refresh(struct tomoe *s);
void lock_frame_rendered(struct tomoe *s, struct screen *output);
void input_lock_begin(struct tomoe *s);
bool lock_active(struct tomoe *s);
bool render_output_buffer(struct output *o, struct buffer *buffer);
bool render_displayed_buffer(struct output *o, struct buffer *buffer);
struct screenshot;
bool ui_text_raster(uint32_t *pixels, int stride, int width, int height, struct rect clip,
    double x, double y, const char *font, const char *text, double size, uint32_t rgba);
void screenshot_key_up(struct tomoe *s, uint32_t keycode);
bool screenshot_key(struct tomoe *s, txkb_keysym_t sym, txkb_keysym_t typed,
    uint32_t modifiers, bool pressed, uint32_t keycode);
bool screenshot_button(struct tomoe *s, uint32_t button, bool pressed);
void screenshot_motion(struct tomoe *s);
bool screenshot_render_frozen(struct output *o, struct frame *f);
bool screenshot_on(const struct output *o);
bool screenshot_marks(const struct output *o, struct texture **texture, struct fbox *box);
bool screenshot_selection(const struct output *o, struct fbox area, double now, float out[16]);
bool screenshot_animate(struct tomoe *s, double now);
void screenshot_output_gone(struct tomoe *s, struct output *o);
void screenshot_finish(struct tomoe *s);
struct surface *lock_keyboard_surface(struct tomoe *s);

struct seat *seat_create(struct tomoe *s);
void seat_destroy(struct seat *seat);
struct seat_client *seat_client_for(struct seat *seat, struct wl_client *client);
struct seat_client *seat_client_from_resource(struct wl_resource *resource);
void seat_set_capabilities(struct seat *seat, uint32_t capabilities);
void seat_set_keyboard(struct seat *seat, struct keymap_slot *keyboard);
struct keymap_slot *seat_get_keyboard(struct seat *seat);
void seat_keyboard_keymap_changed(struct seat *seat);
void seat_pointer_enter(struct seat *seat, struct surface *surface, double sx, double sy);
void seat_pointer_clear_focus(struct seat *seat);
void seat_pointer_send_motion(struct seat *seat, uint32_t time, double sx, double sy);
uint32_t seat_pointer_send_button(struct seat *seat, uint32_t time, uint32_t button,
    uint32_t state);
void seat_pointer_send_axis(struct seat *seat, uint32_t time, uint32_t orientation, double value,
    int32_t discrete, uint32_t source, uint32_t direction);
void seat_pointer_send_frame(struct seat *seat);
void seat_pointer_start_grab(struct seat *seat, struct seat_pointer_grab *grab);
void seat_pointer_end_grab(struct seat *seat);
void seat_pointer_notify_enter(struct seat *seat, struct surface *surface, double sx,
    double sy);
void seat_pointer_notify_clear_focus(struct seat *seat);
void seat_pointer_notify_motion(struct seat *seat, uint32_t time, double sx, double sy);
uint32_t seat_pointer_notify_button(struct seat *seat, uint32_t time, uint32_t button,
    uint32_t state);
void seat_pointer_notify_axis(struct seat *seat, uint32_t time, uint32_t orientation, double value,
    int32_t discrete, uint32_t source, uint32_t direction);
void seat_pointer_notify_frame(struct seat *seat);
bool seat_validate_pointer_grab_serial(struct seat *seat, struct surface *origin,
    uint32_t serial);
void seat_keyboard_enter(struct seat *seat, struct surface *surface, const uint32_t keys[],
    size_t count, const struct keyboard_modifiers *modifiers);
void seat_keyboard_clear_focus(struct seat *seat);
void seat_keyboard_send_key(struct seat *seat, uint32_t time, uint32_t key, uint32_t state);
void seat_keyboard_send_modifiers(struct seat *seat, const struct keyboard_modifiers *modifiers);
void seat_keyboard_start_grab(struct seat *seat, struct seat_keyboard_grab *grab);
void seat_keyboard_end_grab(struct seat *seat);
void seat_keyboard_notify_enter(struct seat *seat, struct surface *surface,
    const uint32_t keys[], size_t count, const struct keyboard_modifiers *modifiers);
void seat_keyboard_notify_clear_focus(struct seat *seat);
void seat_keyboard_notify_key(struct seat *seat, uint32_t time, uint32_t key, uint32_t state);
void seat_keyboard_notify_modifiers(struct seat *seat,
    const struct keyboard_modifiers *modifiers);
size_t keyboard_pressed(struct tomoe *s, uint32_t *keys);
void cursor_requested(struct tomoe *s, struct surface *surface, int32_t x, int32_t y);
void cursor_committed(struct tomoe *s, struct surface *surface);
void cursor_default(struct tomoe *s);
void cursor_shape_set(struct tomoe *s, uint32_t shape);
bool cursor_shape_listen(struct tomoe *s);
enum { GESTURE_SWIPE = 1, GESTURE_PINCH, GESTURE_HOLD };
bool gestures_listen(struct tomoe *s);
void pointer_gesture_begin(struct tomoe *s, struct input_device *device, uint32_t kind,
    uint32_t time, uint32_t fingers);
void pointer_gesture_update(struct tomoe *s, struct input_device *device, uint32_t kind,
    uint32_t time, double dx, double dy, double scale, double rotation);
void pointer_gesture_end(struct tomoe *s, struct input_device *device, uint32_t kind,
    uint32_t time, int cancelled);
void pointer_gesture_cancel(struct tomoe *s);
bool selection_listen(struct tomoe *s);
void screen_state_init(struct screen_state *state);
void screen_state_finish(struct screen_state *state);
bool screen_state_copy(struct screen_state *dst, const struct screen_state *src);
void screen_state_set_enabled(struct screen_state *state, bool enabled);
void screen_state_set_mode(struct screen_state *state, struct screen_mode *mode);
void screen_state_set_custom_mode(struct screen_state *state, int32_t width, int32_t height,
    int32_t refresh);
void screen_state_set_scale(struct screen_state *state, float scale);
void screen_state_set_transform(struct screen_state *state, enum wl_output_transform transform);
void screen_state_set_adaptive_sync_enabled(struct screen_state *state, bool enabled);
void screen_state_set_buffer(struct screen_state *state, struct buffer *buffer);
bool screen_state_set_overlay(struct screen_state *state, size_t index, struct buffer *buffer,
    int x, int y, int width, int height, int src_x, int src_y, int src_width, int src_height,
    bool underlay);
void screen_state_clear_overlays(struct screen_state *state);
void screen_state_set_overlay_wait(struct screen_state *state, size_t index,
    struct timeline *timeline, uint64_t point);
void screen_state_set_wait_timeline(struct screen_state *state,
    struct timeline *timeline, uint64_t point);
bool screen_state_set_gamma(struct screen_state *state, const uint16_t *ramps, size_t size);
void screen_state_set_profile(struct screen_state *state, const struct profile *profile);
void screen_init(struct screen *screen, struct tomoe *s, const struct screen_impl *impl,
    enum screen_kind kind, const char *name);
void screen_describe(struct screen *screen);
struct screen_mode *screen_add_mode(struct screen *screen, int32_t width, int32_t height,
    int32_t refresh, bool preferred);
void screen_destroy(struct screen *screen);
struct screen *screen_from_resource(struct wl_resource *resource);
struct screen_mode *screen_preferred_mode(struct screen *screen);
void screen_transformed_resolution(struct screen *screen, int *width, int *height);
void screen_effective_resolution(struct screen *screen, int *width, int *height);
bool screen_test(struct screen *screen, const struct screen_state *state);
bool screen_commit(struct screen *screen, const struct screen_state *state);
bool screens_test(struct screen_update *updates, size_t count);
bool screens_commit(struct screen_update *updates, size_t count);
const struct format_set *screen_primary_formats(struct screen *screen);
const struct format_set *screen_overlay_formats(struct screen *screen);
dev_t screen_device(struct screen *screen);
size_t screen_gamma_size(struct screen *screen);
size_t screen_degamma_size(struct screen *screen);
void screen_schedule_frame(struct screen *screen);
void screen_schedule_animation(struct screen *screen);
double shader_clock_now(const struct tomoe *s, double now);
void screen_send_frame(struct screen *screen);
void screen_frame_committed(struct screen *screen, const struct screen_state *state);
void screens_frame_after_events(struct tomoe *s);
void screens_idle_frames(struct tomoe *s);
void screens_grab_waiting(struct tomoe *s);
bool screen_frames_late(struct screen *screen);
void screen_send_present(struct screen *screen, struct screen_present *present);
void screen_request_state(struct screen *screen, struct screen_state *state);
void screen_set_position(struct screen *screen, int lx, int ly);
void screen_cursor_move(struct screen *screen, double x, double y);
int64_t screen_next_vblank(const struct screen *screen, int64_t after);
uint32_t screen_cost_heavy(const struct screen *screen);
int64_t monotonic_ns(void);
bool ns_timer_init(struct ns_timer *timer, struct wl_event_loop *loop, void (*fire)(void *data),
    void *data);
void ns_timer_arm(struct ns_timer *timer, int64_t at);
void ns_timer_finish(struct ns_timer *timer);
void screen_lock_software_cursors(struct screen *screen, bool lock);
bool screens_listen(struct tomoe *s);
void cursor_show(struct tomoe *s, struct buffer *buffer, int hotspot_x, int hotspot_y,
    float scale);
void cursor_finish(struct tomoe *s);
void cursor_theme_unwatch(struct tomoe *s);
bool cursor_motion_advance(struct tomoe *s, double now);
bool cursor_motion_rest(const struct tomoe *s);
void cursor_glass_track(struct tomoe *s);
void cursor_glass_image(struct tomoe *s);
void cursor_glass_settings(struct tomoe *s);
void cursor_glass_release(struct tomoe *s);
bool cursor_glass_holds(const struct tomoe *s, const struct screen *screen);
void cursor_glass_finish(struct tomoe *s);
void cursor_motion_matrix(const struct tomoe *s, double matrix[4], double offset[2]);
void cursor_motion_apply(struct tomoe *s, const double matrix[4], const double offset[2],
    double settle);
bool cursor_trail_style(const struct tomoe *s, struct trail_style *style);
void cursor_trail_settings(struct tomoe *s);
void cursor_effects_register_primitives(struct tomoe *server);
void cursor_effects_evaluate(struct tomoe *s, double now);
int tomoe_cursor_apply(struct tomoe *s, double a, double b, double c, double d,
    double offset_x, double offset_y, double settle);
const char *tomoe_cursor_motion(struct tomoe *s);
const char *cursor_shape_name(uint32_t shape);
void screens_cursor_refresh(struct tomoe *s);
struct buffer *pixel_buffer_create(int width, int height, size_t stride, uint32_t format,
    const void *pixels);
bool session_create(struct tomoe *s);
void session_finish(struct tomoe *s);
const char *session_seat_name(struct tomoe *s);
bool session_active(struct tomoe *s);
int session_open(struct tomoe *s, const char *path, int *device);
void session_close(struct tomoe *s, int device, int fd);
void session_change_vt(struct tomoe *s, int vt);
int kms_create(struct tomoe *s);
void kms_start(struct tomoe *s);
void kms_pause(struct tomoe *s);
void kms_resume(struct tomoe *s);
void kms_destroy(struct tomoe *s);
int nested_create(struct tomoe *s);
bool nested_start(struct tomoe *s);
void nested_finish(struct tomoe *s);
bool headless_start(struct tomoe *s);
void headless_finish(struct tomoe *s);
void libinput_active(struct tomoe *s, bool active);

bool buffers_listen(struct tomoe *s);
void dmabuf_surface_feedback(struct surface *surface, struct screen *output, uint8_t mode);
void dmabuf_surface_destroy(struct surface *surface);
bool surfaces_listen(struct tomoe *s);
void surfaces_finish(struct tomoe *s);
void surfaces_fifo_frame(struct screen *screen);
void surfaces_fifo_commit(struct screen *screen);
void surface_fifo_unpaced(struct surface *surface);
struct surface *surface_from_resource(struct wl_resource *resource);
struct region *region_from_resource(struct wl_resource *resource);
bool surface_has_buffer(struct surface *surface);
bool alpha_modifier_listen(struct tomoe *s);
bool content_type_listen(struct tomoe *s);
bool surface_state_has_buffer(const struct surface_state *state);
bool surface_synced_init(struct surface_synced *synced, struct surface *surface,
    const struct surface_synced_impl *impl, void *pending, void *current);
void surface_synced_finish(struct surface_synced *synced);
void surface_map(struct surface *surface);
void surface_unmap(struct surface *surface);
void surface_reject_pending(struct surface *surface, struct wl_resource *resource, uint32_t code,
    const char *message);
bool surface_set_role(struct surface *surface, const struct surface_role *role,
    struct wl_resource *error_resource, uint32_t error_code);
void surface_set_role_object(struct surface *surface, struct wl_resource *resource);
struct surface *surface_root(struct surface *surface);
void surface_extents(struct surface *surface, struct box *box);
bool surface_walk(struct surface *surface, int x, int y, bool reverse, surface_iterator iterator,
    void *data);
void surface_source_box(struct surface *surface, struct fbox *box);
bool surface_accepts_input(struct surface *surface, double sx, double sy);
void surface_send_enter(struct surface *surface, struct screen *output);
void surface_send_leave(struct surface *surface, struct screen *output);
void surface_leave_all(struct surface *surface);
void surface_frame_done(struct surface *surface, const struct timespec *when);
void feedback_rebase(struct screen *screen, size_t from, size_t to);
bool surfaces_fifo_waiting(struct screen *screen);
void surface_set_scale(struct surface *surface, double scale);
void surface_presented(struct surface *surface, struct screen *output, bool zero_copy);
void surface_release_after(struct surface *surface, struct buffer *consumer);
struct node *node_create(struct node *parent);
struct node *node_surface_create(struct node *parent, struct surface *surface);
void node_destroy(struct node *node);
void node_set_enabled(struct node *node, bool enabled);
void node_set_position(struct node *node, int x, int y);
void node_raise_to_top(struct node *node);
void node_reparent(struct node *node, struct node *parent);
void buffers_finish(void);
void seat_selection_focus(struct seat *seat, struct seat_client *client);
void seat_selection_finish(struct seat *seat);
void seat_drag_client_gone(struct seat *seat, struct seat_client *client);
void input_add(struct tomoe *s, struct input_device *device);

bool xwayland_listen(struct tomoe *s);
int xwayland_start(struct tomoe *s);
void xwayland_pair_window(struct tomoe *s, uint32_t xid, uint64_t serial);
bool xwm_start(struct tomoe *s, int fd);
void xwm_stop(struct tomoe *s);
void xwayland_finish(struct tomoe *s);
void xwayland_pair_surface(struct tomoe *s, uint64_t serial, struct surface *surface);
void xwm_associate(struct tomoe *s, uint32_t xid, struct surface *surface);
void xwm_client_gone(struct tomoe *s);
void xwm_configure(struct xdg_toplevel *toplevel, int width, int height);
void xwm_place(struct xdg_toplevel *toplevel, int x, int y);
void xwm_activate(struct xdg_toplevel *toplevel, bool activated);
void xwm_state(struct xdg_toplevel *toplevel, int fullscreen, int maximized);
void xwm_close(struct xdg_toplevel *toplevel);
void xwm_schedule(struct xdg_toplevel *toplevel);
void xwm_wayland_selection(struct tomoe *s, bool primary, bool present);
void xwm_source_send(struct tomoe *s, bool primary, const char *mime, int fd);
bool selection_source_info(struct tomoe *s, bool primary, bool *x11,
    const char *const **mimes, size_t *count);
bool selection_source_send(struct tomoe *s, bool primary, const char *mime, int fd);
bool selection_x11_offer(struct tomoe *s, bool primary, const char *const *mimes, size_t count);
void selection_x11_clear(struct tomoe *s, bool primary);

#endif
