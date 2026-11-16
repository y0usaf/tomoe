#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "backend.h"
#include "resources-internal.h"

extern char **environ;

static const char *exec_status_names[] = {
    "PENDING", "RUNNING", "TERMINATING", "EXITED", "SIGNALED", "SPAWN-ERROR",
    "TIMEOUT", "OUTPUT-LIMIT", "IO-ERROR", "CANCELLED"
};

const char *res_exec_status_name(int status) {
    if (status < 0 || status > EXEC_CANCELLED) return "PENDING";
    return exec_status_names[status];
}

static const char *process_status_names[] = {
    "PENDING", "LAUNCHED", "RUNNING", "WAITING", "STOPPING", "STOPPED", "RECORDED",
    "SPAWN-ERROR", "CANCELLED"
};

const char *res_process_status_name(int status) {
    if (status < 0 || status > PROCESS_CANCELLED) return "PENDING";
    return process_status_names[status];
}

static bool exec_terminal(int status) {
    switch (status) {
    case EXEC_PENDING: case EXEC_RUNNING: case EXEC_TERMINATING:
        return false;
    default:
        return true;
    }
}

struct res_lease *res_lease_start(struct execution *job, bool session, bool background) {
    struct res_lease *lease = calloc(1, sizeof(*lease));
    if (!lease) return NULL;
    lease->job = job;
    lease->background = background;
    lease->id = ++res.lease_sequence;
    if (session) {
        struct res_lease **leases = realloc(res.leases,
            (res.lease_count + 1) * sizeof(*leases));
        if (leases) {
            res.leases = leases;
            res.leases[res.lease_count++] = lease;
        }
    }
    return lease;
}

void res_lease_kill(struct res_lease *lease, bool cancel) {
    (void)cancel;
    if (!lease || !lease->job || lease->stopped) return;
    uint64_t now = res_now();
    int polled = tomoe_exec_poll(lease->job);
    bool kill = polled > 0 || (lease->kill_at && now >= lease->kill_at);
    if (!kill && lease->kill_at) return;
    int result = tomoe_exec_stop(lease->job, kill ? SIGKILL : SIGTERM);
    if (result == 1) {
        if (kill) lease->stopped = true;
        else lease->kill_at = now + 1000;
        lease->stop_error = 0;
        if (!kill) return;
        return;
    }
    if (result != lease->stop_error) {
        lease->stop_error = result;
        fprintf(stderr, "tomoe: Execution cancellation failed: errno %d.\n", -result);
    }
}

bool res_lease_reap(struct res_lease *lease, bool cancel) {
    if (!lease) return true;
    if (!lease->job) return true;
    if (cancel) res_lease_kill(lease, true);
    int status = tomoe_exec_poll(lease->job);
    if (status < 0) {
        fprintf(stderr, "tomoe: Cannot reap managed process: errno %d.\n", -status);
        return false;
    }
    if (status > 0) {
        res_lease_kill(lease, true);
        lease->status = status;
        lease->code = tomoe_exec_code(lease->job);
        tomoe_exec_release(lease->job);
        lease->job = NULL;
        return true;
    }
    return false;
}

void res_lease_drop(struct res_lease *lease) {
    if (!lease) return;
    for (size_t index = 0; index < res.process_count; index++) {
        if (res.processes[index].lease == lease) res.processes[index].lease = NULL;
    }
    for (size_t index = 0; index < res.retired_count; index++) {
        if (res.retired[index].lease == lease) res.retired[index].lease = NULL;
    }
    if (lease->job) {
        tomoe_exec_stop(lease->job, SIGKILL);
        tomoe_exec_poll(lease->job);
        tomoe_exec_release(lease->job);
        lease->job = NULL;
    }
    for (size_t index = 0; index < res.lease_count; index++) {
        if (res.leases[index] == lease) {
            res.leases[index] = res.leases[--res.lease_count];
            break;
        }
    }
    free(lease);
}

static struct res_lease *lease_find(struct execution *job) {
    if (!job) return NULL;
    for (size_t index = 0; index < res.lease_count; index++) {
        if (res.leases[index]->job == job) return res.leases[index];
    }
    return NULL;
}

static void lease_drop_job(struct execution *job) {
    if (!job) return;
    struct res_lease *lease = lease_find(job);
    if (lease) { res_lease_drop(lease); return; }
    int status = tomoe_exec_poll(job);
    if (status > 0) tomoe_exec_release(job);
}

static void exec_callback(void *data, int fd, uint32_t mask) {
    struct res_exec *exec = data;
    (void)fd;
    (void)mask;
    if (!res.started) return;
    (void)exec;
    res_engine_kick();
}

void res_execs_watch(struct res_exec *exec, int which, bool enable) {
    if (which < 0 || which > 2) return;
    if (enable) {
        if (exec->fd[which] || !exec->lease.job) return;
        int descriptor = tomoe_exec_fd(exec->lease.job, which);
        if (descriptor < 0) return;
        exec->fd[which] = loop_watch_fd(res.server, descriptor, LOOP_READABLE,
            exec_callback, exec);
        return;
    }
    if (exec->fd[which]) { loop_remove(exec->fd[which]); exec->fd[which] = NULL; }
}

static void exec_unwatch(struct res_exec *exec) {
    for (int which = 0; which < 3; which++) res_execs_watch(exec, which, false);
}

