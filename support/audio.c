#define _GNU_SOURCE

#include "audio.h"
#include "pipewire.h"

#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define AUDIO_RETRY_MIN_USEC 1000000ULL
#define AUDIO_RETRY_MAX_USEC 30000000ULL
#define AUDIO_POLL_MESSAGES 64U
#define AUDIO_POLL_BYTES 65536U
#define AUDIO_NAME_MAX 256U
#define AUDIO_MAX_DICT 1024U
#define AUDIO_MAX_SINKS 256U
#define AUDIO_MAX_DEVICES 256U
#define AUDIO_REGISTRY_PROXY 2U
#define AUDIO_FIRST_PROXY 3U
#define AUDIO_MAX_PROXY 63U

struct audio_props {
    int valid, mute, soft_mute;
    double volume;
};

struct audio_sink {
    uint32_t global_id, version, device_id;
    int32_t priority;
    char name[AUDIO_NAME_MAX];
};

struct audio_device {
    uint32_t global_id, version;
};

struct spa_prop_view {
    uint32_t key, type, size;
    const uint8_t *val, *pod;
};

struct audio_conn {
    int more_pending, synced;
    uint32_t sync_counter, sync_target;
    uint64_t proxy_mask;
    uint32_t proxy_cursor;
    struct audio_sink *sinks;
    size_t n_sinks, cap_sinks;
    struct audio_device *devices;
    size_t n_devices, cap_devices;
    uint32_t meta_global, meta_proxy;
    char default_sink[AUDIO_NAME_MAX], configured_sink[AUDIO_NAME_MAX];
    uint32_t target_sink_global, sink_proxy;
    uint32_t target_device_global, device_proxy;
    int32_t target_card_profile_device;
    struct audio_props node_props, route_props;
};

struct tomoe_audio {
    struct pww wire;
    struct audio_conn c;
    uint64_t generation, retry_delay_usec, retry_due_usec;
    int available, muted, changed;
    double volume;
    char snapshot[160];
};

static uint64_t monotonic_usec(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) < 0) return 0;
    if ((uint64_t)ts.tv_sec > UINT64_MAX / 1000000ULL) return UINT64_MAX;
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)(ts.tv_nsec / 1000L);
}

static void audio_schedule_retry(struct tomoe_audio *a) {
    uint64_t delay = a->retry_delay_usec ? a->retry_delay_usec : AUDIO_RETRY_MIN_USEC;
    uint64_t now = monotonic_usec();
    a->retry_due_usec = now > UINT64_MAX - delay ? UINT64_MAX : now + delay;
    a->retry_delay_usec = delay > AUDIO_RETRY_MAX_USEC / 2ULL ? AUDIO_RETRY_MAX_USEC : delay * 2ULL;
}

static double linear_to_cubic(float linear) {
    return !isfinite(linear) || linear <= 0.0f ? 0.0 : cbrt((double)linear);
}

static void audio_publish(struct tomoe_audio *a, int available, double volume, int muted) {
    if (!isfinite(volume) || volume < 0.0) volume = 0.0;
    muted = muted ? 1 : 0;
    available = available ? 1 : 0;
    if (a->available != available || a->muted != muted || fabs(a->volume - volume) > 1e-6) {
        a->available = available;
        a->volume = volume;
        a->muted = muted;
        a->changed = 1;
    }
}

static void audio_recompute(struct tomoe_audio *a) {
    const struct audio_props *p = NULL;
    if (a->c.sink_proxy && a->c.route_props.valid) p = &a->c.route_props;
    else if (a->c.sink_proxy && a->c.node_props.valid) p = &a->c.node_props;
    if (p) audio_publish(a, 1, p->volume, p->mute || p->soft_mute);
    else audio_publish(a, 0, 1.0, 0);
}

static uint32_t proxy_alloc(struct tomoe_audio *a) {
    uint32_t span = AUDIO_MAX_PROXY - AUDIO_FIRST_PROXY + 1U;
    for (uint32_t i = 0; i < span; i++) {
        uint32_t id = AUDIO_FIRST_PROXY + ((a->c.proxy_cursor + i) % span);
        if (!(a->c.proxy_mask & (1ULL << id))) {
            a->c.proxy_mask |= (1ULL << id);
            a->c.proxy_cursor = (id - AUDIO_FIRST_PROXY + 1U) % span;
            return id;
        }
    }
    return 0;
}

static void proxy_release(struct tomoe_audio *a, uint32_t id) {
    if (id >= AUDIO_FIRST_PROXY && id <= AUDIO_MAX_PROXY) a->c.proxy_mask &= ~(1ULL << id);
}

static int send_ints(struct tomoe_audio *a, uint32_t obj, uint32_t op, const int32_t *v, size_t n) {
    uint8_t storage[128];
    struct pww_build b;
    struct pww_frame root;
    pww_build_init(&b, storage, sizeof(storage));
    pww_pod_struct_begin(&b, &root);
    for (size_t i = 0; i < n; i++) pww_pod_int(&b, v[i]);
    return pww_message_send_nb(&a->wire, &b, &root, obj, op);
}

static int send_sync(struct tomoe_audio *a, uint32_t *seq_out) {
    uint32_t seq = 0x40000000u | (++a->c.sync_counter & 0x3fffffffu);
    int32_t v[2] = { 0, (int32_t)seq };
    if (seq_out) *seq_out = seq;
    return send_ints(a, 0, PW_CORE_METHOD_SYNC, v, 2U);
}

