#include "gfx_gles.h"
#include "drm.h"
#include "ui.h"
#include <dirent.h>
#include <fcntl.h>
#include <gbm.h>
#include <malloc.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern char **environ;

enum {
    WARM_SIZE = 16,
    WARM_SOURCE_LIMIT = 1 << 20,
    WARM_NAME = sizeof("0123456789abcdef.glsl"),
};

enum { SCENE_RESERVE = 4096 };


struct shader_job {
    struct shader_job *next;
    char name[WARM_NAME];
    pid_t pid;
    uint64_t deadline;
    bool failed;
};

static uint64_t serials;

static const char vertex_source[] =
    "attribute vec2 pos;\n"
    "attribute vec2 local;\n"
    "attribute vec2 texcoord;\n"
    "varying vec2 v_local;\n"
    "varying vec2 v_tex;\n"
    "void main() {\n"
    "    v_local = local;\n"
    "    v_tex = texcoord;\n"
    "    gl_Position = vec4(pos, 0.0, 1.0);\n"
    "}\n";

static const char rounding_source[] =
    "float rounding_alpha(vec2 c, vec2 size, float r) {\n"
    "    vec2 center;\n"
    "    if (c.x < r && c.y < r) center = vec2(r, r);\n"
    "    else if (size.x - r < c.x && c.y < r) center = vec2(size.x - r, r);\n"
    "    else if (size.x - r < c.x && size.y - r < c.y) center = vec2(size.x - r, size.y - r);\n"
    "    else if (c.x < r && size.y - r < c.y) center = vec2(r, size.y - r);\n"
    "    else return 1.0;\n"
    "    float t = clamp(distance(c, center) - r + 0.5, 0.0, 1.0);\n"
    "    return 1.0 - t * t * (3.0 - 2.0 * t);\n"
    "}\n";

static const char sdf_vertex_source[] =
    "attribute vec2 pos;\n"
    "attribute vec2 local;\n"
    "attribute vec4 color;\n"
    "attribute vec4 params;\n"
    "attribute vec4 extra;\n"
    "attribute vec2 size;\n"
    "varying vec4 v_sdf_color;\n"
    "varying vec4 v_sdf_params;\n"
    "varying vec4 v_sdf_extra;\n"
    "varying vec2 v_sdf_size;\n"
    "varying vec2 v_local;\n"
    "void main() {\n"
    "    v_sdf_color = color;\n"
    "    v_sdf_params = params;\n"
    "    v_sdf_extra = extra;\n"
    "    v_sdf_size = size;\n"
    "    v_local = local;\n"
    "    gl_Position = vec4(pos, 0.0, 1.0);\n"
    "}\n";

static const char sdf_prelude[] =
    "precision highp float;\n"
    "varying vec4 v_sdf_color;\n"
    "varying vec4 v_sdf_params;\n"
    "varying vec4 v_sdf_extra;\n"
    "varying vec2 v_sdf_size;\n"
    "varying vec2 v_local;\n"
    "float box_distance(vec2 p, vec2 half_size, float r) {\n"
    "    vec2 q = abs(p) - (half_size - vec2(r));\n"
    "    return length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - r;\n"
    "}\n";

static const char sdf_source[] =
    "void main() {\n"
    "    vec4 color = v_sdf_color;\n"
    "    float alpha = v_sdf_params.x;\n"
    "    float radius = v_sdf_params.z;\n"
    "    float width = v_sdf_extra.x;\n"
    "    float range = v_sdf_extra.y;\n"
    "    float power = v_sdf_extra.z;\n"
    "    float kind = v_sdf_extra.w;\n"
    "    vec2 size = v_sdf_size;\n"
    "    vec4 premultiplied = vec4(color.rgb * color.a, color.a);\n"
    "    if (kind < 0.5) {\n"
    "        float a = rounding_alpha(v_local, size, radius + width);\n"
    "        vec2 inner = v_local - vec2(width);\n"
    "        vec2 inner_size = size - vec2(2.0 * width);\n"
    "        if (0.0 <= inner.x && inner.x <= inner_size.x && 0.0 <= inner.y && inner.y <= inner_size.y)\n"
    "            a *= 1.0 - rounding_alpha(inner, inner_size, radius);\n"
    "        gl_FragColor = premultiplied * a * alpha;\n"
    "        return;\n"
    "    }\n"
    "    vec2 window_size = size - vec2(2.0 * range);\n"
    "    float r = min(radius, min(window_size.x, window_size.y) * 0.5);\n"
    "    float d = box_distance(v_local - size * 0.5, window_size * 0.5, r);\n"
    "    if (d <= -0.5 || d >= range) discard;\n"
    "    float falloff = pow(clamp(1.0 - max(d, 0.0) / range, 0.0, 1.0), power);\n"
    "    gl_FragColor = premultiplied * falloff * smoothstep(-0.5, 0.5, d) * alpha;\n"
    "}\n";

static const char scene_vertex_source[] =
    "attribute vec2 pos;\n"
    "attribute vec2 local;\n"
    "attribute vec2 texcoord;\n"
    "attribute vec4 color;\n"
    "attribute vec4 tint;\n"
    "attribute vec4 params;\n"
    "attribute vec4 extra;\n"
    "attribute vec2 size;\n"
    "varying vec4 v_color;\n"
    "varying vec4 v_tint;\n"
    "varying vec4 v_params;\n"
    "varying vec4 v_extra;\n"
    "varying vec2 v_size;\n"
    "varying vec2 v_local;\n"
    "varying vec2 v_tex;\n"
    "void main() {\n"
    "    v_color = color;\n"
    "    v_tint = tint;\n"
    "    v_params = params;\n"
    "    v_extra = extra;\n"
    "    v_size = size;\n"
    "    v_local = local;\n"
    "    v_tex = texcoord;\n"
    "    gl_Position = vec4(pos, 0.0, 1.0);\n"
    "}\n";

static const char scene_prelude[] =
    "precision highp float;\n"
    "uniform sampler2D tex;\n"
    "varying vec4 v_color;\n"
    "varying vec4 v_tint;\n"
    "varying vec4 v_params;\n"
    "varying vec4 v_extra;\n"
    "varying vec2 v_size;\n"
    "varying vec2 v_local;\n"
    "varying vec2 v_tex;\n";

static const char scene_source[] =
    "void main() {\n"
    "    float alpha = v_params.x;\n"
    "    float kind = v_extra.w;\n"
    "    if (kind < 0.5) {\n"
    "        if (v_params.w > 0.5) {\n"
    "            if (v_local.x < 0.0 || v_local.y < 0.0 || v_local.x > v_size.x ||\n"
    "                    v_local.y > v_size.y) discard;\n"
    "            gl_FragColor = v_color * rounding_alpha(v_local, v_size, v_params.z);\n"
    "            return;\n"
    "        }\n"
    "        gl_FragColor = v_color;\n"
    "        return;\n"
    "    }\n"
    "    {\n"
    "        vec4 c = texture2D(tex, v_tex);\n"
    "        if (v_params.y > 0.5) c.a = 1.0;\n"
    "        if (v_params.w > 0.5) {\n"
    "            if (v_local.x < 0.0 || v_local.y < 0.0 || v_local.x > v_size.x ||\n"
    "                    v_local.y > v_size.y) discard;\n"
    "            c *= rounding_alpha(v_local, v_size, v_params.z);\n"
    "        }\n"
    "        if (v_tint.a > 0.0) c = mix(c, vec4(v_tint.rgb * c.a, c.a), v_tint.a);\n"
    "        gl_FragColor = c * alpha;\n"
    "    }\n"
    "}\n";

enum { TRAIL_VERTICES = 6 };

static const char trail_vertex_source[] =
    "#version 300 es\n"
    "precision highp float;\n"
    "in vec4 cell;\n"
    "in vec2 extent;\n"
    "in vec4 color;\n"
    "in vec2 shape;\n"
    "out vec2 v_local;\n"
    "out vec2 v_size;\n"
    "out vec4 v_color;\n"
    "out float v_radius;\n"
    "out float v_rounded;\n"
    "const vec2 corners[6] = vec2[6](vec2(0.0, 0.0), vec2(1.0, 0.0), vec2(0.0, 1.0),\n"
    "    vec2(0.0, 1.0), vec2(1.0, 0.0), vec2(1.0, 1.0));\n"
    "void main() {\n"
    "    vec2 corner = corners[gl_VertexID];\n"
    "    gl_Position = vec4(mix(cell.xy, cell.zw, corner), 0.0, 1.0);\n"
    "    v_local = corner * extent;\n"
    "    v_size = extent;\n"
    "    v_color = color;\n"
    "    v_radius = shape.x;\n"
    "    v_rounded = shape.y;\n"
    "}\n";

static const char trail_prelude[] =
    "#version 300 es\n"
    "precision highp float;\n"
    "in vec2 v_local;\n"
    "in vec2 v_size;\n"
    "in vec4 v_color;\n"
    "in float v_radius;\n"
    "in float v_rounded;\n"
    "out vec4 fragment;\n";

static const char trail_source[] =
    "void main() {\n"
    "    if (v_rounded > 0.5) {\n"
    "        if (v_local.x < 0.0 || v_local.y < 0.0 || v_local.x > v_size.x ||\n"
    "                v_local.y > v_size.y) discard;\n"
    "        fragment = v_color * rounding_alpha(v_local, v_size, v_radius);\n"
    "        return;\n"
    "    }\n"
    "    fragment = v_color;\n"
    "}\n";

static const char texture_source[] =
    "uniform SAMPLER tex;\n"
    "#ifdef LUT\n"
    "uniform sampler2D lut;\n"
    "#endif\n"
    "uniform vec4 tint;\n"
    "uniform float alpha;\n"
    "uniform float opaque;\n"
    "uniform float clip;\n"
    "uniform vec2 size;\n"
    "uniform float radius;\n"
    "varying vec2 v_local;\n"
    "varying vec2 v_tex;\n"
    "void main() {\n"
    "    vec4 c = texture2D(tex, v_tex);\n"
    "    if (opaque > 0.5) c.a = 1.0;\n"
    "#ifdef LUT\n"
    "    vec3 at = c.rgb * (255.0 / 256.0) + 0.5 / 256.0;\n"
    "    c.rgb = vec3(texture2D(lut, vec2(at.r, 0.5)).r, texture2D(lut, vec2(at.g, 0.5)).g,\n"
    "        texture2D(lut, vec2(at.b, 0.5)).b);\n"
    "#endif\n"
    "    if (clip > 0.5) {\n"
    "        if (v_local.x < 0.0 || v_local.y < 0.0 || v_local.x > size.x || v_local.y > size.y) discard;\n"
    "        c *= rounding_alpha(v_local, size, radius);\n"
    "    }\n"
    "    if (tint.a > 0.0) c = mix(c, vec4(tint.rgb * c.a, c.a), tint.a);\n"
    "    gl_FragColor = c * alpha;\n"
    "}\n";

static const char down_source[] =
    "uniform sampler2D tex;\n"
    "uniform vec2 half_pixel;\n"
    "uniform float offset;\n"
    "varying vec2 v_tex;\n"
    "void main() {\n"
    "    vec4 sum = texture2D(tex, v_tex) * 4.0;\n"
    "    sum += texture2D(tex, v_tex - half_pixel * offset);\n"
    "    sum += texture2D(tex, v_tex + half_pixel * offset);\n"
    "    sum += texture2D(tex, v_tex + vec2(half_pixel.x, -half_pixel.y) * offset);\n"
    "    sum += texture2D(tex, v_tex - vec2(half_pixel.x, -half_pixel.y) * offset);\n"
    "    gl_FragColor = sum / 8.0;\n"
    "}\n";

static const char up_source[] =
    "uniform sampler2D tex;\n"
    "uniform vec2 half_pixel;\n"
    "uniform float offset;\n"
    "uniform float grain;\n"
    "uniform float vibrancy;\n"
    "varying vec2 v_tex;\n"
    "void main() {\n"
    "    vec4 sum = texture2D(tex, v_tex + vec2(-half_pixel.x * 2.0, 0.0) * offset);\n"
    "    sum += texture2D(tex, v_tex + vec2(-half_pixel.x, half_pixel.y) * offset) * 2.0;\n"
    "    sum += texture2D(tex, v_tex + vec2(0.0, half_pixel.y * 2.0) * offset);\n"
    "    sum += texture2D(tex, v_tex + vec2(half_pixel.x, half_pixel.y) * offset) * 2.0;\n"
    "    sum += texture2D(tex, v_tex + vec2(half_pixel.x * 2.0, 0.0) * offset);\n"
    "    sum += texture2D(tex, v_tex + vec2(half_pixel.x, -half_pixel.y) * offset) * 2.0;\n"
    "    sum += texture2D(tex, v_tex + vec2(0.0, -half_pixel.y * 2.0) * offset);\n"
    "    sum += texture2D(tex, v_tex + vec2(-half_pixel.x, -half_pixel.y) * offset) * 2.0;\n"
    "    vec4 c = sum / 12.0;\n"
    "    if (vibrancy != 1.0) {\n"
    "        float l = dot(c.rgb, vec3(0.2126, 0.7152, 0.0722));\n"
    "        c.rgb = clamp(mix(vec3(l), c.rgb, vibrancy), 0.0, 1.0);\n"
    "    }\n"
    "    if (grain > 0.0) {\n"
    "        vec2 q = gl_FragCoord.xy;\n"
    "        float a = fract(52.9829189 * fract(dot(q, vec2(0.06711056, 0.00583715))));\n"
    "        float b = fract(52.9829189 * fract(dot(q + vec2(23.0, 61.0), vec2(0.00583715, 0.06711056))));\n"
    "        c.rgb += (a + b - 1.0) * grain / 255.0;\n"
    "    }\n"
    "    gl_FragColor = c;\n"
    "}\n";