static bool exec_utf8(const unsigned char *text, size_t length, char **out, size_t *out_length) {
    char *decoded = malloc(length * 3 + 1);
    if (!decoded) return false;
    size_t written = 0;
    size_t index = 0;
    while (index < length) {
        unsigned char first = text[index];
        uint32_t code = 0;
        size_t width = 1;
        if (first < 0x80) code = first;
        else if ((first & 0xE0) == 0xC0) { code = first & 0x1F; width = 2; }
        else if ((first & 0xF0) == 0xE0) { code = first & 0x0F; width = 3; }
        else if ((first & 0xF8) == 0xF0) { code = first & 0x07; width = 4; }
        else { code = 0xFFFD; }
        bool valid = width == 1 || code != 0xFFFD;
        for (size_t next = 1; valid && next < width; next++) {
            if (index + next >= length || (text[index + next] & 0xC0) != 0x80) valid = false;
            else code = (code << 6) | (text[index + next] & 0x3F);
        }
        if (valid) {
            if ((width == 2 && code < 0x80) || (width == 3 && code < 0x800) ||
                    (width == 4 && code < 0x10000) || code > 0x10FFFF ||
                    (code >= 0xD800 && code <= 0xDFFF)) code = 0xFFFD;
        } else {
            code = 0xFFFD;
            width = 1;
        }
        if (code < 0x80) decoded[written++] = (char)code;
        else if (code < 0x800) {
            decoded[written++] = (char)(0xC0 | (code >> 6));
            decoded[written++] = (char)(0x80 | (code & 0x3F));
        } else if (code < 0x10000) {
            decoded[written++] = (char)(0xE0 | (code >> 12));
            decoded[written++] = (char)(0x80 | ((code >> 6) & 0x3F));
            decoded[written++] = (char)(0x80 | (code & 0x3F));
        } else {
            decoded[written++] = (char)(0xF0 | (code >> 18));
            decoded[written++] = (char)(0x80 | ((code >> 12) & 0x3F));
            decoded[written++] = (char)(0x80 | ((code >> 6) & 0x3F));
            decoded[written++] = (char)(0x80 | (code & 0x3F));
        }
        index += width;
    }
    decoded[written] = 0;
    *out = decoded;
    *out_length = written;
    return true;
}

static bool unicode_space(uint32_t code) {
    return (code >= 0x09 && code <= 0x0D) || code == 0x20 || code == 0x85 ||
           code == 0xA0 || code == 0x1680 || (code >= 0x2000 && code <= 0x200A) ||
           code == 0x2028 || code == 0x2029 || code == 0x202F || code == 0x205F ||
           code == 0x3000;
}

static char *exec_output_text(const char *text, size_t length) {
    if (!text || !length) return res_dup("");
    char *decoded = NULL;
    size_t decoded_length = 0;
    if (!exec_utf8((const unsigned char *)text, length, &decoded, &decoded_length))
        return res_dup("");
    size_t start = 0, end = decoded_length;
    while (start < end) {
        unsigned char character = (unsigned char)decoded[start];
        uint32_t code = character;
        size_t width = 1;
        if (character >= 0xF0) { code = character & 0x07; width = 4; }
        else if (character >= 0xE0) { code = character & 0x0F; width = 3; }
        else if (character >= 0xC0) { code = character & 0x1F; width = 2; }
        for (size_t next = 1; next < width && start + next < decoded_length; next++)
            code = (code << 6) | ((unsigned char)decoded[start + next] & 0x3F);
        if (!unicode_space(code)) break;
        start += width;
    }
    while (end > start) {
        size_t width = 1;
        size_t scan = end - 1;
        while (scan > start && ((unsigned char)decoded[scan] & 0xC0) == 0x80) { scan--; width++; }
        unsigned char character = (unsigned char)decoded[scan];
        uint32_t code = character;
        if (width == 4) code = character & 0x07;
        else if (width == 3) code = character & 0x0F;
        else if (width == 2) code = character & 0x1F;
        for (size_t next = 1; next < width; next++)
            code = (code << 6) | ((unsigned char)decoded[scan + next] & 0x3F);
        if (!unicode_space(code)) break;
        end = scan;
    }
    char *result = res_dup_span(decoded + start, end - start);
    free(decoded);
    return result;
}

static void exec_start(struct res_exec *exec) {
    int error = 0;
    struct execution *job = tomoe_exec_start(exec->command, &error);
    if (!job) {
        exec->status = EXEC_SPAWN_ERROR;
        struct res_buffer message = {0};
        rb_printf(&message, "Cannot start async command: errno %d.", error);
        free(exec->error);
        exec->error = rb_take(&message);
        exec->deadline = res_now();
        return;
    }
    exec->lease.job = job;
    exec->status = EXEC_RUNNING;
    exec->deadline = res_now() + (uint64_t)(exec->timeout > 0 ? exec->timeout : 0);
    exec->out = malloc(exec->limit + 1);
    exec->err = malloc(exec->limit + 1);
    if (!exec->out || !exec->err) {
        exec->status = EXEC_TERMINATING;
        exec->reason = EXEC_IO_ERROR;
        free(exec->error);
        exec->error = res_dup("Cannot allocate execution output buffers.");
    }
    exec->out_length = exec->err_length = 0;
    exec->out_eof = exec->err_eof = false;
    res_execs_watch(exec, 0, true);
    res_execs_watch(exec, 1, true);
    res_execs_watch(exec, 2, true);
}

