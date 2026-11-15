#include "json.h"
#include "lisp.h"
#include "policy.h"

#include <ctype.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum stub_type {
    STUB_NONE = 0,
    STUB_INT,
    STUB_DOUBLE,
    STUB_STRING,
    STUB_SYMBOL,
    STUB_KEYWORD,
    STUB_CONS,
    STUB_FUNCTION
};

struct stub_value {
    int type;
    bool is_unsigned;
    union {
        int64_t integer;
        double real;
        struct { char *bytes; size_t length; } string;
        struct { char *name; } symbol;
        struct { struct stub_value *car, *cdr; } cons;
    };
};

typedef struct stub_value stub_value;

static stub_value true_value = { .type = STUB_SYMBOL, .symbol = { .name = "T" } };

static stub_value *allocate(int type) {
    stub_value *value = calloc(1, sizeof(*value));
    if (!value) abort();
    value->type = type;
    return value;
}

int lisp_init(size_t region_bytes) {
    (void)region_bytes;
    return 0;
}

void lisp_shutdown(void) { }

int lisp_set_region_limit(size_t region_bytes) {
    (void)region_bytes;
    return 0;
}

void lisp_heap_stats(struct lisp_heap_stats *out) {
    if (out) memset(out, 0, sizeof(*out));
}

void lisp_define_primitive(const char *name, lisp_primitive fn, int min_args, int max_args) {
    (void)name;
    (void)fn;
    (void)min_args;
    (void)max_args;
}

int lisp_load_builtins(const struct lisp_builtin_source *sources, size_t count, char *error,
        size_t error_size) {
    (void)sources;
    (void)count;
    if (error && error_size) error[0] = '\0';
    return 0;
}

void lisp_set_limits(uint64_t steps, uint64_t deadline_ns) {
    (void)steps;
    (void)deadline_ns;
}

void lisp_check_deadline(void) { }

_Noreturn void lisp_error(const char *format, ...) {
    va_list args;
    va_start(args, format);
    fprintf(stderr, "tomoe: lisp: ");
    vfprintf(stderr, format, args);
    fputc('\n', stderr);
    va_end(args);
    abort();
}

lisp_value lisp_nil(void) { return 0; }

lisp_value lisp_t(void) { return (lisp_value)&true_value; }

lisp_value lisp_bool(bool value) { return value ? lisp_t() : lisp_nil(); }

lisp_value lisp_int(int64_t value) {
    stub_value *result = allocate(STUB_INT);
    result->integer = value;
    return (lisp_value)result;
}

lisp_value lisp_uint(uint64_t value) {
    stub_value *result = allocate(STUB_INT);
    result->integer = (int64_t)value;
    result->is_unsigned = true;
    return (lisp_value)result;
}

lisp_value lisp_double(double value) {
    stub_value *result = allocate(STUB_DOUBLE);
    result->real = value;
    return (lisp_value)result;
}

lisp_value lisp_string(const char *text, size_t length) {
    stub_value *result = allocate(STUB_STRING);
    result->string.bytes = malloc(length + 1);
    if (!result->string.bytes) abort();
    if (length) memcpy(result->string.bytes, text, length);
    result->string.bytes[length] = '\0';
    result->string.length = length;
    return (lisp_value)result;
}

lisp_value lisp_cstring(const char *text) {
    return lisp_string(text, strlen(text));
}

lisp_value lisp_keyword(const char *name) {
    stub_value *result = allocate(STUB_KEYWORD);
    result->symbol.name = strdup(name);
    if (!result->symbol.name) abort();
    return (lisp_value)result;
}

lisp_value lisp_cons(lisp_value car, lisp_value cdr) {
    stub_value *result = allocate(STUB_CONS);
    result->cons.car = (stub_value *)car;
    result->cons.cdr = (stub_value *)cdr;
    return (lisp_value)result;
}

lisp_value lisp_list(int count, ...) {
    lisp_value list = lisp_nil();
    va_list args;
    va_start(args, count);
    lisp_value *values = count ? calloc((size_t)count, sizeof(*values)) : NULL;
    if (count && !values) abort();
    for (int i = 0; i < count; i++) values[i] = va_arg(args, lisp_value);
    va_end(args);
    for (int i = count - 1; i >= 0; i--) list = lisp_cons(values[i], list);
    free(values);
    return list;
}

lisp_value lisp_nreverse(lisp_value list) {
    lisp_value result = lisp_nil();
    while (lisp_consp(list)) {
        lisp_value next = lisp_cdr(list);
        ((stub_value *)list)->cons.cdr = (stub_value *)result;
        result = list;
        list = next;
    }
    return result;
}

