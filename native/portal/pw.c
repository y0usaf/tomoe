#include "pw.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "loop.h"

enum { PWV_NODE_PROXY = 2 };
enum { PWV_PORT = 0 };
enum { PWV_FRAME_RATE = 0 };

struct pwv_slot {
    struct pwv_buffer pub;
    struct spa_buffer buffer;
    struct spa_meta metas[PWV_MAX_METAS];
    struct spa_data datas[PWV_MAX_DATAS];
};

struct pwv_target {
    uint32_t id;
    int fd;
    void *activation;
    size_t size;
    bool active;
};

enum { BUFFER_FREE, BUFFER_APP, BUFFER_QUEUED, BUFFER_PORT };

struct pwv {
    struct pww wire;
    struct loop *loop;
    struct loop_source *source;
    struct loop_source *process_source;
    struct pwv_events events;
    void *data;
    uint32_t node_proxy;
    uint32_t node_global;
    bool bound;
    bool failed;
    int process_fd;
    int complete_fd;
    void *activation;
    size_t activation_size;
    struct pwv_target targets[PWV_MAX_TARGETS];
    size_t n_targets;
    struct spa_io_buffers *io;
    const char *node_name;
    const char *description;
    struct pwv_video video;
    int alloc;
    bool cursor;
    uint32_t serial;
    struct spa_buffer *port_buffers[PWV_MAX_BUFFERS];
    uint32_t n_buffers;
    struct pwv_slot slots[PWV_MAX_BUFFERS];
    struct pwv_buffer *dequeued[PWV_MAX_BUFFERS];
    uint32_t n_dequeued;
    struct pwv_buffer *queued[PWV_MAX_BUFFERS];
    uint32_t n_queued;
    int where[PWV_MAX_BUFFERS];
    uint8_t format_pod[1024];
    size_t format_len;
    uint8_t format_modifier_pod[1024];
    size_t format_modifier_len;
    uint8_t buffers_pod[1024];
    size_t buffers_len;
    uint8_t meta_pod[1024];
    size_t meta_len;
    uint8_t io_pod[1024];
    size_t io_len;
    uint8_t negotiated_pod[1024];
    size_t negotiated_len;
    uint32_t param_flags;
    uint32_t publish_gen;
};

static void wire_ready(void *data, int fd, uint32_t mask);
static void process_ready(void *data, int fd, uint32_t mask);
static void run_cycle(struct pwv *v);
static int publish_params(struct pwv *v);

static uint64_t monotonic_ns(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000000000ull + (uint64_t)now.tv_nsec;
}

static uint32_t spa_format_of(uint32_t fourcc) {
    switch (fourcc) {
    case 0x34325241: return SPA_VIDEO_FORMAT_BGRA;
    case 0x34325258: return SPA_VIDEO_FORMAT_BGRX;
    case 0x34324241: return SPA_VIDEO_FORMAT_RGBA;
    case 0x34324258: return SPA_VIDEO_FORMAT_RGBX;
    default: return 0;
    }
}

static void fail(struct pwv *v, const char *error) {
    if (v->failed) return;
    v->failed = true;
    if (v->events.state) v->events.state(v->data, PWV_FAILED, error);
}

static size_t build_format(struct pwv *v, uint8_t *pod, bool with_modifier) {
    struct pww_build b;
    struct pww_frame f;
    pww_build_init(&b, pod, 1024);
    pww_pod_object_begin(&b, &f, SPA_TYPE_OBJECT_FORMAT, SPA_PARAM_ENUMFORMAT);
    pww_pod_prop_id(&b, SPA_FORMAT_MEDIATYPE, SPA_MEDIA_TYPE_VIDEO);
    pww_pod_prop_id(&b, SPA_FORMAT_MEDIASUBTYPE, SPA_MEDIA_SUBTYPE_RAW);
    pww_pod_prop_id(&b, SPA_FORMAT_VIDEO_FORMAT, spa_format_of(v->video.format));
    if (with_modifier) {
        if (v->video.modifier_fixated) {
            pww_pod_prop_long_flags(&b, SPA_FORMAT_VIDEO_MODIFIER, SPA_POD_PROP_FLAG_MANDATORY,
                (int64_t)v->video.modifier);
        } else if (v->video.modifier_count > 1) {
            uint32_t flags = SPA_POD_PROP_FLAG_MANDATORY | SPA_POD_PROP_FLAG_DONT_FIXATE;
            size_t count = v->video.modifier_count;
            if (count > PWV_MAX_MODIFIERS) count = PWV_MAX_MODIFIERS;
            int64_t vals[PWV_MAX_MODIFIERS + 1];
            vals[0] = (int64_t)v->video.modifiers[0];
            for (size_t i = 0; i < count; i++) vals[i + 1] = (int64_t)v->video.modifiers[i];
            pww_pod_prop_choice_long(&b, SPA_FORMAT_VIDEO_MODIFIER, flags, SPA_CHOICE_ENUM,
                vals, (uint32_t)(count + 1));
        } else if (v->video.modifier_count == 1) {
            uint32_t flags = SPA_POD_PROP_FLAG_MANDATORY | SPA_POD_PROP_FLAG_DONT_FIXATE;
            pww_pod_prop_long_flags(&b, SPA_FORMAT_VIDEO_MODIFIER, flags,
                (int64_t)v->video.modifiers[0]);
        } else {
            pww_pod_prop_long(&b, SPA_FORMAT_VIDEO_MODIFIER, (int64_t)v->video.modifier);
        }
    }
    pww_pod_prop_rectangle(&b, SPA_FORMAT_VIDEO_SIZE, (uint32_t)v->video.width,
        (uint32_t)v->video.height);
    pww_pod_prop_fraction(&b, SPA_FORMAT_VIDEO_FRAMERATE, 0, 1);
    uint32_t limit[2] = { v->video.refresh ? v->video.refresh : 60u, 1 };
    uint32_t zero[2] = { 0, 1 };
    uint32_t rates[6] = { limit[0], limit[1], zero[0], zero[1], limit[0], limit[1] };
    pww_pod_prop_choice_fraction(&b, SPA_FORMAT_VIDEO_MAXFRAMERATE, SPA_CHOICE_RANGE, rates, 3);
    pww_pod_object_end(&b, &f);
    return b.fail ? 0 : b.off;
}

