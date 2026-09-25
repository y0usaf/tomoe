#include "internal.h"
#include <wlr/backend/wayland.h>
#include <wlr/backend/drm.h>
#include <wlr/types/wlr_presentation_time.h>
#include <xf86drmMode.h>
#include "ui.h"
#include <inttypes.h>

struct output_facts {
    int x, y;
    int width, height;
    int physical_width, physical_height, refresh;
    int scale_120;
    enum wl_output_transform transform;
    bool enabled;
    bool adaptive_sync_supported;
    bool adaptive_sync;
    uint64_t request_id;
    bool request_pending;
};

static int scale_120(double scale) {
    if (!isfinite(scale) || scale <= 0) return 120;
    scale = snapped_scale(scale);
    if (scale >= (double)INT_MAX / 120.0) return INT_MAX;
    return pixel_round(scale * 120.0);
}

static void output_state_facts(struct output *o, bool pending,
        struct output_facts *facts) {
    struct wlr_output *wlr = o->wlr;
    const struct wlr_output_state *state = pending ? &o->pending : NULL;
    int physical_width = wlr->width, physical_height = wlr->height;
    int refresh = wlr->refresh;
    if (state && (state->committed & WLR_OUTPUT_STATE_MODE)) {
        if (state->mode_type == WLR_OUTPUT_STATE_MODE_FIXED && state->mode) {
            physical_width = state->mode->width;
            physical_height = state->mode->height;
            refresh = state->mode->refresh;
        } else if (state->mode_type == WLR_OUTPUT_STATE_MODE_CUSTOM) {
            physical_width = state->custom_mode.width;
            physical_height = state->custom_mode.height;
            refresh = state->custom_mode.refresh;
        }
    }
    enum wl_output_transform transform = (state &&
            (state->committed & WLR_OUTPUT_STATE_TRANSFORM)) ?
        state->transform : wlr->transform;
    int width = physical_width, height = physical_height;
    if ((int)transform % 2 != 0) {
        width = physical_height; height = physical_width;
    }
    double scale = (state && (state->committed & WLR_OUTPUT_STATE_SCALE)) ?
        state->scale : wlr->scale;
    bool enabled = state && (state->committed & WLR_OUTPUT_STATE_ENABLED) ?
        state->enabled : wlr->enabled;
    if (!pending && !o->admitted) enabled = false;
    bool adaptive_sync_supported = wlr->adaptive_sync_supported;
    bool adaptive_sync = enabled && adaptive_sync_supported &&
        wlr->adaptive_sync_status == WLR_OUTPUT_ADAPTIVE_SYNC_ENABLED;
    if (state && (state->committed & WLR_OUTPUT_STATE_ADAPTIVE_SYNC_ENABLED))
        adaptive_sync = enabled && adaptive_sync_supported && state->adaptive_sync_enabled;
    *facts = (struct output_facts){
        .width = width, .height = height,
        .physical_width = physical_width, .physical_height = physical_height,
        .refresh = refresh, .scale_120 = scale_120(scale),
        .transform = transform, .enabled = enabled,
        .adaptive_sync_supported = adaptive_sync_supported,
        .adaptive_sync = adaptive_sync,
        .request_id = o->request_id,
        .request_pending = !pending && o->request_pending,
    };
}

static const char *output_mirror(const struct output *output, bool pending) {
    return pending ? output->pending_mirror : output->mirror;
}

static bool output_positioned(const struct output *output, bool pending) {
    return pending ? output->pending_positioned : output->positioned;
}

static int output_position_x(const struct output *output, bool pending) {
    return pending ? output->pending_x : output->x;
}

static int output_position_y(const struct output *output, bool pending) {
    return pending ? output->pending_y : output->y;
}

static bool output_right_edge(int64_t left, int width, int64_t *right) {
    if (width <= 0) return false;
    int64_t edge = left + (int64_t)width;
    if ((width > 0 && edge < left) || edge > INT_MAX) return false;
    *right = edge;
    return true;
}

