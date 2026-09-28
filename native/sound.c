#include "internal.h"

#include <errno.h>
#include <math.h>
#include <pipewire/pipewire.h>
#include <signal.h>
#include <spa/param/audio/format-utils.h>
#include <spa/utils/result.h>
#include <stdatomic.h>

#define SOUND_RATE 48000u
#define SOUND_FILE_LIMIT (16u * 1024u * 1024u)
#define SOUND_TRIGGERS 64u
#define SOUND_VOICES 16

enum sound_state { SOUND_OFF, SOUND_CONNECTING, SOUND_STREAMING, SOUND_RETRYING, SOUND_FAILED };

struct sound_sample {
    float *frames;
    uint32_t count;
};
struct sound_cue {
    uint32_t first, count;
    float gain;
};
struct sound_bank {
    uint64_t generation;
    struct sound_cue cues[SOUND_EVENTS];
    struct sound_sample *samples;
    uint32_t sample_count;
    struct sound_bank *next;
};
struct sound_trigger {
    uint64_t generation;
    uint32_t sample;
    float gain;
};
struct sound_voice {
    const struct sound_sample *sample;
    uint32_t position;
    float gain;
};
struct sound {
    struct pw_thread_loop *loop;
    struct pw_context *context;
    struct pw_core *core;
    struct pw_stream *stream;
    struct spa_hook core_listener, stream_listener;
    struct spa_source *retry;
    struct sound_bank *bank, *playing;
    _Atomic(struct sound_bank *) pending, retired;
    uint64_t generation;
    uint32_t cursor[SOUND_EVENTS];
    struct sound_trigger triggers[SOUND_TRIGGERS];
    _Atomic uint32_t head, tail;
    struct sound_voice voices[SOUND_VOICES];
    _Atomic int state, error;
    _Atomic uint64_t played, dropped;
    bool reported;
};

static char sound_text[256];

static uint16_t le16(const unsigned char *p) {
    return (uint16_t)(p[0] | p[1] << 8);
}

