#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <time.h>
#include "resources-internal.h"

struct res_state res;

static void res_schedule(uint64_t when);

static void *res_alloc(size_t size) {
    void *pointer = calloc(1, size ? size : 1);
    return pointer;
}

char *res_dup(const char *text) {
    if (!text) return NULL;
    size_t length = strlen(text);
    char *copy = malloc(length + 1);
    if (!copy) return NULL;
    memcpy(copy, text, length + 1);
    return copy;
}

char *res_dup_span(const char *text, size_t length) {
    char *copy = malloc(length + 1);
    if (!copy) return NULL;
    memcpy(copy, text, length);
    copy[length] = 0;
    return copy;
}

void rb_append(struct res_buffer *buffer, const char *text, size_t length) {
    if (buffer->failed) return;
    if (buffer->length + length + 1 > buffer->capacity) {
        size_t capacity = buffer->capacity ? buffer->capacity : 128;
        while (capacity < buffer->length + length + 1) capacity *= 2;
        char *data = realloc(buffer->data, capacity);
        if (!data) { buffer->failed = true; return; }
        buffer->data = data;
        buffer->capacity = capacity;
    }
    memcpy(buffer->data + buffer->length, text, length);
    buffer->length += length;
    buffer->data[buffer->length] = 0;
}

void rb_printf(struct res_buffer *buffer, const char *format, ...) {
    char stack[256];
    va_list arguments;
    va_start(arguments, format);
    int count = vsnprintf(stack, sizeof(stack), format, arguments);
    va_end(arguments);
    if (count < 0) { buffer->failed = true; return; }
    if ((size_t)count < sizeof(stack)) {
        rb_append(buffer, stack, (size_t)count);
        return;
    }
    char *text = malloc((size_t)count + 1);
    if (!text) { buffer->failed = true; return; }
    va_start(arguments, format);
    vsnprintf(text, (size_t)count + 1, format, arguments);
    va_end(arguments);
    rb_append(buffer, text, (size_t)count);
    free(text);
}

void rb_quoted(struct res_buffer *buffer, const char *text, size_t length) {
    rb_append(buffer, "\"", 1);
    for (size_t index = 0; index < length; index++) {
        char character = text[index];
        if (character == '"' || character == '\\') {
            char escaped[2] = { '\\', character };
            rb_append(buffer, escaped, 2);
        } else {
            rb_append(buffer, &character, 1);
        }
    }
    rb_append(buffer, "\"", 1);
}

void rb_quoted_cstring(struct res_buffer *buffer, const char *text) {
    rb_quoted(buffer, text ? text : "", text ? strlen(text) : 0);
}

char *rb_take(struct res_buffer *buffer) {
    if (buffer->failed) { rb_free(buffer); return NULL; }
    if (!buffer->data) {
        char *empty = res_dup("");
        return empty;
    }
    return buffer->data;
}

void rb_free(struct res_buffer *buffer) {
    free(buffer->data);
    buffer->data = NULL;
    buffer->length = buffer->capacity = 0;
    buffer->failed = false;
}

static bool res_keyword_plain(const char *name) {
    if (!*name) return false;
    bool letter = false;
    for (const char *pointer = name; *pointer; pointer++) {
        unsigned char character = (unsigned char)*pointer;
        if (character >= 'A' && character <= 'Z') { letter = true; continue; }
        if (character >= '0' && character <= '9') continue;
        if (strchr("+-*/.=_", character)) continue;
        return false;
    }
    return letter && !(name[0] >= '0' && name[0] <= '9');
}

void res_append_keyword(struct res_buffer *buffer, const char *name) {
    rb_append(buffer, ":", 1);
    if (res_keyword_plain(name)) {
        rb_append(buffer, name, strlen(name));
        return;
    }
    rb_append(buffer, "|", 1);
    for (const char *pointer = name; *pointer; pointer++) {
        if (*pointer == '|' || *pointer == '\\') rb_append(buffer, "\\", 1);
        rb_append(buffer, pointer, 1);
    }
    rb_append(buffer, "|", 1);
}

void res_push_event(struct tomoe *server, enum tomoe_event_type type, uint16_t flags,
        const char *text, size_t length) {
    struct tomoe_event *event = policy_event_push(server, type);
    if (!event) return;
    event->flags = flags;
    event->str[0] = policy_event_string(server, text, length);
    event->str[1] = EVENT_NO_STRING;
    event->str[2] = EVENT_NO_STRING;
    event->str[3] = EVENT_NO_STRING;
}

uint64_t res_now(void) {
    return loop_now_ms();
}

lisp_value resources_read_text(const char *text, size_t length) {
    lisp_value value = lisp_nil();
    char error[256];
    if (!text) return value;
    if (lisp_read_data(text, length, &value, error, sizeof(error))) return lisp_nil();
    return value;
}

static lisp_value res_event_value(struct tomoe *server, const struct tomoe_event *event) {
    size_t length = 0;
    if (event->str[0] == EVENT_NO_STRING) return lisp_nil();
    const char *text = policy_event_string_at(server, event->str[0], &length);
    if (!text) return lisp_nil();
    return resources_read_text(text, length);
}

static lisp_value converter_timer(struct tomoe *server, const struct tomoe_event *event) {
    return res_event_value(server, event);
}

static lisp_value converter_watch(struct tomoe *server, const struct tomoe_event *event) {
    return res_event_value(server, event);
}

static lisp_value converter_exec(struct tomoe *server, const struct tomoe_event *event) {
    return res_event_value(server, event);
}

static lisp_value converter_process(struct tomoe *server, const struct tomoe_event *event) {
    return res_event_value(server, event);
}

