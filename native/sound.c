#include "internal.h"
#include "xdg-system-bell-v1-protocol.h"
#include "pipewire.h"

#include <errno.h>
#include <math.h>
#include <stdint.h>
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
    struct pwn_client *client;
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

static const char *sound_strerror(int error) {
    return strerror(error < 0 ? -error : error);
}

static void sound_report(struct sound *sound, int state, int error) {
    atomic_store(&sound->error, error);
    atomic_store(&sound->state, state);
    if (sound->reported) return;
    sound->reported = true;
    fprintf(stderr, "tomoe: sound playback unavailable: %s\n", sound_strerror(error));
}

static void sound_changed(void *data, int state, int error) {
    struct sound *sound = data;
    if (state == PWN_STREAMING) {
        atomic_store(&sound->error, 0);
        atomic_store(&sound->state, SOUND_STREAMING);
        sound->reported = false;
    } else if (state == PWN_RETRYING) {
        sound_report(sound, SOUND_RETRYING, error);
    } else if (state == PWN_FAILED) {
        sound_report(sound, SOUND_FAILED, error);
    } else {
        atomic_store(&sound->state, SOUND_CONNECTING);
    }
}

static void sound_process(void *data, struct pwn_buffer *buffer) {
    struct sound *sound = data;
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
    uint32_t count = ~0u;
    for (unsigned c = 0; c < 2; c++) {
        if (!buffer->frames[c]) continue;
        uint32_t n = buffer->maxsize[c] / (uint32_t)sizeof(float);
        if (n < count) count = n;
    }
    if (count == ~0u) count = 0;
    if (buffer->requested && buffer->requested < count) count = buffer->requested;
    for (unsigned c = 0; c < 2; c++)
        if (buffer->frames[c]) memset(buffer->frames[c], 0, count * sizeof(float));
    for (size_t i = 0; i < SOUND_VOICES; i++) {
        struct sound_voice *voice = &sound->voices[i];
        if (!voice->sample) continue;
        uint32_t n = voice->sample->count - voice->position;
        if (n > count) n = count;
        const float *source = voice->sample->frames + (size_t)voice->position * 2;
        for (unsigned c = 0; c < 2; c++) {
            if (!buffer->frames[c]) continue;
            float *out = buffer->frames[c];
            for (uint32_t f = 0; f < n; f++) out[f] += source[f * 2 + c] * voice->gain;
        }
        voice->position += n;
        if (voice->position == voice->sample->count) voice->sample = NULL;
    }
    for (unsigned c = 0; c < 2; c++) {
        if (!buffer->chunk[c]) continue;
        buffer->chunk[c]->offset = 0;
        buffer->chunk[c]->stride = (int32_t)sizeof(float);
        buffer->chunk[c]->size = count * (uint32_t)sizeof(float);
    }
}

static struct sound *sound_start(struct tomoe *s) {
    static const struct pwn_ops ops = { .state = sound_changed, .process = sound_process };
    struct sound *sound = calloc(1, sizeof(*sound));
    if (!sound) return NULL;
    sound->client = pwn_start("tomoe", "tomoe", SOUND_RATE, 2, &ops, sound);
    if (!sound->client) sound_report(sound, SOUND_FAILED, -ENOMEM);
    else atomic_store(&sound->state, SOUND_CONNECTING);
    return s->sound = sound;
}

void sound_finish(struct tomoe *s) {
    struct sound *sound = s->sound;
    if (!sound) return;
    s->sound = NULL;
    pwn_stop(sound->client);
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
        states[atomic_load(&sound->state)], error ? "\"" : "", error ? sound_strerror(error) : "nil",
        error ? "\"" : "", (unsigned long long)atomic_load(&sound->played),
        (unsigned long long)atomic_load(&sound->dropped));
    return sound_text;
}

static void bell_destroy(struct wl_client *client, struct wl_resource *resource) {
    wl_resource_destroy(resource);
}

static void bell_ring(struct wl_client *client, struct wl_resource *resource,
        struct wl_resource *surface) {
    sound_play(wl_resource_get_user_data(resource), SOUND_BELL);
}

static const struct xdg_system_bell_v1_interface bell_impl = {
    .destroy = bell_destroy, .ring = bell_ring,
};

static void bell_bind(struct wl_client *client, void *data, uint32_t version, uint32_t id) {
    struct wl_resource *resource = wl_resource_create(client, &xdg_system_bell_v1_interface, version, id);
    if (!resource) wl_client_post_no_memory(client);
    else wl_resource_set_implementation(resource, &bell_impl, data, NULL);
}

bool system_bell_listen(struct tomoe *s) {
    return wl_global_create(s->display, &xdg_system_bell_v1_interface, 1, s, bell_bind);
}