bool output_locations(struct tomoe *s, bool pending,
        struct output_location **locations, size_t *count) {
    if (!s || !locations || !count) return false;
    *locations = NULL;
    *count = 0;
    size_t length = wl_list_length(&s->outputs);
    struct output_location *resolved = calloc(length ? length : 1,
        sizeof(*resolved));
    if (!resolved) return false;

    size_t index = 0;
    struct output *output;
    wl_list_for_each(output, &s->outputs, link) {
        struct output_facts facts;
        output_state_facts(output, pending, &facts);
        if (facts.enabled && (facts.width <= 0 || facts.height <= 0)) {
            free(resolved);
            return false;
        }
        resolved[index++] = (struct output_location){
            .output = output, .x = output_position_x(output, pending),
            .y = output_position_y(output, pending), .width = facts.width,
            .height = facts.height, .scale_120 = facts.scale_120,
            .active = facts.enabled && (pending || output->admitted),
            .mirrored = false };
    }

    int64_t right = 0;
    for (index = 0; index < length; index++) {
        struct output_location *location = &resolved[index];
        const char *mirror = output_mirror(location->output, pending);
        if (!location->active || mirror[0] ||
                !output_positioned(location->output, pending)) continue;
        int64_t edge;
        if (!output_right_edge(location->x, location->width, &edge)) {
            free(resolved);
            return false;
        }
        if (edge > right) right = edge;
    }
    for (index = 0; index < length; index++) {
        struct output_location *location = &resolved[index];
        const char *mirror = output_mirror(location->output, pending);
        if (!location->active || mirror[0] ||
                output_positioned(location->output, pending)) continue;
        if (right > INT_MAX || right < INT_MIN) {
            free(resolved);
            return false;
        }
        location->x = (int)right;
        location->y = 0;
        if (!output_right_edge(right, location->width, &right)) {
            free(resolved);
            return false;
        }
    }

    for (index = 0; index < length; index++) {
        struct output_location *location = &resolved[index];
        const char *mirror = output_mirror(location->output, pending);
        if (!location->active || !mirror[0]) continue;
        struct output_location *target = NULL;
        for (size_t target_index = 0; target_index < length; target_index++) {
            struct output_location *candidate = &resolved[target_index];
            if (!candidate->active || output_mirror(candidate->output, pending)[0] ||
                    candidate->output == location->output ||
                    strcmp(candidate->output->wlr->name, mirror) != 0) continue;
            target = candidate;
            break;
        }
        if (target) {
            location->x = target->x;
            location->y = target->y;
            location->mirrored = true;
            continue;
        }
        if (right > INT_MAX || right < INT_MIN) {
            free(resolved);
            return false;
        }
        location->x = (int)right;
        location->y = 0;
        if (!output_right_edge(right, location->width, &right)) {
            free(resolved);
            return false;
        }
    }
    *locations = resolved;
    *count = length;
    return true;
}

size_t output_active_count(struct tomoe *s, bool pending) {
    struct output_location *locations = NULL;
    size_t count = 0, active = 0;
    if (!output_locations(s, pending, &locations, &count)) return SIZE_MAX;
    for (size_t i = 0; i < count; i++) if (locations[i].active) active++;
    free(locations);
    return active;
}

const struct output_location *output_location_for(
        const struct output_location *locations, size_t count,
        struct output *output) {
    if (!locations || !output) return NULL;
    for (size_t i = 0; i < count; i++)
        if (locations[i].output == output) return &locations[i];
    return NULL;
}

static bool write_output_facts(FILE *out, struct output *o,
        const struct output_facts *facts) {
    if (!facts->enabled || facts->width == 0 || facts->height == 0) return true;
    if (fputs("(:name ", out) < 0) return false;
    quote(out, o->wlr->name);
    if (fprintf(out, " :x %d :y %d :width %d :height %d"
            " :physical-width %d :physical-height %d :refresh-mhz %d"
            " :scale-120 %d :transform %d :adaptive-sync-supported %s"
            " :adaptive-sync %s :modes (",
            facts->x, facts->y, facts->width, facts->height,
            facts->physical_width, facts->physical_height, facts->refresh,
            facts->scale_120, facts->transform,
            facts->adaptive_sync_supported ? "t" : "nil",
            facts->adaptive_sync ? "t" : "nil") < 0) return false;
    struct wlr_output_mode *mode;
    wl_list_for_each(mode, &o->wlr->modes, link) {
        if (fprintf(out, "(:width %d :height %d :refresh-mhz %d :preferred %s)",
                mode->width, mode->height, mode->refresh,
                mode->preferred ? "t" : "nil") < 0) return false;
    }
    return fputs("))", out) >= 0;
}

static bool write_output_modes(FILE *out, struct output *o) {
    if (fputs(" :modes (", out) < 0) return false;
    struct wlr_output_mode *mode;
    wl_list_for_each(mode, &o->wlr->modes, link) {
        if (fprintf(out, "(:width %d :height %d :refresh-mhz %d :preferred %s)",
                mode->width, mode->height, mode->refresh,
                mode->preferred ? "t" : "nil") < 0) return false;
    }
    return fputc(')', out) != EOF;
}

static bool write_connector(FILE *out, struct output *o, bool pending) {
    struct output_facts facts;
    output_state_facts(o, pending, &facts);
    if (fprintf(out, "(:id %" PRIu64 " :name ", o->id) < 0) return false;
    quote(out, o->wlr->name);
    if (fprintf(out, " :enabled %s :pending %s :adaptive-sync-supported %s"
            " :adaptive-sync %s :request-id %" PRIu64
            " :request-pending %s", facts.enabled ? "t" : "nil",
            (pending || o->admitted) ? "nil" : "t",
            facts.adaptive_sync_supported ? "t" : "nil",
            facts.adaptive_sync ? "t" : "nil", facts.request_id,
            facts.request_pending ? "t" : "nil") < 0) return false;
    return write_output_modes(out, o) && fputc(')', out) != EOF;
}

