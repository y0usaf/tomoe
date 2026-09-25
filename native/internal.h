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
#include <wlr/backend.h>
#include <wlr/render/allocator.h>
#include <wlr/render/pass.h>
#include <wlr/render/wlr_texture.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_virtual_pointer_v1.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_fractional_scale_v1.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/interfaces/wlr_keyboard.h>
#include <wlr/types/wlr_layer_shell_v1.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_pointer.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_subcompositor.h>
#include <wlr/types/wlr_viewporter.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/types/wlr_xdg_output_v1.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/log.h>
#include <wlr/util/transform.h>
#include <xkbcommon/xkbcommon.h>

struct event;
struct binding;
struct pointer_latch { uint32_t button; struct binding *binding; };
struct window;
struct tracked_surface;
struct layer_plan;
struct presentation;
struct keyboard_profile;
struct logical_keyboard;
struct ui_set;
struct ui_asset_pool;
struct ui_pointer;

struct wlr_drm_syncobj_timeline;
enum { PROGRAM_RECT, PROGRAM_SDF, PROGRAM_TEXTURE, PROGRAM_EXTERNAL, PROGRAM_DOWN, PROGRAM_UP,
    PROGRAM_COUNT };
struct program {
    GLuint id;
    GLint pos, local, texcoord;
    GLint tex, alpha, opaque, size, radius, clip, width, kind, color, range, power, half_pixel, offset;
};
#define RING_SLOTS 4
struct ring {
    struct wlr_buffer *slots[RING_SLOTS];
    int width, height;
    bool implicit;
    struct wlr_drm_format_set format;
};
struct wlr_renderer *render_create(struct wlr_backend *backend);
struct wlr_allocator *render_allocator(struct wlr_renderer *renderer);
struct program *render_program(struct wlr_renderer *renderer, int kind);
void render_quad(struct program *p, const float pos[8], const float local[8],
    const float texcoords[8]);
bool render_texture_gl(struct wlr_texture *texture, GLenum *target, GLuint *tex, bool *alpha);
GLuint render_buffer_fbo(struct wlr_renderer *renderer, struct wlr_buffer *buffer);
uint32_t render_read_format(struct wlr_renderer *renderer);
const struct wlr_drm_format_set *render_formats(struct wlr_renderer *renderer);
bool render_wait(struct wlr_renderer *renderer, struct wlr_drm_syncobj_timeline *timeline,
    uint64_t point);
bool ring_configure(struct tomoe *s, struct ring *ring, struct wlr_output *output,
    int width, int height, bool implicit);
struct wlr_buffer *ring_acquire(struct tomoe *s, struct ring *ring);
struct wlr_buffer *ring_create(struct tomoe *s, struct ring *ring);
void ring_finish(struct ring *ring);
bool fenced(struct wlr_output *output);

struct output {
    struct wl_list link;
    struct tomoe *server;
    struct wlr_output *wlr;
    struct wl_listener frame, request, destroy, needs_frame;
    struct wlr_output_state initial, pending;
    struct wlr_output_state deferred;
    bool configured, admitted, pending_configured, pending_positioned;
    bool request_pending, pending_hold;
    uint64_t id;
    uint64_t request_id;
    int pending_x, pending_y;
    int x, y;
    bool positioned;
    char mirror[129];
    char pending_mirror[129];
    struct wlr_buffer *capture_buffer;
    struct wlr_buffer *presented[2];
    struct ring ring;
    bool lock_rendered, gamma_dirty;
    struct wlr_surface *scanout;
};

static inline bool output_is_active(const struct output *output) {
    return output && output->admitted && output->wlr && output->wlr->enabled;
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
    struct wlr_output *output;
    int client_width, client_height;
    bool fullscreen;
    double offset_x, offset_y, alpha;
    struct window_style style;
};
struct frame {
    struct tomoe *server;
    struct wlr_render_pass *pass;
    struct wlr_buffer *buffer;
    int x, y, width, height;
    enum wl_output_transform transform;
    double view_x, view_y, zoom;
    uint32_t focused;
};
struct effects;
void effect_border(struct frame *f, struct wlr_fbox geometry, double width, double radius,
    uint32_t rgba, float alpha);
void effect_shadow(struct frame *f, struct wlr_fbox geometry, double range, double radius,
    uint32_t rgba, double power, float alpha);
bool effect_texture(struct frame *f, const struct wlr_render_texture_options *options,
    struct wlr_fbox dst, struct wlr_fbox clip, double radius);
void effect_blur(struct frame *f, struct wlr_fbox area, double radius, int passes,
    double offset, int margin);
