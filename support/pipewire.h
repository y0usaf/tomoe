#ifndef TOMOE_PIPEWIRE_H
#define TOMOE_PIPEWIRE_H

#include <stdint.h>

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
};

struct pwn_client;

struct pwn_client *pwn_start(const char *app_name, const char *node_name,
        unsigned rate, unsigned channels, const struct pwn_ops *ops, void *data);
void pwn_stop(struct pwn_client *client);

#endif