static size_t build_buffers(struct pwv *v, uint8_t *pod) {
    struct pww_build b;
    struct pww_frame f;
    int32_t counts[3] = { 8, 2, 16 };
    int32_t types[1];
    pww_build_init(&b, pod, 1024);
    pww_pod_object_begin(&b, &f, SPA_TYPE_OBJECT_PARAMBUFFERS, SPA_PARAM_BUFFERS);
    pww_pod_prop_choice_int(&b, SPA_PARAM_BUFFERS_BUFFERS, SPA_CHOICE_RANGE, counts, 3);
    bool dmabuf = v->alloc == PWV_ALLOC_DMABUF && (!v->negotiated_len || v->video.modifier_set);
    int planes = dmabuf && v->video.planes > 0 ? v->video.planes : 1;
    pww_pod_prop_int(&b, SPA_PARAM_BUFFERS_BLOCKS, planes);
    if (dmabuf && planes == 1) {
        types[0] = (1 << SPA_DATA_DMABUF) | (1 << SPA_DATA_MEMFD);
    } else if (dmabuf) {
        types[0] = 1 << SPA_DATA_DMABUF;
    } else {
        size_t stride = (size_t)v->video.width * 4;
        pww_pod_prop_int(&b, SPA_PARAM_BUFFERS_SIZE, (int32_t)(stride * (size_t)v->video.height));
        pww_pod_prop_int(&b, SPA_PARAM_BUFFERS_STRIDE, (int32_t)stride);
        types[0] = 1 << SPA_DATA_MEMFD;
    }
    pww_pod_prop_choice_int(&b, SPA_PARAM_BUFFERS_DATATYPE, SPA_CHOICE_FLAGS, types, 1);
    pww_pod_object_end(&b, &f);
    return b.fail ? 0 : b.off;
}

static size_t build_meta(struct pwv *v, uint8_t *pod) {
    struct pww_build b;
    struct pww_frame f;
    pww_build_init(&b, pod, 1024);
    pww_pod_object_begin(&b, &f, SPA_TYPE_OBJECT_PARAMMETA, SPA_PARAM_META);
    pww_pod_prop_id(&b, SPA_PARAM_META_TYPE, SPA_META_CURSOR);
    pww_pod_prop_int(&b, SPA_PARAM_META_SIZE,
        (int32_t)(sizeof(struct spa_meta_cursor) + sizeof(struct spa_meta_bitmap) + 256 * 256 * 4));
    pww_pod_object_end(&b, &f);
    return b.fail ? 0 : b.off;
}

static size_t build_io(struct pwv *v, uint8_t *pod) {
    struct pww_build b;
    struct pww_frame f;
    pww_build_init(&b, pod, 1024);
    pww_pod_object_begin(&b, &f, SPA_TYPE_OBJECT_PARAMIO, SPA_PARAM_IO);
    pww_pod_prop_id(&b, SPA_PARAM_IO_ID, SPA_IO_BUFFERS);
    pww_pod_prop_int(&b, SPA_PARAM_IO_SIZE, (int32_t)sizeof(struct spa_io_buffers));
    pww_pod_object_end(&b, &f);
    return b.fail ? 0 : b.off;
}

static int send_node_update(struct pwv *v) {
    uint8_t storage[PWW_BUILD];
    struct pww_build b;
    struct pww_frame root, info;
    pww_message_begin(&b, &root, storage);
    pww_pod_int(&b, SPA_NODE_CHANGE_MASK_FLAGS | SPA_NODE_CHANGE_MASK_PROPS |
        SPA_NODE_CHANGE_MASK_PARAMS);
    pww_pod_int(&b, v->format_len ? 1 : 0);
    if (v->format_len) pww_build_raw_pod(&b, v->format_pod, v->format_len);
    pww_pod_struct_begin(&b, &info);
    pww_pod_int(&b, 0);
    pww_pod_int(&b, 1);
    pww_pod_long(&b, SPA_NODE_CHANGE_MASK_FLAGS | SPA_NODE_CHANGE_MASK_PROPS |
        SPA_NODE_CHANGE_MASK_PARAMS);
    pww_pod_long(&b, SPA_NODE_FLAG_RT | SPA_NODE_FLAG_ASYNC);
    pww_pod_int(&b, 9);
    pww_pod_string(&b, "media.type");
    pww_pod_string(&b, "Video");
    pww_pod_string(&b, "media.category");
    pww_pod_string(&b, "Source");
    pww_pod_string(&b, "media.role");
    pww_pod_string(&b, "Screen");
    pww_pod_string(&b, "media.class");
    pww_pod_string(&b, "Stream/Output/Video");
    pww_pod_string(&b, "node.name");
    pww_pod_string(&b, v->node_name);
    pww_pod_string(&b, "node.description");
    pww_pod_string(&b, v->description);
    pww_pod_string(&b, "node.driver");
    pww_pod_string(&b, "true");
    pww_pod_string(&b, "node.async");
    pww_pod_string(&b, "true");
    pww_pod_string(&b, "node.loop.class");
    pww_pod_string(&b, "main");
    pww_pod_int(&b, 4);
    pww_pod_id(&b, SPA_PARAM_PROPS);
    pww_pod_int(&b, SPA_PARAM_INFO_WRITE);
    pww_pod_id(&b, SPA_PARAM_ENUMFORMAT);
    pww_pod_int(&b, SPA_PARAM_INFO_READ | (v->param_flags & SPA_PARAM_INFO_SERIAL));
    pww_pod_id(&b, SPA_PARAM_FORMAT);
    pww_pod_int(&b, SPA_PARAM_INFO_WRITE);
    pww_pod_id(&b, SPA_PARAM_PROCESSLATENCY);
    pww_pod_int(&b, SPA_PARAM_INFO_WRITE);
    pww_pod_struct_end(&b, &info);
    if (b.fail) return -1;
    return pww_message_send(&v->wire, &b, &root, v->node_proxy,
        PW_CLIENT_NODE_METHOD_UPDATE);
}