static const char shader_vertex_source[] =
    "#version 300 es\n"
    "in vec2 pos;\n"
    "in vec2 local;\n"
    "out vec2 v_local;\n"
    "void main() {\n"
    "    v_local = local;\n"
    "    gl_Position = vec4(pos, 0.0, 1.0);\n"
    "}\n";

static const char shader_prelude[] =
    "#version 300 es\n"
    "precision highp float;\n"
    "precision highp int;\n"
    "uniform vec3 iResolution;\n"
    "uniform float iTime;\n"
    "uniform int iFrame;\n"
    "uniform vec4 iMouse;\n"
    "uniform int iTrailCount;\n"
    "uniform vec4 iTrail[64];\n"
    "uniform vec4 iData[48];\n"
    "uniform int iWindowCount;\n"
    "uniform vec4 iWindow[8];\n"
    "uniform vec4 iSelection[4];\n"
    "uniform sampler2D tomoe_marks;\n"
    "uniform vec4 iMarksBox;\n"
    "uniform sampler2D tomoe_beneath;\n"
    "uniform vec4 tomoe_beneath_box;\n"
    "vec4 beneath(vec2 at) {\n"
    "    return texture(tomoe_beneath, (at - tomoe_beneath_box.xy) * tomoe_beneath_box.zw);\n"
    "}\n"
    "uniform sampler2D tomoe_frost;\n"
    "uniform vec4 tomoe_frost_box;\n"
    "vec4 frost(vec2 at) {\n"
    "    return texture(tomoe_frost, (at - tomoe_frost_box.xy) * tomoe_frost_box.zw);\n"
    "}\n"
    "vec4 markvec(vec2 at) {\n"
    "    if (iMarksBox.z <= 0.0 || iMarksBox.w <= 0.0) return vec4(0.0);\n"
    "    vec2 uv = vec2((at.x - iMarksBox.x) / iMarksBox.z,\n"
    "        (iMarksBox.y + iMarksBox.w - at.y) / iMarksBox.w);\n"
    "    if (uv.x < 0.0 || uv.y < 0.0 || uv.x > 1.0 || uv.y > 1.0) return vec4(0.0);\n"
    "    return texture(tomoe_marks, uv);\n"
    "}\n"
    "float marked(vec2 at) { return markvec(at).a; }\n"
    "in vec2 v_local;\n"
    "out vec4 tomoe_color;\n"
    "#line 1\n";

static const char shader_main[] =
    "\nvoid main() {\n"
    "    mainImage(tomoe_color, vec2(v_local.x, iResolution.y - v_local.y));\n"
    "}\n";

enum { SHADER_CACHE_MAX = 32 };

struct shader {
    struct gfx_prog program;
    struct render *r;
    struct shader *next;
    char *source;
    uint64_t hash, used;
    int refs;
    bool cached;
};

static void current(struct render *r) {
    if (eglGetCurrentContext() != G(r)->context)
        eglMakeCurrent(G(r)->display, EGL_NO_SURFACE, EGL_NO_SURFACE, G(r)->context);
}

static bool has(const char *list, const char *name) {
    size_t n = strlen(name);
    for (const char *p = list; p && (p = strstr(p, name)); p += n)
        if ((p == list || p[-1] == ' ') && (p[n] == ' ' || p[n] == '\0')) return true;
    return false;
}

static bool format_alpha(uint32_t format) {
    switch (format) {
    case DRM_FORMAT_ARGB8888: case DRM_FORMAT_ABGR8888: case DRM_FORMAT_RGBA8888:
    case DRM_FORMAT_BGRA8888: case DRM_FORMAT_ARGB2101010: case DRM_FORMAT_ABGR2101010:
    case DRM_FORMAT_RGBA1010102: case DRM_FORMAT_BGRA1010102: case DRM_FORMAT_ABGR16161616F:
    case DRM_FORMAT_ARGB16161616F: case DRM_FORMAT_ABGR16161616: case DRM_FORMAT_ARGB16161616:
    case DRM_FORMAT_ARGB4444: case DRM_FORMAT_ABGR4444: case DRM_FORMAT_RGBA4444:
    case DRM_FORMAT_BGRA4444: case DRM_FORMAT_ARGB1555: case DRM_FORMAT_ABGR1555:
    case DRM_FORMAT_RGBA5551: case DRM_FORMAT_BGRA5551: case DRM_FORMAT_AYUV:
        return true;
    }
    return false;
}

static bool pixel_format(uint32_t format, GLenum *gl, bool *alpha) {
    switch (format) {
    case DRM_FORMAT_ARGB8888: *gl = GL_BGRA_EXT; *alpha = true; return true;
    case DRM_FORMAT_XRGB8888: *gl = GL_BGRA_EXT; *alpha = false; return true;
    case DRM_FORMAT_ABGR8888: *gl = GL_RGBA; *alpha = true; return true;
    case DRM_FORMAT_XBGR8888: *gl = GL_RGBA; *alpha = false; return true;
    }
    return false;
}

static GLuint compile(GLenum type, const char *const *sources, int count, bool report) {
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, count, sources, NULL);
    glCompileShader(shader);
    GLint ok = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (ok) return shader;
    if (report) {
        char log[1024];
        glGetShaderInfoLog(shader, sizeof(log), NULL, log);
        tomoe_log(LOG_ERROR, "tomoe: shader: %s", log);
    }
    glDeleteShader(shader);
    return 0;
}

static void program_locations(struct gfx_prog *p);

static bool link_stages(struct gfx_prog *p, GLuint vs, GLuint fs) {
    p->id = glCreateProgram();
    glAttachShader(p->id, vs);
    glAttachShader(p->id, fs);
    glBindAttribLocation(p->id, 0, "grid");
    glBindAttribLocation(p->id, 1, "uv");
    glBindAttribLocation(p->id, 2, "texcoord");
    glBindAttribLocation(p->id, 3, "pos");
    glBindAttribLocation(p->id, 4, "local");
    glBindAttribLocation(p->id, 5, "color");
    glBindAttribLocation(p->id, 6, "tint");
    glBindAttribLocation(p->id, 7, "params");
    glBindAttribLocation(p->id, 8, "extra");
    glBindAttribLocation(p->id, 9, "size");
    glBindAttribLocation(p->id, 10, "scissor");
    glBindAttribLocation(p->id, 11, "cell");
    glBindAttribLocation(p->id, 12, "extent");
    glBindAttribLocation(p->id, 13, "shape");
    glLinkProgram(p->id);
    glDeleteShader(vs);
    glDeleteShader(fs);
    GLint ok = 0;
    glGetProgramiv(p->id, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[1024];
        glGetProgramInfoLog(p->id, sizeof(log), NULL, log);
        tomoe_log(LOG_ERROR, "tomoe: shader link: %s", log);
        glDeleteProgram(p->id);
        p->id = 0;
        return false;
    }
    program_locations(p);
    return true;
}

static void program_locations(struct gfx_prog *p) {
    p->pos = glGetAttribLocation(p->id, "pos");
    p->local = glGetAttribLocation(p->id, "local");
    p->texcoord = glGetAttribLocation(p->id, "texcoord");
    const char *names[] = { "tex", "alpha", "opaque", "size", "radius", "clip", "width", "kind",
        "color", "range", "power", "half_pixel", "offset", "iResolution", "iTime", "iFrame", "iMouse", "tomoe_beneath",
        "lut", "iTrail", "iTrailCount", "tint", "iBox", "iMin", "iMax", "iDrag", "tex_prev",
        "tex_next", "geo_to_tex", "geo_to_tex_prev", "geo_to_tex_next", "curr_geo_to_prev_geo",
        "curr_geo_to_next_geo", "progress", "clamped_progress", "random_seed", "iPath",
        "iPathCount", "iData", "iWindow", "iWindowCount", "tomoe_beneath_box", "iSelection",
        "tomoe_marks", "iMarksBox", "tomoe_frost", "tomoe_frost_box", "grain", "vibrancy" };
    GLint *slots[] = { &p->tex, &p->alpha, &p->opaque, &p->size, &p->radius, &p->clip, &p->width,
        &p->kind, &p->color, &p->range, &p->power, &p->half_pixel, &p->offset, &p->resolution,
        &p->time, &p->frame, &p->mouse, &p->channel, &p->lut, &p->trail, &p->trail_count, &p->tint,
        &p->box, &p->min_ndc, &p->max_ndc, &p->drag, &p->tex_prev, &p->tex_next,
        &p->geo_to_tex, &p->geo_to_tex_prev, &p->geo_to_tex_next, &p->curr_geo_to_prev_geo,
        &p->curr_geo_to_next_geo, &p->progress, &p->clamped_progress, &p->random_seed,
        &p->path, &p->path_count, &p->data, &p->windows, &p->window_count, &p->beneath,
        &p->selection, &p->marks, &p->marks_box, &p->frost, &p->frost_box, &p->grain,
        &p->vibrancy };
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++)
        *slots[i] = glGetUniformLocation(p->id, names[i]);
}

static bool link_sources(struct gfx_prog *p, const char *const *vertex, int vertex_count,
        const char *const *fragment, int fragment_count) {
    GLuint vs = compile(GL_VERTEX_SHADER, vertex, vertex_count, true);
    GLuint fs = vs ? compile(GL_FRAGMENT_SHADER, fragment, fragment_count, true) : 0;
    if (!fs) {
        glDeleteShader(vs);
        return false;
    }
    return link_stages(p, vs, fs);
}

static const char fragment_precision[] = "precision highp float;\n";

static bool link_program(struct gfx_prog *p, const char *prelude, const char *body) {
    const char *fragment[] = { prelude, fragment_precision, rounding_source, body };
    const char *vertex[] = { vertex_source };
    return link_sources(p, vertex, 1, fragment, 4);
}

static bool link_scene(struct gfx_prog *p) {
    const char *fragment[] = { scene_prelude, rounding_source, scene_source };
    const char *vertex[] = { scene_vertex_source };
    return link_sources(p, vertex, 1, fragment, 3);
}

static bool link_sdf(struct gfx_prog *p) {
    const char *fragment[] = { sdf_prelude, rounding_source, sdf_source };
    const char *vertex[] = { sdf_vertex_source };
    return link_sources(p, vertex, 1, fragment, 3);
}

static bool link_trail(struct gfx_prog *p) {
    const char *fragment[] = { trail_prelude, rounding_source, trail_source };
    const char *vertex[] = { trail_vertex_source };
    return link_sources(p, vertex, 1, fragment, 3);
}

#define EXTERNAL_PRELUDE "#extension GL_OES_EGL_image_external : require\n" \
    "#define SAMPLER samplerExternalOES\n"
static const struct builtin {
    int kind;
    bool external;
    const char *prelude, *body;
} builtins[] = {
    { PROGRAM_TEXTURE_LUT, false, "#define SAMPLER sampler2D\n#define LUT\n", texture_source },
    { PROGRAM_EXTERNAL, true, EXTERNAL_PRELUDE, texture_source },
    { PROGRAM_EXTERNAL_LUT, true, EXTERNAL_PRELUDE "#define LUT\n", texture_source },
    { PROGRAM_DOWN, false, "", down_source },
    { PROGRAM_UP, false, "", up_source },
};

enum shader_await { SHADER_RUNNING, SHADER_READY, SHADER_NO_HELPER, SHADER_FAILED };

static uint64_t fnv(uint64_t hash, const char *text);
static void warm_name(const char *source, char name[WARM_NAME]);
static bool warm_remember(struct render *r, const char *source, const char *name);
static bool warm_load(struct render *r, const char *name, struct gfx_prog *p);
static enum shader_await shader_await(struct render *r, const char *name,
    struct gfx_prog *program);
static void jobs_stop(struct render *r);

static EGLContext compile_begin(struct render *r) {
    current(r);
    return G(r)->context;
}

static void compile_end(struct render *r, EGLContext scratch) {
    if (scratch == EGL_NO_CONTEXT) return;
    glFinish();
    malloc_trim(0);
}

static void shader_cache_prune(struct render *r) {
    while (G(r)->shader_count > SHADER_CACHE_MAX) {
        struct shader **victim = NULL;
        for (struct shader **link = &G(r)->shaders; *link; link = &(*link)->next)
            if ((*link)->refs == 0 && (!victim || (*link)->used < (*victim)->used))
                victim = link;
        if (!victim) break;
        struct shader *evict = *victim;
        *victim = evict->next;
        G(r)->shader_count--;
        current(r);
        glDeleteProgram(evict->program.id);
        free(evict->source);
        free(evict);
    }
}

