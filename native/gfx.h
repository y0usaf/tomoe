#ifndef TOMOE_GFX_H
#define TOMOE_GFX_H

#include "internal.h"
#include <math.h>

struct gbm_device;

enum { PASS_POOL = 4 };

struct held_frame {
    int fd;
    struct pass *pass;
    struct held_frame *next;
};

struct pass {
    struct render *r;
    struct buffer *buffer;
    struct timeline *signal;
    uint64_t point;
    struct oplist *record;
    const struct region *clip;
    const struct oplist *ops;
    size_t op_next;
    bool op_lost;
};

struct render {
    int fd;
    bool timeline;
    struct held_frame *held;
    int held_count;
    bool held_reported, held_refused;
    bool stall_test;
    int stall_write, stall_delay_ms;
    bool stall_capture_on;
    int stall_capture_write, stall_capture_delay_ms;
    size_t textures;
    uint64_t texture_bytes;
    struct gbm_device *gbm;
    struct format_set texture_formats, render_formats, shm_formats;
    struct wl_list bos;
    bool read_bgra;
    struct pass passes[PASS_POOL];
    bool pass_used[PASS_POOL];
    uint64_t gl_calls, quads, runs, fragments, uploads, upload_bytes;
    char *node;
    struct upload *upload;
};

static inline struct rect pixel_bounds(const struct pass *pass, double x0, double y0,
        double x1, double y1) {
    double width = pass->buffer->width, height = pass->buffer->height;
    return (struct rect){
        (int32_t)floor((x0 + 1.0) * 0.5 * width),
        (int32_t)floor((y0 + 1.0) * 0.5 * height),
        (int32_t)ceil((x1 + 1.0) * 0.5 * width),
        (int32_t)ceil((y1 + 1.0) * 0.5 * height),
    };
}

static inline struct rect position_bounds(const struct pass *pass, const float *pos,
        size_t vertices) {
    double x0 = pos[0], x1 = x0, y0 = pos[1], y1 = y0;
    for (size_t i = 1; i < vertices; i++) {
        double x = pos[2 * i], y = pos[2 * i + 1];
        if (x < x0) x0 = x;
        if (x > x1) x1 = x;
        if (y < y0) y0 = y;
        if (y > y1) y1 = y;
    }
    return pixel_bounds(pass, x0, y0, x1, y1);
}

static inline bool bounds_overlap(const struct rect *bounds, const struct rect *clip) {
    return bounds->x1 < bounds->x2 && bounds->y1 < bounds->y2 &&
        bounds->x1 < clip->x2 && bounds->x2 > clip->x1 &&
        bounds->y1 < clip->y2 && bounds->y2 > clip->y1;
}

static inline uint64_t quad_pixels(const struct pass *pass, const float pos[8],
        const struct rect *clip) {
    struct rect bounds = position_bounds(pass, pos, 4);
    int32_t left = bounds.x1, right = bounds.x2;
    int32_t top = bounds.y1, bottom = bounds.y2;
    if (clip) {
        if (clip->x1 > left) left = clip->x1;
        if (clip->y1 > top) top = clip->y1;
        if (clip->x2 < right) right = clip->x2;
        if (clip->y2 < bottom) bottom = clip->y2;
    }
    if (right <= left || bottom <= top) return 0;
    return (uint64_t)(right - left) * (uint64_t)(bottom - top);
}

struct render *render_open(char *node);
bool render_owns(const struct buffer *buffer);

struct render *gfx_alloc(void);
bool gfx_open(struct render *r);
bool gfx_ready(struct render *r);
bool gfx_fences(struct render *r);
void gfx_stop(struct render *r);
void gfx_drop_images(struct render *r);
void gfx_close(struct render *r);
size_t gfx_image_count(struct render *r);
void gfx_current(struct render *r);
void gfx_flush(struct render *r);
void gfx_finish(struct render *r);
int gfx_fence_export(struct render *r);
bool gfx_target_prepare(struct render *r, struct buffer *buffer);
bool gfx_target_begin(struct render *r, struct buffer *buffer);
void gfx_target_end(struct render *r);
void gfx_texture_draw(struct pass *pass, const struct texture_options *options,
    const float pos[8], const float texcoords[8], float alpha);
bool gfx_tex_alpha(const struct texture *texture);
bool gfx_tex_external(const struct texture *texture);

bool gfx_tex_info(const struct texture *texture, bool *external, bool *alpha);
bool gfx_external_ready(struct render *r);
void gfx_state_dirty(struct pass *pass);
void gfx_target_viewport(struct render *r, struct buffer *buffer);
void gfx_target_rebind(struct render *r, struct buffer *buffer);

enum { SHADER_DATA = 192, SHADER_WINDOWS = 8 };

struct gfx_shader_params {
    struct fbox box;
    double time, mouse_x, mouse_y;
    int frame, trail_count, data_count, window_count;
    const float *trail, *data, *windows, *selection, *marks_box;
    struct texture *marks;
    const struct gfx_level *frost, *beneath;
    bool frost_ok;
    float beneath_box[4];
};

bool gfx_level_ensure(struct gfx_level *level, int width, int height);
void gfx_level_release(struct gfx_level *level);
void gfx_level_blit(const struct gfx_level *from, const struct gfx_level *to,
    const struct box *samples, int count, int sx, int sy, int dx, int dy);
bool gfx_level_spill(const struct gfx_level *from, struct gfx_level *to, int width, int height,
    const struct box *samples, int count, int dx, int dy);
void gfx_blur_copy_in(struct render *r, struct buffer *buffer, struct gfx_level *level,
    struct box grown, const struct rect *copy, int copies, int shrink);
void gfx_blur_passes(struct render *r, struct gfx_level *const *levels, int passes,
    double offset, const struct region *down, const struct region *up, float noise,
    float vibrancy);
void gfx_backdrop_grab(struct render *r, struct buffer *buffer, const struct gfx_level *level,
    struct box need, const struct box *shown, const struct box *clear, int count);
void gfx_shader_draw(struct pass *pass, struct gfx_prog *p, const struct gfx_shader_params *sp,
    const float pos[8], const float local[8]);
void gfx_glass_draw(struct pass *pass, struct gfx_prog *p, struct texture *map,
    const struct gfx_level *glass, struct box grown, struct fbox box, double bend,
    const float pos[8], const float local[8]);
void gfx_external_draw(struct pass *pass, const struct texture_options *options, bool alpha,
    struct fbox clip, double radius, const float pos[8], const float local[8],
    const float texcoords[8]);

#endif
