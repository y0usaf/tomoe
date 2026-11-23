#include "internal.h"
#include "pv.h"

enum { CURSOR_WINDOW_MS = 100 };

static struct tomoe *cursor_server;

static void pointer_velocity(const struct tomoe *s, double now, double *vx, double *vy);

static pv cursor_plist(pv plist, const char *name) {
    pv tail = plist;
    while (pv_consp(tail)) {
        pv rest = pv_cdr(tail);
        if (!pv_consp(rest)) return pv_nil();
        if (pv_eq_keyword(pv_car(tail), name)) return pv_car(rest);
        tail = pv_cdr(rest);
    }
    return pv_nil();
}

static int cursor_field(pv head) {
    if (pv_eq_keyword(head, "SPEED")) return CURSOR_FIELD_SPEED;
    if (pv_eq_keyword(head, "ANGLE")) return CURSOR_FIELD_ANGLE;
    if (pv_eq_keyword(head, "ACCELERATION")) return CURSOR_FIELD_ACCELERATION;
    if (pv_eq_keyword(head, "SHAKE")) return CURSOR_FIELD_SHAKE;
    if (pv_eq_keyword(head, "VX")) return CURSOR_FIELD_VX;
    if (pv_eq_keyword(head, "VY")) return CURSOR_FIELD_VY;
    pv_error("Unsupported cursor term field.");
}

static struct cursor_term_clause *term_clause(struct cursor_term *term, int field, double scale) {
    if (term->count == CURSOR_TERM_CLAUSES) pv_error("Cursor term exceeds its clause budget.");
    struct cursor_term_clause *clause = &term->clauses[term->count++];
    clause->field = field;
    clause->scale = scale;
    clause->above = -INFINITY;
    clause->below = INFINITY;
    return clause;
}

static void parse_clause(struct cursor_term *term, pv clause) {
    pv rest = pv_cdr(clause);
    if (!pv_consp(rest)) pv_error("Cursor terms need a scale.");
    double scale = pv_get_double(pv_car(rest));
    struct cursor_term_clause *entry = term_clause(term, cursor_field(pv_car(clause)), scale);
    pv options = pv_cdr(rest);
    pv above = cursor_plist(options, "ABOVE"), below = cursor_plist(options, "BELOW");
    if (!pv_nilp(above)) entry->above = pv_get_double(above);
    if (!pv_nilp(below)) entry->below = pv_get_double(below);
}

static void parse_term(pv value, struct cursor_term *term) {
    term->count = 0;
    if (pv_nilp(value)) return;
    if (!pv_consp(value)) {
        term_clause(term, CURSOR_FIELD_CONSTANT, pv_get_double(value));
        return;
    }
    if (pv_keywordp(pv_car(value))) {
        parse_clause(term, value);
        return;
    }
    for (pv tail = value; pv_consp(tail); tail = pv_cdr(tail)) {
        pv clause = pv_car(tail);
        if (pv_consp(clause)) parse_clause(term, clause);
        else term_clause(term, CURSOR_FIELD_CONSTANT, pv_get_double(clause));
    }
}

static void parse_effect(pv spec, struct cursor_effect *effect) {
    memset(effect, 0, sizeof(*effect));
    effect->settle = 0.08;
    parse_term(cursor_plist(spec, "ROTATION"), &effect->rotation);
    parse_term(cursor_plist(spec, "SCALE"), &effect->scale);
    parse_term(cursor_plist(spec, "STRETCH"), &effect->stretch);
    parse_term(cursor_plist(spec, "STRETCH-ANGLE"), &effect->stretch_angle);
    parse_term(cursor_plist(spec, "OFFSET-X"), &effect->offset_x);
    parse_term(cursor_plist(spec, "OFFSET-Y"), &effect->offset_y);
    pv settle = cursor_plist(spec, "SETTLE");
    if (!pv_nilp(settle)) effect->settle = pv_get_double(settle);
}

struct cursor_fields {
    double speed, angle, acceleration, shake, vx, vy;
};