void effects_finish(struct tomoe *s);
bool background_effects_listen(struct tomoe *s);
const pixman_region32_t *background_blur_region(struct tomoe *s, struct wlr_surface *surface);
struct presentation_output {
    struct output *output;
    struct wlr_box box;
    int scale_120;
};
struct output_location {
    struct output *output;
    int x, y, width, height, scale_120;
    bool active, mirrored;
};
struct presentation_target {
    struct wlr_scene_node *node;
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
void input_device_track(struct tomoe *s, struct wlr_input_device *wlr);
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
struct tomoe {
    struct wl_display *display;
    struct settings settings;
    struct effects *effects;
    struct screenshot *screenshot;
    struct wlr_backend *backend;
    struct wlr_session *session;
    struct wlr_renderer *renderer;
    struct wlr_allocator *allocator;
    struct wlr_scene *scene;
    struct wlr_output_layout *layout;
    struct wlr_scene_tree *window_tree, *layer_tree[4], *fullscreen_tree;
    struct wlr_cursor *cursor;
    struct wlr_xcursor_manager *cursor_manager;
    struct wlr_seat *seat;
    struct logical_keyboard *logical_keyboard;
    struct keyboard_profile *keyboard_profile;
    struct wl_list input_devices, background_effects;
    struct wl_list windows, layers, outputs, keyboards, events, bindings, tracked_surfaces, virtual_pointers;
    struct wl_list activation_tokens;
    size_t activation_tracked_count;
    size_t activation_token_count;
    struct wl_list activation_pending;
    size_t activation_pending_count;
    struct wl_listener new_output, new_input, new_toplevel, new_popup, new_layer_surface;
    struct wl_listener motion, absolute, button, axis, frame;
    struct wl_listener new_virtual_pointer, new_virtual_keyboard;
    struct wl_listener request_cursor, pointer_focus, selection, layout_change, backend_destroy, new_surface;
    char *last_event;
    uint32_t next_id, focused, grab_id;
    uint64_t next_binding_id, next_device_id, next_output_id;
    uint32_t latest_keyboard_enter_serial, latest_pointer_enter_serial;
    bool have_keyboard_enter_serial, have_pointer_enter_serial;
    int grab_mode;
    double grab_x, grab_y;
    double pointer_x, pointer_y;
    struct wlr_surface *cursor_surface;
    bool cursor_hidden;
    uint32_t hovered;
    char *ui_hovered;
    struct pointer_latch pointer_latches[32];
    char *grab_owner, *grab_otherwise;
    uint64_t grab_source;
    size_t pointer_latch_count;
    struct wlr_drm_syncobj_timeline *render_timeline;
    uint64_t render_point;
    struct wl_listener cursor_surface_destroy;
    int view_x, view_y;
    double view_zoom;
    char *hit_result;
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
    struct wlr_primary_selection_v1_device_manager *primary_selection;
    struct wlr_data_control_manager_v1 *data_control;
    struct wlr_ext_data_control_manager_v1 *ext_data_control;
    struct wlr_relative_pointer_manager_v1 *relative_pointer;
    struct wlr_presentation *presentation_time;
    struct wlr_idle_notifier_v1 *idle_notifier;
    struct wlr_idle_inhibit_manager_v1 *idle_inhibit;
    struct wl_list tearings;
    struct wl_list gammas, decorations;
    struct wlr_pointer_constraints_v1 *pointer_constraints;
    struct wlr_pointer_constraint_v1 *active_constraint;
    struct wl_list foreigns, foreign_managers, foreign_lists;
    struct wl_resource *session_lock;
    bool lock_confirmed;
    int lock_state;
    struct wlr_scene_tree *drag_icon_tree, *lock_tree;
    struct wl_list drag_icons, constraints, lock_surfaces;
    struct wl_event_source *lock_deadline_source;
    struct wl_listener request_set_primary_selection, request_start_drag, seat_start_drag;
    struct target drag_icon;
    struct wl_listener new_constraint, constraint_commit,
        constraint_destroy;
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
struct layer {
    struct target target;
    struct wl_list link;
    struct tomoe *server;
    struct wlr_layer_surface_v1 *wlr;
    struct wlr_scene_layer_surface_v1 *scene;
    struct wl_listener commit, destroy, map, unmap, new_popup;
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
    wlr_log(WLR_ERROR, "tomoe: %s", message);
    s->failed = true;
    s->running = false;
}

void quote(FILE *out, const char *text);
FILE *begin_event(struct tomoe *s, struct event **event, size_t *size);
void end_event(struct tomoe *s, struct event *event, FILE *out);
void unmap_event(struct tomoe *s, uint32_t id);

struct wlr_output *any_output(struct tomoe *s);
void outputs_listen(struct tomoe *s);
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
    struct wlr_output *output);
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
void physical_output_box(struct output *o, struct wlr_box *box);
void schedule_scene(struct tomoe *s);
void refresh_scene(struct tomoe *s);
void forget_output(struct tomoe *s, struct wlr_output *output);
bool render_output(struct output *o, struct wlr_output_state *state);
bool render_presentation(struct output *o, struct wlr_output_state *state,
    struct ring *ring, const struct presentation *plan);
bool capture_listen(struct tomoe *s);
bool capture_wants_cursorless(struct output *o);
void capture_serve(struct output *o, struct wlr_buffer *committed, bool scanout);
void capture_output_gone(struct tomoe *s, struct output *o);
void capture_window_gone(struct tomoe *s, uint32_t id);
void finish_output_capture(struct output *o);
void finish_captures(struct tomoe *s);
void frame_done(struct output *o, const struct timespec *when);
void surfaces_textured(struct output *o);
struct wlr_surface *scanout_surface(struct output *o);
bool surface_visible(struct tomoe *s, struct wlr_surface *surface);
double physical_hit_ratio(struct tomoe *s, double x, double y);
uint32_t physical_hit_test(struct tomoe *s, double x, double y,
    struct wlr_surface **surface, double *sx, double *sy);
void set_surface_scale(struct wlr_surface *surface, double scale);
void surfaces_listen(struct tomoe *s, struct wlr_compositor *compositor);

struct wlr_surface *surface_of(struct window *w);
struct window *find_window(struct tomoe *s, uint32_t id);
struct window *find_window_registered(struct tomoe *s, uint32_t id);
struct window *find_window_any(struct tomoe *s, uint32_t id);
uint32_t find_window_id_for_surface(struct tomoe *s, struct wlr_surface *surface);
bool window_surface_mapped(struct tomoe *s, struct wlr_surface *surface);
void popup_create(struct wlr_xdg_popup *xdg, struct wlr_scene_tree *parent);
void windows_listen(struct tomoe *s, struct wlr_xdg_shell *shell);
void windows_refresh(struct tomoe *s);
void foreign_toplevels_refresh(struct tomoe *s);
bool windows_animate(struct tomoe *s);
bool window_capture_size(struct tomoe *s, uint32_t id, int *width, int *height);
struct wlr_scene_node *window_capture_node(struct tomoe *s, uint32_t id, struct target *target);
bool foreign_listen(struct tomoe *s);
void foreign_update(struct tomoe *s, uint32_t id, const char *title, const char *app_id,
    uint32_t state, struct wlr_output *const *outputs, size_t output_count);
void foreign_forget(struct tomoe *s, uint32_t id);
const char *foreign_identifier(struct tomoe *s, uint32_t id);
uint32_t foreign_handle_window(struct wl_resource *handle);
void window_foreign_request(struct tomoe *s, uint32_t id, const char *request, int requested,
    struct wlr_output *output);
bool render_window_buffer(struct tomoe *s, uint32_t id, struct wlr_buffer *buffer);
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
void layers_listen(struct tomoe *s, struct wlr_layer_shell_v1 *layer_shell);

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
bool virtual_pointers_listen(struct tomoe *s);
struct wlr_output *virtual_pointer_output(struct tomoe *s, struct wlr_input_device *device);

bool activation_listen(struct tomoe *s);
void activation_surface_mapped(struct tomoe *s, struct wlr_surface *surface);
void activation_finish(struct tomoe *s);

bool protocols_listen(struct tomoe *s);
void relative_motion_forward(struct tomoe *s, uint32_t time_msec,
    double dx, double dy, double dx_unaccel, double dy_unaccel);
void idle_notify_activity(struct tomoe *s);
void idle_refresh(struct tomoe *s);
void gamma_apply(struct output *o, struct wlr_output_state *state);
bool gamma_listen(struct tomoe *s);
bool decoration_listen(struct tomoe *s);
bool tearing_listen(struct tomoe *s);
bool tearing_async(struct tomoe *s, struct wlr_surface *surface);
void gamma_output_gone(struct output *o);
void drag_icons_refresh(struct tomoe *s);
void constraint_focus(struct tomoe *s, struct wlr_surface *surface, double sx, double sy);
bool constraint_allows(struct tomoe *s, double x, double y);

bool lock_listen(struct tomoe *s);
void lock_finish(struct tomoe *s);
void lock_refresh(struct tomoe *s);
void lock_frame_rendered(struct tomoe *s, struct wlr_output *output);
void input_lock_begin(struct tomoe *s);
bool lock_active(struct tomoe *s);
bool render_output_buffer(struct output *o, struct wlr_buffer *buffer);
struct screenshot;
bool screenshot_key(struct tomoe *s, xkb_keysym_t sym, bool pressed);
bool screenshot_button(struct tomoe *s, uint32_t button, bool pressed);
void screenshot_motion(struct tomoe *s);
bool screenshot_render_frozen(struct output *o, struct frame *f);
void screenshot_render(struct output *o, struct frame *f);
void screenshot_output_gone(struct tomoe *s, struct output *o);
void screenshot_finish(struct tomoe *s);
struct wlr_surface *lock_keyboard_surface(struct tomoe *s);

#endif
