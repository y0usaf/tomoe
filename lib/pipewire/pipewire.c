#define _GNU_SOURCE

#include "pipewire.h"

#define PWN_MAX_BUFFERS 32
#define PWN_MAX_TARGETS 16
#define PWN_MAX_PARAM 4096
#define PWN_RING 65536u

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

struct pwn_target {
    uint32_t id;
    int fd;
    void *activation;
    size_t size;
    int active;
};

struct pwn_data {
    float *frames;
    struct pwn_chunk *chunk;
    uint32_t maxsize;
};

struct pwn_slot {
    struct pwn_data datas[1];
    uint32_t n_datas;
};

struct pwn_param {
    uint32_t id;
    uint32_t flags;
    uint8_t pod[PWN_MAX_PARAM];
    size_t len;
    int dirty;
};

struct pwn_port {
    struct {
        uint32_t status;
        uint32_t buffer_id;
    } *io;
    struct {
        uint32_t delay;
        uint32_t size;
        double rate;
        uint32_t flags;
        int32_t delay_frac;
    } *ratematch;
    struct pwn_slot slots[PWN_MAX_BUFFERS];
    uint32_t n_slots;
    uint32_t free_list[PWN_MAX_BUFFERS];
    uint32_t n_free;
    struct pwn_param params[9];
    size_t n_params;
};

struct pwn_client {
    struct pwn_ops ops;
    void *data;
    pthread_t thread;
    int wake;
    volatile int quit;
    const char *app_name;
    const char *node_name;
    uint32_t rate;
    uint32_t channels;
    struct pww wire;
    uint32_t node_proxy;
    uint32_t node_global;
    int bound;
    int paused;
    int streaming;
    int faulted;
    int process_fd;
    int complete_fd;
    void *activation;
    size_t activation_size;
    void *position;
    void *clock;
    struct pwn_port ports[2];
    struct pwn_target targets[PWN_MAX_TARGETS];
    size_t n_targets;
    struct pwn_param node_params[5];
    size_t n_node_params;
    uint8_t raw_format[PWN_MAX_PARAM];
    size_t raw_format_len;
    uint8_t dsp_format[PWN_MAX_PARAM];
    size_t dsp_format_len;
    uint8_t io_param[PWN_MAX_PARAM];
    size_t io_param_len;
    uint8_t meta_param[PWN_MAX_PARAM];
    size_t meta_param_len;
    uint8_t buffers_param[PWN_MAX_PARAM];
    size_t buffers_param_len;
    uint8_t props_param[PWN_MAX_PARAM];
    float volume;
    int mute;
    float channel_volumes[2];
    uint32_t n_channel_volumes;
    float soft_volumes[2];
    uint32_t n_soft_volumes;
    int soft_mute;
    int have_soft_volume;
    float gain[2];
    float *src[2];
    uint32_t src_have;
    uint64_t src_base;
    double src_pos;
    double src_ratio;
    int src_active;
    struct pwn_chunk src_chunk[2];
};






static void report(struct pwn_client *c, int state, int error) {
    if (c->ops.state) c->ops.state(c->data, state, error);
}

static int fault(struct pwn_client *c, int error) {
    report(c, PWN_RETRYING, error);
    c->faulted = 1;
    return -1;
}

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static int eventfd_write1(int fd) {
    uint64_t one = 1;
    return (int)write(fd, &one, sizeof(one));
}

static int eventfd_read1(int fd) {
    uint64_t count;
    return (int)read(fd, &count, sizeof(count));
}


static void port_clear(struct pwn_client *c, unsigned port_id) {
    for (size_t i = 0; i < c->wire.n_maps;) {
        if (c->wire.maps[i].keep || c->wire.maps[i].owner != (int)port_id) {
            i++;
            continue;
        }
        munmap(c->wire.maps[i].ptr, c->wire.maps[i].len);
        c->wire.maps[i] = c->wire.maps[c->wire.n_maps - 1];
        c->wire.n_maps--;
    }
    c->ports[port_id].n_slots = 0;
    c->ports[port_id].n_free = 0;
}


static void param_toggle_serial(struct pwn_param *params, size_t count) {
    for (size_t i = 0; i < count; i++) {
        if (!params[i].dirty) continue;
        params[i].flags ^= SPA_PARAM_INFO_SERIAL;
        params[i].dirty = 0;
    }
}

static void add_param(struct pwn_param *params, size_t *count, size_t max, uint32_t id,
        uint32_t flags, const void *pod, size_t len) {
    if (len > PWN_MAX_PARAM) return;
    struct pwn_param *p = NULL;
    for (size_t i = 0; i < *count; i++)
        if (params[i].id == id) p = &params[i];
    if (!p) {
        if (*count >= max) return;
        p = &params[(*count)++];
        memset(p, 0, sizeof(*p));
        p->id = id;
    }
    memcpy(p->pod, pod, len);
    p->len = len;
    p->flags |= flags;
    p->dirty = 1;
}

static struct pwn_param *find_param(struct pwn_param *params, size_t count, uint32_t id) {
    for (size_t i = 0; i < count; i++)
        if (params[i].id == id) return &params[i];
    return NULL;
}

static void build_dsp_format(uint8_t *storage, size_t *len) {
    struct pww_build b;
    struct pww_frame f;
    pww_build_init(&b, storage, PWN_MAX_PARAM);
    pww_pod_object_begin(&b, &f, SPA_TYPE_OBJECT_FORMAT, SPA_PARAM_ENUMFORMAT);
    pww_pod_prop_id(&b, SPA_FORMAT_MEDIATYPE, SPA_MEDIA_TYPE_AUDIO);
    pww_pod_prop_id(&b, SPA_FORMAT_MEDIASUBTYPE, SPA_MEDIA_SUBTYPE_DSP);
    pww_pod_prop_id(&b, SPA_FORMAT_AUDIO_FORMAT, SPA_AUDIO_FORMAT_F32P);
    pww_pod_object_end(&b, &f);
    *len = b.fail ? 0 : b.off;
}

static void build_raw_format(struct pwn_client *c, uint8_t *storage, size_t *len) {
    struct pww_build b;
    struct pww_frame f;
    uint32_t positions[2] = { SPA_AUDIO_CHANNEL_FL, SPA_AUDIO_CHANNEL_FR };
    pww_build_init(&b, storage, PWN_MAX_PARAM);
    pww_pod_object_begin(&b, &f, SPA_TYPE_OBJECT_FORMAT, SPA_PARAM_ENUMFORMAT);
    pww_pod_prop_id(&b, SPA_FORMAT_MEDIATYPE, SPA_MEDIA_TYPE_AUDIO);
    pww_pod_prop_id(&b, SPA_FORMAT_MEDIASUBTYPE, SPA_MEDIA_SUBTYPE_RAW);
    pww_pod_prop_id(&b, SPA_FORMAT_AUDIO_FORMAT, SPA_AUDIO_FORMAT_F32);
    pww_pod_prop_int(&b, SPA_FORMAT_AUDIO_RATE, (int32_t)c->rate);
    pww_pod_prop_int(&b, SPA_FORMAT_AUDIO_CHANNELS, 2);
    pww_pod_prop_array_ids(&b, SPA_FORMAT_AUDIO_POSITION, positions, 2);
    pww_pod_object_end(&b, &f);
    *len = b.fail ? 0 : b.off;
}