static uint32_t le32(const unsigned char *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static const char *sound_decode(const unsigned char *bytes, size_t size, struct sound_sample *sample) {
    if (size < 12 || memcmp(bytes, "RIFF", 4) || memcmp(bytes + 8, "WAVE", 4)) return "not a WAV file";
    const unsigned char *format = NULL, *data = NULL;
    uint32_t format_size = 0, data_size = 0;
    for (size_t at = 12; at <= size && size - at >= 8;) {
        uint32_t length = le32(bytes + at + 4);
        if (length > size - at - 8) return "truncated WAV chunk";
        if (!memcmp(bytes + at, "fmt ", 4)) {
            format = bytes + at + 8;
            format_size = length;
        } else if (!memcmp(bytes + at, "data", 4)) {
            data = bytes + at + 8;
            data_size = length;
        }
        at += 8 + (size_t)length + (length & 1);
    }
    if (!format || format_size < 16 || !data) return "WAV lacks a format or data chunk";
    uint16_t tag = le16(format), channels = le16(format + 2), bits = le16(format + 14);
    uint32_t rate = le32(format + 4);
    if (tag == 0xfffe && format_size >= 26) tag = le16(format + 24);
    if (!((tag == 1 && bits == 16) || (tag == 3 && bits == 32)))
        return "WAV needs 16-bit PCM or 32-bit float samples";
    if (channels != 1 && channels != 2) return "WAV needs one or two channels";
    if (rate != SOUND_RATE) {
        snprintf(sound_text, sizeof(sound_text), "WAV runs at %u Hz, not %u Hz", rate, SOUND_RATE);
        return sound_text;
    }
    size_t width = (size_t)bits / 8 * channels, count = data_size / width;
    if (!count) return "WAV holds no samples";
    float *frames = malloc(count * 2 * sizeof(*frames));
    if (!frames) return "out of memory";
    for (size_t i = 0; i < count; i++)
        for (size_t c = 0; c < 2; c++) {
            const unsigned char *p = data + i * width + (channels == 2 ? c * bits / 8 : 0);
            float value;
            if (bits == 16) {
                value = (float)(int16_t)le16(p) / 32768.0f;
            } else {
                uint32_t raw = le32(p);
                memcpy(&value, &raw, sizeof(value));
            }
            frames[i * 2 + c] = isfinite(value) ? value : 0.0f;
        }
    sample->frames = frames;
    sample->count = (uint32_t)count;
    return NULL;
}

static const char *sound_load(const char *path, struct sound_sample *sample) {
    FILE *file = fopen(path, "rbe");
    if (!file) return strerror(errno);
    unsigned char *bytes = malloc(SOUND_FILE_LIMIT + 1);
    size_t size = bytes ? fread(bytes, 1, SOUND_FILE_LIMIT + 1, file) : 0;
    bool failed = !bytes || ferror(file);
    fclose(file);
    const char *error = failed ? "cannot read the file" :
        size > SOUND_FILE_LIMIT ? "file exceeds 16 MiB" : sound_decode(bytes, size, sample);
    free(bytes);
    return error;
}

void sound_bank_free(struct sound_bank *bank) {
    if (!bank) return;
    for (uint32_t i = 0; i < bank->sample_count; i++) free(bank->samples[i].frames);
    free(bank->samples);
    free(bank);
}

static void sound_collect(struct sound *sound) {
    struct sound_bank *bank = atomic_exchange(&sound->retired, NULL);
    while (bank) {
        struct sound_bank *next = bank->next;
        sound_bank_free(bank);
        bank = next;
    }
}

int tomoe_present_sounds(struct tomoe *s) {
    struct presentation *plan = s->presentation;
    if (!plan) return 0;
    sound_bank_free(plan->sounds);
    plan->sounds = calloc(1, sizeof(*plan->sounds));
    return plan->sounds != NULL;
}

const char *tomoe_present_sound(struct tomoe *s, int event, const char *path, double gain) {
    struct sound_bank *bank = s->presentation ? s->presentation->sounds : NULL;
    if (!bank || event < 0 || event >= SOUND_EVENTS || !path || !isfinite(gain)) return "invalid sound";
    struct sound_cue *cue = &bank->cues[event];
    if (cue->count && cue->first + cue->count != bank->sample_count)
        return "sounds must be staged event by event";
    struct sound_sample sample;
    const char *error = sound_load(path, &sample);
    if (error) return error;
    struct sound_sample *samples = realloc(bank->samples, (bank->sample_count + 1) * sizeof(*samples));
    if (!samples) {
        free(sample.frames);
        return "out of memory";
    }
    bank->samples = samples;
    if (!cue->count) cue->first = bank->sample_count;
    samples[bank->sample_count++] = sample;
    cue->count++;
    cue->gain = (float)pow(10.0, gain / 20.0);
    return NULL;
}

static void sound_report(struct sound *sound, int state, int error) {
    atomic_store(&sound->error, error);
    atomic_store(&sound->state, state);
    if (sound->reported) return;
    sound->reported = true;
    fprintf(stderr, "tomoe: sound playback unavailable: %s\n", spa_strerror(error));
}

static void sound_fault(struct sound *sound, int error) {
    sound_report(sound, SOUND_RETRYING, error);
    pw_loop_update_timer(pw_thread_loop_get_loop(sound->loop), sound->retry,
        &(struct timespec){ 1, 0 }, NULL, false);
}

static void sound_core_error(void *data, uint32_t id, int seq, int res, const char *message) {
    if (id == PW_ID_CORE) sound_fault(data, res);
}

static void sound_stream_state(void *data, enum pw_stream_state old,
        enum pw_stream_state state, const char *error) {
    struct sound *sound = data;
    if (state == PW_STREAM_STATE_STREAMING) {
        atomic_store(&sound->error, 0);
        atomic_store(&sound->state, SOUND_STREAMING);
        sound->reported = false;
    } else if (state == PW_STREAM_STATE_ERROR) {
        sound_fault(sound, -EIO);
    } else if (atomic_load(&sound->state) == SOUND_STREAMING) {
        atomic_store(&sound->state, SOUND_CONNECTING);
    }
}

static void sound_process(void *data) {
    struct sound *sound = data;
    struct pw_buffer *buffer = pw_stream_dequeue_buffer(sound->stream);
    if (!buffer) return;
    struct sound_bank *next = atomic_exchange(&sound->pending, NULL);
    if (next) {
        memset(sound->voices, 0, sizeof(sound->voices));
        struct sound_bank *old = sound->playing;
        if (old) {
            old->next = atomic_load(&sound->retired);
            while (!atomic_compare_exchange_weak(&sound->retired, &old->next, old)) { }
        }
        sound->playing = next;
    }
    struct sound_bank *bank = sound->playing;
    uint32_t head = atomic_load_explicit(&sound->head, memory_order_acquire);
    uint32_t tail = atomic_load_explicit(&sound->tail, memory_order_relaxed);
    for (; tail != head; tail++) {
        const struct sound_trigger *trigger = &sound->triggers[tail % SOUND_TRIGGERS];
        if (!bank || trigger->generation != bank->generation || trigger->sample >= bank->sample_count)
            continue;
        struct sound_voice *voice = &sound->voices[0];
        for (size_t i = 0; i < SOUND_VOICES; i++) {
            if (!sound->voices[i].sample) {
                voice = &sound->voices[i];
                break;
            }
            if (sound->voices[i].position > voice->position) voice = &sound->voices[i];
        }
        *voice = (struct sound_voice){ &bank->samples[trigger->sample], 0, trigger->gain };
    }
    atomic_store_explicit(&sound->tail, tail, memory_order_release);
    struct spa_data *out = &buffer->buffer->datas[0];
    float *frames = out->data;
    if (frames) {
        uint32_t count = out->maxsize / (2 * sizeof(float));
        if (buffer->requested && buffer->requested < count) count = (uint32_t)buffer->requested;
        memset(frames, 0, count * 2 * sizeof(float));
        for (size_t i = 0; i < SOUND_VOICES; i++) {
            struct sound_voice *voice = &sound->voices[i];
            if (!voice->sample) continue;
            uint32_t n = voice->sample->count - voice->position;
            if (n > count) n = count;
            const float *source = voice->sample->frames + (size_t)voice->position * 2;
            for (uint32_t f = 0; f < n; f++) {
                frames[f * 2] += source[f * 2] * voice->gain;
                frames[f * 2 + 1] += source[f * 2 + 1] * voice->gain;
            }
            voice->position += n;
            if (voice->position == voice->sample->count) voice->sample = NULL;
        }
        out->chunk->offset = 0;
        out->chunk->stride = 2 * sizeof(float);
        out->chunk->size = count * 2 * sizeof(float);
    }
    pw_stream_queue_buffer(sound->stream, buffer);
}

static const struct pw_core_events sound_core_events = {
    PW_VERSION_CORE_EVENTS,
    .error = sound_core_error,
};

static const struct pw_stream_events sound_stream_events = {
    PW_VERSION_STREAM_EVENTS,
    .state_changed = sound_stream_state,
    .process = sound_process,
};

static void sound_connect(struct sound *sound) {
    sound->core = pw_context_connect(sound->context, NULL, 0);
    if (!sound->core) {
        sound_fault(sound, -errno);
        return;
    }
    pw_core_add_listener(sound->core, &sound->core_listener, &sound_core_events, sound);
    sound->stream = pw_stream_new(sound->core, "tomoe", pw_properties_new(
        PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CATEGORY, "Playback",
        PW_KEY_MEDIA_NAME, "Sounds", PW_KEY_NODE_LATENCY, "256/48000", NULL));
    if (!sound->stream) {
        sound_fault(sound, -errno);
        return;
    }
    pw_stream_add_listener(sound->stream, &sound->stream_listener, &sound_stream_events, sound);
    uint8_t storage[512];
    struct spa_pod_builder builder = SPA_POD_BUILDER_INIT(storage, sizeof(storage));
    const struct spa_pod *params[] = { spa_format_audio_raw_build(&builder, SPA_PARAM_EnumFormat,
        &SPA_AUDIO_INFO_RAW_INIT(.format = SPA_AUDIO_FORMAT_F32, .rate = SOUND_RATE, .channels = 2,
            .position = { SPA_AUDIO_CHANNEL_FL, SPA_AUDIO_CHANNEL_FR })) };
    int result = pw_stream_connect(sound->stream, PW_DIRECTION_OUTPUT, PW_ID_ANY,
        PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS | PW_STREAM_FLAG_RT_PROCESS, params, 1);
    if (result < 0) sound_fault(sound, result);
}

static void sound_disconnect(struct sound *sound) {
    if (sound->stream) {
        spa_hook_remove(&sound->stream_listener);
        pw_stream_destroy(sound->stream);
        sound->stream = NULL;
    }
    if (sound->core) {
        spa_hook_remove(&sound->core_listener);
        pw_core_disconnect(sound->core);
        sound->core = NULL;
    }
}

static void sound_retry(void *data, uint64_t expirations) {
    struct sound *sound = data;
    sound_disconnect(sound);
    atomic_store(&sound->state, SOUND_CONNECTING);
    sound_connect(sound);
}

static int sound_setup(struct spa_loop *loop, bool async, uint32_t seq,
        const void *data, size_t size, void *user_data) {
    struct sound *sound = user_data;
    struct pw_loop *pw = pw_thread_loop_get_loop(sound->loop);
    sound->retry = pw_loop_add_timer(pw, sound_retry, sound);
    sound->context = sound->retry ? pw_context_new(pw,
        pw_properties_new(PW_KEY_APP_NAME, "tomoe", NULL), 0) : NULL;
    if (sound->context) sound_connect(sound);
    else sound_report(sound, SOUND_FAILED, -errno);
    return 0;
}

static int sound_teardown(struct spa_loop *loop, bool async, uint32_t seq,
        const void *data, size_t size, void *user_data) {
    struct sound *sound = user_data;
    sound_disconnect(sound);
    if (sound->context) pw_context_destroy(sound->context);
    if (sound->retry) pw_loop_destroy_source(pw_thread_loop_get_loop(sound->loop), sound->retry);
    return 0;
}

static struct sound *sound_start(struct tomoe *s) {
    static bool initialized;
    struct sound *sound = calloc(1, sizeof(*sound));
    if (!sound) return NULL;
    sigset_t all, previous;
    sigfillset(&all);
    pthread_sigmask(SIG_SETMASK, &all, &previous);
    if (!initialized) pw_init(NULL, NULL);
    initialized = true;
    sound->loop = pw_thread_loop_new("tomoe-sound", NULL);
    int result = sound->loop ? pw_thread_loop_start(sound->loop) : -ENOMEM;
    if (sound->loop && result == 0)
        result = pw_loop_invoke(pw_thread_loop_get_loop(sound->loop), sound_setup, 0, NULL, 0, false, sound);
    pthread_sigmask(SIG_SETMASK, &previous, NULL);
    if (result < 0) {
        if (sound->loop) {
            pw_thread_loop_stop(sound->loop);
            pw_thread_loop_destroy(sound->loop);
            sound->loop = NULL;
        }
        sound_report(sound, SOUND_FAILED, result);
    } else {
        atomic_store(&sound->state, SOUND_CONNECTING);
    }
    return s->sound = sound;
}

void sound_finish(struct tomoe *s) {
    struct sound *sound = s->sound;
    if (!sound) return;
    s->sound = NULL;
    if (sound->loop) {
        sigset_t all, previous;
        sigfillset(&all);
        pthread_sigmask(SIG_SETMASK, &all, &previous);
        pw_loop_invoke(pw_thread_loop_get_loop(sound->loop), sound_teardown, 0, NULL, 0, true, sound);
        pw_thread_loop_stop(sound->loop);
        pw_thread_loop_destroy(sound->loop);
        pthread_sigmask(SIG_SETMASK, &previous, NULL);
    }
    sound_collect(sound);
    sound_bank_free(atomic_load(&sound->pending));
    sound_bank_free(sound->playing);
    free(sound);
}

void sound_publish(struct tomoe *s, struct presentation *plan) {
    struct sound_bank *bank = plan->sounds;
    if (!bank) return;
    plan->sounds = NULL;
    if (!bank->sample_count) {
        sound_bank_free(bank);
        sound_finish(s);
        return;
    }
    struct sound *sound = s->sound ? s->sound : sound_start(s);
    if (!sound) {
        sound_bank_free(bank);
        return;
    }
    bank->generation = ++sound->generation;
    sound_collect(sound);
    sound_bank_free(atomic_exchange(&sound->pending, bank));
    sound->bank = bank;
    memset(sound->cursor, 0, sizeof(sound->cursor));
}

void sound_play(struct tomoe *s, enum sound_event event) {
    struct sound *sound = s->sound;
    if (!sound || s->stopping || !sound->bank || atomic_load(&sound->state) != SOUND_STREAMING) return;
    const struct sound_cue *cue = &sound->bank->cues[event];
    if (!cue->count) return;
    uint32_t head = atomic_load_explicit(&sound->head, memory_order_relaxed);
    if (head - atomic_load_explicit(&sound->tail, memory_order_acquire) >= SOUND_TRIGGERS) {
        atomic_fetch_add(&sound->dropped, 1);
        return;
    }
    sound->triggers[head % SOUND_TRIGGERS] = (struct sound_trigger){
        sound->bank->generation, cue->first + sound->cursor[event]++ % cue->count, cue->gain };
    atomic_store_explicit(&sound->head, head + 1, memory_order_release);
    atomic_fetch_add(&sound->played, 1);
}

const char *tomoe_sound_stats(struct tomoe *s) {
    static const char *const states[] = { "off", "connecting", "streaming", "retrying", "failed" };
    struct sound *sound = s->sound;
    if (!sound) return "(:state :off)";
    int error = atomic_load(&sound->error);
    snprintf(sound_text, sizeof(sound_text), "(:state :%s :error %s%s%s :played %llu :dropped %llu)",
        states[atomic_load(&sound->state)], error ? "\"" : "", error ? spa_strerror(error) : "nil",
        error ? "\"" : "", (unsigned long long)atomic_load(&sound->played),
        (unsigned long long)atomic_load(&sound->dropped));
    return sound_text;
}