bool lisp_nilp(lisp_value value) { return value == 0; }

bool lisp_consp(lisp_value value) {
    stub_value *item = (stub_value *)value;
    return value && item->type == STUB_CONS;
}

bool lisp_intp(lisp_value value) {
    stub_value *item = (stub_value *)value;
    return value && item->type == STUB_INT;
}

bool lisp_numberp(lisp_value value) {
    stub_value *item = (stub_value *)value;
    return value && (item->type == STUB_INT || item->type == STUB_DOUBLE);
}

bool lisp_stringp(lisp_value value) {
    stub_value *item = (stub_value *)value;
    return value && item->type == STUB_STRING;
}

bool lisp_keywordp(lisp_value value) {
    stub_value *item = (stub_value *)value;
    return value && item->type == STUB_KEYWORD;
}

bool lisp_symbolp(lisp_value value) {
    stub_value *item = (stub_value *)value;
    return value && (item->type == STUB_SYMBOL || item->type == STUB_KEYWORD ||
        item->type == STUB_FUNCTION);
}

bool lisp_functionp(lisp_value value) {
    stub_value *item = (stub_value *)value;
    return value && item->type == STUB_FUNCTION;
}

lisp_value lisp_car(lisp_value value) {
    stub_value *item = (stub_value *)value;
    return item && item->type == STUB_CONS ? (lisp_value)item->cons.car : lisp_nil();
}

lisp_value lisp_cdr(lisp_value value) {
    stub_value *item = (stub_value *)value;
    return item && item->type == STUB_CONS ? (lisp_value)item->cons.cdr : lisp_nil();
}

int64_t lisp_get_int(lisp_value value) {
    stub_value *item = (stub_value *)value;
    if (!item) return 0;
    if (item->type == STUB_INT) return item->integer;
    if (item->type == STUB_DOUBLE) return (int64_t)item->real;
    return 0;
}

uint64_t lisp_get_uint(lisp_value value) {
    stub_value *item = (stub_value *)value;
    return item && item->type == STUB_INT ? (uint64_t)item->integer : 0;
}

double lisp_get_double(lisp_value value) {
    stub_value *item = (stub_value *)value;
    if (!item) return 0.0;
    if (item->type == STUB_DOUBLE) return item->real;
    if (item->type == STUB_INT) return item->is_unsigned ? (double)(uint64_t)item->integer
        : (double)item->integer;
    return 0.0;
}

const char *lisp_get_string(lisp_value value, size_t *length) {
    stub_value *item = (stub_value *)value;
    if (!item || item->type != STUB_STRING) {
        if (length) *length = 0;
        return NULL;
    }
    if (length) *length = item->string.length;
    return item->string.bytes;
}

const char *lisp_symbol_name(lisp_value value) {
    stub_value *item = (stub_value *)value;
    if (!item) return NULL;
    if (item->type == STUB_SYMBOL || item->type == STUB_KEYWORD) return item->symbol.name;
    return NULL;
}

lisp_value lisp_getf(lisp_value plist, const char *keyword) {
    while (lisp_consp(plist) && lisp_consp(lisp_cdr(plist))) {
        lisp_value key = lisp_car(plist);
        const char *name = lisp_symbol_name(key);
        if (name && !strcasecmp(name, keyword)) return lisp_car(lisp_cdr(plist));
        plist = lisp_cdr(lisp_cdr(plist));
    }
    return lisp_nil();
}

bool lisp_eq_keyword(lisp_value value, const char *keyword) {
    const char *name = lisp_symbol_name(value);
    return lisp_keywordp(value) && name && !strcmp(name, keyword);
}

struct stub_buffer {
    char *data;
    size_t length, capacity;
};

static bool stub_add(struct stub_buffer *buffer, const char *data, size_t length) {
    if (buffer->length + length + 1 > buffer->capacity) {
        size_t capacity = buffer->capacity ? buffer->capacity * 2 : 64;
        while (capacity < buffer->length + length + 1) capacity *= 2;
        char *grown = realloc(buffer->data, capacity);
        if (!grown) abort();
        buffer->data = grown;
        buffer->capacity = capacity;
    }
    memcpy(buffer->data + buffer->length, data, length);
    buffer->length += length;
    buffer->data[buffer->length] = '\0';
    return true;
}

static bool stub_char(struct stub_buffer *buffer, char c) {
    return stub_add(buffer, &c, 1);
}

