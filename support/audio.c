#define _GNU_SOURCE

#include "audio.h"
#include "pipewire.h"

#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define AUDIO_RETRY_USEC 1000000ULL
#define AUDIO_NAME_MAX 256U
#define AUDIO_MAX_DICT 1024U
#define AUDIO_REGISTRY_PROXY 2U
#define AUDIO_FIRST_PROXY 3U

struct audio_props {
    int valid;
    double volume;
    int muted;
};

struct audio_sink {
    uint32_t global_id;
    uint32_t version;
    uint32_t device_id;
    int32_t priority;
    char name[AUDIO_NAME_MAX];
};

struct audio_device {
    uint32_t global_id;
    uint32_t version;
};

struct tomoe_audio {
    struct pww wire;
    uint64_t retry_due_usec;
    uint32_t sync_counter;
    uint32_t sync_target;
    int synced;

    uint32_t next_proxy;
    uint8_t *proxy_used;
    size_t proxy_cap;
    uint32_t *free_ids;
    size_t n_free;
    size_t free_cap;

    struct audio_sink *sinks;
    size_t n_sinks;
    size_t cap_sinks;

    struct audio_device *devices;
    size_t n_devices;
    size_t cap_devices;

    uint32_t meta_global;
    uint32_t meta_proxy;
    char default_sink[AUDIO_NAME_MAX];
    char configured_sink[AUDIO_NAME_MAX];

    uint32_t target_sink_global;
    uint32_t sink_proxy;
    uint32_t target_device_global;
    uint32_t device_proxy;
    int32_t target_card_profile_device;

    struct audio_props node_props;
    struct audio_props route_props;

    int available;
    double volume;
    int muted;
    int changed;
    char *snapshot;
};

static uint64_t monotonic_usec(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) < 0) return 0;
    if ((uint64_t)ts.tv_sec > UINT64_MAX / 1000000ULL) return UINT64_MAX;
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)(ts.tv_nsec / 1000L);
}

static double linear_to_cubic(float linear) {
    if (!isfinite(linear) || linear <= 0.0f) return 0.0;
    return cbrt((double)linear);
}

static void audio_publish(struct tomoe_audio *audio, int available, double volume, int muted) {
    if (!isfinite(volume) || volume < 0.0) volume = 0.0;
    muted = muted ? 1 : 0;
    available = available ? 1 : 0;
    if (audio->available != available || audio->muted != muted ||
        fabs(audio->volume - volume) > 1e-6) {
        audio->available = available;
        audio->volume = volume;
        audio->muted = muted;
        audio->changed = 1;
    }
}

static void audio_recompute(struct tomoe_audio *audio) {
    if (!audio->sink_proxy) {
        audio_publish(audio, 0, 1.0, 0);
        return;
    }
    if (audio->route_props.valid) {
        audio_publish(audio, 1, audio->route_props.volume, audio->route_props.muted);
    } else if (audio->node_props.valid) {
        audio_publish(audio, 1, audio->node_props.volume, audio->node_props.muted);
    }
}

static uint32_t proxy_alloc(struct tomoe_audio *audio) {
    if (audio->n_free > 0) {
        uint32_t id = audio->free_ids[--audio->n_free];
        if (id < audio->proxy_cap) audio->proxy_used[id] = 1;
        return id;
    }
    uint32_t id = audio->next_proxy;
    if (id >= audio->proxy_cap) {
        size_t cap = audio->proxy_cap ? audio->proxy_cap * 2U : 32U;
        while (cap <= id) cap *= 2U;
        uint8_t *grown = realloc(audio->proxy_used, cap);
        if (!grown) return 0;
        memset(grown + audio->proxy_cap, 0, cap - audio->proxy_cap);
        audio->proxy_used = grown;
        audio->proxy_cap = cap;
    }
    audio->next_proxy = id + 1U;
    audio->proxy_used[id] = 1;
    return id;
}

static void proxy_release(struct tomoe_audio *audio, uint32_t id) {
    if (id < AUDIO_FIRST_PROXY || id >= audio->proxy_cap || !audio->proxy_used[id]) return;
    audio->proxy_used[id] = 0;
    if (audio->n_free == audio->free_cap) {
        size_t cap = audio->free_cap ? audio->free_cap * 2U : 16U;
        uint32_t *grown = realloc(audio->free_ids, cap * sizeof(*grown));
        if (!grown) return;
        audio->free_ids = grown;
        audio->free_cap = cap;
    }
    audio->free_ids[audio->n_free++] = id;
}

static int send_hello(struct tomoe_audio *audio) {
    uint8_t storage[128];
    struct pww_build b;
    struct pww_frame root;
    pww_build_init(&b, storage, sizeof(storage));
    pww_pod_struct_begin(&b, &root);
    pww_pod_int(&b, 4);
    return pww_message_send(&audio->wire, &b, &root, 0, PW_CORE_METHOD_HELLO);
}

static int send_client_props(struct tomoe_audio *audio) {
    uint8_t storage[512];
    struct pww_build b;
    struct pww_frame root, dict;
    pww_build_init(&b, storage, sizeof(storage));
    pww_pod_struct_begin(&b, &root);
    pww_pod_struct_begin(&b, &dict);
    pww_pod_int(&b, 2);
    pww_pod_string(&b, "application.name");
    pww_pod_string(&b, "tomoe-audio");
    pww_pod_string(&b, "application.process.binary");
    pww_pod_string(&b, "tomoe");
    pww_pod_struct_end(&b, &dict);
    return pww_message_send(&audio->wire, &b, &root, 1, PW_CLIENT_METHOD_UPDATE_PROPERTIES);
}

