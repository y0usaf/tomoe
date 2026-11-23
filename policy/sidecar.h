#ifndef TOMOE_SIDECAR_H
#define TOMOE_SIDECAR_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "lisp.h"
#include "policy_proto.h"

#define POLICY_HOOK_BOOT "TOMOE::TOMOE-BOOT"
#define POLICY_HOOK_DISPATCH "TOMOE::TOMOE-DISPATCH"
#define POLICY_HOOK_JSON "TOMOE::TOMOE-JSON"
#define POLICY_HOOK_CONTROL "TOMOE::TOMOE-CONTROL"
#define POLICY_HOOK_INSPECT "TOMOE::TOMOE-INSPECT"
#define POLICY_HOOK_RESUME_ENTRIES "TOMOE::%RESUME-ENTRIES"

struct tomoe;

typedef void (*loop_fd_fn)(void *data, int fd, uint32_t mask);
typedef void (*loop_timer_fn)(void *data);
struct loop_source;
struct loop_source *loop_watch_fd(struct tomoe *server, int fd, uint32_t mask, loop_fd_fn fn, void *data);
void loop_update_fd(struct loop_source *source, uint32_t mask);
void loop_remove(struct loop_source *source);
void loop_set_data(struct loop_source *source, void *data);
struct loop_source *loop_timer(struct tomoe *server, loop_timer_fn fn, void *data);
void loop_timer_arm(struct loop_source *source, uint64_t delay_ms);
uint64_t loop_now_ms(void);
int loop_dispatch(int timeout_ms);
void loop_request_quit(struct tomoe *server);
bool loop_quitting(void);

int policy_call(const char *hook, int argc, const lisp_value *argv, lisp_value *result,
    char *error, size_t error_size);
void policy_hook_begin(bool fresh);
void policy_hook_end_keep(void);

int core_call(int prim, int argc, const lisp_value *argv, lisp_value *result, char *error,
    size_t error_size);
void core_send(int prim, int argc, const lisp_value *argv);
void core_flush(void);
uint32_t core_watch_fd(int fd);
void core_unwatch_fd(uint32_t id);
bool core_connected(void);
void core_send_frame(uint16_t type, const void *a, size_t a_length, const void *b, size_t b_length);
void policy_shader_build(const uint8_t *payload, size_t length);
lisp_value core_memory_report(char *error, size_t error_size, bool *ok);

bool policy_link_work(void);
bool policy_link_ready(void);
void policy_link_loading(bool on);
int policy_link_open(int fd);
void policy_register_local(void);
void policy_register_proxies(void);
lisp_value policy_cached_identifier(uint32_t id, bool *found);

struct ipc_server;
struct control_server;
struct ipc_server *ipc_adopt(int fd);
void ipc_serve(struct ipc_server *server);
void ipc_stop(struct ipc_server *server);
bool ipc_pending(struct ipc_server *server);
void ipc_drain(struct ipc_server *server, uint64_t budget_ms);
void ipc_broadcast(const char *name, lisp_value value);
bool ipc_subscribed(void);
void ipc_screencast_answer(uint64_t token, lisp_value result);
struct control_server *control_adopt(int fd);
void control_serve(struct control_server *server);
void control_stop(struct control_server *server);

int policy_client(int argc, char **argv);
void policy_log(const char *level, const char *format, ...) __attribute__((format(printf, 2, 3)));

#endif