static lisp_value converter_services(struct tomoe *server, const struct tomoe_event *event) {
    lisp_value value = res_event_value(server, event);
    if (lisp_nilp(value)) return value;
    return lisp_cons(lisp_keyword("TYPE"),
        lisp_cons(lisp_keyword("services"),
        lisp_cons(lisp_keyword("SERVICES"),
        lisp_cons(value, lisp_nil()))));
}

static lisp_value converter_system(struct tomoe *server, const struct tomoe_event *event) {
    return res_event_value(server, event);
}

static lisp_value converter_source(struct tomoe *server, const struct tomoe_event *event) {
    return res_event_value(server, event);
}

uint64_t res_refresh_ms(void) {
    if (!res.server) return 16;
    double refresh = loop_refresh_ms(res.server);
    if (!(refresh >= 1)) return 16;
    return (uint64_t)refresh;
}

static uint64_t res_clock_boundary(int64_t step) {
    struct timespec now;
    clock_gettime(CLOCK_REALTIME, &now);
    int64_t seconds = (int64_t)now.tv_sec;
    int64_t milliseconds = (int64_t)(now.tv_nsec / 1000000);
    int64_t left = step - (seconds % step);
    int64_t delay = left * 1000 - milliseconds;
    return delay < 1 ? 1 : (uint64_t)delay;
}

static int64_t res_timer_period(const struct res_timer *timer) {
    switch (timer->clock) {
    case TIMER_CLOCK_REFRESH: return (int64_t)res_refresh_ms();
    case TIMER_CLOCK_SECOND: return 1000;
    case TIMER_CLOCK_MINUTE: return 60000;
    default: return timer->declared;
    }
}

static uint64_t timer_first(const struct res_timer *timer) {
    if (timer->clock == TIMER_CLOCK_SECOND) return res_clock_boundary(1);
    if (timer->clock == TIMER_CLOCK_MINUTE) return res_clock_boundary(60);
    return (uint64_t)(timer->declared < 0 ? 0 : timer->declared);
}

static uint64_t timer_next(const struct res_timer *timer) {
    if (timer->clock == TIMER_CLOCK_SECOND) return res_clock_boundary(1);
    if (timer->clock == TIMER_CLOCK_MINUTE) return res_clock_boundary(60);
    int64_t period = res_timer_period(timer);
    return (uint64_t)(period < 0 ? 0 : period);
}

void res_timers_service(uint64_t now, uint64_t *next) {
    uint64_t start = now;
    size_t delivered = 0;
    for (;;) {
        struct res_timer *due = NULL;
        for (size_t index = 0; index < res.timer_count; index++) {
            struct res_timer *timer = &res.timers[index];
            if (timer->fired || timer->deadline > now) continue;
            if (!due || timer->deadline < due->deadline) due = timer;
        }
        if (!due) break;
        if (delivered >= 64) { if (due->deadline < *next) *next = due->deadline; break; }
        if (delivered && res_now() - start >= 8) {
            if (due->deadline < *next) *next = due->deadline;
            break;
        }
        delivered++;
        due->fired = true;
        struct res_buffer text = {0};
        rb_append(&text, "(:TYPE :TIMER :NAME ", 20);
        res_append_keyword(&text, due->name);
        rb_append(&text, " :OWNER ", 8);
        rb_quoted_cstring(&text, due->owner);
        rb_append(&text, ")", 1);
        char *message = rb_take(&text);
        if (message) {
            res_push_event(res.server, EV_TIMER, EVF_FACT, message, strlen(message));
            free(message);
        }
        if (due->repeat) {
            due->deadline = res_now() + timer_next(due);
            due->fired = false;
        }
    }
    for (size_t index = 0; index < res.timer_count; index++) {
        struct res_timer *timer = &res.timers[index];
        if (!timer->fired && timer->deadline < *next) *next = timer->deadline;
    }
}

void res_timers_free(void) {
    for (size_t index = 0; index < res.timer_count; index++) {
        free(res.timers[index].owner);
        free(res.timers[index].name);
    }
    free(res.timers);
    res.timers = NULL;
    res.timer_count = 0;
}

static void res_engine(void *data);

static void res_engine_callback(void *data) {
    res_engine(data);
}

static void res_schedule(uint64_t when) {
    if (!res.engine) return;
    if (res.engine_armed && when >= res.engine_due) return;
    uint64_t now = res_now();
    res.engine_due = when;
    res.engine_armed = true;
    loop_timer_arm(res.engine, when <= now ? 1 : when - now);
}

void res_engine_kick(void) {
    if (!res.started) return;
    res_schedule(res_now());
}

static void res_engine(void *data) {
    res.engine_armed = false;
    uint64_t now = res_now();
    uint64_t next = now + 86400000ULL;
    res_timers_service(now, &next);
    res_watches_service(now, &next);
    res_execs_service(now, &next);
    res_processes_service(now, &next);
    res_services_service(now, &next);
    res_sources_service(now, &next);
    res_schedule(next);
}

int resources_start(struct tomoe *server) {
    res.server = server;
    if (res.started) return 0;
    res.started = true;
    res.commit_sequence = 1;
    res.engine = loop_timer(server, res_engine_callback, NULL);
    res_services_start();
    uint64_t now = res_now();
    res.system_due = now;
    res_schedule(now);
    return 0;
}

void resources_stop(struct tomoe *server) {
    if (!res.started) return;
    res.started = false;
    if (res.engine) { loop_remove(res.engine); res.engine = NULL; }
    res_timers_free();
    res_watches_free();
    res_execs_free();
    res_processes_stop();
    res_sources_free();
    res_services_stop();
    (void)server;
}