static int send_port_update(struct pwv *v) {
    uint8_t storage[PWW_BUILD];
    struct pww_build b;
    struct pww_frame root, info;
    uint32_t flags = SPA_PARAM_INFO_READ | (v->param_flags & SPA_PARAM_INFO_SERIAL);
    pww_message_begin(&b, &root, storage);
    pww_pod_int(&b, SPA_DIRECTION_OUTPUT);
    pww_pod_int(&b, PWV_PORT);
    pww_pod_int(&b, SPA_PORT_CHANGE_MASK_FLAGS | SPA_PORT_CHANGE_MASK_RATE |
        SPA_PORT_CHANGE_MASK_PROPS | SPA_PORT_CHANGE_MASK_PARAMS);
    uint32_t count = (v->format_len ? 1 : 0) + (v->format_modifier_len ? 1 : 0) +
        (v->buffers_len ? 1 : 0) + (v->meta_len ? 1 : 0) + (v->io_len ? 1 : 0) +
        (v->negotiated_len ? 1 : 0);
    pww_pod_int(&b, (int32_t)count);
    if (v->format_len) pww_build_raw_pod(&b, v->format_pod, v->format_len);
    if (v->format_modifier_len)
        pww_build_raw_pod(&b, v->format_modifier_pod, v->format_modifier_len);
    if (v->buffers_len) pww_build_raw_pod(&b, v->buffers_pod, v->buffers_len);
    if (v->meta_len) pww_build_raw_pod(&b, v->meta_pod, v->meta_len);
    if (v->io_len) pww_build_raw_pod(&b, v->io_pod, v->io_len);
    if (v->negotiated_len) pww_build_raw_pod(&b, v->negotiated_pod, v->negotiated_len);
    pww_pod_struct_begin(&b, &info);
    pww_pod_long(&b, SPA_PORT_CHANGE_MASK_FLAGS | SPA_PORT_CHANGE_MASK_RATE |
        SPA_PORT_CHANGE_MASK_PROPS | SPA_PORT_CHANGE_MASK_PARAMS);
    pww_pod_long(&b, SPA_PORT_FLAG_CAN_ALLOC_BUFFERS | SPA_PORT_FLAG_LIVE);
    pww_pod_int(&b, 0);
    pww_pod_int(&b, 1);
    pww_pod_int(&b, 1);
    pww_pod_string(&b, "port.group");
    pww_pod_string(&b, "stream.0");
    pww_pod_int(&b, 9);
    pww_pod_id(&b, SPA_PARAM_ENUMFORMAT);
    pww_pod_int(&b, flags);
    pww_pod_id(&b, SPA_PARAM_META);
    pww_pod_int(&b, flags);
    pww_pod_id(&b, SPA_PARAM_IO);
    pww_pod_int(&b, SPA_PARAM_INFO_READ);
    pww_pod_id(&b, SPA_PARAM_FORMAT);
    pww_pod_int(&b, SPA_PARAM_INFO_READ | SPA_PARAM_INFO_WRITE);
    pww_pod_id(&b, SPA_PARAM_BUFFERS);
    pww_pod_int(&b, flags);
    pww_pod_id(&b, SPA_PARAM_LATENCY);
    pww_pod_int(&b, SPA_PARAM_INFO_WRITE);
    pww_pod_id(&b, SPA_PARAM_TAG);
    pww_pod_int(&b, SPA_PARAM_INFO_WRITE);
    pww_pod_id(&b, SPA_PARAM_CAPABILITY);
    pww_pod_int(&b, 0);
    pww_pod_id(&b, SPA_PARAM_PEERCAPABILITY);
    pww_pod_int(&b, SPA_PARAM_INFO_WRITE | SPA_PARAM_INFO_READ);
    pww_pod_struct_end(&b, &info);
    if (b.fail) return -1;
    return pww_message_send(&v->wire, &b, &root, v->node_proxy,
        PW_CLIENT_NODE_METHOD_PORT_UPDATE);
}

static void clear_buffers(struct pwv *v) {
    pww_unmap_owner(&v->wire, (int)PWV_PORT);
    if (!v->n_buffers) return;
    for (uint32_t i = 0; i < v->n_buffers; i++) {
        struct pwv_buffer *buffer = &v->slots[i].pub;
        if (v->events.remove_buffer) v->events.remove_buffer(v->data, buffer);
        memset(&v->slots[i], 0, sizeof(v->slots[i]));
    }
    v->n_buffers = 0;
    v->n_dequeued = 0;
    v->n_queued = 0;
    memset(v->where, 0, sizeof(v->where));
    memset(v->port_buffers, 0, sizeof(v->port_buffers));
}

