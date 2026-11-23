#ifndef TOMOE_IPC_H
#define TOMOE_IPC_H
#include <stdbool.h>
#include <stddef.h>
#include "lisp.h"

struct tomoe;
struct ipc_server;

void ipc_stop(struct ipc_server *server);
void ipc_serve(struct ipc_server *server);
bool ipc_pending(struct ipc_server *server);
void ipc_drain(struct ipc_server *server, uint64_t budget_ms);
int ipc_client(const char *path, const char *method, lisp_value params, char *error, size_t error_size);

#endif