static int send_destroy(struct tomoe_audio *a, uint32_t proxy_id) {
    int32_t v = (int32_t)proxy_id;
    return send_ints(a, 0, PW_CORE_METHOD_DESTROY, &v, 1U);
}

static int send_bind(struct tomoe_audio *a, uint32_t global_id, const char *type,
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
    return pww_message_send_nb(&a->wire, &b, &root, AUDIO_REGISTRY_PROXY, PW_REGISTRY_METHOD_BIND);
}

static int send_subscribe(struct tomoe_audio *a, uint32_t proxy_id, uint32_t op, uint32_t param) {
    uint8_t storage[128];
    struct pww_build b;
    struct pww_frame root;
    uint32_t ids[1] = { param };
    pww_build_init(&b, storage, sizeof(storage));
    pww_pod_struct_begin(&b, &root);
    pww_pod_array_ids(&b, ids, 1);
    return pww_message_send_nb(&a->wire, &b, &root, proxy_id, op);
}

static int parse_opt_string(struct pww_parse *p, const char **str, size_t *len) {
    uint32_t type = 0, body = 0;
    const uint8_t *pod = pww_parse_pod(p, &type, &body);
    if (!pod) return -1;
    if (type == SPA_TYPE_NONE) { *str = NULL; *len = 0; return 0; }
    if (type != SPA_TYPE_STRING) return -1;
    const char *ptr = (const char *)(pod + 8);
    size_t n = body;
    while (n > 0 && ptr[n - 1] == '\0') n--;
    *str = ptr;
    *len = n;
    return 0;
}

static int parse_string_slice(struct pww_parse *p, const char **str, size_t *len) {
    return parse_opt_string(p, str, len) < 0 || !*str ? -1 : 0;
}

static int parse_dict_begin(struct pww_parse *p, struct pww_parse *q, uint32_t *count) {
    uint32_t type = 0, body = 0;
    const uint8_t *pod = pww_parse_pod(p, &type, &body);
    if (!pod || type != SPA_TYPE_STRUCT) return -EPROTO;
    pww_parse_init(q, pod + 8U, body);
    return pww_parse_int(q, count) || *count > AUDIO_MAX_DICT ? -EPROTO : 0;
}

static int span_eq(const char *s, size_t len, const char *lit) {
    size_t l = strlen(lit);
    return len == l && memcmp(s, lit, l) == 0;
}

static void span_copy(char *dst, size_t cap, const char *src, size_t len) {
    if (!cap) return;
    if (len >= cap) len = cap - 1U;
    if (len) memcpy(dst, src, len);
    dst[len] = '\0';
}

static int span_parse_uint(const char *s, size_t len, uint32_t limit, uint32_t *out) {
    if (!len) return 0;
    uint32_t v = 0;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c < '0' || c > '9') return 0;
        uint32_t d = (uint32_t)(c - '0');
        if (v > (limit - d) / 10U) return 0;
        v = v * 10U + d;
    }
    *out = v;
    return 1;
}

static int span_parse_u32(const char *s, size_t len, uint32_t *out) {
    return span_parse_uint(s, len, UINT32_MAX, out);
}

static int span_parse_i32(const char *s, size_t len, int32_t *out) {
    if (!len) return 0;
    int neg = s[0] == '-';
    uint32_t limit = neg ? (uint32_t)INT32_MAX + 1U : (uint32_t)INT32_MAX, v = 0;
    if (!span_parse_uint(s + neg, len - (size_t)neg, limit, &v)) return 0;
    *out = neg ? (v == limit ? INT32_MIN : -(int32_t)v) : (int32_t)v;
    return 1;
}

static int json_hex4(const char *s, size_t len, size_t i, uint32_t *out) {
    if (i + 4U > len) return 0;
    uint32_t v = 0;
    for (size_t k = 0; k < 4U; k++) {
        char c = s[i + k];
        int d = c >= '0' && c <= '9' ? c - '0'
              : c >= 'a' && c <= 'f' ? c - 'a' + 10
              : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
        if (d < 0) return 0;
        v = (v << 4) | (uint32_t)d;
    }
    *out = v;
    return 1;
}

static int json_read_string(const char *s, size_t len, size_t *idx,
                            char *out, size_t cap, size_t *out_len) {
    size_t i = *idx, pos = 0;
    if (i >= len || s[i++] != '"') return 0;
    while (i < len && s[i] != '"') {
        unsigned char uc = (unsigned char)s[i++];
        if (uc < 0x20U) return 0;
        if (uc != '\\') {
            if (out && pos + 1U < cap) out[pos++] = (char)uc;
            continue;
        }
        if (i >= len) return 0;
        char esc = s[i++];
        uint32_t cp = 0;
        switch (esc) {
        case '"': case '\\': case '/': cp = (unsigned char)esc; break;
        case 'b': cp = '\b'; break;
        case 'f': cp = '\f'; break;
        case 'n': cp = '\n'; break;
        case 'r': cp = '\r'; break;
        case 't': cp = '\t'; break;
        case 'u':
            if (!json_hex4(s, len, i, &cp)) return 0;
            i += 4U;
            if (cp >= 0xD800U && cp <= 0xDBFFU) {
                uint32_t lo = 0;
                if (i + 6U > len || s[i] != '\\' || s[i + 1U] != 'u' ||
                    !json_hex4(s, len, i + 2U, &lo) || lo < 0xDC00U || lo > 0xDFFFU) return 0;
                i += 6U;
                cp = 0x10000U + (((cp - 0xD800U) << 10) | (lo - 0xDC00U));
            } else if (cp >= 0xDC00U && cp <= 0xDFFFU) {
                return 0;
            }
            break;
        default: return 0;
        }
        if (!out) continue;
        if (!cp || cp > 0x10FFFFU) return 0;
        char u[4];
        size_t n = 0;
        if (cp < 0x80U) {
            u[n++] = (char)cp;
        } else if (cp < 0x800U) {
            u[n++] = (char)(0xC0U | (cp >> 6));
            u[n++] = (char)(0x80U | (cp & 0x3FU));
        } else if (cp < 0x10000U) {
            u[n++] = (char)(0xE0U | (cp >> 12));
            u[n++] = (char)(0x80U | ((cp >> 6) & 0x3FU));
            u[n++] = (char)(0x80U | (cp & 0x3FU));
        } else {
            u[n++] = (char)(0xF0U | (cp >> 18));
            u[n++] = (char)(0x80U | ((cp >> 12) & 0x3FU));
            u[n++] = (char)(0x80U | ((cp >> 6) & 0x3FU));
            u[n++] = (char)(0x80U | (cp & 0x3FU));
        }
        if (pos + n < cap) { memcpy(out + pos, u, n); pos += n; }
    }
    if (i >= len) return 0;
    *idx = i + 1U;
    if (out && cap) out[pos] = '\0';
    if (out_len) *out_len = pos;
    return 1;
}

