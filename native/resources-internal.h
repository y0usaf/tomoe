#ifndef TOMOE_RESOURCES_INTERNAL_H
#define TOMOE_RESOURCES_INTERNAL_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "pv.h"

enum { RES_HELD_MS = 50 };
#include "policy.h"

enum { LOOP_READABLE = 1, LOOP_WRITABLE = 2 };
int tomoe_test_flag(const char *name);

struct res_buffer {
    char *data;
    size_t length, capacity;
    bool failed;
};

void rb_append(struct res_buffer *buffer, const char *text, size_t length);
void rb_printf(struct res_buffer *buffer, const char *format, ...)
    __attribute__((format(printf, 2, 3)));
void rb_quoted(struct res_buffer *buffer, const char *text, size_t length);
void rb_quoted_cstring(struct res_buffer *buffer, const char *text);
char *rb_take(struct res_buffer *buffer);
void rb_free(struct res_buffer *buffer);

char *res_dup(const char *text);
char *res_dup_span(const char *text, size_t length);
bool res_push_event(struct tomoe *server, enum tomoe_event_type type, uint16_t flags,
    const char *text, size_t length);
void res_append_keyword(struct res_buffer *buffer, const char *name);
void res_engine_kick(void);
uint64_t res_now(void);

enum res_timer_clock { TIMER_CLOCK_NONE, TIMER_CLOCK_REFRESH, TIMER_CLOCK_SECOND, TIMER_CLOCK_MINUTE };

struct res_timer {
    char *owner, *name;
    int clock;
    int64_t declared;
    bool repeat, fired;
    uint64_t deadline;
};

enum res_watch_status {
    WATCH_READY,
    WATCH_CHANGED,
    WATCH_OVERFLOW,
    WATCH_UNAVAILABLE,
    WATCH_RESTORED,
    WATCH_READ_ERROR,
    WATCH_MISSING
};

struct res_watch {
    char *owner, *name, *path, *absolute;
    size_t limit;
    uint64_t source_id;
    struct file_watch *native;
    struct loop_source *source;
    int status;
    char *error;
    bool active;
    uint64_t retry;
};

enum res_exec_status {
    EXEC_PENDING,
    EXEC_RUNNING,
    EXEC_TERMINATING,
    EXEC_EXITED,
    EXEC_SIGNALED,
    EXEC_SPAWN_ERROR,
    EXEC_TIMEOUT,
    EXEC_OUTPUT_LIMIT,
    EXEC_IO_ERROR,
    EXEC_CANCELLED
};

struct res_lease {
    struct execution *job;
    struct loop_source *exit_watch;
    bool exited;
    bool stopped;
    bool background;
    int stop_error;
    int status;
    int code;
    uint64_t kill_at;
    uint64_t id;
};

struct res_exec {
    struct res_lease lease;
    char *owner, *name, *command;
    int64_t timeout;
    size_t limit;
    char *out, *err;
    size_t out_length, err_length;
    bool out_eof, err_eof;
    int status;
    int reason;
    int code;
    bool has_code;
    char *error;
    uint64_t deadline;
    bool delivered;
    struct loop_source *fd[3];
};

enum res_process_kind { PROCESS_ONCE, PROCESS_SERVICE, PROCESS_LAUNCH, PROCESS_SPAWN };

enum res_process_status {
    PROCESS_PENDING,
    PROCESS_LAUNCHED,
    PROCESS_RUNNING,
    PROCESS_WAITING,
    PROCESS_STOPPING,
    PROCESS_STOPPED,
    PROCESS_RECORDED,
    PROCESS_SPAWN_ERROR,
    PROCESS_CANCELLED
};

enum res_run_mode { RUN_ONCE_PER_SESSION, RUN_ONCE_PER_CONFIG_VERSION };
enum res_restart { RESTART_NEVER, RESTART_ON_FAILURE, RESTART_ON_EXIT };
enum res_reload { RELOAD_KEEP_IF_UNCHANGED, RELOAD_ALWAYS_RESTART };

struct res_process {
    char *owner;
    uint64_t token;
    uint64_t history_stamp;
    char *name;
    int kind;
    bool session;
    char *command;
    char **argv;
    size_t argc;
    char *cwd;
    char *directory;
    char **env;
    size_t env_count;
    char *service_directory;
    int run, restart, reload;
    bool focus, input;
    struct res_lease *lease;
    bool starts;
    int status;
    int attempts;
    int code;
    bool has_code;
    char *error;
    uint64_t deadline;
};

struct res_source {
    char *path;
    struct file_watch *native;
    struct loop_source *source;
    uint64_t loaded[3];
    uint64_t sample[3];
    bool have_loaded;
    bool have_sample;
    int stable;
};

struct res_producer {
    const char *key;
    int kind;
    void *native;
    struct loop_source *source;
    int fd;
    char *snapshot;
    uint64_t due;
};

enum res_producer_kind {
    PRODUCER_NETWORK,
    PRODUCER_MPRIS,
    PRODUCER_BATTERY,
    PRODUCER_NOTIFICATIONS,
    PRODUCER_AUDIO,
    PRODUCER_COUNT
};

struct want_timer {
    char *owner, *name;
    int clock;
    int64_t declared;
    bool repeat;
};

struct want_watch {
    char *owner, *name, *path, *directory;
    size_t limit;
    uint64_t source_id;
};

