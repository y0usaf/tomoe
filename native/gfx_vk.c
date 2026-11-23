#include "gfx.h"
#include "backend.h"

struct render *gfx_alloc(void) {
    tomoe_log(LOG_ERROR, "tomoe: GFX=vk renderer not built yet");
    return NULL;
}

bool gfx_open(struct render *r) {
    return false;
}

bool gfx_ready(struct render *r) {
    return false;
}

bool gfx_fences(struct render *r) {
    return false;
}

void gfx_stop(struct render *r) {
}

void gfx_drop_images(struct render *r) {
}

void gfx_close(struct render *r) {
}

size_t gfx_image_count(struct render *r) {
    return 0;
}

void gfx_current(struct render *r) {
}

void gfx_flush(struct render *r) {
}

void gfx_upload_trim(struct render *r) {
    (void)r;
}

void gfx_upload_latch(struct render *r) {
    (void)r;
}

void gfx_finish(struct render *r) {
}

int gfx_fence_export(struct render *r) {
    return -1;
}

bool gfx_target_prepare(struct render *r, struct buffer *buffer) {
    return false;
}

bool gfx_target_begin(struct render *r, struct buffer *buffer) {
    return false;
}

void gfx_target_end(struct render *r) {
}

void gfx_target_viewport(struct render *r, struct buffer *buffer) {
}

void gfx_target_rebind(struct render *r, struct buffer *buffer) {
}

void gfx_state_dirty(struct pass *pass) {
}

void gfx_texture_draw(struct pass *pass, const struct texture_options *options,
        const float pos[8], const float texcoords[8], float alpha) {
}

bool gfx_tex_alpha(const struct texture *texture) {
    return false;
}

bool gfx_tex_external(const struct texture *texture) {
    return false;
}

bool gfx_tex_info(const struct texture *texture, bool *external, bool *alpha) {
    return false;
}

bool gfx_external_ready(struct render *r) {
    return false;
}

bool gfx_prog_reads(const struct gfx_prog *p, enum gfx_input input) {
    return false;
}

bool gfx_level_ensure(struct gfx_level *level, int width, int height) {
    return false;
}

void gfx_level_release(struct gfx_level *level) {
    *level = (struct gfx_level){0};
}

void gfx_level_blit(const struct gfx_level *from, const struct gfx_level *to,
        const struct box *samples, int count, int sx, int sy, int dx, int dy) {
}

bool gfx_level_spill(const struct gfx_level *from, struct gfx_level *to, int width, int height,
        const struct box *samples, int count, int dx, int dy) {
    return false;
}

void gfx_blur_copy_in(struct render *r, struct buffer *buffer, struct gfx_level *level,
        struct box grown, const struct rect *copy, int copies, int shrink) {
}

void gfx_blur_passes(struct render *r, struct gfx_level *const *levels, int passes,
        double offset, const struct region *down, const struct region *up, float noise,
        float vibrancy) {
}

void gfx_backdrop_grab(struct render *r, struct buffer *buffer, const struct gfx_level *level,
        struct box need, const struct box *shown, const struct box *clear, int count) {
}

void gfx_shader_draw(struct pass *pass, struct gfx_prog *p, const struct gfx_shader_params *sp,
        const float pos[8], const float local[8]) {
}

void gfx_glass_draw(struct pass *pass, struct gfx_prog *p, struct texture *map,
        const struct gfx_level *glass, struct box grown, struct fbox box, double bend,
        const float pos[8], const float local[8]) {
}

void gfx_external_draw(struct pass *pass, const struct texture_options *options, bool alpha,
        struct fbox clip, double radius, const float pos[8], const float local[8],
        const float texcoords[8]) {
}

void pass_scene(struct pass *pass, const struct scene_quad *quad) {
}

void pass_trail(struct pass *pass, const struct trail_cell *cells, size_t count, uint64_t pixels) {
}

void pass_flush(struct pass *pass) {
}

struct gfx_prog *render_shader_create(struct render *r, const char *source, bool *pending) {
    if (pending) *pending = false;
    return NULL;
}

bool render_shader_reap(struct render *r) {
    return false;
}

struct gfx_prog *render_shader_compile(struct render *r, const char *source) {
    return NULL;
}

void render_shader_destroy(struct gfx_prog *program) {
}

bool render_wait(struct render *renderer, struct timeline *timeline, uint64_t point) {
    return false;
}

struct texture *texture_from_buffer(struct render *r, struct buffer *buffer) {
    return NULL;
}

struct texture *texture_new(struct render *r, uint32_t format, uint32_t width, uint32_t height) {
    return NULL;
}

bool texture_clear(struct texture *texture) {
    return false;
}

bool texture_upload(struct texture *texture, const void *data, uint32_t format,
        uint32_t stride, const struct rect *rects, int count) {
    return false;
}

struct texture *texture_from_pixels(struct render *r, uint32_t format, uint32_t stride,
        uint32_t width, uint32_t height, const void *data) {
    return NULL;
}

bool texture_update(struct texture *texture, struct buffer *buffer, const struct region *damage) {
    return false;
}

bool texture_read_pixels(struct texture *texture, const struct read_options *options) {
    return false;
}

void texture_destroy(struct texture *texture) {
}

int tomoe_warm_shaders(const char *node, const char *directory, const char *marker,
        const char *name) {
    tomoe_log(LOG_ERROR, "tomoe: GFX=vk renderer not built yet");
    return 1;
}