static bool write_active_outputs(FILE *out, struct tomoe *s, bool pending) {
    struct output_location *locations = NULL;
    size_t count = 0;
    if (!output_locations(s, pending, &locations, &count)) return false;
    bool success = true;
    for (size_t i = 0; success && i < count; i++) {
        if (!locations[i].active) continue;
        struct output_facts facts;
        output_state_facts(locations[i].output, pending, &facts);
        facts.x = locations[i].x;
        facts.y = locations[i].y;
        success = write_output_facts(out, locations[i].output, &facts);
    }
    free(locations);
    return success;
}

static bool write_connectors(FILE *out, struct tomoe *s, bool pending) {
    bool success = true;
    struct output *output;
    wl_list_for_each(output, &s->outputs, link) {
        if (!success) break;
        success = write_connector(out, output, pending);
    }
    return success;
}

static bool write_outputs_payload(FILE *out, struct tomoe *s, bool pending) {
    return fputs(":outputs (", out) >= 0 &&
        write_active_outputs(out, s, pending) &&
        fputs(") :connectors (", out) >= 0 &&
        write_connectors(out, s, pending) && fputs(")", out) >= 0;
}

static void outputs_event(struct tomoe *s) {
    if (s->configuring_outputs) return;
    uint64_t revision = s->outputs_revision + 1;
    char *text = NULL;
    size_t text_size = 0;
    FILE *staged = open_memstream(&text, &text_size);
    if (!staged) {
        fail(s, "output event stream allocation failed");
        return;
    }
    bool success = fprintf(staged, "(:type :outputs :revision %" PRIu64 " ",
        revision) >= 0;
    success = success && write_outputs_payload(staged, s, false);
    success = success && fputc(')', staged) != EOF;
    if (fclose(staged) != 0) success = false;
    if (!success) {
        free(text);
        fail(s, "output event serialization failed");
        return;
    }
    struct event *event; size_t size;
    FILE *out = begin_event(s, &event, &size);
    if (!out) {
        free(text);
        return;
    }
    fputs(text, out);
    free(text);
    s->outputs_revision = revision;
    end_event(s, event, out);
}

uint64_t tomoe_outputs_revision(struct tomoe *s) {
    return s->outputs_revision;
}

const char *tomoe_outputs_current(struct tomoe *s) {
    char *text = NULL;
    size_t size = 0;
    FILE *out = open_memstream(&text, &size);
    if (!out) return NULL;

    bool success = fprintf(out, "(:revision %" PRIu64 " ",
        s->outputs_revision) >= 0;
    if (success) success = write_outputs_payload(out, s, false);
    if (success) success = fputs(")", out) >= 0;
    if (ferror(out)) success = false;
    if (fclose(out) != 0) success = false;
    if (!success) {
        free(text);
        return NULL;
    }

    free(s->output_current);
    s->output_current = text;
    return s->output_current;
}

bool presentation_outputs(struct tomoe *s, struct presentation *plan) {
    struct output_location *locations = NULL;
    size_t count = 0;
    if (!output_locations(s, plan->outputs_changed, &locations, &count)) return false;
    size_t active = 0;
    for (size_t i = 0; i < count; i++) if (locations[i].active) active++;
    if (active != plan->output_count) {
        free(locations);
        return false;
    }
    size_t next = 0;
    for (size_t i = 0; i < count; i++) {
        if (!locations[i].active) continue;
        struct presentation_output *entry = &plan->outputs[next++];
        entry->output = locations[i].output;
        entry->scale_120 = locations[i].scale_120;
        entry->box = (struct wlr_box){ .x = locations[i].x, .y = locations[i].y,
            .width = locations[i].width, .height = locations[i].height };
    }
    free(locations);
    return true;
}

const char *tomoe_outputs_preview(struct tomoe *s) {
    free(s->output_preview);
    s->output_preview = NULL;

    char *text = NULL;
    size_t size = 0;
    FILE *out = open_memstream(&text, &size);
    if (!out) return NULL;
    bool success = fputc('(', out) != EOF;
    if (success) success = write_outputs_payload(out, s, true);
    if (success) success = fputc(')', out) != EOF;
    if (fclose(out) != 0) success = false;
    if (!success) {
        free(text);
        return NULL;
    }
    s->output_preview = text;
    return s->output_preview;
}

static struct wlr_output_mode *first_output_mode(struct wlr_output *wlr) {
    if (!wlr || wl_list_empty(&wlr->modes)) return NULL;
    struct wlr_output_mode *mode;
    return wl_container_of(wlr->modes.next, mode, link);
}

static void snapshot_output_state(struct wlr_output *wlr, struct wlr_output_state *state) {
    wlr_output_state_init(state);
    wlr_output_state_set_enabled(state, wlr->enabled);
    if (wlr->current_mode) wlr_output_state_set_mode(state, wlr->current_mode);
    else wlr_output_state_set_custom_mode(state, wlr->width, wlr->height, wlr->refresh);
    wlr_output_state_set_scale(state, wlr->scale);
    wlr_output_state_set_transform(state, wlr->transform);
    if (wlr->adaptive_sync_supported)
        wlr_output_state_set_adaptive_sync_enabled(state,
            wlr->adaptive_sync_status == WLR_OUTPUT_ADAPTIVE_SYNC_ENABLED);
}