static struct gfx_prog *shader_create(struct render *r, const char *source, bool helper,
        bool *pending) {
    bool waiting = false;
    if (!pending) pending = &waiting;
    *pending = false;
    uint64_t hash = fnv(0xcbf29ce484222325ull, source);
    for (struct shader *cached = G(r)->shaders; cached; cached = cached->next) {
        if (cached->hash == hash && cached->source && !strcmp(cached->source, source)) {
            cached->refs++;
            cached->used = ++G(r)->shader_clock;
            return &cached->program;
        }
    }
    struct shader *shader = calloc(1, sizeof(*shader));
    if (!shader) return NULL;
    const char *vertex[] = { shader_vertex_source };
    const char *fragment[] = { shader_prelude, source, shader_main };
    char name[WARM_NAME];
    warm_name(source, name);
    bool handable = warm_remember(r, source, name);
    current(r);
    bool linked = warm_load(r, name, &shader->program);
    if (!linked && !(helper && handable && G(r)->warm_dir && G(r)->binaries)) {
        linked = link_sources(&shader->program, vertex, 1, fragment, 3);
    } else if (!linked) {
        enum shader_await awaited = shader_await(r, name, &shader->program);
        if (awaited == SHADER_RUNNING) *pending = true;
        else if (awaited == SHADER_READY) linked = true;
        else if (awaited == SHADER_NO_HELPER)
            linked = link_sources(&shader->program, vertex, 1, fragment, 3);
    }
    if (linked) {
        glFinish();
        malloc_trim(0);
        shader->r = r;
        shader->source = strdup(source);
        if (shader->source) {
            shader->hash = hash;
            shader->used = ++G(r)->shader_clock;
            shader->refs = 1;
            shader->cached = true;
            shader->next = G(r)->shaders;
            G(r)->shaders = shader;
            G(r)->shader_count++;
            shader_cache_prune(r);
        }
        return &shader->program;
    }
    free(shader);
    return NULL;
}

struct gfx_prog *render_shader_create(struct render *r, const char *source, bool *pending) {
    return shader_create(r, source, true, pending);
}

struct gfx_prog *render_shader_compile(struct render *r, const char *source) {
    bool pending;
    return shader_create(r, source, false, &pending);
}

void render_shader_destroy(struct gfx_prog *program) {
    if (!program) return;
    struct shader *shader = wl_container_of(program, shader, program);
    if (shader->cached) {
        if (shader->refs > 0) shader->refs--;
        shader_cache_prune(shader->r);
        return;
    }
    current(shader->r);
    glDeleteProgram(program->id);
    free(shader);
}

enum { WINDOW_SHADER_LIMIT = 65536, WINDOW_SHADER_ENTRY = 64 };

static void render_quad(struct render *r, struct gfx_prog *p, const float pos[8],
        const float local[8], const float texcoords[8]) {
    G(r)->scene.attributes = false;
    glVertexAttribPointer(p->pos, 2, GL_FLOAT, GL_FALSE, 0, pos);
    glEnableVertexAttribArray(p->pos);
    if (p->local >= 0 && local) {
        glVertexAttribPointer(p->local, 2, GL_FLOAT, GL_FALSE, 0, local);
        glEnableVertexAttribArray(p->local);
    }
    if (p->texcoord >= 0 && texcoords) {
        glVertexAttribPointer(p->texcoord, 2, GL_FLOAT, GL_FALSE, 0, texcoords);
        glEnableVertexAttribArray(p->texcoord);
    }
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glDisableVertexAttribArray(p->pos);
    if (p->local >= 0 && local) glDisableVertexAttribArray(p->local);
    if (p->texcoord >= 0 && texcoords) glDisableVertexAttribArray(p->texcoord);
}

void pass_triangles(struct pass *pass, struct gfx_prog *p, const float *pos, size_t vertices) {
    if (!pass || !p || !p->id || vertices < 3 || p->pos < 0) return;
    glVertexAttribPointer(p->pos, 2, GL_FLOAT, GL_FALSE, 0, pos);
    glEnableVertexAttribArray(p->pos);
    if (!pass->clip) {
        glDrawArrays(GL_TRIANGLES, 0, (GLsizei)vertices);
    } else {
        struct rect bounds = position_bounds(pass, pos, vertices);
        int count = 0;
        const struct rect *rects = region_rectangles(pass->clip, &count);
        glEnable(GL_SCISSOR_TEST);
        for (int i = 0; i < count; i++) {
            if (!bounds_overlap(&bounds, &rects[i])) continue;
            glScissor(rects[i].x1, rects[i].y1, rects[i].x2 - rects[i].x1, rects[i].y2 - rects[i].y1);
            glDrawArrays(GL_TRIANGLES, 0, (GLsizei)vertices);
        }
        glDisable(GL_SCISSOR_TEST);
    }
    glDisableVertexAttribArray(p->pos);
}

void pass_quad(struct pass *pass, struct gfx_prog *p, const float pos[8], const float local[8],
        const float texcoords[8]) {
    struct render *r = pass->r;
    pass_flush(pass);
    if (!pass->clip) {
        r->fragments += quad_pixels(pass, pos, NULL);
        render_quad(r, p, pos, local, texcoords);
        return;
    }
    int count = 0;
    const struct rect *rects = region_rectangles(pass->clip, &count);
    GL(r, glEnable(GL_SCISSOR_TEST));
    for (int i = 0; i < count; i++) {
        uint64_t pixels = quad_pixels(pass, pos, &rects[i]);
        if (!pixels) continue;
        GL(r, glScissor(rects[i].x1, rects[i].y1, rects[i].x2 - rects[i].x1,
            rects[i].y2 - rects[i].y1));
        r->fragments += pixels;
        render_quad(r, p, pos, local, texcoords);
    }
    GL(r, glDisable(GL_SCISSOR_TEST));
}

static void trail_target(struct render *r) {
    if (G(r)->trail.vao) return;
    GL(r, glGenVertexArrays(1, &G(r)->trail.vao));
    GL(r, glGenBuffers(1, &G(r)->trail.vbo));
    GL(r, glBindVertexArray(G(r)->trail.vao));
    GL(r, glBindBuffer(GL_ARRAY_BUFFER, G(r)->trail.vbo));
    GLsizei stride = sizeof(struct trail_cell);
    GL(r, glVertexAttribPointer(11, 4, GL_FLOAT, GL_FALSE, stride,
        (const void *)offsetof(struct trail_cell, clip)));
    GL(r, glVertexAttribPointer(12, 2, GL_FLOAT, GL_FALSE, stride,
        (const void *)offsetof(struct trail_cell, extent)));
    GL(r, glVertexAttribPointer(5, 4, GL_FLOAT, GL_FALSE, stride,
        (const void *)offsetof(struct trail_cell, color)));
    GL(r, glVertexAttribPointer(13, 2, GL_FLOAT, GL_FALSE, stride,
        (const void *)offsetof(struct trail_cell, radius)));
    static const GLuint instances[] = { 5, 11, 12, 13 };
    for (size_t i = 0; i < sizeof(instances) / sizeof(instances[0]); i++) {
        GL(r, glEnableVertexAttribArray(instances[i]));
        GL(r, glVertexAttribDivisor(instances[i], 1));
    }
    GL(r, glBindVertexArray(0));
}

void pass_trail(struct pass *pass, const struct trail_cell *cells, size_t count,
        uint64_t pixels) {
    struct render *r = pass->r;
    struct gfx_prog *p = &G(r)->programs[PROGRAM_TRAIL];
    if (!count || !p->id) return;
    pass_flush(pass);
    trail_target(r);
    GLsizeiptr bytes = (GLsizeiptr)(count * sizeof(*cells));
    GL(r, glBindVertexArray(G(r)->trail.vao));
    GL(r, glBindBuffer(GL_ARRAY_BUFFER, G(r)->trail.vbo));
    if ((size_t)bytes > G(r)->trail.bytes) {
        size_t want = G(r)->trail.bytes ? G(r)->trail.bytes : 4096;
        while (want < (size_t)bytes) want *= 2;
        GL(r, glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)want, NULL, GL_STREAM_DRAW));
        G(r)->trail.bytes = want;
    }
    GL(r, glBufferSubData(GL_ARRAY_BUFFER, 0, bytes, cells));
    r->uploads++;
    r->upload_bytes += (uint64_t)bytes;
    GL(r, glUseProgram(p->id));
    GL(r, glEnable(GL_BLEND));
    GL(r, glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA));
    int rects = 0;
    const struct rect *clip = pass->clip ? region_rectangles((struct region *)pass->clip,
        &rects) : NULL;
    struct rect bounds;
    if (clip) {
        double x0 = cells[0].clip[0], y0 = cells[0].clip[1];
        double x1 = cells[0].clip[2], y1 = cells[0].clip[3];
        for (size_t i = 1; i < count; i++) {
            if (cells[i].clip[0] < x0) x0 = cells[i].clip[0];
            if (cells[i].clip[1] < y0) y0 = cells[i].clip[1];
            if (cells[i].clip[2] > x1) x1 = cells[i].clip[2];
            if (cells[i].clip[3] > y1) y1 = cells[i].clip[3];
        }
        bounds = pixel_bounds(pass, x0, y0, x1, y1);
        GL(r, glEnable(GL_SCISSOR_TEST));
    }
    for (int i = 0; i < (clip ? rects : 1); i++) {
        if (clip) {
            if (!bounds_overlap(&bounds, &clip[i])) continue;
            GL(r, glScissor(clip[i].x1, clip[i].y1, clip[i].x2 - clip[i].x1,
                clip[i].y2 - clip[i].y1));
        }
        GL(r, glDrawArraysInstanced(GL_TRIANGLES, 0, TRAIL_VERTICES, (GLsizei)count));
        r->runs++;
    }
    if (clip) GL(r, glDisable(GL_SCISSOR_TEST));
    GL(r, glBindVertexArray(0));
    GL(r, glBindBuffer(GL_ARRAY_BUFFER, 0));
    r->quads += count;
    r->fragments += pixels;
}

struct gfx_prog *render_program(struct render *renderer, int kind) {
    struct gfx_prog *p = &G(renderer)->programs[kind];
    return p->id ? p : NULL;
}

static void image_destroy(struct image *image) {
    struct render *r = image->r;
    current(r);
    glDeleteTextures(1, &image->tex);
    glDeleteFramebuffers(1, &image->fbo);
    glDeleteRenderbuffers(1, &image->rbo);
    G(r)->destroy_image(G(r)->display, image->egl);
    addon_finish(&image->addon);
    wl_list_remove(&image->link);
    free(image);
}

static void image_addon_destroy(struct addon *addon) {
    struct image *image = wl_container_of(addon, image, addon);
    image_destroy(image);
}

static struct image *image_for(struct render *r, struct buffer *buffer) {
    struct addon *addon = addon_find(&buffer->addons, r, image_addon_destroy);
    if (addon) {
        struct image *image = wl_container_of(addon, image, addon);
        return image;
    }
    struct dmabuf_attributes d;
    if (!buffer_get_dmabuf(buffer, &d)) return NULL;
    static const EGLint planes[][5] = {
        { EGL_DMA_BUF_PLANE0_FD_EXT, EGL_DMA_BUF_PLANE0_OFFSET_EXT, EGL_DMA_BUF_PLANE0_PITCH_EXT,
            EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT, EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT },
        { EGL_DMA_BUF_PLANE1_FD_EXT, EGL_DMA_BUF_PLANE1_OFFSET_EXT, EGL_DMA_BUF_PLANE1_PITCH_EXT,
            EGL_DMA_BUF_PLANE1_MODIFIER_LO_EXT, EGL_DMA_BUF_PLANE1_MODIFIER_HI_EXT },
        { EGL_DMA_BUF_PLANE2_FD_EXT, EGL_DMA_BUF_PLANE2_OFFSET_EXT, EGL_DMA_BUF_PLANE2_PITCH_EXT,
            EGL_DMA_BUF_PLANE2_MODIFIER_LO_EXT, EGL_DMA_BUF_PLANE2_MODIFIER_HI_EXT },
        { EGL_DMA_BUF_PLANE3_FD_EXT, EGL_DMA_BUF_PLANE3_OFFSET_EXT, EGL_DMA_BUF_PLANE3_PITCH_EXT,
            EGL_DMA_BUF_PLANE3_MODIFIER_LO_EXT, EGL_DMA_BUF_PLANE3_MODIFIER_HI_EXT },
    };
    EGLint attribs[50];
    size_t n = 0;
    attribs[n++] = EGL_WIDTH; attribs[n++] = d.width;
    attribs[n++] = EGL_HEIGHT; attribs[n++] = d.height;
    attribs[n++] = EGL_LINUX_DRM_FOURCC_EXT; attribs[n++] = d.format;
    for (int i = 0; i < d.n_planes && i < 4; i++) {
        attribs[n++] = planes[i][0]; attribs[n++] = d.fd[i];
        attribs[n++] = planes[i][1]; attribs[n++] = d.offset[i];
        attribs[n++] = planes[i][2]; attribs[n++] = d.stride[i];
        if (d.modifier != DRM_FORMAT_MOD_INVALID) {
            attribs[n++] = planes[i][3]; attribs[n++] = (EGLint)(d.modifier & 0xffffffff);
            attribs[n++] = planes[i][4]; attribs[n++] = (EGLint)(d.modifier >> 32);
        }
    }
    attribs[n++] = EGL_IMAGE_PRESERVED_KHR; attribs[n++] = EGL_TRUE;
    attribs[n++] = EGL_NONE;
    current(r);
    EGLImageKHR egl = G(r)->create_image(G(r)->display, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, NULL, attribs);
    if (egl == EGL_NO_IMAGE_KHR) {
        tomoe_log(LOG_ERROR, "tomoe: dmabuf import failed: %ux%u format 0x%08x modifier 0x%016llx",
            d.width, d.height, d.format, (unsigned long long)d.modifier);
        return NULL;
    }
    struct image *image = calloc(1, sizeof(*image));
    if (!image) {
        G(r)->destroy_image(G(r)->display, egl);
        return NULL;
    }
    image->r = r;
    image->egl = egl;
    image->external = !format_set_has(&r->render_formats, d.format, d.modifier);
    addon_init(&image->addon, &buffer->addons, r, image_addon_destroy);
    wl_list_insert(&G(r)->images, &image->link);
    return image;
}

