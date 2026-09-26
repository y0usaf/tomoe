#ifndef TOMOE_INTERNAL_H
#define TOMOE_INTERNAL_H
#include "backend.h"
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <GLES2/gl2.h>
#include <wayland-server-core.h>
#include <drm_fourcc.h>
#include <pixman.h>
#include <xkbcommon/xkbcommon.h>
#include "wlr-layer-shell-unstable-v1-protocol.h"
#include "xdg-shell-protocol.h"

enum { LOG_ERROR = 1, LOG_INFO, LOG_DEBUG };
extern int log_verbosity;
void tomoe_log(int level, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

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
void region_scale(pixman_region32_t *dst, const pixman_region32_t *src, float scale);
void region_scale_xy(pixman_region32_t *dst, const pixman_region32_t *src, float sx, float sy);
void region_transform(pixman_region32_t *dst, const pixman_region32_t *src,
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
    bool reactive, has_parent_configure_serial;
    uint32_t parent_configure_serial;
    struct { int32_t width, height; } size, parent_size;
    struct { int32_t x, y; } offset;
};
void positioner_geometry(const struct positioner_rules *rules, struct box *box);
void positioner_unconstrain(const struct positioner_rules *rules, const struct box *constraint,
    struct box *box);
struct buffer *xcursor_load(const char *name, float scale, int *hotspot_x, int *hotspot_y);

struct render;
struct texture { uint32_t width, height; uint64_t serial; };
enum blend_mode { BLEND_PREMULTIPLIED, BLEND_NONE };
enum filter_mode { FILTER_BILINEAR, FILTER_NEAREST };
struct color { float r, g, b, a; };
struct texture_options {
    struct texture *texture;
    struct fbox src_box;
    struct box dst_box;
    const float *alpha;
    const pixman_region32_t *clip;
    enum wl_output_transform transform;
    enum filter_mode filter_mode;
    enum blend_mode blend_mode;
    struct timeline *wait_timeline;
    uint64_t wait_point;
    const struct surface *surface;
};
struct rect_options {
    struct box box;
    struct color color;
    const pixman_region32_t *clip;
    enum blend_mode blend_mode;
};
struct read_options {
    void *data;
    uint32_t format, stride;
    struct box src_box;
};
struct pass;
struct op {
    uint64_t hash, seq;
    const struct surface *surface;
    struct box box, reach;
    struct fbox src;
    bool mapped;
};
struct oplist { struct op *ops; size_t len, cap; bool lost; };
void oplist_finish(struct oplist *list);
bool oplist_damage(const struct oplist *old, const struct oplist *now, pixman_region32_t *out);
void oplist_expand(const struct oplist *list, pixman_region32_t *region);
void render_destroy(struct render *r);
int render_drm_fd(struct render *r);
bool render_has_timeline(struct render *r);
void render_usage(struct render *r, size_t *textures, uint64_t *texture_bytes, size_t *images);
const struct format_set *render_texture_formats(struct render *r);
const struct format_set *render_shm_formats(struct render *r);
struct buffer *render_allocate(struct render *r, int width, int height,
    const struct format *format);
struct texture *texture_from_buffer(struct render *r, struct buffer *buffer);
struct texture *texture_from_pixels(struct render *r, uint32_t format, uint32_t stride,
    uint32_t width, uint32_t height, const void *data);
bool texture_update(struct texture *texture, struct buffer *buffer,
    const pixman_region32_t *damage);
bool texture_read_pixels(struct texture *texture, const struct read_options *options);
void texture_destroy(struct texture *texture);
struct pass *render_begin(struct render *r, struct buffer *buffer, struct timeline *signal,
    uint64_t point);
void pass_add_texture(struct pass *pass, const struct texture_options *options);
void pass_add_rect(struct pass *pass, const struct rect_options *options);
bool pass_submit(struct pass *pass);
struct pass *render_record(struct render *r, struct buffer *buffer, struct oplist *list);
bool pass_record(struct pass *pass, const struct texture_options *texture, struct box box,
    struct box reach, const double *params, size_t count);
void pass_clip(struct pass *pass, const pixman_region32_t *clip);
bool pass_touches(struct pass *pass, struct box box);

struct screen;

struct event;
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
    struct xkb_keymap *keymap;
    struct xkb_state *xkb_state;
    char *keymap_string;
    size_t keymap_size;
    int keymap_fd;
    xkb_led_index_t led_indexes[LED_COUNT];
    xkb_mod_index_t mod_indexes[MODIFIER_COUNT];
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
enum { PROGRAM_RECT, PROGRAM_SDF, PROGRAM_TEXTURE, PROGRAM_EXTERNAL, PROGRAM_DOWN, PROGRAM_UP,
    PROGRAM_COUNT };
struct program {
    GLuint id;
    GLint pos, local, texcoord;
    GLint tex, alpha, opaque, size, radius, clip, width, kind, color, range, power, half_pixel, offset;
    GLint resolution, time, frame;
};
struct program *render_shader_create(struct render *r, const char *source);
void render_shader_destroy(struct program *program);
#define RING_SLOTS 4
struct ring {
    struct buffer *slots[RING_SLOTS];
    uint64_t frames[RING_SLOTS];
    int width, height;
    bool implicit;
    struct format_set format;
};
struct render *render_create(int drm_fd);
struct program *render_program(struct render *renderer, int kind);
void pass_quad(struct pass *pass, struct program *p, const float pos[8], const float local[8],
    const float texcoords[8]);
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
void ring_finish(struct ring *ring);
bool fenced(struct screen *output);

struct screen;
struct session;
struct kms;
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
    SCREEN_WAIT = 1 << 6, SCREEN_GAMMA = 1 << 7,
};
enum screen_mode_type { SCREEN_MODE_FIXED, SCREEN_MODE_CUSTOM };
enum screen_kind { SCREEN_DRM, SCREEN_NESTED, SCREEN_HEADLESS };
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
};
struct screen_update {
    struct screen *output;
    struct screen_state base;
};
struct screen_impl {
    bool (*test)(struct screen_update *updates, size_t count);
    bool (*commit)(struct screen_update *updates, size_t count);
    const struct format_set *(*formats)(struct screen *screen);
    size_t (*gamma_size)(struct screen *screen);
    bool (*cursor)(struct screen *screen, struct buffer *buffer, int hotspot_x, int hotspot_y);
    void (*move_cursor)(struct screen *screen, int x, int y);
    void (*destroy)(struct screen *screen);
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
    bool enabled, adaptive_sync_supported, adaptive_sync, hardware_cursor;
    bool frame_pending, power_off;
    int lx, ly, software_cursor_locks;
    double cursor_x, cursor_y;
    size_t commit_seq;
    struct wl_event_source *idle_frame;
    struct wl_global *global;
    struct wl_list resources, xdg_resources;
    struct {
        struct wl_signal frame, needs_frame, present, request_state, commit, destroy, bind;
    } events;
    void *data;
};

