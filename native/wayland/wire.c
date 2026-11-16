#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#include "wire.h"

void wl_list_init(struct wl_list *list) {
    list->prev = list;
    list->next = list;
}

void wl_list_insert(struct wl_list *list, struct wl_list *elm) {
    elm->prev = list;
    elm->next = list->next;
    list->next = elm;
    elm->next->prev = elm;
}

void wl_list_remove(struct wl_list *elm) {
    elm->prev->next = elm->next;
    elm->next->prev = elm->prev;
    elm->next = NULL;
    elm->prev = NULL;
}

int wl_list_length(const struct wl_list *list) {
    int count = 0;
    for (const struct wl_list *e = list->next; e != list; e = e->next) count++;
    return count;
}

int wl_list_empty(const struct wl_list *list) {
    return list->next == list;
}

void wl_list_insert_list(struct wl_list *list, struct wl_list *other) {
    if (wl_list_empty(other)) return;
    other->next->prev = list;
    other->prev->next = list->next;
    list->next->prev = other->prev;
    list->next = other->next;
}

void *wl_array_add(struct wl_array *array, size_t size) {
    size_t alloc = array->alloc > 0 ? array->alloc : 16;
    while (alloc < array->size + size) alloc *= 2;
    if (array->alloc < alloc) {
        if (array->alloc == 0 && array->data != NULL) abort();
        void *data = array->alloc > 0 ? realloc(array->data, alloc) : malloc(alloc);
        if (!data) return NULL;
        array->data = data;
        array->alloc = alloc;
    }
    void *p = (char *)array->data + array->size;
    array->size += size;
    return p;
}

int wl_array_copy(struct wl_array *array, struct wl_array *source) {
    if (array->size < source->size) {
        if (!wl_array_add(array, source->size - array->size)) return -1;
    } else {
        array->size = source->size;
    }
    if (source->size > 0) memcpy(array->data, source->data, source->size);
    return 0;
}

const char *wire_next_arg(const char *signature, struct wire_arg *arg) {
    arg->nullable = false;
    for (; *signature; signature++) {
        switch (*signature) {
        case 'i': case 'u': case 'f': case 's': case 'o': case 'n': case 'a': case 'h':
            arg->type = *signature;
            return signature + 1;
        case '?':
            arg->nullable = true;
        }
    }
    arg->type = '\0';
    return signature;
}

int wire_arg_count(const char *signature) {
    int count = 0;
    for (; *signature; signature++)
        switch (*signature) {
        case 'i': case 'u': case 'f': case 's': case 'o': case 'n': case 'a': case 'h':
            count++;
        }
    return count;
}

int wire_since(const struct wl_message *message) {
    int since = 0;
    for (const char *s = message->signature; *s >= '0' && *s <= '9'; s++) since = since * 10 + (*s - '0');
    return since ? since : 1;
}

bool wire_init(struct wire *wire, int fd) {
    memset(wire, 0, offsetof(struct wire, fds_in));
    wire->fd = fd;
    wire->out = malloc(WIRE_OUT_FLUSH);
    wire->out_cap = wire->out ? WIRE_OUT_FLUSH : 0;
    return wire->out != NULL;
}

static void close_queued(struct wire *wire) {
    for (uint32_t i = 0; i < wire->fds_in_count; i++)
        close(wire->fds_in[(wire->fds_in_head + i) % WIRE_FDS_IN]);
    for (uint32_t i = 0; i < wire->fds_out_count; i++) close(wire->fds_out[i]);
    wire->fds_in_count = wire->fds_out_count = 0;
}

void wire_release(struct wire *wire) {
    close_queued(wire);
    free(wire->out);
    wire->out = NULL;
    wire->out_len = wire->out_cap = 0;
}

static int queue_fds(struct wire *wire, struct msghdr *msg) {
    bool overflow = false;
    for (struct cmsghdr *c = CMSG_FIRSTHDR(msg); c; c = CMSG_NXTHDR(msg, c)) {
        if (c->cmsg_level != SOL_SOCKET || c->cmsg_type != SCM_RIGHTS) continue;
        size_t count = (c->cmsg_len - CMSG_LEN(0)) / sizeof(int);
        int fds[WIRE_FDS_OUT * 2];
        if (count > sizeof(fds) / sizeof(fds[0])) count = sizeof(fds) / sizeof(fds[0]);
        memcpy(fds, CMSG_DATA(c), count * sizeof(int));
        for (size_t i = 0; i < count; i++) {
            if (overflow || wire->fds_in_count == WIRE_FDS_IN) {
                overflow = true;
                close(fds[i]);
                continue;
            }
            wire->fds_in[(wire->fds_in_head + wire->fds_in_count++) % WIRE_FDS_IN] = fds[i];
        }
    }
    if (overflow) {
        errno = EOVERFLOW;
        return -1;
    }
    return 0;
}

