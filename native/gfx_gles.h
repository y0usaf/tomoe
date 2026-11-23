#ifndef TOMOE_GFX_GLES_H
#define TOMOE_GFX_GLES_H

#include "gfx.h"
#include <GLES2/gl2.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2ext.h>
#include <GLES3/gl3.h>
#include "upload.h"

enum { PROGRAM_SCENE, PROGRAM_SDF, PROGRAM_EXTERNAL, PROGRAM_TEXTURE_LUT, PROGRAM_EXTERNAL_LUT,
    PROGRAM_DOWN, PROGRAM_UP, PROGRAM_TRAIL, PROGRAM_COUNT };
struct gfx_prog {
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

struct scene_vertex;
struct scene_instance;

struct scene {
    struct scene_instance *in, *prev;
    size_t count, cap, prev_count, prev_cap;
    struct scene_vertex *vertex;
    size_t vertex_cap;
    struct pass *owner;
    GLuint vbo, dummy;
    size_t vbo_bytes;
    bool attributes;
};

struct shader;
struct shader_job;

struct gfx {
    struct render base;
    EGLDisplay display;
    EGLContext context;
    struct gfx_prog programs[PROGRAM_COUNT];
    struct shader *shaders;
    size_t shader_count;
    uint64_t shader_clock;
    struct wl_list images;
    PFNEGLCREATEIMAGEKHRPROC create_image;
    PFNEGLDESTROYIMAGEKHRPROC destroy_image;
    PFNEGLCREATESYNCKHRPROC create_sync;
    PFNEGLDESTROYSYNCKHRPROC destroy_sync;
    PFNEGLWAITSYNCKHRPROC wait_sync;
    PFNEGLDUPNATIVEFENCEFDANDROIDPROC dup_fence;
    PFNGLEGLIMAGETARGETTEXTURE2DOESPROC image_texture;
    PFNGLEGLIMAGETARGETRENDERBUFFERSTORAGEOESPROC image_renderbuffer;
    struct scene scene;
    struct { GLuint vao, vbo; size_t bytes; } trail;
    char *exe, *warm_dir, *driver;
    bool binaries;
    struct shader_job *jobs;
    PFNGLBUFFERSTORAGEEXTPROC buffer_storage;
    struct {
        GLuint pbo;
        GLsync fence[UPLOAD_FENCES];
        uint64_t limit[UPLOAD_FENCES];
        int fences;
        int channel;
        bool failed;
    } staging;
};

static inline struct gfx *G(struct render *r) {
    return (struct gfx *)r;
}

static inline GLuint level_texture(const struct gfx_level *level) {
    return (GLuint)level->token;
}

static inline GLuint level_framebuffer(const struct gfx_level *level) {
    return (GLuint)(level->token >> 32);
}

#define GL(r, statement) ((r)->gl_calls++, (void)(statement))

struct image {
    struct addon addon;
    struct wl_list link;
    struct render *r;
    EGLImageKHR egl;
    bool external;
    GLuint tex, rbo, fbo;
};

struct gl_texture {
    struct texture base;
    struct render *r;
    GLenum target, gl;
    GLuint tex, fbo;
    bool alpha;
    GLint filter;
    uint32_t format;
    uint64_t bytes;
    struct buffer *buffer;
};

struct gfx_prog *render_program(struct render *renderer, int kind);
void pass_quad(struct pass *pass, struct gfx_prog *p, const float pos[8], const float local[8],
    const float texcoords[8]);
void pass_triangles(struct pass *pass, struct gfx_prog *p, const float *pos, size_t vertices);
bool render_texture_gl(struct texture *texture, GLenum *target, GLuint *tex, bool *alpha);
GLuint render_buffer_fbo(struct render *renderer, struct buffer *buffer);

#endif
