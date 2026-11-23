#include "gfx_gles.h"

bool gfx_level_ensure(struct gfx_level *level, int width, int height) {
    if (level->token && level->width == width && level->height == height) return true;
    if (!level->token) {
        GLuint texture = 0, framebuffer = 0;
        glGenTextures(1, &texture);
        glGenFramebuffers(1, &framebuffer);
        level->token = (uint64_t)framebuffer << 32 | texture;
    }
    glBindTexture(GL_TEXTURE_2D, level_texture(level));
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, width, height, 0, GL_RGB, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindFramebuffer(GL_FRAMEBUFFER, level_framebuffer(level));
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, level_texture(level), 0);
    level->width = width;
    level->height = height;
    return glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
}

void gfx_level_release(struct gfx_level *level) {
    GLuint texture = level_texture(level), framebuffer = level_framebuffer(level);
    glDeleteFramebuffers(1, &framebuffer);
    glDeleteTextures(1, &texture);
    *level = (struct gfx_level){0};
}

static void sample_pass(struct gfx_prog *p, struct gfx_level *from, struct gfx_level *to, double offset,
        const struct region *where, float grain, float vibrancy) {
    static const float full[] = { -1, -1, 1, -1, -1, 1, 1, 1 };
    static const float coords[] = { 0, 0, 1, 0, 0, 1, 1, 1 };
    glBindFramebuffer(GL_FRAMEBUFFER, level_framebuffer(to));
    glViewport(0, 0, to->width, to->height);
    glUseProgram(p->id);
    glBindTexture(GL_TEXTURE_2D, level_texture(from));
    glUniform1i(p->tex, 0);
    glUniform2f(p->half_pixel, 0.5f / to->width, 0.5f / to->height);
    glUniform1f(p->offset, offset);
    if (p->grain >= 0) glUniform1f(p->grain, grain);
    if (p->vibrancy >= 0) glUniform1f(p->vibrancy, vibrancy);
    glVertexAttribPointer(p->pos, 2, GL_FLOAT, GL_FALSE, 0, full);
    glVertexAttribPointer(p->texcoord, 2, GL_FLOAT, GL_FALSE, 0, coords);
    glEnableVertexAttribArray(p->pos);
    glEnableVertexAttribArray(p->texcoord);
    int count = 0;
    const struct rect *rects = region_rectangles(where, &count);
    for (int i = 0; i < count; i++) {
        glScissor(rects[i].x1, rects[i].y1, rects[i].x2 - rects[i].x1, rects[i].y2 - rects[i].y1);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    }
    glDisableVertexAttribArray(p->pos);
    glDisableVertexAttribArray(p->texcoord);
}

void gfx_external_draw(struct pass *pass, const struct texture_options *options, bool alpha,
    struct fbox clip, double radius, const float pos[8], const float local[8],
    const float texcoords[8]) {
    struct gl_texture *t = wl_container_of(options->texture, t, base);
    GLenum target = t->target;
    GLuint tex = t->tex;
    struct gfx_prog *p = render_program(pass->r, PROGRAM_EXTERNAL);
    GLint filter = options->filter_mode == FILTER_NEAREST ? GL_NEAREST : GL_LINEAR;
    const float *tint = options->tint;
    pass_flush(pass);
    glUseProgram(p->id);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(target, tex);
    glTexParameteri(target, GL_TEXTURE_MIN_FILTER, filter);
    glTexParameteri(target, GL_TEXTURE_MAG_FILTER, filter);
    glUniform1i(p->tex, 0);
    glUniform1f(p->alpha, options->alpha ? *options->alpha : 1);
    glUniform1f(p->opaque, alpha ? 0 : 1);
    glUniform1f(p->clip, 1);
    glUniform2f(p->size, clip.width, clip.height);
    glUniform1f(p->radius, radius);
    glUniform4f(p->tint, tint ? tint[0] : 0, tint ? tint[1] : 0, tint ? tint[2] : 0,
        tint ? tint[3] : 0);
    glEnable(GL_BLEND);
    pass_quad(pass, p, pos, local, texcoords);
    glBindTexture(target, 0);
}

void gfx_level_blit(const struct gfx_level *from, const struct gfx_level *to,
        const struct box *samples, int count, int sx, int sy, int dx, int dy) {
    glBindFramebuffer(GL_READ_FRAMEBUFFER, level_framebuffer(from));
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, level_framebuffer(to));
    for (int i = 0; i < count; i++) {
        struct box s = samples[i];
        glBlitFramebuffer(s.x - sx, s.y - sy, s.x - sx + s.width, s.y - sy + s.height,
            s.x - dx, s.y - dy, s.x - dx + s.width, s.y - dy + s.height,
            GL_COLOR_BUFFER_BIT, GL_NEAREST);
    }
}

