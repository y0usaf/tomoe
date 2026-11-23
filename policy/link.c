#include "sidecar.h"
#include "pv.h"
#include "wayland.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

enum { HANDLE_MAX = 4096, DEPTH_MAX = 512, LIST_MAX = 1 << 22, PENDING_FLUSH = 256 * 1024,
 };

struct message {
    struct tpl_hdr hdr;
    uint8_t *payload;
};

static const struct prim_info {
    const char *name;
    int min, max, cls;
} prims[TPL_PRIM_COUNT] = {
#define TPL_INFO(id, name, min, max, cls) [TPL_PRIM_##id] = { name, min, max, cls },
    TOMOE_PRIMITIVES(TPL_INFO)
#undef TPL_INFO
};

static int link_fd = -1;
static struct pv_buf pending, held;
static uint32_t pending_calls;
static bool mutated;
static uint32_t next_seq = 1;
static uint8_t *in_buf;
static size_t in_length, in_capacity;
static struct message *queue;
static size_t queue_count, queue_capacity;
static struct tpl_state header;
static lisp_value handles[HANDLE_MAX];
static uint32_t handle_count;
static bool handles_on;
static bool ready_sent;
struct dirty_name {
    char *name;
};
static char **dirty_names;
static size_t dirty_count;
static bool snapshot_reset_sent;
static uint32_t boot_flags, boot_delay_ms;
static bool in_batch;
static uint64_t event_serial, effect_counter, batch_effects;
static char *boot_config;
static bool hello_seen;

struct ident_entry {
    uint32_t id;
    char *identifier;
    int64_t pid;
    bool background;
};
static struct ident_entry *idents;
static size_t ident_count, ident_capacity;

static void snapshot_flush(void);
static lisp_value resume_dirty(int argc, lisp_value *argv);
static bool keyed_batch;
static uint32_t next_watch_id = 1;
static void send_now(uint16_t type, uint32_t seq, const void *a, size_t a_length, const void *b,
    size_t b_length);

static void fatal_link(const char *why) {
    fprintf(stderr, "tomoe-policy: %s; exiting\n", why);
    _exit(1);
}

void core_send_frame(uint16_t type, const void *a, size_t a_length, const void *b, size_t b_length) {
    if (link_fd >= 0) send_now(type, 0, a, a_length, b, b_length);
}

bool core_connected(void) {
    return link_fd >= 0;
}

static void write_all(const void *data, size_t length) {
    const uint8_t *bytes = data;
    while (length) {
        ssize_t n = send(link_fd, bytes, length, MSG_NOSIGNAL);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) fatal_link("the compositor link closed");
        bytes += n;
        length -= (size_t)n;
    }
}

static void frame_put(struct pv_buf *buf, uint16_t type, uint32_t seq, const void *a, size_t a_length,
        const void *b, size_t b_length) {
    struct tpl_hdr hdr = { TPL_MAGIC, TPL_VERSION, type, (uint32_t)(a_length + b_length), seq };
    pv_buf_put(buf, &hdr, sizeof(hdr));
    if (a_length) pv_buf_put(buf, a, a_length);
    if (b_length) pv_buf_put(buf, b, b_length);
}

uint32_t core_watch_fd(int fd) {
    if (link_fd < 0 || fd < 0) return 0;
    core_flush();
    uint32_t id = next_watch_id++;
    struct tpl_hdr hdr = { TPL_MAGIC, TPL_VERSION, TPL_WATCH, sizeof(id), 0 };
    uint8_t frame[sizeof(hdr) + sizeof(id)];
    memcpy(frame, &hdr, sizeof(hdr));
    memcpy(frame + sizeof(hdr), &id, sizeof(id));
    char control[CMSG_SPACE(sizeof(int))];
    memset(control, 0, sizeof(control));
    struct iovec iov = { frame, sizeof(frame) };
    struct msghdr message = { .msg_iov = &iov, .msg_iovlen = 1, .msg_control = control,
        .msg_controllen = sizeof(control) };
    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&message);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    cmsg->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(cmsg), &fd, sizeof(int));
    ssize_t sent;
    do sent = sendmsg(link_fd, &message, MSG_NOSIGNAL);
    while (sent < 0 && errno == EINTR);
    if (sent <= 0) fatal_link("the compositor link closed");
    if ((size_t)sent < sizeof(frame)) write_all(frame + sent, sizeof(frame) - (size_t)sent);
    return id;
}

void core_unwatch_fd(uint32_t id) {
    if (link_fd < 0 || !id) return;
    send_now(TPL_UNWATCH, 0, &id, sizeof(id), NULL, 0);
}

static void send_now(uint16_t type, uint32_t seq, const void *a, size_t a_length, const void *b,
        size_t b_length) {
    core_flush();
    struct pv_buf frame = { 0 };
    frame_put(&frame, type, seq, a, a_length, b, b_length);
    if (frame.failed) fatal_link("out of memory framing a message");
    write_all(frame.data, frame.length);
    pv_buf_free(&frame);
}

