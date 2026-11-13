#include "internal.h"
#include "motion.h"

void motion_plan_init(struct motion_plan *plan) {
    *plan = (struct motion_plan){0};
}

void motion_plan_finish(struct motion_plan *plan) {
    motion_plan_init(plan);
}

static uint64_t region_mask(struct tomoe *s, const struct motion_plan *plan) {
    uint64_t mask = 0;
    for (size_t i = 0; i < plan->region_count; i++) {
        const struct motion_region *r = &plan->region[i];
        if (s->pointer_x >= r->x && s->pointer_y >= r->y &&
                s->pointer_x < (double)r->x + r->width && s->pointer_y < (double)r->y + r->height)
            mask |= 1ULL << i;
    }
    return mask;
}

static bool edge_hit(struct tomoe *s, const struct motion_edge *e) {
    int thickness = e->thickness > 0 ? e->thickness : 1;
    struct output *o;
    wl_list_for_each(o, &s->outputs, link) {
        if (!o->screen->enabled) continue;
        struct box box;
        physical_output_box(o, &box);
        double x = s->pointer_x - box.x, y = s->pointer_y - box.y;
        if (x < 0 || y < 0 || x >= box.width || y >= box.height) continue;
        switch (e->side) {
            case MOTION_EDGE_TOP: return y < thickness;
            case MOTION_EDGE_BOTTOM: return y >= box.height - thickness;
            case MOTION_EDGE_LEFT: return x < thickness;
            default: return x >= box.width - thickness;
        }
    }
    return false;
}

static uint64_t edge_mask(struct tomoe *s, const struct motion_plan *plan) {
    uint64_t mask = 0;
    for (size_t i = 0; i < plan->edge_count && i < 4; i++)
        if (edge_hit(s, &plan->edge[i])) mask |= 1ULL << i;
    return mask;
}

static int clamp_int(int value, int low, int high) {
    return value < low ? low : value > high ? high : value;
}

void motion_publish(struct tomoe *s, struct motion_plan *plan) {
    struct motion_state *m = &s->motion;
    bool drag = m->drag_active;
    m->plan = *plan;
    motion_plan_init(plan);
    m->tracked = true;
    m->inside = region_mask(s, &m->plan);
    m->inside_edges = edge_mask(s, &m->plan);
    m->track_x = s->pointer_x;
    m->track_y = s->pointer_y;
    m->moved = 0;
    if (drag && (!m->plan.fast || m->plan.drag_id != m->drag_id ||
            m->plan.drag_mode != m->drag_mode || m->plan.drag_bound != m->drag_bound))
        motion_drag_end(s);
}

bool motion_drag_begin(struct tomoe *s) {
    struct motion_state *m = &s->motion;
    motion_drag_end(s);
    if (!m->plan.fast || !m->plan.drag_mode || !m->plan.drag_id) return false;
    if (s->grab_mode != m->plan.drag_mode || s->grab_id != m->plan.drag_id) return false;
    if (!find_window(s, s->grab_id)) return false;
    m->drag_id = s->grab_id;
    m->drag_mode = s->grab_mode;
    m->drag_bound = m->plan.drag_bound;
    m->drag_min = m->plan.drag_min;
    m->drag_max = m->plan.drag_max;
    if (!window_box_native(s, s->grab_id, &m->base_x, &m->base_y, &m->base_width, &m->base_height))
        return false;
    m->origin_x = s->press_world_x;
    m->origin_y = s->press_world_y;
    m->last_x = m->origin_x;
    m->last_y = m->origin_y;
    m->dx = m->dy = 0;
    m->drag_pending = false;
    m->drag_active = true;
    return true;
}

void motion_drag_flush(struct tomoe *s) {
    struct motion_state *m = &s->motion;
    if (!m->drag_pending) return;
    m->drag_pending = false;
    queue_grab_motion(s, m->drag_id, m->drag_mode, m->last_x, m->last_y, m->dx, m->dy);
    m->dx = m->dy = 0;
}

void motion_drag_end(struct tomoe *s) {
    struct motion_state *m = &s->motion;
    if (!m->drag_active) return;
    motion_drag_flush(s);
    m->drag_active = false;
    m->drag_pending = false;
}

bool motion_drag_motion(struct tomoe *s) {
    struct motion_state *m = &s->motion;
    if (!m->drag_active) return false;
    if (s->grab_mode != m->drag_mode || s->grab_id != m->drag_id || !find_window(s, m->drag_id)) {
        motion_drag_end(s);
        return false;
    }
    double wx = s->pointer_x, wy = s->pointer_y;
    screen_to_world(s, &wx, &wy);
    int x = pixel_round(wx), y = pixel_round(wy);
    int dx = x - m->origin_x, dy = y - m->origin_y;
    int box_x = m->base_x, box_y = m->base_y, box_w = m->base_width, box_h = m->base_height;
    if (m->drag_mode == MOTION_DRAG_RESIZE) {
        box_w = clamp_int(m->base_width + dx, m->drag_min, m->drag_max);
        box_h = clamp_int(m->base_height + dy, m->drag_min, m->drag_max);
    } else if (m->drag_mode == MOTION_DRAG_MOVE) {
        box_x = clamp_int(m->base_x + dx, -m->drag_bound, m->drag_bound);
        box_y = clamp_int(m->base_y + dy, -m->drag_bound, m->drag_bound);
    } else {
        motion_drag_end(s);
        return false;
    }
    m->box_x = box_x;
    m->box_y = box_y;
    m->box_width = box_w;
    m->box_height = box_h;
    window_place_native(s, m->drag_id, box_x, box_y, box_w, box_h);
    m->dx += x - m->last_x;
    m->dy += y - m->last_y;
    m->last_x = x;
    m->last_y = y;
    m->drag_pending = true;
    return true;
}