static void build_io_param(uint8_t *storage, size_t *len) {
    struct pww_build b;
    struct pww_frame f;
    pww_build_init(&b, storage, PWN_MAX_PARAM);
    pww_pod_object_begin(&b, &f, SPA_TYPE_OBJECT_PARAMIO, SPA_PARAM_IO);
    pww_pod_prop_id(&b, SPA_PARAM_IO_ID, SPA_IO_BUFFERS);
    pww_pod_prop_int(&b, SPA_PARAM_IO_SIZE, 8);
    pww_pod_object_end(&b, &f);
    *len = b.fail ? 0 : b.off;
}

static void build_meta_param(uint8_t *storage, size_t *len) {
    struct pww_build b;
    struct pww_frame f;
    pww_build_init(&b, storage, PWN_MAX_PARAM);
    pww_pod_object_begin(&b, &f, SPA_TYPE_OBJECT_PARAMMETA, SPA_PARAM_META);
    pww_pod_prop_id(&b, SPA_PARAM_META_TYPE, SPA_META_HEADER);
    pww_pod_prop_int(&b, SPA_PARAM_META_SIZE, 32);
    pww_pod_object_end(&b, &f);
    *len = b.fail ? 0 : b.off;
}

static void build_buffers_param(uint8_t *storage, size_t *len) {
    struct pww_build b;
    struct pww_frame f;
    pww_build_init(&b, storage, PWN_MAX_PARAM);
    pww_pod_object_begin(&b, &f, SPA_TYPE_OBJECT_PARAMBUFFERS, SPA_PARAM_BUFFERS);
    pww_pod_prop_int(&b, SPA_PARAM_BUFFERS_BUFFERS, 8);
    pww_pod_prop_int(&b, SPA_PARAM_BUFFERS_BLOCKS, 1);
    pww_pod_prop_int(&b, SPA_PARAM_BUFFERS_SIZE, 32768);
    pww_pod_prop_int(&b, SPA_PARAM_BUFFERS_STRIDE, 4);
    pww_pod_object_end(&b, &f);
    *len = b.fail ? 0 : b.off;
}

static float clamp_volume(float v) {
    if (!(v > 0.0f)) return 0.0f;
    if (v > 10.0f) return 10.0f;
    return v;
}

static void update_gains(struct pwn_client *c) {
    int muted = c->have_soft_volume ? c->soft_mute : c->mute;
    const float *volumes = c->have_soft_volume ? c->soft_volumes : c->channel_volumes;
    uint32_t n_volumes = c->have_soft_volume ? c->n_soft_volumes : c->n_channel_volumes;
    float volume = muted ? 0.0f : clamp_volume(c->volume);
    for (unsigned i = 0; i < 2; i++) {
        float gain = volume;
        if (n_volumes >= 2) gain = clamp_volume(volumes[i]) * volume;
        c->gain[i] = gain;
    }
}

static void apply_gain(float *frames, uint32_t count, float gain) {
    if (gain == 0.0f) {
        memset(frames, 0, count * sizeof(float));
        return;
    }
    if (gain == 1.0f) return;
    for (uint32_t i = 0; i < count; i++) frames[i] *= gain;
}

static size_t build_props(struct pwn_client *c, uint8_t *storage) {
    struct pww_build b;
    struct pww_frame f;
    pww_build_init(&b, storage, PWN_MAX_PARAM);
    pww_pod_object_begin(&b, &f, SPA_TYPE_OBJECT_PROPS, SPA_PARAM_PROPS);
    pww_pod_prop_float(&b, SPA_PROP_VOLUME, c->volume);
    pww_pod_prop_bool(&b, SPA_PROP_MUTE, c->mute);
    pww_pod_prop_array_floats(&b, SPA_PROP_CHANNELVOLUMES, c->channel_volumes, 2);
    pww_pod_prop_bool(&b, SPA_PROP_SOFTMUTE, c->soft_mute);
    pww_pod_prop_array_floats(&b, SPA_PROP_SOFTVOLUMES, c->soft_volumes, 2);
    pww_pod_object_end(&b, &f);
    return b.fail ? 0 : b.off;
}

static void copy_float_array(const uint8_t *value, uint32_t size, uint32_t type,
        float *out, uint32_t *count) {
    if (type != SPA_TYPE_ARRAY || size < 8) return;
    uint32_t child_size, child_type;
    memcpy(&child_size, value, 4);
    memcpy(&child_type, value + 4, 4);
    if (child_size != sizeof(float) || child_type != SPA_TYPE_FLOAT) return;
    uint32_t n = (size - 8) / (uint32_t)sizeof(float);
    if (n > 2) n = 2;
    memcpy(out, value + 8, n * sizeof(float));
    *count = n;
}

static void apply_props(struct pwn_client *c, const uint8_t *pod, uint32_t body) {
    int have_channel = 0, have_soft = 0;
    if (body < 8) return;
    size_t end = 8u + body;
    size_t off = 16;
    while (off + 8 <= end) {
        uint32_t key, flags, size, type;
        memcpy(&key, pod + off, 4);
        memcpy(&flags, pod + off + 4, 4);
        off += 8;
        if (off + 8 > end) break;
        memcpy(&size, pod + off, 4);
        memcpy(&type, pod + off + 4, 4);
        if (off + 8 + size <= end) {
            const uint8_t *value = pod + off + 8;
            switch (key) {
            case SPA_PROP_VOLUME:
                if (type == SPA_TYPE_FLOAT && size >= 4) memcpy(&c->volume, value, 4);
                break;
            case SPA_PROP_MUTE:
                if (type == SPA_TYPE_BOOL && size >= 4) {
                    uint32_t muted;
                    memcpy(&muted, value, 4);
                    c->mute = muted ? 1 : 0;
                    have_channel = 1;
                }
                break;
            case SPA_PROP_CHANNELVOLUMES:
                if (type == SPA_TYPE_ARRAY) {
                    copy_float_array(value, size, type, c->channel_volumes,
                            &c->n_channel_volumes);
                    have_channel = 1;
                }
                break;
            case SPA_PROP_SOFTMUTE:
                if (type == SPA_TYPE_BOOL && size >= 4) {
                    uint32_t muted;
                    memcpy(&muted, value, 4);
                    c->soft_mute = muted ? 1 : 0;
                    have_soft = 1;
                }
                break;
            case SPA_PROP_SOFTVOLUMES:
                if (type == SPA_TYPE_ARRAY) {
                    copy_float_array(value, size, type, c->soft_volumes,
                            &c->n_soft_volumes);
                    have_soft = 1;
                }
                break;
            default:
                break;
            }
        }
        off += 8 + size;
        off = (off + 7) & ~(size_t)7;
    }
    if (have_soft) c->have_soft_volume = 1;
    else if (have_channel) c->have_soft_volume = 0;
    update_gains(c);
}

static void push_dict_item(struct pww_build *b, const char *key, const char *value) {
    pww_pod_string(b, key);
    pww_pod_string(b, value);
}

static void push_dict_begin(struct pww_build *b, struct pww_frame *f, uint32_t n) {
    pww_pod_struct_begin(b, f);
    pww_pod_int(b, (int32_t)n);
}

static void push_dict_end(struct pww_build *b, struct pww_frame *f) {
    pww_pod_struct_end(b, f);
}