static double term_value(const struct cursor_term *term, const struct cursor_fields *fields,
        double fallback) {
    if (!term->count) return fallback;
    double total = 0;
    bool contributed = false;
    for (int i = 0; i < term->count; i++) {
        const struct cursor_term_clause *clause = &term->clauses[i];
        double magnitude = 0;
        switch (clause->field) {
        case CURSOR_FIELD_SPEED: magnitude = fields->speed; break;
        case CURSOR_FIELD_ACCELERATION: magnitude = fields->acceleration; break;
        case CURSOR_FIELD_SHAKE: magnitude = fields->shake; break;
        case CURSOR_FIELD_ANGLE:
        case CURSOR_FIELD_VX:
        case CURSOR_FIELD_VY: {
            double value = clause->field == CURSOR_FIELD_ANGLE ? fields->angle :
                (clause->field == CURSOR_FIELD_VX ? fields->vx : fields->vy);
            if (clause->below != INFINITY) {
                if (value > clause->below) value = clause->below;
                else if (value < -clause->below) value = -clause->below;
            }
            total += clause->scale * value;
            contributed = true;
            continue;
        }
        default:
            total += clause->scale;
            contributed = true;
            continue;
        }
        if (!(magnitude > clause->above)) continue;
        total += clause->scale * (magnitude < clause->below ? magnitude : clause->below);
        contributed = true;
    }
    return contributed ? total : fallback;
}

static void effect_matrix(const struct cursor_effect *effect, const struct cursor_fields *fields,
        double matrix[4], double offset[2]) {
    double scale = term_value(&effect->scale, fields, 1);
    double stretch = term_value(&effect->stretch, fields, 0);
    double across = 1 - 0.5 * stretch;
    double along = 1 + stretch;
    double phi = M_PI * (term_value(&effect->stretch_angle, fields, 0) / 180);
    double cp = cos(phi), sp = sin(phi);
    double s00 = cp * cp * across + sp * sp * along;
    double s01 = cp * sp * (across - along);
    double s11 = sp * sp * across + cp * cp * along;
    double theta = M_PI * (term_value(&effect->rotation, fields, 0) / 180);
    double ct = cos(theta), st = sin(theta);
    matrix[0] = scale * (ct * s00 - st * s01);
    matrix[1] = scale * (ct * s01 - st * s11);
    matrix[2] = scale * (st * s00 + ct * s01);
    matrix[3] = scale * (st * s01 + ct * s11);
    offset[0] = term_value(&effect->offset_x, fields, 0);
    offset[1] = term_value(&effect->offset_y, fields, 0);
}

void cursor_effects_evaluate(struct tomoe *s, double now) {
    struct cursor_fields fields = { 0, 0, 0, 0, 0, 0 };
    pointer_velocity(s, now, &fields.vx, &fields.vy);
    fields.speed = hypot(fields.vx, fields.vy);
    fields.angle = atan2(fields.vy, fields.vx) * 180 / M_PI;
    fields.acceleration = hypot(s->cursor_motion.acceleration[0],
        s->cursor_motion.acceleration[1]);
    fields.shake = s->cursor_motion.shake;
    double matrix[4] = { 1, 0, 0, 1 };
    double offset[2] = { 0, 0 };
    double settle = 0;
    for (int i = 0; i < s->cursor_effects.count; i++) {
        const struct cursor_effect *effect = &s->cursor_effects.entries[i];
        double local[4], local_offset[2];
        effect_matrix(effect, &fields, local, local_offset);
        double composed[4];
        composed[0] = matrix[0] * local[0] + matrix[1] * local[2];
        composed[1] = matrix[0] * local[1] + matrix[1] * local[3];
        composed[2] = matrix[2] * local[0] + matrix[3] * local[2];
        composed[3] = matrix[2] * local[1] + matrix[3] * local[3];
        for (int j = 0; j < 4; j++) matrix[j] = composed[j];
        offset[0] += local_offset[0];
        offset[1] += local_offset[1];
        if (effect->settle > settle) settle = effect->settle;
    }
    if (!(settle > 0)) settle = 0.08;
    cursor_motion_apply(s, matrix, offset, settle);
}