static int parse_json_name(const char *json, size_t len, char *out, size_t cap) {
    if (!json || !len || !cap) return 0;
    size_t i = 0;
    while (i < len && isspace((unsigned char)json[i])) i++;
    if (i >= len || json[i++] != '{') return 0;
    for (;;) {
        while (i < len && isspace((unsigned char)json[i])) i++;
        if (i >= len || json[i] == '}') return 0;
        char key[16];
        size_t key_len = 0;
        if (!json_read_string(json, len, &i, key, sizeof(key), &key_len)) return 0;
        while (i < len && isspace((unsigned char)json[i])) i++;
        if (i >= len || json[i++] != ':') return 0;
        while (i < len && isspace((unsigned char)json[i])) i++;
        if (key_len == 4U && memcmp(key, "name", 4U) == 0) {
            size_t val_len = 0;
            return json_read_string(json, len, &i, out, cap, &val_len) && val_len > 0;
        }
        if (i >= len) return 0;
        if (json[i] == '"') {
            if (!json_read_string(json, len, &i, NULL, 0, NULL)) return 0;
        } else if (json[i] == '{' || json[i] == '[') {
            size_t depth = 0;
            while (i < len) {
                if (json[i] == '"') {
                    if (!json_read_string(json, len, &i, NULL, 0, NULL)) return 0;
                    continue;
                }
                if (json[i] == '{' || json[i] == '[') depth++;
                else if ((json[i] == '}' || json[i] == ']') && --depth == 0) { i++; break; }
                i++;
            }
            if (depth != 0) return 0;
        } else {
            while (i < len && json[i] != ',' && json[i] != '}' && !isspace((unsigned char)json[i]))
                i++;
        }
        while (i < len && isspace((unsigned char)json[i])) i++;
        if (i >= len || json[i++] != ',') return 0;
    }
}

static int spa_prop_next(const uint8_t *pod, size_t end, size_t *off, struct spa_prop_view *out) {
    if (*off > end || end - *off < 16U) return 0;
    uint32_t key = 0, size = 0, type = 0;
    memcpy(&key, pod + *off, 4U);
    memcpy(&size, pod + *off + 8U, 4U);
    memcpy(&type, pod + *off + 12U, 4U);
    if ((size_t)size > end - *off - 16U) return 0;
    out->key = key;
    out->type = type;
    out->size = size;
    out->pod = pod + *off + 8U;
    out->val = pod + *off + 16U;
    size_t next = *off + 16U + (size_t)size, rem = next & 7U;
    if (rem) {
        size_t pad = 8U - rem;
        next = pad > end - next ? end : next + pad;
    }
    *off = next;
    return 1;
}

static int parse_bool_prop(const struct spa_prop_view *p, int *out) {
    if (p->type != SPA_TYPE_BOOL || p->size < 4U) return 0;
    uint32_t b = 0;
    memcpy(&b, p->val, 4U);
    *out = b ? 1 : 0;
    return 1;
}

static int parse_float_array_max(const struct spa_prop_view *p, float *out) {
    if (p->type != SPA_TYPE_ARRAY || p->size < 12U) return 0;
    uint32_t csize = 0, ctype = 0;
    memcpy(&csize, p->val, 4U);
    memcpy(&ctype, p->val + 4U, 4U);
    if (csize != 4U || ctype != SPA_TYPE_FLOAT) return 0;
    uint32_t count = (p->size - 8U) / 4U;
    int found = 0;
    float best = 0.0f;
    for (uint32_t i = 0; i < count; i++) {
        float v = 0.0f;
        memcpy(&v, p->val + 8U + (size_t)i * 4U, 4U);
        if (!isfinite(v) || v < 0.0f) continue;
        if (!found || v > best) best = v;
        found = 1;
    }
    if (!found) return 0;
    *out = best;
    return 1;
}

