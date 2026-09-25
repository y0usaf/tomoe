#include "internal.h"

static const char *layer_name(int layer);
static const char *keyboard_name(int keyboard);

struct layer_plan_output {
    struct output *output;
    struct wlr_output *wlr;
    bool enabled_at_plan;
    bool included;
    int x, y, width, height;
    int scale_120;
    struct wlr_box full;
    struct wlr_box usable;
};

struct layer_plan_surface {
    struct layer *layer;
    struct layer_surface *wlr;
    struct wlr_scene_node *scene_node;
    struct wlr_output *native_output;
    bool mapped_at_plan, initialized_at_plan;
    bool included;
    bool staged;
    int override_layer, override_exclusive_zone, override_keyboard;
    int override_visible;
    struct layer_plan_output *output;
    int request_layer, request_keyboard, request_zone;
    int resolved_layer, resolved_zone, resolved_keyboard;
    bool resolved_visible, reserve, configured, unplaced;
    uint32_t anchor, request_exclusive_edge, exclusive_edge;
    uint32_t desired_width, desired_height;
    int margin[4];
    struct wlr_box logical;
    int physical_x, physical_y, physical_width, physical_height;
};

struct layer_plan {
    struct layer_plan_output *outputs;
    size_t output_count;
    size_t active_output_count;
    struct layer_plan_surface *layers;
    size_t layer_count;
    struct tomoe *server;
    bool pending_outputs;
    bool serialized;
    bool prepared;
    bool published;
};

static int64_t clamp_i64_to_int(int64_t value) {
    if (value > INT_MAX) return INT_MAX;
    if (value < INT_MIN) return INT_MIN;
    return value;
}

static int64_t round_div_away(int64_t numerator, int64_t denominator) {
    if (denominator < 0) {
        if (numerator == INT64_MIN) return INT64_MAX;
        numerator = -numerator;
        denominator = -denominator;
    }
    if (denominator == 0) return numerator >= 0 ? INT64_MAX : INT64_MIN;
    __int128 n = numerator;
    bool negative = n < 0;
    if (negative) n = -n;
    __int128 d = denominator;
    __int128 result = (n * 2 + d) / (d * 2);
    if (negative) result = -result;
    if (result > INT64_MAX) return INT64_MAX;
    if (result < INT64_MIN) return INT64_MIN;
    return (int64_t)result;
}

static int scale_120_from_double(double scale) {
    if (!isfinite(scale) || scale <= 0) return 120;
    if (scale >= (double)INT_MAX / 120.0) return INT_MAX;
    int result = pixel_round(scale * 120.0);
    return result > 0 ? result : 1;
}

static int scale_120_value(int scale_120) {
    return scale_120 > 0 ? scale_120 : 120;
}

static int64_t logical_floor(int physical, int scale_120) {
    if (physical <= 0) return 0;
    return (int64_t)physical * 120 / scale_120_value(scale_120);
}

static int64_t logical_to_physical(int64_t logical, int scale_120) {
    __int128 product = (__int128)logical * scale_120_value(scale_120);
    if (product > INT64_MAX) product = INT64_MAX;
    if (product < INT64_MIN) product = INT64_MIN;
    return round_div_away((int64_t)product, 120);
}

static int int_from_i64(int64_t value) {
    return (int)clamp_i64_to_int(value);
}

static int resolved_zone_for_scale(const struct layer *layer, int override,
        int scale_120) {
    if (override < 0) return layer->wlr->current.exclusive_zone;
    return int_from_i64(round_div_away((int64_t)override * 120,
        scale_120_value(scale_120)));
}

static enum wlr_edges exclusive_edge_for(const struct layer_surface *surface,
        int zone) {
    if (zone <= 0) return WLR_EDGE_NONE;
    uint32_t anchor = surface->current.anchor;
    if (surface->current.exclusive_edge != 0)
        anchor = surface->current.exclusive_edge;
    switch (anchor) {
    case ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP:
    case ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT |
            ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT |
            ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP:
        return WLR_EDGE_TOP;
    case ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM:
    case ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT |
            ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT |
            ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM:
        return WLR_EDGE_BOTTOM;
    case ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT:
    case ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP |
            ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM |
            ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT:
        return WLR_EDGE_LEFT;
    case ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT:
    case ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP |
            ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM |
            ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT:
        return WLR_EDGE_RIGHT;
    default:
        return WLR_EDGE_NONE;
    }
}

static void plan_output_reset(struct layer_plan_output *output) {
    output->full = (struct wlr_box){
        .x = 0, .y = 0,
        .width = int_from_i64(logical_floor(output->width, output->scale_120)),
        .height = int_from_i64(logical_floor(output->height, output->scale_120)),
    };
    output->usable = output->full;
}

static void plan_layer_geometry(struct layer_plan_surface *surface,
        struct wlr_box bounds) {
    const int64_t margin_top = surface->margin[0];
    const int64_t margin_right = surface->margin[1];
    const int64_t margin_bottom = surface->margin[2];
    const int64_t margin_left = surface->margin[3];
    int64_t width = surface->desired_width;
    int64_t height = surface->desired_height;
    int64_t x, y;

    if (width == 0) {
        x = (int64_t)bounds.x + margin_left;
        width = (int64_t)bounds.width - (margin_left + margin_right);
    } else if ((surface->anchor & ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT) &&
            (surface->anchor & ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT)) {
        x = (int64_t)bounds.x + bounds.width / 2 - width / 2;
    } else if (surface->anchor & ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT) {
        x = (int64_t)bounds.x + margin_left;
    } else if (surface->anchor & ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT) {
        x = (int64_t)bounds.x + bounds.width - width - margin_right;
    } else {
        x = (int64_t)bounds.x + bounds.width / 2 - width / 2;
    }

    if (height == 0) {
        y = (int64_t)bounds.y + margin_top;
        height = (int64_t)bounds.height - (margin_top + margin_bottom);
    } else if ((surface->anchor & ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP) &&
            (surface->anchor & ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM)) {
        y = (int64_t)bounds.y + bounds.height / 2 - height / 2;
    } else if (surface->anchor & ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP) {
        y = (int64_t)bounds.y + margin_top;
    } else if (surface->anchor & ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM) {
        y = (int64_t)bounds.y + bounds.height - height - margin_bottom;
    } else {
        y = (int64_t)bounds.y + bounds.height / 2 - height / 2;
    }

    if (width < 0) width = 0;
    if (height < 0) height = 0;
    surface->logical = (struct wlr_box){
        .x = int_from_i64(x), .y = int_from_i64(y),
        .width = int_from_i64(width), .height = int_from_i64(height),
    };
}

