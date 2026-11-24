#define _GNU_SOURCE

#include "pipewire.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <sys/un.h>
#include <unistd.h>

void pww_build_init(struct pww_build *b, uint8_t *data, uint32_t cap) {
    b->data = data;
    b->cap = cap;
    b->off = 0;
    b->fail = 0;
}

void pww_build_bytes(struct pww_build *b, const void *data, uint32_t size) {
    if (b->fail) return;
    if ((uint64_t)b->off + size > b->cap) {
        b->fail = 1;
        return;
    }
    memcpy(b->data + b->off, data, size);
    b->off += size;
}

void pww_build_zero(struct pww_build *b, uint32_t size) {
    if (b->fail) return;
    if ((uint64_t)b->off + size > b->cap) {
        b->fail = 1;
        return;
    }
    memset(b->data + b->off, 0, size);
    b->off += size;
}

void pww_build_u32(struct pww_build *b, uint32_t v) { pww_build_bytes(b, &v, 4); }
void pww_build_u64(struct pww_build *b, uint64_t v) { pww_build_bytes(b, &v, 8); }

void pww_build_align(struct pww_build *b) {
    if (b->fail) return;
    uint32_t pad = (8 - (b->off & 7)) & 7u;
    pww_build_zero(b, pad);
}

void pww_pod_begin(struct pww_build *b, struct pww_frame *f) {
    f->off = b->off;
    pww_build_zero(b, 8);
}

void pww_pod_end(struct pww_build *b, struct pww_frame *f, uint32_t type) {
    if (b->fail) return;
    uint32_t size = b->off - f->off - 8;
    memcpy(b->data + f->off, &size, 4);
    memcpy(b->data + f->off + 4, &type, 4);
    pww_build_align(b);
}

void pww_pod_bool(struct pww_build *b, int v) {
    struct pww_frame f;
    uint32_t value = v ? 1 : 0;
    pww_pod_begin(b, &f);
    pww_build_u32(b, value);
    pww_pod_end(b, &f, SPA_TYPE_BOOL);
}

void pww_pod_id(struct pww_build *b, uint32_t v) {
    struct pww_frame f;
    pww_pod_begin(b, &f);
    pww_build_u32(b, v);
    pww_pod_end(b, &f, SPA_TYPE_ID);
}

void pww_pod_int(struct pww_build *b, int32_t v) {
    struct pww_frame f;
    pww_pod_begin(b, &f);
    pww_build_u32(b, (uint32_t)v);
    pww_pod_end(b, &f, SPA_TYPE_INT);
}

void pww_pod_long(struct pww_build *b, int64_t v) {
    struct pww_frame f;
    uint64_t value = (uint64_t)v;
    pww_pod_begin(b, &f);
    pww_build_u64(b, value);
    pww_pod_end(b, &f, SPA_TYPE_LONG);
}

void pww_pod_fd(struct pww_build *b, int64_t v) {
    struct pww_frame f;
    pww_pod_begin(b, &f);
    pww_build_u64(b, (uint64_t)v);
    pww_pod_end(b, &f, SPA_TYPE_FD);
}

void pww_unmap(struct pww *w, void *addr) {
    if (!addr) return;
    for (size_t i = 0; i < w->n_maps; i++) {
        if ((uint8_t *)w->maps[i].ptr + w->maps[i].delta != (uint8_t *)addr) continue;
        munmap(w->maps[i].ptr, w->maps[i].len);
        w->maps[i] = w->maps[w->n_maps - 1];
        w->n_maps--;
        return;
    }
}

void pww_unmap_owner(struct pww *w, int owner) {
    for (size_t i = 0; i < w->n_maps;) {
        if (w->maps[i].keep || w->maps[i].owner != owner) {
            i++;
            continue;
        }
        munmap(w->maps[i].ptr, w->maps[i].len);
        w->maps[i] = w->maps[w->n_maps - 1];
        w->n_maps--;
    }
}

void pww_pod_float(struct pww_build *b, float v) {
    struct pww_frame f;
    pww_pod_begin(b, &f);
    pww_build_bytes(b, &v, sizeof(v));
    pww_pod_end(b, &f, SPA_TYPE_FLOAT);
}
void pww_pod_array_floats(struct pww_build *b, const float *values, uint32_t n) {
    struct pww_frame f;
    pww_pod_begin(b, &f);
    pww_build_u32(b, 4);
    pww_build_u32(b, SPA_TYPE_FLOAT);
    pww_build_bytes(b, values, n * (uint32_t)sizeof(float));
    pww_pod_end(b, &f, SPA_TYPE_ARRAY);
}