void core_flush(void) {
    if (!pending.length || link_fd < 0) return;
    write_all(pending.data, pending.length);
    pending.length = 0;
    pending_calls = 0;
}

static void encode(struct pv_buf *buf, lisp_value value, int depth) {
    if (depth > DEPTH_MAX) {
        pv_buf_u8(buf, PV_NIL);
        return;
    }
    if (lisp_nilp(value)) {
        pv_buf_u8(buf, PV_NIL);
    } else if (value == lisp_t()) {
        pv_buf_u8(buf, PV_T);
    } else if (lisp_intp(value)) {
        int64_t i = lisp_get_int(value);
        pv_buf_u8(buf, PV_INT);
        pv_buf_put(buf, &i, 8);
    } else if (lisp_doublep(value)) {
        double d = lisp_get_double(value);
        pv_buf_u8(buf, PV_DOUBLE);
        pv_buf_put(buf, &d, 8);
    } else if (lisp_stringp(value)) {
        size_t length = 0;
        const char *text = lisp_get_string(value, &length);
        pv_buf_u8(buf, PV_STRING);
        pv_buf_u32(buf, (uint32_t)length);
        pv_buf_put(buf, text, length);
    } else if (lisp_keywordp(value)) {
        const char *name = lisp_symbol_name(value);
        uint32_t length = (uint32_t)strlen(name);
        pv_buf_u8(buf, PV_KEYWORD);
        pv_buf_u32(buf, length);
        pv_buf_put(buf, name, length);
    } else if (lisp_consp(value)) {
        uint32_t count = 0;
        lisp_value tail = value;
        while (lisp_consp(tail) && count < LIST_MAX) {
            count++;
            tail = lisp_cdr(tail);
        }
        pv_buf_u8(buf, PV_LIST);
        pv_buf_u32(buf, count);
        lisp_value cell = value;
        for (uint32_t i = 0; i < count; i++) {
            encode(buf, lisp_car(cell), depth + 1);
            cell = lisp_cdr(cell);
        }
        encode(buf, lisp_consp(tail) ? lisp_nil() : tail, depth + 1);
    } else {
        uint32_t handle = UINT32_MAX;
        if (handles_on && handle_count < HANDLE_MAX) {
            handle = handle_count;
            handles[handle_count++] = value;
        }
        if (lisp_symbolp(value)) {
            const char *name = lisp_symbol_name(value);
            uint32_t length = (uint32_t)strlen(name);
            pv_buf_u8(buf, PV_SYMBOL);
            pv_buf_u32(buf, handle);
            pv_buf_u32(buf, length);
            pv_buf_put(buf, name, length);
        } else {
            uint8_t number = lisp_numberp(value) ? 1 : 0;
            double real = number ? lisp_get_double(value) : 0.0;
            pv_buf_u8(buf, PV_OPAQUE);
            pv_buf_u32(buf, handle);
            pv_buf_u8(buf, number);
            pv_buf_put(buf, &real, 8);
        }
    }
}

static bool take(const uint8_t *data, size_t length, size_t *offset, void *out, size_t size) {
    if (length - *offset < size) return false;
    memcpy(out, data + *offset, size);
    *offset += size;
    return true;
}

static int decode(const uint8_t *data, size_t length, size_t *offset, lisp_value *out, int depth) {
    uint8_t tag;
    if (depth > DEPTH_MAX || !take(data, length, offset, &tag, 1)) return -1;
    uint32_t count, handle;
    switch (tag) {
    case PV_NIL: *out = lisp_nil(); return 0;
    case PV_T: *out = lisp_t(); return 0;
    case PV_INT: {
        int64_t i;
        if (!take(data, length, offset, &i, 8)) return -1;
        *out = lisp_int(i);
        return 0;
    }
    case PV_DOUBLE: {
        double d;
        if (!take(data, length, offset, &d, 8)) return -1;
        *out = lisp_double(d);
        return 0;
    }
    case PV_STRING:
    case PV_KEYWORD:
    case PV_READ: {
        if (!take(data, length, offset, &count, 4) || length - *offset < count) return -1;
        const char *text = (const char *)data + *offset;
        *offset += count;
        if (tag == PV_STRING) {
            *out = lisp_string(text, count);
        } else if (tag == PV_KEYWORD) {
            char name[256];
            size_t n = count < sizeof(name) - 1 ? count : sizeof(name) - 1;
            memcpy(name, text, n);
            name[n] = 0;
            *out = lisp_keyword(name);
        } else {
            char error[256];
            if (lisp_read_data(text, count, out, error, sizeof(error))) *out = lisp_nil();
        }
        return 0;
    }
    case PV_SYMBOL:
        if (!take(data, length, offset, &handle, 4) || !take(data, length, offset, &count, 4) ||
                length - *offset < count) return -1;
        *offset += count;
        *out = handle < handle_count ? handles[handle] : lisp_nil();
        return 0;
    case PV_OPAQUE: {
        uint8_t number;
        double real;
        if (!take(data, length, offset, &handle, 4) || !take(data, length, offset, &number, 1) ||
                !take(data, length, offset, &real, 8)) return -1;
        *out = handle < handle_count ? handles[handle] : number ? lisp_double(real) : lisp_nil();
        return 0;
    }
    case PV_LIST: {
        if (!take(data, length, offset, &count, 4) || count > length - *offset) return -1;
        lisp_value head = lisp_nil(), last = lisp_nil();
        for (uint32_t i = 0; i < count; i++) {
            lisp_value item;
            if (decode(data, length, offset, &item, depth + 1)) return -1;
            lisp_value cell = lisp_cons(item, lisp_nil());
            if (lisp_nilp(last)) head = cell;
            else lisp_set_cdr(last, cell);
            last = cell;
        }
        lisp_value tail;
        if (decode(data, length, offset, &tail, depth + 1)) return -1;
        if (lisp_nilp(last)) head = tail;
        else if (!lisp_nilp(tail)) lisp_set_cdr(last, tail);
        *out = head;
        return 0;
    }
    }
    return -1;
}