static void plan_reserve(struct layer_plan_output *output,
        const struct layer_plan_surface *surface, enum wlr_edges edge) {
    if (edge == WLR_EDGE_NONE || surface->resolved_zone <= 0) return;
    int64_t amount;
    switch (edge) {
    case WLR_EDGE_TOP:
        amount = (int64_t)surface->resolved_zone + surface->margin[0];
        output->usable.y = int_from_i64((int64_t)output->usable.y + amount);
        output->usable.height = int_from_i64((int64_t)output->usable.height - amount);
        break;
    case WLR_EDGE_BOTTOM:
        amount = (int64_t)surface->resolved_zone + surface->margin[2];
        output->usable.height = int_from_i64((int64_t)output->usable.height - amount);
        break;
    case WLR_EDGE_LEFT:
        amount = (int64_t)surface->resolved_zone + surface->margin[3];
        output->usable.x = int_from_i64((int64_t)output->usable.x + amount);
        output->usable.width = int_from_i64((int64_t)output->usable.width - amount);
        break;
    case WLR_EDGE_RIGHT:
        amount = (int64_t)surface->resolved_zone + surface->margin[1];
        output->usable.width = int_from_i64((int64_t)output->usable.width - amount);
        break;
    default:
        return;
    }
    if (output->usable.width < 0) output->usable.width = 0;
    if (output->usable.height < 0) output->usable.height = 0;
}

static void plan_free_arrays(struct layer_plan *plan) {
    if (!plan) return;
    free(plan->outputs);
    free(plan->layers);
    plan->outputs = NULL;
    plan->layers = NULL;
    plan->output_count = plan->active_output_count = plan->layer_count = 0;
}

void layers_preview_finish(struct tomoe *s) {
    if (!s) return;
    free(s->layer_preview_text);
    s->layer_preview_text = NULL;
    if (!s->layer_preview) return;
    plan_free_arrays(s->layer_preview);
    free(s->layer_preview);
    s->layer_preview = NULL;
}

static struct layer_plan_surface *plan_surface_for(struct layer_plan *plan,
        struct layer *layer) {
    for (size_t i = 0; i < plan->layer_count; i++) {
        if (plan->layers[i].layer == layer) return &plan->layers[i];
    }
    return NULL;
}

static struct layer_plan_output *plan_output_for_native(
        struct layer_plan *plan, struct wlr_output *wlr) {
    for (size_t i = 0; i < plan->output_count; i++) {
        if (plan->outputs[i].included && plan->outputs[i].output &&
                plan->outputs[i].output->wlr == wlr) return &plan->outputs[i];
    }
    return NULL;
}

static struct layer_plan_output *first_plan_output(struct layer_plan *plan) {
    for (size_t i = 0; i < plan->output_count; i++) {
        if (plan->outputs[i].included && plan->outputs[i].width > 0 &&
                plan->outputs[i].height > 0) return &plan->outputs[i];
    }
    return NULL;
}

static struct layer_plan_output *plan_output_for_layer(
        struct layer_plan *plan, struct layer *layer) {
    struct layer_plan_output *output = plan_output_for_native(plan,
        layer->wlr->output);
    return output ? output : first_plan_output(plan);
}

static void plan_layer_request(struct layer_plan_surface *surface) {
    struct layer_surface_state *state = &surface->layer->wlr->current;
    surface->anchor = state->anchor;
    surface->desired_width = state->desired_width;
    surface->desired_height = state->desired_height;
    surface->request_layer = (int)state->layer;
    surface->request_keyboard = (int)state->keyboard_interactive;
    surface->request_zone = state->exclusive_zone;
    surface->margin[0] = state->margin.top;
    surface->margin[1] = state->margin.right;
    surface->margin[2] = state->margin.bottom;
    surface->margin[3] = state->margin.left;
    surface->request_exclusive_edge = state->exclusive_edge;
    surface->exclusive_edge = state->exclusive_edge;
    surface->resolved_layer = surface->staged &&
        surface->override_layer >= 0 ? surface->override_layer :
        (surface->staged ? (int)state->layer : layer_of(surface->layer));
    surface->resolved_keyboard = surface->staged &&
        surface->override_keyboard >= 0 ? surface->override_keyboard :
        (surface->staged ? (int)state->keyboard_interactive : keyboard_of(surface->layer));
    surface->resolved_visible = surface->staged ?
        (surface->override_visible < 0 || surface->override_visible != 0) :
        visible_of(surface->layer);
}

static bool plan_prepare_outputs(struct layer_plan *plan) {
    if (!plan || !plan->server) return false;
    struct output_location *locations = NULL;
    size_t location_count = 0;
    if (!output_locations(plan->server, plan->pending_outputs,
            &locations, &location_count) ||
            location_count != plan->output_count) {
        free(locations);
        return false;
    }
    plan->active_output_count = 0;
    for (size_t i = 0; i < plan->output_count; i++) {
        struct layer_plan_output *output = &plan->outputs[i];
        const struct output_location *location = output_location_for(locations,
            location_count, output->output);
        output->wlr = output->output ? output->output->wlr : NULL;
        output->enabled_at_plan = location && location->active;
        if (output->enabled_at_plan) plan->active_output_count++;
        if (!location || !output->included || !output->enabled_at_plan ||
                output->width <= 0 || output->height <= 0 ||
                !output->output || !output->wlr) {
            output->full = (struct wlr_box){0};
            output->usable = output->full;
            continue;
        }
        output->scale_120 = scale_120_value(output->scale_120);
        plan_output_reset(output);
    }
    free(locations);
    return true;
}