void pww_pod_string(struct pww_build *b, const char *s) {
    struct pww_frame f;
    pww_pod_begin(b, &f);
    if (s) pww_build_bytes(b, s, (uint32_t)strlen(s));
    pww_build_zero(b, 1);
    pww_pod_end(b, &f, SPA_TYPE_STRING);
}

void pww_pod_struct_begin(struct pww_build *b, struct pww_frame *f) { pww_pod_begin(b, f); }

void pww_pod_struct_end(struct pww_build *b, struct pww_frame *f) {
    pww_pod_end(b, f, SPA_TYPE_STRUCT);
}

void pww_pod_object_begin(struct pww_build *b, struct pww_frame *f, uint32_t type, uint32_t id) {
    pww_pod_begin(b, f);
    pww_build_u32(b, type);
    pww_build_u32(b, id);
}

void pww_pod_object_end(struct pww_build *b, struct pww_frame *f) {
    pww_pod_end(b, f, SPA_TYPE_OBJECT);
}

void pww_pod_prop(struct pww_build *b, uint32_t key, uint32_t flags) {
    pww_build_u32(b, key);
    pww_build_u32(b, flags);
}

void pww_pod_prop_id(struct pww_build *b, uint32_t key, uint32_t v) {
    pww_pod_prop(b, key, 0);
    pww_pod_id(b, v);
}

void pww_pod_prop_int(struct pww_build *b, uint32_t key, int32_t v) {
    pww_pod_prop(b, key, 0);
    pww_pod_int(b, v);
}

void pww_pod_array_ids(struct pww_build *b, const uint32_t *ids, uint32_t n) {
    struct pww_frame f;
    pww_pod_begin(b, &f);
    pww_build_u32(b, 4);
    pww_build_u32(b, SPA_TYPE_ID);
    for (uint32_t i = 0; i < n; i++) pww_build_u32(b, ids[i]);
    pww_pod_end(b, &f, SPA_TYPE_ARRAY);
}

void pww_pod_prop_array_ids(struct pww_build *b, uint32_t key, const uint32_t *ids, uint32_t n) {
    pww_pod_prop(b, key, 0);
    pww_pod_array_ids(b, ids, n);
}
void pww_pod_prop_float(struct pww_build *b, uint32_t key, float v) {
    pww_pod_prop(b, key, 0);
    pww_pod_float(b, v);
}

void pww_pod_prop_bool(struct pww_build *b, uint32_t key, int v) {
    pww_pod_prop(b, key, 0);
    pww_pod_bool(b, v);
}

void pww_pod_prop_array_floats(struct pww_build *b, uint32_t key, const float *values, uint32_t n) {
    pww_pod_prop(b, key, 0);
    pww_pod_array_floats(b, values, n);
}


void pww_build_raw_pod(struct pww_build *b, const void *pod, size_t len) {
    pww_build_bytes(b, pod, (uint32_t)len);
    pww_build_align(b);
}

void pww_parse_init(struct pww_parse *p, const void *data, size_t size) {
    p->data = data;
    p->size = size;
    p->off = 0;
    p->fail = 0;
}

void pww_parse_align(struct pww_parse *p) {
    p->off = (p->off + 7) & ~(size_t)7;
}

const uint8_t *pww_parse_pod(struct pww_parse *p, uint32_t *type, uint32_t *body) {
    if (p->fail) return NULL;
    if (p->off + 8 > p->size) {
        p->fail = 1;
        return NULL;
    }
    const uint8_t *pod = p->data + p->off;
    uint32_t size;
    memcpy(&size, pod, 4);
    if (p->off + 8 + size > p->size) {
        p->fail = 1;
        return NULL;
    }
    if (type) memcpy(type, pod + 4, 4);
    if (body) *body = size;
    p->off += 8 + size;
    pww_parse_align(p);
    return pod;
}

int pww_parse_typed(struct pww_parse *p, uint32_t want, void *out, size_t n) {
    uint32_t type, body;
    const uint8_t *pod = pww_parse_pod(p, &type, &body);
    if (!pod || type != want || body < n) return -1;
    memcpy(out, pod + 8, n);
    return 0;
}

int pww_parse_int(struct pww_parse *p, uint32_t *v) { return pww_parse_typed(p, SPA_TYPE_INT, v, 4); }
int pww_parse_id(struct pww_parse *p, uint32_t *v) { return pww_parse_typed(p, SPA_TYPE_ID, v, 4); }
int pww_parse_fd(struct pww_parse *p, int64_t *v) { return pww_parse_typed(p, SPA_TYPE_FD, v, 8); }