bool gfx_level_spill(const struct gfx_level *from, struct gfx_level *to, int width, int height,
        const struct box *samples, int count, int dx, int dy) {
    GLint read = 0, draw = 0;
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &read);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &draw);
    GLboolean scissor = glIsEnabled(GL_SCISSOR_TEST);
    glDisable(GL_SCISSOR_TEST);
    bool ok = gfx_level_ensure(to, width, height);
    if (ok) gfx_level_blit(from, to, samples, count, 0, 0, dx, dy);
    else gfx_level_release(to);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)read);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint)draw);
    if (scissor) glEnable(GL_SCISSOR_TEST);
    return ok;
}

void gfx_blur_copy_in(struct render *r, struct buffer *buffer, struct gfx_level *level,
        struct box grown, const struct rect *copy, int copies, int shrink) {
    GLuint target = render_buffer_fbo(r, buffer);
    glDisable(GL_SCISSOR_TEST);
    if (shrink) {
        glBindFramebuffer(GL_READ_FRAMEBUFFER, target);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, level_framebuffer(level));
        for (int i = 0; i < copies; i++)
            glBlitFramebuffer(grown.x + (copy[i].x1 << shrink), grown.y + (copy[i].y1 << shrink),
                grown.x + (copy[i].x2 << shrink), grown.y + (copy[i].y2 << shrink),
                copy[i].x1, copy[i].y1, copy[i].x2, copy[i].y2, GL_COLOR_BUFFER_BIT, GL_LINEAR);
    } else {
        glBindFramebuffer(GL_FRAMEBUFFER, target);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, level_texture(level));
        for (int i = 0; i < copies; i++)
            glCopyTexSubImage2D(GL_TEXTURE_2D, 0, copy[i].x1, copy[i].y1, grown.x + copy[i].x1,
                grown.y + copy[i].y1, copy[i].x2 - copy[i].x1, copy[i].y2 - copy[i].y1);
    }
    glActiveTexture(GL_TEXTURE0);
    glDisable(GL_BLEND);
    glEnable(GL_SCISSOR_TEST);
}

void gfx_blur_passes(struct render *r, struct gfx_level *const *levels, int passes,
        double offset, const struct region *down, const struct region *up, float noise,
        float vibrancy) {
    for (int i = 1; i <= passes; i++)
        sample_pass(render_program(r, PROGRAM_DOWN), levels[i - 1], levels[i], offset, &down[i], 0, 1);
    for (int i = passes - 1; i >= 0; i--)
        sample_pass(render_program(r, PROGRAM_UP), levels[i + 1], levels[i], offset, &up[i],
            i ? 0 : noise, i ? 1 : vibrancy);
    glDisable(GL_SCISSOR_TEST);
}

void gfx_target_viewport(struct render *r, struct buffer *buffer) {
    glBindFramebuffer(GL_FRAMEBUFFER, render_buffer_fbo(r, buffer));
    glViewport(0, 0, buffer->width, buffer->height);
}

void gfx_target_rebind(struct render *r, struct buffer *buffer) {
    gfx_target_viewport(r, buffer);
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
}

void gfx_backdrop_grab(struct render *r, struct buffer *buffer, const struct gfx_level *level,
        struct box need, const struct box *shown, const struct box *clear, int count) {
    GLuint target = render_buffer_fbo(r, buffer);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, level_framebuffer(level));
    if (count) {
        glEnable(GL_SCISSOR_TEST);
        glClearColor(0, 0, 0, 1);
        for (int i = 0; i < count; i++) {
            if (box_empty(&clear[i])) continue;
            glScissor(clear[i].x, clear[i].y, clear[i].width, clear[i].height);
            glClear(GL_COLOR_BUFFER_BIT);
        }
        glDisable(GL_SCISSOR_TEST);
    }
    if (shown) {
        int x = shown->x - need.x, y = shown->y - need.y;
        glBindFramebuffer(GL_READ_FRAMEBUFFER, target);
        glBlitFramebuffer(shown->x, shown->y, shown->x + shown->width, shown->y + shown->height,
            x, y, x + shown->width, y + shown->height, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, target);
    glViewport(0, 0, buffer->width, buffer->height);
}