static bool plan_layers(struct layer_plan *plan) {
    if (!plan_prepare_outputs(plan)) return false;
    for (size_t i = 0; i < plan->layer_count; i++) {
        struct layer_plan_surface *surface = &plan->layers[i];
        surface->output = NULL;
        surface->resolved_zone = 0;
        surface->configured = false;
        surface->unplaced = false;
        surface->reserve = false;
        surface->wlr = NULL;
        surface->scene_node = NULL;
        surface->native_output = NULL;
        surface->mapped_at_plan = false;
        surface->initialized_at_plan = false;
        if (!surface->included || !surface->layer) continue;
        surface->wlr = surface->layer->wlr;
        surface->scene_node = surface->layer->tree ?
            &surface->layer->tree->node : NULL;
        surface->native_output = surface->wlr ? surface->wlr->output : NULL;
        surface->mapped_at_plan = surface->layer->mapped;
        surface->initialized_at_plan = surface->wlr && surface->wlr->initialized;
        if (!surface->wlr || !surface->scene_node) continue;
        plan_layer_request(surface);
        if (surface->resolved_layer < ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND ||
                surface->resolved_layer > ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY) {
            continue;
        }
        if (plan->active_output_count == 0 && surface->layer->mapped &&
                surface->wlr->initialized) {
            int scale = scale_120_from_double(surface->layer->target.scale);
            surface->unplaced = true;
            surface->resolved_zone = resolved_zone_for_scale(surface->layer,
                surface->staged ? surface->override_exclusive_zone :
                    surface->layer->override_exclusive_zone, scale);
            surface->exclusive_edge = exclusive_edge_for(surface->wlr, surface->resolved_zone);
            surface->physical_x = surface->layer->target.x;
            surface->physical_y = surface->layer->target.y;
            surface->logical = (struct wlr_box){
                .x = surface->scene_node->x, .y = surface->scene_node->y,
                .width = int_from_i64(surface->wlr->current.actual_width),
                .height = int_from_i64(surface->wlr->current.actual_height) };
            surface->physical_width = int_from_i64(logical_to_physical(surface->logical.width, scale));
            surface->physical_height = int_from_i64(logical_to_physical(surface->logical.height, scale));
            continue;
        }
        surface->output = plan_output_for_layer(plan, surface->layer);
        if (!surface->output || surface->output->full.width <= 0 ||
                surface->output->full.height <= 0 ||
                !surface->layer->wlr->initialized || !surface->layer->ready_to_configure) {
            surface->output = NULL;
            continue;
        }
        surface->resolved_zone = resolved_zone_for_scale(surface->layer,
            surface->staged ? surface->override_exclusive_zone :
                surface->layer->override_exclusive_zone,
            surface->output->scale_120);
    }

    for (int pass = 0; pass < 2; pass++) {
        for (int want = ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY; want >= 0; want--) {
            for (size_t i = 0; i < plan->layer_count; i++) {
                struct layer_plan_surface *surface = &plan->layers[i];
                if (!surface->output || surface->resolved_layer != want ||
                        (surface->resolved_zone > 0) != (pass == 0)) continue;
                struct layer_plan_output *output = surface->output;
                struct wlr_box bounds = surface->resolved_zone == -1 ?
                    output->full : output->usable;
                plan_layer_geometry(surface, bounds);
                enum wlr_edges edge = exclusive_edge_for(surface->layer->wlr,
                    surface->resolved_zone);
                surface->exclusive_edge = edge;
                surface->configured = true;
                surface->reserve = surface->layer->mapped &&
                    surface->resolved_visible && surface->resolved_zone > 0 &&
                    edge != WLR_EDGE_NONE;
                if (surface->reserve) plan_reserve(output, surface, edge);
                int scale = output->scale_120;
                surface->physical_x = int_from_i64((int64_t)output->x +
                    logical_to_physical(surface->logical.x, scale));
                surface->physical_y = int_from_i64((int64_t)output->y +
                    logical_to_physical(surface->logical.y, scale));
                int64_t right = logical_to_physical(
                    (int64_t)surface->logical.x + surface->logical.width, scale);
                int64_t bottom = logical_to_physical(
                    (int64_t)surface->logical.y + surface->logical.height, scale);
                int64_t left = logical_to_physical(surface->logical.x, scale);
                int64_t top = logical_to_physical(surface->logical.y, scale);
                surface->physical_width = int_from_i64(right - left);
                surface->physical_height = int_from_i64(bottom - top);
            }
        }
    }
    return true;
}

static bool live_output(struct tomoe *s, struct output *wanted) {
    if (!s || !wanted) return false;
    struct output *output;
    wl_list_for_each(output, &s->outputs, link)
        if (output == wanted) return true;
    return false;
}

static bool live_layer(struct tomoe *s, struct layer *wanted) {
    if (!s || !wanted) return false;
    struct layer *layer;
    wl_list_for_each(layer, &s->layers, link)
        if (layer == wanted) return true;
    return false;
}

static bool plan_output_member(const struct layer_plan *plan,
        const struct layer_plan_output *wanted) {
    if (!plan || !wanted) return false;
    for (size_t i = 0; i < plan->output_count; i++)
        if (&plan->outputs[i] == wanted) return true;
    return false;
}

static bool plan_lifetime_valid(struct tomoe *s, const struct layer_plan *plan,
        bool check_enabled) {
    if (!s || !plan || !plan->serialized || !plan->outputs || !plan->layers ||
            plan->output_count != (size_t)wl_list_length(&s->outputs) ||
            plan->layer_count != (size_t)wl_list_length(&s->layers)) return false;

    for (size_t i = 0; i < plan->output_count; i++) {
        const struct layer_plan_output *candidate = &plan->outputs[i];
        struct output *output = candidate->output;
        if (!live_output(s, output) || !output->wlr ||
                candidate->wlr != output->wlr ||
            (check_enabled &&
                 candidate->enabled_at_plan != output_is_active(output))) return false;
        if (candidate->included &&
                (candidate->width <= 0 || candidate->height <= 0 ||
                 candidate->scale_120 <= 0 || !candidate->full.width ||
                 !candidate->full.height || candidate->full.width < 0 ||
                 candidate->full.height < 0 || candidate->usable.width < 0 ||
                 candidate->usable.height < 0)) return false;
    }

    for (size_t i = 0; i < plan->layer_count; i++) {
        const struct layer_plan_surface *candidate = &plan->layers[i];
        struct layer *layer = candidate->layer;
        if (!live_layer(s, layer)) return false;
        if (!candidate->included) continue;
        if (!candidate->wlr || !candidate->scene_node ||
                candidate->wlr != layer->wlr || !layer->tree ||
                candidate->scene_node != &layer->tree->node ||
                layer->target.id == 0 ||
                candidate->mapped_at_plan != layer->mapped ||
                candidate->initialized_at_plan != layer->wlr->initialized ||
                candidate->native_output != layer->wlr->output) return false;

        const struct layer_surface_state *state = &layer->wlr->current;
        if (candidate->anchor != state->anchor ||
                candidate->desired_width != state->desired_width ||
                candidate->desired_height != state->desired_height ||
                candidate->request_layer != (int)state->layer ||
                candidate->request_keyboard != (int)state->keyboard_interactive ||
                candidate->request_zone != state->exclusive_zone ||
                candidate->margin[0] != state->margin.top ||
                candidate->margin[1] != state->margin.right ||
                candidate->margin[2] != state->margin.bottom ||
                candidate->margin[3] != state->margin.left ||
                candidate->request_exclusive_edge != state->exclusive_edge) return false;
        if (candidate->output && !plan_output_member(plan, candidate->output)) return false;
        if (candidate->unplaced) {
            if (plan->active_output_count != 0 || candidate->output || candidate->configured ||
                    !layer->mapped || !layer->wlr->initialized)
                return false;
            continue;
        }
        if (!candidate->output || !candidate->configured ||
                !candidate->output->output ||
                candidate->output->wlr != candidate->output->output->wlr ||
                candidate->output->full.width <= 0 ||
                candidate->output->full.height <= 0 ||
                candidate->resolved_layer < ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND ||
                candidate->resolved_layer > ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY ||
                candidate->physical_width < 0 || candidate->physical_height < 0) {
            if (layer->mapped) return false;
        }
    }
    return true;
}