int pww_parse_string(struct pww_parse *p, const char **s, uint32_t *len) {
    uint32_t type, body;
    const uint8_t *pod = pww_parse_pod(p, &type, &body);
    if (!pod || type != SPA_TYPE_STRING || body == 0) return -1;
    *s = (const char *)(pod + 8);
    *len = body;
    return 0;
}

int pww_parse_dict(struct pww_parse *p) {
    uint32_t n;
    if (pww_parse_int(p, &n) || n > 256) return -1;
    for (uint32_t i = 0; i < n; i++) {
        const char *key, *value;
        uint32_t len;
        if (pww_parse_string(p, &key, &len)) return -1;
        if (pww_parse_string(p, &value, &len)) return -1;
    }
    return 0;
}

int pww_parse_nested_dict(struct pww_parse *p) {
    uint32_t type, body;
    const uint8_t *pod = pww_parse_pod(p, &type, &body);
    if (!pod || type != SPA_TYPE_STRUCT) return -1;
    struct pww_parse q;
    pww_parse_init(&q, pod + 8, body);
    if (pww_parse_dict(&q) || q.fail) return -1;
    return 0;
}

int pww_parse_pod_object(struct pww_parse *p, uint32_t *type, uint32_t *id) {
    uint32_t ptype, body;
    const uint8_t *pod = pww_parse_pod(p, &ptype, &body);
    if (!pod || ptype != SPA_TYPE_OBJECT || body < 8) return -1;
    memcpy(type, pod + 8, 4);
    memcpy(id, pod + 12, 4);
    return 0;
}

int pww_socket_path(char *path, size_t size) {
    const char *remote = getenv("PIPEWIRE_REMOTE");
    if (!remote || !*remote) remote = "pipewire-0";
    if (remote[0] == '/') {
        if (strlen(remote) >= size) return -ENAMETOOLONG;
        strcpy(path, remote);
        return 0;
    }
    const char *runtime = getenv("PIPEWIRE_RUNTIME_DIR");
    if (!runtime || !*runtime) runtime = getenv("XDG_RUNTIME_DIR");
    if (!runtime || !*runtime) return -EINVAL;
    if ((size_t)snprintf(path, size, "%s/%s", runtime, remote) >= size) return -ENAMETOOLONG;
    return 0;
}

int pww_connect(struct pww *w) {
    char path[PATH_MAX];
    int res = pww_socket_path(path, sizeof(path));
    if (res < 0) return res;
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -errno;
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    if (strlen(path) >= sizeof(addr.sun_path)) {
        close(fd);
        return -ENAMETOOLONG;
    }
    strcpy(addr.sun_path, path);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        int error = errno;
        close(fd);
        return error == ENOENT ? -EHOSTDOWN : -error;
    }
    int flags = fcntl(fd, F_GETFL);
    if (flags >= 0) fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    w->fd = fd;
    return 0;
}

void pww_unmap_all(struct pww *w) {
    for (size_t i = 0; i < w->n_maps; i++) munmap(w->maps[i].ptr, w->maps[i].len);
    w->n_maps = 0;
}

void pww_mem_clear(struct pww *w) {
    for (size_t i = 0; i < w->n_mem; i++)
        if (w->mem[i].fd >= 0) close(w->mem[i].fd);
    w->n_mem = 0;
}

int pww_mem_add(struct pww *w, uint32_t id, int fd) {
    for (size_t i = 0; i < w->n_mem; i++) {
        if (w->mem[i].id != id) continue;
        if (w->mem[i].fd >= 0) close(w->mem[i].fd);
        w->mem[i].fd = fd;
        return 0;
    }
    if (w->n_mem >= PWW_MAX_MEM) return -ENOSPC;
    w->mem[w->n_mem].id = id;
    w->mem[w->n_mem].fd = fd;
    w->n_mem++;
    return 0;
}

int pww_mem_fd(struct pww *w, uint32_t id) {
    for (size_t i = 0; i < w->n_mem; i++)
        if (w->mem[i].id == id) return w->mem[i].fd;
    return -1;
}

int pww_mem_remove(struct pww *w, uint32_t id) {
    for (size_t i = 0; i < w->n_mem; i++) {
        if (w->mem[i].id != id) continue;
        if (w->mem[i].fd >= 0) close(w->mem[i].fd);
        w->mem[i] = w->mem[w->n_mem - 1];
        w->n_mem--;
        return 0;
    }
    return -ENOENT;
}

