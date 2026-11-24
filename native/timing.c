#include "internal.h"
#include "timing.h"
#include "upload.h"

#define TIMING_SAMPLES 8192
#define TIMING_STALLS 256
#define TIMING_WORST 8

_Static_assert(STAGE_COUNT == 16, "stage channels");

struct channel {
    uint32_t samples[TIMING_SAMPLES];
    size_t recorded;
    size_t cursor;
};

struct stall {
    uint64_t start;
    uint64_t duration;
};

static const char *const channel_names[TIMING_CHANNELS] = {
    "pointer", "drag", "key", "policy",
    "frame-wait", "frame-scene", "frame-draw", "frame-raster", "frame-submit", "frame-total",
    "commit-frame", "tx-mounts", "tx-material", "tx-invoke", "tx-diff", "tx-commit",
    "mat-init", "mat-quantize", "mat-replay", "mat-output", "mat-layers", "mat-surfaces", "mat-build",
    "tx-dirty", "tx-reconcile", "parse", "transaction",
    "mat-total", "mat-config",
    "latch-input", "latch-commit", "present-input", "present-commit", "present-interval",
    "frame-tail",
    "frame-wait-render",
    "cursor-newest", "cursor-oldest", "loop-tick", "loop-tick-cpu", "loop-tick-frame",
    "loop-tick-idle", "loop-tick-commit", "loop-tick-noncommit", "loop-tick-noncommit-frame",
    "loop-tick-noncommit-idle", "loop-tick-commit-frame", "loop-tick-commit-idle", "commit-request", "upload",
    "upload-bytes", "upload-latch", "stage-dispatch", "stage-step-post", "stage-gestures", "stage-policy",
    "stage-bus", "stage-log", "stage-control", "stage-ipc", "stage-lisp-end", "stage-retire",
    "stage-outputs", "stage-scene", "stage-frame", "stage-flush", "stage-idle-frames",
    "stage-idle-dispatch",
};

static struct channel *channels;

static struct channel *channels_ready(void) {
    if (!channels) channels = calloc(TIMING_CHANNELS, sizeof(*channels));
    return channels;
}
static struct stall stalls[TIMING_STALLS];
static size_t stall_recorded;
static size_t stall_cursor;
static uint64_t stall_threshold = 4170000;

struct turn {
    uint64_t total, cpu;
    bool frame;
    uint64_t commits, uploads, upload_bytes;
    uint64_t stages[STAGE_COUNT];
};

static struct turn current_turn, worst_turns[TIMING_WORST], worst_noncommit[TIMING_WORST];
static size_t worst_count, worst_noncommit_count;
static bool turn_active;
static uint64_t turn_start, turn_start_cpu, turn_mark;
static size_t turn_frames;
bool timing_enabled;

void timing_init(void) {
    timing_enabled = tomoe_test_flag("timings");
    const char *threshold = tomoe_test_flag_value("stall-ns");
    if (threshold && *threshold) {
        char *end = NULL;
        unsigned long long parsed = strtoull(threshold, &end, 10);
        if (end && !*end) stall_threshold = parsed;
    }
}

int tomoe_timing_enabled(void) {
    return timing_enabled ? 1 : 0;
}

uint64_t tomoe_timing_now(void) {
    return timing_enabled ? timing_now() : 0;
}

void tomoe_timing_reset(void) {
    if (channels) memset(channels, 0, TIMING_CHANNELS * sizeof(*channels));
    memset(stalls, 0, sizeof(stalls));
    stall_recorded = 0;
    stall_cursor = 0;
    worst_count = 0;
    worst_noncommit_count = 0;
    turn_active = false;
}

void timing_turn_mark(enum timing_stage stage) {
    if (!timing_enabled || !turn_active) return;
    uint64_t now = timing_now();
    current_turn.stages[stage] += now - turn_mark;
    turn_mark = now;
}

static void keep_worst(struct turn *list, size_t *count, const struct turn *t) {
    size_t slot = *count;
    if (*count < TIMING_WORST) {
        (*count)++;
    } else {
        slot = 0;
        for (size_t i = 1; i < TIMING_WORST; i++)
            if (list[i].total < list[slot].total) slot = i;
        if (list[slot].total >= t->total) return;
    }
    list[slot] = *t;
}