static int layer_presentation_band(int layer) {
    switch (layer) {
    case ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND: return 0;
    case ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM: return 1;
    case ZWLR_LAYER_SHELL_V1_LAYER_TOP: return 3;
    case ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY: return 5;
    default: return -1;
    }
}

static bool candidate_output_matches(const struct presentation *plan,
        const struct layer_plan_output *output) {
    if (!plan || !output || !output->output || !output->wlr) return false;
    const struct presentation_output *candidate = presentation_output_for(plan,
        output->wlr);
    if (!candidate || candidate->output != output->output) return false;
    return candidate->box.x == output->x && candidate->box.y == output->y &&
        candidate->box.width == output->width &&
        candidate->box.height == output->height &&
        candidate->scale_120 == output->scale_120;
}

static bool layers_plan_arithmetic_valid(const struct layer_plan *plan) {
    if (!plan) return false;
    for (size_t i = 0; i < plan->output_count; i++) {
        const struct layer_plan_output *output = &plan->outputs[i];
        if (!output->included) continue;
        if (output->width <= 0 || output->height <= 0 || output->scale_120 <= 0 ||
                output->full.width <= 0 || output->full.height <= 0 ||
                output->usable.width < 0 || output->usable.height < 0) return false;
    }
    for (size_t i = 0; i < plan->layer_count; i++) {
        const struct layer_plan_surface *surface = &plan->layers[i];
        if (!surface->included || !surface->layer || !surface->layer->mapped) continue;
        if (surface->unplaced) continue;
        if (!surface->output || !surface->configured ||
                !plan_output_member(plan, surface->output) ||
                !surface->output->output ||
                !surface->output->included || surface->output->scale_120 <= 0 ||
                surface->logical.width < 0 || surface->logical.height < 0 ||
                surface->physical_width < 0 || surface->physical_height < 0 ||
                surface->resolved_layer < ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND ||
                surface->resolved_layer > ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY)
            return false;
        int64_t left = logical_to_physical(surface->logical.x,
            surface->output->scale_120);
        int64_t top = logical_to_physical(surface->logical.y,
            surface->output->scale_120);
        int64_t right = logical_to_physical(
            (int64_t)surface->logical.x + surface->logical.width,
            surface->output->scale_120);
        int64_t bottom = logical_to_physical(
            (int64_t)surface->logical.y + surface->logical.height,
            surface->output->scale_120);
        __int128 width = (__int128)right - left;
        __int128 height = (__int128)bottom - top;
        __int128 x = (__int128)surface->output->x + left;
        __int128 y = (__int128)surface->output->y + top;
        if (width < 0 || width > INT_MAX || height < 0 || height > INT_MAX ||
                x < INT_MIN || x > INT_MAX || y < INT_MIN || y > INT_MAX ||
                surface->physical_width != (int)width ||
                surface->physical_height != (int)height ||
                surface->physical_x != (int)x || surface->physical_y != (int)y)
            return false;
    }
    return true;
}

static void reset_layer_configure(struct layer *layer) {
    layer->configure_sent = false;
}

static void configure_layer_if_needed(struct layer *layer, int width, int height) {
    if (layer->configure_sent &&
            layer->last_configure_width == width &&
            layer->last_configure_height == height) {
        return;
    }

    layer_surface_configure(layer->wlr, width, height);
    layer->last_configure_width = width;
    layer->last_configure_height = height;
    layer->configure_sent = true;
}

bool layers_prepare_presentation(struct tomoe *s, struct presentation *plan) {
    if (!s || !plan || !s->layer_preview) return false;
    struct layer_plan *candidate = s->layer_preview;
    candidate->prepared = false;
    candidate->published = false;
    if ((plan->target_count && !plan->targets) ||
            (plan->output_count && !plan->outputs)) return false;
    if (!plan_lifetime_valid(s, candidate, false) ||
            !layers_plan_arithmetic_valid(candidate)) return false;

    for (size_t i = 0; i < candidate->output_count; i++) {
        struct layer_plan_output *output = &candidate->outputs[i];
        if (output->included && !candidate_output_matches(plan, output))
            return false;
    }

    size_t original_next_order = plan->next_order;
    size_t next_order = plan->next_order;
    for (size_t i = 0; i < plan->target_count; i++) {
        if (plan->targets[i].order >= next_order) {
            if (plan->targets[i].order == SIZE_MAX) {
                candidate->prepared = false;
                return false;
            }
            next_order = plan->targets[i].order + 1;
        }
    }

    for (size_t i = 0; i < candidate->layer_count; i++) {
        struct layer_plan_surface *surface = &candidate->layers[i];
        struct layer *layer = surface->layer;
        if (!layer || !live_layer(s, layer)) goto failed;
        if (!surface->included || !layer->mapped) {
            if (layer->mapped) {
                struct presentation_target *seed = presentation_target_for(plan,
                    layer->target.id);
                if (!seed) goto failed;
                seed->visible = false;
                seed->staged = false;
            }
            continue;
        }
        if (!surface->staged || (!surface->unplaced &&
                (!surface->configured || !surface->output))) goto failed;

        int band = layer_presentation_band(surface->resolved_layer);
        if (band < 0) goto failed;
        struct presentation_target *target = presentation_target_for(plan,
            layer->target.id);
        if (!target) goto failed;

        size_t order;
        if (surface->scene_node->parent == s->layer_tree[surface->resolved_layer]) {
            order = target->order;
        } else {
            if (next_order == SIZE_MAX) goto failed;
            order = next_order++;
        }

        struct target copied = layer->target;
        copied.kind = TARGET_LAYER;
        copied.x = surface->physical_x;
        copied.y = surface->physical_y;
        if (!surface->unplaced) copied.scale = surface->output->scale_120 / 120.0;
        copied.output = surface->unplaced ? NULL : surface->output->output->wlr;
        *target = (struct presentation_target){
            .node = surface->scene_node,
            .target = copied,
            .layer_x = surface->logical.x,
            .layer_y = surface->logical.y,
            .band = band,
            .order = order,
            .visible = surface->resolved_visible && layer->mapped && !surface->unplaced,
            .staged = true,
            .width = surface->logical.width,
            .height = surface->logical.height,
        };
    }
    plan->next_order = next_order;
    candidate->prepared = true;
    candidate->published = false;
    return true;

failed:
    plan->next_order = original_next_order;
    candidate->prepared = false;
    return false;
}