void *pww_map_fd(struct pww *w, int fd, uint32_t offset, uint32_t size, int keep,
        int owner) {
    if (fd < 0 || size == 0) return NULL;
    size_t page = (size_t)sysconf(_SC_PAGESIZE);
    if (page == 0) page = 4096;
    size_t start = offset & ~(page - 1);
    size_t delta = offset - start;
    size_t len = delta + size;
    len = (len + page - 1) & ~(page - 1);
    if (w->n_maps >= PWW_MAX_MAPS) return NULL;
    void *ptr = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, (off_t)start);
    if (ptr == MAP_FAILED) return NULL;
    w->maps[w->n_maps].ptr = ptr;
    w->maps[w->n_maps].delta = delta;
    w->maps[w->n_maps].len = len;
    w->maps[w->n_maps].keep = keep;
    w->maps[w->n_maps].owner = owner;
    w->n_maps++;
    return (uint8_t *)ptr + delta;
}

int pww_map_mem(struct pww *w, uint32_t id, uint32_t offset, uint32_t size, int keep,
        int owner, void **out, size_t *outsize) {
    int fd = pww_mem_fd(w, id);
    if (fd < 0) return -ENODEV;
    void *ptr = pww_map_fd(w, fd, offset, size, keep, owner);
    if (!ptr) return -ENOMEM;
    *out = ptr;
    if (outsize) *outsize = size;
    return 0;
}

int pww_send_message(struct pww *w, uint32_t id, uint32_t opcode, struct pww_build *b) {
    if (b->fail) return -ENOSPC;
    uint32_t size = b->off;
    if (size > 0xffffffu) return -ENOSPC;
    uint8_t storage[PWW_HEADER + PWW_BUILD];
    if (size > PWW_BUILD) return -ENOSPC;
    uint32_t header[4] = { id, (opcode << 24) | size, w->seq, 0 };
    memcpy(storage, header, sizeof(header));
    memcpy(storage + sizeof(header), b->data, size);
    size_t total = sizeof(header) + size, sent = 0;
    while (sent < total) {
        ssize_t n = send(w->fd, storage + sent, total - sent, MSG_NOSIGNAL | MSG_DONTWAIT);
        if (n < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                struct pollfd p = { .fd = w->fd, .events = POLLOUT };
                if (poll(&p, 1, 1000) <= 0) return -ETIMEDOUT;
                continue;
            }
            return -errno;
        }
        sent += (size_t)n;
    }
    w->seq = (w->seq + 1) & 0x3fffffffu;
    return 0;
}

void pww_message_begin(struct pww_build *b, struct pww_frame *root, uint8_t *storage) {
    pww_build_init(b, storage, PWW_BUILD);
    pww_pod_struct_begin(b, root);
}

int pww_message_send(struct pww *w, struct pww_build *b, struct pww_frame *root, uint32_t id,
        uint32_t opcode) {
    pww_pod_struct_end(b, root);
    return pww_send_message(w, id, opcode, b);
}

int pww_parse_messages(struct pww *w, pww_dispatch dispatch, void *data) {
    while (w->in_len >= PWW_HEADER) {
        uint32_t id, op, size, nfds;
        memcpy(&id, w->in, 4);
        memcpy(&op, w->in + 4, 4);
        memcpy(&nfds, w->in + 12, 4);
        size = op & 0xffffffu;
        if (size > PWW_MAX_MESSAGE) return -EPROTO;
        if (w->in_len < PWW_HEADER + size) return 0;
        if (nfds > w->n_fds) return 0;
        uint32_t opcode = op >> 24;
        struct pww_parse outer;
        pww_parse_init(&outer, w->in + PWW_HEADER, size);
        uint32_t outer_type = 0, outer_body = 0;
        const uint8_t *outer_pod = pww_parse_pod(&outer, &outer_type, &outer_body);
        if (!outer_pod || outer_type != SPA_TYPE_STRUCT) return -EPROTO;
        struct pww_parse p;
        pww_parse_init(&p, outer_pod + 8, outer_body);
        int res = dispatch(data, id, opcode, &p);
        for (uint32_t i = 0; i < nfds; i++) {
            if (w->fds[i] >= 0) close(w->fds[i]);
        }
        memmove(w->fds, w->fds + nfds, (w->n_fds - nfds) * sizeof(int));
        w->n_fds -= nfds;
        memmove(w->in, w->in + PWW_HEADER + size, w->in_len - PWW_HEADER - size);
        w->in_len -= PWW_HEADER + size;
        if (res < 0) return res;
    }
    return 0;
}