void timing_commit_request(uint64_t ns) {
    if (!timing_enabled) return;
    timing_add(TIMING_COMMIT_REQUEST, ns);
    current_turn.commits++;
}

void timing_upload(uint64_t ns, uint64_t bytes) {
    if (!timing_enabled) return;
    timing_add(TIMING_UPLOAD, ns);
    timing_add(TIMING_UPLOAD_BYTES, bytes);
    current_turn.uploads++;
    current_turn.upload_bytes += bytes;
}

void timing_turn_boundary(void *data, uint64_t sleep_ns, uint64_t wake_ns, uint64_t sleep_cpu_ns,
        uint64_t wake_cpu_ns) {
    (void)data;
    if (!timing_enabled) return;
    if (turn_active && sleep_ns >= turn_mark && sleep_ns >= turn_start) {
        current_turn.stages[STAGE_IDLE_DISPATCH] += sleep_ns - turn_mark;
        current_turn.total = sleep_ns - turn_start;
        current_turn.cpu = sleep_cpu_ns >= turn_start_cpu ? sleep_cpu_ns - turn_start_cpu : 0;
        struct channel *ch = channels_ready();
        current_turn.frame = ch && ch[TIMING_FRAME_TOTAL].recorded != turn_frames;
        timing_add(TIMING_LOOP_TICK, current_turn.total);
        timing_add(TIMING_LOOP_TICK_CPU, current_turn.cpu);
        timing_add(current_turn.frame ? TIMING_LOOP_TICK_FRAME : TIMING_LOOP_TICK_IDLE,
            current_turn.total);
        bool commit = current_turn.commits || current_turn.uploads;
        timing_add(commit ? TIMING_LOOP_TICK_COMMIT : TIMING_LOOP_TICK_NONCOMMIT, current_turn.total);
        if (!commit) {
            timing_add(current_turn.frame ? TIMING_LOOP_TICK_NONCOMMIT_FRAME :
                TIMING_LOOP_TICK_NONCOMMIT_IDLE, current_turn.total);
            keep_worst(worst_noncommit, &worst_noncommit_count, &current_turn);
        } else {
            timing_add(current_turn.frame ? TIMING_LOOP_TICK_COMMIT_FRAME :
                TIMING_LOOP_TICK_COMMIT_IDLE, current_turn.total);
        }
        for (int i = 0; i < STAGE_COUNT; i++)
            timing_add((enum timing_channel)(TIMING_STAGE_FIRST + i), current_turn.stages[i]);
        keep_worst(worst_turns, &worst_count, &current_turn);
    }
    memset(&current_turn, 0, sizeof(current_turn));
    turn_active = true;
    turn_start = turn_mark = wake_ns;
    turn_start_cpu = wake_cpu_ns;
    struct channel *ch = channels_ready();
    turn_frames = ch ? ch[TIMING_FRAME_TOTAL].recorded : 0;
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
    struct channel *ch = channels_ready();
    if (!ch) return;
    struct channel *c = &ch[channel];
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
    s->timing.pointer_drawn = s->timing.pointer ? s->timing.pointer_at : 0;
    s->timing.drag_drawn = s->timing.drag ? s->timing.drag_at : 0;
    if ((s->timing.pointer || s->timing.drag) && s->timing.input_at &&
            s->timing.frame_at > s->timing.input_at)
        timing_add(TIMING_FRAME_WAIT, s->timing.frame_at - s->timing.input_at);
    s->timing.input_at = 0;
}