GLuint render_buffer_fbo(struct render *renderer, struct buffer *buffer) {
    struct render *r = renderer;
    struct image *image = image_for(r, buffer);
    if (!image || image->external) return 0;
    if (image->fbo) return image->fbo;
    current(r);
    glGenRenderbuffers(1, &image->rbo);
    glBindRenderbuffer(GL_RENDERBUFFER, image->rbo);
    G(r)->image_renderbuffer(GL_RENDERBUFFER, image->egl);
    glBindRenderbuffer(GL_RENDERBUFFER, 0);
    glGenFramebuffers(1, &image->fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, image->fbo);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, image->rbo);
    bool complete = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (complete) return image->fbo;
    tomoe_log(LOG_ERROR, "tomoe: render target incomplete for %dx%d buffer",
        buffer->width, buffer->height);
    glDeleteFramebuffers(1, &image->fbo);
    image->fbo = 0;
    return 0;
}

static struct gl_texture *texture_of(struct texture *base) {
    struct gl_texture *t = wl_container_of(base, t, base);
    return t;
}

bool render_texture_gl(struct texture *base, GLenum *target, GLuint *tex, bool *alpha) {
    if (!base) return false;
    struct gl_texture *t = texture_of(base);
    *target = t->target;
    *tex = t->tex;
    *alpha = t->alpha;
    return true;
}

static void upload(struct gl_texture *t, const void *data, size_t stride,
        const struct rect *rects, int count) {
    glBindTexture(GL_TEXTURE_2D, t->tex);
    glPixelStorei(GL_UNPACK_ROW_LENGTH_EXT, stride / 4);
    for (int i = 0; i < count; i++) {
        glPixelStorei(GL_UNPACK_SKIP_PIXELS_EXT, rects[i].x1);
        glPixelStorei(GL_UNPACK_SKIP_ROWS_EXT, rects[i].y1);
        glTexSubImage2D(GL_TEXTURE_2D, 0, rects[i].x1, rects[i].y1, rects[i].x2 - rects[i].x1,
            rects[i].y2 - rects[i].y1, t->gl, GL_UNSIGNED_BYTE, data);
    }
    glPixelStorei(GL_UNPACK_ROW_LENGTH_EXT, 0);
    glPixelStorei(GL_UNPACK_SKIP_PIXELS_EXT, 0);
    glPixelStorei(GL_UNPACK_SKIP_ROWS_EXT, 0);
    glBindTexture(GL_TEXTURE_2D, 0);
}

static void staging_poll(struct render *r, bool wait) {
    struct gfx *g = G(r);
    while (g->staging.fences > 0) {
        GLenum status = glClientWaitSync(g->staging.fence[0], wait ? GL_SYNC_FLUSH_COMMANDS_BIT : 0,
            wait ? (GLuint64)1000000000 : 0);
        if (status != GL_ALREADY_SIGNALED && status != GL_CONDITION_SATISFIED) break;
        wait = false;
        if (r->upload) r->upload->gpu_done = g->staging.limit[0];
        glDeleteSync(g->staging.fence[0]);
        g->staging.fences--;
        memmove(g->staging.fence, g->staging.fence + 1, sizeof(GLsync) * (size_t)g->staging.fences);
        memmove(g->staging.limit, g->staging.limit + 1, sizeof(uint64_t) * (size_t)g->staging.fences);
    }
}

static void staging_settle(struct render *r) {
    struct gfx *g = G(r);
    glFinish();
    for (int i = 0; i < g->staging.fences; i++) glDeleteSync(g->staging.fence[i]);
    g->staging.fences = 0;
    if (r->upload) r->upload->gpu_done = r->upload->alloc;
}

static void staging_free(struct render *r) {
    struct gfx *g = G(r);
    staging_poll(r, true);
    if (g->staging.fences) staging_settle(r);
    if (!g->staging.pbo) return;
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, g->staging.pbo);
    glUnmapBuffer(GL_PIXEL_UNPACK_BUFFER);
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
    glDeleteBuffers(1, &g->staging.pbo);
    g->staging.pbo = 0;
    if (r->upload) upload_set_staging(r->upload, NULL, 0);
}

static bool staging_alloc(struct render *r, uint64_t size) {
    struct gfx *g = G(r);
    GLbitfield flags = GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT_EXT | GL_MAP_COHERENT_BIT_EXT;
    while (glGetError() != GL_NO_ERROR) {}
    glGenBuffers(1, &g->staging.pbo);
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, g->staging.pbo);
    g->buffer_storage(GL_PIXEL_UNPACK_BUFFER, (GLsizeiptr)size, NULL, flags);
    void *base = glGetError() == GL_NO_ERROR ?
        glMapBufferRange(GL_PIXEL_UNPACK_BUFFER, 0, (GLsizeiptr)size, flags) : NULL;
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
    if (!base) {
        glDeleteBuffers(1, &g->staging.pbo);
        g->staging.pbo = 0;
        tomoe_log(LOG_ERROR, "tomoe: cannot map a %llu MiB upload staging buffer",
            (unsigned long long)(size >> 20));
        return false;
    }
    upload_set_staging(r->upload, base, size);
    return true;
}

static void staging_init(struct render *r) {
    const char *version = (const char *)glGetString(GL_VERSION);
    const char *extensions = (const char *)glGetString(GL_EXTENSIONS);
    int major = 0;
    if (!version || sscanf(version, "OpenGL ES %d", &major) != 1 || major < 3 ||
            !has(extensions, "GL_EXT_buffer_storage")) return;
    G(r)->buffer_storage = (PFNGLBUFFERSTORAGEEXTPROC)eglGetProcAddress("glBufferStorageEXT");
    if (G(r)->buffer_storage) r->upload = upload_create();
}

static struct upload *staging(struct render *r) {
    if (!r->upload) return NULL;
    if (!G(r)->staging.pbo && !G(r)->staging.failed) {
        current(r);
        if (!staging_alloc(r, upload_wanted(r->upload))) G(r)->staging.failed = true;
    }
    return r->upload;
}

void gfx_upload_trim(struct render *r) {
    if (!G(r)->staging.pbo) return;
    current(r);
    staging_free(r);
    r->upload->stats.trims++;
}

static void reclaim(void *data) {
    struct render *r = data;
    if (!G(r)->staging.fences) return;
    current(r);
    staging_poll(r, false);
}

static void latch_jobs(struct render *r, bool ready) {
    struct upload *u = r->upload;
    if (!upload_pending(u)) return;
    struct gfx *g = G(r);
    uint64_t start = timing_enabled ? timing_now() : 0;
    current(r);
    staging_poll(r, false);
    bool used = false;
    struct upload_job *job;
    while ((job = upload_latch_next(u, ready))) {
        struct gl_texture *t = texture_of(job->texture);
        if (!used) glBindBuffer(GL_PIXEL_UNPACK_BUFFER, g->staging.pbo);
        used = true;
        glBindTexture(GL_TEXTURE_2D, t->tex);
        uintptr_t at = (uintptr_t)(job->src - u->base);
        for (int i = 0; i < job->count; i++) {
            const struct rect *rect = &job->rects[i];
            int width = rect->x2 - rect->x1, height = rect->y2 - rect->y1;
            glTexSubImage2D(GL_TEXTURE_2D, 0, rect->x1, rect->y1, width, height, t->gl,
                GL_UNSIGNED_BYTE, (const void *)at);
            at += (uintptr_t)width * (uintptr_t)height * 4;
        }
    }
    if (used) glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
    glBindTexture(GL_TEXTURE_2D, 0);
    if (used) {
        if (g->staging.fences == UPLOAD_FENCES) staging_poll(r, false);
        GLsync fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
        if (!fence) {
            staging_settle(r);
        } else if (g->staging.fences == UPLOAD_FENCES) {
            glDeleteSync(g->staging.fence[UPLOAD_FENCES - 1]);
            g->staging.fence[UPLOAD_FENCES - 1] = fence;
            g->staging.limit[UPLOAD_FENCES - 1] = u->alloc;
        } else {
            g->staging.fence[g->staging.fences] = fence;
            g->staging.limit[g->staging.fences++] = u->alloc;
        }
    } else if (g->staging.fences) {
        g->staging.limit[g->staging.fences - 1] = u->alloc;
    } else {
        u->gpu_done = u->alloc;
    }
    upload_latch_end(u, !ready);
    uint64_t want = upload_wanted(u);
    if (want && upload_idle(u)) {
        staging_free(r);
        u->stats.resizes++;
        staging_alloc(r, want);
    }
    if (timing_enabled) timing_add(TIMING_UPLOAD_LATCH, timing_now() - start);
}

static void latch(struct render *r) {
    latch_jobs(r, false);
}

void gfx_upload_latch(struct render *r) {
    latch_jobs(r, true);
}

static bool stage(struct gl_texture *t, struct buffer *buffer, const struct region *damage) {
    struct upload *u = staging(t->r);
    int count = 0;
    region_rectangles(damage, &count);
    if (!u || count <= 0) return false;
    size_t offset, stride;
    uint32_t format;
    struct wl_shm_view *view = buffer_shm_view(buffer, &offset, &format, &stride);
    if (view) {
        bool ok = format == t->format && stride % 4 == 0 && buffer->width == (int)t->base.width &&
            buffer->height == (int)t->base.height;
        if (ok && upload_full(u)) latch(t->r);
        if (ok && upload_stage(u, &t->base, buffer, view, offset, stride, damage, reclaim, t->r))
            return true;
        wl_shm_view_unref(view);
    }
    u->stats.sync++;
    latch(t->r);
    return false;
}

bool texture_update(struct texture *base, struct buffer *buffer,
        const struct region *damage) {
    struct gl_texture *t = texture_of(base);
    if (t->buffer) return false;
    if (stage(t, buffer, damage)) {
        base->serial = ++serials;
        return true;
    }
    void *data;
    uint32_t format;
    size_t stride;
    if (!buffer_begin_access(buffer, BUFFER_READ,
            &data, &format, &stride)) return false;
    bool ok = format == t->format && stride % 4 == 0 && buffer->width == (int)base->width &&
        buffer->height == (int)base->height;
    if (ok) {
        int count = 0;
        const struct rect *rects = region_rectangles(damage, &count);
        current(t->r);
        upload(t, data, stride, rects, count);
        base->serial = ++serials;
    }
    buffer_end_access(buffer);
    return ok;
}