static void snapshot_output_baseline(struct wlr_output *wlr,
        struct wlr_output_state *state) {
    wlr_output_state_init(state);
    wlr_output_state_set_enabled(state, true);
    struct wlr_output_mode *mode = wlr_output_preferred_mode(wlr);
    if (!mode) mode = first_output_mode(wlr);
    if (mode) wlr_output_state_set_mode(state, mode);
    else wlr_output_state_set_custom_mode(state, wlr->width, wlr->height,
        wlr->refresh);
    wlr_output_state_set_scale(state, wlr->scale);
    wlr_output_state_set_transform(state, wlr->transform);
    if (wlr->adaptive_sync_supported)
        wlr_output_state_set_adaptive_sync_enabled(state,
            wlr->adaptive_sync_status == WLR_OUTPUT_ADAPTIVE_SYNC_ENABLED);
}

static bool next_output_id(struct tomoe *s, uint64_t *id) {
    if (!s || !id || s->next_output_id == 0) return false;
    *id = s->next_output_id;
    if (s->next_output_id == UINT64_MAX) s->next_output_id = 0;
    else s->next_output_id++;
    return true;
}

static bool copy_output_state(struct wlr_output_state *destination,
        const struct wlr_output_state *source) {
    wlr_output_state_init(destination);
    return wlr_output_state_copy(destination, source);
}

#define OUTPUT_REQUEST_FIELDS (WLR_OUTPUT_STATE_MODE | \
    WLR_OUTPUT_STATE_SCALE | WLR_OUTPUT_STATE_TRANSFORM | \
    WLR_OUTPUT_STATE_ADAPTIVE_SYNC_ENABLED)

static bool has_output_request_fields(const struct wlr_output_state *state) {
    return state && (state->committed & OUTPUT_REQUEST_FIELDS) != 0;
}

static bool set_output_state_fields(struct wlr_output_state *destination,
        const struct wlr_output_state *source, uint32_t fields) {
    if (!destination || !source) return false;
    if (fields & WLR_OUTPUT_STATE_ENABLED)
        wlr_output_state_set_enabled(destination, source->enabled);
    if (fields & WLR_OUTPUT_STATE_MODE) {
        if (source->mode_type == WLR_OUTPUT_STATE_MODE_FIXED) {
            if (!source->mode) return false;
            wlr_output_state_set_mode(destination, source->mode);
        } else if (source->mode_type == WLR_OUTPUT_STATE_MODE_CUSTOM) {
            wlr_output_state_set_custom_mode(destination,
                source->custom_mode.width, source->custom_mode.height,
                source->custom_mode.refresh);
        } else {
            return false;
        }
    }
    if (fields & WLR_OUTPUT_STATE_SCALE)
        wlr_output_state_set_scale(destination, source->scale);
    if (fields & WLR_OUTPUT_STATE_TRANSFORM)
        wlr_output_state_set_transform(destination, source->transform);
    if (fields & WLR_OUTPUT_STATE_ADAPTIVE_SYNC_ENABLED)
        wlr_output_state_set_adaptive_sync_enabled(destination,
            source->adaptive_sync_enabled);
    return true;
}

static bool copy_output_request_state(struct wlr_output_state *destination,
        const struct wlr_output_state *source) {
    wlr_output_state_init(destination);
    return set_output_state_fields(destination, source,
        source ? source->committed & (WLR_OUTPUT_STATE_ENABLED |
            OUTPUT_REQUEST_FIELDS) : 0);
}

static bool overlay_output_request(struct wlr_output_state *destination,
        const struct wlr_output_state *request) {
    return set_output_state_fields(destination, request,
        request ? request->committed & OUTPUT_REQUEST_FIELDS : 0);
}

static bool next_output_request_id(struct output *o) {
    if (!o || o->request_id == UINT64_MAX) return false;
    o->request_id++;
    return true;
}

static bool store_output_request(struct output *o,
        const struct wlr_output_state *request) {
    if (!o || !has_output_request_fields(request)) return true;

    struct wlr_output_state deferred;
    bool copied;
    if (o->request_pending) {
        copied = copy_output_request_state(&deferred, &o->deferred);
    } else {
        copied = copy_output_request_state(&deferred, &o->initial);
    }
    if (!copied || !overlay_output_request(&deferred, request)) {
        wlr_output_state_finish(&deferred);
        return false;
    }
    if (!next_output_request_id(o)) {
        wlr_output_state_finish(&deferred);
        return false;
    }

    wlr_output_state_finish(&o->deferred);
    o->deferred = deferred;
    o->request_pending = true;
    return true;
}

static void clear_output_request(struct output *o) {
    if (!o) return;
    wlr_output_state_finish(&o->deferred);
    wlr_output_state_init(&o->deferred);
    o->request_pending = false;
}