struct output {
    struct wl_list link;
    struct tomoe *server;
    struct screen *screen;
    struct wl_listener frame, request, destroy, needs_frame;
    struct screen_state initial, pending;
    struct screen_state deferred;
    bool configured, admitted, pending_configured, pending_positioned;
    bool request_pending, pending_hold;
    uint64_t id;
    uint64_t request_id;
    int pending_x, pending_y;
    int x, y;
    bool positioned;
    char mirror[129];
    char pending_mirror[129];
    struct buffer *capture_buffer;
    struct buffer *presented[2];
    struct ring ring;
    bool lock_rendered, gamma_dirty;
    struct surface *scanout;
    struct oplist ops;
    uint64_t frames;
    pixman_region32_t damage[8];
    struct { uint64_t frame, age; int64_t pixels; size_t ops; } drawn;
    struct { uint64_t ns, counts[8]; } timing;
};

static inline bool output_is_active(const struct output *output) {
    return output && output->admitted && output->screen && output->screen->enabled;
}

enum target_kind { TARGET_WINDOW, TARGET_LAYER, TARGET_UNMANAGED, TARGET_ICON };
struct window_style {
    int radius, blur, tearing;
    int64_t focused, unfocused;
};
struct target {
    uint32_t id;
    enum target_kind kind;
    int x, y, geometry_x, geometry_y;
    double scale;
    struct screen *output;
    int client_width, client_height;
    bool fullscreen;
    double offset_x, offset_y, alpha;
    struct window_style style;
};
struct frame {
    struct tomoe *server;
    struct pass *pass;
    struct buffer *buffer;
    int x, y, width, height;
    enum wl_output_transform transform;
    double view_x, view_y, zoom;
    uint32_t focused;
};
struct effects;
void effect_border(struct frame *f, struct fbox geometry, double width, double radius,
    uint32_t rgba, float alpha);