static int exec_drain(struct res_exec *exec, int which) {
    size_t budget = 4096;
    int rounds = 0;
    while (budget && rounds++ < 16) {
        char *buffer = which ? exec->err : exec->out;
        if (!buffer) {
            exec->status = EXEC_IO_ERROR;
            return -1;
        }
        size_t offset = which ? exec->err_length : exec->out_length;
        size_t used = exec->out_length + exec->err_length;
        size_t remaining = used < exec->limit ? exec->limit - used : 0;
        size_t want = remaining + 1 < budget ? remaining + 1 : budget;
        long count = tomoe_exec_read(exec->lease.job, which, buffer + offset, want);
        if (count == 0) {
            if (which) exec->err_eof = true;
            else exec->out_eof = true;
            return 0;
        }
        if (count > 0) {
            size_t keep = (size_t)count < remaining ? (size_t)count : remaining;
            if (which) exec->err_length += keep;
            else exec->out_length += keep;
            if ((size_t)count > remaining) return 1;
            budget -= (size_t)count;
            continue;
        }
        int error = (int)-count;
        if (error == EAGAIN || error == EINTR) return 0;
        exec->status = EXEC_IO_ERROR;
        struct res_buffer message = {0};
        rb_printf(&message, "Cannot read async output: errno %d.", error);
        free(exec->error);
        exec->error = rb_take(&message);
        return -1;
    }
    return 0;
}

static void exec_release(struct res_exec *exec) {
    exec_unwatch(exec);
    if (!exec->lease.job) return;
    int status = tomoe_exec_poll(exec->lease.job);
    if (status > 0) {
        exec->lease.code = tomoe_exec_code(exec->lease.job);
        tomoe_exec_release(exec->lease.job);
        exec->lease.job = NULL;
    }
}

static void exec_deliver(struct res_exec *exec) {
    char *out = exec_output_text(exec->out, exec->out_length);
    char *err = exec_output_text(exec->err, exec->err_length);
    struct res_buffer text = {0};
    rb_append(&text, "(:TYPE :EXEC :OWNER ", 20);
    rb_quoted_cstring(&text, exec->owner);
    rb_append(&text, " :NAME ", 7);
    res_append_keyword(&text, exec->name);
    rb_append(&text, " :STATUS ", 9);
    res_append_keyword(&text, res_exec_status_name(exec->status));
    rb_append(&text, " :CODE ", 7);
    if (exec->has_code) rb_printf(&text, "%d", exec->code);
    else rb_append(&text, "NIL", 3);
    rb_append(&text, " :STDOUT ", 9);
    rb_quoted_cstring(&text, out ? out : "");
    rb_append(&text, " :STDERR ", 9);
    rb_quoted_cstring(&text, err ? err : "");
    rb_append(&text, " :ERROR ", 8);
    if (exec->error) rb_quoted_cstring(&text, exec->error);
    else rb_append(&text, "NIL", 3);
    rb_append(&text, ")", 1);
    char *message = rb_take(&text);
    if (message) {
        res_push_event(res.server, EV_EXEC, EVF_FACT, message, text.length);
        free(message);
    }
    free(out);
    free(err);
    exec->delivered = true;
    free(exec->out);
    free(exec->err);
    exec->out = exec->err = NULL;
    exec->out_length = exec->err_length = 0;
}

static void exec_poll(struct res_exec *exec, uint64_t now, uint64_t *next) {
    if (exec->status == EXEC_PENDING) exec_start(exec);
    if (exec->status == EXEC_RUNNING) {
        if (exec_drain(exec, 0) || exec_drain(exec, 1)) {
            exec->status = EXEC_TERMINATING;
            exec->reason = EXEC_OUTPUT_LIMIT;
            free(exec->error);
            exec->error = res_dup("Combined output exceeded the byte limit.");
        }
        if (exec->status == EXEC_RUNNING && exec->out_eof) res_execs_watch(exec, 1, false);
        if (exec->status == EXEC_RUNNING && exec->err_eof) res_execs_watch(exec, 2, false);
    }
    if (exec->status == EXEC_IO_ERROR) {
        exec->status = EXEC_TERMINATING;
        exec->reason = EXEC_IO_ERROR;
    }
    if (exec->status == EXEC_TERMINATING) res_lease_kill(&exec->lease, true);
    if (exec->lease.job) {
        int status = tomoe_exec_poll(exec->lease.job);
        if (status < 0) {
            fprintf(stderr, "tomoe: Cannot reap async command: errno %d.\n", -status);
        }
        if (exec->status == EXEC_RUNNING && !(status > 0 && exec->out_eof && exec->err_eof) &&
                now >= exec->deadline) {
            exec->status = EXEC_TERMINATING;
            exec->reason = EXEC_TIMEOUT;
            free(exec->error);
            exec->error = res_dup("Execution exceeded its timeout.");
        }
        if (status > 0 && (exec->status != EXEC_RUNNING ||
                (exec->out_eof && exec->err_eof))) {
            if (exec->status == EXEC_RUNNING) {
                exec->status = status == 1 ? EXEC_EXITED : EXEC_SIGNALED;
                free(exec->error);
                exec->error = NULL;
            } else if (exec->reason) {
                exec->status = exec->reason;
            }
            exec->code = tomoe_exec_code(exec->lease.job);
            exec->has_code = true;
            exec_release(exec);
        }
    }
    if (!exec->delivered && !exec->lease.job && exec_terminal(exec->status)) exec_deliver(exec);
    if (!exec->delivered) {
        uint64_t due = now + 1000;
        if (exec->status == EXEC_PENDING) due = now;
        else if (exec->lease.kill_at) due = exec->lease.kill_at;
        else if (exec->status == EXEC_RUNNING) due = exec->deadline;
        if (due < *next) *next = due;
    }
}