static void promote_output_request_snapshot(struct output *o,
        struct wlr_output_state *baseline) {
    if (!o || !baseline) return;
    wlr_output_state_finish(&o->initial);
    o->initial = *baseline;
    wlr_output_state_init(baseline);
}

static void strip_disabled_state(struct wlr_output_state *state) {
    if (!(state->committed & WLR_OUTPUT_STATE_ENABLED) || state->enabled) return;
    state->committed &= ~(WLR_OUTPUT_STATE_BUFFER |
        WLR_OUTPUT_STATE_MODE | WLR_OUTPUT_STATE_ADAPTIVE_SYNC_ENABLED |
        WLR_OUTPUT_STATE_RENDER_FORMAT | WLR_OUTPUT_STATE_SUBPIXEL |
        WLR_OUTPUT_STATE_LAYERS | WLR_OUTPUT_STATE_WAIT_TIMELINE |
        WLR_OUTPUT_STATE_SIGNAL_TIMELINE | WLR_OUTPUT_STATE_COLOR_TRANSFORM |
        WLR_OUTPUT_STATE_IMAGE_DESCRIPTION);
}

static bool output_state_enabled(const struct wlr_backend_output_state *state) {
    if (!state || !state->output) return false;
    return (state->base.committed & WLR_OUTPUT_STATE_ENABLED) ?
        state->base.enabled : state->output->enabled;
}

static bool place_outputs(struct tomoe *s) {
    struct output_location *locations = NULL;
    size_t count = 0;
    if (!output_locations(s, false, &locations, &count)) {
        fail(s, "output topology resolution failed");
        return false;
    }
    for (size_t i = 0; i < count; i++) {
        if (!locations[i].active) continue;
        locations[i].output->x = locations[i].x;
        locations[i].output->y = locations[i].y;
    }
    for (size_t i = 0; i < count; i++)
        if (!locations[i].active) forget_output(s, locations[i].output->wlr);
    pointer_sync_cursors(s);
    double scale = reference_scale(s);
    bool configuring = s->configuring_outputs;
    s->configuring_outputs = true;
    bool success = true;
    for (size_t i = 0; i < count; i++) {
        struct output *o = locations[i].output;
        if (!locations[i].active) {
            wlr_output_layout_remove(s->layout, o->wlr);
            continue;
        }
        if (!wlr_output_layout_add(s->layout, o->wlr,
                pixel_round(locations[i].x / scale),
                pixel_round(locations[i].y / scale))) {
            success = false; fail(s, "output layout allocation failed"); break;
        }
    }
    s->configuring_outputs = configuring;
    free(locations);
    return success;
}

int tomoe_outputs_begin(struct tomoe *s) {
    struct output *o;
    wl_list_for_each(o, &s->outputs, link) {
        wlr_output_state_finish(&o->pending);
        if (o->request_pending) {
            wlr_output_state_init(&o->pending);
            if (!copy_output_state(&o->pending, &o->deferred)) {
                wlr_output_state_finish(&o->pending);
                return 0;
            }
        } else if (!o->admitted || o->configured) {
            wlr_output_state_init(&o->pending);
            if (!wlr_output_state_copy(&o->pending, &o->initial)) return 0;
        } else {
            snapshot_output_state(o->wlr, &o->pending);
        }
        o->pending_configured = false;
        o->pending_hold = false;
        o->pending_positioned = false;
        o->pending_mirror[0] = '\0';
    }
    return 1;
}

int tomoe_outputs_pending(struct tomoe *s) {
    if (!s) return 0;
    struct output *o;
    wl_list_for_each(o, &s->outputs, link)
        if (!o->admitted || o->request_pending) return 1;
    return 0;
}

static bool interlaced(struct wlr_output *wlr, struct wlr_output_mode *mode) {
    if (!wlr_output_is_drm(wlr)) return false;
    const drmModeModeInfo *info = wlr_drm_mode_get_info(mode);
    return info && (info->flags & DRM_MODE_FLAG_INTERLACE);
}

static struct wlr_output_mode *pick_output_mode(struct wlr_output *wlr,
        int kind, int width, int height, int refresh) {
    struct wlr_output_mode *preferred = wlr_output_preferred_mode(wlr);
    if (!preferred) preferred = first_output_mode(wlr);
    if (!preferred) return NULL;
    if (kind == 0 && refresh == 0) return preferred;
    struct wlr_output_mode *mode, *best = NULL;
    if (kind == 0) {
        width = preferred->width; height = preferred->height;
    } else if (kind == 1) {
        int64_t area = 0;
        wl_list_for_each(mode, &wlr->modes, link) {
            if (interlaced(wlr, mode) || (int64_t)mode->width * mode->height <= area) continue;
            area = (int64_t)mode->width * mode->height;
            width = mode->width; height = mode->height;
        }
        if (!area) return preferred;
    }
    wl_list_for_each(mode, &wlr->modes, link) {
        if (interlaced(wlr, mode) || mode->width != width || mode->height != height) continue;
        if (refresh <= 0) {
            if (!best || mode->refresh > best->refresh) best = mode;
        } else if (abs(mode->refresh - refresh) <= 1000 &&
                (!best || abs(mode->refresh - refresh) < abs(best->refresh - refresh))) {
            best = mode;
        }
    }
    return best ? best : preferred;
}

