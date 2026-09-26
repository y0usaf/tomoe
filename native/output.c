#include "internal.h"
#include <xf86drm.h>
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
    struct screen *wlr = o->screen;
    const struct screen_state *state = pending ? &o->pending : NULL;
    int physical_width = wlr->width, physical_height = wlr->height;
    int refresh = wlr->refresh;
    if (state && (state->committed & SCREEN_MODE)) {
        if (state->mode_type == SCREEN_MODE_FIXED && state->mode) {
            physical_width = state->mode->width;
            physical_height = state->mode->height;
            refresh = state->mode->refresh;
        } else if (state->mode_type == SCREEN_MODE_CUSTOM) {
            physical_width = state->custom_mode.width;
            physical_height = state->custom_mode.height;
            refresh = state->custom_mode.refresh;
        }
    }
    enum wl_output_transform transform = (state &&
            (state->committed & SCREEN_TRANSFORM)) ?
        state->transform : wlr->transform;
    int width = physical_width, height = physical_height;
    if ((int)transform % 2 != 0) {
        width = physical_height; height = physical_width;
    }
    double scale = (state && (state->committed & SCREEN_SCALE)) ?
        state->scale : wlr->scale;
    bool enabled = state && (state->committed & SCREEN_ENABLED) ?
        state->enabled : wlr->enabled;
    if (!pending && !o->admitted) enabled = false;
    bool adaptive_sync_supported = wlr->adaptive_sync_supported;
    bool adaptive_sync = enabled && adaptive_sync_supported &&
        wlr->adaptive_sync;
    if (state && (state->committed & SCREEN_VRR))
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
                    strcmp(candidate->output->screen->name, mirror) != 0) continue;
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
    quote(out, o->screen->name);
    if (fprintf(out, " :x %d :y %d :width %d :height %d"
            " :physical-width %d :physical-height %d :refresh-mhz %d"
            " :scale-120 %d :transform %d :adaptive-sync-supported %s"
            " :adaptive-sync %s :modes (",
            facts->x, facts->y, facts->width, facts->height,
            facts->physical_width, facts->physical_height, facts->refresh,
            facts->scale_120, facts->transform,
            facts->adaptive_sync_supported ? "t" : "nil",
            facts->adaptive_sync ? "t" : "nil") < 0) return false;
    struct screen_mode *mode;
    wl_list_for_each(mode, &o->screen->modes, link) {
        if (fprintf(out, "(:width %d :height %d :refresh-mhz %d :preferred %s)",
                mode->width, mode->height, mode->refresh,
                mode->preferred ? "t" : "nil") < 0) return false;
    }
    return fputs("))", out) >= 0;
}

static bool write_output_modes(FILE *out, struct output *o) {
    if (fputs(" :modes (", out) < 0) return false;
    struct screen_mode *mode;
    wl_list_for_each(mode, &o->screen->modes, link) {
        if (fprintf(out, "(:width %d :height %d :refresh-mhz %d :preferred %s)",
                mode->width, mode->height, mode->refresh,
                mode->preferred ? "t" : "nil") < 0) return false;
    }
    return fputc(')', out) != EOF;
}

static bool write_render(FILE *out, struct output *o) {
    if (!o->ring.format.len) return fputs(" :render nil", out) >= 0;
    char *name = drmGetFormatName(o->ring.format.formats[0].format);
    bool ok = fputs(" :render (:format ", out) >= 0;
    quote(out, name ? name : "?");
    free(name);
    struct dmabuf_attributes dmabuf;
    if (o->ring.slots[0] && buffer_get_dmabuf(o->ring.slots[0], &dmabuf))
        ok = ok && fprintf(out, " :modifier %" PRIu64, dmabuf.modifier) >= 0;
    else
        ok = ok && fputs(" :modifier nil", out) >= 0;
    return ok && fprintf(out, " :implicit %s :width %d :height %d :fenced %s)",
        o->ring.implicit ? "t" : "nil", o->ring.width, o->ring.height,
        fenced(o->screen) ? "t" : "nil") >= 0;
}