static GLuint texture_fbo(struct gl_texture *t) {
    if (t->buffer) return render_buffer_fbo(t->r, t->buffer);
    if (t->fbo || t->target != GL_TEXTURE_2D) return t->fbo;
    glGenFramebuffers(1, &t->fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, t->fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t->tex, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    return t->fbo;
}

bool texture_read_pixels(struct texture *base, const struct read_options *options) {
    struct gl_texture *t = texture_of(base);
    GLenum gl;
    bool alpha;
    if (!pixel_format(options->format, &gl, &alpha) || (gl == GL_BGRA_EXT && !t->r->read_bgra))
        return false;
    current(t->r);
    latch(t->r);
    while (glGetError() != GL_NO_ERROR) {}
    GLuint fbo = texture_fbo(t);
    if (!fbo) return false;
    struct box src = box_empty(&options->src_box) ?
        (struct box){ 0, 0, base->width, base->height } : options->src_box;
    unsigned char *data = options->data;
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    if (options->stride == (uint32_t)src.width * 4) {
        glReadPixels(src.x, src.y, src.width, src.height, gl, GL_UNSIGNED_BYTE, data);
    } else {
        for (int y = 0; y < src.height; y++)
            glReadPixels(src.x, src.y + y, src.width, 1, gl, GL_UNSIGNED_BYTE,
                data + (size_t)y * options->stride);
    }
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    return glGetError() == GL_NO_ERROR;
}

void texture_destroy(struct texture *base) {
    if (!base) return;
    struct gl_texture *t = texture_of(base);
    upload_cancel(t->r->upload, base);
    if (t->buffer) {
        buffer_unlock(t->buffer);
    } else {
        current(t->r);
        glDeleteTextures(1, &t->tex);
        glDeleteFramebuffers(1, &t->fbo);
        t->r->textures--;
        t->r->texture_bytes -= t->bytes;
    }
    free(t);
}

struct texture *texture_new(struct render *r, uint32_t format, uint32_t width, uint32_t height) {
    struct gl_texture *t = calloc(1, sizeof(*t));
    if (!t) return NULL;
    if (!pixel_format(format, &t->gl, &t->alpha)) {
        tomoe_log(LOG_ERROR, "tomoe: unsupported pixel buffer format 0x%08x", format);
        free(t);
        return NULL;
    }
    t->base = (struct texture){ width, height, ++serials };
    t->r = r;
    t->format = format;
    t->target = GL_TEXTURE_2D;
    current(r);
    glGenTextures(1, &t->tex);
    glBindTexture(GL_TEXTURE_2D, t->tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, t->gl, width, height, 0, t->gl, GL_UNSIGNED_BYTE, NULL);
    glBindTexture(GL_TEXTURE_2D, 0);
    t->bytes = (uint64_t)width * height * 4;
    r->textures++;
    r->texture_bytes += t->bytes;
    return &t->base;
}

bool texture_clear(struct texture *base) {
    struct gl_texture *t = texture_of(base);
    if (!t || t->buffer || t->target != GL_TEXTURE_2D) return false;
    current(t->r);
    while (glGetError() != GL_NO_ERROR) {}
    GLuint fbo = texture_fbo(t);
    if (!fbo) return false;
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    bool ok = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    if (ok) {
        glDisable(GL_SCISSOR_TEST);
        glClearColor(0, 0, 0, 0);
        glClear(GL_COLOR_BUFFER_BIT);
        ok = glGetError() == GL_NO_ERROR;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (ok) base->serial = ++serials;
    return ok;
}

bool texture_upload(struct texture *base, const void *data, uint32_t format,
        uint32_t stride, const struct rect *rects, int count) {
    if (!base || !data || count <= 0 || stride % 4 != 0) return false;
    struct gl_texture *t = texture_of(base);
    if (t->buffer || format != t->format) return false;
    current(t->r);
    upload(t, data, stride, rects, count);
    base->serial = ++serials;
    return true;
}

struct texture *texture_from_pixels(struct render *r, uint32_t format, uint32_t stride,
        uint32_t width, uint32_t height, const void *data) {
    struct texture *texture = texture_new(r, format, width, height);
    if (!texture) return NULL;
    struct rect full = { 0, 0, (int32_t)width, (int32_t)height };
    if (!texture_upload(texture, data, format, stride, &full, 1)) {
        texture_destroy(texture);
        return NULL;
    }
    return texture;
}

struct texture *texture_from_buffer(struct render *r, struct buffer *buffer) {
    struct dmabuf_attributes dmabuf;
    if (!buffer_get_dmabuf(buffer, &dmabuf)) {
        void *data;
        uint32_t format;
        size_t stride;
        size_t offset;
        struct wl_shm_view *view = r->upload ? buffer_shm_view(buffer, &offset, &format, &stride) : NULL;
        if (view) {
            wl_shm_view_unref(view);
            struct texture *texture = texture_new(r, format, (uint32_t)buffer->width,
                (uint32_t)buffer->height);
            if (!texture) return NULL;
            struct region full;
            region_init_rect(&full, 0, 0, (uint32_t)buffer->width, (uint32_t)buffer->height);
            bool staged = stage(texture_of(texture), buffer, &full);
            region_fini(&full);
            if (staged) return texture;
            texture_destroy(texture);
        }
        if (!buffer_begin_access(buffer, BUFFER_READ, &data, &format, &stride)) return NULL;
        struct texture *texture = texture_from_pixels(r, format, (uint32_t)stride,
            (uint32_t)buffer->width, (uint32_t)buffer->height, data);
        buffer_end_access(buffer);
        return texture;
    }
    struct image *image = image_for(r, buffer);
    if (!image || (image->external && !G(r)->programs[PROGRAM_EXTERNAL].id)) return NULL;
    struct gl_texture *t = calloc(1, sizeof(*t));
    if (!t) return NULL;
    t->base = (struct texture){ buffer->width, buffer->height, ++serials };
    t->r = r;
    t->target = image->external ? GL_TEXTURE_EXTERNAL_OES : GL_TEXTURE_2D;
    t->alpha = format_alpha(dmabuf.format);
    current(r);
    bool fresh = !image->tex;
    if (fresh) glGenTextures(1, &image->tex);
    if (fresh || (!image->external && !render_owns(buffer))) {
        glBindTexture(t->target, image->tex);
        glTexParameteri(t->target, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(t->target, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        G(r)->image_texture(t->target, image->egl);
        glBindTexture(t->target, 0);
    }
    t->tex = image->tex;
    t->buffer = buffer_lock(buffer);
    return &t->base;
}

bool render_wait(struct render *r, struct timeline *timeline, uint64_t point) {
    int fd = timeline_export_sync_file(timeline, point);
    if (fd < 0) return false;
    EGLint attribs[] = { EGL_SYNC_NATIVE_FENCE_FD_ANDROID, fd, EGL_NONE };
    EGLSyncKHR sync = G(r)->create_sync(G(r)->display, EGL_SYNC_NATIVE_FENCE_ANDROID, attribs);
    if (sync == EGL_NO_SYNC_KHR) {
        close(fd);
        return false;
    }
    bool ok = G(r)->wait_sync(G(r)->display, sync, 0) == EGL_TRUE;
    G(r)->destroy_sync(G(r)->display, sync);
    return ok;
}

static void draw_rotated(struct pass *pass, struct gfx_prog *p, const float pos[8],
        const struct region *clip, const float texcoords[8]) {
    struct render *r = pass->r;
    if (!clip) {
        render_quad(r, p, pos, NULL, texcoords);
        return;
    }
    int count = 0;
    const struct rect *rects = region_rectangles((struct region *)clip, &count);
    GL(r, glEnable(GL_SCISSOR_TEST));
    for (int i = 0; i < count; i++) {
        GL(r, glScissor(rects[i].x1, rects[i].y1, rects[i].x2 - rects[i].x1,
            rects[i].y2 - rects[i].y1));
        render_quad(r, p, pos, NULL, texcoords);
    }
    GL(r, glDisable(GL_SCISSOR_TEST));
}

struct scene_vertex {
    float pos[2], local[2], tex[2];
    float color[4], tint[4], params[4], extra[4];
    float size[2];
};

struct scene_instance {
    float pos[8], local[8], tex[8];
    float color[4], tint[4], params[4], extra[4];
    float size[2], scissor[4];
    struct gl_texture *texture;
    GLuint id;
    GLenum target;
    int program, filter, blend;
};

struct scene_run {
    size_t first, count;
    struct gl_texture *texture;
    GLuint id;
    GLenum target;
    int program, filter, blend;
    float scissor[4];
};

enum { SCENE_VERTICES = 6, CLIP_RECTS = 16 };

static void scene_emit(struct pass *pass, const struct scene_quad *quad,
        const struct rect *scissor, uint64_t pixels) {
    struct render *r = pass->r;
    struct scene *s = &G(r)->scene;
    if (s->count == s->cap && s->cap && s->owner == pass) {
        pass_flush(pass);
        s->owner = pass;
    }
    if (s->count == s->cap) {
        size_t cap = s->cap ? s->cap * 2 : 64;
        struct scene_instance *grown = realloc(s->in, cap * sizeof(*grown));
        if (!grown) return;
        s->in = grown;
        s->cap = cap;
    }
    struct scene_instance *in = &s->in[s->count];
    memset(in, 0, sizeof(*in));
    memcpy(in->pos, quad->pos, sizeof(in->pos));
    memcpy(in->local, quad->local, sizeof(in->local));
    memcpy(in->tex, quad->tex, sizeof(in->tex));
    memcpy(in->color, quad->color, sizeof(in->color));
    memcpy(in->tint, quad->tint, sizeof(in->tint));
    in->params[0] = quad->alpha;
    in->params[1] = quad->opaque ? 1 : 0;
    in->params[2] = quad->radius;
    in->params[3] = quad->shader_clip ? 1 : 0;
    in->extra[0] = quad->width;
    in->extra[1] = quad->range;
    in->extra[2] = quad->power;
    in->program = quad->kind == SCENE_RECT || quad->kind == SCENE_TEXTURE ?
        PROGRAM_SCENE : PROGRAM_SDF;
    in->extra[3] = in->program == PROGRAM_SDF ? (float)(quad->kind - SCENE_BORDER) :
        (float)quad->kind;
    in->size[0] = quad->size[0];
    in->size[1] = quad->size[1];
    if (scissor) {
        in->scissor[0] = (float)scissor->x1;
        in->scissor[1] = (float)scissor->y1;
        in->scissor[2] = (float)scissor->x2;
        in->scissor[3] = (float)scissor->y2;
    }
    in->texture = quad->texture ? texture_of(quad->texture) : NULL;
    in->id = in->texture ? in->texture->tex : quad->level ? level_texture(quad->level) : 0;
    in->target = in->texture ? in->texture->target : quad->level ? GL_TEXTURE_2D : 0;
    in->filter = quad->filter == SCENE_FILTER_NEAREST ? GL_NEAREST :
        quad->filter ? GL_LINEAR : 0;
    in->blend = quad->blend;
    s->count++;
    r->fragments += pixels;
}

void pass_scene(struct pass *pass, const struct scene_quad *quad) {
    const struct region *clip = quad->clip ? quad->clip : pass->clip;
    if (!G(pass->r)->scene.count) G(pass->r)->scene.owner = pass;
    struct region both;
    if (quad->clip && pass->clip) {
        region_init(&both);
        region_intersect(&both, quad->clip, pass->clip);
        clip = &both;
    }
    if (!clip) scene_emit(pass, quad, NULL, quad_pixels(pass, quad->pos, NULL));
    else {
        int count = 0;
        const struct rect *rects = region_rectangles(
            (struct region *)clip, &count);
        for (int i = 0; i < count; i++) {
            uint64_t pixels = quad_pixels(pass, quad->pos, &rects[i]);
            if (pixels) scene_emit(pass, quad, &rects[i], pixels);
        }
    }
    if (clip == &both) region_fini(&both);
}

static void scene_expand(struct scene_vertex *out, const struct scene_instance *in) {
    static const int corners[SCENE_VERTICES] = { 0, 1, 2, 2, 1, 3 };
    for (int k = 0; k < SCENE_VERTICES; k++) {
        int corner = corners[k];
        struct scene_vertex *v = &out[k];
        v->pos[0] = in->pos[2 * corner];
        v->pos[1] = in->pos[2 * corner + 1];
        v->local[0] = in->local[2 * corner];
        v->local[1] = in->local[2 * corner + 1];
        v->tex[0] = in->tex[2 * corner];
        v->tex[1] = in->tex[2 * corner + 1];
        memcpy(v->color, in->color, sizeof(v->color));
        memcpy(v->tint, in->tint, sizeof(v->tint));
        memcpy(v->params, in->params, sizeof(v->params));
        memcpy(v->extra, in->extra, sizeof(v->extra));
        memcpy(v->size, in->size, sizeof(v->size));
    }
}

static void scene_attributes(struct render *r) {
    struct scene *s = &G(r)->scene;
    if (s->attributes) return;
    s->attributes = true;
    GL(r, glBindBuffer(GL_ARRAY_BUFFER, s->vbo));
    GLsizei stride = sizeof(struct scene_vertex);
    GL(r, glVertexAttribPointer(3, 2, GL_FLOAT, GL_FALSE, stride,
        (const void *)offsetof(struct scene_vertex, pos)));
    GL(r, glVertexAttribPointer(4, 2, GL_FLOAT, GL_FALSE, stride,
        (const void *)offsetof(struct scene_vertex, local)));
    GL(r, glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, stride,
        (const void *)offsetof(struct scene_vertex, tex)));
    GL(r, glVertexAttribPointer(5, 4, GL_FLOAT, GL_FALSE, stride,
        (const void *)offsetof(struct scene_vertex, color)));
    GL(r, glVertexAttribPointer(6, 4, GL_FLOAT, GL_FALSE, stride,
        (const void *)offsetof(struct scene_vertex, tint)));
    GL(r, glVertexAttribPointer(7, 4, GL_FLOAT, GL_FALSE, stride,
        (const void *)offsetof(struct scene_vertex, params)));
    GL(r, glVertexAttribPointer(8, 4, GL_FLOAT, GL_FALSE, stride,
        (const void *)offsetof(struct scene_vertex, extra)));
    GL(r, glVertexAttribPointer(9, 2, GL_FLOAT, GL_FALSE, stride,
        (const void *)offsetof(struct scene_vertex, size)));
    for (GLuint index = 2; index <= 9; index++)
        GL(r, glEnableVertexAttribArray(index));
}

void gfx_state_dirty(struct pass *pass) {
    G(pass->r)->scene.attributes = false;
}

static void scene_dummy_texture(struct render *r) {
    struct scene *s = &G(r)->scene;
    static const uint32_t clear;
    if (s->dummy) return;
    GL(r, glGenTextures(1, &s->dummy));
    GL(r, glBindTexture(GL_TEXTURE_2D, s->dummy));
    GL(r, glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, &clear));
    GL(r, glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST));
    GL(r, glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST));
    GL(r, glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE));
    GL(r, glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE));
}

