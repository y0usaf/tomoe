#define _GNU_SOURCE
#include <errno.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "json.h"

void out_of_memory(void) {
    fputs("speedlab: out of memory\n", stderr);
    exit(125);
}

void *allocate(size_t size) {
    void *data = calloc(1, size ? size : 1);
    if (!data) out_of_memory();
    return data;
}

void *allocate_more(void *data, size_t *capacity, size_t needed, size_t element) {
    if (needed <= *capacity) return data;
    size_t grown = *capacity ? *capacity : 8;
    while (grown < needed) grown *= 2;
    void *again = realloc(data, grown * element);
    if (!again) out_of_memory();
    *capacity = grown;
    return again;
}

void buffer_append(struct buffer *out, const char *text, size_t length) {
    out->data = allocate_more(out->data, &out->capacity, out->length + length + 1, 1);
    memcpy(out->data + out->length, text, length);
    out->length += length;
    out->data[out->length] = '\0';
}

void buffer_text(struct buffer *out, const char *text) {
    buffer_append(out, text, strlen(text));
}

void buffer_printf(struct buffer *out, const char *format, ...) {
    char local[512];
    va_list arguments;
    va_start(arguments, format);
    int written = vsnprintf(local, sizeof(local), format, arguments);
    va_end(arguments);
    if (written < 0) return;
    if ((size_t)written < sizeof(local)) {
        buffer_append(out, local, (size_t)written);
        return;
    }
    char *big = allocate((size_t)written + 1);
    va_start(arguments, format);
    vsnprintf(big, (size_t)written + 1, format, arguments);
    va_end(arguments);
    buffer_append(out, big, (size_t)written);
    free(big);
}

static struct json *make(enum json_kind kind) {
    struct json *value = allocate(sizeof(*value));
    value->kind = kind;
    return value;
}

struct json *json_object(void) {
    return make(JSON_OBJECT);
}

struct json *json_array(void) {
    return make(JSON_ARRAY);
}

struct json *json_string_length(const char *text, size_t length) {
    struct json *value = make(JSON_STRING);
    value->as.string.text = allocate(length + 1);
    memcpy(value->as.string.text, text, length);
    value->as.string.length = length;
    return value;
}

struct json *json_string(const char *text) {
    return json_string_length(text, strlen(text));
}

struct json *json_integer(long long number) {
    struct json *value = make(JSON_INTEGER);
    value->as.integer = number;
    return value;
}

struct json *json_float(double number) {
    struct json *value = make(JSON_FLOAT);
    value->as.number = number;
    return value;
}

struct json *json_boolean(int truth) {
    return make(truth ? JSON_TRUE : JSON_FALSE);
}

struct json *json_null(void) {
    return make(JSON_NULL);
}

void json_push(struct json *array, struct json *value) {
    array->as.array.items =
        allocate_more(array->as.array.items, &array->as.array.capacity, array->as.array.count + 1,
            sizeof(*array->as.array.items));
    array->as.array.items[array->as.array.count++] = value;
}

void json_set_json(struct json *object, const char *key, struct json *value) {
    size_t length = strlen(key);
    for (size_t index = 0; index < object->as.object.count; index++) {
        struct json_member *member = &object->as.object.members[index];
        if (member->key_length == length && !memcmp(member->key, key, length)) {
            json_free(member->value);
            member->value = value;
            return;
        }
    }
    object->as.object.members = allocate_more(object->as.object.members, &object->as.object.capacity,
        object->as.object.count + 1, sizeof(*object->as.object.members));
    struct json_member *member = &object->as.object.members[object->as.object.count++];
    member->key = allocate(length + 1);
    memcpy(member->key, key, length);
    member->key_length = length;
    member->value = value;
}

void json_set(struct json *object, const char *key, struct json *value) {
    json_set_json(object, key, value);
}

struct json *json_get(const struct json *object, const char *key) {
    if (!object || object->kind != JSON_OBJECT) return NULL;
    size_t length = strlen(key);
    for (size_t index = 0; index < object->as.object.count; index++) {
        const struct json_member *member = &object->as.object.members[index];
        if (member->key_length == length && !memcmp(member->key, key, length)) return member->value;
    }
    return NULL;
}

struct json *json_at(const struct json *value, size_t index) {
    if (!value || value->kind != JSON_ARRAY || index >= value->as.array.count) return NULL;
    return value->as.array.items[index];
}