static pv cursor_effects(int argc, pv *argv) {
    cursor_server->cursor_effects.count = 0;
    for (pv tail = argv[0]; pv_consp(tail); tail = pv_cdr(tail)) {
        pv spec = cursor_plist(pv_car(tail), "SPEC");
        if (cursor_server->cursor_effects.count == CURSOR_EFFECTS_MAX)
            pv_error("Cursor effect set exceeds its budget.");
        struct cursor_effect *effect =
            &cursor_server->cursor_effects.entries[cursor_server->cursor_effects.count++];
        parse_effect(spec, effect);
    }
    cursor_effects_evaluate(cursor_server, animation_now());
    return pv_nil();
}

void cursor_effects_register_primitives(struct tomoe *server) {
    cursor_server = server;
    prim_define("%cursor-effects", cursor_effects, 1, 1);
}

static bool matrix_identity(const double *matrix) {
    return matrix[0] == 1 && matrix[1] == 0 && matrix[2] == 0 && matrix[3] == 1;
}

static bool placement_identity(const double *matrix, const double *offset) {
    return matrix_identity(matrix) && offset[0] == 0 && offset[1] == 0;
}

static void pointer_velocity(const struct tomoe *s, double now, double *vx, double *vy) {
    *vx = *vy = 0;
    int count = s->pointer_trail_count;
    if (count < 2) return;
    double window = CURSOR_WINDOW_MS / 1000.0;
    int newest = (s->pointer_trail_head + POINTER_TRAIL - 1) % POINTER_TRAIL;
    const struct pointer_sample *end = &s->pointer_trail[newest];
    if (now - end->t > window) return;
    const struct pointer_sample *start = end;
    for (int i = 1; i < count; i++) {
        const struct pointer_sample *sample =
            &s->pointer_trail[(newest - i + 2 * POINTER_TRAIL) % POINTER_TRAIL];
        if (end->t - sample->t > window) break;
        start = sample;
    }
    double elapsed = end->t - start->t;
    if (elapsed < 0.0005) return;
    *vx = (end->x - start->x) / elapsed;
    *vy = (end->y - start->y) / elapsed;
}

static bool shake_history_add(struct cursor_motion *m, const struct tomoe *s, double now) {
    if (m->history_count && now - m->history_at < 0.008) return false;
    m->history[m->history_head] =
        (struct cursor_sample){ (float)s->pointer_x, (float)s->pointer_y, now };
    m->history_head = (m->history_head + 1) % CURSOR_HISTORY;
    if (m->history_count < CURSOR_HISTORY) m->history_count++;
    m->history_at = now;
    return true;
}

static void shake_measure(struct cursor_motion *m, double now) {
    int count = m->history_count;
    m->shake = 0;
    m->shake_distance = 0;
    if (count < 3) return;
    double travel = 0, left = INFINITY, top = INFINITY, right = -INFINITY,
        bottom = -INFINITY, previous_x = 0, previous_y = 0;
    bool first = true;
    for (int i = 0; i < count; i++) {
        const struct cursor_sample *sample =
            &m->history[(m->history_head - count + i + CURSOR_HISTORY) % CURSOR_HISTORY];
        if (now - sample->t > 1.0) continue;
        if (!first) travel += hypot(sample->x - previous_x, sample->y - previous_y);
        previous_x = sample->x;
        previous_y = sample->y;
        first = false;
        if (sample->x < left) left = sample->x;
        if (sample->x > right) right = sample->x;
        if (sample->y < top) top = sample->y;
        if (sample->y > bottom) bottom = sample->y;
    }
    double diagonal = hypot(right - left, bottom - top);
    m->shake_distance = diagonal;
    if (diagonal > 0 && now - m->history_at <= 0.25) m->shake = travel / diagonal;
}

bool cursor_motion_rest(const struct tomoe *s) {
    const struct cursor_motion *m = &s->cursor_motion;
    if (!placement_identity(m->current, m->current_offset)) return false;
    if (!m->live) return true;
    return placement_identity(m->target, m->target_offset);
}

void cursor_motion_matrix(const struct tomoe *s, double matrix[4], double offset[2]) {
    const struct cursor_motion *m = &s->cursor_motion;
    for (int i = 0; i < 4; i++) matrix[i] = m->current[i];
    offset[0] = m->current_offset[0];
    offset[1] = m->current_offset[1];
}