void res_execs_apply(struct want_exec *wanted, size_t count) {
    bool *used = calloc(res.exec_count ? res.exec_count : 1, sizeof(bool));
    struct res_exec *execs = calloc(count ? count : 1, sizeof(struct res_exec));
    uint64_t now = res_now();
    if (!used || !execs) {
        free(used);
        free(execs);
        for (size_t index = 0; index < count; index++) {
            free(wanted[index].owner);
            free(wanted[index].name);
            free(wanted[index].command);
        }
        return;
    }
    for (size_t index = 0; index < count; index++) {
        ssize_t match = -1;
        for (size_t old = 0; old < res.exec_count; old++) {
            struct res_exec *existing = &res.execs[old];
            if (used[old]) continue;
            if (strcmp(existing->owner, wanted[index].owner) ||
                    strcmp(existing->name, wanted[index].name) ||
                    existing->timeout != wanted[index].timeout ||
                    existing->limit != wanted[index].limit ||
                    strcmp(existing->command, wanted[index].command)) continue;
            match = (ssize_t)old;
            break;
        }
        if (match >= 0) {
            execs[index] = res.execs[match];
            for (int which = 0; which < 3; which++) loop_set_data(execs[index].fd[which], &execs[index]);
            used[match] = true;
            free(wanted[index].owner);
            free(wanted[index].name);
            free(wanted[index].command);
            continue;
        }
        execs[index].owner = wanted[index].owner;
        execs[index].name = wanted[index].name;
        execs[index].command = wanted[index].command;
        execs[index].timeout = wanted[index].timeout;
        execs[index].limit = wanted[index].limit;
        execs[index].status = EXEC_PENDING;
        execs[index].deadline = now;
    }
    for (size_t old = 0; old < res.exec_count; old++) {
        if (used[old]) continue;
        struct res_exec *existing = &res.execs[old];
        if (existing->lease.job) {
            exec_unwatch(existing);
            struct res_exec *retired = realloc(res.retired_execs,
                (res.retired_exec_count + 1) * sizeof(*retired));
            if (retired) {
                res.retired_execs = retired;
                res.retired_execs[res.retired_exec_count++] = *existing;
                memset(existing, 0, sizeof(*existing));
            }
            continue;
        }
        free(existing->owner);
        free(existing->name);
        free(existing->command);
        free(existing->error);
        free(existing->out);
        free(existing->err);
    }
    free(used);
    free(res.execs);
    res.execs = execs;
    res.exec_count = count;
}

void res_execs_service(uint64_t now, uint64_t *next) {
    for (size_t index = 0; index < res.retired_exec_count;) {
        struct res_exec *retired = &res.retired_execs[index];
        res_lease_kill(&retired->lease, true);
        if (res_lease_reap(&retired->lease, true)) {
            free(retired->owner);
            free(retired->name);
            free(retired->command);
            free(retired->error);
            free(retired->out);
            free(retired->err);
            res.retired_execs[index] = res.retired_execs[--res.retired_exec_count];
            continue;
        }
        index++;
    }
    res.exec_retiring = res.retired_exec_count;
    size_t delivered = 0;
    for (size_t index = 0; index < res.exec_count; index++) {
        struct res_exec *exec = &res.execs[index];
        if (exec->delivered) continue;
        if (delivered >= 64) {
            if (exec->deadline < *next) *next = exec->deadline;
            continue;
        }
        delivered++;
        exec_poll(exec, now, next);
    }
    for (size_t index = 0; index < res.retired_exec_count; index++) {
        struct res_exec *retired = &res.retired_execs[index];
        if (retired->lease.kill_at && retired->lease.kill_at < *next)
            *next = retired->lease.kill_at;
    }
}

void res_execs_free(void) {
    for (size_t index = 0; index < res.exec_count; index++) {
        struct res_exec *exec = &res.execs[index];
        exec_unwatch(exec);
        if (exec->lease.job) lease_drop_job(exec->lease.job);
        free(exec->owner);
        free(exec->name);
        free(exec->command);
        free(exec->error);
        free(exec->out);
        free(exec->err);
    }
    free(res.execs);
    res.execs = NULL;
    res.exec_count = 0;
    for (size_t index = 0; index < res.retired_exec_count; index++) {
        struct res_exec *retired = &res.retired_execs[index];
        if (retired->lease.job) lease_drop_job(retired->lease.job);
        free(retired->owner);
        free(retired->name);
        free(retired->command);
        free(retired->error);
        free(retired->out);
        free(retired->err);
    }
    free(res.retired_execs);
    res.retired_execs = NULL;
    res.retired_exec_count = 0;
}

static uint64_t history_get(const char *owner, const char *name) {
    for (size_t index = 0; index < res.history_count; index++) {
        if (!strcmp(res.history[index].owner, owner) && !strcmp(res.history[index].name, name))
            return res.history[index].stamp;
    }
    return 0;
}

static void history_set(const char *owner, const char *name, uint64_t stamp) {
    for (size_t index = 0; index < res.history_count; index++) {
        if (!strcmp(res.history[index].owner, owner) && !strcmp(res.history[index].name, name)) {
            res.history[index].stamp = stamp;
            return;
        }
    }
    if (res.history_count >= 4096) return;
    struct res_history *history = realloc(res.history,
        (res.history_count + 1) * sizeof(*history));
    if (!history) return;
    res.history = history;
    res.history[res.history_count].owner = res_dup(owner);
    res.history[res.history_count].name = res_dup(name);
    res.history[res.history_count].stamp = stamp;
    res.history_count++;
}

size_t res_process_reservations(void) {
    size_t total = res.lease_count + res.retired_count;
    for (size_t index = 0; index < res.process_count; index++) {
        struct res_process *process = &res.processes[index];
        if (process->kind == PROCESS_SERVICE || process->status == PROCESS_PENDING)
            total++;
    }
    return total;
}

