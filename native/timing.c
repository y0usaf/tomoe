#include "internal.h"
#include "timing.h"

#define TIMING_SAMPLES 8192

struct channel {
    uint32_t samples[TIMING_SAMPLES];
    size_t recorded;
    size_t cursor;
};

static const char *const channel_names[TIMING_CHANNELS] = {
    "pointer", "drag", "key", "policy",
    "frame-wait", "frame-scene", "frame-draw", "frame-raster", "frame-submit", "frame-total",
    "commit-frame", "tx-mounts", "tx-material", "tx-invoke", "tx-diff", "tx-commit",
    "mat-init", "mat-quantize", "mat-replay", "mat-output", "mat-layers", "mat-surfaces", "mat-build",
    "tx-dirty", "tx-baselines", "tx-reconcile", "parse", "transaction",
    "latch-input", "latch-commit", "present-input", "present-commit",
};

static struct channel channels[TIMING_CHANNELS];
bool timing_enabled;

void timing_init(void) {
    const char *value = getenv("TOMOE_TIMINGS");
    timing_enabled = value && *value && strcmp(value, "0") != 0;
}

int tomoe_timing_enabled(void) {
    return timing_enabled ? 1 : 0;
}

void tomoe_timing_done(struct tomoe *s) {
    timing_event_done(s);
}

uint64_t tomoe_timing_now(void) {
    return timing_enabled ? timing_now() : 0;
}

void tomoe_timing_reset(void) {
    memset(channels, 0, sizeof(channels));
}

int tomoe_timing_channel(const char *name) {
    for (int i = 0; i < TIMING_CHANNELS; i++)
        if (!strcmp(channel_names[i], name)) return i;
    return -1;
}

void tomoe_timing_sample(int channel, uint64_t ns) {
    if (channel >= 0 && channel < TIMING_CHANNELS) timing_add((enum timing_channel)channel, ns);
}

uint64_t timing_now(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000000000ULL + (uint64_t)now.tv_nsec;
}

void timing_add(enum timing_channel channel, uint64_t ns) {
    if (!timing_enabled || channel >= TIMING_CHANNELS || ns > UINT32_MAX) return;
    struct channel *c = &channels[channel];
    c->samples[c->cursor] = (uint32_t)ns;
    if (++c->cursor == TIMING_SAMPLES) c->cursor = 0;
    c->recorded++;
}

void timing_input(struct tomoe *s, enum timing_channel channel) {
    if (!timing_enabled) return;
    uint64_t now = timing_now();
    s->timing.input_ring[s->timing.input_total++ % TOMOE_PENDING_INPUT] = now;
    if (!s->timing.input_at) s->timing.input_at = now;
    if (channel == TIMING_DRAG) { s->timing.drag_at = now; s->timing.drag = true; }
    else if (channel == TIMING_KEY) { s->timing.key_at = now; s->timing.key = true; }
    else if (!s->timing.pointer) { s->timing.pointer_at = now; s->timing.pointer = true; }
}

void timing_event_taken(struct tomoe *s) {
    if (timing_enabled) s->timing.event_at = timing_now();
}

void timing_event_done(struct tomoe *s) {
    if (!timing_enabled) return;
    uint64_t now = timing_now();
    if (s->timing.event_at) timing_add(TIMING_POLICY, now - s->timing.event_at);
    s->timing.event_at = 0;
    if (s->timing.key) {
        timing_add(TIMING_KEY, now - s->timing.key_at);
        s->timing.key = false;
    }
}

void timing_commit(struct tomoe *s) {
    if (!timing_enabled) return;
    s->timing.commit_at = timing_now();
    s->timing.commit_ring[s->timing.commit_total++ % TOMOE_PENDING_INPUT] = s->timing.commit_at;
    s->timing.commit = true;
}

void timing_frame_start(struct tomoe *s) {
    if (!timing_enabled) return;
    s->timing.frame_at = timing_now();
    if ((s->timing.pointer || s->timing.drag) && s->timing.input_at &&
            s->timing.frame_at > s->timing.input_at)
        timing_add(TIMING_FRAME_WAIT, s->timing.frame_at - s->timing.input_at);
    s->timing.input_at = 0;
}

static void latch_ring(uint64_t *ring, uint64_t *seen, uint64_t total, uint64_t *pending,
        size_t *count) {
    *count = 0;
    uint64_t from = *seen > total - TOMOE_PENDING_INPUT ? *seen : total - TOMOE_PENDING_INPUT;
    for (uint64_t index = from; index < total && *count < TOMOE_PENDING_INPUT; index++)
        pending[(*count)++] = ring[index % TOMOE_PENDING_INPUT];
    *seen = total;
}

