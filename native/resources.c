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

bool res_push_event(struct tomoe *server, enum tomoe_event_type type, uint16_t flags,
        const char *text, size_t length) {
    if (type == EV_WATCH && !policy_accepting(server) && text) {
        const char *content = memmem(text, length, " :CONTENT ", 10);
        if (content) policy_ring_drop_same(server, EV_WATCH, text, (size_t)(content - text));
    }
    struct tomoe_event *event = policy_event_push(server, type);
    if (!event) return false;
    event->flags = flags;
    event->str[0] = policy_event_string(server, text, length);
    event->str[1] = EVENT_NO_STRING;
    event->str[2] = EVENT_NO_STRING;
    event->str[3] = EVENT_NO_STRING;
    return true;
}

uint64_t res_now(void) {
    return loop_now_ms();
}

pv resources_read_text(const char *text, size_t length) {
    if (!text) return pv_nil();
    return pv_read(text, length);
}

static pv res_event_value(struct tomoe *server, const struct tomoe_event *event) {
    size_t length = 0;
    if (event->str[0] == EVENT_NO_STRING) return pv_nil();
    const char *text = policy_event_string_at(server, event->str[0], &length);
    if (!text) return pv_nil();
    return resources_read_text(text, length);
}

static pv converter_timer(struct tomoe *server, const struct tomoe_event *event) {
    return res_event_value(server, event);
}

static pv converter_watch(struct tomoe *server, const struct tomoe_event *event) {
    return res_event_value(server, event);
}

static pv converter_exec(struct tomoe *server, const struct tomoe_event *event) {
    return res_event_value(server, event);
}

static pv converter_process(struct tomoe *server, const struct tomoe_event *event) {
    return res_event_value(server, event);
}

static pv converter_services(struct tomoe *server, const struct tomoe_event *event) {
    pv value = res_event_value(server, event);
    if (pv_nilp(value)) return value;
    return pv_cons(pv_keyword("TYPE"),
        pv_cons(pv_keyword("services"),
        pv_cons(pv_keyword("SERVICES"),
        pv_cons(value, pv_nil()))));
}

static pv converter_system(struct tomoe *server, const struct tomoe_event *event) {
    return res_event_value(server, event);
}