static int send_get_registry(struct tomoe_audio *audio) {
    uint8_t storage[128];
    struct pww_build b;
    struct pww_frame root;
    pww_build_init(&b, storage, sizeof(storage));
    pww_pod_struct_begin(&b, &root);
    pww_pod_int(&b, 3);
    pww_pod_int(&b, (int32_t)AUDIO_REGISTRY_PROXY);
    return pww_message_send(&audio->wire, &b, &root, 0, PW_CORE_METHOD_GET_REGISTRY);
}

static int send_sync(struct tomoe_audio *audio, uint32_t *seq_out) {
    uint32_t seq = 0x40000000u | (++audio->sync_counter & 0x3fffffffu);
    uint8_t storage[128];
    struct pww_build b;
    struct pww_frame root;
    pww_build_init(&b, storage, sizeof(storage));
    pww_pod_struct_begin(&b, &root);
    pww_pod_int(&b, 0);
    pww_pod_int(&b, (int32_t)seq);
    if (seq_out) *seq_out = seq;
    return pww_message_send(&audio->wire, &b, &root, 0, PW_CORE_METHOD_SYNC);
}

static int send_pong(struct tomoe_audio *audio, uint32_t id, uint32_t seq) {
    uint8_t storage[128];
    struct pww_build b;
    struct pww_frame root;
    pww_build_init(&b, storage, sizeof(storage));
    pww_pod_struct_begin(&b, &root);
    pww_pod_int(&b, (int32_t)id);
    pww_pod_int(&b, (int32_t)seq);
    return pww_message_send(&audio->wire, &b, &root, 0, PW_CORE_METHOD_PONG);
}

static int send_destroy(struct tomoe_audio *audio, uint32_t proxy_id) {
    uint8_t storage[128];
    struct pww_build b;
    struct pww_frame root;
    pww_build_init(&b, storage, sizeof(storage));
    pww_pod_struct_begin(&b, &root);
    pww_pod_int(&b, (int32_t)proxy_id);
    return pww_message_send(&audio->wire, &b, &root, 0, PW_CORE_METHOD_DESTROY);
}

static int send_bind(struct tomoe_audio *audio, uint32_t global_id, const char *type,
                     uint32_t version, uint32_t proxy_id) {
    uint8_t storage[512];
    struct pww_build b;
    struct pww_frame root;
    pww_build_init(&b, storage, sizeof(storage));
    pww_pod_struct_begin(&b, &root);
    pww_pod_int(&b, (int32_t)global_id);
    pww_pod_string(&b, type);
    pww_pod_int(&b, (int32_t)version);
    pww_pod_int(&b, (int32_t)proxy_id);
    return pww_message_send(&audio->wire, &b, &root, AUDIO_REGISTRY_PROXY, PW_REGISTRY_METHOD_BIND);
}

static int send_subscribe_param(struct tomoe_audio *audio, uint32_t proxy_id,
                                uint32_t opcode, uint32_t param_id) {
    uint8_t storage[128];
    struct pww_build b;
    struct pww_frame root;
    uint32_t ids[1] = { param_id };
    pww_build_init(&b, storage, sizeof(storage));
    pww_pod_struct_begin(&b, &root);
    pww_pod_array_ids(&b, ids, 1);
    return pww_message_send(&audio->wire, &b, &root, proxy_id, opcode);
}

static int parse_opt_string(struct pww_parse *p, const char **str, size_t *len) {
    uint32_t type = 0, body = 0;
    const uint8_t *pod = pww_parse_pod(p, &type, &body);
    if (!pod) return -1;
    if (type == SPA_TYPE_NONE) {
        *str = NULL;
        *len = 0;
        return 0;
    }
    if (type != SPA_TYPE_STRING) return -1;
    const char *ptr = (const char *)(pod + 8);
    size_t n = body;
    while (n > 0 && ptr[n - 1] == '\0') n--;
    *str = ptr;
    *len = n;
    return 0;
}

static int parse_string_slice(struct pww_parse *p, const char **str, size_t *len) {
    if (parse_opt_string(p, str, len) < 0 || !*str) return -1;
    return 0;
}

static int span_eq(const char *s, size_t len, const char *lit) {
    size_t l = strlen(lit);
    return len == l && memcmp(s, lit, l) == 0;
}

static void span_copy(char *dst, size_t cap, const char *src, size_t len) {
    if (cap == 0) return;
    if (len >= cap) len = cap - 1U;
    if (len > 0) memcpy(dst, src, len);
    dst[len] = '\0';
}

static int span_parse_u32(const char *s, size_t len, uint32_t *out) {
    if (len == 0 || len >= 32U) return 0;
    char buf[32];
    memcpy(buf, s, len);
    buf[len] = '\0';
    char *end = NULL;
    unsigned long v = strtoul(buf, &end, 10);
    if (end == buf || *end != '\0' || v > UINT32_MAX) return 0;
    *out = (uint32_t)v;
    return 1;
}

static int span_parse_i32(const char *s, size_t len, int32_t *out) {
    if (len == 0 || len >= 32U) return 0;
    char buf[32];
    memcpy(buf, s, len);
    buf[len] = '\0';
    char *end = NULL;
    long v = strtol(buf, &end, 10);
    if (end == buf || *end != '\0' || v < INT32_MIN || v > INT32_MAX) return 0;
    *out = (int32_t)v;
    return 1;
}