static void decompose(const double *m, double *angle, double *scale) {
    double rotate = atan2(m[2] - m[1], m[0] + m[3]);
    double c = cos(rotate), s = sin(rotate);
    scale[0] = c * m[0] + s * m[2];
    scale[1] = 0.5 * (c * m[1] + s * m[3] - s * m[0] + c * m[2]);
    scale[2] = -s * m[1] + c * m[3];
    *angle = rotate;
}

static void compose(double angle, const double *scale, double *m) {
    double c = cos(angle), s = sin(angle);
    m[0] = c * scale[0] - s * scale[1];
    m[1] = c * scale[1] - s * scale[2];
    m[2] = s * scale[0] + c * scale[1];
    m[3] = s * scale[1] + c * scale[2];
}

static double ease(double from, double to, double dt, double settle) {
    if (settle <= 0) return to;
    if (dt <= 0) return from;
    return from + (to - from) * (1 - exp(-dt / (settle / 4.6)));
}

static double ease_angle(double from, double to, double dt, double settle) {
    if (settle <= 0) return to;
    if (dt <= 0) return from;
    double delta = to - from;
    if (delta > 3.14159265358979323846) delta -= 6.28318530717958647692;
    if (delta < -3.14159265358979323846) delta += 6.28318530717958647692;
    return from + delta * (1 - exp(-dt / (settle / 4.6)));
}

static void ease_towards(struct cursor_motion *m, bool live, double dt) {
    double angle, scale[3], target_angle = 0, target_scale[3] = { 1, 0, 1 };
    decompose(m->current, &angle, scale);
    if (live) decompose(m->target, &target_angle, target_scale);
    angle = ease_angle(angle, target_angle, dt, m->settle);
    for (int i = 0; i < 3; i++) scale[i] = ease(scale[i], target_scale[i], dt, m->settle);
    compose(angle, scale, m->current);
    for (int i = 0; i < 2; i++)
        m->current_offset[i] = ease(m->current_offset[i],
            live ? m->target_offset[i] : 0, dt, m->settle);
}

static void snap_if_settled(struct cursor_motion *m) {
    if (m->live && !placement_identity(m->target, m->target_offset)) return;
    double angle, scale[3];
    decompose(m->current, &angle, scale);
    if (fabs(angle) > 0.002 || fabs(scale[0] - 1) > 0.002 || fabs(scale[1]) > 0.002 ||
            fabs(scale[2] - 1) > 0.002 || fabs(m->current_offset[0]) > 0.05 ||
            fabs(m->current_offset[1]) > 0.05) return;
    m->current[0] = m->current[3] = 1;
    m->current[1] = m->current[2] = 0;
    m->current_offset[0] = m->current_offset[1] = 0;
}

static void refresh_software(struct tomoe *s) {
    struct cursor_motion *m = &s->cursor_motion;
    bool software = !cursor_motion_rest(s);
    if (software == m->software) return;
    m->software = software;
    screens_cursor_refresh(s);
}

static void cursor_schedule(struct tomoe *s) {
    struct output *o = output_at_physical(s, s->pointer_x, s->pointer_y);
    if (o && output_is_active(o)) screen_schedule_frame(o->screen);
}

bool cursor_trail_style(const struct tomoe *s, struct trail_style *style) {
    const struct settings *settings = &s->settings;
    if (!settings->cursor_trail_enabled || settings->cursor_trail_steps < 1) return false;
    *style = (struct trail_style){ settings->cursor_trail_cell, settings->cursor_trail_life,
        settings->cursor_trail_steps, settings->cursor_trail_shape, settings->cursor_trail_color };
    return true;
}

void cursor_trail_settings(struct tomoe *s) {
    struct trail_style style;
    bool enabled = cursor_trail_style(s, &style);
    if (enabled) effects_trail_reserve(s, style.cell);
    if (enabled == s->cursor_motion.trail_enabled) return;
    s->cursor_motion.trail_enabled = enabled;
    if (enabled) return;
    struct output *o;
    wl_list_for_each(o, &s->outputs, link)
        if (output_is_active(o)) screen_schedule_frame(o->screen);
}

static bool trail_alive(const struct tomoe *s, double now) {
    struct trail_style style;
    if (s->cursor_hidden || !cursor_trail_style(s, &style) || !s->pointer_trail_count) return false;
    int newest = (s->pointer_trail_head + POINTER_TRAIL - 1) % POINTER_TRAIL;
    return now - s->pointer_trail[newest].t < style.life;
}