static void motion_report(struct tomoe *s, uint64_t entered, uint64_t left,
        uint64_t edges_in, uint64_t edges_out, int travelled) {
    static const char *const sides[] = { "top", "bottom", "left", "right" };
    struct motion_state *m = &s->motion;
    struct event *event;
    size_t size;
    FILE *out = begin_event(s, &event, &size);
    if (!out) return;
    fprintf(out, "(:type :pointer :state :motion :id %u :moved t :x %d :y %d :distance %d",
        s->hovered, pixel_round(s->pointer_x), pixel_round(s->pointer_y), travelled);
    fputs(" :entered (", out);
    for (size_t i = 0; i < m->plan.region_count; i++)
        if (entered >> i & 1) {
            fputs(" \"", out);
            fputs(m->plan.region[i].name, out);
            fputs("\"", out);
        }
    fputs(") :left (", out);
    for (size_t i = 0; i < m->plan.region_count; i++)
        if (left >> i & 1) {
            fputs(" \"", out);
            fputs(m->plan.region[i].name, out);
            fputs("\"", out);
        }
    fputs(") :edges-in (", out);
    for (size_t i = 0; i < m->plan.edge_count && i < 4; i++)
        if (edges_in >> i & 1) fprintf(out, " :%s", sides[m->plan.edge[i].side]);
    fputs(") :edges-out (", out);
    for (size_t i = 0; i < m->plan.edge_count && i < 4; i++)
        if (edges_out >> i & 1) fprintf(out, " :%s", sides[m->plan.edge[i].side]);
    fputs("))\n", out);
    end_event(s, event, out);
}

int motion_wake(struct tomoe *s) {
    struct motion_state *m = &s->motion;
    const struct motion_plan *plan = &m->plan;
    if (!plan->fast || lock_active(s)) {
        m->tracked = false;
        m->moved = 0;
        return 0;
    }
    if (!m->tracked) {
        m->inside = region_mask(s, plan);
        m->inside_edges = edge_mask(s, plan);
        m->last_x = s->pointer_x;
        m->last_y = s->pointer_y;
        m->tracked = true;
        return 0;
    }
    uint64_t inside = region_mask(s, plan), edges = edge_mask(s, plan);
    uint64_t entered = inside & ~m->inside, left = m->inside & ~inside;
    uint64_t edges_in = edges & ~m->inside_edges, edges_out = m->inside_edges & ~edges;
    int travelled = 0;
    double dx = s->pointer_x - m->track_x, dy = s->pointer_y - m->track_y;
    m->moved += pixel_round(sqrt(dx * dx + dy * dy));
    travelled = m->moved;
    m->inside = inside;
    m->inside_edges = edges;
    m->track_x = s->pointer_x;
    m->track_y = s->pointer_y;
    bool region_wake = plan->regions && (entered || left);
    bool edge_wake = plan->edges && (edges_in || edges_out);
    bool distance_wake = plan->distance_wake && plan->distance > 0 && m->moved >= plan->distance;
    if (!region_wake && !edge_wake && !distance_wake) return 0;
    m->moved = 0;
    motion_report(s, region_wake ? entered : 0, region_wake ? left : 0,
        edge_wake ? edges_in : 0, edge_wake ? edges_out : 0, travelled);
    return 1;
}

int tomoe_motion_begin(struct tomoe *s) {
    if (!s->presentation) return 0;
    motion_plan_finish(&s->presentation->motion);
    s->presentation->motion_staged = true;
    return 1;
}

int tomoe_motion(struct tomoe *s, int fast, int hover, int regions, int edges, int distance) {
    if (!s->presentation || !s->presentation->motion_staged) return 0;
    if (distance < 0 || distance > 1048576) return 0;
    struct motion_plan *plan = &s->presentation->motion;
    plan->fast = fast != 0;
    plan->hover = hover != 0;
    plan->regions = regions != 0;
    plan->edges = edges != 0;
    plan->distance = distance;
    plan->distance_wake = distance > 0;
    return 1;
}

int tomoe_motion_region(struct tomoe *s, const char *name, int x, int y, int width, int height) {
    if (!s->presentation || !s->presentation->motion_staged || !name ||
            !*name || strlen(name) >= MOTION_NAME_MAX || width < 1 || height < 1) return 0;
    struct motion_plan *plan = &s->presentation->motion;
    if (plan->region_count >= MOTION_REGIONS) return 0;
    struct motion_region *region = &plan->region[plan->region_count++];
    snprintf(region->name, sizeof(region->name), "%s", name);
    region->x = x;
    region->y = y;
    region->width = width;
    region->height = height;
    return 1;
}

int tomoe_motion_edge(struct tomoe *s, int side, int thickness) {
    if (!s->presentation || !s->presentation->motion_staged) return 0;
    if (side < 0 || side > 3 || thickness < 1 || thickness > 1048576) return 0;
    struct motion_plan *plan = &s->presentation->motion;
    if (plan->edge_count >= 4) return 0;
    plan->edge[plan->edge_count++] = (struct motion_edge){ .side = side, .thickness = thickness };
    return 1;
}

int tomoe_motion_drag(struct tomoe *s, uint32_t id, int mode, int bound, int min_size,
        int max_size) {
    if (!s->presentation || !s->presentation->motion_staged) return 0;
    if (mode < 0 || mode > 3 || bound < 0 || bound > 1048576 || min_size < 1 ||
            max_size < min_size || max_size > 1048576) return 0;
    struct motion_plan *plan = &s->presentation->motion;
    plan->drag_id = id;
    plan->drag_mode = mode;
    plan->drag_bound = bound;
    plan->drag_min = min_size;
    plan->drag_max = max_size;
    return 1;
}