static int parse_json_name(const char *json, size_t len, char *out, size_t cap) {
    if (!json || len == 0 || cap == 0) return 0;
    size_t i = 0;
    while (i < len) {
        while (i < len && json[i] != '"') i++;
        if (i >= len) return 0;
        i++;
        size_t key_start = i;
        while (i < len && json[i] != '"') {
            if (json[i] == '\\' && i + 1U < len) i += 2U;
            else i++;
        }
        if (i >= len) return 0;
        size_t key_len = i - key_start;
        i++;
        while (i < len && isspace((unsigned char)json[i])) i++;
        if (i >= len || json[i] != ':') continue;
        i++;
        while (i < len && isspace((unsigned char)json[i])) i++;
        if (key_len == 4U && memcmp(json + key_start, "name", 4U) == 0) {
            if (i >= len || json[i] != '"') return 0;
            i++;
            size_t pos = 0;
            while (i < len && json[i] != '"') {
                char c = json[i++];
                if (c == '\\') {
                    if (i >= len) return 0;
                    c = json[i++];
                }
                if (pos + 1U < cap) out[pos++] = c;
            }
            if (i >= len) return 0;
            out[pos] = '\0';
            return pos > 0 ? 1 : 0;
        }
        if (i < len && json[i] == '"') {
            i++;
            while (i < len && json[i] != '"') {
                if (json[i] == '\\' && i + 1U < len) i += 2U;
                else i++;
            }
            if (i < len) i++;
        }
    }
    return 0;
}

static int parse_props_pod(const uint8_t *pod, uint32_t body, struct audio_props *dst) {
    if (body < 8U) return 0;
    size_t end = 8U + (size_t)body;
    size_t off = 16U;
    int have_vol = 0, have_ch = 0, have_soft_vol = 0;
    int have_mute = 0, have_soft_mute = 0;
    float vol = 1.0f, ch = 1.0f, soft_vol = 1.0f;
    int mute = 0, soft_mute = 0;

    while (off + 16U <= end) {
        uint32_t key, flags, size, type;
        memcpy(&key, pod + off, 4U);
        memcpy(&flags, pod + off + 4U, 4U);
        (void)flags;
        off += 8U;
        memcpy(&size, pod + off, 4U);
        memcpy(&type, pod + off + 4U, 4U);
        if (size > end - (off + 8U)) break;
        const uint8_t *val = pod + off + 8U;
        switch (key) {
        case SPA_PROP_VOLUME:
            if (type == SPA_TYPE_FLOAT && size >= 4U) {
                memcpy(&vol, val, 4U);
                have_vol = 1;
            }
            break;
        case SPA_PROP_MUTE:
            if (type == SPA_TYPE_BOOL && size >= 4U) {
                uint32_t b = 0;
                memcpy(&b, val, 4U);
                mute = b ? 1 : 0;
                have_mute = 1;
            }
            break;
        case SPA_PROP_CHANNELVOLUMES:
            if (type == SPA_TYPE_ARRAY && size >= 12U) {
                uint32_t csize = 0, ctype = 0;
                memcpy(&csize, val, 4U);
                memcpy(&ctype, val + 4U, 4U);
                if (csize == 4U && ctype == SPA_TYPE_FLOAT) {
                    memcpy(&ch, val + 8U, 4U);
                    have_ch = 1;
                }
            }
            break;
        case SPA_PROP_SOFTMUTE:
            if (type == SPA_TYPE_BOOL && size >= 4U) {
                uint32_t b = 0;
                memcpy(&b, val, 4U);
                soft_mute = b ? 1 : 0;
                have_soft_mute = 1;
            }
            break;
        case SPA_PROP_SOFTVOLUMES:
            if (type == SPA_TYPE_ARRAY && size >= 12U) {
                uint32_t csize = 0, ctype = 0;
                memcpy(&csize, val, 4U);
                memcpy(&ctype, val + 4U, 4U);
                if (csize == 4U && ctype == SPA_TYPE_FLOAT) {
                    memcpy(&soft_vol, val + 8U, 4U);
                    have_soft_vol = 1;
                }
            }
            break;
        default:
            break;
        }
        off += 8U + (size_t)size;
        off = (off + 7U) & ~(size_t)7U;
    }

    if (!have_vol && !have_ch && !have_soft_vol && !have_mute && !have_soft_mute) return 0;
    if (!dst->valid) {
        dst->volume = 1.0;
        dst->muted = 0;
        dst->valid = 1;
    }
    if (have_ch) dst->volume = linear_to_cubic(ch);
    else if (have_vol) dst->volume = linear_to_cubic(vol);
    else if (have_soft_vol) dst->volume = linear_to_cubic(soft_vol);
    if (have_mute) dst->muted = mute;
    else if (have_soft_mute) dst->muted = soft_mute;
    return 1;
}

