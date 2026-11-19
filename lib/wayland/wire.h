#ifndef TOMOE_WIRE_H
#define TOMOE_WIRE_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include "wayland-util.h"

#define WIRE_MAX_ARGS 20
#define WIRE_IN_SIZE 4096
#define WIRE_OUT_FLUSH 4096
#define WIRE_OUT_LIMIT (1u << 20)
#define WIRE_FDS_IN 256
#define WIRE_FDS_OUT 28

struct wire {
    int fd;
    uint32_t in_start, in_end;
    uint32_t fds_in_head, fds_in_count;
    uint32_t fds_out_count;
    uint32_t out_len, out_cap;
    uint8_t *out;
    int fds_in[WIRE_FDS_IN];
    int fds_out[WIRE_FDS_OUT];
    uint8_t in[WIRE_IN_SIZE];
};

struct wire_arg {
    char type;
    bool nullable;
};

const char *wire_next_arg(const char *signature, struct wire_arg *arg);
int wire_arg_count(const char *signature);
int wire_since(const struct wl_message *message);

bool wire_init(struct wire *wire, int fd);
void wire_release(struct wire *wire);
int wire_read(struct wire *wire);
int wire_flush(struct wire *wire);
static inline uint32_t wire_pending(const struct wire *wire) {
    return wire->in_end - wire->in_start;
}
static inline const uint32_t *wire_peek(const struct wire *wire) {
    return (const uint32_t *)(wire->in + wire->in_start);
}
static inline void wire_consume(struct wire *wire, uint32_t size) {
    wire->in_start += size;
}
int wire_take_fd(struct wire *wire);
int wire_decode(struct wire *wire, const uint32_t *p, uint32_t size, const struct wl_message *message,
    union wl_argument *args, struct wl_array *arrays);
void wire_close_fds(const struct wl_message *message, const union wl_argument *args, int count);
int wire_send(struct wire *wire, uint32_t id, uint32_t opcode, const struct wl_message *message,
    const union wl_argument *args);
void wire_args_from_va(const char *signature, union wl_argument *args, va_list ap);
void wire_invoke(void (*fn)(void), void *first, void *second, const struct wl_message *message,
    const union wl_argument *args);
void wire_log(bool server, bool send, const struct wl_interface *interface, uint32_t id,
    const struct wl_message *message, const union wl_argument *args,
    const char *(*object_name)(void *context, uint32_t id, char *buffer, size_t size), void *context);
void wire_vlog(wl_log_func_t handler, const char *fmt, ...) WL_PRINTF(2, 3);
bool wire_env_token(const char *env, const char *token);

#endif