int wire_read(struct wire *wire) {
    if (wire->in_start > 0) {
        uint32_t pending = wire->in_end - wire->in_start;
        if (pending) memmove(wire->in, wire->in + wire->in_start, pending);
        wire->in_start = 0;
        wire->in_end = pending;
    }
    for (;;) {
        if (wire->in_end == WIRE_IN_SIZE) return (int)wire->in_end;
        char control[CMSG_SPACE(WIRE_FDS_OUT * sizeof(int))];
        struct iovec iov = { wire->in + wire->in_end, WIRE_IN_SIZE - wire->in_end };
        struct msghdr msg = { .msg_iov = &iov, .msg_iovlen = 1, .msg_control = control,
            .msg_controllen = sizeof(control) };
        ssize_t len;
        do len = recvmsg(wire->fd, &msg, MSG_DONTWAIT | MSG_CMSG_CLOEXEC);
        while (len < 0 && errno == EINTR);
        if (len == 0) return (int)wire->in_end;
        if (len < 0) {
            if (errno == EAGAIN && wire->in_end > 0) return (int)wire->in_end;
            return -1;
        }
        if (queue_fds(wire, &msg) < 0) return -1;
        wire->in_end += (uint32_t)len;
    }
}

int wire_flush(struct wire *wire) {
    uint32_t sent = 0;
    while (sent < wire->out_len) {
        char control[CMSG_SPACE(WIRE_FDS_OUT * sizeof(int))];
        struct iovec iov = { wire->out + sent, wire->out_len - sent };
        struct msghdr msg = { .msg_iov = &iov, .msg_iovlen = 1 };
        if (wire->fds_out_count) {
            memset(control, 0, sizeof(control));
            msg.msg_control = control;
            msg.msg_controllen = CMSG_SPACE(wire->fds_out_count * sizeof(int));
            struct cmsghdr *c = CMSG_FIRSTHDR(&msg);
            c->cmsg_level = SOL_SOCKET;
            c->cmsg_type = SCM_RIGHTS;
            c->cmsg_len = CMSG_LEN(wire->fds_out_count * sizeof(int));
            memcpy(CMSG_DATA(c), wire->fds_out, wire->fds_out_count * sizeof(int));
        }
        ssize_t len;
        do len = sendmsg(wire->fd, &msg, MSG_NOSIGNAL | MSG_DONTWAIT);
        while (len < 0 && errno == EINTR);
        if (len < 0) {
            if (sent) {
                memmove(wire->out, wire->out + sent, wire->out_len - sent);
                wire->out_len -= sent;
            }
            return -1;
        }
        for (uint32_t i = 0; i < wire->fds_out_count; i++) close(wire->fds_out[i]);
        wire->fds_out_count = 0;
        sent += (uint32_t)len;
    }
    wire->out_len = 0;
    return (int)sent;
}

int wire_take_fd(struct wire *wire) {
    if (!wire->fds_in_count) return -1;
    int fd = wire->fds_in[wire->fds_in_head];
    wire->fds_in_head = (wire->fds_in_head + 1) % WIRE_FDS_IN;
    wire->fds_in_count--;
    return fd;
}

void wire_close_fds(const struct wl_message *message, const union wl_argument *args, int count) {
    const char *signature = message->signature;
    struct wire_arg arg;
    for (int i = 0; i < count; i++) {
        signature = wire_next_arg(signature, &arg);
        if (arg.type == 'h' && args[i].h >= 0) close(args[i].h);
    }
}

int wire_decode(struct wire *wire, const uint32_t *p, uint32_t size, const struct wl_message *message,
        union wl_argument *args, struct wl_array *arrays) {
    const uint32_t *end = p + size / 4;
    const char *signature = message->signature;
    struct wire_arg arg;
    int count = wire_arg_count(signature);
    if (count > WIRE_MAX_ARGS) return -1;
    p += 2;
    for (int i = 0; i < count; i++) {
        signature = wire_next_arg(signature, &arg);
        if (arg.type != 'h' && p >= end) goto fail;
        switch (arg.type) {
        case 'i': case 'u': case 'f': case 'o': case 'n':
            args[i].u = *p++;
            if (arg.type == 'n' && args[i].u == 0) goto fail;
            if (arg.type == 'o' && args[i].u == 0 && !arg.nullable) goto fail;
            break;
        case 's': {
            uint32_t length = *p++;
            if (length == 0) {
                if (!arg.nullable) goto fail;
                args[i].s = NULL;
                break;
            }
            uint32_t words = (length + 3) / 4;
            if ((uint32_t)(end - p) < words) goto fail;
            const char *s = (const char *)p;
            if (s[length - 1] != '\0' || strlen(s) != length - 1) goto fail;
            args[i].s = s;
            p += words;
            break;
        }
        case 'a': {
            uint32_t length = *p++;
            uint32_t words = (length + 3) / 4;
            if ((uint32_t)(end - p) < words) goto fail;
            arrays[i] = (struct wl_array){ .size = length, .alloc = 0, .data = (void *)p };
            args[i].a = &arrays[i];
            p += words;
            break;
        }
        case 'h':
            args[i].h = wire_take_fd(wire);
            if (args[i].h < 0) goto fail;
            break;
        }
        continue;
    fail:
        wire_close_fds(message, args, i);
        return -1;
    }
    return count;
}