void gfx_shader_draw(struct pass *pass, struct gfx_prog *p, const struct gfx_shader_params *sp,
        const float pos[8], const float local[8]) {
    if (sp->beneath) {
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, level_texture(sp->beneath));
        glUseProgram(p->id);
        glUniform4f(p->beneath, sp->beneath_box[0], sp->beneath_box[1], sp->beneath_box[2],
            sp->beneath_box[3]);
    }
    glUseProgram(p->id);
    if (sp->frost) {
        glActiveTexture(GL_TEXTURE2);
        glBindTexture(GL_TEXTURE_2D, sp->frost_ok ? level_texture(sp->frost) : 0);
        glUniform1i(p->frost, 2);
        double fw = (double)(sp->frost->width << 1), fh = (double)(sp->frost->height << 1);
        if (p->frost_box >= 0) {
            if (fw > 0 && fh > 0)
                glUniform4f(p->frost_box, -sp->box.x, sp->box.y + sp->box.height, 1.0 / fw, -1.0 / fh);
            else
                glUniform4f(p->frost_box, 0, 0, 0, 0);
        }
        glActiveTexture(GL_TEXTURE0);
    }
    glUniform3f(p->resolution, sp->box.width, sp->box.height, 1);
    glUniform1f(p->time, sp->time);
    glUniform1i(p->frame, sp->frame);
    glUniform4f(p->mouse, sp->mouse_x, sp->mouse_y, 0, 0);
    glUniform1i(p->channel, 0);
    glUniform1i(p->trail_count, sp->trail_count);
    if (sp->trail_count) glUniform4fv(p->trail, sp->trail_count, sp->trail);
    if (p->data >= 0) {
        float data[SHADER_DATA] = { 0 };
        if (sp->data_count) memcpy(data, sp->data, sp->data_count * sizeof(float));
        glUniform4fv(p->data, SHADER_DATA / 4, data);
    }
    glUniform1i(p->window_count, sp->window_count);
    if (sp->window_count) glUniform4fv(p->windows, sp->window_count, sp->windows);
    if (p->selection >= 0) {
        static const float zeros[16] = { 0 };
        glUniform4fv(p->selection, 4, sp->selection ? sp->selection : zeros);
    }
    if (p->marks_box >= 0) {
        static const float zero_box[4] = { 0 };
        glUniform4fv(p->marks_box, 1, sp->marks_box ? sp->marks_box : zero_box);
    }
    if (p->marks >= 0) {
        GLenum target;
        GLuint texture;
        bool alpha;
        glActiveTexture(GL_TEXTURE1);
        if (sp->marks && render_texture_gl(sp->marks, &target, &texture, &alpha)) {
            glBindTexture(target, texture);
            glTexParameteri(target, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(target, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        } else {
            glBindTexture(GL_TEXTURE_2D, 0);
        }
        glUniform1i(p->marks, 1);
        glActiveTexture(GL_TEXTURE0);
    }
    glEnable(GL_BLEND);
    pass_quad(pass, p, pos, local, NULL);
    glBindTexture(GL_TEXTURE_2D, 0);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, 0);
    if (sp->frost) {
        glActiveTexture(GL_TEXTURE2);
        glBindTexture(GL_TEXTURE_2D, 0);
    }
    glActiveTexture(GL_TEXTURE0);
}

static const char glass_source[] =
    "uniform sampler2D tomoe_glass;\n"
    "void mainImage(out vec4 color, in vec2 coord) {\n"
    "    vec4 lens = texture(tomoe_glass, v_local / iResolution.xy);\n"
    "    vec2 normal = lens.rg * 2.0 - 1.0;\n"
    "    vec2 bend = normal * iData[0].x;\n"
    "    vec2 at = gl_FragCoord.xy;\n"
    "    vec3 seen = vec3(beneath(at + bend * 1.12).r, beneath(at + bend).g,\n"
    "        beneath(at + bend * 0.88).b);\n"
    "    vec3 behind = beneath(at - v_local + iResolution.xy * 0.5).rgb;\n"
    "    float luma = dot(behind, vec3(0.2126, 0.7152, 0.0722));\n"
    "    float dark = 1.0 - smoothstep(0.42, 0.50, luma);\n"
    "    vec3 smoked = seen * vec3(0.54, 0.56, 0.66);\n"
    "    vec3 frost = mix(seen, vec3(0.94, 0.95, 0.99), 0.85);\n"
    "    vec3 body = mix(smoked, frost, dark);\n"
    "    body += vec3(max(dot(normal, vec2(-0.55, -0.83)), 0.0) * 0.12);\n"
    "    float ink = lens.b * 2.0 - 1.0;\n"
    "    float edge = abs(ink);\n"
    "    vec3 rim = ink < 0.0 ? vec3(0.0) : vec3(1.0);\n"
    "    float gain = mix(0.45, 1.0, ink < 0.0 ? 1.0 - dark : dark);\n"
    "    color = vec4(body * lens.a + rim * edge * gain, lens.a + edge * gain);\n"
    "}\n";

struct gfx_prog *gfx_glass_program(struct render *r) {
    return render_shader_compile(r, glass_source);
}

void gfx_glass_draw(struct pass *pass, struct gfx_prog *p, struct texture *map,
        const struct gfx_level *glass, struct box grown, struct fbox box, double bend,
        const float pos[8], const float local[8]) {
    struct gl_texture *t = wl_container_of(map, t, base);
    GLuint lens = t->tex;
    glUseProgram(p->id);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, lens);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glUniform1i(glGetUniformLocation(p->id, "tomoe_glass"), 1);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, level_texture(glass));
    glUniform1i(p->channel, 0);
    glUniform4f(p->beneath, grown.x, grown.y, 1.0f / grown.width, 1.0f / grown.height);
    glUniform3f(p->resolution, box.width, box.height, 1);
    const float data[4] = { (float)bend, 0, 0, 0 };
    glUniform4fv(p->data, 1, data);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glEnable(GL_BLEND);
    pass_quad(pass, p, pos, local, NULL);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, 0);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, 0);
    gfx_state_dirty(pass);
}