static void latch_ring(uint64_t *ring, uint64_t *seen, uint64_t total, uint64_t *pending,
        size_t *count) {
    if (*seen >= total) return;
    uint64_t oldest = total > TOMOE_PENDING_INPUT ? total - TOMOE_PENDING_INPUT : 0;
    uint64_t from = *seen > oldest ? *seen : oldest;
    size_t used = *count;
    for (uint64_t index = from; index < total && used < TOMOE_PENDING_INPUT; index++)
        pending[used++] = ring[index % TOMOE_PENDING_INPUT];
    *count = used;
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
        uint64_t total = now - s->timing.frame_at;
        timing_add(TIMING_FRAME_TOTAL, total);
        timing_add(TIMING_FRAME_SCENE, s->timing.scene_ns);
        timing_add(TIMING_FRAME_DRAW, s->timing.draw_ns);
        timing_add(TIMING_FRAME_RASTER, s->timing.raster_ns);
        timing_add(TIMING_FRAME_SUBMIT, s->timing.submit_ns);
        if (total >= stall_threshold) {
            stalls[stall_cursor] = (struct stall){ s->timing.frame_at, total };
            if (++stall_cursor == TIMING_STALLS) stall_cursor = 0;
            stall_recorded++;
        }
    }
    s->timing.frame_at = 0;
    s->timing.scene_ns = s->timing.draw_ns = s->timing.raster_ns = s->timing.submit_ns = 0;
    if (!committed) return;
    if (s->timing.pointer_drawn) {
        timing_add(TIMING_POINTER, now - s->timing.pointer_drawn);
        s->timing.pointer = false;
    }
    if (s->timing.drag_drawn) {
        timing_add(TIMING_DRAG, now - s->timing.drag_drawn);
        s->timing.drag = s->timing.drag_at != s->timing.drag_drawn;
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
    static const struct channel empty;
    const struct channel *c = channels ? &channels[channel] : &empty;
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
    fprintf(out, " :mean %" PRIu64 " :p50 %u :p90 %u :p99 %u :max %u :p999 %u)\n",
        total / count, sorted[(count - 1) * 50 / 100], sorted[(count - 1) * 90 / 100],
        sorted[(count - 1) * 99 / 100], sorted[count - 1], sorted[(count - 1) * 999 / 1000]);
}

static void write_turns(FILE *out, const struct turn *list, size_t count) {
    for (size_t i = 0; i < count; i++) {
        const struct turn *t = &list[i];
        fprintf(out, "%s(:total %" PRIu64 " :cpu %" PRIu64 " :frame %s :commits %" PRIu64
            " :uploads %" PRIu64 " :upload-bytes %" PRIu64, i ? " " : "", t->total, t->cpu,
            t->frame ? "t" : "nil", t->commits, t->uploads, t->upload_bytes);
        for (int k = 0; k < STAGE_COUNT; k++)
            if (t->stages[k])
                fprintf(out, " :%s %" PRIu64, channel_names[TIMING_STAGE_FIRST + k] + 6,
                    t->stages[k]);
        fputc(')', out);
    }
}

static const char *timing_report(void) {
    static char *result;
    static uint32_t sorted[TIMING_SAMPLES];
    free(result);
    result = NULL;
    size_t size = 0;
    FILE *out = open_memstream(&result, &size);
    if (!out) return NULL;
    fprintf(out, "(:enabled %s :window %d :threshold %" PRIu64 " :channels (\n",
        timing_enabled ? "t" : "nil", TIMING_SAMPLES, stall_threshold);
    for (enum timing_channel channel = 0; channel < TIMING_CHANNELS; channel++)
        write_channel(out, channel, sorted);
    fputs(") :worst-turns (", out);
    write_turns(out, worst_turns, worst_count);
    fputs(") :worst-noncommit-turns (", out);
    write_turns(out, worst_noncommit, worst_noncommit_count);
    fputs(") :stalls (", out);
    size_t count = stall_recorded < TIMING_STALLS ? stall_recorded : TIMING_STALLS;
    size_t first = stall_recorded < TIMING_STALLS ? 0 : stall_cursor;
    for (size_t i = 0; i < count; i++) {
        struct stall *stall = &stalls[(first + i) % TIMING_STALLS];
        fprintf(out, "%s(%" PRIu64 " %" PRIu64 ")", i ? " " : "", stall->start, stall->duration);
    }
    fputs(") :upload ", out);
    upload_describe(out);
    fputs(")\n", out);
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