static int parse_route_pod(const uint8_t *pod, uint32_t body, int32_t want_device,
                           struct audio_props *dst) {
    if (body < 8U || want_device < 0) return 0;
    size_t end = 8U + (size_t)body;
    size_t off = 16U;
    int have_dir = 0, have_dev = 0;
    uint32_t dir = 0;
    int32_t dev = -1;
    const uint8_t *props_pod = NULL;
    uint32_t props_body = 0;

    while (off + 16U <= end) {
        uint32_t key, flags, size, type;
        memcpy(&key, pod + off, 4U);
        memcpy(&flags, pod + off + 4U, 4U);
        (void)flags;
        off += 8U;
        memcpy(&size, pod + off, 4U);
        memcpy(&type, pod + off + 4U, 4U);
        if (size > end - (off + 8U)) break;
        const uint8_t *val = pod + off + 8U;
        if (key == SPA_PARAM_ROUTE_DIRECTION && type == SPA_TYPE_ID && size >= 4U) {
            memcpy(&dir, val, 4U);
            have_dir = 1;
        } else if (key == SPA_PARAM_ROUTE_DEVICE && type == SPA_TYPE_INT && size >= 4U) {
            memcpy(&dev, val, 4U);
            have_dev = 1;
        } else if (key == SPA_PARAM_ROUTE_PROPS && type == SPA_TYPE_OBJECT && size >= 8U) {
            props_pod = pod + off;
            props_body = size;
        }
        off += 8U + (size_t)size;
        off = (off + 7U) & ~(size_t)7U;
    }

    if (!have_dir || dir != SPA_DIRECTION_OUTPUT || !have_dev || dev != want_device || !props_pod)
        return 0;
    return parse_props_pod(props_pod, props_body, dst);
}

static struct audio_device *find_device(struct tomoe_audio *audio, uint32_t global_id) {
    for (size_t i = 0; i < audio->n_devices; i++) {
        if (audio->devices[i].global_id == global_id) return &audio->devices[i];
    }
    return NULL;
}

static int upsert_device(struct tomoe_audio *audio, uint32_t global_id, uint32_t version) {
    struct audio_device *existing = find_device(audio, global_id);
    if (existing) {
        existing->version = version;
        return 0;
    }
    if (audio->n_devices == audio->cap_devices) {
        size_t cap = audio->cap_devices ? audio->cap_devices * 2U : 16U;
        struct audio_device *grown = realloc(audio->devices, cap * sizeof(*grown));
        if (!grown) return -ENOMEM;
        audio->devices = grown;
        audio->cap_devices = cap;
    }
    audio->devices[audio->n_devices].global_id = global_id;
    audio->devices[audio->n_devices].version = version;
    audio->n_devices++;
    return 0;
}

static int upsert_sink(struct tomoe_audio *audio, uint32_t global_id, uint32_t version,
                       const char *name, uint32_t device_id, int32_t priority) {
    for (size_t i = 0; i < audio->n_sinks; i++) {
        if (audio->sinks[i].global_id != global_id) continue;
        audio->sinks[i].version = version;
        audio->sinks[i].device_id = device_id;
        audio->sinks[i].priority = priority;
        snprintf(audio->sinks[i].name, sizeof(audio->sinks[i].name), "%s", name);
        return 0;
    }
    if (audio->n_sinks == audio->cap_sinks) {
        size_t cap = audio->cap_sinks ? audio->cap_sinks * 2U : 16U;
        struct audio_sink *grown = realloc(audio->sinks, cap * sizeof(*grown));
        if (!grown) return -ENOMEM;
        audio->sinks = grown;
        audio->cap_sinks = cap;
    }
    struct audio_sink *slot = &audio->sinks[audio->n_sinks++];
    slot->global_id = global_id;
    slot->version = version;
    slot->device_id = device_id;
    slot->priority = priority;
    snprintf(slot->name, sizeof(slot->name), "%s", name);
    return 0;
}

static int unbind_device(struct tomoe_audio *audio) {
    audio->route_props.valid = 0;
    if (!audio->device_proxy) return 0;
    uint32_t proxy = audio->device_proxy;
    audio->device_proxy = 0;
    return send_destroy(audio, proxy);
}

static int bind_target_device(struct tomoe_audio *audio) {
    if (audio->device_proxy || audio->target_device_global == SPA_ID_INVALID ||
        audio->target_card_profile_device < 0)
        return 0;
    struct audio_device *dev = find_device(audio, audio->target_device_global);
    if (!dev) return 0;
    uint32_t proxy = proxy_alloc(audio);
    if (!proxy) return -ENOMEM;
    uint32_t ver = dev->version < 3U ? dev->version : 3U;
    int rc = send_bind(audio, dev->global_id, "PipeWire:Interface:Device", ver, proxy);
    if (rc < 0) return rc;
    audio->device_proxy = proxy;
    return send_subscribe_param(audio, proxy, PW_DEVICE_METHOD_SUBSCRIBE_PARAMS, SPA_PARAM_ROUTE);
}

static struct audio_sink *select_target_sink(struct tomoe_audio *audio) {
    if (audio->default_sink[0]) {
        for (size_t i = 0; i < audio->n_sinks; i++) {
            if (strcmp(audio->sinks[i].name, audio->default_sink) == 0) return &audio->sinks[i];
        }
    }
    if (audio->configured_sink[0]) {
        for (size_t i = 0; i < audio->n_sinks; i++) {
            if (strcmp(audio->sinks[i].name, audio->configured_sink) == 0) return &audio->sinks[i];
        }
    }
    struct audio_sink *best = NULL;
    for (size_t i = 0; i < audio->n_sinks; i++) {
        struct audio_sink *s = &audio->sinks[i];
        if (!best || s->priority > best->priority ||
            (s->priority == best->priority && s->global_id < best->global_id)) {
            best = s;
        }
    }
    return best;
}