static bool res_get(lisp_value plist, const char *name, lisp_value *out) {
    lisp_value cursor = plist;
    while (lisp_consp(cursor)) {
        lisp_value key = lisp_car(cursor);
        lisp_value tail = lisp_cdr(cursor);
        if (!lisp_consp(tail)) return false;
        if (lisp_keywordp(key) && lisp_eq_keyword(key, name)) {
            *out = lisp_car(tail);
            return true;
        }
        cursor = lisp_cdr(tail);
    }
    return false;
}

static bool res_get_keyword(lisp_value plist, const char *name, const char **out) {
    lisp_value value;
    if (!res_get(plist, name, &value)) return false;
    if (!lisp_keywordp(value)) return false;
    const char *symbol = lisp_symbol_name(value);
    if (!symbol) return false;
    *out = symbol;
    return true;
}

static bool res_get_string(lisp_value plist, const char *name, char **out) {
    lisp_value value;
    *out = NULL;
    if (!res_get(plist, name, &value)) return true;
    if (lisp_nilp(value)) return true;
    if (!lisp_stringp(value)) return false;
    size_t length = 0;
    const char *text = lisp_get_string(value, &length);
    if (!text) return false;
    *out = res_dup_span(text, length);
    return *out != NULL;
}

static bool res_get_int(lisp_value plist, const char *name, int64_t *out) {
    lisp_value value;
    if (!res_get(plist, name, &value)) return false;
    if (!lisp_intp(value)) return false;
    *out = lisp_get_int(value);
    return true;
}

static char *res_keyword_text(lisp_value value) {
    const char *symbol = lisp_symbol_name(value);
    if (!symbol) return NULL;
    return res_dup(symbol);
}

static void res_error(char *error, size_t size, const char *format, ...) {
    va_list arguments;
    va_start(arguments, format);
    vsnprintf(error, size, format, arguments);
    va_end(arguments);
}

static void want_watch_free(struct want_watch *want) {
    free(want->owner); free(want->name); free(want->path); free(want->directory);
}

static void want_timer_free(struct want_timer *want) {
    free(want->owner); free(want->name);
}

static void want_exec_free(struct want_exec *want) {
    free(want->owner); free(want->name); free(want->command);
}

static void want_process_free(struct want_process *want) {
    free(want->owner); free(want->name); free(want->command); free(want->cwd);
    free(want->directory);
    for (size_t index = 0; index < want->argc; index++) free(want->argv[index]);
    free(want->argv);
    for (size_t index = 0; index < want->env_count; index++) free(want->env[index]);
    free(want->env);
}

static bool res_parse_timer(lisp_value entry, struct want_timer *want, char *error,
        size_t size) {
    lisp_value owner, arguments;
    if (!res_get(entry, "OWNER", &owner) || !lisp_stringp(owner)) {
        res_error(error, size, "Owned timer without an owner name.");
        return false;
    }
    size_t owner_length = 0;
    const char *owner_text = lisp_get_string(owner, &owner_length);
    want->owner = res_dup_span(owner_text, owner_length);
    if (!res_get(entry, "ARGUMENTS", &arguments) || !lisp_consp(arguments)) {
        res_error(error, size, "Owned timer without arguments.");
        return false;
    }
    lisp_value name = lisp_car(arguments);
    lisp_value tail = lisp_cdr(arguments);
    if (!lisp_keywordp(name) || !lisp_consp(tail)) {
        res_error(error, size, "Invalid owned timer.");
        return false;
    }
    want->name = res_keyword_text(name);
    lisp_value milliseconds = lisp_car(tail);
    tail = lisp_cdr(tail);
    if (!lisp_consp(tail)) {
        res_error(error, size, "Invalid owned timer.");
        return false;
    }
    want->repeat = !lisp_nilp(lisp_car(tail));
    if (lisp_intp(milliseconds)) {
        int64_t value = lisp_get_int(milliseconds);
        if (value < 0 || value > 2147483647) {
            res_error(error, size, "Invalid timer period.");
            return false;
        }
        want->declared = value;
        want->clock = TIMER_CLOCK_NONE;
    } else if (lisp_keywordp(milliseconds)) {
        const char *symbol = lisp_symbol_name(milliseconds);
        if (symbol && !strcmp(symbol, "REFRESH")) {
            want->clock = TIMER_CLOCK_REFRESH;
            want->declared = (int64_t)res_refresh_ms();
        } else if (symbol && !strcmp(symbol, "SECOND")) {
            want->clock = TIMER_CLOCK_SECOND;
            want->declared = 1000;
        } else if (symbol && !strcmp(symbol, "MINUTE")) {
            want->clock = TIMER_CLOCK_MINUTE;
            want->declared = 60000;
        } else {
            res_error(error, size, "Invalid timer period.");
            return false;
        }
    } else {
        res_error(error, size, "Invalid timer period.");
        return false;
    }
    return true;
}

static bool res_parse_watch(lisp_value entry, struct want_watch *want, char *error,
        size_t size) {
    lisp_value owner, arguments;
    int64_t source = 0;
    if (res_get_int(entry, "SOURCE-ID", &source) && source > 0) want->source_id = (uint64_t)source;
    if (!res_get_string(entry, "DIRECTORY", &want->directory)) {
        res_error(error, size, "Invalid file watch directory.");
        return false;
    }
    if (!res_get(entry, "OWNER", &owner) || !lisp_stringp(owner)) {
        res_error(error, size, "File watch without an owner name.");
        return false;
    }
    size_t owner_length = 0;
    const char *owner_text = lisp_get_string(owner, &owner_length);
    want->owner = res_dup_span(owner_text, owner_length);
    if (!res_get(entry, "ARGUMENTS", &arguments) || !lisp_consp(arguments)) {
        res_error(error, size, "File watch without arguments.");
        return false;
    }
    lisp_value name = lisp_car(arguments);
    lisp_value tail = lisp_cdr(arguments);
    if (!lisp_keywordp(name) || !lisp_consp(tail)) {
        res_error(error, size, "Invalid file watch.");
        return false;
    }
    want->name = res_keyword_text(name);
    lisp_value path = lisp_car(tail);
    tail = lisp_cdr(tail);
    if (!lisp_stringp(path) || !lisp_consp(tail)) {
        res_error(error, size, "Invalid file watch.");
        return false;
    }
    size_t path_length = 0;
    const char *path_text = lisp_get_string(path, &path_length);
    if (!path_length || path_length > 65536 || memchr(path_text, 0, path_length)) {
        res_error(error, size, "Invalid file watch path.");
        return false;
    }
    want->path = res_dup_span(path_text, path_length);
    lisp_value limit = lisp_car(tail);
    if (!lisp_intp(limit)) {
        res_error(error, size, "Invalid file watch content limit.");
        return false;
    }
    int64_t value = lisp_get_int(limit);
    if (value < 1 || value > 65536) {
        res_error(error, size, "Invalid file watch content limit.");
        return false;
    }
    want->limit = (size_t)value;
    return true;
}