static uint64_t effect_id(int prim) {
    if (!in_batch || !tpl_one_shot(prim)) return 0;
    effect_counter++;
    batch_effects++;
    if (boot_flags & TPL_HELLO_TEST_LOCAL_IDS) return (uint64_t)prim + 1;
    if (!event_serial || effect_counter >= 1u << 20) return 0;
    return event_serial << 20 | effect_counter;
}

static void frame_call(struct pv_buf *buf, int prim, uint16_t flags, uint32_t seq, int argc,
        const lisp_value *argv) {
    struct pv_buf body = { 0 };
    struct tpl_call call = { (uint16_t)prim, flags, (uint32_t)argc, effect_id(prim) };
    pv_buf_put(&body, &call, sizeof(call));
    for (int i = 0; i < argc; i++) encode(&body, argv[i], 0);
    if (body.failed || body.length > TPL_MAX_PAYLOAD) {
        pv_buf_free(&body);
        lisp_error("Cannot encode the arguments of %s.", prims[prim].name);
    }
    frame_put(buf, TPL_CALL, seq, body.data, body.length, NULL, 0);
    pv_buf_free(&body);
}

void core_send(int prim, int argc, const lisp_value *argv) {
    if (link_fd < 0) return;
    handles_on = false;
    if ((boot_flags & TPL_HELLO_TEST_CRASH_BEFORE_FLUSH) && in_batch && tpl_one_shot(prim)) {
        frame_call(&held, prim, 0, next_seq++, argc, argv);
        return;
    }
    frame_call(&pending, prim, 0, next_seq++, argc, argv);
    pending_calls++;
    mutated = true;
    if (pending.failed) fatal_link("out of memory queueing a call");
    if (pending.length > PENDING_FLUSH) core_flush();
}

static bool read_some(int timeout_ms) {
    if (in_capacity - in_length < 65536) {
        size_t capacity = in_capacity ? in_capacity * 2 : 262144;
        while (capacity - in_length < 65536) capacity *= 2;
        uint8_t *grown = realloc(in_buf, capacity);
        if (!grown) fatal_link("out of memory reading the link");
        in_buf = grown;
        in_capacity = capacity;
    }
    if (timeout_ms >= 0) {
        struct pollfd fd = { link_fd, POLLIN, 0 };
        int ready = poll(&fd, 1, timeout_ms);
        if (ready == 0) return false;
    }
    ssize_t n = recv(link_fd, in_buf + in_length, in_capacity - in_length,
        timeout_ms == 0 ? MSG_DONTWAIT : 0);
    if (n < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) return false;
    if (n <= 0) fatal_link("the compositor closed the link");
    in_length += (size_t)n;
    return true;
}

static bool next_frame(struct message *out) {
    if (in_length < sizeof(struct tpl_hdr)) return false;
    struct tpl_hdr hdr;
    memcpy(&hdr, in_buf, sizeof(hdr));
    if (hdr.magic != TPL_MAGIC) fatal_link("the compositor sent a malformed message");
    if (hdr.version != TPL_VERSION) fatal_link("the compositor speaks another protocol version");
    if (hdr.len > TPL_MAX_PAYLOAD) fatal_link("the compositor sent an oversized message");
    size_t frame = sizeof(hdr) + hdr.len;
    if (in_length < frame) return false;
    out->hdr = hdr;
    out->payload = malloc(hdr.len ? hdr.len : 1);
    if (!out->payload) fatal_link("out of memory reading a message");
    memcpy(out->payload, in_buf + sizeof(hdr), hdr.len);
    in_length -= frame;
    if (in_length) memmove(in_buf, in_buf + frame, in_length);
    return true;
}