int tomoe_output(struct tomoe *s, const char *name, int kind,
        int width, int height, int refresh, int scale, int x, int y, int positioned) {
    if (!s || !name) return 0;
    struct output *o;
    wl_list_for_each(o, &s->outputs, link) {
        if (strcmp(o->wlr->name, name) != 0) continue;
        struct wlr_output_mode *mode = pick_output_mode(o->wlr, kind, width, height, refresh);
        if (mode) wlr_output_state_set_mode(&o->pending, mode);
        else if (kind == 2) wlr_output_state_set_custom_mode(&o->pending, width, height, refresh);
        wlr_output_state_set_scale(&o->pending, scale / 120.0f);
        o->pending_configured = true;
        o->pending_hold = false;
        o->pending_positioned = positioned != 0;
        o->pending_x = x; o->pending_y = y;
        o->pending_mirror[0] = '\0';
        return 1;
    }
    return 1;
}

int tomoe_output_options(struct tomoe *s, const char *name, int enabled,
        const char *mirror, int adaptive_sync) {
    if (!s || !name || !name[0] || strnlen(name, 129) > 128 ||
            (enabled != 0 && enabled != 1) ||
            (adaptive_sync != 0 && adaptive_sync != 1)) return 0;
    const char *requested_mirror = mirror ?: "";
    if (strnlen(requested_mirror, 129) > 128) return 0;
    struct output *o;
    wl_list_for_each(o, &s->outputs, link) {
        if (strcmp(o->wlr->name, name) != 0) continue;
        wlr_output_state_set_enabled(&o->pending, enabled != 0);
        if (o->wlr->adaptive_sync_supported)
            wlr_output_state_set_adaptive_sync_enabled(&o->pending,
                adaptive_sync != 0);
        memcpy(o->pending_mirror, requested_mirror,
            strlen(requested_mirror) + 1);
        o->pending_hold = false;
        return 1;
    }
    return 1;
}

int tomoe_output_hold(struct tomoe *s, const char *name) {
    if (!s || !name || !name[0] || strnlen(name, 129) > 128) return 0;
    struct output *o;
    wl_list_for_each(o, &s->outputs, link) {
        if (strcmp(o->wlr->name, name) != 0) continue;
        wlr_output_state_finish(&o->pending);
        if (o->admitted) {
            snapshot_output_state(o->wlr, &o->pending);
        } else {
            wlr_output_state_init(&o->pending);
            if (!wlr_output_state_copy(&o->pending, &o->initial)) return 0;
            wlr_output_state_set_enabled(&o->pending, false);
        }
        o->pending_configured = true;
        o->pending_hold = true;
        o->pending_positioned = o->positioned;
        o->pending_x = o->x; o->pending_y = o->y;
        memcpy(o->pending_mirror, o->mirror, sizeof(o->pending_mirror));
        return 1;
    }
    return 1;
}

static const char *commit_outputs(struct tomoe *s,
        struct wlr_backend_output_state *states, size_t count, bool *attempted,
        const struct presentation *plan) {
    struct wlr_output_swapchain_manager manager;
    wlr_output_swapchain_manager_init(&manager, s->backend);
    struct wlr_backend_output_state *enabled_states =
        calloc(count ? count : 1, sizeof(*enabled_states));
    const char *error = "Output renderer initialization failed; previous settings retained.";
    size_t enabled_count = 0;
    if (!enabled_states) goto done;
    for (size_t i = 0; i < count; i++) {
        if (!output_state_enabled(&states[i])) continue;
        enabled_states[enabled_count++] = states[i];
        if (states[i].output->renderer) continue;
        if (!wlr_output_init_render(states[i].output, s->allocator, s->renderer)) {
            goto done;
        }
    }
    for (size_t i = 0; i < count; i++) strip_disabled_state(&states[i].base);
    error = "Output configuration rejected by the backend; previous settings retained.";
    if (enabled_count && !wlr_output_swapchain_manager_prepare(&manager,
            enabled_states, enabled_count)) goto done;
    error = "Cannot render the requested output configuration; previous settings retained.";
    for (size_t i = 0; i < count; i++) {
        if (!output_state_enabled(&states[i])) continue;
        struct output *o;
        wl_list_for_each(o, &s->outputs, link) {
            if (o->wlr != states[i].output) continue;
            if (!render_presentation(o, &states[i].base,
                    wlr_output_swapchain_manager_get_swapchain(&manager, states[i].output), plan)) goto done;
            break;
        }
    }
    *attempted = true;
    error = "Output commit failed.";
    if (!wlr_backend_commit(s->backend, states, count)) goto done;
    wlr_output_swapchain_manager_apply(&manager);
    error = NULL;
done:
    finish_captures(s);
    wlr_output_swapchain_manager_finish(&manager);
    free(enabled_states);
    return error;
}

