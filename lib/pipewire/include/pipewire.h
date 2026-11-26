#ifndef TOMOE_PIPEWIRE_H
#define TOMOE_PIPEWIRE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
    SPA_TYPE_NONE = 1,
    SPA_TYPE_BOOL,
    SPA_TYPE_ID,
    SPA_TYPE_INT,
    SPA_TYPE_LONG,
    SPA_TYPE_FLOAT,
    SPA_TYPE_DOUBLE,
    SPA_TYPE_STRING,
    SPA_TYPE_BYTES,
    SPA_TYPE_RECTANGLE,
    SPA_TYPE_FRACTION,
    SPA_TYPE_BITMAP,
    SPA_TYPE_ARRAY,
    SPA_TYPE_STRUCT,
    SPA_TYPE_OBJECT,
    SPA_TYPE_SEQUENCE,
    SPA_TYPE_POINTER,
    SPA_TYPE_FD,
    SPA_TYPE_CHOICE,
    SPA_TYPE_POD,
};

enum {
    SPA_TYPE_OBJECT_PROPINFO = 0x40000 + 1,
    SPA_TYPE_OBJECT_PROPS = 0x40000 + 2,
    SPA_TYPE_OBJECT_FORMAT = 0x40000 + 3,
    SPA_TYPE_OBJECT_PARAMBUFFERS = 0x40000 + 4,
    SPA_TYPE_OBJECT_PARAMMETA = 0x40000 + 5,
    SPA_TYPE_OBJECT_PARAMIO = 0x40000 + 6,
    SPA_TYPE_OBJECT_PARAMPORTCONFIG = 0x40000 + 8,
    SPA_TYPE_OBJECT_PARAMROUTE = 0x40000 + 9,
    SPA_TYPE_OBJECT_PARAMLATENCY = 0x40000 + 11,
    SPA_TYPE_OBJECT_PARAMPROCESSLATENCY = 0x40000 + 12,
    SPA_TYPE_OBJECT_PARAMTAG = 0x40000 + 13,
    SPA_TYPE_OBJECT_PEERPARAM = 0x40000 + 14,
};

enum {
    SPA_PARAM_INVALID = 0,
    SPA_PARAM_PROPINFO = 1,
    SPA_PARAM_PROPS = 2,
    SPA_PARAM_ENUMFORMAT = 3,
    SPA_PARAM_FORMAT = 4,
    SPA_PARAM_BUFFERS = 5,
    SPA_PARAM_META = 6,
    SPA_PARAM_IO = 7,
    SPA_PARAM_ENUMPORTCONFIG = 10,
    SPA_PARAM_PORTCONFIG = 11,
    SPA_PARAM_ROUTE = 13,
    SPA_PARAM_LATENCY = 15,
    SPA_PARAM_PROCESSLATENCY = 16,
    SPA_PARAM_TAG = 17,
    SPA_PARAM_CAPABILITY = 19,
    SPA_PARAM_PEERCAPABILITY = 20,
};

enum {
    SPA_PARAM_INFO_SERIAL = 1,
    SPA_PARAM_INFO_READ = 2,
    SPA_PARAM_INFO_WRITE = 4,
};

enum {
    SPA_PROP_VOLUME = 65539,
    SPA_PROP_MUTE = 65540,
    SPA_PROP_CHANNELVOLUMES = 65544,
    SPA_PROP_SOFTMUTE = 65551,
    SPA_PROP_SOFTVOLUMES = 65552,
};

enum {
    SPA_PARAM_ROUTE_DIRECTION = 2,
    SPA_PARAM_ROUTE_DEVICE = 3,
    SPA_PARAM_ROUTE_PROPS = 10,
};

enum {
    SPA_FORMAT_MEDIATYPE = 1,
    SPA_FORMAT_MEDIASUBTYPE = 2,
    SPA_FORMAT_AUDIO_FORMAT = 65537,
    SPA_FORMAT_AUDIO_RATE = 65539,
    SPA_FORMAT_AUDIO_CHANNELS = 65540,
    SPA_FORMAT_AUDIO_POSITION = 65541,
};

