#ifndef TOMOE_PORTAL_LOOP_H
#define TOMOE_PORTAL_LOOP_H

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#define LOOP_IO_IN 1u
#define LOOP_IO_ERR 2u
#define LOOP_IO_HUP 4u

struct loop;
struct loop_source;
struct loop_timer;

struct loop *loop_new(void);
void loop_free(struct loop *loop);
void loop_run(struct loop *loop);
void loop_quit(struct loop *loop);
void loop_signal(struct loop *loop);
void loop_on_iterate(struct loop *loop, void (*run)(void *data), void *data);

struct loop_source *loop_add_io(struct loop *loop, int fd, uint32_t mask,
    void (*ready)(void *data, int fd, uint32_t mask), void *data);
void loop_del_io(struct loop *loop, struct loop_source *source);

struct loop_timer *loop_add_timer(struct loop *loop, void (*fired)(void *data, uint64_t fires),
    void *data);
void loop_update_timer(struct loop *loop, struct loop_timer *timer, const struct timespec *value,
    const struct timespec *interval, bool absolute);
void loop_del_timer(struct loop *loop, struct loop_timer *timer);

int loop_add_signals(struct loop *loop, void (*received)(void *data, int number), void *data);

#endif