size_t json_length(const struct json *value) {
    if (!value) return 0;
    if (value->kind == JSON_ARRAY) return value->as.array.count;
    if (value->kind == JSON_OBJECT) return value->as.object.count;
    if (value->kind == JSON_STRING) return value->as.string.length;
    return 0;
}

const char *json_text(const struct json *value) {
    return value && value->kind == JSON_STRING ? value->as.string.text : NULL;
}

long long json_int_value(const struct json *value) {
    if (!value) return 0;
    if (value->kind == JSON_INTEGER) return value->as.integer;
    if (value->kind == JSON_FLOAT) return (long long)value->as.number;
    return 0;
}

double json_double_value(const struct json *value) {
    if (!value) return 0;
    if (value->kind == JSON_INTEGER) return (double)value->as.integer;
    if (value->kind == JSON_FLOAT) return value->as.number;
    return 0;
}

int json_truth(const struct json *value) {
    if (!value) return 0;
    switch (value->kind) {
    case JSON_TRUE: return 1;
    case JSON_FALSE:
    case JSON_NULL: return 0;
    case JSON_INTEGER: return value->as.integer != 0;
    case JSON_FLOAT: return value->as.number != 0;
    case JSON_STRING: return value->as.string.length != 0;
    case JSON_ARRAY: return value->as.array.count != 0;
    case JSON_OBJECT: return value->as.object.count != 0;
    }
    return 0;
}

void json_free(struct json *value) {
    if (!value) return;
    switch (value->kind) {
    case JSON_STRING: free(value->as.string.text); break;
    case JSON_ARRAY:
        for (size_t index = 0; index < value->as.array.count; index++) json_free(value->as.array.items[index]);
        free(value->as.array.items);
        break;
    case JSON_OBJECT:
        for (size_t index = 0; index < value->as.object.count; index++) {
            free(value->as.object.members[index].key);
            json_free(value->as.object.members[index].value);
        }
        free(value->as.object.members);
        break;
    default: break;
    }
    free(value);
}

double json_round(double value, int digits) {
    if (!isfinite(value)) return value;
    char text[512];
    snprintf(text, sizeof(text), "%.*f", digits, value);
    return strtod(text, NULL);
}

void json_format_number(struct buffer *out, double value) {
    if (isnan(value)) {
        buffer_text(out, "NaN");
        return;
    }
    if (isinf(value)) {
        buffer_text(out, value < 0 ? "-Infinity" : "Infinity");
        return;
    }
    char text[64], digits[40];
    int precision = 0;
    for (int candidate = 1; candidate <= 17; candidate++) {
        snprintf(text, sizeof(text), "%.*e", candidate - 1, value);
        if (strtod(text, NULL) == value) {
            precision = candidate;
            break;
        }
    }
    if (!precision) snprintf(text, sizeof(text), "%.17e", value);
    const char *cursor = text;
    size_t count = 0;
    if (*cursor == '-') cursor++;
    for (; *cursor && *cursor != 'e'; cursor++)
        if (*cursor != '.') digits[count++] = *cursor;
    while (count > 1 && digits[count - 1] == '0') count--;
    int exponent = 0;
    if (*cursor == 'e') exponent = atoi(cursor + 1);
    int point = exponent + 1;
    struct buffer body = { 0 };
    if (value < 0 || (value == 0 && signbit(value))) buffer_text(&body, "-");
    if (point > -4 && point <= 16) {
        if (point <= 0) {
            buffer_text(&body, "0.");
            for (int index = 0; index < -point; index++) buffer_text(&body, "0");
            buffer_append(&body, digits, count);
        } else if ((size_t)point >= count) {
            buffer_append(&body, digits, count);
            for (size_t index = count; index < (size_t)point; index++) buffer_text(&body, "0");
            buffer_text(&body, ".0");
        } else {
            buffer_append(&body, digits, (size_t)point);
            buffer_text(&body, ".");
            buffer_append(&body, digits + point, count - (size_t)point);
        }
    } else {
        buffer_append(&body, digits, 1);
        if (count > 1) {
            buffer_text(&body, ".");
            buffer_append(&body, digits + 1, count - 1);
        }
        buffer_printf(&body, "e%c%02d", point - 1 < 0 ? '-' : '+', abs(point - 1));
    }
    buffer_append(out, body.data, body.length);
    free(body.data);
}

