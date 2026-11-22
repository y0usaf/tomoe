#ifndef TOMOE_PORTAL_SPA_H
#define TOMOE_PORTAL_SPA_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "pipewire.h"

struct spa_point {
    int32_t x;
    int32_t y;
};

struct spa_rectangle {
    uint32_t width;
    uint32_t height;
};

struct spa_fraction {
    uint32_t num;
    uint32_t denom;
};

struct spa_chunk {
    uint32_t offset;
    uint32_t size;
    int32_t stride;
    int32_t flags;
};

struct spa_data {
    uint32_t type;
    uint32_t flags;
    int64_t fd;
    uint32_t mapoffset;
    uint32_t maxsize;
    void *data;
    struct spa_chunk *chunk;
};

struct spa_meta {
    uint32_t type;
    uint32_t size;
    void *data;
};

struct spa_buffer {
    uint32_t n_metas;
    uint32_t n_datas;
    struct spa_meta *metas;
    struct spa_data *datas;
};

struct spa_meta_bitmap {
    uint32_t format;
    struct spa_rectangle size;
    int32_t stride;
    uint32_t offset;
};

struct spa_meta_cursor {
    uint32_t id;
    uint32_t flags;
    struct spa_point position;
    struct spa_point hotspot;
    uint32_t bitmap_offset;
};

struct spa_io_buffers {
    int32_t status;
    uint32_t buffer_id;
};

struct spa_video_info_raw {
    uint32_t format;
    uint64_t modifier;
    uint32_t flags;
    struct spa_rectangle size;
    struct spa_fraction framerate;
    struct spa_fraction max_framerate;
};

enum {
    SPA_FORMAT_VIDEO_FORMAT = 0x20001,
    SPA_FORMAT_VIDEO_MODIFIER = 0x20002,
    SPA_FORMAT_VIDEO_SIZE = 0x20003,
    SPA_FORMAT_VIDEO_FRAMERATE = 0x20004,
    SPA_FORMAT_VIDEO_MAXFRAMERATE = 0x20005,
};

enum {
    SPA_MEDIA_TYPE_VIDEO = 2,
};

enum {
    SPA_VIDEO_FORMAT_RGBX = 7,
    SPA_VIDEO_FORMAT_BGRX = 8,
    SPA_VIDEO_FORMAT_RGBA = 11,
    SPA_VIDEO_FORMAT_BGRA = 12,
};

enum {
    SPA_META_BITMAP = 4,
    SPA_META_CURSOR = 5,
};

enum {
    SPA_DATA_FLAG_READABLE = 1,
    SPA_DATA_FLAG_WRITABLE = 2,
    SPA_DATA_FLAG_READWRITE = 3,
};

enum {
    SPA_DATA_MEMFD = 2,
    SPA_DATA_DMABUF = 3,
};

enum {
    SPA_PARAM_BUFFERS_DATATYPE = 6,
};

enum {
    SPA_CHOICE_NONE = 0,
    SPA_CHOICE_RANGE = 1,
    SPA_CHOICE_STEP = 2,
    SPA_CHOICE_ENUM = 3,
    SPA_CHOICE_FLAGS = 4,
};

enum {
    SPA_PORT_FLAG_CAN_ALLOC_BUFFERS = 4,
    SPA_PORT_FLAG_LIVE = 32,
};

enum {
    SPA_VIDEO_FLAG_NONE = 0,
    SPA_VIDEO_FLAG_MODIFIER = 8,
    SPA_VIDEO_FLAG_MODIFIER_FIXATION_REQUIRED = 16,
};

enum {
    SPA_NODE_FLAG_ASYNC = 64,
};

static inline struct spa_meta *spa_buffer_find_meta(const struct spa_buffer *buffer, uint32_t type) {
    for (uint32_t i = 0; buffer && i < buffer->n_metas; i++)
        if (buffer->metas[i].type == type) return &buffer->metas[i];
    return NULL;
}

int spa_parse_video_format(const void *pod, size_t len, struct spa_video_info_raw *info);

#endif