static int resolve_target_sink(struct tomoe_audio *audio) {
    struct audio_sink *wanted = select_target_sink(audio);
    uint32_t wanted_id = wanted ? wanted->global_id : SPA_ID_INVALID;
    if (wanted_id == audio->target_sink_global && (wanted_id == SPA_ID_INVALID || audio->sink_proxy))
        return 0;

    int rc = unbind_device(audio);
    if (rc < 0) return rc;
    if (audio->sink_proxy) {
        uint32_t old_proxy = audio->sink_proxy;
        audio->sink_proxy = 0;
        rc = send_destroy(audio, old_proxy);
        if (rc < 0) return rc;
    }

    audio->target_sink_global = SPA_ID_INVALID;
    audio->target_device_global = SPA_ID_INVALID;
    audio->target_card_profile_device = -1;
    audio->node_props.valid = 0;
    audio->route_props.valid = 0;

    if (!wanted) {
        audio_recompute(audio);
        return 0;
    }

    uint32_t proxy = proxy_alloc(audio);
    if (!proxy) return -ENOMEM;
    uint32_t ver = wanted->version < 3U ? wanted->version : 3U;
    rc = send_bind(audio, wanted->global_id, "PipeWire:Interface:Node", ver, proxy);
    if (rc < 0) return rc;
    audio->target_sink_global = wanted->global_id;
    audio->target_device_global = wanted->device_id;
    audio->sink_proxy = proxy;
    return send_subscribe_param(audio, proxy, PW_NODE_METHOD_SUBSCRIBE_PARAMS, SPA_PARAM_PROPS);
}

static int handle_registry_global(struct tomoe_audio *audio, struct pww_parse *p) {
    uint32_t global_id = 0, perms = 0, version = 0;
    const char *type = NULL;
    size_t type_len = 0;
    if (pww_parse_int(p, &global_id) || pww_parse_int(p, &perms) ||
        parse_string_slice(p, &type, &type_len) || pww_parse_int(p, &version))
        return -EPROTO;
    (void)perms;

    uint32_t pod_type = 0, pod_body = 0;
    const uint8_t *dict_pod = pww_parse_pod(p, &pod_type, &pod_body);
    if (!dict_pod || pod_type != SPA_TYPE_STRUCT) return -EPROTO;

    struct pww_parse q;
    pww_parse_init(&q, dict_pod + 8U, pod_body);
    uint32_t count = 0;
    if (pww_parse_int(&q, &count) || count > AUDIO_MAX_DICT) return -EPROTO;

    int is_meta = span_eq(type, type_len, "PipeWire:Interface:Metadata");
    int is_node = span_eq(type, type_len, "PipeWire:Interface:Node");
    int is_device = span_eq(type, type_len, "PipeWire:Interface:Device");
    if (!is_meta && !is_node && !is_device) return 0;

    int meta_is_default = 0;
    int node_is_sink = 0;
    char node_name[AUDIO_NAME_MAX];
    node_name[0] = '\0';
    uint32_t device_id = SPA_ID_INVALID;
    int32_t priority = 0;

    for (uint32_t i = 0; i < count; i++) {
        const char *k = NULL, *v = NULL;
        size_t klen = 0, vlen = 0;
        if (parse_string_slice(&q, &k, &klen) || parse_opt_string(&q, &v, &vlen)) return -EPROTO;
        if (!v) continue;
        if (is_meta && span_eq(k, klen, "metadata.name") && span_eq(v, vlen, "default")) {
            meta_is_default = 1;
        } else if (is_node) {
            if (span_eq(k, klen, "media.class") && span_eq(v, vlen, "Audio/Sink")) {
                node_is_sink = 1;
            } else if (span_eq(k, klen, "node.name")) {
                span_copy(node_name, sizeof(node_name), v, vlen);
            } else if (span_eq(k, klen, "device.id")) {
                (void)span_parse_u32(v, vlen, &device_id);
            } else if (span_eq(k, klen, "priority.session")) {
                (void)span_parse_i32(v, vlen, &priority);
            }
        }
    }

    if (is_meta && meta_is_default && !audio->meta_proxy) {
        uint32_t proxy = proxy_alloc(audio);
        if (!proxy) return -ENOMEM;
        uint32_t ver = version < 3U ? version : 3U;
        int rc = send_bind(audio, global_id, "PipeWire:Interface:Metadata", ver, proxy);
        if (rc < 0) return rc;
        audio->meta_global = global_id;
        audio->meta_proxy = proxy;
        if (!audio->synced) {
            rc = send_sync(audio, &audio->sync_target);
            if (rc < 0) return rc;
        }
    } else if (is_node && node_is_sink && node_name[0]) {
        int rc = upsert_sink(audio, global_id, version, node_name, device_id, priority);
        if (rc < 0) return rc;
        if (audio->synced) return resolve_target_sink(audio);
    } else if (is_device) {
        int rc = upsert_device(audio, global_id, version);
        if (rc < 0) return rc;
        if (audio->synced && audio->target_device_global == global_id &&
            audio->target_card_profile_device >= 0 && !audio->device_proxy) {
            return bind_target_device(audio);
        }
    }
    return 0;
}