static uint32_t node_flag(struct pwn_client *c, uint32_t id, uint32_t flags) {
    struct pwn_param *p = find_param(c->node_params, c->n_node_params, id);
    return p ? flags | p->flags : flags;
}

static uint32_t port_flag(struct pwn_client *c, unsigned port_id, uint32_t id, uint32_t flags) {
    struct pwn_port *port = &c->ports[port_id];
    struct pwn_param *p = find_param(port->params, port->n_params, id);
    return p ? flags | p->flags : flags;
}

static int send_update(struct pwn_client *c) {
    uint8_t storage[PWW_BUILD];
    struct pww_build b;
    struct pww_frame root, info;
    param_toggle_serial(c->node_params, c->n_node_params);
    pww_message_begin(&b, &root, storage);
    pww_pod_int(&b, SPA_NODE_CHANGE_MASK_FLAGS | SPA_NODE_CHANGE_MASK_PROPS |
            SPA_NODE_CHANGE_MASK_PARAMS);
    uint32_t n = 0;
    for (size_t i = 0; i < c->n_node_params; i++)
        if (c->node_params[i].len) n++;
    pww_pod_int(&b, (int32_t)n);
    for (size_t i = 0; i < c->n_node_params; i++)
        if (c->node_params[i].len)
            pww_build_raw_pod(&b, c->node_params[i].pod, c->node_params[i].len);
    pww_pod_struct_begin(&b, &info);
    pww_pod_int(&b, 0);
    pww_pod_int(&b, 2);
    pww_pod_long(&b, SPA_NODE_CHANGE_MASK_FLAGS | SPA_NODE_CHANGE_MASK_PROPS |
            SPA_NODE_CHANGE_MASK_PARAMS);
    pww_pod_long(&b, SPA_NODE_FLAG_RT);
    pww_pod_int(&b, 10);
    push_dict_item(&b, "media.type", "Audio");
    push_dict_item(&b, "media.category", "Playback");
    push_dict_item(&b, "media.role", "Music");
    push_dict_item(&b, "media.name", "Sounds");
    push_dict_item(&b, "media.class", "Stream/Output/Audio");
    push_dict_item(&b, "node.name", c->node_name);
    push_dict_item(&b, "node.latency", "256/48000");
    push_dict_item(&b, "node.autoconnect", "true");
    push_dict_item(&b, "node.want-driver", "true");
    push_dict_item(&b, "stream.is-live", "true");
    pww_pod_int(&b, 5);
    pww_pod_id(&b, SPA_PARAM_PROPINFO);
    pww_pod_int(&b, 0);
    pww_pod_id(&b, SPA_PARAM_PROPS);
    pww_pod_int(&b, (int32_t)node_flag(c, SPA_PARAM_PROPS, SPA_PARAM_INFO_WRITE));
    pww_pod_id(&b, SPA_PARAM_ENUMFORMAT);
    pww_pod_int(&b, (int32_t)node_flag(c, SPA_PARAM_ENUMFORMAT, SPA_PARAM_INFO_READ));
    pww_pod_id(&b, SPA_PARAM_FORMAT);
    pww_pod_int(&b, (int32_t)node_flag(c, SPA_PARAM_FORMAT, SPA_PARAM_INFO_WRITE));
    pww_pod_id(&b, SPA_PARAM_PROCESSLATENCY);
    pww_pod_int(&b, (int32_t)node_flag(c, SPA_PARAM_PROCESSLATENCY, SPA_PARAM_INFO_WRITE));
    pww_pod_struct_end(&b, &info);
    return pww_message_send(&c->wire, &b, &root, c->node_proxy, PW_CLIENT_NODE_METHOD_UPDATE);
}

static int send_port_update(struct pwn_client *c, unsigned port_id) {
    uint8_t storage[PWW_BUILD];
    struct pww_build b;
    struct pww_frame root, info;
    struct pwn_port *port = &c->ports[port_id];
    param_toggle_serial(port->params, port->n_params);
    pww_message_begin(&b, &root, storage);
    pww_pod_int(&b, SPA_DIRECTION_OUTPUT);
    pww_pod_int(&b, (int32_t)port_id);
    pww_pod_int(&b, SPA_PORT_CHANGE_MASK_FLAGS | SPA_PORT_CHANGE_MASK_RATE |
            SPA_PORT_CHANGE_MASK_PROPS | SPA_PORT_CHANGE_MASK_PARAMS);
    uint32_t n = 0;
    for (size_t i = 0; i < port->n_params; i++)
        if (port->params[i].len) n++;
    pww_pod_int(&b, (int32_t)n);
    for (size_t i = 0; i < port->n_params; i++)
        if (port->params[i].len)
            pww_build_raw_pod(&b, port->params[i].pod, port->params[i].len);
    pww_pod_struct_begin(&b, &info);
    pww_pod_long(&b, SPA_PORT_CHANGE_MASK_FLAGS | SPA_PORT_CHANGE_MASK_RATE |
            SPA_PORT_CHANGE_MASK_PROPS | SPA_PORT_CHANGE_MASK_PARAMS);
    pww_pod_long(&b, 0);
    pww_pod_int(&b, 0);
    pww_pod_int(&b, 1);
    pww_pod_int(&b, 3);
    push_dict_item(&b, "port.group", "stream.0");
    push_dict_item(&b, "format.dsp", "32 bit float mono audio");
    push_dict_item(&b, "audio.channel", port_id == 0 ? "FL" : "FR");
    pww_pod_int(&b, 9);
    pww_pod_id(&b, SPA_PARAM_ENUMFORMAT);
    pww_pod_int(&b, (int32_t)port_flag(c, port_id, SPA_PARAM_ENUMFORMAT, SPA_PARAM_INFO_READ));
    pww_pod_id(&b, SPA_PARAM_META);
    pww_pod_int(&b, (int32_t)port_flag(c, port_id, SPA_PARAM_META, SPA_PARAM_INFO_READ));
    pww_pod_id(&b, SPA_PARAM_IO);
    pww_pod_int(&b, (int32_t)port_flag(c, port_id, SPA_PARAM_IO, SPA_PARAM_INFO_READ));
    pww_pod_id(&b, SPA_PARAM_FORMAT);
    pww_pod_int(&b, (int32_t)port_flag(c, port_id, SPA_PARAM_FORMAT, SPA_PARAM_INFO_WRITE));
    pww_pod_id(&b, SPA_PARAM_BUFFERS);
    pww_pod_int(&b, (int32_t)port_flag(c, port_id, SPA_PARAM_BUFFERS, SPA_PARAM_INFO_READ));
    pww_pod_id(&b, SPA_PARAM_LATENCY);
    pww_pod_int(&b, (int32_t)port_flag(c, port_id, SPA_PARAM_LATENCY, SPA_PARAM_INFO_WRITE));
    pww_pod_id(&b, SPA_PARAM_TAG);
    pww_pod_int(&b, (int32_t)port_flag(c, port_id, SPA_PARAM_TAG, SPA_PARAM_INFO_WRITE));
    pww_pod_id(&b, SPA_PARAM_CAPABILITY);
    pww_pod_int(&b, (int32_t)port_flag(c, port_id, SPA_PARAM_CAPABILITY, 0));
    pww_pod_id(&b, SPA_PARAM_PEERCAPABILITY);
    pww_pod_int(&b, (int32_t)port_flag(c, port_id, SPA_PARAM_PEERCAPABILITY,
            SPA_PARAM_INFO_WRITE | SPA_PARAM_INFO_READ));
    pww_pod_struct_end(&b, &info);
    return pww_message_send(&c->wire, &b, &root, c->node_proxy, PW_CLIENT_NODE_METHOD_PORT_UPDATE);
}

