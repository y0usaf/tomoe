#include "internal.h"
#include "lisp.h"

enum { CURSOR_WINDOW_MS = 100 };

static struct tomoe *cursor_server;

static void pointer_velocity(const struct tomoe *s, double now, double *vx, double *vy);

static lisp_value cursor_plist(lisp_value plist, const char *name) {
    lisp_value tail = plist;
    while (lisp_consp(tail)) {
        lisp_value rest = lisp_cdr(tail);
        if (!lisp_consp(rest)) return lisp_nil();
        if (lisp_eq_keyword(lisp_car(tail), name)) return lisp_car(rest);
        tail = lisp_cdr(rest);
    }
    return lisp_nil();
}

static int cursor_field(lisp_value head) {
    if (lisp_eq_keyword(head, "SPEED")) return CURSOR_FIELD_SPEED;
    if (lisp_eq_keyword(head, "ANGLE")) return CURSOR_FIELD_ANGLE;
    if (lisp_eq_keyword(head, "ACCELERATION")) return CURSOR_FIELD_ACCELERATION;
    if (lisp_eq_keyword(head, "SHAKE")) return CURSOR_FIELD_SHAKE;
    if (lisp_eq_keyword(head, "VX")) return CURSOR_FIELD_VX;
    if (lisp_eq_keyword(head, "VY")) return CURSOR_FIELD_VY;
    lisp_error("Unsupported cursor term field.");
}

static struct cursor_term_clause *term_clause(struct cursor_term *term, int field, double scale) {
    if (term->count == CURSOR_TERM_CLAUSES) lisp_error("Cursor term exceeds its clause budget.");
    struct cursor_term_clause *clause = &term->clauses[term->count++];
    clause->field = field;
    clause->scale = scale;
    clause->above = -INFINITY;
    clause->below = INFINITY;
    return clause;
}

static void parse_clause(struct cursor_term *term, lisp_value clause) {
    lisp_value rest = lisp_cdr(clause);
    if (!lisp_consp(rest)) lisp_error("Cursor terms need a scale.");
    double scale = lisp_get_double(lisp_car(rest));
    struct cursor_term_clause *entry = term_clause(term, cursor_field(lisp_car(clause)), scale);
    lisp_value options = lisp_cdr(rest);
    lisp_value above = cursor_plist(options, "ABOVE"), below = cursor_plist(options, "BELOW");
    if (!lisp_nilp(above)) entry->above = lisp_get_double(above);
    if (!lisp_nilp(below)) entry->below = lisp_get_double(below);
}

static void parse_term(lisp_value value, struct cursor_term *term) {
    term->count = 0;
    if (lisp_nilp(value)) return;
    if (!lisp_consp(value)) {
        term_clause(term, CURSOR_FIELD_CONSTANT, lisp_get_double(value));
        return;
    }
    if (lisp_keywordp(lisp_car(value))) {
        parse_clause(term, value);
        return;
    }
    for (lisp_value tail = value; lisp_consp(tail); tail = lisp_cdr(tail)) {
        lisp_value clause = lisp_car(tail);
        if (lisp_consp(clause)) parse_clause(term, clause);
        else term_clause(term, CURSOR_FIELD_CONSTANT, lisp_get_double(clause));
    }
}

static void parse_effect(lisp_value spec, struct cursor_effect *effect) {
    memset(effect, 0, sizeof(*effect));
    effect->settle = 0.08;
    parse_term(cursor_plist(spec, "ROTATION"), &effect->rotation);
    parse_term(cursor_plist(spec, "SCALE"), &effect->scale);
    parse_term(cursor_plist(spec, "STRETCH"), &effect->stretch);
    parse_term(cursor_plist(spec, "STRETCH-ANGLE"), &effect->stretch_angle);
    parse_term(cursor_plist(spec, "OFFSET-X"), &effect->offset_x);
    parse_term(cursor_plist(spec, "OFFSET-Y"), &effect->offset_y);
    lisp_value settle = cursor_plist(spec, "SETTLE");
    if (!lisp_nilp(settle)) effect->settle = lisp_get_double(settle);
    lisp_value trail = cursor_plist(spec, "TRAIL");
    if (lisp_consp(trail)) {
        effect->trail_on = true;
        effect->trail.cell = lisp_get_double(cursor_plist(trail, "CELL"));
        effect->trail.life = lisp_get_double(cursor_plist(trail, "LIFE"));
        effect->trail.steps = (int)lisp_get_int(cursor_plist(trail, "STEPS"));
        effect->trail.rgba = (uint32_t)lisp_get_uint(cursor_plist(trail, "COLOR"));
    }
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
    bool trail_on = false;
    struct trail_style trail = { 0 };
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
        if (effect->trail_on) { trail = effect->trail; trail_on = true; }
    }
    if (!(settle > 0)) settle = 0.08;
    cursor_motion_apply(s, matrix, offset, settle, trail_on ? &trail : NULL);
}

static lisp_value cursor_effects(int argc, lisp_value *argv) {
    cursor_server->cursor_effects.count = 0;
    for (lisp_value tail = argv[0]; lisp_consp(tail); tail = lisp_cdr(tail)) {
        lisp_value spec = cursor_plist(lisp_car(tail), "SPEC");
        if (cursor_server->cursor_effects.count == CURSOR_EFFECTS_MAX)
            lisp_error("Cursor effect set exceeds its budget.");
        struct cursor_effect *effect =
            &cursor_server->cursor_effects.entries[cursor_server->cursor_effects.count++];
        parse_effect(spec, effect);
        if (effect->trail_on && !effects_admit_trail(cursor_server)) effect->trail_on = false;
    }
    cursor_effects_evaluate(cursor_server, animation_now());
    return lisp_nil();
}

void cursor_effects_register_primitives(struct tomoe *server) {
    cursor_server = server;
    lisp_define_primitive("%cursor-effects", cursor_effects, 1, 1);
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

static bool trail_alive(const struct tomoe *s, double now) {
    const struct cursor_motion *m = &s->cursor_motion;
    if (m->trail.steps <= 0 || !s->pointer_trail_count) return false;
    int newest = (s->pointer_trail_head + POINTER_TRAIL - 1) % POINTER_TRAIL;
    return now - s->pointer_trail[newest].t < m->trail.life;
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
        double settle, const struct trail_style *trail) {
    struct cursor_motion *m = &s->cursor_motion;
    for (int i = 0; i < 4; i++) m->target[i] = matrix[i];
    for (int i = 0; i < 2; i++) m->target_offset[i] = offset[i];
    m->settle = settle < 0 ? 0 : settle > 4 ? 4 : settle;
    m->presented = animation_now();
    m->live = true;
    m->trail = trail ? *trail : (struct trail_style){0};
    if (m->trail.steps < 0) m->trail.steps = 0;
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
        double offset_x, double offset_y, double settle, double cell, double life,
        int steps, uint32_t rgba) {
    if (!s || !isfinite(a) || !isfinite(b) || !isfinite(c) || !isfinite(d) ||
            !isfinite(offset_x) || !isfinite(offset_y) || !isfinite(settle) ||
            !isfinite(cell) || !isfinite(life)) return 0;
    struct trail_style trail = {0};
    if (steps > 0 && effects_admit_trail(s)) {
        trail = (struct trail_style){ cell < 1 ? 1 : cell > 256 ? 256 : cell,
            life < 0 ? 0 : life > 4 ? 4 : life, steps > 8 ? 8 : steps, rgba };
    }
    double matrix[4] = { a, b, c, d };
    double offset[2] = { offset_x, offset_y };
    cursor_motion_apply(s, matrix, offset, settle, &trail);
    return 1;
}