static size_t process_reservations(void) {
    size_t total = res.lease_count + res.retired_count;
    for (size_t index = 0; index < res.process_count; index++) {
        struct res_process *process = &res.processes[index];
        if (process->kind == PROCESS_SERVICE || process->status == PROCESS_PENDING)
            total++;
    }
    return total;
}

static bool process_arguments_equal(const struct res_process *process,
        const struct want_process *want) {
    if (process->kind != want->kind) return false;
    if (!!process->command != !!want->command) return false;
    if (process->command && strcmp(process->command, want->command)) return false;
    if (process->argc != want->argc) return false;
    for (size_t index = 0; index < process->argc; index++) {
        if (strcmp(process->argv[index], want->argv[index])) return false;
    }
    if (!!process->cwd != !!want->cwd) return false;
    if (process->cwd && strcmp(process->cwd, want->cwd)) return false;
    if (process->env_count != want->env_count) return false;
    for (size_t index = 0; index < process->env_count; index++) {
        if (strcmp(process->env[index], want->env[index])) return false;
    }
    return process->run == want->run && process->restart == want->restart &&
           process->reload == want->reload;
}

static void process_free(struct res_process *process) {
    tomoe_service_directory_remove(process->service_directory);
    free(process->owner);
    free(process->name);
    free(process->command);
    for (size_t index = 0; index < process->argc; index++) free(process->argv[index]);
    free(process->argv);
    free(process->cwd);
    free(process->directory);
    for (size_t index = 0; index < process->env_count; index++) free(process->env[index]);
    free(process->env);
    free(process->error);
    memset(process, 0, sizeof(*process));
}

static void process_adopt(struct res_process *process, const struct want_process *want) {
    process->owner = want->owner;
    process->name = want->name;
    process->kind = want->kind;
    process->command = want->command;
    process->argv = want->argv;
    process->argc = want->argc;
    process->cwd = want->cwd;
    process->directory = want->directory;
    process->env = want->env;
    process->env_count = want->env_count;
    process->run = want->run;
    process->restart = want->restart;
    process->reload = want->reload;
    process->token = want->token;
    process->session = want->kind == PROCESS_ONCE;
    process->status = PROCESS_PENDING;
}

void res_processes_apply(struct want_process *wanted, size_t count) {
    bool *used = calloc(res.process_count ? res.process_count : 1, sizeof(bool));
    struct res_process *processes = calloc(count ? count : 1, sizeof(struct res_process));
    uint64_t now = res_now();
    if (!used || !processes) {
        free(used);
        free(processes);
        for (size_t index = 0; index < count; index++) {
            free(wanted[index].owner);
            free(wanted[index].name);
            free(wanted[index].command);
            free(wanted[index].cwd);
            free(wanted[index].directory);
            for (size_t argument = 0; argument < wanted[index].argc; argument++)
                free(wanted[index].argv[argument]);
            free(wanted[index].argv);
            for (size_t entry = 0; entry < wanted[index].env_count; entry++)
                free(wanted[index].env[entry]);
            free(wanted[index].env);
        }
        return;
    }
    for (size_t index = 0; index < count; index++) {
        struct want_process *want = &wanted[index];
        ssize_t match = -1;
        for (size_t old = 0; old < res.process_count; old++) {
            if (used[old]) continue;
            struct res_process *existing = &res.processes[old];
            if (strcmp(existing->owner, want->owner) || strcmp(existing->name, want->name))
                continue;
            match = (ssize_t)old;
            break;
        }
        struct res_process *old = match >= 0 ? &res.processes[match] : NULL;
        bool same = old && process_arguments_equal(old, want);
        bool same_source = old && old->token == want->token;
        bool once = want->kind == PROCESS_ONCE;
        uint64_t stamp = history_get(want->owner, want->name);
        bool due = once && (stamp == 0 ||
            (want->run == RUN_ONCE_PER_CONFIG_VERSION && stamp != want->token));
        bool keep = same && (once ? (!due || (same_source && old->status != PROCESS_SPAWN_ERROR))
            : (same_source || (want->reload == RELOAD_KEEP_IF_UNCHANGED &&
                (old->status == PROCESS_RUNNING || old->status == PROCESS_WAITING))));
        if (keep) {
            processes[index] = *old;
            processes[index].token = want->token;
            memset(old, 0, sizeof(*old));
            used[match] = true;
            free(want->owner);
            free(want->name);
            free(want->command);
            free(want->cwd);
            free(want->directory);
            for (size_t argument = 0; argument < want->argc; argument++) free(want->argv[argument]);
            free(want->argv);
            for (size_t entry = 0; entry < want->env_count; entry++) free(want->env[entry]);
            free(want->env);
            continue;
        }
        process_adopt(&processes[index], want);
        processes[index].status = (once && !due) ? PROCESS_RECORDED : PROCESS_PENDING;
        processes[index].history_stamp = stamp;
        processes[index].deadline = now;
    }
    for (size_t old = 0; old < res.process_count; old++) {
        if (used[old]) continue;
        struct res_process *existing = &res.processes[old];
        if (existing->kind != PROCESS_ONCE && existing->lease && existing->lease->job) {
            struct res_process *retired = realloc(res.retired,
                (res.retired_count + 1) * sizeof(*retired));
            if (retired) {
                res.retired = retired;
                res.retired[res.retired_count++] = *existing;
                memset(existing, 0, sizeof(*existing));
                continue;
            }
        }
        process_free(existing);
    }
    free(used);
    free(res.processes);
    res.processes = processes;
    res.process_count = count;
}