const char *tomoe_outputs_apply(struct tomoe *s) {
    size_t count = wl_list_length(&s->outputs), initialized = 0;
    if (count == 0) {
        if (s->presentation) presentation_publish(s);
        return s->failed ? "Presentation publication failed; stopping the compositor." : NULL;
    }
    bool was_suspended = s->screencopy && s->screencopy->suspended;
    if (s->screencopy) wlr_screencopy_manager_v1_set_suspended(s->screencopy, true);
    struct wlr_backend_output_state *states = calloc(count, sizeof(*states));
    struct wlr_backend_output_state *previous = calloc(count, sizeof(*previous));
    uint64_t *request_ids = calloc(count, sizeof(*request_ids));
    bool *request_holds = calloc(count, sizeof(*request_holds));
    struct wlr_output_state *request_states = calloc(count, sizeof(*request_states));
    const char *error = "Cannot allocate output configuration.";
    if (!states || !previous || !request_ids || !request_holds || !request_states) goto done;
    struct output *o;
    wl_list_for_each(o, &s->outputs, link) {
        size_t i = initialized++;
        states[i].output = previous[i].output = o->wlr;
        request_ids[i] = o->request_pending ? o->request_id : 0;
        request_holds[i] = o->pending_hold;
        wlr_output_state_init(&request_states[i]);
        wlr_output_state_init(&states[i].base);
        snapshot_output_state(o->wlr, &previous[i].base);
        if (o->request_pending &&
                !wlr_output_state_copy(&request_states[i], &o->deferred)) goto done;
        if (!wlr_output_state_copy(&states[i].base, &o->pending)) goto done;
    }
    s->configuring_outputs = true;
    bool attempted = false;
    bool rollback_completed = false;
    error = commit_outputs(s, states, count, &attempted, s->presentation);
    if (error && attempted) {
        bool restore_attempted = false;
        if (commit_outputs(s, previous, count, &restore_attempted, NULL)) {
            error = "Output rollback failed; stopping the compositor.";
            fail(s, error);
        } else {
            error = "Output commit failed; previous settings restored.";
            rollback_completed = true;
        }
    }
    if (!error) {
        wl_list_for_each(o, &s->outputs, link) {
            o->admitted = true;
            o->x = o->pending_x;
            o->y = o->pending_y;
            o->configured = o->pending_configured;
            o->positioned = o->pending_configured &&
                !o->pending_mirror[0] && o->pending_positioned;
            if (o->pending_configured)
                memcpy(o->mirror, o->pending_mirror, sizeof(o->mirror));
            else
                o->mirror[0] = '\0';
        }
        if (!place_outputs(s)) error = "Output layout allocation failed.";
    }
    if (!error) {
        wl_list_for_each(o, &s->outputs, link)
            wlr_xcursor_manager_load(s->cursor_manager, o->wlr->scale);
    }
    s->configuring_outputs = false;
    if (error && (!attempted || rollback_completed) && !s->failed) {
        bool notify_request = false;
        size_t i = 0;
        wl_list_for_each(o, &s->outputs, link) {
            if (o->request_pending &&
                    (request_ids[i] == 0 || o->request_id != request_ids[i])) {
                notify_request = true;
                break;
            }
            i++;
        }
        if (notify_request) outputs_event(s);
    }
    if (!error) {
        if (s->presentation) presentation_publish(s);
        else {
            windows_refresh(s);
            arrange_layers(s);
        }
        if (s->failed) {
            error = "Presentation publication failed; stopping the compositor.";
        } else {
            size_t i = 0;
            wl_list_for_each(o, &s->outputs, link) {
                if (request_ids[i] != 0) {
                    if (!request_holds[i])
                        promote_output_request_snapshot(o, &request_states[i]);
                    if (o->request_pending && o->request_id == request_ids[i])
                        clear_output_request(o);
                }
                i++;
            }
            update_workareas(s);
            outputs_event(s);
            schedule_scene(s);
        }
    }
done:
    for (size_t i = 0; i < initialized; i++) {
        wlr_output_state_finish(&states[i].base);
        wlr_output_state_finish(&previous[i].base);
        wlr_output_state_finish(&request_states[i]);
    }
    free(states); free(previous); free(request_ids); free(request_holds);
    free(request_states);
    if (s->screencopy && !was_suspended)
        wlr_screencopy_manager_v1_set_suspended(s->screencopy, false);
    return error;
}

struct wlr_output *any_output(struct tomoe *s) {
    if (!s) return NULL;
    struct output *o;
    wl_list_for_each(o, &s->outputs, link)
        if (output_is_active(o)) return o->wlr;
    return NULL;
}