static bool plan_alloc(struct layer_plan *plan, size_t outputs, size_t layers) {
    *plan = (struct layer_plan){0};
    plan->output_count = outputs;
    plan->layer_count = layers;
    plan->outputs = calloc(outputs ? outputs : 1, sizeof(*plan->outputs));
    plan->layers = calloc(layers ? layers : 1, sizeof(*plan->layers));
    if (!plan->outputs || !plan->layers) {
        plan_free_arrays(plan);
        return false;
    }
    return true;
}

static bool plan_init_live(struct tomoe *s, struct layer_plan *plan) {
    size_t outputs = wl_list_length(&s->outputs);
    size_t layers = wl_list_length(&s->layers);
    if (!plan_alloc(plan, outputs, layers)) return false;
    plan->server = s;
    plan->pending_outputs = false;
    struct output_location *locations = NULL;
    size_t location_count = 0;
    if (!output_locations(s, false, &locations, &location_count) ||
            location_count != outputs) {
        free(locations);
        plan_free_arrays(plan);
        return false;
    }
    size_t i = 0;
    struct output *output;
    wl_list_for_each(output, &s->outputs, link) {
        struct layer_plan_output *dst = &plan->outputs[i++];
        const struct output_location *location = output_location_for(
            locations, location_count, output);
        dst->output = output;
        dst->included = location && location->active;
        dst->x = location ? location->x : 0;
        dst->y = location ? location->y : 0;
        dst->width = location ? location->width : 0;
        dst->height = location ? location->height : 0;
        dst->scale_120 = location ? location->scale_120 : 120;
        dst->wlr = output->wlr;
        dst->enabled_at_plan = location && location->active;
    }
    free(locations);
    i = 0;
    struct layer *layer;
    wl_list_for_each(layer, &s->layers, link) {
        struct layer_plan_surface *dst = &plan->layers[i++];
        dst->layer = layer;
        dst->included = true;
        dst->staged = false;
        dst->override_layer = layer->override_layer;
        dst->override_exclusive_zone = layer->override_exclusive_zone;
        dst->override_keyboard = layer->override_keyboard;
        dst->override_visible = layer->override_visible;
    }
    return true;
}

int tomoe_layers_begin(struct tomoe *s) {
    if (!s) return 0;
    layers_preview_finish(s);
    struct layer_plan *plan = calloc(1, sizeof(*plan));
    if (!plan) return 0;
    size_t outputs = wl_list_length(&s->outputs);
    size_t layers = wl_list_length(&s->layers);
    if (!plan_alloc(plan, outputs, layers)) {
        free(plan);
        return 0;
    }
    plan->server = s;
    plan->pending_outputs = true;
    struct output_location *locations = NULL;
    size_t location_count = 0;
    if (!output_locations(s, true, &locations, &location_count) ||
            location_count != outputs) {
        free(locations);
        plan_free_arrays(plan);
        free(plan);
        return 0;
    }
    size_t i = 0;
    struct output *output;
    wl_list_for_each(output, &s->outputs, link) {
        const struct output_location *location = output_location_for(
            locations, location_count, output);
        plan->outputs[i].output = output;
        plan->outputs[i].included = false;
        plan->outputs[i].x = location ? location->x : 0;
        plan->outputs[i].y = location ? location->y : 0;
        plan->outputs[i].width = location ? location->width : 0;
        plan->outputs[i].height = location ? location->height : 0;
        plan->outputs[i].scale_120 = location ? location->scale_120 : 120;
        plan->outputs[i].wlr = output->wlr;
        plan->outputs[i].enabled_at_plan = location && location->active;
        i++;
    }
    free(locations);
    i = 0;
    struct layer *layer;
    wl_list_for_each(layer, &s->layers, link) {
        struct layer_plan_surface *surface = &plan->layers[i++];
        surface->layer = layer;
        surface->staged = true;
        surface->override_layer = -1;
        surface->override_exclusive_zone = -1;
        surface->override_keyboard = -1;
        surface->override_visible = -1;
    }
    s->layer_preview = plan;
    return 1;
}

int tomoe_layers_output(struct tomoe *s, const char *name,
        int x, int y, int width, int height, int scale_120) {
    if (!s || !s->layer_preview || !name) return 0;
    struct layer_plan *plan = s->layer_preview;
    struct layer_plan_output *output = NULL;
    for (size_t i = 0; i < plan->output_count; i++) {
        struct output *candidate = plan->outputs[i].output;
        if (candidate && strcmp(candidate->wlr->name, name) == 0) {
            output = &plan->outputs[i];
            break;
        }
    }
    if (!output) return 1;
    output->included = true;
    output->x = x; output->y = y;
    output->width = width; output->height = height;
    output->scale_120 = scale_120_value(scale_120);
    plan->serialized = false;
    plan->prepared = false;
    return 1;
}

int tomoe_layers_surface(struct tomoe *s, uint32_t id,
        int layer, int exclusive_zone, int keyboard, int visible) {
    if (!s || !s->layer_preview) return 0;
    struct layer *native = find_layer(s, id);
    if (!native || !native->mapped) return 1;
    struct layer_plan_surface *surface = plan_surface_for(s->layer_preview, native);
    if (!surface) return 1;
    surface->included = true;
    surface->staged = true;
    surface->override_layer = layer;
    surface->override_exclusive_zone = exclusive_zone;
    surface->override_keyboard = keyboard;
    surface->override_visible = visible;
    s->layer_preview->serialized = false;
    s->layer_preview->prepared = false;
    return 1;
}

static const char *edge_name(uint32_t edge) {
    switch (edge) {
    case WLR_EDGE_TOP: return "top";
    case WLR_EDGE_RIGHT: return "right";
    case WLR_EDGE_BOTTOM: return "bottom";
    case WLR_EDGE_LEFT: return "left";
    default: return NULL;
    }
}