void effect_shadow(struct frame *f, struct fbox geometry, double range, double radius,
    uint32_t rgba, double power, float alpha);
bool effect_texture(struct frame *f, const struct texture_options *options,
    struct fbox dst, struct fbox clip, double radius);
void effect_blur(struct frame *f, struct fbox area, double radius, int passes,
    double offset, int margin);
void effect_shader(struct frame *f, struct program *p, struct fbox area, double time, int frame);
void effects_finish(struct tomoe *s);
bool background_effects_listen(struct tomoe *s);
const pixman_region32_t *background_blur_region(struct tomoe *s, struct surface *surface);
struct presentation_output {
    struct output *output;
    struct box box;
    int scale_120;
};
struct output_location {
    struct output *output;
    int x, y, width, height, scale_120;
    bool active, mirrored;
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
enum animation_kind { ANIMATION_OFF, ANIMATION_SPRING, ANIMATION_EASE };
struct animation_config {
    int kind, duration_ms, curve;
    double damping_ratio, stiffness, epsilon, bezier[4];
};
struct animation {
    double from, to, start, duration;
    struct animation_config config;
    bool active;
};
double animation_now(void);
void animation_start(struct animation *a, const struct animation_config *config,
    double from, double to, double now);
double animation_value(struct animation *a, double now);
struct input_config {
    double disabled, disabled_on_external_mouse, tap, tap_drag, tap_drag_lock, natural_scroll;
    double accel_speed, accel_profile, dwt, left_handed, middle_emulation;
    double scroll_method, scroll_button, click_method;
};
struct named_input_config {
    char *name;
    struct input_config config;
};
struct settings {
    struct input_config touchpad, mouse;
    struct named_input_config devices[64];
    size_t device_count;
    bool force_ssd, honor_invalid_serial, tearing, wait_frame;
    int nested_width, nested_height;
    int border_width, border_radius;
    uint32_t border_focused, border_unfocused;
    int shadow_range;
    uint32_t shadow_color;
    double shadow_power;
    bool blur_enabled;
    int blur_passes, blur_margin;
    double blur_offset;
    bool screenshot_freeze;
    struct animation_config window_move, window_open;
    char *blur_namespaces[64];
    size_t blur_namespace_count;
};
void settings_default(struct settings *settings);
void settings_finish(struct settings *settings);
void input_config_unset(struct input_config *config);
int input_setting(struct settings *settings, const char *key, double value, const char *text);
void input_devices_apply(struct tomoe *s);
bool libinput_listen(struct tomoe *s);
void libinput_finish(struct tomoe *s);
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
bool keymap_slot_set(struct keymap_slot *slot, struct xkb_keymap *keymap);
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
    bool restack, explicit_stacking, outputs_changed, replace_bindings;
    struct ui_set *ui;
    struct settings *settings;
    char *grab_owner, *grab_otherwise;
    uint64_t grab_source;
    bool grab_staged;
};
void settings_publish(struct tomoe *s, struct presentation *plan);
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
    struct buffer *buffer;
    int dx, dy, width, height, buffer_width, buffer_height, scale;
    enum wl_output_transform transform;
    pixman_region32_t surface_damage, buffer_damage, input;
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
    struct texture *texture;
    struct buffer *buffer;
    struct release *release;
    pixman_region32_t input_region;
    const struct surface_role *role;
    struct wl_resource *role_resource, *viewport, *fractional, *syncobj;
    struct wl_listener role_resource_destroy;
    struct subsurface *subsurface;
    struct screen *primary;
    uint64_t commit_seq;
    struct { uint64_t seq, prev; struct box box; } commits[4];
    int preferred_scale;
    uint32_t fractional_scale;
    bool mapped, pending_rejected, sent_transform, seen, scene_owned;
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