static pv converter_source(struct tomoe *server, const struct tomoe_event *event) {
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
    bool held = false;
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
        if (!policy_reads(res.server, EV_TIMER)) {
            held = true;
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
        if (message && due->repeat && policy_loading(res.server) && !tomoe_test_flag("policy-timer-pileup"))
            policy_ring_drop_same(res.server, EV_TIMER, message, strlen(message));
        bool pushed = message && res_push_event(res.server, EV_TIMER, EVF_FACT, message, strlen(message));
        free(message);
        if (!pushed) {
            due->fired = false;
            held = true;
            break;
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
    if (held && *next < res_now() + RES_HELD_MS) *next = res_now() + RES_HELD_MS;
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

void resources_outputs_changed(void) {
    res_engine_kick();
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

static bool res_get(pv plist, const char *name, pv *out) {
    pv cursor = plist;
    while (pv_consp(cursor)) {
        pv key = pv_car(cursor);
        pv tail = pv_cdr(cursor);
        if (!pv_consp(tail)) return false;
        if (pv_keywordp(key) && pv_eq_keyword(key, name)) {
            *out = pv_car(tail);
            return true;
        }
        cursor = pv_cdr(tail);
    }
    return false;
}

static bool res_get_keyword(pv plist, const char *name, const char **out) {
    pv value;
    if (!res_get(plist, name, &value)) return false;
    if (!pv_keywordp(value)) return false;
    const char *symbol = pv_symbol_name(value);
    if (!symbol) return false;
    *out = symbol;
    return true;
}

static bool res_get_string(pv plist, const char *name, char **out) {
    pv value;
    *out = NULL;
    if (!res_get(plist, name, &value)) return true;
    if (pv_nilp(value)) return true;
    if (!pv_stringp(value)) return false;
    size_t length = 0;
    const char *text = pv_get_string(value, &length);
    if (!text) return false;
    *out = res_dup_span(text, length);
    return *out != NULL;
}

static bool res_get_int(pv plist, const char *name, int64_t *out) {
    pv value;
    if (!res_get(plist, name, &value)) return false;
    if (!pv_intp(value)) return false;
    *out = pv_get_int(value);
    return true;
}

static char *res_keyword_text(pv value) {
    const char *symbol = pv_symbol_name(value);
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

static bool res_parse_timer(pv entry, struct want_timer *want, char *error,
        size_t size) {
    pv owner, arguments;
    if (!res_get(entry, "OWNER", &owner) || !pv_stringp(owner)) {
        res_error(error, size, "Owned timer without an owner name.");
        return false;
    }
    size_t owner_length = 0;
    const char *owner_text = pv_get_string(owner, &owner_length);
    want->owner = res_dup_span(owner_text, owner_length);
    if (!res_get(entry, "ARGUMENTS", &arguments) || !pv_consp(arguments)) {
        res_error(error, size, "Owned timer without arguments.");
        return false;
    }
    pv name = pv_car(arguments);
    pv tail = pv_cdr(arguments);
    if (!pv_keywordp(name) || !pv_consp(tail)) {
        res_error(error, size, "Invalid owned timer.");
        return false;
    }
    want->name = res_keyword_text(name);
    pv milliseconds = pv_car(tail);
    tail = pv_cdr(tail);
    if (!pv_consp(tail)) {
        res_error(error, size, "Invalid owned timer.");
        return false;
    }
    want->repeat = !pv_nilp(pv_car(tail));
    if (pv_intp(milliseconds)) {
        int64_t value = pv_get_int(milliseconds);
        if (value < 0 || value > 2147483647) {
            res_error(error, size, "Invalid timer period.");
            return false;
        }
        want->declared = value;
        want->clock = TIMER_CLOCK_NONE;
    } else if (pv_keywordp(milliseconds)) {
        const char *symbol = pv_symbol_name(milliseconds);
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

static bool res_parse_watch(pv entry, struct want_watch *want, char *error,
        size_t size) {
    pv owner, arguments;
    int64_t source = 0;
    if (res_get_int(entry, "SOURCE-ID", &source) && source > 0) want->source_id = (uint64_t)source;
    if (!res_get_string(entry, "DIRECTORY", &want->directory)) {
        res_error(error, size, "Invalid file watch directory.");
        return false;
    }
    if (!res_get(entry, "OWNER", &owner) || !pv_stringp(owner)) {
        res_error(error, size, "File watch without an owner name.");
        return false;
    }
    size_t owner_length = 0;
    const char *owner_text = pv_get_string(owner, &owner_length);
    want->owner = res_dup_span(owner_text, owner_length);
    if (!res_get(entry, "ARGUMENTS", &arguments) || !pv_consp(arguments)) {
        res_error(error, size, "File watch without arguments.");
        return false;
    }
    pv name = pv_car(arguments);
    pv tail = pv_cdr(arguments);
    if (!pv_keywordp(name) || !pv_consp(tail)) {
        res_error(error, size, "Invalid file watch.");
        return false;
    }
    want->name = res_keyword_text(name);
    pv path = pv_car(tail);
    tail = pv_cdr(tail);
    if (!pv_stringp(path) || !pv_consp(tail)) {
        res_error(error, size, "Invalid file watch.");
        return false;
    }
    size_t path_length = 0;
    const char *path_text = pv_get_string(path, &path_length);
    if (!path_length || path_length > 65536 || memchr(path_text, 0, path_length)) {
        res_error(error, size, "Invalid file watch path.");
        return false;
    }
    want->path = res_dup_span(path_text, path_length);
    pv limit = pv_car(tail);
    if (!pv_intp(limit)) {
        res_error(error, size, "Invalid file watch content limit.");
        return false;
    }
    int64_t value = pv_get_int(limit);
    if (value < 1 || value > 65536) {
        res_error(error, size, "Invalid file watch content limit.");
        return false;
    }
    want->limit = (size_t)value;
    return true;
}

static bool res_parse_exec(pv entry, struct want_exec *want, char *error,
        size_t size) {
    pv owner, arguments;
    if (!res_get(entry, "OWNER", &owner) || !pv_stringp(owner)) {
        res_error(error, size, "Owned execution without an owner name.");
        return false;
    }
    size_t owner_length = 0;
    const char *owner_text = pv_get_string(owner, &owner_length);
    want->owner = res_dup_span(owner_text, owner_length);
    if (!res_get(entry, "ARGUMENTS", &arguments) || !pv_consp(arguments)) {
        res_error(error, size, "Owned execution without arguments.");
        return false;
    }
    pv name = pv_car(arguments);
    pv tail = pv_cdr(arguments);
    if (!pv_keywordp(name) || !pv_consp(tail)) {
        res_error(error, size, "Invalid owned execution.");
        return false;
    }
    want->name = res_keyword_text(name);
    pv command = pv_car(tail);
    tail = pv_cdr(tail);
    if (!pv_stringp(command) || !pv_consp(tail)) {
        res_error(error, size, "Invalid async command string.");
        return false;
    }
    size_t command_length = 0;
    const char *command_text = pv_get_string(command, &command_length);
    if (command_length > 65536 || memchr(command_text, 0, command_length)) {
        res_error(error, size, "Invalid async command string.");
        return false;
    }
    want->command = res_dup_span(command_text, command_length);
    pv timeout = pv_car(tail);
    tail = pv_cdr(tail);
    if (!pv_intp(timeout) || !pv_consp(tail)) {
        res_error(error, size, "Invalid execution timeout.");
        return false;
    }
    int64_t timeout_value = pv_get_int(timeout);
    if (timeout_value < 1 || timeout_value > 2147483647) {
        res_error(error, size, "Invalid execution timeout.");
        return false;
    }
    want->timeout = timeout_value;
    pv limit = pv_car(tail);
    if (!pv_intp(limit)) {
        res_error(error, size, "Invalid execution output limit.");
        return false;
    }
    int64_t limit_value = pv_get_int(limit);
    if (limit_value < 1 || limit_value > 65536) {
        res_error(error, size, "Invalid execution output limit.");
        return false;
    }
    want->limit = (size_t)limit_value;
    return true;
}

static char *res_string_value(pv value) {
    if (!pv_stringp(value)) return NULL;
    size_t length = 0;
    const char *text = pv_get_string(value, &length);
    if (!text || memchr(text, 0, length)) return NULL;
    return res_dup_span(text, length);
}

static bool res_parse_vector(pv list, char ***out, size_t *count, size_t limit,
        bool executable, char *error, size_t size) {
    size_t length = 0;
    for (pv cursor = list; pv_consp(cursor); cursor = pv_cdr(cursor)) length++;
    if (!length || length > limit) {
        res_error(error, size, executable ? "Process argv must contain 1 through 128 strings."
                                          : "More than 64 environment overrides.");
        return false;
    }
    char **vector = res_alloc((length + 1) * sizeof(char *));
    if (!vector) { res_error(error, size, "Cannot allocate resources."); return false; }
    size_t index = 0;
    for (pv cursor = list; pv_consp(cursor); cursor = pv_cdr(cursor)) {
        char *entry = res_string_value(pv_car(cursor));
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

static bool res_parse_environment(pv list, char ***out, size_t *count, char *error,
        size_t size) {
    size_t length = 0;
    for (pv cursor = list; pv_consp(cursor); cursor = pv_cdr(cursor)) length++;
    if (length > 64) {
        res_error(error, size, "More than 64 environment overrides.");
        return false;
    }
    char **vector = res_alloc((length + 1) * sizeof(char *));
    if (!vector) { res_error(error, size, "Cannot allocate resources."); return false; }
    size_t index = 0;
    for (pv cursor = list; pv_consp(cursor); cursor = pv_cdr(cursor)) {
        pv pair = pv_car(cursor);
        if (!pv_consp(pair)) {
            for (size_t done = 0; done < index; done++) free(vector[done]);
            free(vector);
            res_error(error, size, "Environment entries must be (name . value) pairs.");
            return false;
        }
        char *name = res_string_value(pv_car(pair));
        char *value = res_string_value(pv_cdr(pair));
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

static bool res_parse_process(pv entry, struct want_process *want, char *error,
        size_t size) {
    pv owner, arguments;
    int64_t token = 0;
    if (res_get_int(entry, "TOKEN", &token) && token > 0) want->token = (uint64_t)token;
    if (!res_get_string(entry, "DIRECTORY", &want->directory)) {
        res_error(error, size, "Invalid process directory.");
        return false;
    }
    if (!res_get(entry, "OWNER", &owner) || !pv_stringp(owner)) {
        res_error(error, size, "Process declaration without an owner name.");
        return false;
    }
    size_t owner_length = 0;
    const char *owner_text = pv_get_string(owner, &owner_length);
    want->owner = res_dup_span(owner_text, owner_length);
    if (!res_get(entry, "ARGUMENTS", &arguments) || !pv_consp(arguments)) {
        res_error(error, size, "Process declaration without arguments.");
        return false;
    }
    pv kind = pv_car(arguments);
    pv tail = pv_cdr(arguments);
    if (!pv_keywordp(kind) || !pv_consp(tail)) {
        res_error(error, size, "Invalid process declaration.");
        return false;
    }
    const char *symbol = pv_symbol_name(kind);
    if (symbol && !strcmp(symbol, "ONCE")) want->kind = PROCESS_ONCE;
    else if (symbol && !strcmp(symbol, "SERVICE")) want->kind = PROCESS_SERVICE;
    else { res_error(error, size, "Invalid process kind."); return false; }
    pv name = pv_car(tail);
    tail = pv_cdr(tail);
    if (!pv_keywordp(name) || !pv_consp(tail)) {
        res_error(error, size, "Invalid process declaration.");
        return false;
    }
    want->name = res_keyword_text(name);
    pv command = pv_car(tail);
    tail = pv_cdr(tail);
    if (pv_stringp(command)) {
        want->command = res_string_value(command);
        if (!want->command) { res_error(error, size, "Invalid process command."); return false; }
        if (strlen(want->command) > 65536) {
            res_error(error, size, "Invalid process command.");
            return false;
        }
    } else if (pv_consp(command)) {
        if (!res_parse_vector(command, &want->argv, &want->argc, 128, true, error, size))
            return false;
    } else {
        res_error(error, size, "Invalid process command.");
        return false;
    }
    pv cwd = pv_car(tail);
    tail = pv_cdr(tail);
    if (!pv_nilp(cwd)) {
        want->cwd = res_string_value(cwd);
        if (!want->cwd || !*want->cwd) {
            res_error(error, size, "Invalid process directory.");
            return false;
        }
    }
    if (!pv_consp(tail)) { res_error(error, size, "Invalid process declaration."); return false; }
    if (!res_parse_environment(pv_car(tail), &want->env, &want->env_count, error, size))
        return false;
    tail = pv_cdr(tail);
    if (!pv_consp(tail)) { res_error(error, size, "Invalid process declaration."); return false; }
    pv run = pv_car(tail);
    tail = pv_cdr(tail);
    if (!pv_nilp(run)) {
        const char *run_symbol = pv_keywordp(run) ? pv_symbol_name(run) : NULL;
        if (run_symbol && !strcmp(run_symbol, "ONCE-PER-SESSION")) want->run = RUN_ONCE_PER_SESSION;
        else if (run_symbol && !strcmp(run_symbol, "ONCE-PER-CONFIG-VERSION"))
            want->run = RUN_ONCE_PER_CONFIG_VERSION;
        else { res_error(error, size, "Invalid process run mode."); return false; }
    }
    if (!pv_consp(tail)) { res_error(error, size, "Invalid process declaration."); return false; }
    pv restart = pv_car(tail);
    tail = pv_cdr(tail);
    const char *restart_symbol = pv_keywordp(restart) ? pv_symbol_name(restart) : NULL;
    if (restart_symbol && !strcmp(restart_symbol, "NEVER")) want->restart = RESTART_NEVER;
    else if (restart_symbol && !strcmp(restart_symbol, "ON-FAILURE")) want->restart = RESTART_ON_FAILURE;
    else if (restart_symbol && !strcmp(restart_symbol, "ON-EXIT")) want->restart = RESTART_ON_EXIT;
    else if (pv_nilp(restart) && want->kind == PROCESS_ONCE) want->restart = RESTART_NEVER;
    else { res_error(error, size, "Invalid process restart policy."); return false; }
    if (!pv_consp(tail)) { res_error(error, size, "Invalid process declaration."); return false; }
    pv reload = pv_car(tail);
    const char *reload_symbol = pv_keywordp(reload) ? pv_symbol_name(reload) : NULL;
    if (reload_symbol && !strcmp(reload_symbol, "KEEP-IF-UNCHANGED"))
        want->reload = RELOAD_KEEP_IF_UNCHANGED;
    else if (reload_symbol && !strcmp(reload_symbol, "ALWAYS-RESTART"))
        want->reload = RELOAD_ALWAYS_RESTART;
    else if (pv_nilp(reload) && want->kind == PROCESS_ONCE)
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

struct res_plan {
    struct want_timer *timers;
    struct want_watch *watches;
    struct want_exec *execs;
    struct want_process *processes;
    size_t timer_count, watch_count, exec_count, process_count;
};

static void res_plan_free(struct res_plan *plan) {
    for (size_t index = 0; index < plan->timer_count; index++) want_timer_free(&plan->timers[index]);
    for (size_t index = 0; index < plan->watch_count; index++) want_watch_free(&plan->watches[index]);
    for (size_t index = 0; index < plan->exec_count; index++) want_exec_free(&plan->execs[index]);
    for (size_t index = 0; index < plan->process_count; index++) want_process_free(&plan->processes[index]);
    free(plan->timers); free(plan->watches); free(plan->execs); free(plan->processes);
    memset(plan, 0, sizeof(*plan));
}

static const char *res_plan_make(pv wanted, struct res_plan *plan, char *error, size_t size) {
    memset(plan, 0, sizeof(*plan));
    error[0] = 0;
    if (!res.started && res.server) resources_start(res.server);
    if (!res.started) return "Resource ownership is not running.";
    size_t length = 0;
    for (pv cursor = wanted; pv_consp(cursor); cursor = pv_cdr(cursor)) length++;
    plan->timers = res_alloc((length + 1) * sizeof(*plan->timers));
    plan->watches = res_alloc((length + 1) * sizeof(*plan->watches));
    plan->execs = res_alloc((length + 1) * sizeof(*plan->execs));
    plan->processes = res_alloc((length + 1) * sizeof(*plan->processes));
    bool ok = plan->timers && plan->watches && plan->execs && plan->processes;
    for (pv cursor = wanted; ok && pv_consp(cursor); cursor = pv_cdr(cursor)) {
        pv entry = pv_car(cursor);
        const char *kind = NULL;
        if (!pv_consp(entry) || !res_get_keyword(entry, "KIND", &kind)) {
            res_error(error, size, "Resource entry without a kind.");
            ok = false;
            break;
        }
        if (!strcmp(kind, "TIMER")) {
            if (++plan->timer_count > 4096) {
                res_error(error, size, "More than 4096 owned timers.");
                ok = false;
                break;
            }
            ok = res_parse_timer(entry, &plan->timers[plan->timer_count - 1], error, size);
        } else if (!strcmp(kind, "WATCH")) {
            if (++plan->watch_count > 256) {
                res_error(error, size, "More than 256 owned file watches.");
                ok = false;
                break;
            }
            ok = res_parse_watch(entry, &plan->watches[plan->watch_count - 1], error, size);
            for (size_t index = 0; ok && index + 1 < plan->watch_count; index++) {
                if (!strcmp(plan->watches[index].owner, plan->watches[plan->watch_count - 1].owner) &&
                        !strcmp(plan->watches[index].name, plan->watches[plan->watch_count - 1].name)) {
                    res_error(error, size, "Duplicate file watch: %s",
                              plan->watches[plan->watch_count - 1].name);
                    ok = false;
                }
            }
        } else if (!strcmp(kind, "EXEC")) {
            if (++plan->exec_count > 64) {
                res_error(error, size, "More than 64 owned executions.");
                ok = false;
                break;
            }
            ok = res_parse_exec(entry, &plan->execs[plan->exec_count - 1], error, size);
        } else if (!strcmp(kind, "PROCESS")) {
            if (++plan->process_count > 64) {
                res_error(error, size, "More than 64 process declarations.");
                ok = false;
                break;
            }
            ok = res_parse_process(entry, &plan->processes[plan->process_count - 1], error, size);
        } else {
            res_error(error, size, "Unknown resource kind.");
            ok = false;
        }
    }
    const char *rejection = NULL;
    if (!ok) rejection = error[0] ? error : "Invalid resource declaration.";
    else if (plan->watch_count + res.watch_count > 512) rejection = "More than 512 live or candidate file watches.";
    else if (plan->exec_count + res.retired_exec_count > 128) rejection = "More than 128 active or retiring executions.";
    else if (plan->process_count + res_process_reservations() > 128)
        rejection = "More than 128 live or reserved managed processes.";
    if (rejection) res_plan_free(plan);
    return rejection;
}

static pv prim_resources_check(int argc, pv *argv) {
    char error[512];
    struct res_plan plan;
    const char *rejection = res_plan_make(argv[0], &plan, error, sizeof(error));
    if (rejection) return pv_cstring(rejection);
    res_plan_free(&plan);
    return pv_nil();
}

static pv prim_resources_commit(int argc, pv *argv) {
    char error[512];
    struct res_plan plan;
    const char *rejection = res_plan_make(argv[0], &plan, error, sizeof(error));
    if (rejection) return pv_cstring(rejection);
    res.commit_sequence++;
    res_apply_timers(plan.timers, plan.timer_count);
    free(plan.timers);
    res_execs_apply(plan.execs, plan.exec_count);
    free(plan.execs);
    res_processes_apply(plan.processes, plan.process_count);
    free(plan.processes);
    res_watches_apply(plan.watches, plan.watch_count);
    free(plan.watches);
    res_schedule(res_now());
    return pv_nil();
}

static pv prim_spawn(int argc, pv *argv) {
    if (!res.started) pv_error("Resource ownership is not running.");
    return res_spawn(argv[0], false);
}

static pv prim_launch(int argc, pv *argv) {
    if (!res.started) pv_error("Resource ownership is not running.");
    (void)res_spawn(argv[0], true);
    return pv_nil();
}

static pv prim_media(int argc, pv *argv) {
    const char *action = pv_keywordp(argv[0]) ? pv_symbol_name(argv[0]) : NULL;
    const char *method = NULL;
    if (action && !strcmp(action, "PLAY-PAUSE")) method = "PlayPause";
    else if (action && !strcmp(action, "NEXT")) method = "Next";
    else if (action && !strcmp(action, "PREVIOUS")) method = "Previous";
    else pv_error("Media action must be :PLAY-PAUSE, :NEXT or :PREVIOUS.");
    char error[512];
    if (!res_media(method, error, sizeof(error))) pv_error("%s", error);
    return pv_nil();
}

static pv prim_backlight(int argc, pv *argv) {
    if (!pv_intp(argv[0])) pv_error("Brightness step must be an integer.");
    char error[512];
    if (!res_backlight(pv_get_int(argv[0]), error, sizeof(error))) pv_error("%s", error);
    return pv_nil();
}

static pv prim_describe(int argc, pv *argv) {
    if (!pv_keywordp(argv[0])) pv_error("Describe needs a keyword kind.");
    const char *kind = pv_symbol_name(argv[0]);
    return resources_describe(res.server, kind);
}

static pv prim_services(int argc, pv *argv) {
    return res_services_value();
}

static pv prim_system(int argc, pv *argv) {
    return res_system_value();
}

static pv prim_background_window_p(int argc, pv *argv) {
    if (!pv_intp(argv[0])) return pv_nil();
    return pv_bool(res_background_window_p(pv_get_int(argv[0])));
}

static pv prim_sources(int argc, pv *argv) {
    if (!res.started) pv_error("Resource ownership is not running.");
    char error[512];
    if (!res_sources_set(argv[0], error, sizeof(error))) pv_error("%s", error);
    return pv_nil();
}

void resources_register_primitives(struct tomoe *server) {
    res.server = server;
    prim_define("%resources-check", prim_resources_check, 1, 1);
    prim_define("%resources-commit", prim_resources_commit, 1, 1);
    prim_define("%spawn", prim_spawn, 1, 1);
    prim_define("%launch", prim_launch, 1, 1);
    prim_define("%media", prim_media, 1, 1);
    prim_define("%backlight", prim_backlight, 1, 1);
    prim_define("%describe", prim_describe, 1, 1);
    prim_define("%services", prim_services, 0, 0);
    prim_define("%system", prim_system, 0, 0);
    prim_define("%background-window-p", prim_background_window_p, 1, 1);
    prim_define("%sources", prim_sources, 1, 1);
    policy_set_converter(EV_TIMER, converter_timer);
    policy_set_converter(EV_WATCH, converter_watch);
    policy_set_converter(EV_EXEC, converter_exec);
    policy_set_converter(EV_PROCESS, converter_process);
    policy_set_converter(EV_SERVICES, converter_services);
    policy_set_converter(EV_SYSTEM, converter_system);
    policy_set_converter(EV_SOURCE, converter_source);
}

pv resources_describe(struct tomoe *server, const char *kind) {
    pv list = pv_nil();
    (void)server;
    if (!kind) return list;
    if (!strcmp(kind, "TIMERS")) {
        for (size_t index = res.timer_count; index > 0; index--) {
            struct res_timer *timer = &res.timers[index - 1];
            pv plist = pv_list(10,
                pv_keyword("OWNER"), pv_cstring(timer->owner),
                pv_keyword("NAME"), pv_keyword(timer->name),
                pv_keyword("KIND"), pv_keyword(timer->repeat ? "INTERVAL" : "ONCE"),
                pv_keyword("MILLISECONDS"), pv_int(res_timer_period(timer)),
                pv_keyword("STATUS"), pv_keyword(timer->fired ? "FIRED" : "PENDING"));
            list = pv_cons(plist, list);
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
            pv plist = pv_list(10,
                pv_keyword("OWNER"), pv_cstring(watch->owner),
                pv_keyword("NAME"), pv_keyword(watch->name),
                pv_keyword("PATH"), pv_cstring(watch->path),
                pv_keyword("CONTENT-LIMIT"), pv_uint(watch->limit),
                pv_keyword("STATUS"), pv_keyword(status));
            list = pv_cons(plist, list);
        }
        return list;
    }
    if (!strcmp(kind, "EXECUTIONS")) {
        for (size_t index = res.exec_count; index > 0; index--) {
            struct res_exec *exec = &res.execs[index - 1];
            pv code = exec->has_code ? pv_int(exec->code) : pv_nil();
            pv plist = pv_list(14,
                pv_keyword("OWNER"), pv_cstring(exec->owner),
                pv_keyword("NAME"), pv_keyword(exec->name),
                pv_keyword("STATUS"), pv_keyword(res_exec_status_name(exec->status)),
                pv_keyword("DELIVERED"), pv_bool(exec->delivered),
                pv_keyword("TIMEOUT"), pv_int(exec->timeout),
                pv_keyword("OUTPUT-LIMIT"), pv_uint(exec->limit),
                pv_keyword("CODE"), code);
            list = pv_cons(plist, list);
        }
        return list;
    }
    if (!strcmp(kind, "MANAGED-PROCESSES")) {
        for (size_t index = res.process_count; index > 0; index--) {
            struct res_process *process = &res.processes[index - 1];
            pv code = process->has_code ? pv_int(process->code) : pv_nil();
            pv error = process->error ? pv_cstring(process->error) : pv_nil();
            pv plist = pv_list(14,
                pv_keyword("OWNER"), pv_cstring(process->owner),
                pv_keyword("NAME"), pv_keyword(process->name),
                pv_keyword("KIND"), pv_keyword(process->kind == PROCESS_ONCE ? "ONCE" : "SERVICE"),
                pv_keyword("STATUS"), pv_keyword(res_process_status_name(process->status)),
                pv_keyword("ATTEMPTS"), pv_int(process->attempts),
                pv_keyword("RUNNING"), pv_bool(process->lease && process->lease->job),
                pv_keyword("CODE"), code,
                pv_keyword("ERROR"), error);
            list = pv_cons(plist, list);
        }
        return list;
    }
    if (!strcmp(kind, "COUNTERS")) {
        return pv_list(10,
            pv_keyword("RETIRING-EXECUTIONS"), pv_uint(res.exec_retiring),
            pv_keyword("RETIRING-PROCESSES"), pv_uint(res.retired_count),
            pv_keyword("ONCE-PROCESSES"), pv_uint(res.lease_count),
            pv_keyword("PENDING-SPAWNS"), pv_int(0),
            pv_keyword("PROCESS-HISTORY"), pv_uint(res.history_count));
    }
    return list;
}

bool res_plist_get(pv plist, const char *name, pv *out) {
    return res_get(plist, name, out);
}

void res_error_text(char *error, size_t size, const char *format, ...) {
    va_list arguments;
    va_start(arguments, format);
    vsnprintf(error, size, format, arguments);
    va_end(arguments);
}

char *res_plist_string(pv value) {
    return res_string_value(value);
}

char *res_plist_string_field(pv plist, const char *name) {
    char *text = NULL;
    res_get_string(plist, name, &text);
    return text;
}

char **res_plist_environment(pv plist, size_t *count, char *error, size_t size) {
    pv value;
    *count = 0;
    if (!res_get(plist, "ENV", &value) || pv_nilp(value)) {
        char **empty = calloc(1, sizeof(char *));
        return empty;
    }
    char **entries = NULL;
    if (!res_parse_environment(value, &entries, count, error, size)) return NULL;
    return entries;
}