static void queue_push(struct message *message) {
    if (message->hdr.type == TPL_PING) {
        free(message->payload);
        send_now(TPL_PONG, message->hdr.seq, NULL, 0, NULL, 0);
        return;
    }
    if (queue_count == queue_capacity) {
        size_t capacity = queue_capacity ? queue_capacity * 2 : 16;
        struct message *grown = realloc(queue, capacity * sizeof(*grown));
        if (!grown) fatal_link("out of memory queueing a message");
        queue = grown;
        queue_capacity = capacity;
    }
    queue[queue_count++] = *message;
}

int core_call(int prim, int argc, const lisp_value *argv, lisp_value *result, char *error,
        size_t error_size) {
    *result = lisp_nil();
    if (link_fd < 0) {
        snprintf(error, error_size, "The compositor is not connected.");
        return -1;
    }
    uint32_t seq = next_seq++;
    handles_on = true;
    handle_count = 0;
    frame_call(&pending, prim, TPL_CALL_SYNC, seq, argc, argv);
    handles_on = false;
    core_flush();
    for (;;) {
        struct message message;
        while (!next_frame(&message)) read_some(-1);
        if (message.hdr.type != TPL_REPLY) {
            queue_push(&message);
            continue;
        }
        if (message.hdr.seq != seq) {
            free(message.payload);
            continue;
        }
        struct tpl_reply head;
        int status = -1;
        if (message.hdr.len >= sizeof(head)) {
            memcpy(&head, message.payload, sizeof(head));
            if (prim == TPL_PRIM_FENCE || prim == TPL_PRIM_PRESENT_APPLY) {
                header = head.state;
                mutated = false;
            }
            size_t offset = sizeof(head);
            lisp_value value;
            if (!decode(message.payload, message.hdr.len, &offset, &value, 0)) {
                if (head.status == TPL_OK) {
                    *result = value;
                    status = 0;
                } else {
                    size_t length = 0;
                    const char *text = lisp_stringp(value) ? lisp_get_string(value, &length) : "";
                    snprintf(error, error_size, "%.*s", (int)length, text);
                }
            } else {
                snprintf(error, error_size, "The compositor sent a malformed reply.");
            }
        }
        handle_count = 0;
        free(message.payload);
        struct message more;
        while (next_frame(&more)) queue_push(&more);
        return status;
    }
}

static const struct tpl_state *fresh_header(void) {
    if (mutated) {
        lisp_value ignored;
        char error[512];
        if (core_call(TPL_PRIM_FENCE, 0, NULL, &ignored, error, sizeof(error)))
            lisp_error("%s", error);
    }
    return &header;
}

static void ident_store(uint32_t id, const char *identifier, size_t length, int64_t pid, bool background) {
    struct ident_entry *entry = NULL;
    for (size_t i = 0; i < ident_count; i++)
        if (idents[i].id == id) entry = &idents[i];
    if (!entry) {
        if (ident_count == ident_capacity) {
            size_t capacity = ident_capacity ? ident_capacity * 2 : 64;
            struct ident_entry *grown = realloc(idents, capacity * sizeof(*grown));
            if (!grown) return;
            idents = grown;
            ident_capacity = capacity;
        }
        entry = &idents[ident_count++];
        entry->id = id;
        entry->identifier = NULL;
    }
    free(entry->identifier);
    entry->identifier = identifier ? strndup(identifier, length) : NULL;
    entry->pid = pid;
    entry->background = background;
}

static void ident_forget(uint32_t id) {
    for (size_t i = 0; i < ident_count; i++) {
        if (idents[i].id != id) continue;
        free(idents[i].identifier);
        idents[i] = idents[--ident_count];
        return;
    }
}

lisp_value policy_cached_identifier(uint32_t id, bool *found) {
    for (size_t i = 0; i < ident_count; i++) {
        if (idents[i].id != id) continue;
        *found = true;
        return idents[i].identifier ? lisp_cstring(idents[i].identifier) : lisp_nil();
    }
    *found = false;
    return lisp_nil();
}

static void absorb_aux(lisp_value event, lisp_value aux) {
    if (lisp_eq_keyword(lisp_getf(event, "TYPE"), "UNMAP")) {
        ident_forget((uint32_t)lisp_get_uint(lisp_getf(event, "ID")));
        return;
    }
    if (!lisp_consp(aux)) return;
    uint32_t id = (uint32_t)lisp_get_uint(lisp_car(aux));
    lisp_value identifier = lisp_car(lisp_cdr(aux));
    bool background = !lisp_nilp(lisp_car(lisp_cdr(lisp_cdr(aux))));
    size_t length = 0;
    const char *text = lisp_stringp(identifier) ? lisp_get_string(identifier, &length) : NULL;
    ident_store(id, text, length, lisp_get_int(lisp_getf(event, "PID")), background);
}

static lisp_value proxy(int prim, int argc, lisp_value *argv) {
    const struct prim_info *info = &prims[prim];
    if (info->cls == TPL_SYNC) {
        lisp_value result;
        char error[1024];
        if (core_call(prim, argc, argv, &result, error, sizeof(error))) lisp_error("%s", error);
        return result;
    }
    core_send(prim, argc, argv);
    return info->cls == TPL_PIPE ? lisp_nil() : lisp_int(1);
}