enum {
    SPA_MEDIA_TYPE_AUDIO = 1,
    SPA_MEDIA_SUBTYPE_RAW = 1,
    SPA_MEDIA_SUBTYPE_DSP = 2,
    SPA_AUDIO_FORMAT_F32 = 283,
    SPA_AUDIO_FORMAT_F32P = 518,
    SPA_AUDIO_CHANNEL_FL = 3,
    SPA_AUDIO_CHANNEL_FR = 4,
};

enum {
    SPA_PARAM_IO_ID = 1,
    SPA_PARAM_IO_SIZE = 2,
    SPA_PARAM_META_TYPE = 1,
    SPA_PARAM_META_SIZE = 2,
    SPA_PARAM_BUFFERS_BUFFERS = 1,
    SPA_PARAM_BUFFERS_BLOCKS = 2,
    SPA_PARAM_BUFFERS_SIZE = 3,
    SPA_PARAM_BUFFERS_STRIDE = 4,
};

enum {
    SPA_IO_BUFFERS = 1,
    SPA_IO_CLOCK = 3,
    SPA_IO_POSITION = 7,
    SPA_IO_RATEMATCH = 8,
    SPA_META_HEADER = 1,
};

enum {
    SPA_STATUS_OK = 0,
    SPA_STATUS_NEED_DATA = 1,
    SPA_STATUS_HAVE_DATA = 2,
};

enum {
    SPA_TYPE_OBJECT_COMMANDNODE = 0x30002,
};

enum {
    SPA_NODE_COMMAND_SUSPEND = 0,
    SPA_NODE_COMMAND_PAUSE = 1,
    SPA_NODE_COMMAND_START = 2,
    SPA_NODE_COMMAND_FLUSH = 5,
};

enum {
    SPA_NODE_CHANGE_MASK_FLAGS = 1,
    SPA_NODE_CHANGE_MASK_PROPS = 2,
    SPA_NODE_CHANGE_MASK_PARAMS = 4,
    SPA_NODE_FLAG_RT = 1,
};

enum {
    SPA_PORT_CHANGE_MASK_FLAGS = 1,
    SPA_PORT_CHANGE_MASK_RATE = 2,
    SPA_PORT_CHANGE_MASK_PROPS = 4,
    SPA_PORT_CHANGE_MASK_PARAMS = 8,
};

enum {
    SPA_DATA_INVALID = 0,
    SPA_DATA_MEMPTR = 1,
    SPA_DATA_MEMID = 4,
};

enum {
    SPA_DIRECTION_INPUT = 0,
    SPA_DIRECTION_OUTPUT = 1,
    SPA_ID_INVALID = 0xffffffffu,
};

enum {
    PW_CORE_METHOD_HELLO = 1,
    PW_CORE_METHOD_SYNC = 2,
    PW_CORE_METHOD_PONG = 3,
    PW_CORE_METHOD_GET_REGISTRY = 5,
    PW_CORE_METHOD_CREATE_OBJECT = 6,
    PW_CORE_METHOD_DESTROY = 7,
};

enum {
    PW_CORE_EVENT_INFO = 0,
    PW_CORE_EVENT_DONE = 1,
    PW_CORE_EVENT_PING = 2,
    PW_CORE_EVENT_ERROR = 3,
    PW_CORE_EVENT_REMOVE_ID = 4,
    PW_CORE_EVENT_BOUND_ID = 5,
    PW_CORE_EVENT_ADD_MEM = 6,
    PW_CORE_EVENT_REMOVE_MEM = 7,
    PW_CORE_EVENT_BOUND_PROPS = 8,
};

enum {
    PW_CLIENT_METHOD_UPDATE_PROPERTIES = 2,
};

enum {
    PW_REGISTRY_METHOD_BIND = 1,
};