static void update_acceleration(struct tomoe *s, double now) {
    struct cursor_motion *m = &s->cursor_motion;
    double vx, vy;
    pointer_velocity(s, now, &vx, &vy);
    if (m->velocity_at > 0 && now > m->velocity_at && now - m->velocity_at >= 0.0005) {
        double dt = now - m->velocity_at;
        m->acceleration[0] = (vx - m->velocity[0]) / dt;
        m->acceleration[1] = (vy - m->velocity[1]) / dt;
        double magnitude = hypot(m->acceleration[0], m->acceleration[1]);
        if (magnitude > 1e7) {
            m->acceleration[0] *= 1e7 / magnitude;
            m->acceleration[1] *= 1e7 / magnitude;
        }
    } else {
        m->acceleration[0] = m->acceleration[1] = 0;
    }
    m->velocity[0] = vx;
    m->velocity[1] = vy;
    m->velocity_at = now;
}

bool cursor_motion_advance(struct tomoe *s, double now) {
    struct cursor_motion *m = &s->cursor_motion;
    if (now > m->sample) {
        double dt = m->sample > 0 ? now - m->sample : 0;
        if (dt > 0.25) dt = 0;
        m->sample = now;
        if (shake_history_add(m, s, now)) shake_measure(m, now);
        update_acceleration(s, now);
        if (s->cursor_effects.count > 0) cursor_effects_evaluate(s, now);
        m->live = s->cursor_effects.count > 0 ||
            (m->presented > 0 && now - m->presented <= m->settle);
        ease_towards(m, m->live, dt);
        snap_if_settled(m);
        refresh_software(s);
    }
    bool active = !cursor_motion_rest(s) || trail_alive(s, now);
    if (active) cursor_schedule(s);
    return active;
}

void cursor_motion_apply(struct tomoe *s, const double matrix[4], const double offset[2],
        double settle) {
    struct cursor_motion *m = &s->cursor_motion;
    for (int i = 0; i < 4; i++) m->target[i] = matrix[i];
    for (int i = 0; i < 2; i++) m->target_offset[i] = offset[i];
    m->settle = settle < 0 ? 0 : settle > 4 ? 4 : settle;
    m->presented = animation_now();
    m->live = true;
    if (placement_identity(m->target, m->target_offset) && m->settle == 0) {
        m->current[0] = m->current[3] = 1;
        m->current[1] = m->current[2] = 0;
        m->current_offset[0] = m->current_offset[1] = 0;
    }
    refresh_software(s);
    if (!cursor_motion_rest(s) || trail_alive(s, m->presented)) {
        struct output *o;
        wl_list_for_each(o, &s->outputs, link)
            if (output_is_active(o)) screen_schedule_frame(o->screen);
    }
}

const char *tomoe_cursor_motion(struct tomoe *s) {
    free(s->motion_result);
    s->motion_result = NULL;
    double now = animation_now(), vx, vy;
    pointer_velocity(s, now, &vx, &vy);
    const char *shape = s->cursor_surface ? "clientside" :
        cursor_shape_name(s->default_cursor_shape);
    char *text = NULL;
    size_t size = 0;
    FILE *out = open_memstream(&text, &size);
    if (!out) return NULL;
    bool ok = fprintf(out,
        "(:x %.17g :y %.17g :vx %.17g :vy %.17g :time %.17g :shape \\\"%s\\\" :shake %.17g"
        " :shake-distance %.17g :ax %.17g :ay %.17g :acceleration %.17g :deformed %s)",
        s->pointer_x, s->pointer_y, vx, vy, now, shape ? shape : "default",
        s->cursor_motion.shake, s->cursor_motion.shake_distance,
        s->cursor_motion.acceleration[0], s->cursor_motion.acceleration[1],
        hypot(s->cursor_motion.acceleration[0], s->cursor_motion.acceleration[1]),
        cursor_motion_rest(s) ? "nil" : "t") >= 0;
    if (ferror(out)) ok = false;
    if (fclose(out) != 0) ok = false;
    if (!ok) {
        free(text);
        return NULL;
    }
    s->motion_result = text;
    return s->motion_result;
}

