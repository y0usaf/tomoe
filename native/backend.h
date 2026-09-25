#ifndef TOMOE_BACKEND_H
#define TOMOE_BACKEND_H
#include <stdint.h>

struct tomoe;
int tomoe_abi_version(void);
struct tomoe *tomoe_create(const char *socket_name);
const char *tomoe_display_name(struct tomoe *server);
const char *tomoe_activation_token(struct tomoe *server);
void tomoe_activation_revoke(struct tomoe *server, const char *token);
int tomoe_step(struct tomoe *server, int timeout_ms);
const char *tomoe_next_event(struct tomoe *server);
int tomoe_event_count(struct tomoe *server);
int tomoe_event_barrier(struct tomoe *server);
void tomoe_destroy(struct tomoe *server);
void tomoe_set_view(struct tomoe *server, int x, int y, double zoom);
const char *tomoe_hit_test(struct tomoe *server, double x, double y);
void tomoe_place(struct tomoe *server, uint32_t id, int x, int y,
    int width, int height, int visible);
void tomoe_focus(struct tomoe *server, uint32_t id);
uint32_t tomoe_keyboard_focus(struct tomoe *server);
void tomoe_close(struct tomoe *server, uint32_t id);
int tomoe_grab(struct tomoe *server, uint32_t id, int mode);
uint32_t tomoe_grab_id(struct tomoe *server);
int tomoe_grab_mode(struct tomoe *server);
void tomoe_window_state(struct tomoe *server, uint32_t id, int fullscreen,
    int maximize);
void tomoe_layer(struct tomoe *server, uint32_t id, int layer,
    int exclusive_zone, int keyboard, int visible);
uint32_t tomoe_keysym(const char *name);
void tomoe_clear_bindings(struct tomoe *server);
int tomoe_bind(struct tomoe *server, uint32_t modifiers, uint32_t keysym,
    const char *owner, const char *press, const char *release,
    uint64_t source_id);
int tomoe_binding_current(struct tomoe *server, uint64_t binding_id);
int tomoe_outputs_begin(struct tomoe *server);
int tomoe_outputs_pending(struct tomoe *server);
int tomoe_output(struct tomoe *server, const char *name, int mode,
    int width, int height, int refresh, int scale, int x, int y, int positioned);
int tomoe_output_options(struct tomoe *server, const char *name, int enabled,
    const char *mirror, int adaptive_sync);
int tomoe_output_hold(struct tomoe *server, const char *name);
const char *tomoe_outputs_apply(struct tomoe *server);
const char *tomoe_outputs_preview(struct tomoe *server);
uint64_t tomoe_outputs_revision(struct tomoe *server);
const char *tomoe_outputs_current(struct tomoe *server);
int tomoe_layers_begin(struct tomoe *server);
int tomoe_layers_output(struct tomoe *server, const char *name,
    int x, int y, int width, int height, int scale_120);
int tomoe_layers_surface(struct tomoe *server, uint32_t id,
    int layer, int exclusive_zone, int keyboard, int visible);
const char *tomoe_layers_preview(struct tomoe *server);
int tomoe_present_begin(struct tomoe *server, int view_x, int view_y,
    double zoom, uint32_t focus, int restack, int outputs_changed,
    int replace_bindings, uint32_t grab_id, int grab_mode);
int tomoe_present_bind(struct tomoe *server, uint32_t modifiers, uint32_t keysym,
    const char *owner, const char *press, const char *release,
    uint64_t source_id);
int tomoe_present_keyboard(struct tomoe *server, const char *rules,
    const char *model, const char *layout, const char *variant,
    const char *options, int repeat_rate, int repeat_delay);
int tomoe_present_window(struct tomoe *server, uint32_t id, int x, int y,
    int width, int height, int visible, int fullscreen, int maximize);
int tomoe_present_stack(struct tomoe *server, uint32_t id);
int tomoe_present_window_style(struct tomoe *server, uint32_t id, int radius, int blur,
    int tearing, int64_t focused, int64_t unfocused);
int tomoe_present_ui_surface(struct tomoe *server, const char *owner,
    uint64_t source_id, const char *name, const char *output,
    const char *signature, int x, int y, int width, int height, int layer);
int tomoe_present_ui_clip(struct tomoe *server, int x, int y, int width, int height);
uint64_t tomoe_ui_asset_load(struct tomoe *server, const char *owner,
    uint64_t source_id, int kind, const char *path, const char *name);
uint64_t tomoe_ui_asset_size(struct tomoe *server, uint64_t id);
void tomoe_ui_assets_discard(struct tomoe *server);
int tomoe_present_ui_asset_ref(struct tomoe *server, uint64_t id);
int tomoe_present_ui_asset(struct tomoe *server, uint64_t id,
    double x, double y, double width, double height, int tint, uint32_t rgba);
int tomoe_present_ui_rect(struct tomoe *server, double x, double y, double width,
    double height, uint32_t rgba, double radius, double stroke);
int tomoe_present_ui_text(struct tomoe *server, double x, double y,
    const char *text, const char *font, double size, double line_height, uint32_t rgba);
int tomoe_present_ui_arc(struct tomoe *server, double cx, double cy, double radius,
    double thickness, double start, double end, uint32_t rgba);
int tomoe_present_ui_hit(struct tomoe *server, const char *key, const char *command,
    int x, int y, int width, int height);
int tomoe_present_ui_end(struct tomoe *server);
uint64_t tomoe_ui_text_size(const char *text, const char *font,
    double size, double line_height);
int tomoe_ui_callback_current(struct tomoe *server, uint64_t callback_id);
const char *tomoe_ui_stats(struct tomoe *server);
int tomoe_present_settings(struct tomoe *server);
int tomoe_present_setting(struct tomoe *server, const char *key, double value);
int tomoe_present_setting_text(struct tomoe *server, const char *key, const char *text);
const char *tomoe_present_apply(struct tomoe *server);
void tomoe_present_abort(struct tomoe *server);
#endif
