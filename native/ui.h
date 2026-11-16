#ifndef TOMOE_UI_H
#define TOMOE_UI_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <wayland-server-protocol.h>

#include "ui-shape.h"

struct box;
struct output;
struct presentation;
struct tomoe;
struct ui_set;
struct ui_surface;
struct frame;

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
    uint64_t plan_id, int x, int y, int width, int height, int layer,
    int click_through, int slide, int blur, uint32_t attach, int attach_axes);
int tomoe_present_ui_clip(struct tomoe *s, int x, int y, int width, int height);
int tomoe_present_ui_asset_ref(struct tomoe *s, uint64_t id);
int tomoe_present_ui_backdrop(struct tomoe *s, uint64_t id, double fps, double idle);
int tomoe_present_ui_shader_inputs(struct tomoe *s, const double *data, int data_count,
    const uint32_t *windows, int window_count, int clocked, double clock, double until);
int tomoe_present_ui_trail(struct tomoe *s, double cell, double life, int steps,
    uint32_t rgba, double fps, double idle);
int tomoe_present_ui_asset(struct tomoe *s, uint64_t id,
    double x, double y, double width, double height, int tint, uint32_t rgba);
struct ui_shape {
    double x, y, width, height, radius, stroke;
    uint32_t rgba;
    int fill;
    double geometry[4];
    int stop_count;
    double stop_at[UI_SHAPE_STOPS];
    uint32_t stop_rgba[UI_SHAPE_STOPS];
    int shadow_count;
    double shadow[UI_SHAPE_SHADOWS][4];
    uint32_t shadow_rgba[UI_SHAPE_SHADOWS];
    const double *points;
    long point_count;
};

int tomoe_present_ui_shape(struct tomoe *s, const struct ui_shape *shape);
int tomoe_present_ui_text(struct tomoe *s, double x, double y,
    const char *text, const char *font, double size, double line_height,
    uint32_t rgba, int rotate);
int tomoe_present_ui_arc(struct tomoe *s, double cx, double cy, double radius,
    double thickness, double start, double end, uint32_t rgba);
int tomoe_present_ui_hit(struct tomoe *s, const char *key, const char *command,
    const char *hover, int x, int y, int width, int height);
int tomoe_present_ui_end(struct tomoe *s);

uint64_t tomoe_ui_text_size(const char *text, const char *font,
    double size, double line_height);
const char *tomoe_ui_stats(struct tomoe *s);

bool ui_hit_at(struct tomoe *s, double x, double y, struct ui_hit *out);
bool ui_damage_since(const struct ui_surface *surface, uint64_t seq, struct box *out);
void ui_render(struct output *o, struct frame *f, const struct presentation *plan, int layer);
void ui_output_finish(struct tomoe *s, const char *output);
bool ui_on_output(struct output *o, int layer);
size_t ui_claim(struct output *o, int layer, struct box *boxes, bool *blurs, size_t max);
bool ui_prepare(struct tomoe *s);
void ui_publish(struct tomoe *s);
bool ui_animate(struct tomoe *s, double now);
void ui_backdrops_ready(struct tomoe *s);
void ui_pointer_moved(struct tomoe *s);
void ui_set_finish(struct ui_set *set);
void ui_finish(struct tomoe *s);

void ui_input_finish(struct tomoe *s);

#endif
