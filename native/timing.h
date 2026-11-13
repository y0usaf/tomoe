#ifndef TOMOE_TIMING_H
#define TOMOE_TIMING_H
#include <stdbool.h>
#include <stdint.h>

enum timing_channel {
    TIMING_POINTER,
    TIMING_DRAG,
    TIMING_KEY,
    TIMING_POLICY,
    TIMING_FRAME_WAIT,
    TIMING_FRAME_SCENE,
    TIMING_FRAME_DRAW,
    TIMING_FRAME_RASTER,
    TIMING_FRAME_SUBMIT,
    TIMING_FRAME_TOTAL,
    TIMING_COMMIT_FRAME,
    TIMING_TX_MOUNTS,
    TIMING_TX_MATERIAL,
    TIMING_TX_INVOKE,
    TIMING_TX_DIFF,
    TIMING_TX_COMMIT,
    TIMING_MAT_INIT,
    TIMING_MAT_QUANTIZE,
    TIMING_MAT_REPLAY,
    TIMING_MAT_OUTPUT,
    TIMING_MAT_LAYERS,
    TIMING_MAT_SURFACES,
    TIMING_MAT_BUILD,
    TIMING_TX_DIRTY,
    TIMING_TX_BASELINES,
    TIMING_TX_RECONCILE,
    TIMING_PARSE,
    TIMING_TRANSACTION,
    TIMING_LATCH_INPUT,
    TIMING_LATCH_COMMIT,
    TIMING_PRESENT_INPUT,
    TIMING_PRESENT_COMMIT,
    TIMING_CHANNELS
};

struct tomoe;
struct screen;

extern bool timing_enabled;

void timing_init(void);
uint64_t timing_now(void);
void timing_add(enum timing_channel channel, uint64_t ns);
void timing_input(struct tomoe *s, enum timing_channel channel);
void timing_event_taken(struct tomoe *s);
void timing_event_done(struct tomoe *s);
void timing_commit(struct tomoe *s);
void timing_frame_start(struct tomoe *s);
void timing_frame_latch(struct screen *screen);
void timing_screen_present(struct screen *screen, int64_t when_ns);
void timing_frame_scene(struct tomoe *s, uint64_t ns);
void timing_frame_draw(struct tomoe *s, uint64_t ns);
void timing_frame_raster(struct tomoe *s, uint64_t ns);
void timing_frame_submit(struct tomoe *s, uint64_t ns);
void timing_frame_end(struct tomoe *s, bool committed);
const char *tomoe_timings(void);
int tomoe_timing_enabled(void);
void tomoe_timing_done(struct tomoe *s);
uint64_t tomoe_timing_now(void);
int tomoe_timing_channel(const char *name);
void tomoe_timing_reset(void);
void tomoe_timing_sample(int channel, uint64_t ns);

#endif