#define TPL_PROXY(id, name, min, max, cls) \
    static lisp_value proxy_##id(int argc, lisp_value *argv) { return proxy(TPL_PRIM_##id, argc, argv); }
TOMOE_PRIMITIVES(TPL_PROXY)
#undef TPL_PROXY

static lisp_value window_identifier(int argc, lisp_value *argv) {
    bool found;
    lisp_value cached = policy_cached_identifier((uint32_t)lisp_get_uint(argv[0]), &found);
    return found ? cached : proxy(TPL_PRIM_WINDOW_IDENTIFIER, argc, argv);
}

static lisp_value background_window_p(int argc, lisp_value *argv) {
    if (!lisp_intp(argv[0])) return lisp_nil();
    int64_t pid = lisp_get_int(argv[0]);
    for (size_t i = ident_count; i > 0; i--)
        if (idents[i - 1].pid == pid) return lisp_bool(idents[i - 1].background);
    return proxy(TPL_PRIM_BACKGROUND_WINDOW_P, argc, argv);
}

static lisp_value present_setting(int argc, lisp_value *argv) {
    if (lisp_stringp(argv[0])) {
        const char *name = lisp_get_string(argv[0], NULL);
        const char *suffix = "budgets-lisp-heap-mb";
        double value = lisp_get_double(argv[1]);
        if (name && !strcmp(name, suffix) && value > 0)
            lisp_set_region_limit((size_t)value * 1024 * 1024);
    }
    return proxy(TPL_PRIM_PRESENT_SETTING, argc, argv);
}

static lisp_value event_begin(int argc, lisp_value *argv) {
    event_serial = lisp_intp(argv[0]) ? (uint64_t)lisp_get_int(argv[0]) : 0;
    effect_counter = 0;
    return lisp_nil();
}

static lisp_value header_focus(int argc, lisp_value *argv) {
    return lisp_uint(fresh_header()->keyboard_focus);
}

static lisp_value header_grab_id(int argc, lisp_value *argv) {
    return lisp_uint(fresh_header()->grab_id);
}

static lisp_value header_grab_mode(int argc, lisp_value *argv) {
    return lisp_int(fresh_header()->grab_mode);
}

static lisp_value header_outputs_revision(int argc, lisp_value *argv) {
    return lisp_uint(fresh_header()->outputs_revision);
}

static lisp_value header_activity_serial(int argc, lisp_value *argv) {
    return lisp_uint(fresh_header()->activity_serial);
}

static lisp_value header_now_ms(int argc, lisp_value *argv) {
    return lisp_uint(header.mono_ns / 1000000u);
}

static lisp_value header_strftime(int argc, lisp_value *argv) {
    const char *format = lisp_stringp(argv[0]) ? lisp_get_string(argv[0], NULL) : "";
    time_t now = header.real_ns ? (time_t)(header.real_ns / 1000000000ll) : time(NULL);
    struct tm broken;
    localtime_r(&now, &broken);
    char buffer[1024];
    size_t written = strftime(buffer, sizeof(buffer), format ? format : "", &broken);
    return lisp_string(buffer, written);
}