static void stub_print(struct stub_buffer *buffer, lisp_value value, bool readable) {
    stub_value *item = (stub_value *)value;
    if (!item) {
        stub_add(buffer, "NIL", 3);
        return;
    }
    char digits[64];
    switch (item->type) {
    case STUB_INT:
        snprintf(digits, sizeof(digits), "%lld", (long long)item->integer);
        stub_add(buffer, digits, strlen(digits));
        break;
    case STUB_DOUBLE: {
        snprintf(digits, sizeof(digits), "%.17g", item->real);
        if (!strpbrk(digits, ".eE")) strcat(digits, ".0");
        strcat(digits, "d0");
        stub_add(buffer, digits, strlen(digits));
        break;
    }
    case STUB_STRING:
        if (readable) {
            stub_char(buffer, '"');
            for (size_t i = 0; i < item->string.length; i++) {
                char c = item->string.bytes[i];
                switch (c) {
                case '"': stub_add(buffer, "\\\"", 2); break;
                case '\\': stub_add(buffer, "\\\\", 2); break;
                case '\n': stub_add(buffer, "\\n", 2); break;
                case '\r': stub_add(buffer, "\\r", 2); break;
                case '\t': stub_add(buffer, "\\t", 2); break;
                default: stub_char(buffer, c); break;
                }
            }
            stub_char(buffer, '"');
        } else {
            stub_add(buffer, item->string.bytes, item->string.length);
        }
        break;
    case STUB_KEYWORD:
        stub_char(buffer, ':');
        stub_add(buffer, item->symbol.name, strlen(item->symbol.name));
        break;
    case STUB_SYMBOL:
    case STUB_FUNCTION:
        stub_add(buffer, item->symbol.name ? item->symbol.name : "FUNCTION",
            strlen(item->symbol.name ? item->symbol.name : "FUNCTION"));
        break;
    case STUB_CONS: {
        stub_char(buffer, '(');
        lisp_value tail = value;
        bool first = true;
        while (lisp_consp(tail)) {
            if (!first) stub_char(buffer, ' ');
            stub_print(buffer, lisp_car(tail), readable);
            first = false;
            tail = lisp_cdr(tail);
        }
        if (!lisp_nilp(tail)) {
            stub_add(buffer, " . ", 3);
            stub_print(buffer, tail, readable);
        }
        stub_char(buffer, ')');
        break;
    }
    default:
        stub_add(buffer, "NIL", 3);
        break;
    }
}

size_t lisp_print(lisp_value value, bool readably, char *buffer, size_t size) {
    struct stub_buffer out = {0};
    stub_print(&out, value, readably);
    if (buffer && size) {
        size_t length = out.length < size - 1 ? out.length : size - 1;
        memcpy(buffer, out.data ? out.data : "", length);
        buffer[length] = '\0';
    }
    size_t result = out.length;
    free(out.data);
    return result;
}

char *lisp_print_alloc(lisp_value value, bool readably) {
    struct stub_buffer out = {0};
    stub_print(&out, value, readably);
    if (!out.data) {
        out.data = malloc(1);
        if (!out.data) abort();
        out.data[0] = '\0';
    }
    return out.data;
}

struct stub_reader {
    const char *text;
    size_t length, position;
    char *error;
    size_t error_size;
};

static void reader_fail(struct stub_reader *reader, const char *format, ...) {
    if (!reader->error || !reader->error_size) return;
    va_list args;
    va_start(args, format);
    vsnprintf(reader->error, reader->error_size, format, args);
    va_end(args);
}

static void reader_skip(struct stub_reader *reader) {
    while (reader->position < reader->length) {
        char c = reader->text[reader->position];
        if (c != ' ' && c != '\t' && c != '\n' && c != '\r' && c != '\f') break;
        reader->position++;
    }
}

static bool reader_delimiter(char c) {
    return c == '(' || c == ')' || c == '"' || c == ';' || c == ' ' || c == '\t' ||
        c == '\n' || c == '\r' || c == '\f';
}

static lisp_value reader_value(struct stub_reader *reader, bool *ok);