static char *process_cwd(const struct res_process *process) {
    if (!process->cwd) return NULL;
    if (process->cwd[0] == '/' || !process->directory || !*process->directory)
        return res_dup(process->cwd);
    size_t directory_length = strlen(process->directory);
    size_t cwd_length = strlen(process->cwd);
    size_t separator = process->directory[directory_length - 1] == '/' ? 0 : 1;
    char *cwd = malloc(directory_length + separator + cwd_length + 1);
    if (!cwd) return NULL;
    memcpy(cwd, process->directory, directory_length);
    if (separator) cwd[directory_length] = '/';
    memcpy(cwd + directory_length + separator, process->cwd, cwd_length + 1);
    return cwd;
}

static bool overridden(const char *name, char **entries, size_t count) {
    size_t length = strlen(name);
    for (size_t index = 0; index < count; index++) {
        if (!strncmp(entries[index], name, length) && entries[index][length] == '=')
            return true;
    }
    return false;
}

static bool service_environment_entry(const char *entry) {
    return !strncmp(entry, "TOMOE_SERVICE_TOKEN=", 20) ||
           !strncmp(entry, "TOMOE_SERVICE_RUNTIME_DIR=", 26);
}

static char **process_environ(const struct res_process *process, const char *token,
        const char *service_token, const char *service_directory, char ***allocated) {
    size_t environment_count = 0;
    while (environ && environ[environment_count]) environment_count++;
    size_t capacity = process->env_count + 5 + environment_count + 1;
    char **entries = calloc(capacity, sizeof(char *));
    char **owned = calloc(capacity, sizeof(char *));
    bool service = service_token && service_directory;
    if (!entries || !owned) { free(entries); free(owned); return NULL; }
    size_t count = 0, owned_count = 0;
    if (service) {
        static const char *names[] = { "TOMOE_SERVICE_TOKEN", "TOMOE_SERVICE_RUNTIME_DIR" };
        const char *values[] = { service_token, service_directory };
        for (size_t index = 0; index < 2; index++) {
            size_t length = strlen(names[index]) + strlen(values[index]) + 2;
            char *entry = malloc(length);
            if (!entry) continue;
            snprintf(entry, length, "%s=%s", names[index], values[index]);
            entries[count++] = entry;
            owned[owned_count++] = entry;
        }
    }
    for (size_t index = 0; index < process->env_count; index++) {
        const char *entry = process->env[index];
        if (service && service_environment_entry(entry)) continue;
        entries[count++] = (char *)entry;
    }
    if (token) {
        static const char *names[] = { "XDG_ACTIVATION_TOKEN", "DESKTOP_STARTUP_ID" };
        for (size_t index = 0; index < 2; index++) {
            if (overridden(names[index], entries, count)) continue;
            size_t length = strlen(names[index]) + strlen(token) + 2;
            char *entry = malloc(length);
            if (!entry) continue;
            snprintf(entry, length, "%s=%s", names[index], token);
            entries[count++] = entry;
            owned[owned_count++] = entry;
        }
    }
    for (size_t index = 0; index < environment_count; index++) {
        const char *entry = environ[index];
        if (service && service_environment_entry(entry)) continue;
        const char *separator = strchr(entry, '=');
        if (!separator) continue;
        size_t name_length = (size_t)(separator - entry);
        char *name = res_dup_span(entry, name_length);
        bool skip = name && overridden(name, entries, count);
        free(name);
        if (!skip) entries[count++] = (char *)entry;
    }
    entries[count] = NULL;
    *allocated = owned;
    return entries;
}

static void process_start(struct res_process *process) {
    char *shell = getenv("TOMOE_SHELL");
    if (!shell || !*shell) shell = "/bin/sh";
    char *shell_argv[4];
    char **argv = process->argv;
    if (process->command) {
        shell_argv[0] = shell;
        shell_argv[1] = "-c";
        shell_argv[2] = process->command;
        shell_argv[3] = NULL;
        argv = shell_argv;
    }
    char *cwd = process_cwd(process);
    const char *token = NULL;
    char *revoke = NULL;
    process->attempts++;
    if (process->session && process->focus && process->input) {
        token = tomoe_activation_token(res.server);
        if (!token) {
            process->status = PROCESS_SPAWN_ERROR;
            free(process->error);
            process->error = res_dup("Cannot issue application activation token.");
            free(cwd);
            return;
        }
        revoke = res_dup(token);
    }
    bool service = process->kind == PROCESS_SERVICE;
    char *service_directory = NULL;
    if (service) {
        char message[256];
        service_directory = tomoe_service_directory_create(process->owner, process->name,
            message, sizeof(message));
        if (!service_directory) {
            process->status = PROCESS_SPAWN_ERROR;
            free(process->error);
            process->error = res_dup(message);
            free(cwd);
            return;
        }
    }
    char **owned = NULL;
    char **environment = process_environ(process, token,
        service ? tomoe_service_token() : NULL, service_directory, &owned);
    int error = 0;
    struct execution *job = NULL;
    if (environment && argv && service)
        job = tomoe_process_start_service(argv, cwd, environment, &error);
    else if (environment && argv)
        job = tomoe_process_start(argv, cwd, environment, &error);
    if (!job) {
        if (revoke) tomoe_activation_revoke(res.server, revoke);
        process->status = PROCESS_SPAWN_ERROR;
        struct res_buffer message = {0};
        rb_printf(&message, "Cannot start process %s: errno %d.",
                  argv && argv[0] ? argv[0] : "", error ? error : ENOMEM);
        free(process->error);
        process->error = rb_take(&message);
        tomoe_service_directory_remove(service_directory);
        service_directory = NULL;
    } else {
        process->service_directory = service_directory;
        service_directory = NULL;
        process->lease = res_lease_start(job, process->session, !process->focus);
        process->status = process->session ? PROCESS_LAUNCHED : PROCESS_RUNNING;
        process->deadline = res_now() + 1000;
        free(process->error);
        process->error = NULL;
        process->has_code = false;
        if (process->kind == PROCESS_ONCE) history_set(process->owner, process->name, process->token);
    }
    if (owned) {
        for (size_t index = 0; owned[index]; index++) free(owned[index]);
    }
    free(owned);
    free(environment);
    free(cwd);
    free(revoke);
}

