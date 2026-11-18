#ifndef TOMOE_TEST_HARNESS_H
#define TOMOE_TEST_HARNESS_H

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern char **environ;

#define HARNESS_ENV_MAX 64
#define HARNESS_ARG_MAX 64

struct harness_env {
    char *items[HARNESS_ENV_MAX];
    int count;
};

static inline void harness_fail(const char *message) {
    fprintf(stderr, "%s\n", message);
    fflush(stderr);
    exit(1);
}

static inline void harness_check(int condition, const char *message) {
    if (!condition) harness_fail(message);
}

static inline void harness_say(const char *message) {
    printf("%s\n", message);
    fflush(stdout);
}

static inline double harness_now(void) {
    struct timespec time;
    clock_gettime(CLOCK_MONOTONIC, &time);
    return (double)time.tv_sec + (double)time.tv_nsec / 1e9;
}

static inline void harness_sleep(double seconds) {
    struct timespec pause;
    pause.tv_sec = (time_t)seconds;
    pause.tv_nsec = (long)((seconds - (double)pause.tv_sec) * 1e9);
    nanosleep(&pause, NULL);
}

static inline void env_reset(struct harness_env *env) {
    for (int index = 0; index < env->count; index++) free(env->items[index]);
    env->count = 0;
    env->items[0] = NULL;
}

static inline void env_put(struct harness_env *env, const char *key, const char *value) {
    char text[8192];
    snprintf(text, sizeof text, "%s=%s", key, value);
    size_t length = strlen(key);
    for (int index = 0; index < env->count; index++)
        if (strncmp(env->items[index], key, length) == 0 && env->items[index][length] == '=') {
            char *copy = strdup(text);
            if (!copy) harness_fail("out of memory");
            free(env->items[index]);
            env->items[index] = copy;
            return;
        }
    if (env->count + 1 >= HARNESS_ENV_MAX) harness_fail("too many environment variables");
    char *copy = strdup(text);
    if (!copy) harness_fail("out of memory");
    env->items[env->count++] = copy;
    env->items[env->count] = NULL;
}

static inline void env_keep(struct harness_env *env, const char *key) {
    const char *value = getenv(key);
    if (value) env_put(env, key, value);
}

static inline void env_append(struct harness_env *env, const char *key, const char *extra) {
    const char *value = getenv(key);
    char text[8192];
    if (value && *value)
        snprintf(text, sizeof text, "%s:%s", extra, value);
    else
        snprintf(text, sizeof text, "%s", extra);
    env_put(env, key, text);
}

static inline char **env_items(struct harness_env *env) {
    return env->items;
}

static inline void harness_concat(char *out, size_t size, const char *left, const char *right) {
    size_t first = strlen(left), second = strlen(right);
    if (first + second + 1 > size) harness_fail("a path is too long");
    memcpy(out, left, first);
    memcpy(out + first, right, second + 1);
}

static inline void harness_join(char *out, size_t size, const char *left, const char *right) {
    size_t first = strlen(left), second = strlen(right);
    if (first + second + 2 > size) harness_fail("a path is too long");
    memcpy(out, left, first);
    out[first] = '/';
    memcpy(out + first + 1, right, second + 1);
}

static inline void harness_mkparents(const char *path, mode_t mode) {
    char copy[4096];
    snprintf(copy, sizeof copy, "%s", path);
    for (char *at = copy + 1; *at; at++) {
        if (*at != '/') continue;
        *at = '\0';
        mkdir(copy, mode);
        *at = '/';
    }
    mkdir(copy, mode);
}

static inline const char *harness_tmpdir(char *buffer, size_t size) {
    const char *root = getenv("TMPDIR");
    if (!root || !*root) root = "/tmp";
    snprintf(buffer, size, "%s/tomoe-test-XXXXXX", root);
    if (!mkdtemp(buffer)) harness_fail("cannot create a temporary directory");
    return buffer;
}