void wire_args_from_va(const char *signature, union wl_argument *args, va_list ap) {
    struct wire_arg arg;
    int count = wire_arg_count(signature);
    for (int i = 0; i < count && i < WIRE_MAX_ARGS; i++) {
        signature = wire_next_arg(signature, &arg);
        switch (arg.type) {
        case 'i': args[i].i = va_arg(ap, int32_t); break;
        case 'u': args[i].u = va_arg(ap, uint32_t); break;
        case 'f': args[i].f = va_arg(ap, wl_fixed_t); break;
        case 's': args[i].s = va_arg(ap, const char *); break;
        case 'o': args[i].o = va_arg(ap, struct wl_object *); break;
        case 'n': args[i].o = va_arg(ap, struct wl_object *); break;
        case 'a': args[i].a = va_arg(ap, struct wl_array *); break;
        case 'h': args[i].h = va_arg(ap, int32_t); break;
        }
    }
}

static uint8_t *reserve(struct wire *wire, uint32_t size) {
    if (wire->out_len + size > WIRE_OUT_FLUSH && wire->out_len) {
        if (wire_flush(wire) < 0 && errno != EAGAIN) return NULL;
    }
    if (wire->out_len + size > wire->out_cap) {
        uint32_t cap = wire->out_cap ? wire->out_cap : WIRE_OUT_FLUSH;
        while (cap < wire->out_len + size) cap *= 2;
        if (cap > WIRE_OUT_LIMIT) {
            errno = E2BIG;
            return NULL;
        }
        uint8_t *out = realloc(wire->out, cap);
        if (!out) return NULL;
        wire->out = out;
        wire->out_cap = cap;
    }
    uint8_t *at = wire->out + wire->out_len;
    wire->out_len += size;
    return at;
}

int wire_send(struct wire *wire, uint32_t id, uint32_t opcode, const struct wl_message *message,
        const union wl_argument *args) {
    const char *signature = message->signature;
    struct wire_arg arg;
    int count = wire_arg_count(signature);
    if (count > WIRE_MAX_ARGS) {
        errno = E2BIG;
        return -1;
    }
    uint32_t size = 8;
    int fds = 0;
    for (int i = 0; i < count; i++) {
        signature = wire_next_arg(signature, &arg);
        switch (arg.type) {
        case 's':
            size += 4 + (args[i].s ? ((uint32_t)strlen(args[i].s) + 4) & ~3u : 0);
            break;
        case 'a':
            size += 4 + (args[i].a ? ((uint32_t)args[i].a->size + 3) & ~3u : 0);
            break;
        case 'h':
            fds++;
            break;
        default:
            size += 4;
        }
    }
    if (size > WL_MAX_MESSAGE_SIZE) {
        errno = E2BIG;
        return -1;
    }
    if (fds && wire->fds_out_count + (uint32_t)fds > WIRE_FDS_OUT && wire_flush(wire) < 0) return -1;
    if (fds) {
        signature = message->signature;
        for (int i = 0; i < count; i++) {
            signature = wire_next_arg(signature, &arg);
            if (arg.type != 'h') continue;
            int fd = fcntl(args[i].h, F_DUPFD_CLOEXEC, 0);
            if (fd < 0) abort();
            wire->fds_out[wire->fds_out_count++] = fd;
        }
    }
    uint32_t *p = (uint32_t *)reserve(wire, size);
    if (!p) return -1;
    *p++ = id;
    *p++ = size << 16 | (opcode & 0xffff);
    signature = message->signature;
    for (int i = 0; i < count; i++) {
        signature = wire_next_arg(signature, &arg);
        switch (arg.type) {
        case 's':
            if (!args[i].s) {
                *p++ = 0;
            } else {
                uint32_t length = (uint32_t)strlen(args[i].s) + 1;
                *p++ = length;
                p[(length - 1) / 4] = 0;
                memcpy(p, args[i].s, length);
                p += (length + 3) / 4;
            }
            break;
        case 'a':
            if (!args[i].a || !args[i].a->size) {
                *p++ = 0;
            } else {
                uint32_t length = (uint32_t)args[i].a->size;
                *p++ = length;
                p[(length - 1) / 4] = 0;
                memcpy(p, args[i].a->data, length);
                p += (length + 3) / 4;
            }
            break;
        case 'h':
            break;
        default:
            *p++ = args[i].u;
        }
    }
    return 0;
}