static lisp_value reader_list(struct stub_reader *reader, bool *ok) {
    reader->position++;
    lisp_value head = lisp_nil(), tail = lisp_nil();
    for (;;) {
        reader_skip(reader);
        if (reader->position >= reader->length) {
            reader_fail(reader, "Unexpected end of input.");
            *ok = false;
            return lisp_nil();
        }
        if (reader->text[reader->position] == ')') {
            reader->position++;
            return head;
        }
        if (reader->text[reader->position] == '.') {
            size_t next = reader->position + 1;
            if (next < reader->length && (reader->text[next] == ' ' || reader->text[next] == ')')) {
                reader->position++;
                lisp_value rest = reader_value(reader, ok);
                if (!*ok) return lisp_nil();
                if (lisp_nilp(tail)) head = rest;
                else ((stub_value *)tail)->cons.cdr = (stub_value *)rest;
                reader_skip(reader);
                if (reader->position >= reader->length ||
                        reader->text[reader->position] != ')') {
                    reader_fail(reader, "Dotted list is not closed.");
                    *ok = false;
                    return lisp_nil();
                }
                reader->position++;
                return head;
            }
        }
        lisp_value item = reader_value(reader, ok);
        if (!*ok) return lisp_nil();
        lisp_value cell = lisp_cons(item, lisp_nil());
        if (lisp_nilp(head)) head = cell;
        else ((stub_value *)tail)->cons.cdr = (stub_value *)cell;
        tail = cell;
    }
}

static lisp_value reader_string(struct stub_reader *reader, bool *ok) {
    reader->position++;
    struct stub_buffer buffer = {0};
    for (;;) {
        if (reader->position >= reader->length) {
            free(buffer.data);
            reader_fail(reader, "Unterminated string.");
            *ok = false;
            return lisp_nil();
        }
        char c = reader->text[reader->position++];
        if (c == '"') break;
        if (c == '\\') {
            if (reader->position >= reader->length) {
                free(buffer.data);
                reader_fail(reader, "Unterminated string.");
                *ok = false;
                return lisp_nil();
            }
            char escape = reader->text[reader->position++];
            switch (escape) {
            case 'n': stub_char(&buffer, '\n'); break;
            case 't': stub_char(&buffer, '\t'); break;
            case 'r': stub_char(&buffer, '\r'); break;
            case '\\': stub_char(&buffer, '\\'); break;
            case '"': stub_char(&buffer, '"'); break;
            default: stub_char(&buffer, escape); break;
            }
            continue;
        }
        stub_char(&buffer, c);
    }
    lisp_value result = lisp_string(buffer.data ? buffer.data : "", buffer.length);
    free(buffer.data);
    return result;
}

static lisp_value reader_token(struct stub_reader *reader, bool *ok) {
    size_t start = reader->position;
    while (reader->position < reader->length &&
            !reader_delimiter(reader->text[reader->position])) reader->position++;
    size_t length = reader->position - start;
    if (!length) {
        reader_fail(reader, "Unexpected character in data.");
        *ok = false;
        return lisp_nil();
    }
    char *token = malloc(length + 1);
    if (!token) abort();
    for (size_t i = 0; i < length; i++) token[i] = (char)toupper((unsigned char)reader->text[start + i]);
    token[length] = '\0';
    bool keyword = token[0] == ':';
    lisp_value result;
    if (!keyword && !strcmp(token, "NIL")) {
        result = lisp_nil();
    } else if (!keyword && !strcmp(token, "T")) {
        result = lisp_t();
    } else {
        char *number_end = NULL;
        bool numeric = isdigit((unsigned char)token[0]) ||
            ((token[0] == '-' || token[0] == '+' || token[0] == '.') &&
             isdigit((unsigned char)token[1]));
        bool floating = numeric && (strchr(token, '.') || strchr(token, 'E') ||
            strchr(token, 'D'));
        if (numeric) {
            char *scan = token;
            for (char *p = token; *p; p++) if (*p == 'D' || *p == 'd') *p = 'E';
            if (floating) {
                double value = strtod(token, &number_end);
                if (number_end && !*number_end) result = lisp_double(value);
                else numeric = false;
            } else {
                long long value = strtoll(token, &number_end, 10);
                if (number_end && !*number_end) result = lisp_int((int64_t)value);
                else numeric = false;
            }
            (void)scan;
        }
        if (!numeric) {
            if (keyword) result = lisp_keyword(token + 1);
            else {
                stub_value *symbol = allocate(STUB_SYMBOL);
                symbol->symbol.name = strdup(token);
                if (!symbol->symbol.name) abort();
                result = (lisp_value)symbol;
            }
        }
    }
    free(token);
    return result;
}

static lisp_value reader_value(struct stub_reader *reader, bool *ok) {
    reader_skip(reader);
    if (reader->position >= reader->length) {
        reader_fail(reader, "Unexpected end of input.");
        *ok = false;
        return lisp_nil();
    }
    char c = reader->text[reader->position];
    if (c == '(') return reader_list(reader, ok);
    if (c == ')') {
        reader_fail(reader, "Unexpected close parenthesis.");
        *ok = false;
        return lisp_nil();
    }
    if (c == '"') return reader_string(reader, ok);
    return reader_token(reader, ok);
}