int tomoe_cursor_apply(struct tomoe *s, double a, double b, double c, double d,
        double offset_x, double offset_y, double settle) {
    if (!s || !isfinite(a) || !isfinite(b) || !isfinite(c) || !isfinite(d) ||
            !isfinite(offset_x) || !isfinite(offset_y) || !isfinite(settle)) return 0;
    double matrix[4] = { a, b, c, d };
    double offset[2] = { offset_x, offset_y };
    cursor_motion_apply(s, matrix, offset, settle);
    return 1;
}

enum { GLASS_SIDE = 256 };
static const float GLASS_FAR = 1e20f;

static const double GLASS_EDGE_INK = 0.7;

static double glass_line(double distance, double low, double high) {
    double from = distance - 0.5 > low ? distance - 0.5 : low;
    double to = distance + 0.5 < high ? distance + 0.5 : high;
    return to > from ? (to - from) * GLASS_EDGE_INK : 0;
}

static double glass_edge(double distance, double cover) {
    double dark = glass_line(distance, 0, 1) * (1 - cover);
    double light = glass_line(distance, 1, 2);
    return light - dark;
}

static void distance_line(const float *f, int n, float *d, int *v, float *z) {
    int k = 0;
    v[0] = 0;
    z[0] = -INFINITY;
    z[1] = INFINITY;
    for (int q = 1; q < n; q++) {
        float cut = ((f[q] + (float)q * q) - (f[v[k]] + (float)v[k] * v[k])) / (2.0f * (q - v[k]));
        while (cut <= z[k]) {
            k--;
            cut = ((f[q] + (float)q * q) - (f[v[k]] + (float)v[k] * v[k])) / (2.0f * (q - v[k]));
        }
        k++;
        v[k] = q;
        z[k] = cut;
        z[k + 1] = INFINITY;
    }
    k = 0;
    for (int q = 0; q < n; q++) {
        while (z[k + 1] < q) k++;
        float span = (float)(q - v[k]);
        d[q] = span * span + f[v[k]];
    }
}

static void distance_grid(float *grid, int width, int height, float *line, float *out,
        int *v, float *z) {
    for (int x = 0; x < width; x++) {
        for (int y = 0; y < height; y++) line[y] = grid[y * width + x];
        distance_line(line, height, out, v, z);
        for (int y = 0; y < height; y++) grid[y * width + x] = out[y];
    }
    for (int y = 0; y < height; y++) {
        distance_line(grid + y * width, width, out, v, z);
        memcpy(grid + y * width, out, sizeof(float) * width);
    }
}