static int handle_registry_remove(struct tomoe_audio *audio, struct pww_parse *p) {
    uint32_t global_id = 0;
    if (pww_parse_int(p, &global_id)) return -EPROTO;

    for (size_t i = 0; i < audio->n_sinks; i++) {
        if (audio->sinks[i].global_id != global_id) continue;
        audio->sinks[i] = audio->sinks[--audio->n_sinks];
        break;
    }
    for (size_t i = 0; i < audio->n_devices; i++) {
        if (audio->devices[i].global_id != global_id) continue;
        audio->devices[i] = audio->devices[--audio->n_devices];
        break;
    }
    if (audio->meta_global == global_id) {
        audio->meta_global = SPA_ID_INVALID;
        if (audio->meta_proxy) {
            uint32_t proxy = audio->meta_proxy;
            audio->meta_proxy = 0;
            int rc = send_destroy(audio, proxy);
            if (rc < 0) return rc;
        }
        audio->default_sink[0] = '\0';
        audio->configured_sink[0] = '\0';
        if (audio->synced) return resolve_target_sink(audio);
        return 0;
    }
    if (audio->target_sink_global == global_id) {
        if (audio->synced) return resolve_target_sink(audio);
        return 0;
    }
    if (audio->target_device_global == global_id) {
        audio->target_device_global = SPA_ID_INVALID;
        int rc = unbind_device(audio);
        audio_recompute(audio);
        return rc;
    }
    return 0;
}

static int handle_metadata_property(struct tomoe_audio *audio, struct pww_parse *p) {
    uint32_t subject = 0;
    const char *key = NULL, *type = NULL, *value = NULL;
    size_t key_len = 0, type_len = 0, value_len = 0;
    if (pww_parse_int(p, &subject) || parse_opt_string(p, &key, &key_len) ||
        parse_opt_string(p, &type, &type_len) || parse_opt_string(p, &value, &value_len))
        return -EPROTO;
    (void)type;
    (void)type_len;
    if (subject != 0) return 0;

    int relevant = 0;
    if (!key) {
        audio->default_sink[0] = '\0';
        audio->configured_sink[0] = '\0';
        relevant = 1;
    } else if (span_eq(key, key_len, "default.audio.sink")) {
        if (!value || !parse_json_name(value, value_len, audio->default_sink,
                                       sizeof(audio->default_sink)))
            audio->default_sink[0] = '\0';
        relevant = 1;
    } else if (span_eq(key, key_len, "default.configured.audio.sink")) {
        if (!value || !parse_json_name(value, value_len, audio->configured_sink,
                                       sizeof(audio->configured_sink)))
            audio->configured_sink[0] = '\0';
        relevant = 1;
    }
    if (relevant && audio->synced) return resolve_target_sink(audio);
    return 0;
}

static int handle_node_info(struct tomoe_audio *audio, struct pww_parse *p) {
    uint32_t id, max_in, max_out, n_in, n_out, state;
    int64_t change_mask = 0;
    const char *error = NULL;
    size_t error_len = 0;
    if (pww_parse_int(p, &id) || pww_parse_int(p, &max_in) || pww_parse_int(p, &max_out) ||
        pww_parse_typed(p, SPA_TYPE_LONG, &change_mask, 8U) || pww_parse_int(p, &n_in) ||
        pww_parse_int(p, &n_out) || pww_parse_id(p, &state) ||
        parse_opt_string(p, &error, &error_len))
        return -EPROTO;
    (void)id;
    (void)max_in;
    (void)max_out;
    (void)change_mask;
    (void)n_in;
    (void)n_out;
    (void)state;
    (void)error;
    (void)error_len;

    uint32_t pod_type = 0, pod_body = 0;
    const uint8_t *dict_pod = pww_parse_pod(p, &pod_type, &pod_body);
    if (!dict_pod || pod_type != SPA_TYPE_STRUCT) return -EPROTO;

    struct pww_parse q;
    pww_parse_init(&q, dict_pod + 8U, pod_body);
    uint32_t count = 0;
    if (pww_parse_int(&q, &count) || count > AUDIO_MAX_DICT) return -EPROTO;

    uint32_t dev_id = SPA_ID_INVALID;
    int have_dev = 0;
    int32_t cpd = -1;
    int have_cpd = 0;

    for (uint32_t i = 0; i < count; i++) {
        const char *k = NULL, *v = NULL;
        size_t klen = 0, vlen = 0;
        if (parse_string_slice(&q, &k, &klen) || parse_opt_string(&q, &v, &vlen)) return -EPROTO;
        if (!v) continue;
        if (span_eq(k, klen, "device.id") && span_parse_u32(v, vlen, &dev_id)) {
            have_dev = 1;
        } else if (span_eq(k, klen, "card.profile.device") && span_parse_i32(v, vlen, &cpd)) {
            have_cpd = 1;
        }
    }

    if (have_dev && dev_id != audio->target_device_global) {
        int rc = unbind_device(audio);
        if (rc < 0) return rc;
        audio->target_device_global = dev_id;
    }
    if (have_cpd && cpd != audio->target_card_profile_device) {
        audio->target_card_profile_device = cpd;
        audio->route_props.valid = 0;
    }
    if (audio->target_device_global != SPA_ID_INVALID && audio->target_card_profile_device >= 0 &&
        !audio->device_proxy) {
        return bind_target_device(audio);
    }
    return 0;
}