static int parse_props_pod(const uint8_t *pod, uint32_t body, struct audio_props *dst) {
    if (body < 8U) return 0;
    uint32_t obj_type = 0;
    memcpy(&obj_type, pod + 8U, 4U);
    if (obj_type != SPA_TYPE_OBJECT_PROPS) return 0;
    size_t end = 8U + (size_t)body, off = 16U;
    int have_vol = 0, have_ch = 0, have_soft_vol = 0, have_mute = 0, have_soft_mute = 0;
    int mute = 0, soft_mute = 0;
    float vol = 1.0f, ch = 1.0f, soft_vol = 1.0f;
    struct spa_prop_view prop;

    while (spa_prop_next(pod, end, &off, &prop)) {
        switch (prop.key) {
        case SPA_PROP_VOLUME:
            if (prop.type == SPA_TYPE_FLOAT && prop.size >= 4U) {
                float v = 0.0f;
                memcpy(&v, prop.val, 4U);
                if (isfinite(v) && v >= 0.0f) { vol = v; have_vol = 1; }
            }
            break;
        case SPA_PROP_MUTE:
            if (parse_bool_prop(&prop, &mute)) have_mute = 1;
            break;
        case SPA_PROP_CHANNELVOLUMES:
            if (parse_float_array_max(&prop, &ch)) have_ch = 1;
            break;
        case SPA_PROP_SOFTMUTE:
            if (parse_bool_prop(&prop, &soft_mute)) have_soft_mute = 1;
            break;
        case SPA_PROP_SOFTVOLUMES:
            if (parse_float_array_max(&prop, &soft_vol)) have_soft_vol = 1;
            break;
        default: break;
        }
    }
    if (!have_vol && !have_ch && !have_soft_vol && !have_mute && !have_soft_mute) return 0;
    if (!dst->valid) { dst->volume = 1.0; dst->mute = 0; dst->soft_mute = 0; dst->valid = 1; }
    if (have_ch) dst->volume = linear_to_cubic(ch);
    else if (have_vol) dst->volume = linear_to_cubic(vol);
    else if (have_soft_vol) dst->volume = linear_to_cubic(soft_vol);
    if (have_mute) dst->mute = mute;
    if (have_soft_mute) dst->soft_mute = soft_mute;
    return 1;
}

static int parse_route_pod(const uint8_t *pod, uint32_t body, int32_t want_device,
                           struct audio_props *dst) {
    if (body < 8U || want_device < 0) return 0;
    uint32_t obj_type = 0;
    memcpy(&obj_type, pod + 8U, 4U);
    if (obj_type != SPA_TYPE_OBJECT_PARAMROUTE) return 0;
    size_t end = 8U + (size_t)body, off = 16U;
    int have_dir = 0, have_dev = 0;
    uint32_t dir = 0, props_body = 0;
    int32_t dev = -1;
    const uint8_t *props_pod = NULL;
    struct spa_prop_view prop;

    while (spa_prop_next(pod, end, &off, &prop)) {
        if (prop.key == SPA_PARAM_ROUTE_DIRECTION && prop.type == SPA_TYPE_ID && prop.size >= 4U) {
            memcpy(&dir, prop.val, 4U);
            have_dir = 1;
        } else if (prop.key == SPA_PARAM_ROUTE_DEVICE && prop.type == SPA_TYPE_INT &&
                   prop.size >= 4U) {
            memcpy(&dev, prop.val, 4U);
            have_dev = 1;
        } else if (prop.key == SPA_PARAM_ROUTE_PROPS && prop.type == SPA_TYPE_OBJECT &&
                   prop.size >= 8U) {
            props_pod = prop.pod;
            props_body = prop.size;
        }
    }
    if (!have_dir || dir != SPA_DIRECTION_OUTPUT || !have_dev || dev != want_device || !props_pod)
        return 0;
    return parse_props_pod(props_pod, props_body, dst);
}

static int vec_grow(void **ptr, size_t *cap, size_t elem_size, size_t max_elems) {
    size_t next = *cap ? *cap * 2U : 16U;
    if (next > max_elems) next = max_elems;
    if (next > SIZE_MAX / elem_size) return -ENOMEM;
    void *grown = realloc(*ptr, next * elem_size);
    if (!grown) return -ENOMEM;
    *ptr = grown;
    *cap = next;
    return 0;
}

static struct audio_device *find_device(struct tomoe_audio *a, uint32_t global_id) {
    for (size_t i = 0; i < a->c.n_devices; i++)
        if (a->c.devices[i].global_id == global_id) return &a->c.devices[i];
    return NULL;
}

static void remove_sink_global(struct tomoe_audio *a, uint32_t global_id) {
    for (size_t i = 0; i < a->c.n_sinks; i++) {
        if (a->c.sinks[i].global_id != global_id) continue;
        a->c.sinks[i] = a->c.sinks[--a->c.n_sinks];
        return;
    }
}

static void remove_device_global(struct tomoe_audio *a, uint32_t global_id) {
    for (size_t i = 0; i < a->c.n_devices; i++) {
        if (a->c.devices[i].global_id != global_id) continue;
        a->c.devices[i] = a->c.devices[--a->c.n_devices];
        return;
    }
}

static int upsert_device(struct tomoe_audio *a, uint32_t global_id, uint32_t version) {
    struct audio_device *existing = find_device(a, global_id);
    if (existing) { existing->version = version; return 0; }
    if (a->c.n_devices >= AUDIO_MAX_DEVICES) return 0;
    if (a->c.n_devices == a->c.cap_devices) {
        int rc = vec_grow((void **)&a->c.devices, &a->c.cap_devices,
                          sizeof(*a->c.devices), AUDIO_MAX_DEVICES);
        if (rc < 0) return rc;
    }
    a->c.devices[a->c.n_devices++] = (struct audio_device){ global_id, version };
    return 0;
}

