#ifndef TOMOE_UI_ASSETS_H
#define TOMOE_UI_ASSETS_H

#include <stdbool.h>
#include <stdint.h>
#include <cairo.h>

struct program;
struct texture;
struct tomoe;
struct ui_asset;
struct ui_asset_pool;

uint64_t tomoe_ui_asset_load(struct tomoe *s, const char *owner,
    uint64_t source_id, int kind, const char *path, const char *name);
uint64_t tomoe_ui_asset_size(struct tomoe *s, uint64_t id);
void tomoe_ui_assets_discard(struct tomoe *s);
struct ui_asset *ui_asset_acquire(struct tomoe *s, uint64_t id);
struct ui_asset *ui_asset_retain(struct ui_asset *asset);
struct texture *ui_asset_backdrop(const struct ui_asset *asset, int *x, int *y);
struct program *ui_asset_shader(const struct ui_asset *asset, double *epoch);
bool ui_asset_owned_by(const struct ui_asset *asset, const char *owner, uint64_t source_id);
void ui_asset_release(struct ui_asset *asset);
bool ui_asset_paint(struct ui_asset *asset, cairo_t *cairo,
    double x, double y, double width, double height, bool tint, uint32_t rgba);
uint64_t ui_assets_count(struct tomoe *s);
uint64_t ui_assets_bytes(struct tomoe *s);
uint64_t ui_assets_loads(struct tomoe *s);
void ui_assets_finish(struct tomoe *s);

#endif