static bool glass_lens(struct tomoe *s, const uint8_t *pixels, size_t stride, int width,
        int height) {
    struct cursor_glass *g = &s->cursor_glass;
    int pad = CURSOR_GLASS_EDGE, lw = width + 2 * pad, lh = height + 2 * pad;
    int w = lw + 2, h = lh + 2, side = w > h ? w : h;
    size_t cells = (size_t)w * h;
    float *inner = malloc(cells * sizeof(float)), *outer = malloc(cells * sizeof(float));
    float *field = malloc((size_t)lw * lh * sizeof(float) * 4);
    float *line = malloc(((size_t)side * 3 + 1) * sizeof(float));
    int *v = malloc((size_t)side * sizeof(int));
    uint8_t *lens = malloc((size_t)lw * lh * 4);
    bool body = false, ok = inner && outer && field && line && v && lens;
    for (int y = 0; ok && y < h; y++)
        for (int x = 0; x < w; x++) {
            int ix = x - pad - 1, iy = y - pad - 1;
            const uint8_t *px = ix >= 0 && iy >= 0 && ix < width && iy < height ?
                pixels + (size_t)iy * stride + (size_t)ix * 4 : NULL;
            bool in = px && px[3] && (px[0] > 2 || px[1] > 2 || px[2] > 2);
            body |= in;
            inner[y * w + x] = in ? GLASS_FAR : 0;
            outer[y * w + x] = in ? 0 : GLASS_FAR;
        }
    ok = ok && body;
    if (ok) {
        distance_grid(inner, w, h, line, line + side, v, line + 2 * side);
        distance_grid(outer, w, h, line, line + side, v, line + 2 * side);
    }
    double unit = (width > height ? width : height) / 32.0, reach = 0;
    float *edge = field, *rise = field + (size_t)lw * lh;
    for (int y = 0; ok && y < lh; y++)
        for (int x = 0; x < lw; x++) {
            size_t at = (size_t)(y + 1) * w + x + 1;
            double signed_distance = inner[at] > 0 ? 0.5 - sqrt(inner[at]) : sqrt(outer[at]) - 0.5;
            double depth = fmin(fmax(-signed_distance / (4 * unit), 0), 1);
            edge[y * lw + x] = (float)signed_distance;
            rise[y * lw + x] = (float)sqrt(fmax(1 - (1 - depth) * (1 - depth), 0));
        }
    float *bend = field + (size_t)lw * lh * 2;
    for (int y = 0; ok && y < lh; y++)
        for (int x = 0; x < lw; x++) {
            int x0 = x > 0 ? x - 1 : x, x1 = x < lw - 1 ? x + 1 : x;
            int y0 = y > 0 ? y - 1 : y, y1 = y < lh - 1 ? y + 1 : y;
            double gx = x1 > x0 ? (rise[y * lw + x1] - rise[y * lw + x0]) / (x1 - x0) : 0;
            double gy = y1 > y0 ? (rise[y1 * lw + x] - rise[y0 * lw + x]) / (y1 - y0) : 0;
            double cover = fmin(fmax(0.5 - edge[y * lw + x], 0), 1);
            bend[2 * (y * lw + x)] = (float)(-gx * 36 * unit);
            bend[2 * (y * lw + x) + 1] = (float)(-gy * 36 * unit);
            if (cover > 0) reach = fmax(reach, fmax(fabs(gx), fabs(gy)) * 36 * unit);
        }
    for (int i = 0; ok && i < lw * lh; i++) {
        double cover = fmin(fmax(0.5 - edge[i], 0), 1);
        double bx = reach > 0 ? bend[2 * i] / reach : 0, by = reach > 0 ? bend[2 * i + 1] / reach : 0;
        double ink = glass_edge(edge[i], cover);
        lens[4 * i] = (uint8_t)lround((fmin(fmax(bx, -1), 1) * 0.5 + 0.5) * 255);
        lens[4 * i + 1] = (uint8_t)lround((fmin(fmax(by, -1), 1) * 0.5 + 0.5) * 255);
        lens[4 * i + 2] = (uint8_t)lround((fmin(fmax(ink, -1), 1) * 0.5 + 0.5) * 255);
        lens[4 * i + 3] = (uint8_t)lround(cover * 255);
    }
    struct texture *map = ok ? texture_from_pixels(s->renderer, DRM_FORMAT_ABGR8888,
        (uint32_t)lw * 4, (uint32_t)lw, (uint32_t)lh, lens) : NULL;
    free(inner);
    free(outer);
    free(field);
    free(line);
    free(v);
    free(lens);
    if (!map) return false;
    texture_destroy(g->map);
    g->map = map;
    g->bend = reach;
    g->map_generation = s->cursor_generation;
    return true;
}

static bool glass_map(struct tomoe *s) {
    struct cursor_glass *g = &s->cursor_glass;
    struct cursor_image *image = &s->cursor_image;
    if (g->map && g->map_generation == s->cursor_generation) return true;
    struct buffer *buffer = image->buffer;
    if (!buffer || buffer->width <= 0 || buffer->height <= 0 || buffer->width > GLASS_SIDE ||
            buffer->height > GLASS_SIDE) return false;
    void *pixels = NULL;
    uint32_t format = 0;
    size_t stride = 0;
    if (!buffer_begin_access(buffer, BUFFER_READ, &pixels, &format, &stride)) return false;
    bool ok = pixels && stride >= (size_t)buffer->width * 4 &&
        (format == DRM_FORMAT_ARGB8888 || format == DRM_FORMAT_ABGR8888) &&
        glass_lens(s, pixels, stride, buffer->width, buffer->height);
    buffer_end_access(buffer);
    return ok;
}