enum {
    PW_REGISTRY_EVENT_GLOBAL = 0,
    PW_REGISTRY_EVENT_GLOBAL_REMOVE = 1,
};

enum {
    PW_METADATA_EVENT_PROPERTY = 0,
};

enum {
    PW_NODE_METHOD_SUBSCRIBE_PARAMS = 1,
};

enum {
    PW_NODE_EVENT_INFO = 0,
    PW_NODE_EVENT_PARAM = 1,
};

enum {
    PW_DEVICE_METHOD_SUBSCRIBE_PARAMS = 1,
};

enum {
    PW_DEVICE_EVENT_PARAM = 1,
};

enum {
    PW_CLIENT_NODE_METHOD_UPDATE = 2,
    PW_CLIENT_NODE_METHOD_PORT_UPDATE = 3,
    PW_CLIENT_NODE_METHOD_SET_ACTIVE = 4,
    PW_CLIENT_NODE_METHOD_PORT_BUFFERS = 6,
};

enum {
    PW_CLIENT_NODE_EVENT_TRANSPORT = 0,
    PW_CLIENT_NODE_EVENT_SET_PARAM = 1,
    PW_CLIENT_NODE_EVENT_SET_IO = 2,
    PW_CLIENT_NODE_EVENT_EVENT = 3,
    PW_CLIENT_NODE_EVENT_COMMAND = 4,
    PW_CLIENT_NODE_EVENT_ADD_PORT = 5,
    PW_CLIENT_NODE_EVENT_REMOVE_PORT = 6,
    PW_CLIENT_NODE_EVENT_PORT_SET_PARAM = 7,
    PW_CLIENT_NODE_EVENT_PORT_USE_BUFFERS = 8,
    PW_CLIENT_NODE_EVENT_PORT_SET_IO = 9,
    PW_CLIENT_NODE_EVENT_SET_ACTIVATION = 10,
    PW_CLIENT_NODE_EVENT_PORT_SET_MIX_INFO = 11,
};

enum {
    PW_NODE_ACTIVATION_NOT_TRIGGERED = 0,
    PW_NODE_ACTIVATION_TRIGGERED = 1,
    PW_NODE_ACTIVATION_AWAKE = 2,
    PW_NODE_ACTIVATION_FINISHED = 3,
    PW_NODE_ACTIVATION_INACTIVE = 4,
};

enum {
    PW_ACT_STATUS = 0,
    PW_ACT_STATE = 8,
    PW_ACT_SIGNAL_TIME = 32,
    PW_ACT_AWAKE_TIME = 40,
    PW_ACT_FINISH_TIME = 48,
    PW_ACT_SERVER_VERSION = 544,
    PW_ACT_MINIMUM = 20,
};

enum { PW_STATE_STATUS = 0, PW_STATE_REQUIRED = 4, PW_STATE_PENDING = 8 };
enum { SPA_CLOCK_RATE = 80, SPA_CLOCK_DURATION = 96 };


#define PWW_HEADER 16u
#define PWW_MAX_MEM 128
#define PWW_MAX_MAPS 256
#define PWW_MAX_FDS 64
#define PWW_MAX_MESSAGE (1u << 20)
#define PWW_MAX_IN (2u * PWW_MAX_MESSAGE)
#define PWW_MAX_OUT (64u * 1024u)
#define PWW_BUILD (64u * 1024u)

struct pww_mem {
    uint32_t id;
    int fd;
};

struct pww_map {
    void *ptr;
    size_t delta;
    size_t len;
    int keep;
    int owner;
};

struct pww {
    int fd;
    uint32_t seq;
    uint8_t *in;
    size_t in_len;
    size_t in_cap;
    uint8_t *out;
    size_t out_len;
    size_t out_off;
    size_t out_cap;
    int connecting;
    int fds[PWW_MAX_FDS];
    size_t n_fds;
    struct pww_mem mem[PWW_MAX_MEM];
    size_t n_mem;
    struct pww_map maps[PWW_MAX_MAPS];
    size_t n_maps;
};