int pww_recv(struct pww *w, pww_dispatch dispatch, void *data) {
    for (;;) {
        uint8_t buf[65536];
        char control[CMSG_SPACE(PWW_MAX_FDS * sizeof(int))];
        struct iovec iov = { buf, sizeof(buf) };
        struct msghdr msg;
        memset(&msg, 0, sizeof(msg));
        msg.msg_iov = &iov;
        msg.msg_iovlen = 1;
        msg.msg_control = control;
        msg.msg_controllen = sizeof(control);
        ssize_t n = recvmsg(w->fd, &msg, MSG_DONTWAIT | MSG_CMSG_CLOEXEC);
        if (n < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
            return -errno;
        }
        if (n == 0) return -EPIPE;
        for (struct cmsghdr *cm = CMSG_FIRSTHDR(&msg); cm; cm = CMSG_NXTHDR(&msg, cm)) {
            if (cm->cmsg_level != SOL_SOCKET || cm->cmsg_type != SCM_RIGHTS) continue;
            size_t count = (cm->cmsg_len - CMSG_LEN(0)) / sizeof(int);
            int *fds = (int *)CMSG_DATA(cm);
            for (size_t i = 0; i < count; i++) {
                if (w->n_fds >= PWW_MAX_FDS) {
                    close(fds[i]);
                    continue;
                }
                w->fds[w->n_fds++] = fds[i];
            }
        }
        if (msg.msg_flags & MSG_CTRUNC) return -EMSGSIZE;
        if (w->in_len + (size_t)n > w->in_cap) {
            size_t cap = w->in_cap ? w->in_cap : 65536;
            while (cap < w->in_len + (size_t)n) cap *= 2;
            uint8_t *in = realloc(w->in, cap);
            if (!in) return -ENOMEM;
            w->in = in;
            w->in_cap = cap;
        }
        memcpy(w->in + w->in_len, buf, (size_t)n);
        w->in_len += (size_t)n;
        int res = pww_parse_messages(w, dispatch, data);
        if (res < 0) return res;
    }
}

void pww_pod_rectangle(struct pww_build *b, uint32_t width, uint32_t height) {
    struct pww_frame f;
    pww_pod_begin(b, &f);
    pww_build_u32(b, width);
    pww_build_u32(b, height);
    pww_pod_end(b, &f, SPA_TYPE_RECTANGLE);
}

void pww_pod_fraction(struct pww_build *b, uint32_t num, uint32_t denom) {
    struct pww_frame f;
    pww_pod_begin(b, &f);
    pww_build_u32(b, num);
    pww_build_u32(b, denom);
    pww_pod_end(b, &f, SPA_TYPE_FRACTION);
}

void pww_pod_choice_int(struct pww_build *b, uint32_t choice, const int32_t *values,
        uint32_t count) {
    struct pww_frame f;
    pww_pod_begin(b, &f);
    pww_build_u32(b, choice);
    pww_build_u32(b, 0);
    pww_build_u32(b, 4);
    pww_build_u32(b, SPA_TYPE_INT);
    for (uint32_t i = 0; i < count; i++) pww_build_u32(b, (uint32_t)values[i]);
    pww_pod_end(b, &f, SPA_TYPE_CHOICE);
}

void pww_pod_choice_long(struct pww_build *b, uint32_t choice, const int64_t *values,
        uint32_t count) {
    struct pww_frame f;
    pww_pod_begin(b, &f);
    pww_build_u32(b, choice);
    pww_build_u32(b, 0);
    pww_build_u32(b, 8);
    pww_build_u32(b, SPA_TYPE_LONG);
    for (uint32_t i = 0; i < count; i++) pww_build_u64(b, (uint64_t)values[i]);
    pww_pod_end(b, &f, SPA_TYPE_CHOICE);
}

void pww_pod_choice_fraction(struct pww_build *b, uint32_t choice, const uint32_t *values,
        uint32_t count) {
    struct pww_frame f;
    pww_pod_begin(b, &f);
    pww_build_u32(b, choice);
    pww_build_u32(b, 0);
    pww_build_u32(b, 8);
    pww_build_u32(b, SPA_TYPE_FRACTION);
    for (uint32_t i = 0; i < count; i++) pww_build_u32(b, values[i]);
    pww_pod_end(b, &f, SPA_TYPE_CHOICE);
}