static int send_set_active(struct pwn_client *c) {
    uint8_t storage[256];
    struct pww_build b;
    struct pww_frame root;
    pww_message_begin(&b, &root, storage);
    pww_pod_bool(&b, 1);
    return pww_message_send(&c->wire, &b, &root, c->node_proxy, PW_CLIENT_NODE_METHOD_SET_ACTIVE);
}

static void push_node_props(struct pww_build *b, struct pwn_client *c) {
    push_dict_item(b, "media.type", "Audio");
    push_dict_item(b, "media.category", "Playback");
    push_dict_item(b, "media.role", "Music");
    push_dict_item(b, "media.name", "Sounds");
    push_dict_item(b, "media.class", "Stream/Output/Audio");
    push_dict_item(b, "node.name", c->node_name);
    push_dict_item(b, "node.latency", "256/48000");
    push_dict_item(b, "node.autoconnect", "true");
    push_dict_item(b, "node.want-driver", "true");
    push_dict_item(b, "stream.is-live", "true");
}

static int send_create_object(struct pwn_client *c) {
    uint8_t storage[PWW_BUILD];
    struct pww_build b;
    struct pww_frame root, dict;
    pww_message_begin(&b, &root, storage);
    pww_pod_string(&b, "client-node");
    pww_pod_string(&b, "PipeWire:Interface:ClientNode");
    pww_pod_int(&b, 6);
    push_dict_begin(&b, &dict, 10);
    push_node_props(&b, c);
    push_dict_end(&b, &dict);
    pww_pod_int(&b, (int32_t)c->node_proxy);
    return pww_message_send(&c->wire, &b, &root, 0, PW_CORE_METHOD_CREATE_OBJECT);
}

static int send_hello(struct pwn_client *c) {
    uint8_t storage[256];
    struct pww_build b;
    struct pww_frame root;
    pww_message_begin(&b, &root, storage);
    pww_pod_int(&b, 4);
    return pww_message_send(&c->wire, &b, &root, 0, PW_CORE_METHOD_HELLO);
}

static int send_client_props(struct pwn_client *c) {
    uint8_t storage[1024];
    struct pww_build b;
    struct pww_frame root, dict;
    pww_message_begin(&b, &root, storage);
    push_dict_begin(&b, &dict, 2);
    push_dict_item(&b, "application.name", c->app_name);
    push_dict_item(&b, "application.process.binary", "tomoe");
    push_dict_end(&b, &dict);
    return pww_message_send(&c->wire, &b, &root, 1, PW_CLIENT_METHOD_UPDATE_PROPERTIES);
}

static int send_sync(struct pwn_client *c) {
    uint8_t storage[256];
    struct pww_build b;
    struct pww_frame root;
    pww_message_begin(&b, &root, storage);
    pww_pod_int(&b, 0);
    pww_pod_int(&b, (int32_t)(0x40000000u | (c->wire.seq & 0x3fffffffu)));
    return pww_message_send(&c->wire, &b, &root, 0, PW_CORE_METHOD_SYNC);
}

static int send_pong(struct pwn_client *c, uint32_t id, uint32_t seq) {
    uint8_t storage[256];
    struct pww_build b;
    struct pww_frame root;
    pww_message_begin(&b, &root, storage);
    pww_pod_int(&b, (int32_t)id);
    pww_pod_int(&b, (int32_t)seq);
    return pww_message_send(&c->wire, &b, &root, 0, PW_CORE_METHOD_PONG);
}

static void trigger_targets(struct pwn_client *c, uint64_t nsec) {
    for (size_t i = 0; i < c->n_targets; i++) {
        struct pwn_target *t = &c->targets[i];
        if (!t->active || !t->activation || t->size < PW_ACT_MINIMUM || t->fd < 0) continue;
        uint8_t *a = t->activation;
        int32_t *pending = (int32_t *)(a + PW_ACT_STATE + PW_STATE_PENDING);
        if (__atomic_sub_fetch(pending, 1, __ATOMIC_ACQ_REL) != 0) continue;
        uint32_t server_version = 0;
        if (t->size >= PW_ACT_SERVER_VERSION + 4)
            memcpy(&server_version, a + PW_ACT_SERVER_VERSION, 4);
        if (server_version >= 1) {
            uint32_t expected = PW_NODE_ACTIVATION_NOT_TRIGGERED;
            if (!__atomic_compare_exchange_n((uint32_t *)(a + PW_ACT_STATUS), &expected,
                    PW_NODE_ACTIVATION_TRIGGERED, false, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED))
                continue;
        } else {
            __atomic_store_n((uint32_t *)(a + PW_ACT_STATUS), PW_NODE_ACTIVATION_TRIGGERED,
                    __ATOMIC_RELEASE);
        }
        memcpy(a + PW_ACT_SIGNAL_TIME, &nsec, 8);
        eventfd_write1(t->fd);
    }
}

static void reset_resampler(struct pwn_client *c) {
    c->src_have = 0;
    c->src_base = 0;
    c->src_pos = 0;
    c->src_active = 0;
    c->src_ratio = 0;
}

static void resample_plan(struct pwn_client *c, uint32_t *requested, uint32_t *needed,
        int *resample) {
    uint32_t num = 1, denom = 48000;
    if (c->position) {
        memcpy(&num, (uint8_t *)c->position + SPA_CLOCK_RATE, 4);
        memcpy(&denom, (uint8_t *)c->position + SPA_CLOCK_RATE + 4, 4);
    }
    if (!num || !denom || denom == 48000u * num) {
        *needed = *requested;
        *resample = 0;
        return;
    }
    double ratio = 48000.0 * (double)num / (double)denom;
    if (!c->src_active || c->src_ratio != ratio) {
        reset_resampler(c);
        c->src_active = 1;
        c->src_ratio = ratio;
    }
    uint64_t keep = (uint64_t)c->src_pos;
    if (keep > c->src_base) {
        uint64_t drop64 = keep - c->src_base;
        if (drop64 > c->src_have) drop64 = c->src_have;
        uint32_t drop = (uint32_t)drop64;
        if (drop) {
            memmove(c->src[0], c->src[0] + drop, (c->src_have - drop) * sizeof(float));
            memmove(c->src[1], c->src[1] + drop, (c->src_have - drop) * sizeof(float));
            c->src_base = keep;
            c->src_have -= drop;
        }
    }
    uint32_t frames = 0;
    for (;;) {
        double last = c->src_pos +
            (*requested > 0 ? (double)(*requested - 1) * ratio : 0.0);
        uint64_t highest = (uint64_t)last + 1;
        uint64_t end = c->src_base + c->src_have;
        uint64_t want = highest + 1;
        frames = want > end ? (uint32_t)(want - end) : 0;
        if ((uint64_t)c->src_have + frames <= PWN_RING) break;
        uint32_t room = PWN_RING - c->src_have;
        uint64_t top = c->src_base + c->src_have + room - 1;
        if (top <= (uint64_t)c->src_pos || ratio <= 0.0) {
            *requested = 0;
            *needed = 0;
            *resample = 1;
            return;
        }
        uint32_t shrink = (uint32_t)(((double)top - c->src_pos) / ratio) + 1;
        if (shrink == 0 || shrink >= *requested) {
            *requested = 0;
            *needed = 0;
            *resample = 1;
            return;
        }
        *requested = shrink;
    }
    *needed = frames;
    *resample = 1;
}