static int handle_node_param(struct tomoe_audio *audio, struct pww_parse *p) {
    uint32_t seq = 0, param_id = 0, index = 0, next = 0;
    if (pww_parse_int(p, &seq) || pww_parse_id(p, &param_id) ||
        pww_parse_int(p, &index) || pww_parse_int(p, &next))
        return -EPROTO;
    (void)seq;
    (void)index;
    (void)next;
    uint32_t type = 0, body = 0;
    const uint8_t *pod = pww_parse_pod(p, &type, &body);
    if (!pod) return -EPROTO;
    if (param_id == SPA_PARAM_PROPS && type == SPA_TYPE_OBJECT) {
        if (parse_props_pod(pod, body, &audio->node_props)) audio_recompute(audio);
    }
    return 0;
}

static int handle_device_param(struct tomoe_audio *audio, struct pww_parse *p) {
    uint32_t seq = 0, param_id = 0, index = 0, next = 0;
    if (pww_parse_int(p, &seq) || pww_parse_id(p, &param_id) ||
        pww_parse_int(p, &index) || pww_parse_int(p, &next))
        return -EPROTO;
    (void)seq;
    (void)index;
    (void)next;
    uint32_t type = 0, body = 0;
    const uint8_t *pod = pww_parse_pod(p, &type, &body);
    if (!pod) return -EPROTO;
    if (param_id == SPA_PARAM_ROUTE && type == SPA_TYPE_OBJECT) {
        if (parse_route_pod(pod, body, audio->target_card_profile_device, &audio->route_props))
            audio_recompute(audio);
    }
    return 0;
}

static int audio_dispatch(void *data, uint32_t id, uint32_t opcode, struct pww_parse *p) {
    struct tomoe_audio *audio = data;
    if (id == 0) {
        switch (opcode) {
        case PW_CORE_EVENT_PING: {
            uint32_t target = 0, seq = 0;
            if (pww_parse_int(p, &target) || pww_parse_int(p, &seq)) return -EPROTO;
            return send_pong(audio, target, seq);
        }
        case PW_CORE_EVENT_DONE: {
            uint32_t target = 0, seq = 0;
            if (pww_parse_int(p, &target) || pww_parse_int(p, &seq)) return -EPROTO;
            if (!audio->synced && target == 0 && seq == audio->sync_target) {
                audio->synced = 1;
                return resolve_target_sink(audio);
            }
            return 0;
        }
        case PW_CORE_EVENT_REMOVE_ID: {
            uint32_t removed = 0;
            if (pww_parse_int(p, &removed)) return -EPROTO;
            proxy_release(audio, removed);
            if (removed == audio->meta_proxy) {
                audio->meta_proxy = 0;
                audio->meta_global = SPA_ID_INVALID;
            } else if (removed == audio->sink_proxy) {
                audio->sink_proxy = 0;
                audio->target_sink_global = SPA_ID_INVALID;
                audio->node_props.valid = 0;
                audio_recompute(audio);
            } else if (removed == audio->device_proxy) {
                audio->device_proxy = 0;
                audio->route_props.valid = 0;
                audio_recompute(audio);
            }
            return 0;
        }
        case PW_CORE_EVENT_ERROR: {
            uint32_t target = 0, seq = 0, res = 0;
            if (pww_parse_int(p, &target) || pww_parse_int(p, &seq) || pww_parse_int(p, &res))
                return -EPROTO;
            (void)seq;
            (void)res;
            if (target == 0 || target == AUDIO_REGISTRY_PROXY) return -EPROTO;
            return 0;
        }
        default:
            return 0;
        }
    }
    if (id == AUDIO_REGISTRY_PROXY) {
        if (opcode == PW_REGISTRY_EVENT_GLOBAL) return handle_registry_global(audio, p);
        if (opcode == PW_REGISTRY_EVENT_GLOBAL_REMOVE) return handle_registry_remove(audio, p);
        return 0;
    }
    if (audio->meta_proxy && id == audio->meta_proxy) {
        if (opcode == PW_METADATA_EVENT_PROPERTY) return handle_metadata_property(audio, p);
        return 0;
    }
    if (audio->sink_proxy && id == audio->sink_proxy) {
        if (opcode == PW_NODE_EVENT_INFO) return handle_node_info(audio, p);
        if (opcode == PW_NODE_EVENT_PARAM) return handle_node_param(audio, p);
        return 0;
    }
    if (audio->device_proxy && id == audio->device_proxy) {
        if (opcode == PW_DEVICE_EVENT_PARAM) return handle_device_param(audio, p);
        return 0;
    }
    return 0;
}

static void audio_disconnect(struct tomoe_audio *audio, int schedule_retry) {
    if (audio->wire.fd >= 0) close(audio->wire.fd);
    for (size_t i = 0; i < audio->wire.n_fds; i++) {
        if (audio->wire.fds[i] >= 0) close(audio->wire.fds[i]);
    }
    pww_unmap_all(&audio->wire);
    pww_mem_clear(&audio->wire);
    free(audio->wire.in);
    memset(&audio->wire, 0, sizeof(audio->wire));
    audio->wire.fd = -1;

    free(audio->sinks);
    audio->sinks = NULL;
    audio->n_sinks = 0;
    audio->cap_sinks = 0;

    free(audio->devices);
    audio->devices = NULL;
    audio->n_devices = 0;
    audio->cap_devices = 0;

    free(audio->proxy_used);
    audio->proxy_used = NULL;
    audio->proxy_cap = 0;

    free(audio->free_ids);
    audio->free_ids = NULL;
    audio->n_free = 0;
    audio->free_cap = 0;

    audio->next_proxy = AUDIO_FIRST_PROXY;
    audio->sync_counter = 0;
    audio->sync_target = 0;
    audio->synced = 0;

    audio->meta_global = SPA_ID_INVALID;
    audio->meta_proxy = 0;
    audio->default_sink[0] = '\0';
    audio->configured_sink[0] = '\0';

    audio->target_sink_global = SPA_ID_INVALID;
    audio->sink_proxy = 0;
    audio->target_device_global = SPA_ID_INVALID;
    audio->device_proxy = 0;
    audio->target_card_profile_device = -1;
    audio->node_props.valid = 0;
    audio->route_props.valid = 0;

    audio_publish(audio, 0, 1.0, 0);
    audio->retry_due_usec = schedule_retry ? monotonic_usec() + AUDIO_RETRY_USEC : 0;
}