static bool process_retired_identity(const struct res_process *process) {
    for (size_t index = 0; index < res.retired_count; index++) {
        if (!strcmp(res.retired[index].owner, process->owner) &&
                !strcmp(res.retired[index].name, process->name))
            return true;
    }
    return false;
}

static bool process_restart(const struct res_process *process, int status, int code) {
    if (process->restart == RESTART_ON_EXIT) return true;
    if (process->restart == RESTART_ON_FAILURE) return status == 2 || code != 0;
    return false;
}

static void leases_service(uint64_t now, uint64_t *next) {
    for (size_t index = 0; index < res.lease_count;) {
        struct res_lease *lease = res.leases[index];
        if (!lease->job) { res_lease_drop(lease); continue; }
        int status = tomoe_exec_poll(lease->job);
        if (status < 0) fprintf(stderr, "tomoe: Cannot reap session process: errno %d.\n", -status);
        if (lease->stopped) {
            if (res_lease_reap(lease, true)) { res_lease_drop(lease); continue; }
        } else if (status > 0) {
            int alive = tomoe_process_group_alive(lease->job);
            if (alive < 0)
                fprintf(stderr, "tomoe: Cannot inspect session process group: errno %d.\n", -alive);
            if (alive == 0) {
                tomoe_exec_release(lease->job);
                lease->job = NULL;
                res_lease_drop(lease);
                continue;
            }
        }
        if (lease->job && lease->kill_at && lease->kill_at < *next) *next = lease->kill_at;
        index++;
    }
    (void)now;
}

bool res_background_window_p(int64_t pid) {
    if (pid <= 0) return false;
    for (size_t index = 0; index < res.lease_count; index++) {
        struct res_lease *lease = res.leases[index];
        if (!lease->background || !lease->job || lease->stopped) continue;
        int root = tomoe_process_pid(lease->job);
        if (root <= 0) continue;
        int64_t current = pid;
        for (int depth = 0; depth < 64; depth++) {
            if (current == root) return true;
            if (current <= 1) break;
            current = res_process_parent(current);
        }
    }
    return false;
}

void res_background_forget(void) {
    for (size_t index = 0; index < res.lease_count; index++) {
        struct res_lease *lease = res.leases[index];
        if (!lease->job || lease->stopped) lease->background = false;
    }
}

void res_processes_service(uint64_t now, uint64_t *next) {
    for (size_t index = 0; index < res.retired_count;) {
        struct res_process *process = &res.retired[index];
        if (!process->lease) {
            process_free(process);
            res.retired[index] = res.retired[--res.retired_count];
            continue;
        }
        res_lease_kill(process->lease, true);
        if (res_lease_reap(process->lease, true)) {
            res_lease_drop(process->lease);
            process_free(process);
            res.retired[index] = res.retired[--res.retired_count];
            continue;
        }
        if (process->lease->kill_at && process->lease->kill_at < *next)
            *next = process->lease->kill_at;
        index++;
    }
    for (size_t index = 0; index < res.process_count; index++) {
        struct res_process *process = &res.processes[index];
        bool active = process->status == PROCESS_PENDING || process->status == PROCESS_WAITING ||
                      process->status == PROCESS_RUNNING || process->status == PROCESS_STOPPING;
        if (!active) continue;
        if (process->lease && process->lease->job &&
                (process->status == PROCESS_RUNNING || process->status == PROCESS_STOPPING) &&
                now >= process->deadline) {
            bool stopping = process->status == PROCESS_STOPPING;
            if (res_lease_reap(process->lease, stopping)) {
                process->code = process->lease->code;
                process->has_code = true;
                if (stopping) process->status = PROCESS_SPAWN_ERROR;
                else if (process_restart(process, process->lease->status, process->code))
                    process->status = PROCESS_WAITING;
                else process->status = PROCESS_STOPPED;
                res_lease_drop(process->lease);
                process->lease = NULL;
                tomoe_service_directory_remove(process->service_directory);
                process->service_directory = NULL;
            } else {
                process->deadline = now + 1000;
            }
        }
        if ((process->status == PROCESS_PENDING || process->status == PROCESS_WAITING) &&
                !process_retired_identity(process)) {
            process_start(process);
        }
        if (process->status == PROCESS_PENDING || process->status == PROCESS_WAITING ||
                process->status == PROCESS_RUNNING || process->status == PROCESS_STOPPING) {
            uint64_t due = process->lease && process->lease->kill_at ? process->lease->kill_at
                                                                     : process->deadline;
            if (due < *next) *next = due;
        }
    }
    leases_service(now, next);
    res_background_forget();
}