static uint32_t port_requested(struct pwn_client *c, struct pwn_port *port) {
    if (port->ratematch) return port->ratematch->size;
    if (c->position) {
        uint32_t duration;
        memcpy(&duration, (uint8_t *)c->position + SPA_CLOCK_DURATION, 4);
        return duration;
    }
    return 0;
}

static uint32_t mix(struct pwn_client *c) {
    int active = 0;
    uint32_t requested = 0, capacity = 0;
    for (unsigned i = 0; i < 2; i++) {
        struct pwn_port *port = &c->ports[i];
        if (!port->io || port->n_slots == 0) continue;
        active++;
        if (port->io->status == SPA_STATUS_HAVE_DATA) return SPA_STATUS_HAVE_DATA;
        uint32_t frames = port_requested(c, port);
        if (frames == 0) return SPA_STATUS_NEED_DATA;
        if (requested == 0 || frames < requested) requested = frames;
        for (uint32_t k = 0; k < port->n_slots; k++) {
            uint32_t room = port->slots[k].datas[0].maxsize / (uint32_t)sizeof(float);
            if (room && (capacity == 0 || room < capacity)) capacity = room;
        }
    }
    if (!active) return SPA_STATUS_OK;
    if (capacity && requested > capacity) requested = capacity;
    if (requested == 0) return SPA_STATUS_NEED_DATA;
    uint32_t needed = requested;
    int resample = 0;
    resample_plan(c, &requested, &needed, &resample);
    if (!resample && c->src_active) reset_resampler(c);
    for (unsigned i = 0; i < 2; i++) {
        struct pwn_port *port = &c->ports[i];
        if (!port->io || port->n_slots == 0) continue;
        if (port->io->buffer_id < port->n_slots)
            port->free_list[port->n_free++] = port->io->buffer_id;
        if (port->n_free == 0) {
            port->io->buffer_id = SPA_ID_INVALID;
            port->io->status = SPA_STATUS_NEED_DATA;
            return SPA_STATUS_NEED_DATA;
        }
    }
    struct pwn_buffer buffer;
    memset(&buffer, 0, sizeof(buffer));
    buffer.requested = needed;
    buffer.channels = 2;
    for (unsigned i = 0; i < 2; i++) {
        struct pwn_port *port = &c->ports[i];
        if (!port->io || port->n_slots == 0) continue;
        uint32_t id = port->free_list[--port->n_free];
        struct pwn_slot *b = &port->slots[id];
        port->io->buffer_id = id;
        port->io->status = SPA_STATUS_HAVE_DATA;
        if (resample) {
            buffer.frames[i] = c->src[i] + c->src_have;
            buffer.maxsize[i] = needed * (uint32_t)sizeof(float);
            buffer.chunk[i] = &c->src_chunk[i];
        } else {
            buffer.frames[i] = b->datas[0].frames;
            buffer.maxsize[i] = b->datas[0].maxsize;
            buffer.chunk[i] = b->datas[0].chunk;
        }
    }
    if (resample) {
        c->src_chunk[0] = (struct pwn_chunk){ 0, 0, 0, 0 };
        c->src_chunk[1] = (struct pwn_chunk){ 0, 0, 0, 0 };
    }
    if (c->ops.process) c->ops.process(c->data, &buffer);
    if (!resample) {
        for (unsigned i = 0; i < 2; i++) {
            struct pwn_port *port = &c->ports[i];
            if (!port->io || port->n_slots == 0) continue;
            uint32_t id = port->io->buffer_id;
            if (id >= port->n_slots) continue;
            apply_gain(port->slots[id].datas[0].frames, requested, c->gain[i]);
        }
        return SPA_STATUS_HAVE_DATA;
    }
    for (unsigned i = 0; i < 2; i++)
        apply_gain(c->src[i] + c->src_have, needed, c->gain[i]);
    c->src_have += needed;
    float *outs[2] = { NULL, NULL };
    struct pwn_slot *slots[2] = { NULL, NULL };
    for (unsigned i = 0; i < 2; i++) {
        struct pwn_port *port = &c->ports[i];
        if (!port->io || port->n_slots == 0) continue;
        uint32_t id = port->io->buffer_id;
        if (id >= port->n_slots) continue;
        slots[i] = &port->slots[id];
        outs[i] = slots[i]->datas[0].frames;
    }
    double pos = c->src_pos;
    for (uint32_t n = 0; n < requested; n++) {
        uint64_t index = (uint64_t)pos;
        double frac = pos - (double)index;
        uint32_t off = (uint32_t)(index - c->src_base);
        for (unsigned i = 0; i < 2; i++) {
            if (!outs[i]) continue;
            float a = c->src[i][off], b2 = c->src[i][off + 1];
            outs[i][n] = (float)((double)a + ((double)b2 - (double)a) * frac);
        }
        pos += c->src_ratio;
    }
    c->src_pos = pos;
    for (unsigned i = 0; i < 2; i++) {
        if (!slots[i]) continue;
        slots[i]->datas[0].chunk->offset = 0;
        slots[i]->datas[0].chunk->stride = (int32_t)sizeof(float);
        slots[i]->datas[0].chunk->size = requested * (uint32_t)sizeof(float);
    }
    return SPA_STATUS_HAVE_DATA;
}