static int audio_connect(struct tomoe_audio *audio) {
    if (audio->wire.fd >= 0) return 0;
    int rc = pww_connect(&audio->wire);
    if (rc < 0) {
        audio->retry_due_usec = monotonic_usec() + AUDIO_RETRY_USEC;
        return rc;
    }
    audio->next_proxy = AUDIO_FIRST_PROXY;
    audio->meta_global = SPA_ID_INVALID;
    audio->target_sink_global = SPA_ID_INVALID;
    audio->target_device_global = SPA_ID_INVALID;
    audio->target_card_profile_device = -1;
    if (send_hello(audio) < 0 || send_client_props(audio) < 0 ||
        send_get_registry(audio) < 0 || send_sync(audio, &audio->sync_target) < 0) {
        audio_disconnect(audio, 1);
        return -EIO;
    }
    audio->retry_due_usec = 0;
    return 0;
}

static char *snapshot_build(const struct tomoe_audio *audio) {
    char vol[64];
    int n = snprintf(vol, sizeof(vol), "%.17g", audio->volume);
    if (n < 0 || (size_t)n >= sizeof(vol)) return NULL;
    for (int i = 0; i < n; i++) {
        if (vol[i] == 'e' || vol[i] == 'E') vol[i] = 'd';
    }
    if (!strchr(vol, 'd')) {
        if ((size_t)n + 2U >= sizeof(vol)) return NULL;
        vol[n++] = 'd';
        vol[n++] = '0';
        vol[n] = '\0';
    }
    char buf[160];
    int total = snprintf(buf, sizeof(buf), "(:available %s :volume %s :muted %s)",
                         audio->available ? "t" : "nil", vol, audio->muted ? "t" : "nil");
    if (total < 0 || (size_t)total >= sizeof(buf)) return NULL;
    char *copy = malloc((size_t)total + 1U);
    if (!copy) return NULL;
    memcpy(copy, buf, (size_t)total + 1U);
    return copy;
}

struct tomoe_audio *tomoe_audio_open(int *error) {
    if (!error) return NULL;
    struct tomoe_audio *audio = calloc(1, sizeof(*audio));
    if (!audio) {
        *error = ENOMEM;
        return NULL;
    }
    audio->wire.fd = -1;
    audio->next_proxy = AUDIO_FIRST_PROXY;
    audio->meta_global = SPA_ID_INVALID;
    audio->target_sink_global = SPA_ID_INVALID;
    audio->target_device_global = SPA_ID_INVALID;
    audio->target_card_profile_device = -1;
    audio->volume = 1.0;
    (void)audio_connect(audio);
    *error = 0;
    return audio;
}

int tomoe_audio_poll(struct tomoe_audio *audio) {
    if (!audio) return -EINVAL;
    audio->changed = 0;
    if (audio->wire.fd < 0) {
        uint64_t now = monotonic_usec();
        if (audio->retry_due_usec && now < audio->retry_due_usec) return 0;
        if (audio_connect(audio) < 0) return 0;
    }
    int rc = pww_recv(&audio->wire, audio_dispatch, audio);
    if (rc < 0) {
        audio_disconnect(audio, 1);
    }
    return audio->changed ? 1 : 0;
}

const char *tomoe_audio_snapshot(struct tomoe_audio *audio) {
    if (!audio) return NULL;
    char *snapshot = snapshot_build(audio);
    if (!snapshot) return NULL;
    free(audio->snapshot);
    audio->snapshot = snapshot;
    return snapshot;
}

int tomoe_audio_fd(const struct tomoe_audio *audio) {
    return audio ? audio->wire.fd : -1;
}

unsigned tomoe_audio_mask(const struct tomoe_audio *audio) {
    (void)audio;
    return 1U;
}

int tomoe_audio_timeout(const struct tomoe_audio *audio, int max_ms) {
    if (!audio || audio->wire.fd >= 0 || !audio->retry_due_usec) return max_ms;
    uint64_t now = monotonic_usec();
    uint64_t rem_ms = 0;
    if (audio->retry_due_usec > now) {
        rem_ms = (audio->retry_due_usec - now + 999ULL) / 1000ULL;
    }
    if (rem_ms > (uint64_t)INT32_MAX) rem_ms = (uint64_t)INT32_MAX;
    int ms = (int)rem_ms;
    if (max_ms >= 0 && max_ms < ms) return max_ms;
    return ms;
}

void tomoe_audio_close(struct tomoe_audio *audio) {
    if (!audio) return;
    audio_disconnect(audio, 0);
    free(audio->snapshot);
    free(audio);
}