static bool res_parse_exec(lisp_value entry, struct want_exec *want, char *error,
        size_t size) {
    lisp_value owner, arguments;
    if (!res_get(entry, "OWNER", &owner) || !lisp_stringp(owner)) {
        res_error(error, size, "Owned execution without an owner name.");
        return false;
    }
    size_t owner_length = 0;
    const char *owner_text = lisp_get_string(owner, &owner_length);
    want->owner = res_dup_span(owner_text, owner_length);
    if (!res_get(entry, "ARGUMENTS", &arguments) || !lisp_consp(arguments)) {
        res_error(error, size, "Owned execution without arguments.");
        return false;
    }
    lisp_value name = lisp_car(arguments);
    lisp_value tail = lisp_cdr(arguments);
    if (!lisp_keywordp(name) || !lisp_consp(tail)) {
        res_error(error, size, "Invalid owned execution.");
        return false;
    }
    want->name = res_keyword_text(name);
    lisp_value command = lisp_car(tail);
    tail = lisp_cdr(tail);
    if (!lisp_stringp(command) || !lisp_consp(tail)) {
        res_error(error, size, "Invalid async command string.");
        return false;
    }
    size_t command_length = 0;
    const char *command_text = lisp_get_string(command, &command_length);
    if (command_length > 65536 || memchr(command_text, 0, command_length)) {
        res_error(error, size, "Invalid async command string.");
        return false;
    }
    want->command = res_dup_span(command_text, command_length);
    lisp_value timeout = lisp_car(tail);
    tail = lisp_cdr(tail);
    if (!lisp_intp(timeout) || !lisp_consp(tail)) {
        res_error(error, size, "Invalid execution timeout.");
        return false;
    }
    int64_t timeout_value = lisp_get_int(timeout);
    if (timeout_value < 1 || timeout_value > 2147483647) {
        res_error(error, size, "Invalid execution timeout.");
        return false;
    }
    want->timeout = timeout_value;
    lisp_value limit = lisp_car(tail);
    if (!lisp_intp(limit)) {
        res_error(error, size, "Invalid execution output limit.");
        return false;
    }
    int64_t limit_value = lisp_get_int(limit);
    if (limit_value < 1 || limit_value > 65536) {
        res_error(error, size, "Invalid execution output limit.");
        return false;
    }
    want->limit = (size_t)limit_value;
    return true;
}

static char *res_string_value(lisp_value value) {
    if (!lisp_stringp(value)) return NULL;
    size_t length = 0;
    const char *text = lisp_get_string(value, &length);
    if (!text || memchr(text, 0, length)) return NULL;
    return res_dup_span(text, length);
}

static bool res_parse_vector(lisp_value list, char ***out, size_t *count, size_t limit,
        bool executable, char *error, size_t size) {
    size_t length = 0;
    for (lisp_value cursor = list; lisp_consp(cursor); cursor = lisp_cdr(cursor)) length++;
    if (!length || length > limit) {
        res_error(error, size, executable ? "Process argv must contain 1 through 128 strings."
                                          : "More than 64 environment overrides.");
        return false;
    }
    char **vector = res_alloc((length + 1) * sizeof(char *));
    if (!vector) { res_error(error, size, "Cannot allocate resources."); return false; }
    size_t index = 0;
    for (lisp_value cursor = list; lisp_consp(cursor); cursor = lisp_cdr(cursor)) {
        char *entry = res_string_value(lisp_car(cursor));
        if (!entry) {
            for (size_t done = 0; done < index; done++) free(vector[done]);
            free(vector);
            res_error(error, size, executable ? "Process argv entries must be strings."
                                              : "Environment entries must be strings.");
            return false;
        }
        vector[index++] = entry;
    }
    if (executable && !*vector[0]) {
        for (size_t done = 0; done < index; done++) free(vector[done]);
        free(vector);
        res_error(error, size, "Empty process executable.");
        return false;
    }
    *out = vector;
    *count = index;
    return true;
}

