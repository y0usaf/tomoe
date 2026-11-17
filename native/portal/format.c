#include "spa.h"

#include <string.h>

static uint32_t pod_u32(uint32_t type, uint32_t size, const uint8_t *value, uint32_t fallback) {
    if (type == SPA_TYPE_CHOICE) {
        if (size < 16) return fallback;
        type = 0;
        memcpy(&type, value + 12, 4);
        value += 16;
        size -= 16;
    }
    if ((type != SPA_TYPE_INT && type != SPA_TYPE_ID && type != SPA_TYPE_LONG &&
            type != SPA_TYPE_FLOAT) || size < 4) return fallback;
    uint32_t out = fallback;
    memcpy(&out, value, 4);
    return out;
}

static uint64_t pod_u64(uint32_t type, uint32_t size, const uint8_t *value, uint64_t fallback) {
    if (type == SPA_TYPE_CHOICE) {
        if (size < 16) return fallback;
        type = 0;
        memcpy(&type, value + 12, 4);
        value += 16;
        size -= 16;
    }
    if (type != SPA_TYPE_LONG || size < 8) return fallback;
    uint64_t out = fallback;
    memcpy(&out, value, 8);
    return out;
}

static bool pod_values(uint32_t type, uint32_t size, const uint8_t *value, uint32_t wanted,
        const uint8_t **out, uint32_t needed) {
    if (type == SPA_TYPE_CHOICE) {
        if (size < 16) return false;
        type = 0;
        memcpy(&type, value + 12, 4);
        value += 16;
        size -= 16;
    }
    if (type != wanted || size < needed) return false;
    *out = value;
    return true;
}

int spa_parse_video_format(const void *pod, size_t len, struct spa_video_info_raw *info) {
    struct pww_parse parse;
    pww_parse_init(&parse, pod, len);
    uint32_t type, body;
    const uint8_t *object = pww_parse_pod(&parse, &type, &body);
    if (!object || type != SPA_TYPE_OBJECT || body < 8) return -1;
    memset(info, 0, sizeof(*info));
    size_t end = 8 + (size_t)body;
    size_t offset = 16;
    while (offset + 16 <= end) {
        uint32_t key, flags, size, ptype;
        memcpy(&key, object + offset, 4);
        memcpy(&flags, object + offset + 4, 4);
        offset += 8;
        if (offset + 8 > end) break;
        memcpy(&size, object + offset, 4);
        memcpy(&ptype, object + offset + 4, 4);
        const uint8_t *value = object + offset + 8;
        if (offset + 8 + size > end) break;
        switch (key) {
        case SPA_FORMAT_VIDEO_FORMAT:
            info->format = pod_u32(ptype, size, value, info->format);
            break;
        case SPA_FORMAT_VIDEO_MODIFIER:
            info->modifier = pod_u64(ptype, size, value, info->modifier);
            info->flags |= SPA_VIDEO_FLAG_MODIFIER;
            break;
        case SPA_FORMAT_VIDEO_SIZE: {
            const uint8_t *values = NULL;
            if (pod_values(ptype, size, value, SPA_TYPE_RECTANGLE, &values, 8))
                memcpy(&info->size, values, 8);
            break;
        }
        case SPA_FORMAT_VIDEO_FRAMERATE: {
            const uint8_t *values = NULL;
            if (pod_values(ptype, size, value, SPA_TYPE_FRACTION, &values, 8))
                memcpy(&info->framerate, values, 8);
            break;
        }
        case SPA_FORMAT_VIDEO_MAXFRAMERATE: {
            const uint8_t *values = NULL;
            if (pod_values(ptype, size, value, SPA_TYPE_FRACTION, &values, 8))
                memcpy(&info->max_framerate, values, 8);
            break;
        }
        default:
            break;
        }
        offset += 8 + ((size + 7u) & ~7u);
    }
    return 0;
}