static bool write_edge(FILE *out, uint32_t edge) {
    const char *name = edge_name(edge);
    return name ? fprintf(out, ":%s", name) >= 0 : fputs("nil", out) >= 0;
}

static bool write_anchor_list(FILE *out, uint32_t anchor) {
    static const struct {
        uint32_t bit;
        const char *name;
    } anchors[] = {
        { ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP, "top" },
        { ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM, "bottom" },
        { ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT, "left" },
        { ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT, "right" },
    };
    if (fputc('(', out) == EOF) return false;
    for (size_t i = 0; i < sizeof(anchors) / sizeof(anchors[0]); i++) {
        if ((anchor & anchors[i].bit) && fprintf(out, " :%s", anchors[i].name) < 0)
            return false;
    }
    return fputc(')', out) != EOF;
}

static bool write_layer_plan(FILE *out, const struct layer_plan_surface *surface) {
    struct layer *layer = surface->layer;
    int scale = surface->unplaced ? scale_120_from_double(layer->target.scale) :
        surface->output->scale_120;
    int physical_zone = surface->resolved_zone < 0 ? surface->resolved_zone :
        int_from_i64(logical_to_physical(surface->resolved_zone, scale));
    int physical_margin[4];
    for (int i = 0; i < 4; i++)
        physical_margin[i] = int_from_i64(logical_to_physical(surface->margin[i], scale));
    if (fprintf(out, "(:id %u :namespace ", layer->target.id) < 0) return false;
    quote(out, layer->wlr->namespace);
    if (fprintf(out, " :layer :%s :anchors ", layer_name(surface->resolved_layer)) < 0 ||
            !write_anchor_list(out, surface->anchor) ||
            fprintf(out, " :exclusive-zone %d :margin (%d %d %d %d)"
                " :width %d :height %d :keyboard :%s :scale-120 %d :output ",
                physical_zone, physical_margin[0], physical_margin[1],
                physical_margin[2], physical_margin[3], surface->physical_width,
                surface->physical_height, keyboard_name(surface->resolved_keyboard),
                scale) < 0) return false;
    quote(out, surface->unplaced ? "" : surface->output->output->wlr->name);
    if (fprintf(out, " :visible %s :x %d :y %d :exclusive-edge ",
            surface->resolved_visible && !surface->unplaced ? "t" : "nil", surface->physical_x,
            surface->physical_y) < 0 || !write_edge(out, surface->exclusive_edge))
        return false;
    return fputc(')', out) != EOF;
}

static int physical_inset(int64_t logical, int scale_120) {
    return int_from_i64(logical_to_physical(logical, scale_120));
}

static bool write_workarea(FILE *out, const struct layer_plan_output *output) {
    struct wlr_box full = output->full;
    struct wlr_box usable = output->usable;
    int left = physical_inset(usable.x - full.x, output->scale_120);
    int top = physical_inset(usable.y - full.y, output->scale_120);
    int right = physical_inset((int64_t)full.x + full.width - usable.x - usable.width,
        output->scale_120);
    int bottom = physical_inset((int64_t)full.y + full.height - usable.y - usable.height,
        output->scale_120);
    int64_t width64 = (int64_t)output->width - left - right;
    int64_t height64 = (int64_t)output->height - top - bottom;
    if (usable.width <= 0) width64 = 0;
    if (usable.height <= 0) height64 = 0;
    if (width64 < 0) width64 = 0;
    if (height64 < 0) height64 = 0;
    int width = int_from_i64(width64);
    int height = int_from_i64(height64);
    return fprintf(out, "(:name ") >= 0 &&
        (quote(out, output->output->wlr->name),
         fprintf(out, " :x %d :y %d :width %d :height %d)",
             int_from_i64((int64_t)output->x + left),
             int_from_i64((int64_t)output->y + top), width, height) >= 0);
}

const char *tomoe_layers_preview(struct tomoe *s) {
    if (!s || !s->layer_preview) return NULL;
    free(s->layer_preview_text);
    s->layer_preview_text = NULL;
    struct layer_plan *plan = s->layer_preview;
    plan->serialized = false;
    plan->prepared = false;
    plan->published = false;
    if (!plan_layers(plan)) return NULL;
    char *text = NULL;
    size_t size = 0;
    FILE *out = open_memstream(&text, &size);
    if (!out) return NULL;
    bool success = fputs("(:layers (", out) >= 0;
    for (size_t i = 0; success && i < plan->layer_count; i++) {
        struct layer_plan_surface *surface = &plan->layers[i];
        if (surface->included && surface->staged && (surface->configured || surface->unplaced))
            success = write_layer_plan(out, surface);
    }
    if (success && fputs(") :workareas (", out) < 0) success = false;
    for (size_t i = 0; success && i < plan->output_count; i++) {
        struct layer_plan_output *output = &plan->outputs[i];
        if (output->included && output->full.width > 0 && output->full.height > 0) {
            success = write_workarea(out, output);
        }
    }
    if (success) success = fputs("))", out) >= 0;
    if (fclose(out) != 0) success = false;
    if (!success) {
        free(text);
        return NULL;
    }
    s->layer_preview_text = text;
    plan->serialized = true;
    return text;
}