void res_processes_stop(void) {
    for (size_t index = 0; index < res.process_count; index++) {
        struct res_process *process = &res.processes[index];
        if (process->lease) res_lease_kill(process->lease, true);
    }
    for (size_t index = 0; index < res.retired_count; index++) {
        if (res.retired[index].lease) res_lease_kill(res.retired[index].lease, true);
    }
    for (size_t index = 0; index < res.lease_count; index++) res_lease_kill(res.leases[index], true);
    for (int attempt = 0; attempt < 200; attempt++) {
        bool pending = false;
        for (size_t index = 0; index < res.process_count; index++) {
            struct res_process *process = &res.processes[index];
            if (process->lease && process->lease->job && !res_lease_reap(process->lease, true))
                pending = true;
        }
        for (size_t index = 0; index < res.retired_count; index++) {
            if (res.retired[index].lease && res.retired[index].lease->job &&
                    !res_lease_reap(res.retired[index].lease, true))
                pending = true;
        }
        for (size_t index = 0; index < res.lease_count; index++) {
            struct res_lease *lease = res.leases[index];
            if (lease->job && !res_lease_reap(lease, true)) pending = true;
        }
        if (!pending) break;
        usleep(10000);
    }
    for (size_t index = 0; index < res.process_count; index++) {
        if (res.processes[index].lease) {
            res_lease_drop(res.processes[index].lease);
            res.processes[index].lease = NULL;
        }
        process_free(&res.processes[index]);
    }
    free(res.processes);
    res.processes = NULL;
    res.process_count = 0;
    for (size_t index = 0; index < res.retired_count; index++) {
        if (res.retired[index].lease) {
            res_lease_drop(res.retired[index].lease);
            res.retired[index].lease = NULL;
        }
        process_free(&res.retired[index]);
    }
    free(res.retired);
    res.retired = NULL;
    res.retired_count = 0;
    while (res.lease_count) res_lease_drop(res.leases[0]);
    for (size_t index = 0; index < res.history_count; index++) {
        free(res.history[index].owner);
        free(res.history[index].name);
    }
    free(res.history);
    res.history = NULL;
    res.history_count = 0;
}

static bool plist_argv(lisp_value plist, char ***argv, size_t *count, char *error,
        size_t size) {
    lisp_value value;
    if (!res_plist_get(plist, "ARGV", &value) || !lisp_consp(value)) {
        res_error_text(error, size, "A launch needs a non-empty :ARGV list.");
        return false;
    }
    size_t length = 0;
    for (lisp_value cursor = value; lisp_consp(cursor); cursor = lisp_cdr(cursor)) length++;
    if (!length || length > 128) {
        res_error_text(error, size, "Process argv must contain 1 through 128 strings.");
        return false;
    }
    char **entries = calloc(length + 1, sizeof(char *));
    if (!entries) { res_error_text(error, size, "Cannot allocate resources."); return false; }
    size_t index = 0;
    for (lisp_value cursor = value; lisp_consp(cursor); cursor = lisp_cdr(cursor)) {
        char *entry = res_plist_string(lisp_car(cursor));
        if (!entry) { res_error_text(error, size, "Process argv entries must be strings."); return false; }
        entries[index++] = entry;
    }
    if (!*entries[0]) { res_error_text(error, size, "Empty process executable."); return false; }
    *argv = entries;
    *count = index;
    return true;
}

lisp_value res_spawn(lisp_value plist, bool launch) {
    char error[512];
    error[0] = 0;
    char **argv = NULL;
    size_t argc = 0;
    char *command = NULL;
    lisp_value shell;
    if (res_plist_get(plist, "ARGV", &shell) && lisp_stringp(shell)) {
        command = res_plist_string(shell);
        if (!command || !*command || strlen(command) > 65536) {
            free(command);
            lisp_error("Invalid process command.");
        }
    } else if (!plist_argv(plist, &argv, &argc, error, sizeof(error))) {
        lisp_error("%s", error);
    }
    struct res_process process;
    memset(&process, 0, sizeof(process));
    process.command = command;
    process.argv = argv;
    process.argc = argc;
    process.kind = launch ? PROCESS_LAUNCH : PROCESS_SPAWN;
    process.session = true;
    process.focus = true;
    process.cwd = res_plist_string_field(plist, "CWD");
    process.directory = res_plist_string_field(plist, "DIRECTORY");
    process.env = res_plist_environment(plist, &process.env_count, error, sizeof(error));
    if (!process.env && error[0]) lisp_error("%s", error);
    lisp_value focus;
    if (res_plist_get(plist, "FOCUS", &focus)) process.focus = !lisp_nilp(focus);
    if (!launch && !process.focus) process.focus = false;
    if (process_reservations() + 1 > 128) {
        process_free(&process);
        lisp_error("More than 128 live or reserved managed processes.");
    }
    lisp_value input;
    process.input = res_plist_get(plist, "INPUT", &input) && !lisp_nilp(input);
    process_start(&process);
    uint64_t lease_id = process.lease ? process.lease->id : 0;
    if (process.status == PROCESS_SPAWN_ERROR) {
        char message[512];
        snprintf(message, sizeof(message), "%s",
                 process.error ? process.error : "Cannot start process.");
        process_free(&process);
        lisp_error("%s", message);
    }
    process_free(&process);
    if (launch) return lisp_nil();
    return lisp_uint(lease_id);
}

int64_t res_process_parent(int64_t pid) {
    char path[64];
    snprintf(path, sizeof(path), "/proc/%lld/stat", (long long)pid);
    FILE *file = fopen(path, "r");
    if (!file) return 0;
    char line[4096];
    if (!fgets(line, sizeof(line), file)) { fclose(file); return 0; }
    fclose(file);
    char *end = strrchr(line, ')');
    if (!end) return 0;
    char *cursor = end + 1;
    while (*cursor == ' ') cursor++;
    while (*cursor && *cursor != ' ') cursor++;
    while (*cursor == ' ') cursor++;
    if (!*cursor) return 0;
    return strtoll(cursor, NULL, 10);
}