static void output_frame(struct wl_listener *listener, void *data) {
    struct output *o = wl_container_of(listener, o, frame);
    if (!output_is_active(o)) return;
    struct wlr_output_state state;
    wlr_output_state_init(&state);
    struct wlr_surface *scanout = scanout_surface(o);
    if (scanout) {
        wlr_output_state_set_buffer(&state, &scanout->buffer->base);
        if (!wlr_output_test_state(o->wlr, &state)) {
            wlr_output_state_finish(&state);
            wlr_output_state_init(&state);
            scanout = NULL;
        }
    }
    if (scanout != o->scanout)
        wlr_log(WLR_DEBUG, "tomoe: output %s direct scanout %s", o->wlr->name,
            scanout ? "engaged" : "disengaged");
    o->scanout = scanout;
    if (scanout) {
        refresh_scene(o->server);
        pointer_sync_cursors(o->server);
    }
    bool success = scanout || render_output(o, &state, NULL);
    if (success && scanout)
        wlr_presentation_surface_scanned_out_on_output(scanout, o->wlr);
    else if (success) surfaces_textured(o);
    if (success && o->gamma_dirty) gamma_apply(o, &state);
    if (success && scanout && windows_want_tearing(o->server, o)) {
        state.tearing_page_flip = true;
        if (!wlr_output_test_state(o->wlr, &state)) state.tearing_page_flip = false;
    }
    success = success && wlr_output_commit_state(o->wlr, &state);
    finish_output_capture(o);
    wlr_output_state_finish(&state);
    if (!success) { fail(o->server, "output commit failed"); return; }
    lock_frame_rendered(o->server, o->wlr);
    if (windows_animate(o->server)) wlr_output_schedule_frame(o->wlr);
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    frame_done(o, &now);
}
static void output_needs_frame(struct wl_listener *listener, void *data) {
    struct output *o = wl_container_of(listener, o, needs_frame);
    if (output_is_active(o)) wlr_output_schedule_frame(o->wlr);
}
void outputs_request_nested_size(struct tomoe *s) {
    struct output *o;
    wl_list_for_each(o, &s->outputs, link) {
        if (!wlr_output_is_wl(o->wlr)) continue;
        struct wlr_output_state state;
        wlr_output_state_init(&state);
        wlr_output_state_set_custom_mode(&state, s->settings.nested_width,
            s->settings.nested_height, 0);
        if (!store_output_request(o, &state)) fail(s, "output request could not be retained");
        wlr_output_state_finish(&state);
    }
    outputs_event(s);
}
static void output_request(struct wl_listener *listener, void *data) {
    struct output *o = wl_container_of(listener, o, request);
    const struct wlr_output_event_request_state *event = data;
    if (!store_output_request(o, event ? event->state : NULL)) {
        fail(o->server, "output request could not be retained");
    } else if (has_output_request_fields(event ? event->state : NULL)) {
        outputs_event(o->server);
    }
}
static void output_destroy(struct wl_listener *listener, void *data) {
    struct output *o = wl_container_of(listener, o, destroy);
    struct tomoe *s = o->server;
    struct wlr_output *wlr = o->wlr;
    finish_output_capture(o);
    ui_output_finish(s, wlr->name);
    detach(&o->frame); detach(&o->request); detach(&o->destroy); detach(&o->needs_frame);
    forget_output(s, wlr);
    wlr_output_state_finish(&o->initial);
    wlr_output_state_finish(&o->pending);
    wlr_output_state_finish(&o->deferred);
    wl_list_remove(&o->link); free(o);
    if (s->stopping) return;
    struct layer *l;
    wl_list_for_each(l, &s->layers, link) if (l->wlr->output == wlr) l->wlr->output = NULL;
    place_outputs(s);
    windows_refresh(s);
    outputs_event(s);
    arrange_layers(s);
    schedule_scene(s);
}
static void layout_change(struct wl_listener *listener, void *data) {
    struct tomoe *s = wl_container_of(listener, s, layout_change);
    if (s->configuring_outputs) return;
    outputs_event(s);
    arrange_layers(s);
    update_workareas(s);
    windows_refresh(s);
    schedule_scene(s);
}
static void new_output(struct wl_listener *listener, void *data) {
    struct tomoe *s = wl_container_of(listener, s, new_output);
    struct wlr_output *wlr = data;
    uint64_t id;
    if (!next_output_id(s, &id)) {
        fail(s, "output identifiers exhausted"); return;
    }
    struct output *o = calloc(1, sizeof(*o));
    if (!o) { fail(s, "output allocation failed"); return; }
    o->server = s; o->wlr = wlr; o->id = id;
    o->admitted = false;
    snapshot_output_baseline(wlr, &o->initial);
    wlr_output_state_init(&o->deferred);
    if (!copy_output_state(&o->pending, &o->initial)) {
        wlr_output_state_finish(&o->initial);
        free(o);
        fail(s, "output baseline allocation failed");
        return;
    }
    wl_list_insert(s->outputs.prev, &o->link);
    listen(&o->frame, &wlr->events.frame, output_frame);
    listen(&o->request, &wlr->events.request_state, output_request);
    listen(&o->destroy, &wlr->events.destroy, output_destroy);
    listen(&o->needs_frame, &wlr->events.needs_frame, output_needs_frame);
    outputs_event(s);
}
void outputs_listen(struct tomoe *s) {
    listen(&s->new_output, &s->backend->events.new_output, new_output);
    listen(&s->layout_change, &s->layout->events.change, layout_change);
}