static bool write_connector(FILE *out, struct output *o, bool pending) {
    struct output_facts facts;
    output_state_facts(o, pending, &facts);
    if (fprintf(out, "(:id %" PRIu64 " :name ", o->id) < 0) return false;
    quote(out, o->screen->name);
    if (fprintf(out, " :enabled %s :pending %s :adaptive-sync-supported %s"
            " :adaptive-sync %s :request-id %" PRIu64
            " :request-pending %s :power %s", facts.enabled ? "t" : "nil",
            (pending || o->admitted) ? "nil" : "t",
            facts.adaptive_sync_supported ? "t" : "nil",
            facts.adaptive_sync ? "t" : "nil", facts.request_id,
            facts.request_pending ? "t" : "nil", o->screen->power_off ? "nil" : "t") < 0)
        return false;
    return write_output_modes(out, o) && write_render(out, o) && fputc(')', out) != EOF;
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

void outputs_event(struct tomoe *s) {
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

const char *tomoe_frames(struct tomoe *s) {
    free(s->frames_result);
    s->frames_result = NULL;
    size_t size = 0;
    FILE *out = open_memstream(&s->frames_result, &size);
    if (!out) return NULL;
    fputc('(', out);
    struct output *o;
    wl_list_for_each(o, &s->outputs, link) {
        fputs("(:name ", out);
        quote(out, o->screen->name);
        fprintf(out, " :frame %" PRIu64 " :drawn-frame %" PRIu64 " :buffer-age %" PRIu64
            " :drawn-pixels %" PRId64 " :pixels %" PRId64 " :draws %zu :scanout %s)",
            o->frames, o->drawn.frame, o->drawn.age, o->drawn.pixels,
            (int64_t)o->ring.width * o->ring.height, o->drawn.ops, o->scanout ? "t" : "nil");
    }
    fputc(')', out);
    bool success = !ferror(out);
    if (fclose(out) != 0 || !success) {
        free(s->frames_result);
        s->frames_result = NULL;
    }
    return s->frames_result;
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
        entry->box = (struct box){ .x = locations[i].x, .y = locations[i].y,
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

static struct screen_mode *first_output_mode(struct screen *wlr) {
    if (!wlr || wl_list_empty(&wlr->modes)) return NULL;
    struct screen_mode *mode;
    return wl_container_of(wlr->modes.next, mode, link);
}

static void snapshot_output_state(struct screen *wlr, struct screen_state *state) {
    screen_state_init(state);
    screen_state_set_enabled(state, wlr->enabled);
    if (wlr->current_mode) screen_state_set_mode(state, wlr->current_mode);
    else screen_state_set_custom_mode(state, wlr->width, wlr->height, wlr->refresh);
    screen_state_set_scale(state, wlr->scale);
    screen_state_set_transform(state, wlr->transform);
    if (wlr->adaptive_sync_supported)
        screen_state_set_adaptive_sync_enabled(state,
            wlr->adaptive_sync);
}

static void snapshot_output_baseline(struct screen *wlr,
        struct screen_state *state) {
    screen_state_init(state);
    screen_state_set_enabled(state, true);
    struct screen_mode *mode = screen_preferred_mode(wlr);
    if (!mode) mode = first_output_mode(wlr);
    if (mode) screen_state_set_mode(state, mode);
    else screen_state_set_custom_mode(state, wlr->width, wlr->height,
        wlr->refresh);
    screen_state_set_scale(state, wlr->scale);
    screen_state_set_transform(state, wlr->transform);
    if (wlr->adaptive_sync_supported)
        screen_state_set_adaptive_sync_enabled(state,
            wlr->adaptive_sync);
}

static bool next_output_id(struct tomoe *s, uint64_t *id) {
    if (!s || !id || s->next_output_id == 0) return false;
    *id = s->next_output_id;
    if (s->next_output_id == UINT64_MAX) s->next_output_id = 0;
    else s->next_output_id++;
    return true;
}

static bool copy_output_state(struct screen_state *destination,
        const struct screen_state *source) {
    screen_state_init(destination);
    return screen_state_copy(destination, source);
}

#define OUTPUT_REQUEST_FIELDS (SCREEN_MODE | \
    SCREEN_SCALE | SCREEN_TRANSFORM | \
    SCREEN_VRR)

static bool has_output_request_fields(const struct screen_state *state) {
    return state && (state->committed & OUTPUT_REQUEST_FIELDS) != 0;
}

static bool set_output_state_fields(struct screen_state *destination,
        const struct screen_state *source, uint32_t fields) {
    if (!destination || !source) return false;
    if (fields & SCREEN_ENABLED)
        screen_state_set_enabled(destination, source->enabled);
    if (fields & SCREEN_MODE) {
        if (source->mode_type == SCREEN_MODE_FIXED) {
            if (!source->mode) return false;
            screen_state_set_mode(destination, source->mode);
        } else if (source->mode_type == SCREEN_MODE_CUSTOM) {
            screen_state_set_custom_mode(destination,
                source->custom_mode.width, source->custom_mode.height,
                source->custom_mode.refresh);
        } else {
            return false;
        }
    }
    if (fields & SCREEN_SCALE)
        screen_state_set_scale(destination, source->scale);
    if (fields & SCREEN_TRANSFORM)
        screen_state_set_transform(destination, source->transform);
    if (fields & SCREEN_VRR)
        screen_state_set_adaptive_sync_enabled(destination,
            source->adaptive_sync_enabled);
    return true;
}

static bool copy_output_request_state(struct screen_state *destination,
        const struct screen_state *source) {
    screen_state_init(destination);
    return set_output_state_fields(destination, source,
        source ? source->committed & (SCREEN_ENABLED |
            OUTPUT_REQUEST_FIELDS) : 0);
}

static bool overlay_output_request(struct screen_state *destination,
        const struct screen_state *request) {
    return set_output_state_fields(destination, request,
        request ? request->committed & OUTPUT_REQUEST_FIELDS : 0);
}

static bool next_output_request_id(struct output *o) {
    if (!o || o->request_id == UINT64_MAX) return false;
    o->request_id++;
    return true;
}

static bool store_output_request(struct output *o,
        const struct screen_state *request) {
    if (!o || !has_output_request_fields(request)) return true;

    struct screen_state deferred;
    bool copied;
    if (o->request_pending) {
        copied = copy_output_request_state(&deferred, &o->deferred);
    } else {
        copied = copy_output_request_state(&deferred, &o->initial);
    }
    if (!copied || !overlay_output_request(&deferred, request)) {
        screen_state_finish(&deferred);
        return false;
    }
    if (!next_output_request_id(o)) {
        screen_state_finish(&deferred);
        return false;
    }

    screen_state_finish(&o->deferred);
    o->deferred = deferred;
    o->request_pending = true;
    return true;
}

static void clear_output_request(struct output *o) {
    if (!o) return;
    screen_state_finish(&o->deferred);
    screen_state_init(&o->deferred);
    o->request_pending = false;
}

static void promote_output_request_snapshot(struct output *o,
        struct screen_state *baseline) {
    if (!o || !baseline) return;
    screen_state_finish(&o->initial);
    o->initial = *baseline;
    screen_state_init(baseline);
}

static void strip_disabled_state(struct screen_state *state) {
    if (!(state->committed & SCREEN_ENABLED) || state->enabled) return;
    state->committed &= ~(SCREEN_BUFFER |
        SCREEN_MODE | SCREEN_VRR | SCREEN_WAIT | SCREEN_GAMMA);
}

static bool output_state_enabled(const struct screen_update *state) {
    if (!state || !state->output) return false;
    return (state->base.committed & SCREEN_ENABLED) ?
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
        if (!locations[i].active) forget_output(s, locations[i].output->screen);
    pointer_sync_cursors(s);
    double scale = reference_scale(s);
    bool configuring = s->configuring_outputs;
    s->configuring_outputs = true;
    bool success = true;
    for (size_t i = 0; i < count; i++) {
        struct output *o = locations[i].output;
        if (!locations[i].active) continue;
        screen_set_position(o->screen, pixel_round(locations[i].x / scale),
            pixel_round(locations[i].y / scale));
    }
    s->configuring_outputs = configuring;
    free(locations);
    return success;
}

int tomoe_outputs_begin(struct tomoe *s) {
    struct output *o;
    wl_list_for_each(o, &s->outputs, link) {
        screen_state_finish(&o->pending);
        if (o->request_pending) {
            screen_state_init(&o->pending);
            if (!copy_output_state(&o->pending, &o->deferred)) {
                screen_state_finish(&o->pending);
                return 0;
            }
        } else if (!o->admitted || o->configured) {
            screen_state_init(&o->pending);
            if (!screen_state_copy(&o->pending, &o->initial)) return 0;
        } else {
            snapshot_output_state(o->screen, &o->pending);
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

static bool interlaced(struct screen *wlr, struct screen_mode *mode) {
    return mode->interlaced;
}

static struct screen_mode *pick_output_mode(struct screen *wlr,
        int kind, int width, int height, int refresh) {
    struct screen_mode *preferred = screen_preferred_mode(wlr);
    if (!preferred) preferred = first_output_mode(wlr);
    if (!preferred) return NULL;
    if (kind == 0 && refresh == 0) return preferred;
    struct screen_mode *mode, *best = NULL;
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
        if (strcmp(o->screen->name, name) != 0) continue;
        struct screen_mode *mode = pick_output_mode(o->screen, kind, width, height, refresh);
        if (mode) screen_state_set_mode(&o->pending, mode);
        else if (kind == 2) screen_state_set_custom_mode(&o->pending, width, height, refresh);
        screen_state_set_scale(&o->pending, scale / 120.0f);
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
        if (strcmp(o->screen->name, name) != 0) continue;
        screen_state_set_enabled(&o->pending, enabled != 0);
        if (o->screen->adaptive_sync_supported)
            screen_state_set_adaptive_sync_enabled(&o->pending,
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
        if (strcmp(o->screen->name, name) != 0) continue;
        screen_state_finish(&o->pending);
        if (o->admitted) {
            snapshot_output_state(o->screen, &o->pending);
        } else {
            screen_state_init(&o->pending);
            if (!screen_state_copy(&o->pending, &o->initial)) return 0;
            screen_state_set_enabled(&o->pending, false);
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

static void state_size(const struct screen_update *state, int *width, int *height) {
    *width = state->output->width;
    *height = state->output->height;
    if (!(state->base.committed & SCREEN_MODE)) return;
    bool fixed = state->base.mode_type == SCREEN_MODE_FIXED;
    *width = fixed ? state->base.mode->width : state->base.custom_mode.width;
    *height = fixed ? state->base.mode->height : state->base.custom_mode.height;
}

static bool test_rings(struct tomoe *s, struct screen_update *states, size_t count,
        struct ring *rings, bool implicit) {
    bool ok = true;
    for (size_t i = 0; ok && i < count; i++) {
        if (!output_state_enabled(&states[i])) continue;
        int width, height;
        state_size(&states[i], &width, &height);
        struct buffer *buffer = ring_configure(s, &rings[i], states[i].output, width, height,
            implicit) ? ring_acquire(s, &rings[i]) : NULL;
        if (buffer) screen_state_set_buffer(&states[i].base, buffer);
        buffer_unlock(buffer);
        ok = buffer != NULL;
    }
    ok = ok && screens_test(states, count);
    for (size_t i = 0; i < count; i++) {
        if (!(states[i].base.committed & SCREEN_BUFFER)) continue;
        buffer_unlock(states[i].base.buffer);
        states[i].base.buffer = NULL;
        states[i].base.committed &= ~SCREEN_BUFFER;
    }
    return ok;
}

static const char *commit_outputs(struct tomoe *s,
        struct screen_update *states, size_t count, bool *attempted,
        const struct presentation *plan) {
    struct ring *rings = calloc(count ? count : 1, sizeof(*rings));
    const char *error = "Output renderer initialization failed; previous settings retained.";
    size_t enabled_count = 0;
    if (!rings) goto done;
    for (size_t i = 0; i < count; i++)
        if (output_state_enabled(&states[i])) enabled_count++;
    for (size_t i = 0; i < count; i++) strip_disabled_state(&states[i].base);
    error = "Output configuration rejected by the backend; previous settings retained.";
    if (enabled_count && !test_rings(s, states, count, rings, false) &&
            !test_rings(s, states, count, rings, true)) goto done;
    error = "Cannot render the requested output configuration; previous settings retained.";
    for (size_t i = 0; i < count; i++) {
        if (!output_state_enabled(&states[i])) continue;
        struct output *o;
        wl_list_for_each(o, &s->outputs, link) {
            if (o->screen != states[i].output) continue;
            if (!render_presentation(o, &states[i].base, &rings[i], plan)) goto done;
            break;
        }
    }
    *attempted = true;
    error = "Output commit failed.";
    if (!screens_commit(states, count)) goto done;
    for (size_t i = 0; i < count; i++) {
        struct output *o;
        wl_list_for_each(o, &s->outputs, link) {
            if (o->screen != states[i].output) continue;
            ring_finish(&o->ring);
            o->ring = rings[i];
            rings[i] = (struct ring){0};
        }
    }
    error = NULL;
done:
    finish_captures(s);
    for (size_t i = 0; rings && i < count; i++) ring_finish(&rings[i]);
    free(rings);
    return error;
}

const char *tomoe_outputs_apply(struct tomoe *s) {
    size_t count = wl_list_length(&s->outputs), initialized = 0;
    if (count == 0) {
        if (s->presentation) presentation_publish(s);
        return s->failed ? "Presentation publication failed; stopping the compositor." : NULL;
    }
    struct screen_update *states = calloc(count, sizeof(*states));
    struct screen_update *previous = calloc(count, sizeof(*previous));
    uint64_t *request_ids = calloc(count, sizeof(*request_ids));
    bool *request_holds = calloc(count, sizeof(*request_holds));
    struct screen_state *request_states = calloc(count, sizeof(*request_states));
    const char *error = "Cannot allocate output configuration.";
    if (!states || !previous || !request_ids || !request_holds || !request_states) goto done;
    struct output *o;
    wl_list_for_each(o, &s->outputs, link) {
        size_t i = initialized++;
        states[i].output = previous[i].output = o->screen;
        request_ids[i] = o->request_pending ? o->request_id : 0;
        request_holds[i] = o->pending_hold;
        screen_state_init(&request_states[i]);
        screen_state_init(&states[i].base);
        snapshot_output_state(o->screen, &previous[i].base);
        if (o->request_pending &&
                !screen_state_copy(&request_states[i], &o->deferred)) goto done;
        if (!screen_state_copy(&states[i].base, &o->pending)) goto done;
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
            outputs_event(s);
            schedule_scene(s);
        }
    }
done:
    for (size_t i = 0; i < initialized; i++) {
        screen_state_finish(&states[i].base);
        screen_state_finish(&previous[i].base);
        screen_state_finish(&request_states[i]);
    }
    free(states); free(previous); free(request_ids); free(request_holds);
    free(request_states);
    return error;
}

struct screen *any_output(struct tomoe *s) {
    if (!s) return NULL;
    struct output *o;
    wl_list_for_each(o, &s->outputs, link)
        if (output_is_active(o)) return o->screen;
    return NULL;
}

static void output_frame(struct wl_listener *listener, void *data) {
    struct output *o = wl_container_of(listener, o, frame);
    if (!output_is_active(o)) return;
    struct screen_state state;
    screen_state_init(&state);
    struct surface *scanout = scanout_surface(o);
    if (scanout) {
        screen_state_set_buffer(&state, scanout->buffer);
        if (scanout->current.acquire)
            screen_state_set_wait_timeline(&state, scanout->current.acquire,
                scanout->current.acquire_point);
        if (!screen_test(o->screen, &state)) {
            screen_state_finish(&state);
            screen_state_init(&state);
            scanout = NULL;
        }
    }
    if (scanout != o->scanout)
        tomoe_log(LOG_DEBUG, "tomoe: output %s direct scanout %s", o->screen->name,
            scanout ? "engaged" : "disengaged");
    o->scanout = scanout;
    if (scanout) {
        refresh_scene(o->server);
        pointer_sync_cursors(o->server);
    }
    bool success = scanout || render_output(o, &state);
    if (success && scanout)
        surface_presented(scanout, o->screen, true);
    else if (success) surfaces_textured(o);
    if (success && o->gamma_dirty) gamma_apply(o, &state);
    if (success && scanout && windows_want_tearing(o->server, o)) {
        state.tearing_page_flip = true;
        if (!screen_test(o->screen, &state)) state.tearing_page_flip = false;
    }
    success = success && screen_commit(o->screen, &state);
    if (success && scanout) surface_release_after(scanout, state.buffer);
    if (success && !scanout && (state.committed & SCREEN_BUFFER)) {
        buffer_unlock(o->presented[1]);
        o->presented[1] = o->presented[0];
        o->presented[0] = buffer_lock(state.buffer);
    }
    if (success && (state.committed & SCREEN_BUFFER))
        capture_serve(o, state.buffer, scanout != NULL);
    finish_output_capture(o);
    screen_state_finish(&state);
    if (!success) {
        tomoe_log(LOG_ERROR, "tomoe: output %s commit failed", o->screen->name);
        return;
    }
    lock_frame_rendered(o->server, o->screen);
    if (windows_animate(o->server)) screen_schedule_frame(o->screen);
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    frame_done(o, &now);
}
static void output_needs_frame(struct wl_listener *listener, void *data) {
    struct output *o = wl_container_of(listener, o, needs_frame);
    if (output_is_active(o)) screen_schedule_frame(o->screen);
}
void outputs_request_nested_size(struct tomoe *s) {
    struct output *o;
    wl_list_for_each(o, &s->outputs, link) {
        if (o->screen->kind != SCREEN_NESTED) continue;
        struct screen_state state;
        screen_state_init(&state);
        screen_state_set_custom_mode(&state, s->settings.nested_width,
            s->settings.nested_height, 0);
        if (!store_output_request(o, &state)) fail(s, "output request could not be retained");
        screen_state_finish(&state);
    }
    outputs_event(s);
}
static void output_request(struct wl_listener *listener, void *data) {
    struct output *o = wl_container_of(listener, o, request);
    const struct screen_state *event = data;
    if (!store_output_request(o, event)) {
        fail(o->server, "output request could not be retained");
    } else if (has_output_request_fields(event)) {
        outputs_event(o->server);
    }
}
static void output_destroy(struct wl_listener *listener, void *data) {
    struct output *o = wl_container_of(listener, o, destroy);
    struct tomoe *s = o->server;
    struct screen *wlr = o->screen;
    finish_output_capture(o);
    buffer_unlock(o->presented[0]);
    buffer_unlock(o->presented[1]);
    ring_finish(&o->ring);
    oplist_finish(&o->ops);
    for (size_t i = 0; i < sizeof(o->damage) / sizeof(o->damage[0]); i++)
        pixman_region32_fini(&o->damage[i]);
    screenshot_output_gone(s, o);
    capture_output_gone(s, o);
    gamma_output_gone(o);
    power_output_gone(o);
    ui_output_finish(s, wlr->name);
    detach(&o->frame); detach(&o->request); detach(&o->destroy); detach(&o->needs_frame);
    forget_output(s, wlr);
    screen_state_finish(&o->initial);
    screen_state_finish(&o->pending);
    screen_state_finish(&o->deferred);
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
void output_added(struct tomoe *s, struct screen *wlr) {
    uint64_t id;
    if (!next_output_id(s, &id)) {
        fail(s, "output identifiers exhausted"); return;
    }
    struct output *o = calloc(1, sizeof(*o));
    if (!o) { fail(s, "output allocation failed"); return; }
    o->server = s; o->screen = wlr; o->id = id;
    o->admitted = false;
    for (size_t i = 0; i < sizeof(o->damage) / sizeof(o->damage[0]); i++)
        pixman_region32_init(&o->damage[i]);
    snapshot_output_baseline(wlr, &o->initial);
    screen_state_init(&o->deferred);
    if (!copy_output_state(&o->pending, &o->initial)) {
        screen_state_finish(&o->initial);
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