static inline pid_t harness_spawn(char *const argv[], char **env, int *input, int *output,
        const char *log_path, int output_is_log) {
    int in_pipe[2] = { -1, -1 }, out_pipe[2] = { -1, -1 };
    if (input && pipe(in_pipe) != 0) harness_fail("cannot make a pipe");
    if (output && !output_is_log && pipe(out_pipe) != 0) harness_fail("cannot make a pipe");
    pid_t child = fork();
    if (child < 0) harness_fail("cannot fork");
    if (child == 0) {
        if (input) {
            dup2(in_pipe[0], STDIN_FILENO);
            close(in_pipe[0]);
            close(in_pipe[1]);
        }
        if (output && !output_is_log) {
            dup2(out_pipe[1], STDOUT_FILENO);
            close(out_pipe[0]);
            close(out_pipe[1]);
        }
        if (log_path) {
            int log = open(log_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (log >= 0) {
                dup2(log, STDOUT_FILENO);
                dup2(log, STDERR_FILENO);
                close(log);
            }
        }
        execvpe(argv[0], argv, env);
        fprintf(stderr, "harness: cannot run %s: %s\n", argv[0], strerror(errno));
        _exit(127);
    }
    if (input) {
        close(in_pipe[0]);
        *input = in_pipe[1];
    }
    if (output && !output_is_log) {
        close(out_pipe[1]);
        *output = out_pipe[0];
    }
    return child;
}

struct harness_child {
    pid_t pid;
    int input, output;
    int ended;
};

static inline int harness_read_line(struct harness_child *child, char *line, size_t size) {
    size_t used = 0;
    int saw = 0;
    for (;;) {
        char byte;
        ssize_t got = read(child->output, &byte, 1);
        if (got <= 0) {
            child->ended = 1;
            break;
        }
        saw = 1;
        if (byte == '\n') break;
        if (used + 1 < size) line[used++] = byte;
    }
    line[used] = '\0';
    while (used && (line[used - 1] == '\r' || line[used - 1] == ' ' || line[used - 1] == '\t'))
        line[--used] = '\0';
    size_t start = 0;
    while (line[start] == ' ' || line[start] == '\t') start++;
    if (start) memmove(line, line + start, used - start + 1);
    return saw;
}

static inline void harness_write_line(struct harness_child *child, const char *text) {
    char line[8192];
    int length = snprintf(line, sizeof line, "%s\n", text);
    size_t sent = 0;
    while (sent < (size_t)length) {
        ssize_t used = write(child->input, line + sent, (size_t)length - sent);
        if (used < 0) {
            if (errno == EINTR) continue;
            harness_fail("cannot write to a child");
        }
        sent += (size_t)used;
    }
}

static inline int harness_child_exited(struct harness_child *child) {
    int status = 0;
    pid_t done = waitpid(child->pid, &status, WNOHANG);
    if (done != child->pid) return -1;
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    return 128 + (WIFSIGNALED(status) ? WTERMSIG(status) : 0);
}

static inline int harness_wait_status(pid_t pid, double timeout) {
    double deadline = harness_now() + timeout;
    for (;;) {
        int status = 0;
        pid_t done = waitpid(pid, &status, WNOHANG);
        if (done == pid) {
            if (WIFEXITED(status)) return WEXITSTATUS(status);
            return 128 + (WIFSIGNALED(status) ? WTERMSIG(status) : 0);
        }
        if (harness_now() > deadline) return -1;
        harness_sleep(0.01);
    }
}

static inline void harness_terminate(pid_t pid) {
    if (pid <= 0) return;
    int status = 0;
    if (waitpid(pid, &status, WNOHANG) == pid) return;
    kill(pid, SIGTERM);
    if (harness_wait_status(pid, 10) < 0) {
        kill(pid, SIGKILL);
        harness_wait_status(pid, 10);
    }
}

struct harness_run {
    int status;
    char *output;
};

static inline struct harness_run harness_run(char *const argv[], char **env) {
    struct harness_run run = { -1, NULL };
    int pipe_out[2];
    if (pipe(pipe_out) != 0) harness_fail("cannot make a pipe");
    pid_t child = fork();
    if (child < 0) harness_fail("cannot fork");
    if (child == 0) {
        dup2(pipe_out[1], STDOUT_FILENO);
        close(pipe_out[0]);
        close(pipe_out[1]);
        execvpe(argv[0], argv, env);
        fprintf(stderr, "harness: cannot run %s: %s\n", argv[0], strerror(errno));
        _exit(127);
    }
    close(pipe_out[1]);
    size_t capacity = 65536, used = 0;
    char *text = malloc(capacity);
    if (!text) harness_fail("out of memory");
    for (;;) {
        if (used + 4096 > capacity) {
            capacity *= 2;
            char *grown = realloc(text, capacity);
            if (!grown) harness_fail("out of memory");
            text = grown;
        }
        ssize_t got = read(pipe_out[0], text + used, capacity - used - 1);
        if (got < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (got == 0) break;
        used += (size_t)got;
    }
    close(pipe_out[0]);
    text[used] = '\0';
    run.output = text;
    run.status = harness_wait_status(child, 120);
    return run;
}

static inline void harness_run_ok(char *const argv[], char **env, char **output, const char *what) {
    struct harness_run run = harness_run(argv, env);
    if (run.status != 0) {
        fprintf(stderr, "%s failed with %d\n%s\n", what, run.status,
                run.output ? run.output : "");
        harness_fail(what);
    }
    if (output) *output = run.output;
    else free(run.output);
}

static inline const char *json_key(const char *text, const char *key) {
    char pattern[256];
    snprintf(pattern, sizeof pattern, "\"%s\"", key);
    const char *at = strstr(text, pattern);
    if (!at) return NULL;
    at += strlen(pattern);
    while (*at == ' ' || *at == '\t' || *at == '\n' || *at == '\r') at++;
    if (*at != ':') return NULL;
    at++;
    while (*at == ' ' || *at == '\t' || *at == '\n' || *at == '\r') at++;
    return at;
}

static inline int json_string(const char *text, const char *key, char *out, size_t size) {
    const char *at = json_key(text, key);
    if (!at || *at != '"') return 0;
    at++;
    size_t used = 0;
    while (*at && *at != '"') {
        if (*at == '\\' && at[1]) {
            at++;
            char value = *at;
            if (value == 'n') value = '\n';
            else if (value == 't') value = '\t';
            else if (value == 'r') value = '\r';
            else if (value == 'u') {
                unsigned code = 0;
                for (int index = 0; index < 4 && at[1]; index++) {
                    at++;
                    char digit = *at;
                    code <<= 4;
                    if (digit >= '0' && digit <= '9') code |= (unsigned)(digit - '0');
                    else if (digit >= 'a' && digit <= 'f') code |= (unsigned)(digit - 'a' + 10);
                    else if (digit >= 'A' && digit <= 'F') code |= (unsigned)(digit - 'A' + 10);
                }
                if (used + 1 < size) out[used++] = (char)(code & 0xff);
                at++;
                continue;
            }
            if (used + 1 < size) out[used++] = value;
            at++;
            continue;
        }
        if (used + 1 < size) out[used++] = *at;
        at++;
    }
    out[used] = '\0';
    return 1;
}

static inline int json_number(const char *text, const char *key, double *value) {
    const char *at = json_key(text, key);
    if (!at) return 0;
    char *stop = NULL;
    double parsed = strtod(at, &stop);
    if (stop == at) return 0;
    *value = parsed;
    return 1;
}

static inline int json_integer(const char *text, const char *key, long long *value) {
    const char *at = json_key(text, key);
    if (!at) return 0;
    char *stop = NULL;
    long long parsed = strtoll(at, &stop, 10);
    if (stop == at) return 0;
    *value = parsed;
    return 1;
}

static inline int json_boolean(const char *text, const char *key, int *value) {
    const char *at = json_key(text, key);
    if (!at) return 0;
    if (strncmp(at, "true", 4) == 0) {
        *value = 1;
        return 1;
    }
    if (strncmp(at, "false", 5) == 0) {
        *value = 0;
        return 1;
    }
    return 0;
}

static inline int json_strings_joined(const char *text, const char *key, char *out, size_t size) {
    const char *at = json_key(text, key);
    if (!at) return 0;
    size_t used = 0;
    out[0] = '\0';
    if (*at == '"') {
        char value[65536];
        if (!json_string(text, key, value, sizeof value)) return 0;
        snprintf(out, size, "%s", value);
        return 1;
    }
    if (*at != '[') return 0;
    at++;
    for (;;) {
        while (*at == ' ' || *at == '\t' || *at == '\n' || *at == '\r' || *at == ',') at++;
        if (*at == ']' || !*at) break;
        if (*at != '"') return 0;
        const char *start = ++at;
        while (*at && *at != '"') {
            if (*at == '\\' && at[1]) at++;
            at++;
        }
        size_t length = (size_t)(at - start);
        if (used + length < size) {
            memcpy(out + used, start, length);
            used += length;
        }
        if (*at == '"') at++;
    }
    out[used] = '\0';
    return 1;
}


static inline size_t base64_decode(const char *text, unsigned char *out, size_t size) {
    static const char table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t used = 0;
    unsigned value = 0;
    int bits = 0;
    for (const char *at = text; *at; at++) {
        const char *found = strchr(table, *at);
        if (!found) {
            if (*at == '=') break;
            continue;
        }
        value = (value << 6) | (unsigned)(found - table);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (used < size) out[used++] = (unsigned char)((value >> bits) & 0xff);
        }
    }
    return used;
}

static inline int harness_file_exists(const char *path) {
    struct stat status;
    return stat(path, &status) == 0;
}

static inline void harness_dump_file(const char *path) {
    FILE *file = fopen(path, "r");
    if (!file) return;
    char line[4096];
    while (fgets(line, sizeof line, file)) fputs(line, stderr);
    fclose(file);
    fflush(stderr);
}

static inline void harness_write_file(const char *path, const char *text) {
    FILE *file = fopen(path, "w");
    if (!file) harness_fail("cannot write a file");
    fputs(text, file);
    fclose(file);
}

#endif