static void scene_run_draw(struct render *r, struct scene_run *run, int *blending,
        bool *scissoring, int *program) {
    if (*program != run->program) {
        GL(r, glUseProgram(G(r)->programs[run->program].id));
        *program = run->program;
    }
    if (run->id) {
        GL(r, glBindTexture(run->target, run->id));
        if (run->filter && (!run->texture || run->texture->filter != run->filter)) {
            GL(r, glTexParameteri(run->target, GL_TEXTURE_MIN_FILTER, run->filter));
            GL(r, glTexParameteri(run->target, GL_TEXTURE_MAG_FILTER, run->filter));
            if (run->texture) run->texture->filter = run->filter;
        }
    } else if (run->program == PROGRAM_SCENE) {
        GL(r, glBindTexture(GL_TEXTURE_2D, G(r)->scene.dummy));
    }
    if (*blending != run->blend) {
        if (!run->blend) {
            GL(r, glDisable(GL_BLEND));
        } else {
            if (!*blending) GL(r, glEnable(GL_BLEND));
            if (run->blend == 2) GL(r, glBlendFunc(GL_ZERO, GL_ONE_MINUS_SRC_ALPHA));
            else GL(r, glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA));
        }
        *blending = run->blend;
    }
    if (run->scissor[2] > run->scissor[0]) {
        if (!*scissoring) {
            GL(r, glEnable(GL_SCISSOR_TEST));
            *scissoring = true;
        }
        GL(r, glScissor((GLint)run->scissor[0], (GLint)run->scissor[1],
            (GLsizei)(run->scissor[2] - run->scissor[0]),
            (GLsizei)(run->scissor[3] - run->scissor[1])));
    } else if (*scissoring) {
        GL(r, glDisable(GL_SCISSOR_TEST));
        *scissoring = false;
    }
    GL(r, glDrawArrays(GL_TRIANGLES, (GLint)(run->first * SCENE_VERTICES),
        (GLsizei)(run->count * SCENE_VERTICES)));
    r->runs++;
    r->quads += run->count;
}

static void scene_draw(struct render *r, size_t count) {
    if (!count) return;
    if (!G(r)->programs[PROGRAM_SCENE].id || !G(r)->programs[PROGRAM_SDF].id) {
        tomoe_log(LOG_ERROR, "tomoe: a draw program is missing");
        return;
    }
    scene_dummy_texture(r);
    GL(r, glEnable(GL_BLEND));
    GL(r, glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA));
    int blending = 1;
    bool scissoring = false;
    int program = PROGRAM_COUNT;
    struct scene_run run = {0};
    for (size_t i = 0; i < count; i++) {
        const struct scene_instance *in = &G(r)->scene.prev[i];
        bool joins = run.count && run.blend == in->blend && run.program == in->program &&
            (!in->id || !run.id || (run.id == in->id && run.target == in->target &&
                run.filter == in->filter)) && !memcmp(run.scissor, in->scissor,
                    sizeof(run.scissor));
        if (joins) {
            if (!run.id) {
                run.texture = in->texture;
                run.id = in->id;
                run.target = in->target;
                run.filter = in->filter;
            }
            run.count++;
            continue;
        }
        if (run.count) scene_run_draw(r, &run, &blending, &scissoring, &program);
        run = (struct scene_run){ .first = i, .count = 1, .texture = in->texture,
            .id = in->id, .target = in->target, .filter = in->filter, .blend = in->blend,
            .program = in->program };
        memcpy(run.scissor, in->scissor, sizeof(run.scissor));
    }
    if (run.count) scene_run_draw(r, &run, &blending, &scissoring, &program);
    if (blending == 2) GL(r, glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA));
    if (!blending) GL(r, glEnable(GL_BLEND));
    if (scissoring) GL(r, glDisable(GL_SCISSOR_TEST));
}

void pass_flush(struct pass *pass) {
    struct render *r = pass->r;
    struct scene *s = &G(r)->scene;
    if (!s->count || s->owner != pass) return;
    s->owner = NULL;
    size_t vertices = s->count * SCENE_VERTICES;
    if (vertices > s->vertex_cap) {
        size_t want = s->cap * SCENE_VERTICES;
        struct scene_vertex *staging = realloc(s->vertex, want * sizeof(*staging));
        if (!staging) {
            tomoe_log(LOG_ERROR, "tomoe: cannot stage %zu scene instances", s->count);
            s->count = 0;
            return;
        }
        s->vertex = staging;
        s->vertex_cap = want;
    }
    struct scene_vertex *staging = s->vertex;
    size_t bytes = vertices * sizeof(*staging);
    bool full = bytes > s->vbo_bytes;
    if (full) {
        size_t want = s->vbo_bytes ? s->vbo_bytes : 4096;
        while (want < bytes) want *= 2;
        if (!s->vbo) GL(r, glGenBuffers(1, &s->vbo));
        GL(r, glBindBuffer(GL_ARRAY_BUFFER, s->vbo));
        GL(r, glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)want, NULL, GL_STREAM_DRAW));
        s->vbo_bytes = want;
    }
    size_t first = 0, last = s->count;
    if (!full) {
        while (first < s->count && first < s->prev_count &&
                !memcmp(&s->in[first], &s->prev[first], sizeof(*s->in))) first++;
        while (last > first && last <= s->prev_count &&
                !memcmp(&s->in[last - 1], &s->prev[last - 1], sizeof(*s->in))) last--;
    }
    if (last > first) {
        for (size_t i = first; i < last; i++)
            scene_expand(&s->vertex[i * SCENE_VERTICES], &s->in[i]);
        size_t offset = first * SCENE_VERTICES * sizeof(*staging);
        GL(r, glBindBuffer(GL_ARRAY_BUFFER, s->vbo));
        GL(r, glBufferSubData(GL_ARRAY_BUFFER, (GLintptr)offset,
            (GLsizeiptr)((last - first) * SCENE_VERTICES * sizeof(*staging)),
            &s->vertex[first * SCENE_VERTICES]));
        r->uploads++;
        r->upload_bytes += (last - first) * SCENE_VERTICES * sizeof(*staging);
    }
    struct scene_instance *previous = s->prev;
    size_t previous_cap = s->prev_cap, drawn = s->count;
    s->prev = s->in;
    s->prev_cap = s->cap;
    s->prev_count = drawn;
    s->in = previous;
    s->cap = previous_cap;
    s->count = 0;
    scene_attributes(r);
    scene_draw(r, drawn);
    GL(r, glBindBuffer(GL_ARRAY_BUFFER, 0));
}


void gfx_texture_draw(struct pass *pass, const struct texture_options *options,
        const float pos[8], const float texcoords[8], float alpha) {
    struct render *r = pass->r;
    struct gl_texture *t = texture_of(options->texture);
    pass_flush(pass);
    bool external = t->target == GL_TEXTURE_EXTERNAL_OES;
    int kind = external ? (options->lut ? PROGRAM_EXTERNAL_LUT : PROGRAM_EXTERNAL) :
        PROGRAM_TEXTURE_LUT;
    struct gfx_prog *p = &G(r)->programs[kind];
    if (!p->id) return;
    GL(r, glUseProgram(p->id));
    GL(r, glActiveTexture(GL_TEXTURE0));
    GL(r, glBindTexture(t->target, t->tex));
    GLint filter = options->filter_mode == FILTER_NEAREST ? GL_NEAREST : GL_LINEAR;
    if (t->filter != filter) {
        GL(r, glTexParameteri(t->target, GL_TEXTURE_MIN_FILTER, filter));
        GL(r, glTexParameteri(t->target, GL_TEXTURE_MAG_FILTER, filter));
        t->filter = filter;
    }
    GL(r, glUniform1i(p->tex, 0));
    GL(r, glUniform1f(p->alpha, alpha));
    GL(r, glUniform1f(p->opaque, t->alpha ? 0 : 1));
    GL(r, glUniform1f(p->clip, 0));
    const float *tint = options->tint;
    GL(r, glUniform4f(p->tint, tint ? tint[0] : 0, tint ? tint[1] : 0, tint ? tint[2] : 0,
        tint ? tint[3] : 0));
    if (options->lut) {
        GL(r, glActiveTexture(GL_TEXTURE1));
        GL(r, glBindTexture(GL_TEXTURE_2D, texture_of(options->lut)->tex));
        GL(r, glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST));
        GL(r, glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST));
        GL(r, glUniform1i(p->lut, 1));
    }
    draw_rotated(pass, p, pos, options->clip ? options->clip : pass->clip, texcoords);
    if (options->lut) {
        GL(r, glBindTexture(GL_TEXTURE_2D, 0));
        GL(r, glActiveTexture(GL_TEXTURE0));
    }
    GL(r, glBindTexture(t->target, 0));
}

static int flush_fence(struct render *r) {
    EGLint attribs[] = { EGL_NONE };
    EGLSyncKHR sync = G(r)->create_sync(G(r)->display, EGL_SYNC_NATIVE_FENCE_ANDROID, attribs);
    glFlush();
    int fd = sync != EGL_NO_SYNC_KHR ? G(r)->dup_fence(G(r)->display, sync) : -1;
    if (sync != EGL_NO_SYNC_KHR) G(r)->destroy_sync(G(r)->display, sync);
    return fd;
}

