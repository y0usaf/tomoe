#ifndef TOMOE_CONTROL_H
#define TOMOE_CONTROL_H
#include <stdbool.h>
#include <stddef.h>

struct tomoe;
struct control_server;

enum control_operation {
    CONTROL_INSPECT,
    CONTROL_RELOAD,
    CONTROL_MOUNT,
    CONTROL_UNMOUNT,
    CONTROL_EVENT,
    CONTROL_HIT_TEST,
    CONTROL_FRAMES,
    CONTROL_ANIMATION,
    CONTROL_MEMORY,
    CONTROL_TIMINGS,
    CONTROL_QUIT
};

void control_stop(struct control_server *server);
void control_serve(struct control_server *server);
int control_client(const char *path, enum control_operation operation, int count, char **arguments,
    char *error, size_t error_size);

#endif