static void recycle_buffer(struct pwv *v) {
    if (!v->io || !v->n_buffers) return;
    if (v->io->status == SPA_STATUS_HAVE_DATA) return;
    uint32_t id = v->io->buffer_id;
    if (id >= v->n_buffers || v->where[id] != BUFFER_PORT) return;
    v->where[id] = BUFFER_FREE;
    v->io->buffer_id = SPA_ID_INVALID;
    if (v->n_dequeued < PWV_MAX_BUFFERS) v->dequeued[v->n_dequeued++] = &v->slots[id].pub;
}

static void trigger_targets(struct pwv *v, uint64_t nsec) {
    for (size_t i = 0; i < v->n_targets; i++) {
        struct pwv_target *target = &v->targets[i];
        if (target->id == v->node_global) continue;
        if (!target->active || !target->activation || target->size < PW_ACT_MINIMUM ||
                target->fd < 0) {
            continue;
        }
        uint8_t *a = target->activation;
        int32_t *pending = (int32_t *)(a + PW_ACT_STATE + PW_STATE_PENDING);
        int32_t left = __atomic_sub_fetch(pending, 1, __ATOMIC_ACQ_REL);
        if (left > 0) continue;
        __atomic_store_n((uint32_t *)(a + PW_ACT_STATUS), PW_NODE_ACTIVATION_TRIGGERED,
            __ATOMIC_RELEASE);
        memcpy(a + PW_ACT_SIGNAL_TIME, &nsec, 8);
        uint64_t one = 1;
        if (write(target->fd, &one, sizeof(one)) < 0) continue;
    }
}

static int32_t produce(struct pwv *v) {
    if (!v->io) return SPA_STATUS_NEED_DATA;
    if (v->io->status == SPA_STATUS_HAVE_DATA) {
        uint32_t id = v->io->buffer_id;
        if (id >= v->n_buffers || v->where[id] != BUFFER_PORT) {
            v->io->status = SPA_STATUS_NEED_DATA;
            v->io->buffer_id = SPA_ID_INVALID;
        }
    }
    if (v->io->status != SPA_STATUS_HAVE_DATA && v->n_queued) {
        struct pwv_buffer *buffer = v->queued[0];
        memmove(&v->queued[0], &v->queued[1], (v->n_queued - 1) * sizeof(v->queued[0]));
        v->n_queued--;
        v->where[buffer->id] = BUFFER_PORT;
        v->io->buffer_id = buffer->id;
        v->io->status = SPA_STATUS_HAVE_DATA;
        return SPA_STATUS_HAVE_DATA;
    }
    return SPA_STATUS_NEED_DATA;
}