static int upsert_sink(struct tomoe_audio *a, uint32_t global_id, uint32_t version,
                       const char *name, uint32_t device_id, int32_t priority) {
    struct audio_sink *slot = NULL;
    for (size_t i = 0; i < a->c.n_sinks; i++) {
        if (a->c.sinks[i].global_id == global_id) { slot = &a->c.sinks[i]; break; }
    }
    if (!slot) {
        if (a->c.n_sinks >= AUDIO_MAX_SINKS) return 0;
        if (a->c.n_sinks == a->c.cap_sinks) {
            int rc = vec_grow((void **)&a->c.sinks, &a->c.cap_sinks,
                              sizeof(*a->c.sinks), AUDIO_MAX_SINKS);
            if (rc < 0) return rc;
        }
        slot = &a->c.sinks[a->c.n_sinks++];
    }
    slot->global_id = global_id;
    slot->version = version;
    slot->device_id = device_id;
    slot->priority = priority;
    snprintf(slot->name, sizeof(slot->name), "%s", name);
    return 0;
}

static int unbind_device(struct tomoe_audio *a) {
    a->c.route_props.valid = 0;
    if (!a->c.device_proxy) return 0;
    uint32_t proxy = a->c.device_proxy;
    a->c.device_proxy = 0;
    return send_destroy(a, proxy);
}

static int bind_target_device(struct tomoe_audio *a) {
    if (a->c.device_proxy || a->c.target_device_global == SPA_ID_INVALID ||
        a->c.target_card_profile_device < 0)
        return 0;
    struct audio_device *dev = find_device(a, a->c.target_device_global);
    if (!dev) return 0;
    uint32_t proxy = proxy_alloc(a);
    if (!proxy) return -ENOSPC;
    uint32_t ver = dev->version < 3U ? dev->version : 3U;
    int rc = send_bind(a, dev->global_id, "PipeWire:Interface:Device", ver, proxy);
    if (rc < 0) return rc;
    a->c.device_proxy = proxy;
    return send_subscribe(a, proxy, PW_DEVICE_METHOD_SUBSCRIBE_PARAMS, SPA_PARAM_ROUTE);
}

static struct audio_sink *select_target_sink(struct tomoe_audio *a) {
    const char *pref[2] = { a->c.default_sink, a->c.configured_sink };
    for (int k = 0; k < 2; k++) {
        if (!pref[k][0]) continue;
        for (size_t i = 0; i < a->c.n_sinks; i++)
            if (strcmp(a->c.sinks[i].name, pref[k]) == 0) return &a->c.sinks[i];
    }
    struct audio_sink *best = NULL;
    for (size_t i = 0; i < a->c.n_sinks; i++) {
        struct audio_sink *s = &a->c.sinks[i];
        if (!best || s->priority > best->priority ||
            (s->priority == best->priority && s->global_id < best->global_id))
            best = s;
    }
    return best;
}

static int resolve_target_sink(struct tomoe_audio *a) {
    struct audio_sink *wanted = select_target_sink(a);
    uint32_t wanted_id = wanted ? wanted->global_id : SPA_ID_INVALID;
    if (wanted_id == a->c.target_sink_global && (wanted_id == SPA_ID_INVALID || a->c.sink_proxy))
        return 0;
    int rc = unbind_device(a);
    if (rc < 0) return rc;
    if (a->c.sink_proxy) {
        uint32_t old = a->c.sink_proxy;
        a->c.sink_proxy = 0;
        rc = send_destroy(a, old);
        if (rc < 0) return rc;
    }
    a->c.target_sink_global = SPA_ID_INVALID;
    a->c.target_device_global = SPA_ID_INVALID;
    a->c.target_card_profile_device = -1;
    a->c.node_props.valid = 0;
    a->c.route_props.valid = 0;
    audio_recompute(a);
    if (!wanted) return 0;
    uint32_t proxy = proxy_alloc(a);
    if (!proxy) return -ENOSPC;
    uint32_t ver = wanted->version < 3U ? wanted->version : 3U;
    rc = send_bind(a, wanted->global_id, "PipeWire:Interface:Node", ver, proxy);
    if (rc < 0) return rc;
    a->c.target_sink_global = wanted->global_id;
    a->c.target_device_global = wanted->device_id;
    a->c.sink_proxy = proxy;
    return send_subscribe(a, proxy, PW_NODE_METHOD_SUBSCRIBE_PARAMS, SPA_PARAM_PROPS);
}

static int handle_registry_global(struct tomoe_audio *a, struct pww_parse *p) {
    uint32_t global_id = 0, perms = 0, version = 0, count = 0;
    const char *type = NULL;
    size_t type_len = 0;
    struct pww_parse q;
    if (pww_parse_int(p, &global_id) || pww_parse_int(p, &perms) ||
        parse_string_slice(p, &type, &type_len) || pww_parse_int(p, &version) ||
        parse_dict_begin(p, &q, &count))
        return -EPROTO;
    int is_meta = span_eq(type, type_len, "PipeWire:Interface:Metadata");
    int is_node = span_eq(type, type_len, "PipeWire:Interface:Node");
    int is_device = span_eq(type, type_len, "PipeWire:Interface:Device");
    if (!is_meta && !is_node && !is_device) return 0;

    int meta_is_default = 0, node_is_sink = 0;
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
            if (span_eq(k, klen, "media.class") && span_eq(v, vlen, "Audio/Sink"))
                node_is_sink = 1;
            else if (span_eq(k, klen, "node.name"))
                span_copy(node_name, sizeof(node_name), v, vlen);
            else if (span_eq(k, klen, "device.id"))
                (void)span_parse_u32(v, vlen, &device_id);
            else if (span_eq(k, klen, "priority.session"))
                (void)span_parse_i32(v, vlen, &priority);
        }
    }

    if (is_meta && meta_is_default && !a->c.meta_proxy) {
        uint32_t proxy = proxy_alloc(a);
        if (!proxy) return -ENOSPC;
        uint32_t ver = version < 3U ? version : 3U;
        int rc = send_bind(a, global_id, "PipeWire:Interface:Metadata", ver, proxy);
        if (rc < 0) return rc;
        a->c.meta_global = global_id;
        a->c.meta_proxy = proxy;
        if (!a->c.synced) return send_sync(a, &a->c.sync_target);
    } else if (is_node && node_is_sink && node_name[0]) {
        int rc = upsert_sink(a, global_id, version, node_name, device_id, priority);
        if (rc < 0) return rc;
        if (a->c.synced) return resolve_target_sink(a);
    } else if (is_device) {
        int rc = upsert_device(a, global_id, version);
        if (rc < 0) return rc;
        if (a->c.synced && a->c.target_device_global == global_id &&
            a->c.target_card_profile_device >= 0 && !a->c.device_proxy)
            return bind_target_device(a);
    }
    return 0;
}