static bool res_parse_environment(lisp_value list, char ***out, size_t *count, char *error,
        size_t size) {
    size_t length = 0;
    for (lisp_value cursor = list; lisp_consp(cursor); cursor = lisp_cdr(cursor)) length++;
    if (length > 64) {
        res_error(error, size, "More than 64 environment overrides.");
        return false;
    }
    char **vector = res_alloc((length + 1) * sizeof(char *));
    if (!vector) { res_error(error, size, "Cannot allocate resources."); return false; }
    size_t index = 0;
    for (lisp_value cursor = list; lisp_consp(cursor); cursor = lisp_cdr(cursor)) {
        lisp_value pair = lisp_car(cursor);
        if (!lisp_consp(pair)) {
            for (size_t done = 0; done < index; done++) free(vector[done]);
            free(vector);
            res_error(error, size, "Environment entries must be (name . value) pairs.");
            return false;
        }
        char *name = res_string_value(lisp_car(pair));
        char *value = res_string_value(lisp_cdr(pair));
        if (!name || !value || !*name || strchr(name, '=')) {
            free(name); free(value);
            for (size_t done = 0; done < index; done++) free(vector[done]);
            free(vector);
            res_error(error, size, "Invalid environment name.");
            return false;
        }
        for (size_t done = 0; done < index; done++) {
            const char *existing = vector[done];
            size_t name_length = strlen(name);
            if (!strncmp(existing, name, name_length) && existing[name_length] == '=') {
                res_error(error, size, "Invalid or duplicate environment name: %s", name);
                free(name); free(value);
                for (size_t other = 0; other < index; other++) free(vector[other]);
                free(vector);
                return false;
            }
        }
        size_t size_needed = strlen(name) + strlen(value) + 2;
        char *entry = malloc(size_needed);
        if (!entry) {
            free(name); free(value);
            for (size_t done = 0; done < index; done++) free(vector[done]);
            free(vector);
            res_error(error, size, "Cannot allocate resources.");
            return false;
        }
        snprintf(entry, size_needed, "%s=%s", name, value);
        free(name); free(value);
        vector[index++] = entry;
    }
    for (size_t first = 0; first + 1 < index; first++) {
        for (size_t second = first + 1; second < index; second++) {
            if (strcmp(vector[first], vector[second]) > 0) {
                char *swap = vector[first];
                vector[first] = vector[second];
                vector[second] = swap;
            }
        }
    }
    *out = vector;
    *count = index;
    return true;
}

static bool res_parse_process(lisp_value entry, struct want_process *want, char *error,
        size_t size) {
    lisp_value owner, arguments;
    int64_t token = 0;
    if (res_get_int(entry, "TOKEN", &token) && token > 0) want->token = (uint64_t)token;
    if (!res_get_string(entry, "DIRECTORY", &want->directory)) {
        res_error(error, size, "Invalid process directory.");
        return false;
    }
    if (!res_get(entry, "OWNER", &owner) || !lisp_stringp(owner)) {
        res_error(error, size, "Process declaration without an owner name.");
        return false;
    }
    size_t owner_length = 0;
    const char *owner_text = lisp_get_string(owner, &owner_length);
    want->owner = res_dup_span(owner_text, owner_length);
    if (!res_get(entry, "ARGUMENTS", &arguments) || !lisp_consp(arguments)) {
        res_error(error, size, "Process declaration without arguments.");
        return false;
    }
    lisp_value kind = lisp_car(arguments);
    lisp_value tail = lisp_cdr(arguments);
    if (!lisp_keywordp(kind) || !lisp_consp(tail)) {
        res_error(error, size, "Invalid process declaration.");
        return false;
    }
    const char *symbol = lisp_symbol_name(kind);
    if (symbol && !strcmp(symbol, "ONCE")) want->kind = PROCESS_ONCE;
    else if (symbol && !strcmp(symbol, "SERVICE")) want->kind = PROCESS_SERVICE;
    else { res_error(error, size, "Invalid process kind."); return false; }
    lisp_value name = lisp_car(tail);
    tail = lisp_cdr(tail);
    if (!lisp_keywordp(name) || !lisp_consp(tail)) {
        res_error(error, size, "Invalid process declaration.");
        return false;
    }
    want->name = res_keyword_text(name);
    lisp_value command = lisp_car(tail);
    tail = lisp_cdr(tail);
    if (lisp_stringp(command)) {
        want->command = res_string_value(command);
        if (!want->command) { res_error(error, size, "Invalid process command."); return false; }
        if (strlen(want->command) > 65536) {
            res_error(error, size, "Invalid process command.");
            return false;
        }
    } else if (lisp_consp(command)) {
        if (!res_parse_vector(command, &want->argv, &want->argc, 128, true, error, size))
            return false;
    } else {
        res_error(error, size, "Invalid process command.");
        return false;
    }
    lisp_value cwd = lisp_car(tail);
    tail = lisp_cdr(tail);
    if (!lisp_nilp(cwd)) {
        want->cwd = res_string_value(cwd);
        if (!want->cwd || !*want->cwd) {
            res_error(error, size, "Invalid process directory.");
            return false;
        }
    }
    if (!lisp_consp(tail)) { res_error(error, size, "Invalid process declaration."); return false; }
    if (!res_parse_environment(lisp_car(tail), &want->env, &want->env_count, error, size))
        return false;
    tail = lisp_cdr(tail);
    if (!lisp_consp(tail)) { res_error(error, size, "Invalid process declaration."); return false; }
    lisp_value run = lisp_car(tail);
    tail = lisp_cdr(tail);
    if (!lisp_nilp(run)) {
        const char *run_symbol = lisp_keywordp(run) ? lisp_symbol_name(run) : NULL;
        if (run_symbol && !strcmp(run_symbol, "ONCE-PER-SESSION")) want->run = RUN_ONCE_PER_SESSION;
        else if (run_symbol && !strcmp(run_symbol, "ONCE-PER-CONFIG-VERSION"))
            want->run = RUN_ONCE_PER_CONFIG_VERSION;
        else { res_error(error, size, "Invalid process run mode."); return false; }
    }
    if (!lisp_consp(tail)) { res_error(error, size, "Invalid process declaration."); return false; }
    lisp_value restart = lisp_car(tail);
    tail = lisp_cdr(tail);
    const char *restart_symbol = lisp_keywordp(restart) ? lisp_symbol_name(restart) : NULL;
    if (restart_symbol && !strcmp(restart_symbol, "NEVER")) want->restart = RESTART_NEVER;
    else if (restart_symbol && !strcmp(restart_symbol, "ON-FAILURE")) want->restart = RESTART_ON_FAILURE;
    else if (restart_symbol && !strcmp(restart_symbol, "ON-EXIT")) want->restart = RESTART_ON_EXIT;
    else if (lisp_nilp(restart) && want->kind == PROCESS_ONCE) want->restart = RESTART_NEVER;
    else { res_error(error, size, "Invalid process restart policy."); return false; }
    if (!lisp_consp(tail)) { res_error(error, size, "Invalid process declaration."); return false; }
    lisp_value reload = lisp_car(tail);
    const char *reload_symbol = lisp_keywordp(reload) ? lisp_symbol_name(reload) : NULL;
    if (reload_symbol && !strcmp(reload_symbol, "KEEP-IF-UNCHANGED"))
        want->reload = RELOAD_KEEP_IF_UNCHANGED;
    else if (reload_symbol && !strcmp(reload_symbol, "ALWAYS-RESTART"))
        want->reload = RELOAD_ALWAYS_RESTART;
    else if (lisp_nilp(reload) && want->kind == PROCESS_ONCE)
        want->reload = RELOAD_KEEP_IF_UNCHANGED;
    else { res_error(error, size, "Invalid process reload policy."); return false; }
    return true;
}