struct want_exec {
    char *owner, *name, *command;
    int64_t timeout;
    size_t limit;
};

struct want_process {
    char *owner, *name, *command;
    char **argv;
    size_t argc;
    char *cwd, *directory;
    char **env;
    size_t env_count;
    int kind, run, restart, reload;
    uint64_t token;
};

struct res_state {
    struct tomoe *server;
    struct loop_source *engine;
    bool started, engine_armed;
    uint64_t engine_due;

    struct res_timer *timers;
    size_t timer_count;

    struct res_watch *watches;
    size_t watch_count;

    struct res_exec *execs;
    size_t exec_count;
    struct res_exec *retired_execs;
    size_t retired_exec_count;
    size_t exec_retiring;

    struct res_process *processes;
    size_t process_count;
    struct res_process *retired;
    size_t retired_count;

    struct res_lease **leases;
    size_t lease_count;
    uint64_t lease_sequence;
    uint64_t commit_sequence;

    struct res_history { char *owner, *name; uint64_t stamp; } *history;
    size_t history_count;

    char *system_text;
    uint64_t system_due;

    struct res_source *sources;
    size_t source_count;
    uint64_t source_due;
    bool source_scanning;
    int source_scans;

    struct res_producer producers[PRODUCER_COUNT];
};

extern struct res_state res;

struct tomoe;
struct file_watch;
struct execution;

struct file_watch *tomoe_watch_open(const char *absolute_path, int *error);
void tomoe_watch_close(struct file_watch *watch);
int tomoe_watch_descriptor(const struct file_watch *watch);
int tomoe_watch_poll(struct file_watch *watch);
long tomoe_watch_read(struct file_watch *watch, unsigned char *buffer, unsigned long limit);

int tomoe_exec_supported(void);
struct execution *tomoe_exec_start(const char *command, int *error);
long tomoe_exec_read(struct execution *job, int which, void *buffer, size_t size);
int tomoe_exec_poll(struct execution *job);
int tomoe_exec_code(struct execution *job);
int tomoe_exec_fd(struct execution *job, int which);
int tomoe_exec_stop(struct execution *job, int sig);
int tomoe_exec_release(struct execution *job);
struct execution *tomoe_process_start(char *const argv[], const char *cwd, char *const envp[],
    int *error);
struct execution *tomoe_process_start_service(char *const argv[], const char *cwd,
    char *const envp[], int *error);
int tomoe_service_run_begin(const char *runtime, int *cleared, char *error, size_t error_size);
void tomoe_service_run_end(void);
const char *tomoe_service_token(void);
const char *tomoe_service_run_directory(void);
char *tomoe_service_directory_create(const char *owner, const char *name, char *error,
    size_t error_size);
void tomoe_service_directory_remove(char *path);
int tomoe_process_pid(struct execution *job);
int tomoe_process_group_alive(struct execution *job);
int tomoe_backlight_set(const char *name, unsigned value);
unsigned tomoe_nvml_count(void);
int tomoe_nvml_sample(unsigned index, unsigned *busy, unsigned *used, unsigned *total,
    unsigned *celsius);

void res_timers_service(uint64_t now, uint64_t *next);
void res_timers_free(void);

void res_watches_apply(struct want_watch *wanted, size_t count);
void res_watches_service(uint64_t now, uint64_t *next);
void res_watches_free(void);
void res_watch_deliver(struct res_watch *watch, int status, const char *content,
    const char *error, bool with_age);

void res_execs_apply(struct want_exec *wanted, size_t count);
void res_execs_service(uint64_t now, uint64_t *next);
void res_execs_free(void);
void res_execs_watch(struct res_exec *exec, int which, bool enable);
const char *res_exec_status_name(int status);

void res_processes_apply(struct want_process *wanted, size_t count);
void res_processes_service(uint64_t now, uint64_t *next);
const char *res_process_status_name(int status);
pv res_spawn(pv plist, bool launch);
size_t res_process_reservations(void);

uint64_t res_refresh_ms(void);
void res_processes_stop(void);
struct res_lease *res_lease_start(struct execution *job, bool session, bool background);
void res_lease_drop(struct res_lease *lease);
void res_lease_kill(struct res_lease *lease, bool cancel);
bool res_lease_reap(struct res_lease *lease, bool cancel);

bool res_background_window_p(int64_t pid);
void res_background_forget(void);

void res_services_start(void);
void res_services_stop(void);
void res_services_service(uint64_t now, uint64_t *next);
bool res_media(const char *method, char *error, size_t size);
bool res_backlight(int64_t percent, char *error, size_t size);
pv res_services_value(void);
pv res_system_value(void);
bool res_system_read(void);

bool res_sources_set(pv paths, char *error, size_t size);
void res_sources_service(uint64_t now, uint64_t *next);
void res_sources_free(void);

pv resources_read_text(const char *text, size_t length);
bool res_plist_get(pv plist, const char *name, pv *out);
void res_error_text(char *error, size_t size, const char *format, ...)
    __attribute__((format(printf, 3, 4)));
char *res_plist_string(pv value);
char *res_plist_string_field(pv plist, const char *name);
char **res_plist_environment(pv plist, size_t *count, char *error, size_t size);
int64_t res_process_parent(int64_t pid);

#endif
