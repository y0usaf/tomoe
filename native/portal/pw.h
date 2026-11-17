#ifndef TOMOE_PORTAL_PW_H
#define TOMOE_PORTAL_PW_H

#include <stdbool.h>
#include <stdint.h>

#include "spa.h"

struct loop;
struct pwv;

enum { PWV_MAX_BUFFERS = 8 };
enum { PWV_MAX_TARGETS = 16 };
enum { PWV_MAX_METAS = 8 };
enum { PWV_MAX_DATAS = 4 };

enum { PWV_BOUND = 1, PWV_FAILED = 2 };
enum { PWV_ALLOC_MEMORY = 0, PWV_ALLOC_DMABUF = 1 };

struct pwv_video {
    uint32_t format;
    uint64_t modifier;
    bool modifier_set;
    int width;
    int height;
    int planes;
    uint32_t refresh;
};

struct pwv_buffer {
    uint32_t id;
    struct spa_buffer *buffer;
    void *user_data;
};

struct pwv_events {
    void (*state)(void *data, int state, const char *error);
    void (*format)(void *data, const struct pwv_video *video);
    bool (*add_buffer)(void *data, struct pwv_buffer *buffer);
    void (*remove_buffer)(void *data, struct pwv_buffer *buffer);
    void (*process)(void *data);
};

struct pwv *pwv_start(struct loop *loop, const char *node_name, const char *description,
    const struct pwv_video *video, int alloc, bool cursor, const struct pwv_events *events,
    void *data);
void pwv_stop(struct pwv *v);
int pwv_publish(struct pwv *v, const struct pwv_video *video, int alloc);
struct pwv_buffer *pwv_dequeue(struct pwv *v);
void pwv_queue(struct pwv *v, struct pwv_buffer *buffer);
uint32_t pwv_node_id(struct pwv *v);

#endif