static bool res_timer_equal(const struct res_timer *timer, const struct want_timer *want) {
    return !strcmp(timer->owner, want->owner) && !strcmp(timer->name, want->name) &&
           timer->clock == want->clock && timer->declared == want->declared &&
           timer->repeat == want->repeat;
}

static void res_drop_timer(struct res_timer *timer) {
    free(timer->owner);
    free(timer->name);
    memset(timer, 0, sizeof(*timer));
}

static void res_apply_timers(struct want_timer *wanted, size_t count) {
    bool *used = res_alloc((res.timer_count ? res.timer_count : 1) * sizeof(bool));
    struct res_timer *timers = res_alloc((count ? count : 1) * sizeof(struct res_timer));
    uint64_t now = res_now();
    if (!used || !timers) {
        free(used);
        free(timers);
        for (size_t index = 0; index < count; index++) {
            free(wanted[index].owner);
            free(wanted[index].name);
        }
        return;
    }
    for (size_t index = 0; index < count; index++) {
        struct want_timer *want = &wanted[index];
        ssize_t match = -1;
        for (size_t old = 0; old < res.timer_count; old++) {
            if (used[old] || !res_timer_equal(&res.timers[old], want)) continue;
            match = (ssize_t)old;
            break;
        }
        if (match >= 0) {
            timers[index] = res.timers[match];
            used[match] = true;
            free(want->owner);
            free(want->name);
            continue;
        }
        timers[index].owner = want->owner;
        timers[index].name = want->name;
        timers[index].clock = want->clock;
        timers[index].declared = want->declared;
        timers[index].repeat = want->repeat;
        timers[index].deadline = now + timer_first(&timers[index]);
        want->owner = want->name = NULL;
    }
    for (size_t old = 0; old < res.timer_count; old++) {
        if (!used[old]) res_drop_timer(&res.timers[old]);
    }
    free(used);
    free(res.timers);
    res.timers = timers;
    res.timer_count = count;
    uint64_t next = res_now() + 86400000ULL;
    for (size_t index = 0; index < count; index++) {
        if (!timers[index].fired && timers[index].deadline < next) next = timers[index].deadline;
    }
    res_schedule(next);
}