struct tomoe {
    struct wl_display *display;
    struct settings settings;
    struct effects *effects;
    struct screenshot *screenshot;
    struct session *session;
    struct kms *kms;
    enum screen_kind backend;
    struct render *renderer;
    struct node *scene;
    struct node *window_tree, *layer_tree[4], *fullscreen_tree;
    struct cursor_image cursor_image;
    struct buffer *default_cursor;
    float default_cursor_scale;
    int default_hotspot_x, default_hotspot_y;
    struct seat *seat;
    struct logical_keyboard *logical_keyboard;
    struct keyboard_profile *keyboard_profile;
    struct wl_list input_devices, background_effects;
    struct wl_list surfaces, feedbacks;
    struct wl_list windows, layers, outputs, keyboards, events, bindings, virtual_pointers;
    struct wl_list activation_tokens;
    size_t activation_tracked_count;
    size_t activation_token_count;
    struct wl_list activation_pending;
    size_t activation_pending_count;
    struct wl_listener new_input;
    struct libinput *libinput;
    struct wl_event_source *libinput_source;
    struct wl_list libinput_fds;

    char *last_event;
    uint32_t next_id, focused, grab_id;
    uint64_t next_binding_id, next_device_id, next_output_id;
    uint32_t latest_keyboard_enter_serial, latest_pointer_enter_serial;
    bool have_keyboard_enter_serial, have_pointer_enter_serial;
    int grab_mode;
    double grab_x, grab_y;
    double pointer_x, pointer_y;
    struct surface *cursor_surface;
    int32_t cursor_hotspot_x, cursor_hotspot_y;
    bool cursor_hidden;
    uint32_t hovered;
    char *ui_hovered;
    struct pointer_latch pointer_latches[32];
    char *grab_owner, *grab_otherwise;
    uint64_t grab_source;
    size_t pointer_latch_count;
    struct timeline *render_timeline;
    uint64_t render_point;
    struct wl_listener cursor_surface_destroy;
    int view_x, view_y;
    double view_zoom;
    char *hit_result, *frames_result, *memory_result;
    char *output_preview;
    char *output_current;
    uint64_t outputs_revision;
    struct layer_plan *layer_preview;
    char *layer_preview_text;
    struct presentation *presentation;
    size_t event_count;
    size_t observation_barrier;
    bool running, stopping, failed, configuring_outputs, scene_dirty;
    struct ui_set *ui;
    struct ui_pointer *ui_pointers;
    struct ui_asset_pool *ui_assets;
    uint64_t next_ui_callback_id, ui_rasterizations;
    char *ui_stats_result;
    struct wl_list copy_frames, capture_sessions;
    struct wl_list relative_pointers;
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
    char *title, *app_id;
    struct toplevel_state pending, current, scheduled;
    struct {
        bool maximized, fullscreen, minimized;
        struct screen *fullscreen_output;
        struct wl_listener fullscreen_output_destroy;
    } requested;
    struct surface_synced synced;
    struct {
        struct wl_signal destroy, set_title, set_app_id, request_maximize, request_fullscreen,
            request_minimize, request_move, request_resize;
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
    uint32_t configure_serial, committed;
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
struct node *xdg_surface_scene(struct node *parent, struct xdg_surface *xdg);
uint32_t xdg_surface_schedule_configure(struct xdg_surface *xdg);
uint32_t xdg_toplevel_configure_size(struct xdg_toplevel *toplevel, int32_t width, int32_t height);
uint32_t xdg_toplevel_configure_activated(struct xdg_toplevel *toplevel, bool activated);
uint32_t xdg_toplevel_configure_maximized(struct xdg_toplevel *toplevel, bool maximized);
uint32_t xdg_toplevel_configure_fullscreen(struct xdg_toplevel *toplevel, bool fullscreen);
void xdg_toplevel_close(struct xdg_toplevel *toplevel);
void xdg_popup_dismiss(struct xdg_popup *popup);
void xdg_popup_unconstrain_from_box(struct xdg_popup *popup, const struct box *box);
void xdg_toplevel_created(struct tomoe *s, struct xdg_toplevel *toplevel);
void xdg_popup_created(struct tomoe *s, struct xdg_popup *popup);
enum { LAYER_STATE_SIZE = 1, LAYER_STATE_ANCHOR = 2, LAYER_STATE_ZONE = 4, LAYER_STATE_MARGIN = 8,
    LAYER_STATE_KEYBOARD = 16, LAYER_STATE_LAYER = 32 };
struct layer_surface_state {
    uint32_t committed, anchor;
    int32_t exclusive_zone;
    struct { int32_t top, right, bottom, left; } margin;
    uint32_t keyboard_interactive, desired_width, desired_height, layer, exclusive_edge;
    uint32_t actual_width, actual_height, configure_serial;
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
FILE *begin_event(struct tomoe *s, struct event **event, size_t *size);
void end_event(struct tomoe *s, struct event *event, FILE *out);
void unmap_event(struct tomoe *s, uint32_t id);

struct screen *any_output(struct tomoe *s);
void output_added(struct tomoe *s, struct screen *screen);
void outputs_request_nested_size(struct tomoe *s);
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
void screen_to_protocol(struct tomoe *s, double *x, double *y);
void physical_output_box(struct output *o, struct box *box);
void schedule_scene(struct tomoe *s);
void refresh_scene(struct tomoe *s);
void forget_output(struct tomoe *s, struct screen *output);
bool render_output(struct output *o, struct screen_state *state);
bool render_presentation(struct output *o, struct screen_state *state,
    struct ring *ring, const struct presentation *plan);
bool capture_listen(struct tomoe *s);
bool capture_wants_cursorless(struct output *o);
void capture_serve(struct output *o, struct buffer *committed, bool scanout);
void capture_output_gone(struct tomoe *s, struct output *o);
void capture_window_gone(struct tomoe *s, uint32_t id);
void finish_output_capture(struct output *o);
void finish_captures(struct tomoe *s);
void frame_done(struct output *o, const struct timespec *when);
void surfaces_textured(struct output *o);
struct surface *scanout_surface(struct output *o);
bool surface_visible(struct tomoe *s, struct surface *surface);
double physical_hit_ratio(struct tomoe *s, double x, double y);
uint32_t physical_hit_test(struct tomoe *s, double x, double y,
    struct surface **surface, double *sx, double *sy);
bool scene_covers(struct tomoe *s, double x, double y, int layer);

struct surface *surface_of(struct window *w);
struct window *find_window(struct tomoe *s, uint32_t id);
struct window *find_window_registered(struct tomoe *s, uint32_t id);
struct window *find_window_any(struct tomoe *s, uint32_t id);
uint32_t find_window_id_for_surface(struct tomoe *s, struct surface *surface);
bool window_surface_mapped(struct tomoe *s, struct surface *surface);
void popup_create(struct xdg_popup *xdg, struct node *parent);
void windows_refresh(struct tomoe *s);
void foreign_toplevels_refresh(struct tomoe *s);
bool windows_animate(struct tomoe *s);
bool window_capture_size(struct tomoe *s, uint32_t id, int *width, int *height);
struct node *window_capture_node(struct tomoe *s, uint32_t id, struct target *target);
bool foreign_listen(struct tomoe *s);
void foreign_update(struct tomoe *s, uint32_t id, const char *title, const char *app_id,
    uint32_t state, struct screen *const *outputs, size_t output_count);
void foreign_forget(struct tomoe *s, uint32_t id);
const char *foreign_identifier(struct tomoe *s, uint32_t id);
uint32_t foreign_handle_window(struct wl_resource *handle);
void window_foreign_request(struct tomoe *s, uint32_t id, const char *request, int requested,
    struct screen *output);
bool render_window_buffer(struct tomoe *s, uint32_t id, struct buffer *buffer);
bool windows_want_tearing(struct tomoe *s, struct output *o);
void windows_prepare_presentation(struct tomoe *s, struct presentation *plan);
void windows_publish_presentation(struct tomoe *s, struct presentation *plan);

void layers_preview_finish(struct tomoe *s);
bool layers_prepare_presentation(struct tomoe *s, struct presentation *plan);
void layers_publish_presentation(struct tomoe *s);
struct layer *find_layer(struct tomoe *s, uint32_t id);
int layer_of(struct layer *l);
int exclusive_zone_of(struct layer *l);
int keyboard_of(struct layer *l);
bool visible_of(struct layer *l);
void arrange_layers(struct tomoe *s);

int tomoe_present_keyboard(struct tomoe *s, const char *rules, const char *model,
    const char *layout, const char *variant, const char *options,
    int repeat_rate, int repeat_delay);
bool keyboard_profile_publish(struct tomoe *s, struct presentation *plan);
void keyboard_profile_finish(struct keyboard_profile *profile);
bool keyboard_logical_init(struct tomoe *s, struct xkb_keymap *keymap,
    int repeat_rate, int repeat_delay);
void keyboard_logical_finish(struct tomoe *s);
void keyboard_sync_leds(struct tomoe *s);
void update_keyboard_focus(struct tomoe *s);
void grab_clear(struct tomoe *s);
void input_listen(struct tomoe *s);
void presentation_bindings_finish(struct presentation *plan);
void presentation_input_publish(struct tomoe *s, struct presentation *plan);
void pointer_refresh(struct tomoe *s);
void ui_input_finish(struct tomoe *s);
void pointer_sync_cursors(struct tomoe *s);
bool virtual_input_listen(struct tomoe *s);
struct screen *virtual_pointer_output(struct tomoe *s, struct input_device *device);

bool activation_listen(struct tomoe *s);
void activation_surface_mapped(struct tomoe *s, struct surface *surface);
void activation_finish(struct tomoe *s);

bool protocols_listen(struct tomoe *s);
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
bool power_listen(struct tomoe *s);
bool output_power(struct output *o, bool on);
void power_output_gone(struct output *o);
void outputs_event(struct tomoe *s);
void drag_icons_refresh(struct tomoe *s);
void constraint_focus(struct tomoe *s, struct surface *surface, double sx, double sy);
bool constraint_allows(struct tomoe *s, double x, double y);

bool lock_listen(struct tomoe *s);
void lock_finish(struct tomoe *s);
void lock_refresh(struct tomoe *s);
void lock_frame_rendered(struct tomoe *s, struct screen *output);
void input_lock_begin(struct tomoe *s);
bool lock_active(struct tomoe *s);
bool render_output_buffer(struct output *o, struct buffer *buffer);
struct screenshot;
bool screenshot_key(struct tomoe *s, xkb_keysym_t sym, bool pressed);
bool screenshot_button(struct tomoe *s, uint32_t button, bool pressed);
void screenshot_motion(struct tomoe *s);
bool screenshot_render_frozen(struct output *o, struct frame *f);
void screenshot_render(struct output *o, struct frame *f);
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
void screen_state_set_wait_timeline(struct screen_state *state,
    struct timeline *timeline, uint64_t point);
bool screen_state_set_gamma(struct screen_state *state, const uint16_t *ramps, size_t size);
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
size_t screen_gamma_size(struct screen *screen);
void screen_schedule_frame(struct screen *screen);
void screen_send_frame(struct screen *screen);
void screen_send_present(struct screen *screen, struct screen_present *present);
void screen_request_state(struct screen *screen, struct screen_state *state);
void screen_set_position(struct screen *screen, int lx, int ly);
void screen_cursor_move(struct screen *screen, double x, double y);
void screen_lock_software_cursors(struct screen *screen, bool lock);
bool screens_listen(struct tomoe *s);
void cursor_show(struct tomoe *s, struct buffer *buffer, int hotspot_x, int hotspot_y,
    float scale);
void cursor_finish(struct tomoe *s);
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
struct udev *kms_udev(struct tomoe *s);
int nested_create(struct tomoe *s);
bool nested_start(struct tomoe *s);
void nested_finish(struct tomoe *s);
bool headless_start(struct tomoe *s);
void headless_finish(struct tomoe *s);
void libinput_active(struct tomoe *s, bool active);

bool buffers_listen(struct tomoe *s);
bool surfaces_listen(struct tomoe *s);
void surfaces_finish(struct tomoe *s);
struct surface *surface_from_resource(struct wl_resource *resource);
pixman_region32_t *region_from_resource(struct wl_resource *resource);
bool surface_has_buffer(struct surface *surface);
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
bool surface_on_output(struct surface *surface, struct screen *output);
void surface_frame_done(struct surface *surface, const struct timespec *when);
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
void input_remove(struct tomoe *s, struct input_device *device);

#endif