int lisp_read_data(const char *text, size_t length, lisp_value *out, char *error,
        size_t error_size) {
    if (error && error_size) error[0] = '\0';
    struct stub_reader reader = { .text = text, .length = length, .error = error,
        .error_size = error_size };
    bool ok = true;
    lisp_value value = reader_value(&reader, &ok);
    if (!ok) return -1;
    reader_skip(&reader);
    if (reader.position != reader.length) {
        reader_fail(&reader, "Trailing data after the first form.");
        return -1;
    }
    *out = value;
    return 0;
}

int lisp_call(const char *function, int argc, const lisp_value *argv, lisp_value *result,
        char *error, size_t error_size) {
    *result = lisp_nil();
    if (!strcasecmp(function, "TOMOE::TOMOE-BOOT") ||
            !strcasecmp(function, "TOMOE::TOMOE-DISPATCH") ||
            !strcasecmp(function, "TOMOE::TOMOE-CONTROL") ||
            !strcasecmp(function, "TOMOE::TOMOE-LOAD") ||
            !strcasecmp(function, "TOMOE::TOMOE-UNMOUNT") ||
            !strcasecmp(function, "TOMOE::TOMOE-COMMAND")) {
        return 0;
    }
    if (!strcasecmp(function, "TOMOE::TOMOE-JSON")) {
        if (argc < 1) {
            snprintf(error, error_size, "TOMOE-JSON needs a method.");
            return -1;
        }
        size_t length = 0;
        const char *method = lisp_get_string(argv[0], &length);
        if (!method) {
            snprintf(error, error_size, "TOMOE-JSON needs a method string.");
            return -1;
        }
        if (length == 7 && !memcmp(method, "version", 7)) {
            const char *keys[2] = { "wire", "version" };
            const char *version = getenv("TOMOE_VERSION");
            lisp_value values[2] = { lisp_int(3), lisp_cstring(version && *version ? version : "dev") };
            *result = json_build_object(2, keys, values);
            return 0;
        }
        if (length == 4 && !memcmp(method, "quit", 4)) {
            loop_request_quit(NULL);
            *result = lisp_t();
            return 0;
        }
        if (length == 8 && !memcmp(method, "commands", 8)) {
            *result = json_build_array(0, NULL);
            return 0;
        }
        if (length == 9 && !memcmp(method, "subscribe", 9)) {
            const char *keys[1] = { "events" };
            lisp_value values[1] = { lisp_cstring("all") };
            *result = json_build_object(1, keys, values);
            return 0;
        }
        snprintf(error, error_size, "unknown method: %.*s", (int)length, method);
        return -1;
    }
    snprintf(error, error_size, "undefined function %s", function);
    return -1;
}

void lisp_end_turn(void) { }

struct tomoe_event *policy_event_push(struct tomoe *server, enum tomoe_event_type type) {
    (void)server;
    (void)type;
    return NULL;
}

uint32_t policy_event_string(struct tomoe *server, const char *text, size_t length) {
    (void)server;
    (void)text;
    (void)length;
    return EVENT_NO_STRING;
}

uint32_t policy_event_cstring(struct tomoe *server, const char *text) {
    (void)server;
    (void)text;
    return EVENT_NO_STRING;
}

const char *policy_event_string_at(struct tomoe *server, uint32_t handle, size_t *length) {
    (void)server;
    (void)handle;
    if (length) *length = 0;
    return NULL;
}

int policy_event_barrier(struct tomoe *server) {
    (void)server;
    return 0;
}

bool policy_reads(struct tomoe *server, enum tomoe_event_type type) {
    (void)server;
    (void)type;
    return false;
}

uint64_t policy_events_dropped(struct tomoe *server) {
    (void)server;
    return 0;
}

bool policy_events_pending(struct tomoe *server) {
    (void)server;
    return false;
}

void policy_drain(struct tomoe *server) { (void)server; }

void policy_set_converter(enum tomoe_event_type type, policy_converter fn) {
    (void)type;
    (void)fn;
}

void policy_register_primitives(struct tomoe *server) { (void)server; }
void resources_register_primitives(struct tomoe *server) { (void)server; }
void ui_layout_register_primitives(struct tomoe *server) { (void)server; }

int policy_call(const char *hook, int argc, const lisp_value *argv, lisp_value *result,
        char *error, size_t error_size) {
    return lisp_call(hook, argc, argv, result, error, error_size);
}

int resources_start(struct tomoe *server) {
    (void)server;
    return 0;
}

void resources_stop(struct tomoe *server) { (void)server; }

lisp_value resources_describe(struct tomoe *server, const char *kind) {
    (void)server;
    (void)kind;
    return lisp_nil();
}