static void run_cycle(struct pwv *v) {
    uint64_t nsec = monotonic_ns();
    recycle_buffer(v);
    bool owned = false;
    if (v->activation && v->activation_size >= PW_ACT_MINIMUM) {
        uint8_t *a = v->activation;
        uint32_t expected = PW_NODE_ACTIVATION_TRIGGERED;
        if (__atomic_compare_exchange_n((uint32_t *)(a + PW_ACT_STATUS), &expected,
                PW_NODE_ACTIVATION_AWAKE, false, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) {
            owned = true;
            memcpy(a + PW_ACT_AWAKE_TIME, &nsec, 8);
        }
    }
    int32_t status = produce(v);
    if (owned) {
        uint8_t *a = v->activation;
        __atomic_store_n((int32_t *)(a + PW_ACT_STATE + PW_STATE_STATUS), status,
            __ATOMIC_RELEASE);
        uint32_t expected = PW_NODE_ACTIVATION_AWAKE;
        if (__atomic_compare_exchange_n((uint32_t *)(a + PW_ACT_STATUS), &expected,
                PW_NODE_ACTIVATION_FINISHED, false, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) {
            uint64_t finish = monotonic_ns();
            memcpy(a + PW_ACT_FINISH_TIME, &finish, 8);
        }
    }
    if (status == SPA_STATUS_HAVE_DATA) {
        trigger_targets(v, nsec);
        if (v->complete_fd >= 0) {
            uint64_t one = 1;
            if (write(v->complete_fd, &one, sizeof(one)) < 0) return;
        }
    }
    if (v->events.process && v->n_dequeued) v->events.process(v->data);
}

static int send_active(struct pwv *v, bool active) {
    uint8_t storage[256];
    struct pww_build b;
    struct pww_frame root;
    pww_message_begin(&b, &root, storage);
    pww_pod_bool(&b, active ? 1 : 0);
    return pww_message_send(&v->wire, &b, &root, v->node_proxy,
        PW_CLIENT_NODE_METHOD_SET_ACTIVE);
}

static int send_port_buffers(struct pwv *v) {
    uint8_t storage[PWW_BUILD];
    struct pww_build b;
    struct pww_frame root;
    int fds[PWW_MAX_FDS];
    uint32_t n_fds = 0;
    pww_message_begin(&b, &root, storage);
    pww_pod_int(&b, SPA_DIRECTION_OUTPUT);
    pww_pod_int(&b, PWV_PORT);
    pww_pod_int(&b, (int32_t)SPA_ID_INVALID);
    pww_pod_int(&b, (int32_t)v->n_buffers);
    for (uint32_t i = 0; i < v->n_buffers; i++) {
        struct spa_buffer *buffer = v->port_buffers[i];
        pww_pod_int(&b, (int32_t)buffer->n_datas);
        for (uint32_t j = 0; j < buffer->n_datas; j++) {
            struct spa_data *data = &buffer->datas[j];
            if (n_fds >= PWW_MAX_FDS) return -ENOSPC;
            fds[n_fds] = (int)data->fd;
            pww_pod_id(&b, data->type);
            pww_pod_fd(&b, (int64_t)n_fds);
            n_fds++;
            pww_pod_int(&b, (int32_t)data->flags);
            pww_pod_int(&b, (int32_t)data->mapoffset);
            pww_pod_int(&b, (int32_t)data->maxsize);
        }
    }
    if (b.fail) return -1;
    pww_pod_struct_end(&b, &root);
    return pww_send_message_fds(&v->wire, v->node_proxy, PW_CLIENT_NODE_METHOD_PORT_BUFFERS, &b,
        fds, n_fds);
}

static int handle_use_buffers(struct pwv *v, struct pww_parse *p) {
    uint32_t direction, port, mix, flags, count;
    if (pww_parse_int(p, &direction) || pww_parse_int(p, &port) || pww_parse_int(p, &mix) ||
            pww_parse_int(p, &flags) || pww_parse_int(p, &count)) return -EPROTO;
    if (direction != SPA_DIRECTION_OUTPUT || port != PWV_PORT) return 0;
    clear_buffers(v);
    if (count == 0) return 0;
    if (count > PWV_MAX_BUFFERS) return -ENOSPC;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t mem_id, offset, size, n_metas, n_datas;
        if (pww_parse_int(p, &mem_id) || pww_parse_int(p, &offset) || pww_parse_int(p, &size) ||
                pww_parse_int(p, &n_metas)) return -EPROTO;
        if (n_metas > PWV_MAX_METAS) return -ENOSPC;
        uint32_t meta_type[PWV_MAX_METAS], meta_size[PWV_MAX_METAS];
        for (uint32_t j = 0; j < n_metas; j++)
            if (pww_parse_id(p, &meta_type[j]) || pww_parse_int(p, &meta_size[j]))
                return -EPROTO;
        if (pww_parse_int(p, &n_datas)) return -EPROTO;
        if (n_datas > PWV_MAX_DATAS) return -ENOSPC;
        uint32_t data_type[PWV_MAX_DATAS], data_id[PWV_MAX_DATAS], data_flags[PWV_MAX_DATAS];
        uint32_t data_offset[PWV_MAX_DATAS], data_maxsize[PWV_MAX_DATAS];
        for (uint32_t j = 0; j < n_datas; j++)
            if (pww_parse_id(p, &data_type[j]) || pww_parse_int(p, &data_id[j]) ||
                    pww_parse_int(p, &data_flags[j]) || pww_parse_int(p, &data_offset[j]) ||
                    pww_parse_int(p, &data_maxsize[j])) return -EPROTO;
        void *base = NULL;
        if (pww_map_mem(&v->wire, mem_id, offset, size, 0, (int)PWV_PORT, &base, NULL) < 0)
            return -ENOMEM;
        struct pwv_slot *slot = &v->slots[i];
        memset(slot, 0, sizeof(*slot));
        size_t meta_offset = 0;
        for (uint32_t j = 0; j < n_metas; j++) {
            slot->metas[j].type = meta_type[j];
            slot->metas[j].size = meta_size[j];
            slot->metas[j].data = (uint8_t *)base + meta_offset;
            meta_offset += (meta_size[j] + 7u) & ~7u;
        }
        struct spa_chunk *chunk = (struct spa_chunk *)((uint8_t *)base + meta_offset);
        for (uint32_t j = 0; j < n_datas; j++) {
            slot->datas[j].type = data_type[j];
            slot->datas[j].flags = data_flags[j];
            slot->datas[j].fd = -1;
            slot->datas[j].mapoffset = data_offset[j];
            slot->datas[j].maxsize = data_maxsize[j];
            slot->datas[j].data = NULL;
            slot->datas[j].chunk = &chunk[j];
        }
        slot->buffer.n_metas = n_metas;
        slot->buffer.n_datas = n_datas;
        slot->buffer.metas = slot->metas;
        slot->buffer.datas = slot->datas;
        slot->pub.id = i;
        slot->pub.buffer = &slot->buffer;
        slot->pub.user_data = NULL;
        v->port_buffers[i] = &slot->buffer;
        v->where[i] = BUFFER_FREE;
    }
    v->n_buffers = count;
    bool filled = true;
    for (uint32_t i = 0; i < count; i++)
        if (!v->events.add_buffer || !v->events.add_buffer(v->data, &v->slots[i].pub))
            filled = false;
    if (!filled) {
        clear_buffers(v);
        return 0;
    }
    if (send_port_buffers(v) < 0) {
        clear_buffers(v);
        return 0;
    }
    v->n_dequeued = 0;
    for (uint32_t i = 0; i < count; i++) {
        v->where[i] = BUFFER_FREE;
        v->dequeued[v->n_dequeued++] = &v->slots[i].pub;
    }
    if (v->events.process) v->events.process(v->data);
    return 0;
}

static int handle_set_activation(struct pwv *v, struct pww_parse *p) {
    uint32_t node_id, mem_id, offset, size;
    int64_t index;
    if (pww_parse_int(p, &node_id) || pww_parse_fd(p, &index) || pww_parse_int(p, &mem_id) ||
            pww_parse_int(p, &offset) || pww_parse_int(p, &size)) return -EPROTO;
    struct pwv_target *target = NULL;
    for (size_t i = 0; i < v->n_targets; i++)
        if (v->targets[i].id == node_id) target = &v->targets[i];
    if (mem_id == SPA_ID_INVALID) {
        if (target) {
            target->active = false;
            target->activation = NULL;
            target->fd = -1;
        }
        return 0;
    }
    if (index < 0 || index >= (int64_t)v->wire.n_fds) return -EPROTO;
    if (!target) {
        if (v->n_targets >= PWV_MAX_TARGETS) return -ENOSPC;
        target = &v->targets[v->n_targets++];
        memset(target, 0, sizeof(*target));
        target->id = node_id;
    }
    void *ptr = NULL;
    size_t mapped = 0;
    if (pww_map_mem(&v->wire, mem_id, offset, size, 1, -1, &ptr, &mapped) < 0) return -ENOMEM;
    target->activation = ptr;
    target->size = mapped;
    target->fd = v->wire.fds[index];
    v->wire.fds[index] = -1;
    target->active = true;
    return 0;
}

static int handle_transport(struct pwv *v, struct pww_parse *p) {
    int64_t read_index, write_index;
    uint32_t mem_id, offset, size;
    if (pww_parse_fd(p, &read_index) || pww_parse_fd(p, &write_index) ||
            pww_parse_int(p, &mem_id) || pww_parse_int(p, &offset) || pww_parse_int(p, &size))
        return -EPROTO;
    if (read_index < 0 || read_index >= (int64_t)v->wire.n_fds) return -EPROTO;
    if (write_index < 0 || write_index >= (int64_t)v->wire.n_fds) return -EPROTO;
    v->process_fd = v->wire.fds[read_index];
    v->wire.fds[read_index] = -1;
    if (v->process_fd >= 0 && !v->process_source)
        v->process_source = loop_add_io(v->loop, v->process_fd, LOOP_IO_IN, process_ready, v);
    v->complete_fd = v->wire.fds[write_index];
    v->wire.fds[write_index] = -1;
    void *activation = NULL;
    size_t activation_size = 0;
    if (mem_id != SPA_ID_INVALID &&
            pww_map_mem(&v->wire, mem_id, offset, size, 1, -1, &activation, &activation_size) < 0)
        return -ENOMEM;
    v->activation = activation;
    v->activation_size = activation_size;
    if (activation && activation_size >= PW_ACT_MINIMUM)
        __atomic_store_n((uint32_t *)((uint8_t *)activation + PW_ACT_STATUS),
            PW_NODE_ACTIVATION_FINISHED, __ATOMIC_RELEASE);
    return 0;
}

static int parse_format(struct pwv *v, const uint8_t *pod, size_t len) {
    struct spa_video_info_raw info;
    if (spa_parse_video_format(pod, len, &info) < 0) return -1;
    uint32_t refresh = v->video.refresh;
    if (info.max_framerate.denom && info.max_framerate.num)
        refresh = info.max_framerate.num / info.max_framerate.denom;
    v->video.format =
        info.format == SPA_VIDEO_FORMAT_BGRA ? 0x34325241 :
        info.format == SPA_VIDEO_FORMAT_BGRX ? 0x34325258 :
        info.format == SPA_VIDEO_FORMAT_RGBA ? 0x34324241 :
        info.format == SPA_VIDEO_FORMAT_RGBX ? 0x34324258 : v->video.format;
    v->video.modifier = info.modifier;
    v->video.modifier_set = (info.flags & SPA_VIDEO_FLAG_MODIFIER) != 0;
    v->video.modifier_fixated = (info.flags & SPA_VIDEO_FLAG_MODIFIER_FIXATION_REQUIRED) == 0;
    if (info.size.width && info.size.height) {
        v->video.width = (int)info.size.width;
        v->video.height = (int)info.size.height;
    }
    v->video.refresh = refresh;
    if (v->events.format) v->events.format(v->data, &v->video);
    return 0;
}

static int dispatch(void *data, uint32_t id, uint32_t opcode, struct pww_parse *p) {
    struct pwv *v = data;
    if (id == 0) {
        switch (opcode) {
        case PW_CORE_EVENT_PING: {
            uint32_t target, seq;
            if (pww_parse_int(p, &target) || pww_parse_int(p, &seq)) return -EPROTO;
            uint8_t storage[256];
            struct pww_build b;
            struct pww_frame root;
            pww_message_begin(&b, &root, storage);
            pww_pod_int(&b, (int32_t)target);
            pww_pod_int(&b, (int32_t)seq);
            return pww_message_send(&v->wire, &b, &root, 0, PW_CORE_METHOD_PONG);
        }
        case PW_CORE_EVENT_ERROR: {
            uint32_t target, seq, res;
            const char *message;
            uint32_t len;
            if (pww_parse_int(p, &target) || pww_parse_int(p, &seq) || pww_parse_int(p, &res) ||
                    pww_parse_string(p, &message, &len)) return -EPROTO;
            if (target == 0) fail(v, message);
            return 0;
        }
        case PW_CORE_EVENT_ADD_MEM: {
            uint32_t mem_id, type, flags;
            int64_t index;
            if (pww_parse_int(p, &mem_id) || pww_parse_id(p, &type) || pww_parse_fd(p, &index) ||
                    pww_parse_int(p, &flags)) return -EPROTO;
            if (index < 0 || index >= (int64_t)v->wire.n_fds) return -EPROTO;
            int fd = v->wire.fds[index];
            v->wire.fds[index] = -1;
            return pww_mem_add(&v->wire, mem_id, fd);
        }
        case PW_CORE_EVENT_REMOVE_MEM: {
            uint32_t mem_id;
            if (pww_parse_int(p, &mem_id)) return -EPROTO;
            pww_mem_remove(&v->wire, mem_id);
            return 0;
        }
        case PW_CORE_EVENT_BOUND_ID:
        case PW_CORE_EVENT_BOUND_PROPS: {
            uint32_t proxy, global;
            if (pww_parse_int(p, &proxy) || pww_parse_int(p, &global)) return -EPROTO;
            if (opcode == PW_CORE_EVENT_BOUND_PROPS && pww_parse_nested_dict(p)) return -EPROTO;
            if (proxy != v->node_proxy) return 0;
            v->node_global = global;
            v->bound = true;
            if (v->events.state) v->events.state(v->data, PWV_BOUND, NULL);
            return 0;
        }
        default:
            return 0;
        }
    }
    if (id != v->node_proxy) return 0;
    switch (opcode) {
    case PW_CLIENT_NODE_EVENT_TRANSPORT:
        return handle_transport(v, p);
    case PW_CLIENT_NODE_EVENT_SET_IO: {
        uint32_t io_id, mem_id, offset, size;
        if (pww_parse_id(p, &io_id) || pww_parse_int(p, &mem_id) || pww_parse_int(p, &offset) ||
                pww_parse_int(p, &size)) return -EPROTO;
        void *ptr = NULL;
        if (mem_id != SPA_ID_INVALID &&
                pww_map_mem(&v->wire, mem_id, offset, size, 1, -1, &ptr, NULL) < 0)
            return -ENOMEM;
        if (io_id == SPA_IO_BUFFERS) v->io = ptr;
        return 0;
    }
    case PW_CLIENT_NODE_EVENT_PORT_SET_IO: {
        uint32_t direction, port, mix, io_id, mem_id, offset, size;
        if (pww_parse_int(p, &direction) || pww_parse_int(p, &port) || pww_parse_int(p, &mix) ||
                pww_parse_id(p, &io_id) || pww_parse_int(p, &mem_id) ||
                pww_parse_int(p, &offset) || pww_parse_int(p, &size)) return -EPROTO;
        if (direction != SPA_DIRECTION_OUTPUT || port != PWV_PORT) return 0;
        void *ptr = NULL;
        if (mem_id != SPA_ID_INVALID &&
                pww_map_mem(&v->wire, mem_id, offset, size, 1, -1, &ptr, NULL) < 0)
            return -ENOMEM;
        if (io_id == SPA_IO_BUFFERS) {
            v->io = ptr;
            if (ptr) run_cycle(v);
        }
        return 0;
    }
    case PW_CLIENT_NODE_EVENT_PORT_USE_BUFFERS:
        return handle_use_buffers(v, p);
    case PW_CLIENT_NODE_EVENT_SET_ACTIVATION:
        return handle_set_activation(v, p);
    case PW_CLIENT_NODE_EVENT_PORT_SET_PARAM: {
        uint32_t direction, port, param, flags;
        if (pww_parse_int(p, &direction) || pww_parse_int(p, &port) || pww_parse_id(p, &param) ||
                pww_parse_int(p, &flags)) return -EPROTO;
        if (direction != SPA_DIRECTION_OUTPUT || port != PWV_PORT) return 0;
        if (param == SPA_PARAM_FORMAT) {
            uint32_t type, body;
            const uint8_t *pod = pww_parse_pod(p, &type, &body);
            clear_buffers(v);
            v->negotiated_len = 0;
            if (!pod || type != SPA_TYPE_OBJECT) {
                v->video.modifier_set = false;
                v->video.modifier_fixated = false;
                if (v->events.format) v->events.format(v->data, NULL);
                publish_params(v);
                send_port_update(v);
                return 0;
            }
            if (8 + body > sizeof(v->negotiated_pod)) return 0;
            uint32_t gen = v->publish_gen;
            if (parse_format(v, pod, 8 + body) < 0) return 0;
            if (v->publish_gen == gen && v->video.modifier_fixated) {
                memcpy(v->negotiated_pod, pod, 8 + body);
                v->negotiated_len = 8 + body;
                v->buffers_len = build_buffers(v, v->buffers_pod);
                send_port_update(v);
            }
            return 0;
        }
        return 0;
    }
    case PW_CLIENT_NODE_EVENT_COMMAND: {
        uint32_t type, command;
        if (pww_parse_pod_object(p, &type, &command)) return -EPROTO;
        if (type != SPA_TYPE_OBJECT_COMMANDNODE) return 0;
        if (command == SPA_NODE_COMMAND_START) run_cycle(v);
        return 0;
    }
    default:
        return 0;
    }
}

static void process_ready(void *data, int fd, uint32_t mask) {
    struct pwv *v = data;
    uint64_t count;
    if (read(fd, &count, sizeof(count)) < 0 && errno != EAGAIN) return;
    run_cycle(v);
}

static void wire_ready(void *data, int fd, uint32_t mask) {
    struct pwv *v = data;
    if (mask & (LOOP_IO_ERR | LOOP_IO_HUP)) {
        fail(v, "the PipeWire connection closed");
        return;
    }
    if (!(mask & LOOP_IO_IN)) return;
    if (pww_recv(&v->wire, dispatch, v) < 0) fail(v, "the PipeWire connection failed");
}

static int publish_params(struct pwv *v) {
    v->param_flags ^= SPA_PARAM_INFO_SERIAL;
    v->format_len = build_format(v, v->format_pod, false);
    v->format_modifier_len = 0;
    if (v->alloc != PWV_ALLOC_MEMORY)
        v->format_modifier_len = build_format(v, v->format_modifier_pod, true);
    v->buffers_len = build_buffers(v, v->buffers_pod);
    v->meta_len = v->cursor ? build_meta(v, v->meta_pod) : 0;
    v->io_len = build_io(v, v->io_pod);
    return 0;
}

int pwv_publish(struct pwv *v, const struct pwv_video *video, int alloc) {
    if (!v) return -1;
    bool streaming = v->n_buffers > 0;
    if (streaming) send_active(v, false);
    v->publish_gen++;
    v->negotiated_len = 0;
    v->video = *video;
    v->alloc = alloc;
    publish_params(v);
    int node = send_node_update(v);
    int port = send_port_update(v);
    if (streaming) send_active(v, true);
    return node < 0 || port < 0 ? -1 : 0;
}

struct pwv *pwv_start(struct loop *loop, const char *node_name, const char *description,
        const struct pwv_video *video, int alloc, bool cursor, const struct pwv_events *events,
        void *data) {
    struct pwv *v = calloc(1, sizeof(*v));
    if (!v) return NULL;
    v->loop = loop;
    v->events = *events;
    v->data = data;
    v->node_proxy = PWV_NODE_PROXY;
    v->node_global = SPA_ID_INVALID;
    v->process_fd = -1;
    v->complete_fd = -1;
    v->io = NULL;
    v->video = *video;
    v->alloc = alloc;
    v->cursor = cursor;
    v->node_name = node_name;
    v->description = description ? description : "tomoe portal screencast";
    if (pww_connect(&v->wire) < 0) {
        free(v);
        return NULL;
    }
    v->source = loop_add_io(loop, v->wire.fd, LOOP_IO_IN, wire_ready, v);
    publish_params(v);
    uint8_t storage[PWW_BUILD];
    struct pww_build b;
    struct pww_frame root, dict;
    pww_message_begin(&b, &root, storage);
    pww_pod_int(&b, 4);
    if (pww_message_send(&v->wire, &b, &root, 0, PW_CORE_METHOD_HELLO) < 0) goto failed;
    pww_message_begin(&b, &root, storage);
    pww_pod_struct_begin(&b, &dict);
    pww_pod_int(&b, 2);
    pww_pod_string(&b, "application.name");
    pww_pod_string(&b, "tomoe-portal");
    pww_pod_string(&b, "application.process.binary");
    pww_pod_string(&b, "xdg-desktop-portal-tomoe");
    pww_pod_struct_end(&b, &dict);
    if (pww_message_send(&v->wire, &b, &root, 1, PW_CLIENT_METHOD_UPDATE_PROPERTIES) < 0)
        goto failed;
    pww_message_begin(&b, &root, storage);
    pww_pod_string(&b, "client-node");
    pww_pod_string(&b, "PipeWire:Interface:ClientNode");
    pww_pod_int(&b, 6);
    pww_pod_struct_begin(&b, &dict);
    pww_pod_int(&b, 10);
    pww_pod_string(&b, "media.type");
    pww_pod_string(&b, "Video");
    pww_pod_string(&b, "media.category");
    pww_pod_string(&b, "Source");
    pww_pod_string(&b, "media.role");
    pww_pod_string(&b, "Screen");
    pww_pod_string(&b, "media.class");
    pww_pod_string(&b, "Stream/Output/Video");
    pww_pod_string(&b, "node.name");
    pww_pod_string(&b, v->node_name);
    pww_pod_string(&b, "node.description");
    pww_pod_string(&b, v->description);
    pww_pod_string(&b, "node.driver");
    pww_pod_string(&b, "true");
    pww_pod_string(&b, "node.async");
    pww_pod_string(&b, "true");
    pww_pod_string(&b, "node.loop.class");
    pww_pod_string(&b, "main");
    pww_pod_struct_end(&b, &dict);
    pww_pod_int(&b, (int32_t)v->node_proxy);
    if (pww_message_send(&v->wire, &b, &root, 0, PW_CORE_METHOD_CREATE_OBJECT) < 0) goto failed;
    if (send_node_update(v) < 0) goto failed;
    if (send_port_update(v) < 0) goto failed;
    pww_message_begin(&b, &root, storage);
    pww_pod_bool(&b, 1);
    if (pww_message_send(&v->wire, &b, &root, v->node_proxy,
            PW_CLIENT_NODE_METHOD_SET_ACTIVE) < 0) goto failed;
    pww_message_begin(&b, &root, storage);
    pww_pod_int(&b, 0);
    pww_pod_int(&b, (int32_t)(0x40000000u | (v->wire.seq & 0x3fffffffu)));
    if (pww_message_send(&v->wire, &b, &root, 0, PW_CORE_METHOD_SYNC) < 0) goto failed;
    return v;
failed:
    pwv_stop(v);
    return NULL;
}

void pwv_stop(struct pwv *v) {
    if (!v) return;
    if (v->process_source) loop_del_io(v->loop, v->process_source);
    if (v->source) loop_del_io(v->loop, v->source);
    if (v->wire.fd >= 0) close(v->wire.fd);
    v->wire.fd = -1;
    for (size_t i = 0; i < v->wire.n_fds; i++)
        if (v->wire.fds[i] >= 0) close(v->wire.fds[i]);
    v->wire.n_fds = 0;
    pww_unmap_all(&v->wire);
    pww_mem_clear(&v->wire);
    free(v->wire.in);
    for (size_t i = 0; i < v->n_targets; i++)
        if (v->targets[i].fd >= 0) close(v->targets[i].fd);
    if (v->process_fd >= 0) close(v->process_fd);
    if (v->complete_fd >= 0) close(v->complete_fd);
    free(v);
}

struct pwv_buffer *pwv_dequeue(struct pwv *v) {
    if (!v || !v->n_dequeued) return NULL;
    struct pwv_buffer *buffer = v->dequeued[0];
    memmove(&v->dequeued[0], &v->dequeued[1], (v->n_dequeued - 1) * sizeof(v->dequeued[0]));
    v->n_dequeued--;
    v->where[buffer->id] = BUFFER_APP;
    return buffer;
}

void pwv_queue(struct pwv *v, struct pwv_buffer *buffer) {
    if (!v || !buffer || v->where[buffer->id] != BUFFER_APP) return;
    if (v->n_queued >= PWV_MAX_BUFFERS) return;
    v->where[buffer->id] = BUFFER_QUEUED;
    v->queued[v->n_queued++] = buffer;
    run_cycle(v);
}

uint32_t pwv_node_id(struct pwv *v) {
    return v ? v->node_global : SPA_ID_INVALID;
}