struct layer *find_layer(struct tomoe *s, uint32_t id) {
    struct layer *l;
    wl_list_for_each(l, &s->layers, link) if (l->target.id == id) return l;
    return NULL;
}
int layer_of(struct layer *l) {
    return l->override_layer >= 0 ? l->override_layer : (int)l->wlr->current.layer;
}
int exclusive_zone_of(struct layer *l) {
    return l->override_exclusive_zone >= 0 ?
        resolved_zone_for_scale(l, l->override_exclusive_zone,
            scale_120_from_double(l->target.scale)) :
        l->wlr->current.exclusive_zone;
}
int keyboard_of(struct layer *l) {
    return l->override_keyboard >= 0 ?
        l->override_keyboard : (int)l->wlr->current.keyboard_interactive;
}
bool visible_of(struct layer *l) {
    return l->override_visible < 0 || l->override_visible != 0;
}
static const char *layer_name(int layer) {
    switch (layer) {
    case ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND: return "background";
    case ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM: return "bottom";
    case ZWLR_LAYER_SHELL_V1_LAYER_TOP: return "top";
    }
    return "overlay";
}
static const char *keyboard_name(int keyboard) {
    switch (keyboard) {
    case ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE: return "none";
    case ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE: return "exclusive";
    }
    return "on-demand";
}
static const struct {
    uint32_t bit;
    const char *name;
} layer_anchors[] = {
    { ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP, "top" },
    { ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM, "bottom" },
    { ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT, "left" },
    { ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT, "right" },
};
static struct layer_state layer_state_of(struct layer *l) {
    struct layer_surface_state *client = &l->wlr->current;
    struct layer_state state = {
        .anchor = client->anchor,
        .width = physical_offset(client->actual_width, l->target.scale),
        .height = physical_offset(client->actual_height, l->target.scale),
        .margin = { physical_offset(client->margin.top, l->target.scale),
            physical_offset(client->margin.right, l->target.scale),
            physical_offset(client->margin.bottom, l->target.scale),
            physical_offset(client->margin.left, l->target.scale) },
        .layer = (int)client->layer,
        .exclusive_zone = client->exclusive_zone < 0 ? client->exclusive_zone :
            physical_offset(client->exclusive_zone, l->target.scale),
        .keyboard = (int)client->keyboard_interactive,
        .desired_width = client->desired_width,
        .desired_height = client->desired_height,
        .request_margin = { client->margin.top, client->margin.right,
            client->margin.bottom, client->margin.left },
        .request_exclusive_zone = client->exclusive_zone,
        .exclusive_edge = client->exclusive_edge,
    };
    return state;
}
static int layer_state_changed(struct layer_state *last, struct layer_state *state) {
    return last->anchor != state->anchor || last->width != state->width ||
        last->height != state->height || last->layer != state->layer ||
        last->exclusive_zone != state->exclusive_zone ||
        last->keyboard != state->keyboard ||
        memcmp(last->margin, state->margin, sizeof(last->margin)) != 0 ||
        last->desired_width != state->desired_width ||
        last->desired_height != state->desired_height ||
        last->request_exclusive_zone != state->request_exclusive_zone ||
        last->exclusive_edge != state->exclusive_edge ||
        memcmp(last->request_margin, state->request_margin,
            sizeof(last->request_margin)) != 0;
}
static void layer_event(struct layer *l) {
    if (!l->mapped) return;
    struct layer_state state = layer_state_of(l);
    if (l->announced && !layer_state_changed(&l->last, &state)) return;
    struct event *event; size_t size;
    FILE *out = begin_event(l->server, &event, &size);
    if (!out) return;
    fprintf(out, "(:type :layer :id %u :namespace ", l->target.id);
    quote(out, l->wlr->namespace);
    fprintf(out, " :layer :%s :anchors (", layer_name(state.layer));
    for (size_t i = 0; i < sizeof(layer_anchors) / sizeof(layer_anchors[0]); i++) {
        if (state.anchor & layer_anchors[i].bit) fprintf(out, " :%s", layer_anchors[i].name);
    }
    fprintf(out, ") :exclusive-zone %d :margin (%d %d %d %d)"
        " :width %u :height %u :keyboard :%s :scale-120 %.0f :output ",
        state.exclusive_zone, state.margin[0], state.margin[1], state.margin[2],
        state.margin[3], state.width, state.height, keyboard_name(state.keyboard),
        l->target.scale * 120.0);
    quote(out, l->wlr->output ? l->wlr->output->name : "");
    struct layer_surface_state *client = &l->wlr->current;
    fprintf(out, " :request (:width %u :height %u :margin (%d %d %d %d)"
        " :exclusive-zone %d :anchors ", client->desired_width,
        client->desired_height, client->margin.top, client->margin.right,
        client->margin.bottom, client->margin.left, client->exclusive_zone);
    write_anchor_list(out, client->anchor);
    fputs(" :exclusive-edge ", out);
    write_edge(out, client->exclusive_edge);
    fprintf(out, " :layer :%s :keyboard :%s)", layer_name(client->layer),
        keyboard_name(client->keyboard_interactive));
    fputc(')', out);
    l->last = state;
    l->announced = true;
    end_event(l->server, event, out);
}

static void layer_reparent_to(struct layer *l, int layer) {
    if (!l || layer < ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND ||
            layer > ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY) return;
    struct wlr_scene_tree *parent = l->server->layer_tree[layer];
    if (l->tree->node.parent != parent)
        wlr_scene_node_reparent(&l->tree->node, parent);
}

static void layer_reparent(struct layer *l) {
    layer_reparent_to(l, layer_of(l));
}

static bool publish_layer_plan(struct tomoe *s, struct layer_plan *plan,
        bool candidate) {
    if (!s || !plan) return false;
    if (candidate && (!plan->prepared || plan->published ||
            !plan_lifetime_valid(s, plan, true) ||
            !layers_plan_arithmetic_valid(plan))) return false;

    if (candidate) {
        for (size_t i = 0; i < plan->layer_count; i++) {
            struct layer_plan_surface *surface = &plan->layers[i];
            if (!surface->layer || !surface->included || !surface->staged) continue;
            surface->layer->override_layer = surface->override_layer;
            surface->layer->override_exclusive_zone = surface->override_exclusive_zone;
            surface->layer->override_keyboard = surface->override_keyboard;
            surface->layer->override_visible = surface->override_visible;
        }
    }

    for (size_t i = 0; i < plan->layer_count; i++) {
        struct layer_plan_surface *surface = &plan->layers[i];
        struct layer *layer = surface->layer;
        if (!layer || !layer->wlr || !layer->tree) continue;
        if (candidate && layer->mapped && !surface->included) {
            wlr_scene_node_set_enabled(&layer->tree->node, false);
            continue;
        }
        if (candidate && layer->mapped && !surface->unplaced &&
                (!surface->included || !surface->configured || !surface->output))
            return false;

        struct layer_plan_output *output = surface->output;
        struct wlr_output *wlr_output = output && output->output ?
            output->output->wlr : layer->wlr->output;
        struct output *live_output_record = NULL;
        struct output *candidate_output;
        wl_list_for_each(candidate_output, &s->outputs, link) {
            if (candidate_output->wlr != wlr_output) continue;
            live_output_record = candidate_output;
            break;
        }
        if (!live_output_record || !output_is_active(live_output_record))
            wlr_output = any_output(s);
        if (surface->unplaced && !wlr_output)
            layer->wlr->output = NULL;
        if (wlr_output && layer->wlr->output != wlr_output) {
            layer->wlr->output = wlr_output;
        }
        if (layer->target.output != wlr_output) layer->announced = false;
        double scale = output ? output->scale_120 / 120.0 :
            (wlr_output ? snapped_scale(wlr_output->scale) : reference_scale(s));
        if (layer->target.scale != scale) layer->announced = false;
        layer->target.output = wlr_output;
        layer->target.scale = scale;
        set_surface_scale(layer->wlr->surface, scale);

        int resolved_layer = surface->configured ? surface->resolved_layer :
            layer_of(layer);
        bool visible = surface->configured ? surface->resolved_visible :
            visible_of(layer);
        layer_reparent_to(layer, resolved_layer);
        wlr_scene_node_set_enabled(&layer->tree->node,
            layer->mapped && visible && !surface->unplaced);

        if (!surface->configured || !output || (candidate && !layer->mapped)) continue;
        wlr_scene_node_set_position(&layer->tree->node,
            surface->logical.x, surface->logical.y);
        configure_layer_if_needed(layer, surface->logical.width,
            surface->logical.height);
        layer->target.x = surface->physical_x;
        layer->target.y = surface->physical_y;
    }

    if (candidate) plan->published = true;
    return true;
}