struct pww_build {
    uint8_t *data;
    uint32_t cap;
    uint32_t off;
    int fail;
};
struct pww_frame {
    uint32_t off;
};
struct pww_parse {
    const uint8_t *data;
    size_t size;
    size_t off;
    int fail;
};

typedef int (*pww_dispatch)(void *data, uint32_t id, uint32_t opcode, struct pww_parse *p);

void pww_build_init(struct pww_build *b, uint8_t *data, uint32_t cap);
void pww_build_bytes(struct pww_build *b, const void *data, uint32_t size);
void pww_build_zero(struct pww_build *b, uint32_t size);
void pww_build_u32(struct pww_build *b, uint32_t v);
void pww_build_u64(struct pww_build *b, uint64_t v);
void pww_build_align(struct pww_build *b);
void pww_pod_begin(struct pww_build *b, struct pww_frame *f);
void pww_pod_end(struct pww_build *b, struct pww_frame *f, uint32_t type);
void pww_pod_bool(struct pww_build *b, int v);
void pww_pod_id(struct pww_build *b, uint32_t v);
void pww_pod_int(struct pww_build *b, int32_t v);
void pww_pod_long(struct pww_build *b, int64_t v);
void pww_pod_fd(struct pww_build *b, int64_t v);
void pww_unmap(struct pww *w, void *addr);
void pww_unmap_owner(struct pww *w, int owner);
void pww_pod_float(struct pww_build *b, float v);
void pww_pod_array_floats(struct pww_build *b, const float *values, uint32_t n);
void pww_pod_string(struct pww_build *b, const char *s);
void pww_pod_struct_begin(struct pww_build *b, struct pww_frame *f);
void pww_pod_struct_end(struct pww_build *b, struct pww_frame *f);
void pww_pod_object_begin(struct pww_build *b, struct pww_frame *f, uint32_t type, uint32_t id);
void pww_pod_object_end(struct pww_build *b, struct pww_frame *f);
void pww_pod_prop(struct pww_build *b, uint32_t key, uint32_t flags);
void pww_pod_prop_id(struct pww_build *b, uint32_t key, uint32_t v);
void pww_pod_prop_int(struct pww_build *b, uint32_t key, int32_t v);
void pww_pod_array_ids(struct pww_build *b, const uint32_t *ids, uint32_t n);
void pww_pod_prop_array_ids(struct pww_build *b, uint32_t key, const uint32_t *ids, uint32_t n);
void pww_pod_prop_float(struct pww_build *b, uint32_t key, float v);
void pww_pod_prop_bool(struct pww_build *b, uint32_t key, int v);
void pww_pod_prop_array_floats(struct pww_build *b, uint32_t key, const float *values, uint32_t n);
void pww_build_raw_pod(struct pww_build *b, const void *pod, size_t len);
void pww_parse_init(struct pww_parse *p, const void *data, size_t size);
void pww_parse_align(struct pww_parse *p);
const uint8_t *pww_parse_pod(struct pww_parse *p, uint32_t *type, uint32_t *body);
int pww_parse_typed(struct pww_parse *p, uint32_t want, void *out, size_t n);
int pww_parse_int(struct pww_parse *p, uint32_t *v);
int pww_parse_id(struct pww_parse *p, uint32_t *v);
int pww_parse_fd(struct pww_parse *p, int64_t *v);
int pww_parse_string(struct pww_parse *p, const char **s, uint32_t *len);
int pww_parse_dict(struct pww_parse *p);
int pww_parse_nested_dict(struct pww_parse *p);
int pww_parse_pod_object(struct pww_parse *p, uint32_t *type, uint32_t *id);
int pww_socket_path(char *path, size_t size);
int pww_connect(struct pww *w);
int pww_connect_nb(struct pww *w);
int pww_flush(struct pww *w);
void pww_unmap_all(struct pww *w);
void pww_mem_clear(struct pww *w);
int pww_mem_add(struct pww *w, uint32_t id, int fd);
int pww_mem_fd(struct pww *w, uint32_t id);
int pww_mem_remove(struct pww *w, uint32_t id);
void *pww_map_fd(struct pww *w, int fd, uint32_t offset, uint32_t size, int keep, int owner);
int pww_map_mem(struct pww *w, uint32_t id, uint32_t offset, uint32_t size, int keep, int owner, void **out, size_t *outsize);
int pww_send_message(struct pww *w, uint32_t id, uint32_t opcode, struct pww_build *b);
void pww_message_begin(struct pww_build *b, struct pww_frame *root, uint8_t *storage);
int pww_send_message_fds(struct pww *w, uint32_t id, uint32_t opcode, struct pww_build *b, const int *fds, uint32_t n_fds);
int pww_message_send(struct pww *w, struct pww_build *b, struct pww_frame *root, uint32_t id, uint32_t opcode);
int pww_send_nb(struct pww *w, uint32_t id, uint32_t opcode, struct pww_build *b);
int pww_message_send_nb(struct pww *w, struct pww_build *b, struct pww_frame *root, uint32_t id, uint32_t opcode);
int pww_parse_messages(struct pww *w, pww_dispatch dispatch, void *data);
int pww_recv(struct pww *w, pww_dispatch dispatch, void *data);
int pww_recv_budget(struct pww *w, pww_dispatch dispatch, void *data, size_t max_msgs, size_t max_bytes);