typedef void (*call8)(uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t);
typedef void (*call22)(uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t,
    uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t,
    uintptr_t, uintptr_t, uintptr_t, uintptr_t);

void wire_invoke(void (*fn)(void), void *first, void *second, const struct wl_message *message,
        const union wl_argument *args) {
    uintptr_t v[WIRE_MAX_ARGS + 2] = { (uintptr_t)first, (uintptr_t)second };
    const char *signature = message->signature;
    struct wire_arg arg;
    int count = wire_arg_count(signature);
    for (int i = 0; i < count; i++) {
        signature = wire_next_arg(signature, &arg);
        switch (arg.type) {
        case 'i': case 'f': case 'h': v[i + 2] = (uintptr_t)(intptr_t)args[i].i; break;
        case 'u': v[i + 2] = args[i].u; break;
        case 's': v[i + 2] = (uintptr_t)args[i].s; break;
        case 'a': v[i + 2] = (uintptr_t)args[i].a; break;
        default: v[i + 2] = (uintptr_t)args[i].o; break;
        }
    }
    if (count <= 6) {
        ((call8)fn)(v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7]);
    } else {
        ((call22)fn)(v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7], v[8], v[9], v[10], v[11], v[12],
            v[13], v[14], v[15], v[16], v[17], v[18], v[19], v[20], v[21]);
    }
}

bool wire_env_token(const char *env, const char *token) {
    size_t length = strlen(token);
    for (const char *p = env; p && *p;) {
        const char *comma = strchr(p, ',');
        size_t n = comma ? (size_t)(comma - p) : strlen(p);
        if (n == length && !strncmp(p, token, n)) return true;
        p = comma ? comma + 1 : NULL;
    }
    return false;
}

void wire_vlog(wl_log_func_t handler, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    if (handler) handler(fmt, ap);
    else vfprintf(stderr, fmt, ap);
    va_end(ap);
}

void wire_log(bool server, bool send, const struct wl_interface *interface, uint32_t id,
        const struct wl_message *message, const union wl_argument *args,
        const char *(*object_name)(void *context, uint32_t id, char *buffer, size_t size), void *context) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    unsigned long long us = (unsigned long long)now.tv_sec * 1000000ull + (unsigned long long)now.tv_nsec / 1000;
    char line[8192];
    size_t used = 0;
#define PUT(...) do { int n_ = snprintf(line + used, used < sizeof(line) ? sizeof(line) - used : 0, __VA_ARGS__); \
        if (n_ > 0) used += (size_t)n_; } while (0)
    PUT("[%7llu.%03llu] %s%s%s#%u.%s(", us / 1000 % 10000000, us % 1000, server ? "" : "{Default Queue} ",
        send ? " -> " : "", interface->name, id, message->name);
    const char *signature = message->signature;
    struct wire_arg arg;
    int count = wire_arg_count(signature);
    char name[128];
    for (int i = 0; i < count; i++) {
        signature = wire_next_arg(signature, &arg);
        if (i) PUT(", ");
        switch (arg.type) {
        case 'u': PUT("%u", args[i].u); break;
        case 'i': PUT("%d", args[i].i); break;
        case 'f': PUT("%.8f", wl_fixed_to_double(args[i].f)); break;
        case 's': if (args[i].s) PUT("\"%s\"", args[i].s); else PUT("nil"); break;
        case 'o': PUT("%s", object_name(context, args[i].u, name, sizeof(name))); break;
        case 'n':
            if (message->types[i]) PUT("new id %s#%u", message->types[i]->name, args[i].u);
            else PUT("new id %s", object_name(context, args[i].u, name, sizeof(name)));
            break;
        case 'a': PUT("array[%zu]", args[i].a ? args[i].a->size : 0); break;
        case 'h': PUT("fd %d", args[i].h); break;
        }
    }
    PUT(")\n");
#undef PUT
    fwrite(line, 1, used < sizeof(line) ? used : sizeof(line) - 1, stderr);
}