static void write_string(struct buffer *out, const char *text, size_t length) {
    buffer_text(out, "\"");
    size_t start = 0;
    for (size_t index = 0; index < length; index++) {
        unsigned char byte = (unsigned char)text[index];
        if (byte >= 0x20 && byte != '"' && byte != '\\' && byte < 0x7f) continue;
        buffer_append(out, text + start, index - start);
        start = index + 1;
        switch (byte) {
        case '"': buffer_text(out, "\\\""); continue;
        case '\\': buffer_text(out, "\\\\"); continue;
        case '\n': buffer_text(out, "\\n"); continue;
        case '\r': buffer_text(out, "\\r"); continue;
        case '\t': buffer_text(out, "\\t"); continue;
        case '\b': buffer_text(out, "\\b"); continue;
        case '\f': buffer_text(out, "\\f"); continue;
        default: break;
        }
        if (byte < 0x20) {
            buffer_printf(out, "\\u%04x", byte);
            continue;
        }
        size_t width = byte >= 0xf0 ? 4 : byte >= 0xe0 ? 3 : byte >= 0xc0 ? 2 : 1;
        if (width == 1 || index + width > length) {
            buffer_printf(out, "\\u%04x", byte);
            continue;
        }
        unsigned int code = width == 2 ? (unsigned int)(byte & 0x1f)
                                          : width == 3 ? (unsigned int)(byte & 0x0f) : (unsigned int)(byte & 0x07);
        for (size_t extra = 1; extra < width; extra++)
            code = (code << 6) | ((unsigned char)text[index + extra] & 0x3f);
        index += width - 1;
        start = index + 1;
        if (code >= 0x10000) {
            unsigned int high = 0xd800 + ((code - 0x10000) >> 10), low = 0xdc00 + ((code - 0x10000) & 0x3ff);
            buffer_printf(out, "\\u%04x\\u%04x", high, low);
        } else {
            buffer_printf(out, "\\u%04x", code);
        }
    }
    buffer_append(out, text + start, length - start);
    buffer_text(out, "\"");
}

static void newline(struct buffer *out, int indent, int depth) {
    if (!indent) return;
    buffer_text(out, "\n");
    for (int index = 0; index < depth; index++) buffer_text(out, " ");
}

void json_format_depth(struct buffer *out, const struct json *value, int indent, int depth) {
    if (!value) {
        buffer_text(out, "null");
        return;
    }
    switch (value->kind) {
    case JSON_NULL: buffer_text(out, "null"); break;
    case JSON_TRUE: buffer_text(out, "true"); break;
    case JSON_FALSE: buffer_text(out, "false"); break;
    case JSON_INTEGER: buffer_printf(out, "%lld", value->as.integer); break;
    case JSON_FLOAT: json_format_number(out, value->as.number); break;
    case JSON_STRING: write_string(out, value->as.string.text, value->as.string.length); break;
    case JSON_ARRAY:
        if (!value->as.array.count) {
            buffer_text(out, "[]");
            break;
        }
        buffer_text(out, "[");
        for (size_t index = 0; index < value->as.array.count; index++) {
            if (index) buffer_text(out, indent ? "," : ", ");
            newline(out, indent, depth + 1);
            json_format_depth(out, value->as.array.items[index], indent, depth + 1);
        }
        newline(out, indent, depth);
        buffer_text(out, "]");
        break;
    case JSON_OBJECT:
        if (!value->as.object.count) {
            buffer_text(out, "{}");
            break;
        }
        buffer_text(out, "{");
        for (size_t index = 0; index < value->as.object.count; index++) {
            const struct json_member *member = &value->as.object.members[index];
            if (index) buffer_text(out, indent ? "," : ", ");
            newline(out, indent, depth + 1);
            write_string(out, member->key, member->key_length);
            buffer_text(out, ": ");
            json_format_depth(out, member->value, indent, depth + 1);
        }
        newline(out, indent, depth);
        buffer_text(out, "}");
        break;
    }
}

void json_format(struct buffer *out, const struct json *value, int indent) {
    json_format_depth(out, value, indent, 0);
}

struct parser {
    const char *text;
    size_t length, at;
    char *error;
    size_t error_length;
    int failed;
};

static void fail(struct parser *parser, const char *what) {
    if (parser->failed) return;
    parser->failed = 1;
    if (parser->error && parser->error_length)
        snprintf(parser->error, parser->error_length, "%s at byte %zu", what, parser->at);
}

static void skip_space(struct parser *parser) {
    while (parser->at < parser->length) {
        char byte = parser->text[parser->at];
        if (byte != ' ' && byte != '\t' && byte != '\n' && byte != '\r') break;
        parser->at++;
    }
}