void policy_register_proxies(void) {
#define TPL_DEFINE(id, name, min, max, cls) \
    if (TPL_PRIM_##id != TPL_PRIM_FENCE && TPL_PRIM_##id != TPL_PRIM_MEMORY) \
        lisp_define_primitive(name, proxy_##id, min, max);
    TOMOE_PRIMITIVES(TPL_DEFINE)
#undef TPL_DEFINE
    lisp_define_primitive("%window-identifier", window_identifier, 1, 1);
    lisp_define_primitive("%background-window-p", background_window_p, 1, 1);
    lisp_define_primitive("%present-setting", present_setting, 2, 2);
    lisp_define_primitive("%event-begin", event_begin, 1, 1);
    lisp_define_primitive("%resume-dirty", resume_dirty, 1, 1);
    lisp_define_primitive("%keyboard-focus", header_focus, 0, 0);
    lisp_define_primitive("%grab-id", header_grab_id, 0, 0);
    lisp_define_primitive("%grab-mode", header_grab_mode, 0, 0);
    lisp_define_primitive("%outputs-revision", header_outputs_revision, 0, 0);
    lisp_define_primitive("%activity-serial", header_activity_serial, 0, 0);
    lisp_define_primitive("%now-ms", header_now_ms, 0, 0);
    lisp_define_primitive("%strftime", header_strftime, 1, 1);
}

void policy_log(const char *level, const char *format, ...) {
    char text[2048];
    va_list ap;
    va_start(ap, format);
    vsnprintf(text, sizeof(text), format, ap);
    va_end(ap);
    if (link_fd < 0 || !hello_seen) {
        fprintf(stderr, "tomoe-policy: %s\n", text);
        return;
    }
    lisp_value args[2] = { lisp_keyword(level), lisp_cstring(text) };
    core_send(TPL_PRIM_LOG, 2, args);
}

static void adopt_header(const struct tpl_state *state) {
    header = *state;
    mutated = false;
    lisp_set_turn_clock(state->mono_ns);
}

int policy_call(const char *hook, int argc, const lisp_value *argv, lisp_value *result,
        char *error, size_t error_size) {
    return lisp_call(hook, argc, argv, result, error, error_size);
}

void policy_hook_begin(bool fresh) {
    if (link_fd >= 0) send_now(TPL_HOOK_BEGIN, 0, NULL, 0, NULL, 0);
    if (fresh && link_fd >= 0) {
        lisp_value ignored;
        char error[256];
        if (!core_call(TPL_PRIM_FENCE, 0, NULL, &ignored, error, sizeof(error)))
            adopt_header(&header);
    }
}

static void send_done(uint32_t seq) {
    bool late = boot_flags & TPL_HELLO_TEST_SNAPSHOT_LATE;
    if (!late) snapshot_flush();
    if (keyed_batch && (boot_flags & TPL_HELLO_TEST_CRASH_AFTER_SNAPSHOT)) raise(SIGKILL);
    struct tpl_done done = { lisp_get_seed() };
    send_now(TPL_DONE, seq, &done, sizeof(done), NULL, 0);
    if (keyed_batch && (boot_flags & TPL_HELLO_TEST_CRASH_AFTER_DONE)) raise(SIGKILL);
    if (late) snapshot_flush();
    keyed_batch = false;
}

static void pause_ms(uint32_t ms) {
    struct timespec pause = { ms / 1000, (long)(ms % 1000) * 1000000l };
    while (nanosleep(&pause, &pause) && errno == EINTR) {}
}

void policy_hook_end_keep(void) {
    if (link_fd >= 0) send_done(0);
}

static void send_loading(bool on) {
    uint32_t value = on;
    send_now(TPL_LOADING, 0, &value, sizeof(value), NULL, 0);
}

static void handle_resume(struct message *message) {
    struct tpl_resume head;
    if (message->hdr.len < sizeof(head)) fatal_link("the compositor sent a short resume");
    memcpy(&head, message->payload, sizeof(head));
    adopt_header(&head.state);
    lisp_set_seed(head.has_rng ? head.rng : head.state.seed);
    if (boot_delay_ms) pause_ms(boot_delay_ms);
    send_loading(true);
    lisp_set_limits(4000000, 1000000000ull);
    size_t offset = sizeof(head);
    const uint8_t *data = message->payload;
    size_t length = message->hdr.len;
    lisp_value facts = lisp_nil(), facts_last = lisp_nil();
    for (uint32_t i = 0; i < head.facts; i++) {
        uint8_t kind;
        uint64_t serial;
        lisp_value event, aux;
        if (!take(data, length, &offset, &kind, 1) || !take(data, length, &offset, &serial, sizeof(serial)) ||
                decode(data, length, &offset, &event, 0) || decode(data, length, &offset, &aux, 0))
            fatal_link("the compositor sent malformed facts");
        absorb_aux(event, aux);
        lisp_value cell = lisp_cons(event, lisp_nil());
        if (lisp_nilp(facts_last)) facts = cell;
        else lisp_set_cdr(facts_last, cell);
        facts_last = cell;
    }
    lisp_value resume = lisp_nil();
    for (uint32_t i = 0; i < head.snapshots; i++) {
        lisp_value entry;
        if (decode(data, length, &offset, &entry, 0)) fatal_link("the compositor sent a malformed snapshot");
        lisp_value text = lisp_car(lisp_cdr(entry));
        if (!lisp_stringp(text)) continue;
        size_t text_length = 0;
        const char *source = lisp_get_string(text, &text_length);
        lisp_value saved;
        char error[256];
        if (lisp_read_data(source, text_length, &saved, error, sizeof(error))) {
            policy_log("ERROR", "tomoe: saved state of %s is unreadable (%s); it starts fresh",
                lisp_stringp(lisp_car(entry)) ? lisp_get_string(lisp_car(entry), NULL) : "?", error);
            continue;
        }
        resume = lisp_cons(saved, resume);
    }
    lisp_value sources = lisp_nil();
    if (boot_config) sources = lisp_cons(lisp_cstring(boot_config), lisp_nil());
    lisp_value args[5] = { sources, lisp_bool(boot_flags & TPL_HELLO_BARE),
        lisp_bool(boot_flags & TPL_HELLO_WATCH), facts, resume };
    lisp_value result;
    char error[1024];
    if (policy_call(POLICY_HOOK_BOOT, 5, args, &result, error, sizeof(error))) {
        policy_log("ERROR", "tomoe: %s", error);
    } else if (lisp_stringp(result)) {
        size_t text_length = 0;
        const char *text = lisp_get_string(result, &text_length);
        policy_log("ERROR", "tomoe: %.*s", (int)text_length, text);
    }
    core_flush();
    send_done(0);
    lisp_end_turn();
    uint32_t version = TPL_VERSION;
    send_now(TPL_READY, 0, &version, sizeof(version), NULL, 0);
    ready_sent = true;
}

static void handle_events(struct message *message) {
    struct tpl_events head;
    if (message->hdr.len < sizeof(head)) fatal_link("the compositor sent a short event batch");
    memcpy(&head, message->payload, sizeof(head));
    adopt_header(&head.state);
    size_t offset = sizeof(head);
    const uint8_t *data = message->payload;
    size_t length = message->hdr.len;
    lisp_value events = lisp_nil(), last = lisp_nil(), serials = lisp_nil(), serials_last = lisp_nil();
    uint64_t count = 0;
    for (uint32_t i = 0; i < head.count; i++) {
        uint8_t kind;
        uint64_t serial = 0;
        lisp_value first, second;
        if (!take(data, length, &offset, &kind, 1) ||
                (kind == TPL_ITEM_DISPATCH && !take(data, length, &offset, &serial, sizeof(serial))) ||
                decode(data, length, &offset, &first, 0) || decode(data, length, &offset, &second, 0))
            fatal_link("the compositor sent a malformed event batch");
        if (kind == TPL_ITEM_BROADCAST) {
            const char *name = lisp_stringp(first) ? lisp_get_string(first, NULL) : NULL;
            if (name) ipc_broadcast(name, second);
            continue;
        }
        absorb_aux(first, second);
        if (lisp_eq_keyword(lisp_getf(first, "TYPE"), "KEY")) keyed_batch = true;
        lisp_value cell = lisp_cons(first, lisp_nil());
        if (lisp_nilp(last)) events = cell;
        else lisp_set_cdr(last, cell);
        last = cell;
        lisp_value serial_cell = lisp_cons(lisp_uint(serial), lisp_nil());
        if (lisp_nilp(serials_last)) serials = serial_cell;
        else lisp_set_cdr(serials_last, serial_cell);
        serials_last = serial_cell;
        count++;
    }
    if (count) {
        lisp_set_limits(4000000ull * count, 1000000000ull);
        lisp_value result;
        char error[1024];
        in_batch = true;
        event_serial = 0;
        effect_counter = 0;
        batch_effects = 0;
        lisp_value dispatch_args[2] = { events, serials };
        if (policy_call(POLICY_HOOK_DISPATCH, 2, dispatch_args, &result, error, sizeof(error)))
            policy_log("ERROR", "tomoe: dispatch failed: %s", error);
        in_batch = false;
    }
    core_flush();
    if (held.length) raise(SIGKILL);
    if ((boot_flags & TPL_HELLO_TEST_CRASH_BEFORE_DONE) && batch_effects) raise(SIGKILL);
    send_done(message->hdr.seq);
    lisp_end_turn();
}

static void handle_hello(struct message *message) {
    struct tpl_hello hello;
    if (message->hdr.len < sizeof(hello)) fatal_link("the compositor sent a short hello");
    memcpy(&hello, message->payload, sizeof(hello));
    if (hello.version != TPL_VERSION) fatal_link("the compositor speaks another protocol version");
    boot_flags = hello.flags;
    boot_delay_ms = hello.delay_ms;
    free(boot_config);
    boot_config = NULL;
    if (hello.flags & TPL_HELLO_CONFIG)
        boot_config = strndup((const char *)message->payload + sizeof(hello),
            message->hdr.len - sizeof(hello));
    hello_seen = true;
}

static void process(struct message *message) {
    switch (message->hdr.type) {
    case TPL_HELLO: handle_hello(message); break;
    case TPL_RESUME: handle_resume(message); break;
    case TPL_EVENTS: handle_events(message); break;
    case TPL_SHADER_BUILD: policy_shader_build(message->payload, message->hdr.len); break;
    default: break;
    }
    free(message->payload);
}

static void link_ready(void *data, int fd, uint32_t mask) {
    if (mask & (WL_EVENT_ERROR | WL_EVENT_HANGUP)) {
        if (!(mask & WL_EVENT_READABLE)) fatal_link("the compositor closed the link");
    }
    read_some(0);
    struct message message;
    while (next_frame(&message)) queue_push(&message);
}

bool policy_link_work(void) {
    if (!queue_count) return false;
    struct message message = queue[0];
    memmove(queue, queue + 1, (queue_count - 1) * sizeof(*queue));
    queue_count--;
    process(&message);
    return queue_count > 0;
}

struct sent_state {
    char *name;
    char *text;
    bool seen;
};
static struct sent_state *sent;
static size_t sent_count;

static struct sent_state *sent_find(const char *name) {
    for (size_t i = 0; i < sent_count; i++)
        if (!strcmp(sent[i].name, name)) return &sent[i];
    return NULL;
}

static void snapshot_put(const char *name, const char *text) {
    struct pv_buf blob = { 0 };
    uint32_t name_length = (uint32_t)strlen(name);
    size_t text_length = strlen(text);
    pv_buf_u32(&blob, name_length);
    pv_buf_put(&blob, name, name_length);
    pv_buf_u8(&blob, PV_LIST);
    pv_buf_u32(&blob, 2);
    pv_buf_u8(&blob, PV_STRING);
    pv_buf_u32(&blob, name_length);
    pv_buf_put(&blob, name, name_length);
    pv_buf_u8(&blob, PV_STRING);
    pv_buf_u32(&blob, (uint32_t)text_length);
    pv_buf_put(&blob, text, text_length);
    pv_buf_u8(&blob, PV_NIL);
    if (!blob.failed && blob.length <= TPL_MAX_PAYLOAD)
        send_now(TPL_SNAPSHOT, 0, blob.data, blob.length, NULL, 0);
    pv_buf_free(&blob);
}

static void snapshot_flush(void) {
    if (!dirty_count && snapshot_reset_sent) return;
    if (!snapshot_reset_sent) {
        uint32_t zero = 0;
        send_now(TPL_SNAPSHOT, 0, &zero, 4, NULL, 0);
        snapshot_reset_sent = true;
        for (size_t i = 0; i < sent_count; i++) {
            free(sent[i].name);
            free(sent[i].text);
        }
        sent_count = 0;
    }
    lisp_value names = lisp_nil();
    for (size_t i = 0; i < dirty_count; i++) names = lisp_cons(lisp_cstring(dirty_names[i]), names);
    lisp_value entries;
    char error[512];
    int failed = lisp_call(POLICY_HOOK_RESUME_ENTRIES, 1, &names, &entries, error, sizeof(error));
    for (size_t i = 0; i < sent_count; i++) sent[i].seen = false;
    for (lisp_value tail = failed ? lisp_nil() : entries; lisp_consp(tail); tail = lisp_cdr(tail)) {
        lisp_value entry = lisp_car(tail);
        const char *name = lisp_stringp(lisp_car(entry)) ? lisp_get_string(lisp_car(entry), NULL) : NULL;
        if (!name || !*name || strlen(name) > 255) continue;
        char *text = lisp_print_alloc(entry, true);
        if (!text) continue;
        struct sent_state *old = sent_find(name);
        if (old && !strcmp(old->text, text)) {
            old->seen = true;
            free(text);
            continue;
        }
        if (!old) {
            struct sent_state *grown = realloc(sent, (sent_count + 1) * sizeof(*grown));
            char *copy = strdup(name);
            if (!grown || !copy) {
                if (grown) sent = grown;
                free(copy);
                free(text);
                continue;
            }
            sent = grown;
            old = &sent[sent_count++];
            old->name = copy;
            old->text = NULL;
        }
        snapshot_put(name, text);
        free(old->text);
        old->text = text;
        old->seen = true;
    }
    for (size_t d = 0; d < dirty_count; d++) {
        bool present = false;
        for (lisp_value tail = failed ? lisp_nil() : entries; lisp_consp(tail); tail = lisp_cdr(tail)) {
            lisp_value entry_name = lisp_car(lisp_car(tail));
            if (lisp_stringp(entry_name) && !strcmp(lisp_get_string(entry_name, NULL), dirty_names[d]))
                present = true;
        }
        if (!present && !failed) {
            uint32_t name_length = (uint32_t)strlen(dirty_names[d]);
            send_now(TPL_SNAPSHOT, 0, &name_length, 4, dirty_names[d], name_length);
            for (size_t i = 0; i < sent_count; i++) {
                if (strcmp(sent[i].name, dirty_names[d])) continue;
                free(sent[i].name);
                free(sent[i].text);
                sent[i] = sent[--sent_count];
                break;
            }
        }
        free(dirty_names[d]);
    }
    dirty_count = 0;
}

static lisp_value resume_dirty(int argc, lisp_value *argv) {
    if (!lisp_stringp(argv[0])) return lisp_nil();
    const char *name = lisp_get_string(argv[0], NULL);
    for (size_t i = 0; i < dirty_count; i++)
        if (!strcmp(dirty_names[i], name)) return lisp_nil();
    char **grown = realloc(dirty_names, (dirty_count + 1) * sizeof(*grown));
    if (!grown) return lisp_nil();
    dirty_names = grown;
    char *copy = strdup(name);
    if (copy) dirty_names[dirty_count++] = copy;
    return lisp_nil();
}

bool policy_link_ready(void) {
    return ready_sent;
}

void policy_link_loading(bool on) {
    if (link_fd >= 0) send_loading(on);
}

int policy_link_open(int fd) {
    link_fd = fd;
    int flags = fcntl(fd, F_GETFL);
    if (flags >= 0) fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);
    if (!loop_watch_fd(NULL, fd, WL_EVENT_READABLE, link_ready, NULL)) return -1;
    return 0;
}

lisp_value core_memory_report(char *error, size_t error_size, bool *ok) {
    lisp_value report;
    *ok = core_call(TPL_PRIM_MEMORY, 0, NULL, &report, error, error_size) == 0;
    return report;
}