void layers_publish_presentation(struct tomoe *s) {
    if (!s || !s->layer_preview) return;
    struct layer_plan *plan = s->layer_preview;
    if (!publish_layer_plan(s, plan, true)) {
        if (!plan->published) fail(s, "layer presentation lifetime changed");
        return;
    }
    struct layer *layer;
    wl_list_for_each(layer, &s->layers, link) layer_event(layer);
}

void arrange_layers(struct tomoe *s) {
    if (s->stopping) return;
    struct layer_plan plan;
    if (!plan_init_live(s, &plan) || !plan_layers(&plan)) {
        plan_free_arrays(&plan);
        fail(s, "layer arrangement allocation failed");
        return;
    }

    if (!publish_layer_plan(s, &plan, false)) {
        plan_free_arrays(&plan);
        fail(s, "layer arrangement publication failed");
        return;
    }
    struct layer *l;
    plan_free_arrays(&plan);
    wl_list_for_each(l, &s->layers, link) layer_event(l);
    update_keyboard_focus(s);
    schedule_scene(s);
}

static void layer_set_override(struct layer *l, int layer, int exclusive_zone,
        int keyboard, int visible) {
    if (layer == -1 || (layer >= ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND &&
            layer <= ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY)) l->override_layer = layer;
    if (exclusive_zone >= -1) l->override_exclusive_zone = exclusive_zone;
    if (keyboard >= -1 &&
            keyboard <= ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_ON_DEMAND) {
        l->override_keyboard = keyboard;
    }
    if (visible >= -1 && visible <= 1) l->override_visible = visible;
}

void tomoe_layer(struct tomoe *s, uint32_t id, int layer, int exclusive_zone,
        int keyboard, int visible) {
    struct layer *l = find_layer(s, id);
    if (!l) return;
    layer_set_override(l, layer, exclusive_zone, keyboard, visible);
    layer_reparent(l);
    wlr_scene_node_set_enabled(&l->tree->node,
        l->mapped && visible_of(l));
    arrange_layers(s);
}

static void layer_commit(struct wl_listener *listener, void *data) {
    struct layer *l = wl_container_of(listener, l, commit);
    l->ready_to_configure = l->wlr->initialized;
    if (l->wlr->initial_commit) reset_layer_configure(l);
    if (l->wlr->initial_commit || l->wlr->current.committed != 0) {
        arrange_layers(l->server);
    } else {
        layer_event(l);
    }
    schedule_scene(l->server);
}
static void layer_map(struct wl_listener *listener, void *data) {
    struct layer *l = wl_container_of(listener, l, map);
    l->mapped = true;
    wlr_scene_node_set_enabled(&l->tree->node, visible_of(l));
    arrange_layers(l->server);
}
static void layer_unmap(struct wl_listener *listener, void *data) {
    struct layer *l = wl_container_of(listener, l, unmap);
    l->mapped = false;
    l->ready_to_configure = false;
    reset_layer_configure(l);
    if (l->server->grab_id == l->target.id) grab_clear(l->server);
    l->announced = false;
    l->override_layer = -1;
    l->override_exclusive_zone = -1;
    l->override_keyboard = -1;
    l->override_visible = -1;
    wlr_scene_node_set_enabled(&l->tree->node, false);
    unmap_event(l->server, l->target.id);
    arrange_layers(l->server);
}
void layer_destroyed(struct layer_surface *ls) {
    struct layer *l = ls->data;
    struct tomoe *s = l->server;
    detach(&l->commit); detach(&l->map); detach(&l->unmap);
    if (s->grab_id == l->target.id) grab_clear(s);
    wlr_scene_node_destroy(&l->tree->node);
    wl_list_remove(&l->link);
    free(l);
    arrange_layers(s);
}
void layer_popup_created(struct layer_surface *ls, struct xdg_popup *popup) {
    struct layer *l = ls->data;
    popup_create(popup, l->tree);
}
void layer_created(struct tomoe *s, struct layer_surface *wlr) {
    if (!wlr->output) wlr->output = any_output(s);
    if (s->next_id == UINT32_MAX) { fail(s, "surface IDs exhausted"); return; }
    struct layer *l = calloc(1, sizeof(*l));
    struct wlr_scene_tree *tree = l ? wlr_scene_tree_create(
        s->layer_tree[(int)wlr->current.layer]) : NULL;
    if (!tree || !wlr_scene_subsurface_tree_create(tree, wlr->surface)) {
        if (tree) wlr_scene_node_destroy(&tree->node);
        free(l);
        wl_resource_post_no_memory(wlr->resource);
        return;
    }
    reset_layer_configure(l);
    l->server = s;
    l->wlr = wlr;
    l->tree = tree;
    l->target.id = ++s->next_id;
    l->target.kind = TARGET_LAYER;
    l->target.output = wlr->output;
    l->target.scale = wlr->output ? snapped_scale(wlr->output->scale) : reference_scale(s);
    set_surface_scale(wlr->surface, l->target.scale);
    l->override_layer = -1;
    l->override_exclusive_zone = -1;
    l->override_keyboard = -1;
    l->override_visible = -1;
    l->tree->node.data = &l->target;
    wlr_scene_node_set_enabled(&l->tree->node, false);
    wlr->data = l;
    wl_list_insert(s->layers.prev, &l->link);
    listen(&l->commit, &wlr->surface->events.commit, layer_commit);
    listen(&l->map, &wlr->surface->events.map, layer_map);
    listen(&l->unmap, &wlr->surface->events.unmap, layer_unmap);
}