static struct json *parse_value(struct parser *parser, int depth);

static void append_utf8(struct buffer *out, unsigned int code) {
    char bytes[4];
    size_t width;
    if (code < 0x80) {
        bytes[0] = (char)code;
        width = 1;
    } else if (code < 0x800) {
        bytes[0] = (char)(0xc0 | (code >> 6));
        bytes[1] = (char)(0x80 | (code & 0x3f));
        width = 2;
    } else if (code < 0x10000) {
        bytes[0] = (char)(0xe0 | (code >> 12));
        bytes[1] = (char)(0x80 | ((code >> 6) & 0x3f));
        bytes[2] = (char)(0x80 | (code & 0x3f));
        width = 3;
    } else {
        bytes[0] = (char)(0xf0 | (code >> 18));
        bytes[1] = (char)(0x80 | ((code >> 12) & 0x3f));
        bytes[2] = (char)(0x80 | ((code >> 6) & 0x3f));
        bytes[3] = (char)(0x80 | (code & 0x3f));
        width = 4;
    }
    buffer_append(out, bytes, width);
}

static int hex_digit(char byte) {
    if (byte >= '0' && byte <= '9') return byte - '0';
    if (byte >= 'a' && byte <= 'f') return byte - 'a' + 10;
    if (byte >= 'A' && byte <= 'F') return byte - 'A' + 10;
    return -1;
}

static unsigned int parse_escape(struct parser *parser) {
    if (parser->at >= parser->length) {
        fail(parser, "truncated escape");
        return 0;
    }
    char byte = parser->text[parser->at++];
    switch (byte) {
    case '"': return '"';
    case '\\': return '\\';
    case '/': return '/';
    case 'b': return '\b';
    case 'f': return '\f';
    case 'n': return '\n';
    case 'r': return '\r';
    case 't': return '\t';
    case 'u': break;
    default: fail(parser, "bad escape"); return 0;
    }
    unsigned int code = 0;
    for (int index = 0; index < 4; index++) {
        if (parser->at >= parser->length) {
            fail(parser, "truncated escape");
            return 0;
        }
        int digit = hex_digit(parser->text[parser->at++]);
        if (digit < 0) {
            fail(parser, "bad escape");
            return 0;
        }
        code = code << 4 | (unsigned int)digit;
    }
    if (code >= 0xd800 && code <= 0xdbff && parser->at + 6 <= parser->length && parser->text[parser->at] == '\\' &&
        parser->text[parser->at + 1] == 'u') {
        unsigned int low = 0;
        int good = 1;
        for (int index = 0; index < 4; index++) {
            int digit = hex_digit(parser->text[parser->at + 2 + index]);
            if (digit < 0) {
                good = 0;
                break;
            }
            low = low << 4 | (unsigned int)digit;
        }
        if (good && low >= 0xdc00 && low <= 0xdfff) {
            parser->at += 6;
            return 0x10000 + ((code - 0xd800) << 10) + (low - 0xdc00);
        }
    }
    return code;
}

static struct json *parse_string(struct parser *parser) {
    parser->at++;
    struct buffer text = { 0 };
    buffer_text(&text, "");
    while (1) {
        if (parser->at >= parser->length) {
            fail(parser, "truncated string");
            free(text.data);
            return NULL;
        }
        unsigned char byte = (unsigned char)parser->text[parser->at];
        if (byte == '"') {
            parser->at++;
            break;
        }
        if (byte != '\\') {
            size_t start = parser->at;
            while (parser->at < parser->length && parser->text[parser->at] != '"' &&
                   parser->text[parser->at] != '\\')
                parser->at++;
            buffer_append(&text, parser->text + start, parser->at - start);
            continue;
        }
        parser->at++;
        unsigned int code = parse_escape(parser);
        if (parser->failed) {
            free(text.data);
            return NULL;
        }
        append_utf8(&text, code);
    }
    struct json *value = json_string_length(text.data, text.length);
    free(text.data);
    return value;
}