static lisp_value prim_resources_commit(int argc, lisp_value *argv) {
    char error[512];
    error[0] = 0;
    if (!res.started && res.server) resources_start(res.server);
    if (!res.started) return lisp_cstring("Resource ownership is not running.");
    struct want_timer *timers = NULL;
    struct want_watch *watches = NULL;
    struct want_exec *execs = NULL;
    struct want_process *processes = NULL;
    size_t timer_count = 0, watch_count = 0, exec_count = 0, process_count = 0;
    size_t length = 0;
    for (lisp_value cursor = argv[0]; lisp_consp(cursor); cursor = lisp_cdr(cursor)) length++;
    timers = res_alloc((length + 1) * sizeof(*timers));
    watches = res_alloc((length + 1) * sizeof(*watches));
    execs = res_alloc((length + 1) * sizeof(*execs));
    processes = res_alloc((length + 1) * sizeof(*processes));
    bool ok = timers && watches && execs && processes;
    for (lisp_value cursor = argv[0]; ok && lisp_consp(cursor); cursor = lisp_cdr(cursor)) {
        lisp_value entry = lisp_car(cursor);
        const char *kind = NULL;
        if (!lisp_consp(entry) || !res_get_keyword(entry, "KIND", &kind)) {
            res_error(error, sizeof(error), "Resource entry without a kind.");
            ok = false;
            break;
        }
        if (!strcmp(kind, "TIMER")) {
            if (++timer_count > 4096) {
                res_error(error, sizeof(error), "More than 4096 owned timers.");
                ok = false;
                break;
            }
            ok = res_parse_timer(entry, &timers[timer_count - 1], error, sizeof(error));
        } else if (!strcmp(kind, "WATCH")) {
            if (++watch_count > 256) {
                res_error(error, sizeof(error), "More than 256 owned file watches.");
                ok = false;
                break;
            }
            ok = res_parse_watch(entry, &watches[watch_count - 1], error, sizeof(error));
            for (size_t index = 0; ok && index + 1 < watch_count; index++) {
                if (!strcmp(watches[index].owner, watches[watch_count - 1].owner) &&
                        !strcmp(watches[index].name, watches[watch_count - 1].name)) {
                    res_error(error, sizeof(error), "Duplicate file watch: %s",
                              watches[watch_count - 1].name);
                    ok = false;
                }
            }
        } else if (!strcmp(kind, "EXEC")) {
            if (++exec_count > 64) {
                res_error(error, sizeof(error), "More than 64 owned executions.");
                ok = false;
                break;
            }
            ok = res_parse_exec(entry, &execs[exec_count - 1], error, sizeof(error));
        } else if (!strcmp(kind, "PROCESS")) {
            if (++process_count > 64) {
                res_error(error, sizeof(error), "More than 64 process declarations.");
                ok = false;
                break;
            }
            ok = res_parse_process(entry, &processes[process_count - 1], error, sizeof(error));
        } else {
            res_error(error, sizeof(error), "Unknown resource kind.");
            ok = false;
        }
    }
    if (!ok) {
        for (size_t index = 0; index < timer_count; index++) want_timer_free(&timers[index]);
        for (size_t index = 0; index < watch_count; index++) want_watch_free(&watches[index]);
        for (size_t index = 0; index < exec_count; index++) want_exec_free(&execs[index]);
        for (size_t index = 0; index < process_count; index++) want_process_free(&processes[index]);
        free(timers); free(watches); free(execs); free(processes);
        lisp_value value = lisp_cstring(error[0] ? error : "Invalid resource declaration.");
        return value;
    }
    const char *rejection = NULL;
    if (watch_count + res.watch_count > 512) rejection = "More than 512 live or candidate file watches.";
    else if (exec_count + res.retired_exec_count > 128) rejection = "More than 128 active or retiring executions.";
    else if (process_count + res_process_reservations() > 128)
        rejection = "More than 128 live or reserved managed processes.";
    if (rejection) {
        for (size_t index = 0; index < watch_count; index++) want_watch_free(&watches[index]);
        for (size_t index = 0; index < exec_count; index++) want_exec_free(&execs[index]);
        for (size_t index = 0; index < process_count; index++) want_process_free(&processes[index]);
        for (size_t index = 0; index < timer_count; index++) want_timer_free(&timers[index]);
        free(timers); free(watches); free(execs); free(processes);
        return lisp_cstring(rejection);
    }
    res.commit_sequence++;
    res_apply_timers(timers, timer_count);
    free(timers);
    res_execs_apply(execs, exec_count);
    free(execs);
    res_processes_apply(processes, process_count);
    free(processes);
    res_watches_apply(watches, watch_count);
    free(watches);
    uint64_t now = res_now();
    res_schedule(now);
    return lisp_nil();
}

static lisp_value prim_spawn(int argc, lisp_value *argv) {
    if (!res.started) lisp_error("Resource ownership is not running.");
    return res_spawn(argv[0], false);
}

static lisp_value prim_launch(int argc, lisp_value *argv) {
    if (!res.started) lisp_error("Resource ownership is not running.");
    (void)res_spawn(argv[0], true);
    return lisp_nil();
}

static lisp_value prim_media(int argc, lisp_value *argv) {
    const char *action = lisp_keywordp(argv[0]) ? lisp_symbol_name(argv[0]) : NULL;
    const char *method = NULL;
    if (action && !strcmp(action, "PLAY-PAUSE")) method = "PlayPause";
    else if (action && !strcmp(action, "NEXT")) method = "Next";
    else if (action && !strcmp(action, "PREVIOUS")) method = "Previous";
    else lisp_error("Media action must be :PLAY-PAUSE, :NEXT or :PREVIOUS.");
    char error[512];
    if (!res_media(method, error, sizeof(error))) lisp_error("%s", error);
    return lisp_nil();
}

static lisp_value prim_backlight(int argc, lisp_value *argv) {
    if (!lisp_intp(argv[0])) lisp_error("Brightness step must be an integer.");
    char error[512];
    if (!res_backlight(lisp_get_int(argv[0]), error, sizeof(error))) lisp_error("%s", error);
    return lisp_nil();
}

static lisp_value prim_describe(int argc, lisp_value *argv) {
    if (!lisp_keywordp(argv[0])) lisp_error("Describe needs a keyword kind.");
    const char *kind = lisp_symbol_name(argv[0]);
    return resources_describe(res.server, kind);
}

static lisp_value prim_services(int argc, lisp_value *argv) {
    return res_services_value();
}

static lisp_value prim_system(int argc, lisp_value *argv) {
    return res_system_value();
}

static lisp_value prim_background_window_p(int argc, lisp_value *argv) {
    if (!lisp_intp(argv[0])) return lisp_nil();
    return lisp_bool(res_background_window_p(lisp_get_int(argv[0])));
}

static lisp_value prim_sources(int argc, lisp_value *argv) {
    if (!res.started) lisp_error("Resource ownership is not running.");
    char error[512];
    if (!res_sources_set(argv[0], error, sizeof(error))) lisp_error("%s", error);
    return lisp_nil();
}

void resources_register_primitives(struct tomoe *server) {
    res.server = server;
    lisp_define_primitive("%resources-commit", prim_resources_commit, 1, 1);
    lisp_define_primitive("%spawn", prim_spawn, 1, 1);
    lisp_define_primitive("%launch", prim_launch, 1, 1);
    lisp_define_primitive("%media", prim_media, 1, 1);
    lisp_define_primitive("%backlight", prim_backlight, 1, 1);
    lisp_define_primitive("%describe", prim_describe, 1, 1);
    lisp_define_primitive("%services", prim_services, 0, 0);
    lisp_define_primitive("%system", prim_system, 0, 0);
    lisp_define_primitive("%background-window-p", prim_background_window_p, 1, 1);
    lisp_define_primitive("%sources", prim_sources, 1, 1);
    policy_set_converter(EV_TIMER, converter_timer);
    policy_set_converter(EV_WATCH, converter_watch);
    policy_set_converter(EV_EXEC, converter_exec);
    policy_set_converter(EV_PROCESS, converter_process);
    policy_set_converter(EV_SERVICES, converter_services);
    policy_set_converter(EV_SYSTEM, converter_system);
    policy_set_converter(EV_SOURCE, converter_source);
}