static void process_cycle(struct pwn_client *c) {
    if (!c->activation || c->activation_size < PW_ACT_MINIMUM) return;
    uint8_t *a = c->activation;
    uint64_t nsec = now_ns();
    uint32_t expected = PW_NODE_ACTIVATION_TRIGGERED;
    if (!__atomic_compare_exchange_n((uint32_t *)(a + PW_ACT_STATUS), &expected,
            PW_NODE_ACTIVATION_AWAKE, false, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED))
        return;
    memcpy(a + PW_ACT_AWAKE_TIME, &nsec, 8);
    int32_t status = (int32_t)mix(c);
    __atomic_store_n((int32_t *)(a + PW_ACT_STATE + PW_STATE_STATUS), status, __ATOMIC_RELEASE);
    expected = PW_NODE_ACTIVATION_AWAKE;
    if (__atomic_compare_exchange_n((uint32_t *)(a + PW_ACT_STATUS), &expected,
            PW_NODE_ACTIVATION_FINISHED, false, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) {
        uint64_t finish = now_ns();
        memcpy(a + PW_ACT_FINISH_TIME, &finish, 8);
    }
    trigger_targets(c, nsec);
}

static int handle_transport(struct pwn_client *c, struct pww_parse *p) {
    int64_t ridx, widx;
    uint32_t mem_id, offset, size;
    if (pww_parse_fd(p, &ridx) || pww_parse_fd(p, &widx)) return -EPROTO;
    if (pww_parse_int(p, &mem_id) || pww_parse_int(p, &offset) || pww_parse_int(p, &size)) return -EPROTO;
    if (ridx < 0 || ridx >= (int64_t)c->wire.n_fds) return -EPROTO;
    if (widx < 0 || widx >= (int64_t)c->wire.n_fds) return -EPROTO;
    c->process_fd = c->wire.fds[ridx];
    c->wire.fds[ridx] = -1;
    c->complete_fd = c->wire.fds[widx];
    c->wire.fds[widx] = -1;
    void *activation = NULL;
    size_t activation_size = 0;
    if (mem_id != SPA_ID_INVALID) {
        int res = pww_map_mem(&c->wire, mem_id, offset, size, 1, -1, &activation, &activation_size);
        if (res < 0) return res;
    }
    c->activation = activation;
    c->activation_size = activation_size;
    if (activation && activation_size >= PW_ACT_MINIMUM)
        __atomic_store_n((uint32_t *)((uint8_t *)activation + PW_ACT_STATUS),
                PW_NODE_ACTIVATION_FINISHED, __ATOMIC_RELEASE);
    return 0;
}

static int handle_set_io(struct pwn_client *c, struct pww_parse *p) {
    uint32_t id, memid, offset, size;
    if (pww_parse_id(p, &id) || pww_parse_int(p, &memid) || pww_parse_int(p, &offset) ||
        pww_parse_int(p, &size)) return -EPROTO;
    void *ptr = NULL;
    if (memid != SPA_ID_INVALID) {
        int res = pww_map_mem(&c->wire, memid, offset, size, 1, -1, &ptr, NULL);
        if (res < 0) return res;
    }
    if (id == SPA_IO_POSITION) {
        c->position = ptr;
        if (ptr && c->activation && c->activation_size >= 552) {
            uint32_t clock_id;
            memcpy(&clock_id, (uint8_t *)ptr + 4, 4);
            __atomic_store_n((uint32_t *)((uint8_t *)c->activation + 548), clock_id,
                    __ATOMIC_RELEASE);
        }
    } else if (id == SPA_IO_CLOCK) {
        c->clock = ptr;
    }
    return 0;
}

static int handle_set_activation(struct pwn_client *c, struct pww_parse *p) {
    uint32_t node_id, memid, offset, size;
    int64_t idx;
    if (pww_parse_int(p, &node_id) || pww_parse_fd(p, &idx) || pww_parse_int(p, &memid) ||
        pww_parse_int(p, &offset) || pww_parse_int(p, &size)) return -EPROTO;
    struct pwn_target *t = NULL;
    for (size_t i = 0; i < c->n_targets; i++)
        if (c->targets[i].id == node_id) t = &c->targets[i];
    if (memid == SPA_ID_INVALID) {
        if (t) {
            t->active = 0;
            t->activation = NULL;
            t->fd = -1;
        }
        return 0;
    }
    if (idx < 0 || idx >= (int64_t)c->wire.n_fds) return -EPROTO;
    if (!t) {
        if (c->n_targets >= PWN_MAX_TARGETS) return -ENOSPC;
        t = &c->targets[c->n_targets++];
        memset(t, 0, sizeof(*t));
        t->id = node_id;
    }
    void *ptr = NULL;
    size_t mapped = 0;
    int res = pww_map_mem(&c->wire, memid, offset, size, 1, -1, &ptr, &mapped);
    if (res < 0) return res;
    t->activation = ptr;
    t->size = mapped;
    t->fd = c->wire.fds[idx];
    c->wire.fds[idx] = -1;
    t->active = 1;
    return 0;
}

static int handle_use_buffers(struct pwn_client *c, struct pww_parse *p) {
    uint32_t direction, port_id, mix_id, flags, n;
    if (pww_parse_int(p, &direction) || pww_parse_int(p, &port_id) || pww_parse_int(p, &mix_id) ||
        pww_parse_int(p, &flags) || pww_parse_int(p, &n)) return -EPROTO;
    (void)mix_id;
    if (direction != SPA_DIRECTION_OUTPUT || port_id >= 2) return -ENOTSUP;
    if (n > PWN_MAX_BUFFERS) return -ENOSPC;
    struct pwn_port *port = &c->ports[port_id];
    port_clear(c, port_id);
    for (uint32_t i = 0; i < n; i++) {
        uint32_t mem_id, offset, size, n_metas, n_datas;
        if (pww_parse_int(p, &mem_id) || pww_parse_int(p, &offset) || pww_parse_int(p, &size) ||
            pww_parse_int(p, &n_metas)) return -EPROTO;
        if (n_metas > 8) return -ENOSPC;
        uint32_t meta_size[8];
        for (uint32_t j = 0; j < n_metas; j++) {
            uint32_t type;
            if (pww_parse_id(p, &type) || pww_parse_int(p, &meta_size[j])) return -EPROTO;
        }
        if (pww_parse_int(p, &n_datas)) return -EPROTO;
        if (n_datas != 1) return -ENOTSUP;
        uint32_t type, data, data_flags, mapoffset, maxsize;
        if (pww_parse_id(p, &type) || pww_parse_int(p, &data) || pww_parse_int(p, &data_flags) ||
            pww_parse_int(p, &mapoffset) || pww_parse_int(p, &maxsize)) return -EPROTO;
        (void)data_flags;
        void *base = NULL;
        if (mem_id == SPA_ID_INVALID) return -EPROTO;
        int res = pww_map_mem(&c->wire, mem_id, offset, size, 0, (int)port_id, &base, NULL);
        if (res < 0) return res;
        size_t meta_offset = 0;
        for (uint32_t j = 0; j < n_metas; j++)
            meta_offset += (meta_size[j] + 7u) & ~7u;
        struct pwn_slot *b = &port->slots[i];
        memset(b, 0, sizeof(*b));
        b->n_datas = 1;
        b->datas[0].chunk = (struct pwn_chunk *)((uint8_t *)base + meta_offset);
        if (type == SPA_DATA_MEMID) {
            int fd = pww_mem_fd(&c->wire, data);
            if (fd < 0) return -ENODEV;
            b->datas[0].frames = pww_map_fd(&c->wire, fd, mapoffset, maxsize, 0, (int)port_id);
            if (!b->datas[0].frames) return -ENOMEM;
        } else if (type == SPA_DATA_MEMPTR) {
            b->datas[0].frames = (float *)((uint8_t *)base + data);
        } else {
            return -ENOTSUP;
        }
        b->datas[0].maxsize = maxsize;
        port->free_list[i] = i;
    }
    port->n_slots = n;
    port->n_free = n;
    return 0;
}

static int handle_command(struct pwn_client *c, struct pww_parse *p) {
    uint32_t type, id;
    if (pww_parse_pod_object(p, &type, &id)) return -EPROTO;
    if (type != 0x30002) return 0;
    switch (id) {
    case SPA_NODE_COMMAND_START:
        if (c->activation && c->activation_size >= PW_ACT_MINIMUM)
            __atomic_store_n((uint32_t *)((uint8_t *)c->activation + PW_ACT_STATUS),
                    PW_NODE_ACTIVATION_FINISHED, __ATOMIC_RELEASE);
        if (c->paused) {
            c->paused = 0;
            c->streaming = 1;
            report(c, PWN_STREAMING, 0);
        }
        break;
    case SPA_NODE_COMMAND_PAUSE:
    case SPA_NODE_COMMAND_SUSPEND:
    case SPA_NODE_COMMAND_FLUSH:
        if (c->streaming && id != SPA_NODE_COMMAND_FLUSH) {
            c->streaming = 0;
            c->paused = 1;
            report(c, PWN_CONNECTING, 0);
        }
        break;
    default:
        break;
    }
    return 0;
}

static int node_param_changed(struct pwn_client *c, struct pww_parse *p) {
    uint32_t id, flags, type, body;
    const uint8_t *pod;
    if (pww_parse_id(p, &id) || pww_parse_int(p, &flags)) return -EPROTO;
    pod = pww_parse_pod(p, &type, &body);
    if (!pod || type != SPA_TYPE_OBJECT) return 0;
    if (id == SPA_PARAM_PROPS) {
        apply_props(c, pod, body);
        size_t len = build_props(c, c->props_param);
        if (len)
            add_param(c->node_params, &c->n_node_params, 5, SPA_PARAM_PROPS,
                    SPA_PARAM_INFO_READ, c->props_param, len);
    } else {
        add_param(c->node_params, &c->n_node_params, 5, id, SPA_PARAM_INFO_READ, pod, 8 + body);
    }
    int res = send_update(c);
    if (res < 0) return res;
    if (id == SPA_PARAM_PORTCONFIG) {
        uint8_t storage[256];
        struct pww_build props;
        struct pww_frame object;
        (void)props;
        (void)object;
        size_t len = build_props(c, storage);
        if (len) {
            add_param(c->node_params, &c->n_node_params, 5, SPA_PARAM_PROPS,
                    SPA_PARAM_INFO_READ, storage, len);
            res = send_update(c);
            if (res < 0) return res;
        }
    }
    return 0;
}

static int port_param_changed(struct pwn_client *c, struct pww_parse *p) {
    uint32_t direction, port_id, id, flags, type, body;
    const uint8_t *pod;
    if (pww_parse_int(p, &direction) || pww_parse_int(p, &port_id) || pww_parse_id(p, &id) ||
        pww_parse_int(p, &flags)) return -EPROTO;
    pod = pww_parse_pod(p, &type, &body);
    if (!pod || type != SPA_TYPE_OBJECT) return 0;
    if (direction != SPA_DIRECTION_OUTPUT || port_id >= 2) return 0;
    struct pwn_port *port = &c->ports[port_id];
    add_param(port->params, &port->n_params, 9, id, SPA_PARAM_INFO_READ, pod, 8 + body);
    return send_port_update(c, port_id);
}

static int handle_core_event(struct pwn_client *c, uint32_t opcode, struct pww_parse *p) {
    switch (opcode) {
    case PW_CORE_EVENT_PING: {
        uint32_t id, seq;
        if (pww_parse_int(p, &id) || pww_parse_int(p, &seq)) return -EPROTO;
        return send_pong(c, id, seq);
    }
    case PW_CORE_EVENT_ERROR: {
        uint32_t id, seq, res;
        const char *message;
        uint32_t len;
        if (pww_parse_int(p, &id) || pww_parse_int(p, &seq) || pww_parse_int(p, &res) ||
            pww_parse_string(p, &message, &len)) return -EPROTO;
        if (id == 0) return fault(c, (int)(int32_t)res);
        return fault(c, -EIO);
    }
    case PW_CORE_EVENT_ADD_MEM: {
        uint32_t id, type, flags;
        int64_t idx;
        if (pww_parse_int(p, &id) || pww_parse_id(p, &type) || pww_parse_fd(p, &idx) ||
            pww_parse_int(p, &flags)) return -EPROTO;
        if (idx < 0 || idx >= (int64_t)c->wire.n_fds) return -EPROTO;
        int fd = c->wire.fds[idx];
        c->wire.fds[idx] = -1;
        return pww_mem_add(&c->wire, id, fd);
    }
    case PW_CORE_EVENT_REMOVE_MEM: {
        uint32_t id;
        if (pww_parse_int(p, &id)) return -EPROTO;
        pww_mem_remove(&c->wire, id);
        return 0;
    }
    case PW_CORE_EVENT_BOUND_ID:
    case PW_CORE_EVENT_BOUND_PROPS: {
        uint32_t id, global_id;
        if (pww_parse_int(p, &id) || pww_parse_int(p, &global_id)) return -EPROTO;
        if (opcode == PW_CORE_EVENT_BOUND_PROPS && pww_parse_nested_dict(p)) return -EPROTO;
        if (id == c->node_proxy) {
            c->node_global = global_id;
            c->bound = 1;
            c->paused = 1;
        }
        return 0;
    }
    case PW_CORE_EVENT_DONE:
    case PW_CORE_EVENT_INFO:
    case PW_CORE_EVENT_REMOVE_ID:
    default:
        return 0;
    }
}

static int handle_node_event(struct pwn_client *c, uint32_t opcode, struct pww_parse *p) {
    switch (opcode) {
    case PW_CLIENT_NODE_EVENT_TRANSPORT:
        return handle_transport(c, p);
    case PW_CLIENT_NODE_EVENT_SET_IO:
        return handle_set_io(c, p);
    case PW_CLIENT_NODE_EVENT_SET_ACTIVATION:
        return handle_set_activation(c, p);
    case PW_CLIENT_NODE_EVENT_PORT_USE_BUFFERS:
        return handle_use_buffers(c, p);
    case PW_CLIENT_NODE_EVENT_PORT_SET_IO: {
        uint32_t direction, port_id, mix_id, id, memid, offset, size;
        if (pww_parse_int(p, &direction) || pww_parse_int(p, &port_id) || pww_parse_int(p, &mix_id) ||
            pww_parse_id(p, &id) || pww_parse_int(p, &memid) || pww_parse_int(p, &offset) ||
            pww_parse_int(p, &size)) return -EPROTO;
        (void)mix_id;
        if (direction != SPA_DIRECTION_OUTPUT || port_id >= 2) return 0;
        void *ptr = NULL;
        if (memid != SPA_ID_INVALID) {
            int res = pww_map_mem(&c->wire, memid, offset, size, 1, -1, &ptr, NULL);
            if (res < 0) return res;
        }
        if (id == SPA_IO_BUFFERS) c->ports[port_id].io = ptr;
        else if (id == SPA_IO_RATEMATCH) c->ports[port_id].ratematch = ptr;
        return 0;
    }
    case PW_CLIENT_NODE_EVENT_SET_PARAM:
        return node_param_changed(c, p);
    case PW_CLIENT_NODE_EVENT_PORT_SET_PARAM:
        return port_param_changed(c, p);
    case PW_CLIENT_NODE_EVENT_COMMAND:
        return handle_command(c, p);
    default:
        return 0;
    }
}


static void client_reset(struct pwn_client *c) {
    if (c->wire.fd >= 0) close(c->wire.fd);
    c->wire.fd = -1;
    if (c->process_fd >= 0) close(c->process_fd);
    if (c->complete_fd >= 0) close(c->complete_fd);
    c->process_fd = -1;
    c->complete_fd = -1;
    for (size_t i = 0; i < c->wire.n_fds; i++)
        if (c->wire.fds[i] >= 0) close(c->wire.fds[i]);
    c->wire.n_fds = 0;
    pww_unmap_all(&c->wire);
    pww_mem_clear(&c->wire);
    c->wire.in_len = 0;
    c->activation = NULL;
    c->activation_size = 0;
    c->position = NULL;
    c->clock = NULL;
    for (unsigned i = 0; i < 2; i++) {
        c->ports[i].io = NULL;
        c->ports[i].ratematch = NULL;
        c->ports[i].n_slots = 0;
        c->ports[i].n_free = 0;
        c->ports[i].n_params = 0;
    }
    reset_resampler(c);
    c->volume = 1.0f;
    c->mute = 0;
    c->n_channel_volumes = 0;
    c->soft_mute = 0;
    c->n_soft_volumes = 0;
    c->have_soft_volume = 0;
    c->gain[0] = c->gain[1] = 1.0f;
    memset(c->targets, 0, sizeof(c->targets));
    c->n_targets = 0;
    c->bound = 0;
    c->paused = 0;
    c->streaming = 0;
    c->faulted = 0;
    c->node_global = SPA_ID_INVALID;
    c->n_node_params = 0;
    c->wire.seq = 0;
}

static void prepare_params(struct pwn_client *c) {
    build_raw_format(c, c->raw_format, &c->raw_format_len);
    build_dsp_format(c->dsp_format, &c->dsp_format_len);
    build_io_param(c->io_param, &c->io_param_len);
    build_meta_param(c->meta_param, &c->meta_param_len);
    build_buffers_param(c->buffers_param, &c->buffers_param_len);
    add_param(c->node_params, &c->n_node_params, 5, SPA_PARAM_ENUMFORMAT, SPA_PARAM_INFO_READ,
            c->raw_format, c->raw_format_len);
    size_t props_len = build_props(c, c->props_param);
    if (props_len)
        add_param(c->node_params, &c->n_node_params, 5, SPA_PARAM_PROPS, SPA_PARAM_INFO_READ,
                c->props_param, props_len);
    for (unsigned i = 0; i < 2; i++) {
        struct pwn_port *port = &c->ports[i];
        add_param(port->params, &port->n_params, 9, SPA_PARAM_ENUMFORMAT,
                SPA_PARAM_INFO_READ, c->dsp_format, c->dsp_format_len);
        add_param(port->params, &port->n_params, 9, SPA_PARAM_META,
                SPA_PARAM_INFO_READ, c->meta_param, c->meta_param_len);
        add_param(port->params, &port->n_params, 9, SPA_PARAM_IO,
                SPA_PARAM_INFO_READ, c->io_param, c->io_param_len);
        add_param(port->params, &port->n_params, 9, SPA_PARAM_BUFFERS,
                SPA_PARAM_INFO_READ, c->buffers_param, c->buffers_param_len);
    }
}

static int client_setup(struct pwn_client *c) {
    int res = pww_connect(&c->wire);
    if (res < 0) return res;
    c->node_proxy = 2;
    c->node_global = SPA_ID_INVALID;
    prepare_params(c);
    if ((res = send_hello(c)) < 0) return res;
    if ((res = send_client_props(c)) < 0) return res;
    if ((res = send_create_object(c)) < 0) return res;
    if ((res = send_update(c)) < 0) return res;
    if ((res = send_port_update(c, 0)) < 0) return res;
    if ((res = send_port_update(c, 1)) < 0) return res;
    if ((res = send_set_active(c)) < 0) return res;
    return send_sync(c);
}

static int dispatch_client(void *data, uint32_t id, uint32_t opcode, struct pww_parse *p) {
    struct pwn_client *c = data;
    if (id == 0) return handle_core_event(c, opcode, p);
    if (id == c->node_proxy) return handle_node_event(c, opcode, p);
    return 0;
}

static void *client_thread(void *arg) {
    struct pwn_client *c = arg;
    sigset_t all, previous;
    sigfillset(&all);
    pthread_sigmask(SIG_SETMASK, &all, &previous);
    while (!c->quit) {
        int res = client_setup(c);
        if (res < 0) {
            report(c, PWN_RETRYING, res);
            struct pollfd p = { .fd = c->wake, .events = POLLIN };
            poll(&p, 1, 1000);
            client_reset(c);
            continue;
        }
        report(c, PWN_CONNECTING, 0);
        while (!c->quit) {
            struct pollfd p[3];
            int n = 0;
            int wake_index = n;
            p[n++] = (struct pollfd){ .fd = c->wake, .events = POLLIN };
            int fd_index = n;
            p[n++] = (struct pollfd){ .fd = c->wire.fd, .events = POLLIN };
            int process_index = -1;
            if (c->process_fd >= 0) {
                process_index = n;
                p[n++] = (struct pollfd){ .fd = c->process_fd, .events = POLLIN };
            }
            int ready = poll(p, (nfds_t)n, -1);
            if (ready < 0) {
                if (errno == EINTR) continue;
                res = -errno;
                break;
            }
            if (p[wake_index].revents & POLLIN) eventfd_read1(c->wake);
            if (p[fd_index].revents & (POLLIN | POLLHUP | POLLERR)) {
                res = pww_recv(&c->wire, dispatch_client, c);
                if (res < 0) break;
            }
            if (process_index >= 0 && (p[process_index].revents & POLLIN)) {
                eventfd_read1(c->process_fd);
                process_cycle(c);
            }
        }
        if (!c->quit) {
            if (!c->faulted) report(c, PWN_RETRYING, res);
            struct pollfd p = { .fd = c->wake, .events = POLLIN };
            poll(&p, 1, 1000);
        }
        client_reset(c);
    }
    pthread_sigmask(SIG_SETMASK, &previous, NULL);
    return NULL;
}

struct pwn_client *pwn_start(const char *app_name, const char *node_name,
        unsigned rate, unsigned channels, const struct pwn_ops *ops, void *data) {
    struct pwn_client *c = calloc(1, sizeof(*c));
    if (!c) return NULL;
    c->ops = *ops;
    c->data = data;
    c->app_name = app_name;
    c->node_name = node_name;
    c->rate = rate;
    c->channels = channels;
    c->wire.fd = -1;
    c->process_fd = -1;
    c->complete_fd = -1;
    c->volume = 1.0f;
    c->channel_volumes[0] = c->channel_volumes[1] = 1.0f;
    c->soft_volumes[0] = c->soft_volumes[1] = 1.0f;
    c->gain[0] = c->gain[1] = 1.0f;
    c->src[0] = calloc(PWN_RING, sizeof(float));
    c->src[1] = calloc(PWN_RING, sizeof(float));
    if (!c->src[0] || !c->src[1]) {
        free(c->src[0]);
        free(c->src[1]);
        free(c);
        return NULL;
    }
    c->wake = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (c->wake < 0) {
        free(c->src[0]);
        free(c->src[1]);
        free(c);
        return NULL;
    }
    if (pthread_create(&c->thread, NULL, client_thread, c) != 0) {
        close(c->wake);
        free(c->src[0]);
        free(c->src[1]);
        free(c);
        return NULL;
    }
    return c;
}

void pwn_stop(struct pwn_client *c) {
    if (!c) return;
    c->quit = 1;
    eventfd_write1(c->wake);
    pthread_join(c->thread, NULL);
    client_reset(c);
    close(c->wake);
    free(c->wire.in);
    free(c->src[0]);
    free(c->src[1]);
    free(c);
}