static bool egl_init(struct render *r) {
    const char *client = eglQueryString(EGL_NO_DISPLAY, EGL_EXTENSIONS);
    PFNEGLGETPLATFORMDISPLAYEXTPROC get_display =
        (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
    if (!client || !has(client, "EGL_KHR_platform_gbm") || !get_display) {
        tomoe_log(LOG_ERROR, "tomoe: EGL lacks the GBM platform");
        return false;
    }
    G(r)->display = get_display(EGL_PLATFORM_GBM_KHR, r->gbm, NULL);
    if (G(r)->display == EGL_NO_DISPLAY || !eglInitialize(G(r)->display, NULL, NULL)) {
        tomoe_log(LOG_ERROR, "tomoe: EGL initialization failed on the %s GBM device",
            gbm_device_get_backend_name(r->gbm));
        G(r)->display = EGL_NO_DISPLAY;
        return false;
    }
    const char *egl = eglQueryString(G(r)->display, EGL_EXTENSIONS);
    const char *required[] = { "EGL_KHR_image_base", "EGL_EXT_image_dma_buf_import",
        "EGL_EXT_image_dma_buf_import_modifiers", "EGL_KHR_no_config_context",
        "EGL_KHR_surfaceless_context" };
    for (size_t i = 0; i < sizeof(required) / sizeof(required[0]); i++) {
        if (has(egl, required[i])) continue;
        tomoe_log(LOG_ERROR, "tomoe: EGL lacks %s", required[i]);
        return false;
    }
    EGLint context_attribs[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    if (!eglBindAPI(EGL_OPENGL_ES_API) || (G(r)->context = eglCreateContext(G(r)->display,
            EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, context_attribs)) == EGL_NO_CONTEXT ||
            !eglMakeCurrent(G(r)->display, EGL_NO_SURFACE, EGL_NO_SURFACE, G(r)->context)) {
        G(r)->context = EGL_NO_CONTEXT;
        tomoe_log(LOG_ERROR, "tomoe: cannot create a GLES2 context");
        return false;
    }
    const char *gl = (const char *)glGetString(GL_EXTENSIONS);
    const char *gl_required[] = { "GL_OES_EGL_image", "GL_EXT_texture_format_BGRA8888",
        "GL_EXT_unpack_subimage" };
    for (size_t i = 0; i < sizeof(gl_required) / sizeof(gl_required[0]); i++) {
        if (has(gl, gl_required[i])) continue;
        tomoe_log(LOG_ERROR, "tomoe: GLES lacks %s", gl_required[i]);
        return false;
    }
    r->read_bgra = has(gl, "GL_EXT_read_format_bgra");
    G(r)->create_image = (PFNEGLCREATEIMAGEKHRPROC)eglGetProcAddress("eglCreateImageKHR");
    G(r)->destroy_image = (PFNEGLDESTROYIMAGEKHRPROC)eglGetProcAddress("eglDestroyImageKHR");
    G(r)->image_texture = (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)
        eglGetProcAddress("glEGLImageTargetTexture2DOES");
    G(r)->image_renderbuffer = (PFNGLEGLIMAGETARGETRENDERBUFFERSTORAGEOESPROC)
        eglGetProcAddress("glEGLImageTargetRenderbufferStorageOES");
    if (has(egl, "EGL_ANDROID_native_fence_sync") && has(egl, "EGL_KHR_wait_sync")) {
        G(r)->create_sync = (PFNEGLCREATESYNCKHRPROC)eglGetProcAddress("eglCreateSyncKHR");
        G(r)->destroy_sync = (PFNEGLDESTROYSYNCKHRPROC)eglGetProcAddress("eglDestroySyncKHR");
        G(r)->wait_sync = (PFNEGLWAITSYNCKHRPROC)eglGetProcAddress("eglWaitSyncKHR");
        G(r)->dup_fence = (PFNEGLDUPNATIVEFENCEFDANDROIDPROC)
            eglGetProcAddress("eglDupNativeFenceFDANDROID");
    }
    return G(r)->create_image && G(r)->destroy_image && G(r)->image_texture && G(r)->image_renderbuffer;
}

static bool formats_init(struct render *r) {
    PFNEGLQUERYDMABUFFORMATSEXTPROC query_formats =
        (PFNEGLQUERYDMABUFFORMATSEXTPROC)eglGetProcAddress("eglQueryDmaBufFormatsEXT");
    PFNEGLQUERYDMABUFMODIFIERSEXTPROC query_modifiers =
        (PFNEGLQUERYDMABUFMODIFIERSEXTPROC)eglGetProcAddress("eglQueryDmaBufModifiersEXT");
    EGLint count = 0;
    if (!query_formats || !query_modifiers || !query_formats(G(r)->display, 0, NULL, &count) ||
            count <= 0) return false;
    EGLint *formats = calloc(count, sizeof(*formats));
    if (!formats || !query_formats(G(r)->display, count, formats, &count)) {
        free(formats);
        return false;
    }
    for (EGLint i = 0; i < count; i++) {
        EGLint n = 0;
        query_modifiers(G(r)->display, formats[i], 0, NULL, NULL, &n);
        EGLuint64KHR *modifiers = calloc(n > 0 ? n : 1, sizeof(*modifiers));
        EGLBoolean *external = calloc(n > 0 ? n : 1, sizeof(*external));
        if (modifiers && external && n > 0)
            query_modifiers(G(r)->display, formats[i], n, modifiers, external, &n);
        format_set_add(&r->texture_formats, formats[i], DRM_FORMAT_MOD_INVALID);
        format_set_add(&r->render_formats, formats[i], DRM_FORMAT_MOD_INVALID);
        for (EGLint j = 0; modifiers && external && j < n; j++) {
            format_set_add(&r->texture_formats, formats[i], modifiers[j]);
            if (!external[j]) format_set_add(&r->render_formats, formats[i], modifiers[j]);
        }
        free(modifiers);
        free(external);
    }
    free(formats);
    const uint32_t shm[] = { DRM_FORMAT_ARGB8888, DRM_FORMAT_XRGB8888, DRM_FORMAT_ABGR8888,
        DRM_FORMAT_XBGR8888 };
    for (size_t i = 0; i < sizeof(shm) / sizeof(shm[0]); i++)
        format_set_add(&r->shm_formats, shm[i], DRM_FORMAT_MOD_LINEAR);
    return true;
}

static bool programs_init(struct render *r) {
    bool external = has((const char *)glGetString(GL_EXTENSIONS), "GL_OES_EGL_image_external");
    if (!link_scene(&G(r)->programs[PROGRAM_SCENE])) return false;
    if (!link_sdf(&G(r)->programs[PROGRAM_SDF])) return false;
    if (!link_trail(&G(r)->programs[PROGRAM_TRAIL]))
        tomoe_log(LOG_ERROR, "tomoe: the pointer trail's program did not link");
    for (size_t i = 0; i < sizeof(builtins) / sizeof(builtins[0]); i++)
        if ((external || !builtins[i].external) &&
                !link_program(&G(r)->programs[builtins[i].kind], builtins[i].prelude, builtins[i].body))
            return false;
    return true;
}

static bool programs_ready(struct render *r) {
    EGLContext scratch = compile_begin(r);
    bool linked = programs_init(r);
    compile_end(r, scratch);
    if (!linked) {
        tomoe_log(LOG_ERROR, "tomoe: shader setup failed");
        return false;
    }
    return true;
}

static uint64_t fnv(uint64_t hash, const char *text) {
    for (const unsigned char *p = (const unsigned char *)text; *p; p++)
        hash = (hash ^ *p) * 0x100000001b3ull;
    return (hash ^ 0xff) * 0x100000001b3ull;
}

static void warm_setup(struct render *r) {
    char exe[PATH_MAX];
    ssize_t length = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    const char *cache = getenv("XDG_CACHE_HOME"), *home = getenv("HOME");
    char *base = NULL;
    if (length <= 0 || !r->node) return;
    exe[length] = '\0';
    if (cache && *cache == '/') base = strdup(cache);
    else if (home && *home == '/' && asprintf(&base, "%s/.cache", home) < 0) base = NULL;
    if (!base) return;
    char *tomoe = NULL, *dir = NULL;
    if (asprintf(&tomoe, "%s/tomoe", base) >= 0 && asprintf(&dir, "%s/shaders", tomoe) >= 0 &&
            (mkdir(base, 0700) == 0 || errno == EEXIST) &&
            (mkdir(tomoe, 0700) == 0 || errno == EEXIST) &&
            (mkdir(dir, 0700) == 0 || errno == EEXIST)) {
        const char *slash = strrchr(exe, '/');
        if (asprintf(&G(r)->driver, "%s\n%s\n%s\n%s", (const char *)glGetString(GL_VENDOR),
                (const char *)glGetString(GL_RENDERER), (const char *)glGetString(GL_VERSION),
                slash ? slash + 1 : exe) >= 0 && (G(r)->exe = strdup(exe))) {
            G(r)->warm_dir = dir;
            dir = NULL;
            GLint formats = 0;
            glGetIntegerv(GL_NUM_PROGRAM_BINARY_FORMATS, &formats);
            G(r)->binaries = formats > 0;
        } else {
            free(G(r)->driver);
            G(r)->driver = NULL;
        }
    }
    free(dir);
    free(tomoe);
    free(base);
}

static int shader_file(const struct dirent *entry) {
    size_t length = strlen(entry->d_name);
    return length > 5 && !strcmp(entry->d_name + length - 5, ".glsl");
}

static void warm_name(const char *source, char name[WARM_NAME]) {
    snprintf(name, WARM_NAME, "%016llx.glsl",
        (unsigned long long)fnv(0xcbf29ce484222325ull, source));
}

static char *warm_binary_path(const char *dir, const char *name) {
    char *path = NULL;
    int stem = (int)(strlen(name) - strlen(".glsl"));
    return asprintf(&path, "%s/%.*s.bin", dir, stem, name) < 0 ? NULL : path;
}

static bool warm_remember(struct render *r, const char *source, const char *name) {
    char *path = NULL, *temp = NULL;
    if (!G(r)->warm_dir || strlen(source) > WARM_SOURCE_LIMIT ||
            asprintf(&path, "%s/%s", G(r)->warm_dir, name) < 0) return false;
    if (access(path, F_OK) != 0 && asprintf(&temp, "%s.%ld", path, (long)getpid()) >= 0) {
        FILE *out = fopen(temp, "we");
        bool ok = out && fputs(source, out) >= 0;
        if (out && fclose(out) != 0) ok = false;
        if (!ok || rename(temp, path) != 0) unlink(temp);
    }
    bool there = access(path, F_OK) == 0;
    free(temp);
    free(path);
    return there;
}

static uint64_t warm_now_ns(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000000000ull + (uint64_t)now.tv_nsec;
}

static struct shader_job *job_find(struct render *r, const char *name) {
    for (struct shader_job *job = G(r)->jobs; job; job = job->next)
        if (!strcmp(job->name, name)) return job;
    return NULL;
}

static void job_forget(struct render *r, struct shader_job *job) {
    for (struct shader_job **link = &G(r)->jobs; *link; link = &(*link)->next)
        if (*link == job) {
            *link = job->next;
            free(job);
            return;
        }
}

static void job_stop(struct shader_job *job) {
    if (job->pid <= 0) return;
    kill(job->pid, SIGKILL);
    while (waitpid(job->pid, NULL, 0) < 0 && errno == EINTR) {}
    job->pid = 0;
}

static void jobs_stop(struct render *r) {
    struct shader_job *job;
    for (job = G(r)->jobs; job; job = job->next) job_stop(job);
    while ((job = G(r)->jobs)) {
        G(r)->jobs = job->next;
        free(job);
    }
}

static struct shader_job *job_start(struct render *r, const char *name) {
    struct shader_job *job = calloc(1, sizeof(*job));
    if (!job) return NULL;
    snprintf(job->name, sizeof(job->name), "%s", name);
    char flag[] = "--warm-shaders";
    char *argv[] = { G(r)->exe, flag, r->node, G(r)->warm_dir, "", job->name, NULL };
    posix_spawn_file_actions_t actions;
    pid_t child = 0;
    if (posix_spawn_file_actions_init(&actions) != 0) {
        free(job);
        return NULL;
    }
    posix_spawn_file_actions_addclosefrom_np(&actions, 3);
    int spawned = posix_spawn(&child, G(r)->exe, &actions, NULL, argv, environ);
    posix_spawn_file_actions_destroy(&actions);
    if (spawned != 0) {
        free(job);
        return NULL;
    }
    job->pid = child;
    job->deadline = warm_now_ns() + (uint64_t)TOMOE_SHADER_COPY_MS * 1000000ull;
    job->next = G(r)->jobs;
    G(r)->jobs = job;
    return job;
}

bool render_shader_reap(struct render *r) {
    bool busy = false;
    for (struct shader_job *job = G(r)->jobs, *next; job; job = next) {
        next = job->next;
        if (job->pid <= 0) continue;
        int status = 0;
        pid_t done = waitpid(job->pid, &status, WNOHANG);
        bool running = done == 0 || (done < 0 && errno == EINTR);
        if (running && warm_now_ns() < job->deadline) {
            busy = true;
            continue;
        }
        if (running) {
            job_stop(job);
            if (!job->name[0]) {
                tomoe_log(LOG_ERROR, "tomoe: the shader prewarm did not finish in %d s",
                    TOMOE_SHADER_COPY_MS / 1000);
                job_forget(r, job);
                continue;
            }
            job->failed = true;
            tomoe_log(LOG_ERROR, "tomoe: a shader copy did not finish in %d s: %s",
                TOMOE_SHADER_COPY_MS / 1000, job->name);
            continue;
        }
        job->pid = 0;
        job_forget(r, job);
    }
    return busy;
}

static enum shader_await shader_await(struct render *r, const char *name,
        struct gfx_prog *program) {
    struct shader_job *job = job_find(r, name);
    if (job && job->failed) return SHADER_FAILED;
    if (!job && !(job = job_start(r, name))) return SHADER_NO_HELPER;
    if (job->pid > 0) {
        int status = 0;
        pid_t child = job->pid, done = waitpid(child, &status, WNOHANG);
        if (done == 0) {
            if (warm_now_ns() < job->deadline) return SHADER_RUNNING;
            job_stop(job);
            job->failed = true;
            tomoe_log(LOG_ERROR, "tomoe: a shader copy did not finish in %d s: %s",
                TOMOE_SHADER_COPY_MS / 1000, name);
            return SHADER_FAILED;
        }
        if (done < 0 && errno == EINTR) return SHADER_RUNNING;
        job->pid = 0;
        if (done != child || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
            job->failed = true;
            tomoe_log(LOG_ERROR, "tomoe: a shader copy could not compile %s", name);
            return SHADER_FAILED;
        }
    }
    if (!warm_load(r, name, program)) {
        job->failed = true;
        tomoe_log(LOG_ERROR, "tomoe: the copied shader does not load: %s", name);
        return SHADER_FAILED;
    }
    job_forget(r, job);
    return SHADER_READY;
}

static bool warm_spawn(struct render *r, const char *marker) {
    char flag[] = "--warm-shaders";
    char *argv[] = { G(r)->exe, flag, r->node, G(r)->warm_dir, (char *)marker, "", NULL };
    struct shader_job *job = calloc(1, sizeof(*job));
    if (!job) return false;
    posix_spawn_file_actions_t actions;
    pid_t child = 0;
    if (posix_spawn_file_actions_init(&actions) != 0) {
        free(job);
        return false;
    }
    posix_spawn_file_actions_addclosefrom_np(&actions, 3);
    int spawned = posix_spawn(&child, G(r)->exe, &actions, NULL, argv, environ);
    posix_spawn_file_actions_destroy(&actions);
    if (spawned != 0) {
        free(job);
        return false;
    }
    job->pid = child;
    job->deadline = warm_now_ns() + (uint64_t)TOMOE_SHADER_COPY_MS * 1000000ull;
    job->next = G(r)->jobs;
    G(r)->jobs = job;
    return true;
}

static void warm(struct render *r) {
    if (!G(r)->warm_dir) return;
    uint64_t hash = fnv(0xcbf29ce484222325ull, G(r)->driver);
    for (size_t i = 0; i < sizeof(builtins) / sizeof(builtins[0]); i++)
        hash = fnv(fnv(fnv(fnv(fnv(hash, builtins[i].prelude), fragment_precision),
            rounding_source), builtins[i].body), vertex_source);
    hash = fnv(fnv(fnv(fnv(hash, scene_vertex_source), scene_prelude), rounding_source),
        scene_source);
    hash = fnv(fnv(fnv(fnv(hash, sdf_vertex_source), sdf_prelude), rounding_source), sdf_source);
    hash = fnv(fnv(fnv(hash, shader_vertex_source), shader_prelude), shader_main);
    struct dirent **entries = NULL;
    int count = scandir(G(r)->warm_dir, &entries, shader_file, alphasort);
    for (int i = 0; i < count; i++) {
        hash = fnv(hash, entries[i]->d_name);
        free(entries[i]);
    }
    free(entries);
    char *marker = NULL;
    if (asprintf(&marker, "%s/%016llx.warm", G(r)->warm_dir, (unsigned long long)hash) < 0) return;
    if (access(marker, F_OK) != 0) warm_spawn(r, marker);
    free(marker);
}

static void warm_save(const char *dir, const char *name, const struct gfx_prog *p) {
    GLint length = 0;
    glGetProgramiv(p->id, GL_PROGRAM_BINARY_LENGTH, &length);
    char *path = length > 0 ? warm_binary_path(dir, name) : NULL, *temp = NULL;
    uint8_t *data = path ? malloc(sizeof(GLenum) + (size_t)length) : NULL;
    GLenum format = 0;
    if (data && asprintf(&temp, "%s.%ld", path, (long)getpid()) >= 0) {
        glGetProgramBinary(p->id, length, &length, &format, data + sizeof(format));
        memcpy(data, &format, sizeof(format));
        size_t size = sizeof(format) + (size_t)length;
        FILE *out = fopen(temp, "we");
        bool ok = out && fwrite(data, 1, size, out) == size;
        if (out && fclose(out) != 0) ok = false;
        if (!ok || rename(temp, path) != 0) unlink(temp);
    }
    free(temp);
    free(data);
    free(path);
}

static bool warm_load(struct render *r, const char *name, struct gfx_prog *p) {
    char *path = G(r)->warm_dir && G(r)->binaries ? warm_binary_path(G(r)->warm_dir, name) : NULL;
    FILE *in = path ? fopen(path, "re") : NULL;
    free(path);
    if (!in) return false;
    struct stat st;
    uint8_t *data = fstat(fileno(in), &st) == 0 && st.st_size > (off_t)sizeof(GLenum) ?
        malloc((size_t)st.st_size) : NULL;
    bool loaded = data && fread(data, 1, (size_t)st.st_size, in) == (size_t)st.st_size;
    fclose(in);
    if (loaded) {
        GLenum format;
        memcpy(&format, data, sizeof(format));
        p->id = glCreateProgram();
        glProgramBinary(p->id, format, data + sizeof(format),
            (GLsizei)(st.st_size - (off_t)sizeof(format)));
        GLint ok = 0;
        glGetProgramiv(p->id, GL_LINK_STATUS, &ok);
        loaded = ok;
        if (loaded) program_locations(p);
        else {
            glDeleteProgram(p->id);
            p->id = 0;
            while (glGetError() != GL_NO_ERROR) {}
        }
    }
    free(data);
    return loaded;
}

static void warm_draws(struct render *r, struct gfx_prog **shaders, int count) {
    const struct format *format = format_set_get(&r->render_formats, DRM_FORMAT_XRGB8888);
    struct buffer *target = format ? render_allocate(r, WARM_SIZE, WARM_SIZE, format) : NULL;
    if (!target) return;
    static const uint32_t pixels[WARM_SIZE * WARM_SIZE];
    struct texture *textures[] = {
        texture_from_pixels(r, DRM_FORMAT_ARGB8888, WARM_SIZE * 4, WARM_SIZE, WARM_SIZE, pixels),
        texture_from_pixels(r, DRM_FORMAT_XRGB8888, WARM_SIZE * 4, WARM_SIZE, WARM_SIZE, pixels),
    };
    struct pass *pass = render_begin(r, target, NULL, 0);
    if (pass) {
        pass_add_rect(pass, &(struct rect_options){ .color = { 0, 0, 0, 1 } });
        pass_add_rect(pass, &(struct rect_options){ .color = { 0, 0, 0, 0.5f } });
        const float half = 0.5f;
        for (int i = 0; i < 2; i++)
            for (int lut = 0; lut < 2; lut++)
                for (int blended = 0; blended < 2; blended++)
                    if (textures[i]) pass_add_texture(pass, &(struct texture_options){
                        .texture = textures[i], .lut = lut ? textures[0] : NULL,
                        .alpha = blended ? &half : NULL });
        static const float full[] = { -1, -1, 1, -1, -1, 1, 1, 1 };
        pass_flush(pass);
        glEnable(GL_BLEND);
        for (int i = 0; i < count; i++) {
            glUseProgram(shaders[i]->id);
            glUniform3f(shaders[i]->resolution, WARM_SIZE, WARM_SIZE, 1);
            render_quad(r, shaders[i], full, full, NULL);
        }
        pass_submit(pass);
    }
    glFinish();
    for (int i = 0; i < 2; i++) texture_destroy(textures[i]);
    buffer_drop(target);
}

static char *read_source(const char *dir, const char *name) {
    char *path = NULL, *source = NULL;
    size_t size = 0;
    if (asprintf(&path, "%s/%s", dir, name) < 0) return NULL;
    FILE *in = fopen(path, "re"), *out = in ? open_memstream(&source, &size) : NULL;
    char chunk[16384];
    size_t n;
    while (out && size <= WARM_SOURCE_LIMIT && (n = fread(chunk, 1, sizeof(chunk), in)) > 0)
        fwrite(chunk, 1, n, out);
    if (out && fclose(out) != 0) {
        free(source);
        source = NULL;
    }
    if (in) fclose(in);
    free(path);
    return source;
}

int tomoe_warm_shaders(const char *node, const char *dir, const char *marker,
        const char *name) {
    struct render *r = render_open(strdup(node));
    if (!r || !programs_init(r)) {
        if (r) render_destroy(r);
        return 1;
    }
    bool every = !name || !*name;
    struct dirent **entries = NULL;
    int count = every ? scandir(dir, &entries, shader_file, alphasort) : 1, linked = 0;
    struct gfx_prog **shaders = calloc(count > 0 ? (size_t)count : 1, sizeof(*shaders));
    for (int i = 0; i < count; i++) {
        const char *file = every ? entries[i]->d_name : name;
        char *source = shaders ? read_source(dir, file) : NULL;
        struct gfx_prog *shader = source ? render_shader_create(r, source, NULL) : NULL;
        if (shader) {
            warm_save(dir, file, shader);
            shaders[linked++] = shader;
        }
        free(source);
        if (every) free(entries[i]);
    }
    free(entries);
    warm_draws(r, shaders, linked);
    for (int i = 0; i < linked; i++) render_shader_destroy(shaders[i]);
    free(shaders);
    render_destroy(r);
    if (marker && *marker) {
        int fd = open(marker, O_WRONLY | O_CREAT | O_CLOEXEC, 0600);
        if (fd >= 0) close(fd);
    }
    return 0;
}

static void scene_reserve(struct scene *s) {
    struct scene_instance *in = s->cap < SCENE_RESERVE ?
        realloc(s->in, SCENE_RESERVE * sizeof(*in)) : NULL;
    if (in) {
        s->in = in;
        s->cap = SCENE_RESERVE;
    }
    struct scene_instance *prev = s->prev_cap < SCENE_RESERVE ?
        realloc(s->prev, SCENE_RESERVE * sizeof(*prev)) : NULL;
    if (prev) {
        s->prev = prev;
        s->prev_cap = SCENE_RESERVE;
    }
    struct scene_vertex *vertex = s->vertex_cap < SCENE_RESERVE * SCENE_VERTICES ?
        realloc(s->vertex, SCENE_RESERVE * SCENE_VERTICES * sizeof(*vertex)) : NULL;
    if (vertex) {
        s->vertex = vertex;
        s->vertex_cap = SCENE_RESERVE * SCENE_VERTICES;
    }
}

struct render *gfx_alloc(void) {
    struct gfx *g = calloc(1, sizeof(*g));
    if (!g) return NULL;
    wl_list_init(&g->images);
    return &g->base;
}

bool gfx_open(struct render *r) {
    if (!egl_init(r)) return false;
    if (!formats_init(r)) {
        tomoe_log(LOG_ERROR, "tomoe: EGL reports no dmabuf formats");
        return false;
    }
    return true;
}

bool gfx_ready(struct render *r) {
    warm_setup(r);
    warm(r);
    if (!programs_ready(r)) return false;
    scene_reserve(&G(r)->scene);
    staging_init(r);
    return true;
}

bool gfx_fences(struct render *r) {
    return G(r)->dup_fence && G(r)->wait_sync;
}

void gfx_stop(struct render *r) {
    jobs_stop(r);
    if (r->upload) {
        upload_report(r->upload);
        upload_destroy(r->upload);
        r->upload = NULL;
    }
}

void gfx_drop_images(struct render *r) {
    struct image *image, *next_image;
    wl_list_for_each_safe(image, next_image, &G(r)->images, link) image_destroy(image);
}

void gfx_close(struct render *r) {
    struct gfx *g = G(r);
    if (g->context) {
        current(r);
        while (g->shaders) {
            struct shader *s = g->shaders;
            g->shaders = s->next;
            glDeleteProgram(s->program.id);
            free(s->source);
            free(s);
        }
        g->shader_count = 0;
        for (int i = 0; i < PROGRAM_COUNT; i++) glDeleteProgram(g->programs[i].id);
        staging_free(r);
        if (g->scene.vbo) glDeleteBuffers(1, &g->scene.vbo);
        if (g->scene.dummy) glDeleteTextures(1, &g->scene.dummy);
        if (g->trail.vbo) glDeleteBuffers(1, &g->trail.vbo);
        if (g->trail.vao) glDeleteVertexArrays(1, &g->trail.vao);
        eglMakeCurrent(g->display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        eglDestroyContext(g->display, g->context);
    }
    if (g->display) eglTerminate(g->display);
    free(g->scene.in);
    free(g->scene.prev);
    free(g->scene.vertex);
    free(g->exe);
    free(g->warm_dir);
    free(g->driver);
}

size_t gfx_image_count(struct render *r) {
    return (size_t)wl_list_length(&G(r)->images);
}

void gfx_current(struct render *r) {
    current(r);
}

void gfx_flush(struct render *r) {
    glFlush();
}

void gfx_finish(struct render *r) {
    glFinish();
}

int gfx_fence_export(struct render *r) {
    return G(r)->dup_fence ? flush_fence(r) : -1;
}

bool gfx_target_prepare(struct render *r, struct buffer *buffer) {
    return render_buffer_fbo(r, buffer) != 0;
}

bool gfx_target_begin(struct render *r, struct buffer *buffer) {
    GLuint fbo = render_buffer_fbo(r, buffer);
    if (!fbo) return false;
    current(r);
    latch(r);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glViewport(0, 0, buffer->width, buffer->height);
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glDisable(GL_SCISSOR_TEST);
    return true;
}

void gfx_target_end(struct render *r) {
    if (G(r)->context) glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

bool gfx_tex_alpha(const struct texture *texture) {
    return texture_of((struct texture *)texture)->alpha;
}

bool gfx_tex_external(const struct texture *texture) {
    return texture_of((struct texture *)texture)->target == GL_TEXTURE_EXTERNAL_OES;
}

bool gfx_prog_reads(const struct gfx_prog *p, enum gfx_input input) {
    switch (input) {
    case GFX_IN_BENEATH: return p->channel >= 0;
    case GFX_IN_FROST: return p->frost >= 0;
    case GFX_IN_MARKS: return p->marks >= 0;
    case GFX_IN_SELECTION: return p->selection >= 0;
    case GFX_IN_DATA: return p->data >= 0;
    case GFX_IN_WINDOWS: return p->windows >= 0 || p->window_count >= 0;
    case GFX_IN_MOUSE: return p->mouse >= 0;
    case GFX_IN_TRAIL: return p->trail >= 0 || p->trail_count >= 0;
    }
    return false;
}

bool gfx_tex_info(const struct texture *texture, bool *external, bool *alpha) {
    if (!texture) return false;
    const struct gl_texture *t = texture_of((struct texture *)texture);
    if (!t->tex) return false;
    if (external) *external = t->target == GL_TEXTURE_EXTERNAL_OES;
    if (alpha) *alpha = t->alpha;
    return true;
}

bool gfx_external_ready(struct render *r) {
    return render_program(r, PROGRAM_EXTERNAL) != NULL;
}