void pww_pod_prop_long(struct pww_build *b, uint32_t key, int64_t v) {
    pww_pod_prop(b, key, 0);
    pww_pod_long(b, v);
}

void pww_pod_prop_long_flags(struct pww_build *b, uint32_t key, uint32_t flags, int64_t v) {
    pww_pod_prop(b, key, flags);
    pww_pod_long(b, v);
}

void pww_pod_prop_rectangle(struct pww_build *b, uint32_t key, uint32_t width, uint32_t height) {
    pww_pod_prop(b, key, 0);
    pww_pod_rectangle(b, width, height);
}

void pww_pod_prop_fraction(struct pww_build *b, uint32_t key, uint32_t num, uint32_t denom) {
    pww_pod_prop(b, key, 0);
    pww_pod_fraction(b, num, denom);
}

void pww_pod_prop_choice_int(struct pww_build *b, uint32_t key, uint32_t choice,
        const int32_t *values, uint32_t count) {
    pww_pod_prop(b, key, 0);
    pww_pod_choice_int(b, choice, values, count);
}

void pww_pod_prop_choice_long(struct pww_build *b, uint32_t key, uint32_t flags, uint32_t choice,
        const int64_t *values, uint32_t count) {
    pww_pod_prop(b, key, flags);
    pww_pod_choice_long(b, choice, values, count);
}

void pww_pod_prop_choice_fraction(struct pww_build *b, uint32_t key, uint32_t choice,
        const uint32_t *values, uint32_t count) {
    pww_pod_prop(b, key, 0);
    pww_pod_choice_fraction(b, choice, values, count);
}

int pww_send_message_fds(struct pww *w, uint32_t id, uint32_t opcode, struct pww_build *b,
        const int *fds, uint32_t n_fds) {
    if (b->fail) return -ENOSPC;
    uint32_t size = b->off;
    if (size > 0xffffffu || size > PWW_BUILD) return -ENOSPC;
    if (n_fds > PWW_MAX_FDS) return -EINVAL;
    uint8_t storage[PWW_HEADER + PWW_BUILD];
    uint32_t header[4] = { id, (opcode << 24) | size, w->seq, n_fds };
    memcpy(storage, header, sizeof(header));
    memcpy(storage + sizeof(header), b->data, size);
    size_t total = sizeof(header) + size;
    char control[CMSG_SPACE(PWW_MAX_FDS * sizeof(int))];
    memset(control, 0, sizeof(control));
    struct iovec iov = { storage, total };
    struct msghdr msg;
    memset(&msg, 0, sizeof(msg));
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    if (n_fds > 0) {
        msg.msg_control = control;
        msg.msg_controllen = CMSG_SPACE(n_fds * sizeof(int));
        struct cmsghdr *cm = CMSG_FIRSTHDR(&msg);
        cm->cmsg_level = SOL_SOCKET;
        cm->cmsg_type = SCM_RIGHTS;
        cm->cmsg_len = CMSG_LEN(n_fds * sizeof(int));
        memcpy(CMSG_DATA(cm), fds, n_fds * sizeof(int));
    }
    for (;;) {
        ssize_t n = sendmsg(w->fd, &msg, MSG_NOSIGNAL | MSG_DONTWAIT);
        if (n < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                struct pollfd p = { .fd = w->fd, .events = POLLOUT };
                if (poll(&p, 1, 1000) <= 0) return -ETIMEDOUT;
                continue;
            }
            return -errno;
        }
        if ((size_t)n < total) {
            size_t sent = (size_t)n;
            msg.msg_control = NULL;
            msg.msg_controllen = 0;
            iov.iov_base = storage + sent;
            iov.iov_len = total - sent;
            while (sent < total) {
                ssize_t more = sendmsg(w->fd, &msg, MSG_NOSIGNAL | MSG_DONTWAIT);
                if (more < 0) {
                    if (errno == EINTR) continue;
                    if (errno == EAGAIN || errno == EWOULDBLOCK) {
                        struct pollfd p = { .fd = w->fd, .events = POLLOUT };
                        if (poll(&p, 1, 1000) <= 0) return -ETIMEDOUT;
                        continue;
                    }
                    return -errno;
                }
                sent += (size_t)more;
                iov.iov_base = storage + sent;
                iov.iov_len = total - sent;
            }
        }
        break;
    }
    w->seq = (w->seq + 1) & 0x3fffffffu;
    return 0;
}