static struct json *parse_number(struct parser *parser) {
    size_t start = parser->at;
    int floating = 0;
    if (parser->at < parser->length && (parser->text[parser->at] == '-' || parser->text[parser->at] == '+'))
        parser->at++;
    while (parser->at < parser->length) {
        char byte = parser->text[parser->at];
        if (byte >= '0' && byte <= '9') {
            parser->at++;
            continue;
        }
        if (byte == '.' || byte == 'e' || byte == 'E' || byte == '+' || byte == '-') {
            floating = 1;
            parser->at++;
            continue;
        }
        break;
    }
    if (parser->at == start) {
        fail(parser, "bad number");
        return NULL;
    }
    char local[64];
    size_t width = parser->at - start;
    char *text = width < sizeof(local) ? local : allocate(width + 1);
    memcpy(text, parser->text + start, width);
    text[width] = '\0';
    struct json *value;
    if (!floating) {
        errno = 0;
        char *end = NULL;
        long long number = strtoll(text, &end, 10);
        if (!errno && end && *end == '\0') value = json_integer(number);
        else value = json_float(strtod(text, NULL));
    } else {
        value = json_float(strtod(text, NULL));
    }
    if (text != local) free(text);
    return value;
}

static int match(struct parser *parser, const char *word) {
    size_t length = strlen(word);
    if (parser->at + length > parser->length || memcmp(parser->text + parser->at, word, length)) return 0;
    parser->at += length;
    return 1;
}

static struct json *parse_value(struct parser *parser, int depth) {
    if (depth > 200) {
        fail(parser, "too deep");
        return NULL;
    }
    skip_space(parser);
    if (parser->at >= parser->length) {
        fail(parser, "unexpected end");
        return NULL;
    }
    char byte = parser->text[parser->at];
    if (byte == '{') {
        parser->at++;
        struct json *object = json_object();
        skip_space(parser);
        if (parser->at < parser->length && parser->text[parser->at] == '}') {
            parser->at++;
            return object;
        }
        while (1) {
            skip_space(parser);
            if (parser->at >= parser->length || parser->text[parser->at] != '"') {
                fail(parser, "expected a key");
                json_free(object);
                return NULL;
            }
            struct json *key = parse_string(parser);
            if (!key) {
                json_free(object);
                return NULL;
            }
            skip_space(parser);
            if (parser->at >= parser->length || parser->text[parser->at] != ':') {
                fail(parser, "expected a colon");
                json_free(key);
                json_free(object);
                return NULL;
            }
            parser->at++;
            struct json *value = parse_value(parser, depth + 1);
            if (!value) {
                json_free(key);
                json_free(object);
                return NULL;
            }
            json_set_json(object, key->as.string.text, value);
            json_free(key);
            skip_space(parser);
            if (parser->at < parser->length && parser->text[parser->at] == ',') {
                parser->at++;
                continue;
            }
            if (parser->at < parser->length && parser->text[parser->at] == '}') {
                parser->at++;
                return object;
            }
            fail(parser, "expected a comma or a brace");
            json_free(object);
            return NULL;
        }
    }
    if (byte == '[') {
        parser->at++;
        struct json *array = json_array();
        skip_space(parser);
        if (parser->at < parser->length && parser->text[parser->at] == ']') {
            parser->at++;
            return array;
        }
        while (1) {
            struct json *value = parse_value(parser, depth + 1);
            if (!value) {
                json_free(array);
                return NULL;
            }
            json_push(array, value);
            skip_space(parser);
            if (parser->at < parser->length && parser->text[parser->at] == ',') {
                parser->at++;
                continue;
            }
            if (parser->at < parser->length && parser->text[parser->at] == ']') {
                parser->at++;
                return array;
            }
            fail(parser, "expected a comma or a bracket");
            json_free(array);
            return NULL;
        }
    }
    if (byte == '"') return parse_string(parser);
    if (match(parser, "null")) return json_null();
    if (match(parser, "true")) return json_boolean(1);
    if (match(parser, "false")) return json_boolean(0);
    if (byte == 'N' && match(parser, "NaN")) return json_float(NAN);
    if (byte == 'I' && match(parser, "Infinity")) return json_float(INFINITY);
    if (byte == '-' && parser->at + 1 < parser->length && parser->text[parser->at + 1] == 'I') {
        parser->at++;
        if (match(parser, "Infinity")) return json_float(-INFINITY);
        fail(parser, "bad value");
        return NULL;
    }
    return parse_number(parser);
}

struct json *json_parse(const char *text, size_t length, char *error, size_t error_length) {
    struct parser parser = { text, length, 0, error, error_length, 0 };
    if (error && error_length) error[0] = '\0';
    struct json *value = parse_value(&parser, 0);
    if (!value) return NULL;
    skip_space(&parser);
    if (parser.at != parser.length) {
        fail(&parser, "trailing bytes");
        json_free(value);
        return NULL;
    }
    return value;
}