static int handle_registry_remove(struct tomoe_audio *a, struct pww_parse *p) {
    uint32_t global_id = 0;
    if (pww_parse_int(p, &global_id)) return -EPROTO;
    remove_sink_global(a, global_id);
    remove_device_global(a, global_id);
    if (a->c.meta_global == global_id) {
        a->c.meta_global = SPA_ID_INVALID;
        if (a->c.meta_proxy) {
            uint32_t proxy = a->c.meta_proxy;
            a->c.meta_proxy = 0;
            int rc = send_destroy(a, proxy);
            if (rc < 0) return rc;
        }
        a->c.default_sink[0] = '\0';
        a->c.configured_sink[0] = '\0';
        return a->c.synced ? resolve_target_sink(a) : 0;
    }
    if (a->c.target_sink_global == global_id)
        return a->c.synced ? resolve_target_sink(a) : 0;
    if (a->c.target_device_global == global_id) {
        a->c.target_device_global = SPA_ID_INVALID;
        int rc = unbind_device(a);
        audio_recompute(a);
        return rc;
    }
    return 0;
}

static int handle_metadata_property(struct tomoe_audio *a, struct pww_parse *p) {
    uint32_t subject = 0;
    const char *key = NULL, *type = NULL, *value = NULL;
    size_t key_len = 0, type_len = 0, value_len = 0;
    if (pww_parse_int(p, &subject) || parse_opt_string(p, &key, &key_len) ||
        parse_opt_string(p, &type, &type_len) || parse_opt_string(p, &value, &value_len))
        return -EPROTO;
    if (subject != 0) return 0;
    int relevant = 0;
    if (!key) {
        a->c.default_sink[0] = '\0';
        a->c.configured_sink[0] = '\0';
        relevant = 1;
    } else if (span_eq(key, key_len, "default.audio.sink")) {
        if (!value || !parse_json_name(value, value_len, a->c.default_sink, sizeof(a->c.default_sink)))
            a->c.default_sink[0] = '\0';
        relevant = 1;
    } else if (span_eq(key, key_len, "default.configured.audio.sink")) {
        if (!value || !parse_json_name(value, value_len, a->c.configured_sink, sizeof(a->c.configured_sink)))
            a->c.configured_sink[0] = '\0';
        relevant = 1;
    }
    return relevant && a->c.synced ? resolve_target_sink(a) : 0;
}

static int handle_node_info(struct tomoe_audio *a, struct pww_parse *p) {
    uint32_t id, max_in, max_out, n_in, n_out, state, count = 0;
    int64_t change_mask = 0;
    const char *error = NULL;
    size_t error_len = 0;
    struct pww_parse q;
    if (pww_parse_int(p, &id) || pww_parse_int(p, &max_in) || pww_parse_int(p, &max_out) ||
        pww_parse_typed(p, SPA_TYPE_LONG, &change_mask, 8U) || pww_parse_int(p, &n_in) ||
        pww_parse_int(p, &n_out) || pww_parse_id(p, &state) ||
        parse_opt_string(p, &error, &error_len) || parse_dict_begin(p, &q, &count))
        return -EPROTO;

    uint32_t dev_id = SPA_ID_INVALID;
    int have_dev = 0, have_cpd = 0, changed = 0;
    int32_t cpd = -1;
    for (uint32_t i = 0; i < count; i++) {
        const char *k = NULL, *v = NULL;
        size_t klen = 0, vlen = 0;
        if (parse_string_slice(&q, &k, &klen) || parse_opt_string(&q, &v, &vlen)) return -EPROTO;
        if (!v) continue;
        if (span_eq(k, klen, "device.id") && span_parse_u32(v, vlen, &dev_id)) have_dev = 1;
        else if (span_eq(k, klen, "card.profile.device") && span_parse_i32(v, vlen, &cpd))
            have_cpd = 1;
    }
    if (have_dev && dev_id != a->c.target_device_global) {
        int rc = unbind_device(a);
        if (rc < 0) return rc;
        a->c.target_device_global = dev_id;
        changed = 1;
    }
    if (have_cpd && cpd != a->c.target_card_profile_device) {
        a->c.target_card_profile_device = cpd;
        a->c.route_props.valid = 0;
        if (cpd < 0 && a->c.device_proxy) {
            int rc = unbind_device(a);
            if (rc < 0) return rc;
        }
        changed = 1;
    }
    if (changed) audio_recompute(a);
    if (a->c.target_device_global != SPA_ID_INVALID && a->c.target_card_profile_device >= 0) {
        if (!a->c.device_proxy) return bind_target_device(a);
        if (changed)
            return send_subscribe(a, a->c.device_proxy, PW_DEVICE_METHOD_SUBSCRIBE_PARAMS,
                                  SPA_PARAM_ROUTE);
    }
    return 0;
}