void pww_pod_rectangle(struct pww_build *b, uint32_t width, uint32_t height);
void pww_pod_fraction(struct pww_build *b, uint32_t num, uint32_t denom);
enum {
    SPA_POD_PROP_FLAG_MANDATORY = 1u << 3,
    SPA_POD_PROP_FLAG_DONT_FIXATE = 1u << 4,
};

void pww_pod_choice_int(struct pww_build *b, uint32_t choice, const int32_t *values, uint32_t count);
void pww_pod_choice_long(struct pww_build *b, uint32_t choice, const int64_t *values, uint32_t count);
void pww_pod_choice_fraction(struct pww_build *b, uint32_t choice, const uint32_t *values, uint32_t count);
void pww_pod_prop_long(struct pww_build *b, uint32_t key, int64_t v);
void pww_pod_prop_long_flags(struct pww_build *b, uint32_t key, uint32_t flags, int64_t v);
void pww_pod_prop_rectangle(struct pww_build *b, uint32_t key, uint32_t width, uint32_t height);
void pww_pod_prop_fraction(struct pww_build *b, uint32_t key, uint32_t num, uint32_t denom);
void pww_pod_prop_choice_int(struct pww_build *b, uint32_t key, uint32_t choice, const int32_t *values, uint32_t count);
void pww_pod_prop_choice_long(struct pww_build *b, uint32_t key, uint32_t flags, uint32_t choice, const int64_t *values, uint32_t count);
void pww_pod_prop_choice_fraction(struct pww_build *b, uint32_t key, uint32_t choice, const uint32_t *values, uint32_t count);

enum pwn_state { PWN_CONNECTING, PWN_STREAMING, PWN_RETRYING, PWN_FAILED };

struct pwn_chunk {
    uint32_t offset;
    uint32_t size;
    int32_t stride;
    int32_t flags;
};

struct pwn_buffer {
    float *frames[2];
    uint32_t maxsize[2];
    struct pwn_chunk *chunk[2];
    uint32_t requested;
    unsigned channels;
};

struct pwn_ops {
    void (*state)(void *data, int state, int error);
    void (*process)(void *data, struct pwn_buffer *buffer);
    bool (*active)(void *data);
};

struct pwn_client;

struct pwn_client *pwn_start(const char *app_name, const char *node_name,
        unsigned rate, unsigned channels, const struct pwn_ops *ops, void *data);
void pwn_stop(struct pwn_client *client);
void pwn_wake(struct pwn_client *client);

#endif