lisp_value resources_describe(struct tomoe *server, const char *kind) {
    lisp_value list = lisp_nil();
    (void)server;
    if (!kind) return list;
    if (!strcmp(kind, "TIMERS")) {
        for (size_t index = res.timer_count; index > 0; index--) {
            struct res_timer *timer = &res.timers[index - 1];
            lisp_value plist = lisp_list(10,
                lisp_keyword("OWNER"), lisp_cstring(timer->owner),
                lisp_keyword("NAME"), lisp_keyword(timer->name),
                lisp_keyword("KIND"), lisp_keyword(timer->repeat ? "INTERVAL" : "ONCE"),
                lisp_keyword("MILLISECONDS"), lisp_int(res_timer_period(timer)),
                lisp_keyword("STATUS"), lisp_keyword(timer->fired ? "FIRED" : "PENDING"));
            list = lisp_cons(plist, list);
        }
        return list;
    }
    if (!strcmp(kind, "WATCHES")) {
        static const char *names[] = { "READY", "CHANGED", "OVERFLOW", "UNAVAILABLE",
            "RESTORED", "READ-ERROR", "MISSING" };
        for (size_t index = res.watch_count; index > 0; index--) {
            struct res_watch *watch = &res.watches[index - 1];
            const char *status = (watch->status >= 0 && watch->status <= WATCH_MISSING)
                ? names[watch->status] : "READY";
            lisp_value plist = lisp_list(10,
                lisp_keyword("OWNER"), lisp_cstring(watch->owner),
                lisp_keyword("NAME"), lisp_keyword(watch->name),
                lisp_keyword("PATH"), lisp_cstring(watch->path),
                lisp_keyword("CONTENT-LIMIT"), lisp_uint(watch->limit),
                lisp_keyword("STATUS"), lisp_keyword(status));
            list = lisp_cons(plist, list);
        }
        return list;
    }
    if (!strcmp(kind, "EXECUTIONS")) {
        for (size_t index = res.exec_count; index > 0; index--) {
            struct res_exec *exec = &res.execs[index - 1];
            lisp_value code = exec->has_code ? lisp_int(exec->code) : lisp_nil();
            lisp_value plist = lisp_list(14,
                lisp_keyword("OWNER"), lisp_cstring(exec->owner),
                lisp_keyword("NAME"), lisp_keyword(exec->name),
                lisp_keyword("STATUS"), lisp_keyword(res_exec_status_name(exec->status)),
                lisp_keyword("DELIVERED"), lisp_bool(exec->delivered),
                lisp_keyword("TIMEOUT"), lisp_int(exec->timeout),
                lisp_keyword("OUTPUT-LIMIT"), lisp_uint(exec->limit),
                lisp_keyword("CODE"), code);
            list = lisp_cons(plist, list);
        }
        return list;
    }
    if (!strcmp(kind, "MANAGED-PROCESSES")) {
        for (size_t index = res.process_count; index > 0; index--) {
            struct res_process *process = &res.processes[index - 1];
            lisp_value code = process->has_code ? lisp_int(process->code) : lisp_nil();
            lisp_value error = process->error ? lisp_cstring(process->error) : lisp_nil();
            lisp_value plist = lisp_list(14,
                lisp_keyword("OWNER"), lisp_cstring(process->owner),
                lisp_keyword("NAME"), lisp_keyword(process->name),
                lisp_keyword("KIND"), lisp_keyword(process->kind == PROCESS_ONCE ? "ONCE" : "SERVICE"),
                lisp_keyword("STATUS"), lisp_keyword(res_process_status_name(process->status)),
                lisp_keyword("ATTEMPTS"), lisp_int(process->attempts),
                lisp_keyword("RUNNING"), lisp_bool(process->lease && process->lease->job),
                lisp_keyword("CODE"), code,
                lisp_keyword("ERROR"), error);
            list = lisp_cons(plist, list);
        }
        return list;
    }
    if (!strcmp(kind, "COUNTERS")) {
        return lisp_list(10,
            lisp_keyword("RETIRING-EXECUTIONS"), lisp_uint(res.exec_retiring),
            lisp_keyword("RETIRING-PROCESSES"), lisp_uint(res.retired_count),
            lisp_keyword("ONCE-PROCESSES"), lisp_uint(res.lease_count),
            lisp_keyword("PENDING-SPAWNS"), lisp_int(0),
            lisp_keyword("PROCESS-HISTORY"), lisp_uint(res.history_count));
    }
    return list;
}

bool res_plist_get(lisp_value plist, const char *name, lisp_value *out) {
    return res_get(plist, name, out);
}

void res_error_text(char *error, size_t size, const char *format, ...) {
    va_list arguments;
    va_start(arguments, format);
    vsnprintf(error, size, format, arguments);
    va_end(arguments);
}

char *res_plist_string(lisp_value value) {
    return res_string_value(value);
}

char *res_plist_string_field(lisp_value plist, const char *name) {
    char *text = NULL;
    res_get_string(plist, name, &text);
    return text;
}

char **res_plist_environment(lisp_value plist, size_t *count, char *error, size_t size) {
    lisp_value value;
    *count = 0;
    if (!res_get(plist, "ENV", &value) || lisp_nilp(value)) {
        char **empty = calloc(1, sizeof(char *));
        return empty;
    }
    char **entries = NULL;
    if (!res_parse_environment(value, &entries, count, error, size)) return NULL;
    return entries;
}