static bool glass_ready(struct tomoe *s) {
    struct cursor_glass *g = &s->cursor_glass;
    if (g->broken || !s->renderer || !s->effects) return false;
    if (!g->program && !(g->program = gfx_glass_program(s->renderer))) {
        g->broken = true;
        tomoe_log(LOG_ERROR, "tomoe: the glass cursor shader did not build; the cursor stays plain");
        return false;
    }
    return glass_map(s);
}

static bool glass_admits(struct tomoe *s, const struct output *o) {
    const struct cursor_image *image = &s->cursor_image;
    return s->settings.cursor_glass_enabled && o && output_is_active(o) &&
        o->screen->transform == WL_OUTPUT_TRANSFORM_NORMAL && !o->scanout &&
        !o->planes.count && (o->screen->hardware_cursor || !o->screen->impl->cursor) &&
        !s->cursor_hidden && image->buffer && image->texture &&
        (!s->cursor_surface || s->cursor_surface->current.alpha == UINT32_MAX) &&
        cursor_motion_rest(s);
}

static void glass_redraw(struct tomoe *s, const struct screen *screen) {
    struct output *o;
    wl_list_for_each(o, &s->outputs, link)
        if (o->screen == screen && output_is_active(o)) screen_schedule_frame(o->screen);
}

static int glass_rest(void *data);

static void glass_arm(struct tomoe *s, int64_t ms) {
    struct cursor_glass *g = &s->cursor_glass;
    if (!g->timer) g->timer = wl_event_loop_add_timer(wl_display_get_event_loop(s->display),
        glass_rest, s);
    if (!g->timer) return;
    wl_event_source_timer_update(g->timer, ms < 1 ? 1 : ms > INT_MAX ? INT_MAX : (int)ms);
    g->armed = true;
}

static int glass_rest(void *data) {
    struct tomoe *s = data;
    struct cursor_glass *g = &s->cursor_glass;
    g->armed = false;
    if (!s->settings.cursor_glass_enabled || g->engaged || g->broken || s->stopping) return 0;
    int64_t delay = (int64_t)s->settings.cursor_glass_delay_ms * 1000000;
    int64_t still = monotonic_ns() - g->moved_ns;
    if (still < delay) {
        glass_arm(s, (delay - still + 999999) / 1000000);
        return 0;
    }
    struct output *o = output_at_physical(s, s->pointer_x, s->pointer_y);
    if (!glass_admits(s, o) || !glass_ready(s)) return 0;
    g->engaged = true;
    g->screen = o->screen;
    screens_cursor_refresh(s);
    glass_redraw(s, o->screen);
    return 0;
}

void cursor_glass_release(struct tomoe *s) {
    struct cursor_glass *g = &s->cursor_glass;
    if (!g->engaged) return;
    const struct screen *screen = g->screen;
    g->engaged = false;
    g->screen = NULL;
    screens_cursor_refresh(s);
    glass_redraw(s, screen);
}

void cursor_glass_track(struct tomoe *s) {
    struct cursor_glass *g = &s->cursor_glass;
    if (g->x == s->pointer_x && g->y == s->pointer_y) return;
    g->x = s->pointer_x;
    g->y = s->pointer_y;
    g->moved_ns = monotonic_ns();
    cursor_glass_release(s);
    if (!g->armed && s->settings.cursor_glass_enabled && !g->broken)
        glass_arm(s, s->settings.cursor_glass_delay_ms);
}

void cursor_glass_image(struct tomoe *s) {
    if (s->cursor_glass.engaged && !glass_map(s)) cursor_glass_release(s);
}

void cursor_glass_settings(struct tomoe *s) {
    struct cursor_glass *g = &s->cursor_glass;
    if (!s->settings.cursor_glass_enabled) cursor_glass_release(s);
    else if (!g->armed && !g->engaged && !g->broken) glass_arm(s, s->settings.cursor_glass_delay_ms);
}

bool cursor_glass_holds(const struct tomoe *s, const struct screen *screen) {
    return s->cursor_glass.engaged && s->cursor_glass.screen == screen;
}

void cursor_glass_finish(struct tomoe *s) {
    struct cursor_glass *g = &s->cursor_glass;
    if (g->timer) wl_event_source_remove(g->timer);
    texture_destroy(g->map);
    render_shader_destroy(g->program);
    *g = (struct cursor_glass){0};
}