static int handle_param(struct tomoe_audio *a, struct pww_parse *p, int is_device) {
    uint32_t seq = 0, param_id = 0, index = 0, next = 0, type = 0, body = 0;
    if (pww_parse_int(p, &seq) || pww_parse_id(p, &param_id) ||
        pww_parse_int(p, &index) || pww_parse_int(p, &next))
        return -EPROTO;
    const uint8_t *pod = pww_parse_pod(p, &type, &body);
    if (!pod) return -EPROTO;
    if (type != SPA_TYPE_OBJECT) return 0;
    int updated = 0;
    if (!is_device && param_id == SPA_PARAM_PROPS)
        updated = parse_props_pod(pod, body, &a->c.node_props);
    else if (is_device && param_id == SPA_PARAM_ROUTE)
        updated = parse_route_pod(pod, body, a->c.target_card_profile_device, &a->c.route_props);
    if (updated) audio_recompute(a);
    return 0;
}

static int handle_proxy_gone(struct tomoe_audio *a, uint32_t id) {
    proxy_release(a, id);
    if (id == a->c.meta_proxy) {
        a->c.meta_proxy = 0;
        a->c.meta_global = SPA_ID_INVALID;
        a->c.default_sink[0] = '\0';
        a->c.configured_sink[0] = '\0';
        if (a->c.synced) return resolve_target_sink(a);
    } else if (id == a->c.sink_proxy) {
        if (a->c.target_sink_global != SPA_ID_INVALID)
            remove_sink_global(a, a->c.target_sink_global);
        a->c.sink_proxy = 0;
        a->c.target_sink_global = SPA_ID_INVALID;
        a->c.target_device_global = SPA_ID_INVALID;
        a->c.target_card_profile_device = -1;
        a->c.node_props.valid = 0;
        int rc = unbind_device(a);
        if (rc < 0) return rc;
        if (a->c.synced) return resolve_target_sink(a);
        audio_recompute(a);
    } else if (id == a->c.device_proxy) {
        if (a->c.target_device_global != SPA_ID_INVALID)
            remove_device_global(a, a->c.target_device_global);
        a->c.device_proxy = 0;
        a->c.route_props.valid = 0;
        audio_recompute(a);
    }
    return 0;
}

static int audio_dispatch(void *data, uint32_t id, uint32_t opcode, struct pww_parse *p) {
    struct tomoe_audio *a = data;
    if (id == 0) {
        switch (opcode) {
        case PW_CORE_EVENT_PING: {
            uint32_t target = 0, seq = 0;
            if (pww_parse_int(p, &target) || pww_parse_int(p, &seq)) return -EPROTO;
            int32_t v[2] = { (int32_t)target, (int32_t)seq };
            return send_ints(a, 0, PW_CORE_METHOD_PONG, v, 2U);
        }
        case PW_CORE_EVENT_DONE: {
            uint32_t target = 0, seq = 0;
            if (pww_parse_int(p, &target) || pww_parse_int(p, &seq)) return -EPROTO;
            if (!a->c.synced && target == 0 && seq == a->c.sync_target) {
                a->c.synced = 1;
                a->retry_delay_usec = AUDIO_RETRY_MIN_USEC;
                return resolve_target_sink(a);
            }
            return 0;
        }
        case PW_CORE_EVENT_REMOVE_ID: {
            uint32_t removed = 0;
            if (pww_parse_int(p, &removed)) return -EPROTO;
            if (removed == AUDIO_REGISTRY_PROXY) return -EPROTO;
            return removed < AUDIO_FIRST_PROXY ? 0 : handle_proxy_gone(a, removed);
        }
        case PW_CORE_EVENT_ERROR: {
            uint32_t target = 0, seq = 0, res = 0;
            if (pww_parse_int(p, &target) || pww_parse_int(p, &seq) || pww_parse_int(p, &res))
                return -EPROTO;
            if (target == 0 || target == AUDIO_REGISTRY_PROXY) return -EPROTO;
            return target < AUDIO_FIRST_PROXY ? 0 : handle_proxy_gone(a, target);
        }
        default: return 0;
        }
    }
    if (id == AUDIO_REGISTRY_PROXY) {
        if (opcode == PW_REGISTRY_EVENT_GLOBAL) return handle_registry_global(a, p);
        if (opcode == PW_REGISTRY_EVENT_GLOBAL_REMOVE) return handle_registry_remove(a, p);
        return 0;
    }
    if (a->c.meta_proxy && id == a->c.meta_proxy)
        return opcode == PW_METADATA_EVENT_PROPERTY ? handle_metadata_property(a, p) : 0;
    if (a->c.sink_proxy && id == a->c.sink_proxy) {
        if (opcode == PW_NODE_EVENT_INFO) return handle_node_info(a, p);
        if (opcode == PW_NODE_EVENT_PARAM) return handle_param(a, p, 0);
        return 0;
    }
    if (a->c.device_proxy && id == a->c.device_proxy)
        return opcode == PW_DEVICE_EVENT_PARAM ? handle_param(a, p, 1) : 0;
    return 0;
}