void timing_frame_latch(struct screen *screen) {
    if (!timing_enabled) return;
    struct tomoe *s = screen->server;
    uint64_t now = timing_now();
    latch_ring(s->timing.input_ring, &screen->input_seen, s->timing.input_total,
        screen->pending_input, &screen->pending_input_count);
    latch_ring(s->timing.commit_ring, &screen->commit_seen, s->timing.commit_total,
        screen->pending_commit, &screen->pending_commit_count);
    if (screen->pending_input_count) {
        uint64_t newest = screen->pending_input[screen->pending_input_count - 1];
        if (now > newest) timing_add(TIMING_LATCH_INPUT, now - newest);
    }
    if (screen->pending_commit_count) {
        uint64_t newest = screen->pending_commit[screen->pending_commit_count - 1];
        if (now > newest) timing_add(TIMING_LATCH_COMMIT, now - newest);
    }
}

void timing_screen_present(struct screen *screen, int64_t when_ns) {
    if (!timing_enabled) return;
    for (size_t i = 0; i < screen->pending_input_count; i++)
        if (when_ns > (int64_t)screen->pending_input[i])
            timing_add(TIMING_PRESENT_INPUT, (uint64_t)when_ns - screen->pending_input[i]);
    for (size_t i = 0; i < screen->pending_commit_count; i++)
        if (when_ns > (int64_t)screen->pending_commit[i])
            timing_add(TIMING_PRESENT_COMMIT, (uint64_t)when_ns - screen->pending_commit[i]);
    screen->pending_input_count = screen->pending_commit_count = 0;
}

void timing_frame_scene(struct tomoe *s, uint64_t ns) {
    if (timing_enabled) s->timing.scene_ns += ns;
}

void timing_frame_draw(struct tomoe *s, uint64_t ns) {
    if (timing_enabled) s->timing.draw_ns += ns;
}

void timing_frame_raster(struct tomoe *s, uint64_t ns) {
    if (timing_enabled) s->timing.raster_ns += ns;
}

void timing_frame_submit(struct tomoe *s, uint64_t ns) {
    if (timing_enabled) s->timing.submit_ns += ns;
}

void timing_frame_end(struct tomoe *s, bool committed) {
    if (!timing_enabled) return;
    uint64_t now = timing_now();
    if (s->timing.frame_at) {
        timing_add(TIMING_FRAME_TOTAL, now - s->timing.frame_at);
        timing_add(TIMING_FRAME_SCENE, s->timing.scene_ns);
        timing_add(TIMING_FRAME_DRAW, s->timing.draw_ns);
        timing_add(TIMING_FRAME_RASTER, s->timing.raster_ns);
        timing_add(TIMING_FRAME_SUBMIT, s->timing.submit_ns);
    }
    s->timing.frame_at = 0;
    s->timing.scene_ns = s->timing.draw_ns = s->timing.raster_ns = s->timing.submit_ns = 0;
    if (!committed) return;
    if (s->timing.pointer) {
        timing_add(TIMING_POINTER, now - s->timing.pointer_at);
        s->timing.pointer = false;
    }
    if (s->timing.drag) {
        timing_add(TIMING_DRAG, now - s->timing.drag_at);
        s->timing.drag = false;
    }
    if (s->timing.commit) {
        timing_add(TIMING_COMMIT_FRAME, now - s->timing.commit_at);
        s->timing.commit = false;
    }
}

static int compare(const void *first, const void *second) {
    uint32_t a = *(const uint32_t *)first, b = *(const uint32_t *)second;
    return a < b ? -1 : a > b;
}

static void write_channel(FILE *out, enum timing_channel channel, uint32_t *sorted) {
    struct channel *c = &channels[channel];
    size_t count = c->recorded < TIMING_SAMPLES ? c->recorded : TIMING_SAMPLES;
    fputs("(:name \"", out);
    fputs(channel_names[channel], out);
    fprintf(out, "\" :count %zu", c->recorded);
    if (!count) {
        fputs(")\n", out);
        return;
    }
    memcpy(sorted, c->samples, count * sizeof(*sorted));
    qsort(sorted, count, sizeof(*sorted), compare);
    uint64_t total = 0;
    for (size_t i = 0; i < count; i++) total += sorted[i];
    fprintf(out, " :mean %" PRIu64 " :p50 %u :p90 %u :p99 %u :max %u)\n",
        total / count, sorted[(count - 1) * 50 / 100], sorted[(count - 1) * 90 / 100],
        sorted[(count - 1) * 99 / 100], sorted[count - 1]);
}

static const char *timing_report(void) {
    static char *result;
    static uint32_t sorted[TIMING_SAMPLES];
    free(result);
    result = NULL;
    size_t size = 0;
    FILE *out = open_memstream(&result, &size);
    if (!out) return NULL;
    fprintf(out, "(:enabled %s :window %d :channels (\n",
        timing_enabled ? "t" : "nil", TIMING_SAMPLES);
    for (enum timing_channel channel = 0; channel < TIMING_CHANNELS; channel++)
        write_channel(out, channel, sorted);
    fputs("))\n", out);
    bool success = !ferror(out);
    if (fclose(out) != 0 || !success) {
        free(result);
        result = NULL;
    }
    return result;
}

const char *tomoe_timings(void) {
    return timing_report();
}
