#ifndef TOMOE_UI_H
#define TOMOE_UI_H

#include <stdbool.h>
#include <stdint.h>

#include <wayland-server-protocol.h>

struct output;
struct presentation;
struct tomoe;
struct ui_set;
struct pass;

struct ui_hit {
    const char *owner;
    const char *name;
    const char *output;
    const char *key;
    const char *command, *hover;
    uint64_t source_id;
    uint64_t callback_id;
    double x;
    double y;
};

int tomoe_present_ui_surface(struct tomoe *s, const char *owner,
    uint64_t source_id, const char *name, const char *output,
    const char *signature, int x, int y, int width, int height, int layer);
int tomoe_present_ui_clip(struct tomoe *s, int x, int y, int width, int height);
int tomoe_present_ui_asset_ref(struct tomoe *s, uint64_t id);
int tomoe_present_ui_asset(struct tomoe *s, uint64_t id,
    double x, double y, double width, double height, int tint, uint32_t rgba);
int tomoe_present_ui_rect(struct tomoe *s, double x, double y, double width,
    double height, uint32_t rgba, double radius, double stroke);
int tomoe_present_ui_text(struct tomoe *s, double x, double y,
    const char *text, const char *font, double size, double line_height,
    uint32_t rgba);
int tomoe_present_ui_arc(struct tomoe *s, double cx, double cy, double radius,
    double thickness, double start, double end, uint32_t rgba);
int tomoe_present_ui_hit(struct tomoe *s, const char *key, const char *command,
    const char *hover, int x, int y, int width, int height);
int tomoe_present_ui_end(struct tomoe *s);

uint64_t tomoe_ui_text_size(const char *text, const char *font,
    double size, double line_height);
int tomoe_ui_callback_current(struct tomoe *s, uint64_t callback_id);
const char *tomoe_ui_stats(struct tomoe *s);

bool ui_hit_at(struct tomoe *s, double x, double y, struct ui_hit *out);
void ui_render(struct output *o, struct pass *pass,
    const struct presentation *plan, int x, int y, int width, int height,
    enum wl_output_transform transform);
void ui_output_finish(struct tomoe *s, const char *output);
bool ui_on_output(struct output *o);
bool ui_prepare(struct tomoe *s);
void ui_publish(struct tomoe *s);
void ui_set_finish(struct ui_set *set);
void ui_finish(struct tomoe *s);

void ui_input_finish(struct tomoe *s);

#endif