static void audio_reset_conn(struct tomoe_audio *a) {
    free(a->c.sinks);
    free(a->c.devices);
    memset(&a->c, 0, sizeof(a->c));
    a->c.meta_global = SPA_ID_INVALID;
    a->c.target_sink_global = SPA_ID_INVALID;
    a->c.target_device_global = SPA_ID_INVALID;
    a->c.target_card_profile_device = -1;
}

static void audio_disconnect(struct tomoe_audio *a, int schedule_retry) {
    if (a->wire.fd >= 0) close(a->wire.fd);
    for (size_t i = 0; i < a->wire.n_fds; i++)
        if (a->wire.fds[i] >= 0) close(a->wire.fds[i]);
    free(a->wire.in);
    free(a->wire.out);
    memset(&a->wire, 0, sizeof(a->wire));
    a->wire.fd = -1;
    audio_reset_conn(a);
    audio_publish(a, 0, 1.0, 0);
    if (schedule_retry) audio_schedule_retry(a);
    else a->retry_due_usec = 0;
}

static int audio_connect(struct tomoe_audio *a) {
    if (a->wire.fd >= 0) return 0;
    int rc = pww_connect_nb(&a->wire);
    if (rc < 0) {
        audio_schedule_retry(a);
        return rc;
    }
    a->generation++;
    audio_reset_conn(a);

    int32_t hello = 4, reg[2] = { 3, (int32_t)AUDIO_REGISTRY_PROXY };
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

    if (send_ints(a, 0, PW_CORE_METHOD_HELLO, &hello, 1U) < 0 ||
        pww_message_send_nb(&a->wire, &b, &root, 1, PW_CLIENT_METHOD_UPDATE_PROPERTIES) < 0 ||
        send_ints(a, 0, PW_CORE_METHOD_GET_REGISTRY, reg, 2U) < 0 ||
        send_sync(a, &a->c.sync_target) < 0) {
        audio_disconnect(a, 1);
        return -EIO;
    }
    a->retry_due_usec = 0;
    return 0;
}

struct tomoe_audio *tomoe_audio_open(int *error) {
    if (!error) return NULL;
    struct tomoe_audio *a = calloc(1, sizeof(*a));
    if (!a) { *error = ENOMEM; return NULL; }
    a->wire.fd = -1;
    a->retry_delay_usec = AUDIO_RETRY_MIN_USEC;
    a->volume = 1.0;
    audio_reset_conn(a);
    (void)audio_connect(a);
    *error = 0;
    return a;
}

int tomoe_audio_poll(struct tomoe_audio *a) {
    if (!a) return -EINVAL;
    a->changed = 0;
    a->c.more_pending = 0;
    if (a->wire.fd < 0) {
        uint64_t now = monotonic_usec();
        if (a->retry_due_usec && now < a->retry_due_usec) return 0;
        if (audio_connect(a) < 0) return 0;
    }
    if (pww_flush(&a->wire) < 0) {
        audio_disconnect(a, 1);
        return a->changed ? 1 : 0;
    }
    if (a->wire.connecting) return a->changed ? 1 : 0;
    int rc = pww_recv_budget(&a->wire, audio_dispatch, a, AUDIO_POLL_MESSAGES, AUDIO_POLL_BYTES);
    if (rc < 0) audio_disconnect(a, 1);
    else if (rc > 0) a->c.more_pending = 1;
    if (a->wire.fd >= 0 && a->wire.out_off < a->wire.out_len && pww_flush(&a->wire) < 0)
        audio_disconnect(a, 1);
    return a->changed ? 1 : 0;
}

const char *tomoe_audio_snapshot(struct tomoe_audio *a) {
    if (!a) return NULL;
    char vol[64];
    int n = snprintf(vol, sizeof(vol), "%.17g", a->volume);
    if (n < 0 || (size_t)n >= sizeof(vol)) return NULL;
    for (int i = 0; i < n; i++)
        if (vol[i] == 'e' || vol[i] == 'E') vol[i] = 'd';
    if (!strchr(vol, 'd')) {
        if ((size_t)n + 2U >= sizeof(vol)) return NULL;
        vol[n++] = 'd';
        vol[n++] = '0';
        vol[n] = '\0';
    }
    int total = snprintf(a->snapshot, sizeof(a->snapshot),
                         "(:available %s :volume %s :muted %s)",
                         a->available ? "t" : "nil", vol, a->muted ? "t" : "nil");
    return total < 0 || (size_t)total >= sizeof(a->snapshot) ? NULL : a->snapshot;
}

int tomoe_audio_fd(const struct tomoe_audio *a) { return a ? a->wire.fd : -1; }

unsigned tomoe_audio_mask(const struct tomoe_audio *a) {
    if (!a || a->wire.fd < 0) return 1U;
    return 1U | (a->wire.connecting || a->wire.out_off < a->wire.out_len ? 2U : 0U);
}

uint64_t tomoe_audio_generation(const struct tomoe_audio *a) { return a ? a->generation : 0; }

int tomoe_audio_timeout(const struct tomoe_audio *a, int max_ms) {
    if (!a) return max_ms;
    if (a->wire.fd >= 0) return a->c.more_pending ? 0 : max_ms;
    if (!a->retry_due_usec) return max_ms;
    uint64_t now = monotonic_usec();
    uint64_t rem = a->retry_due_usec > now ? (a->retry_due_usec - now + 999ULL) / 1000ULL : 0;
    int ms = rem > (uint64_t)INT32_MAX ? INT32_MAX : (int)rem;
    return max_ms >= 0 && max_ms < ms ? max_ms : ms;
}

void tomoe_audio_close(struct tomoe_audio *a) {
    if (!a) return;
    audio_disconnect(a, 0);
    free(a);
}
