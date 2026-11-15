#include "json.h"

#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct buffer {
    char *data;
    size_t length, capacity;
};

static bool buffer_reserve(struct buffer *buffer, size_t extra) {
    if (buffer->length + extra + 1 <= buffer->capacity) return true;
    size_t capacity = buffer->capacity ? buffer->capacity : 64;
    while (capacity < buffer->length + extra + 1) {
        if (capacity > JSON_MAX_TEXT * 2) return false;
        capacity *= 2;
    }
    char *data = realloc(buffer->data, capacity);
    if (!data) return false;
    buffer->data = data;
    buffer->capacity = capacity;
    return true;
}

static void buffer_free(struct buffer *buffer) {
    free(buffer->data);
    buffer->data = NULL;
    buffer->length = buffer->capacity = 0;
}

static bool buffer_add(struct buffer *buffer, const char *data, size_t length) {
    if (!buffer_reserve(buffer, length)) return false;
    memcpy(buffer->data + buffer->length, data, length);
    buffer->length += length;
    buffer->data[buffer->length] = '\0';
    return true;
}

static bool buffer_char(struct buffer *buffer, char c) {
    return buffer_add(buffer, &c, 1);
}

static void fail(char *error, size_t size, const char *format, ...) {
    if (!error || !size) return;
    va_list args;
    va_start(args, format);
    vsnprintf(error, size, format, args);
    va_end(args);
}

static int utf8_decode(const unsigned char *text, size_t length, uint32_t *out) {
    if (!length) return -1;
    unsigned char first = text[0];
    if (first < 0x80) {
        *out = first;
        return 1;
    }
    int extra;
    uint32_t value;
    if ((first & 0xe0) == 0xc0) {
        extra = 1;
        value = first & 0x1f;
    } else if ((first & 0xf0) == 0xe0) {
        extra = 2;
        value = first & 0x0f;
    } else if ((first & 0xf8) == 0xf0) {
        extra = 3;
        value = first & 0x07;
    } else {
        return -1;
    }
    if (length < (size_t)extra + 1) return -1;
    for (int i = 1; i <= extra; i++) {
        if ((text[i] & 0xc0) != 0x80) return -1;
        value = (value << 6) | (text[i] & 0x3f);
    }
    if ((extra == 1 && value < 0x80) || (extra == 2 && value < 0x800) ||
            (extra == 3 && value < 0x10000)) return -1;
    if (value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff)) return -1;
    *out = value;
    return extra + 1;
}

static bool buffer_codepoint(struct buffer *buffer, uint32_t code) {
    char out[4];
    size_t length;
    if (code < 0x80) {
        out[0] = (char)code;
        length = 1;
    } else if (code < 0x800) {
        out[0] = (char)(0xc0 | (code >> 6));
        out[1] = (char)(0x80 | (code & 0x3f));
        length = 2;
    } else if (code < 0x10000) {
        out[0] = (char)(0xe0 | (code >> 12));
        out[1] = (char)(0x80 | ((code >> 6) & 0x3f));
        out[2] = (char)(0x80 | (code & 0x3f));
        length = 3;
    } else {
        out[0] = (char)(0xf0 | (code >> 18));
        out[1] = (char)(0x80 | ((code >> 12) & 0x3f));
        out[2] = (char)(0x80 | ((code >> 6) & 0x3f));
        out[3] = (char)(0x80 | (code & 0x3f));
        length = 4;
    }
    return buffer_add(buffer, out, length);
}

static bool keyword_named(lisp_value value, const char *name) {
    if (!lisp_keywordp(value)) return false;
    const char *found = lisp_symbol_name(value);
    return found && !strcmp(found, name);
}

bool json_objectp(lisp_value value) {
    if (!lisp_consp(value) || !keyword_named(lisp_car(value), "JSON-OBJECT")) return false;
    lisp_value rest = lisp_cdr(value);
    return lisp_consp(rest) && lisp_nilp(lisp_cdr(rest));
}

bool json_arrayp(lisp_value value) {
    if (!lisp_consp(value) || !keyword_named(lisp_car(value), "JSON-ARRAY")) return false;
    lisp_value rest = lisp_cdr(value);
    return lisp_consp(rest) && lisp_nilp(lisp_cdr(rest));
}

lisp_value json_second(lisp_value value) {
    return lisp_car(lisp_cdr(value));
}

static bool key_matches(lisp_value key, const char *name) {
    size_t length = 0;
    const char *text = lisp_get_string(key, &length);
    if (!text) return false;
    return length == strlen(name) && !memcmp(text, name, length);
}

bool json_has(lisp_value object, const char *key, lisp_value *out) {
    if (!json_objectp(object)) return false;
    for (lisp_value tail = json_second(object); lisp_consp(tail); tail = lisp_cdr(tail)) {
        lisp_value entry = lisp_car(tail);
        if (!lisp_consp(entry)) return false;
        if (key_matches(lisp_car(entry), key)) {
            if (out) *out = lisp_cdr(entry);
            return true;
        }
    }
    return false;
}

lisp_value json_get(lisp_value object, const char *key) {
    lisp_value value = lisp_nil();
    json_has(object, key, &value);
    return value;
}

lisp_value json_build_object(int count, const char **keys, const lisp_value *values) {
    lisp_value members = lisp_nil();
    for (int i = 0; i < count; i++) {
        lisp_value entry = lisp_nil();
        bool found = false;
        for (lisp_value tail = members; lisp_consp(tail); tail = lisp_cdr(tail)) {
            lisp_value candidate = lisp_car(tail);
            if (key_matches(lisp_car(candidate), keys[i])) {
                entry = candidate;
                found = true;
                break;
            }
        }
        if (found) {
            lisp_value replacement = lisp_cons(lisp_car(entry), values[i]);
            lisp_value rebuilt = lisp_nil();
            for (lisp_value tail = members; lisp_consp(tail); tail = lisp_cdr(tail)) {
                lisp_value candidate = lisp_car(tail);
                rebuilt = lisp_cons(candidate == entry ? replacement : candidate, rebuilt);
            }
            members = lisp_nreverse(rebuilt);
        } else {
            members = lisp_cons(lisp_cons(lisp_cstring(keys[i]), values[i]), members);
        }
    }
    return lisp_cons(lisp_keyword("JSON-OBJECT"), lisp_cons(lisp_nreverse(members), lisp_nil()));
}

lisp_value json_build_array(int count, const lisp_value *values) {
    lisp_value items = lisp_nil();
    for (int i = count - 1; i >= 0; i--) items = lisp_cons(values[i], items);
    return lisp_cons(lisp_keyword("JSON-ARRAY"), lisp_cons(items, lisp_nil()));
}

struct parser {
    const char *text;
    size_t length, position, nodes;
    bool reject_duplicates;
    char *error;
    size_t error_size;
};

static bool parse_fail(struct parser *p, const char *format, ...) {
    if (!p->error || !p->error_size) return false;
    va_list args;
    va_start(args, format);
    vsnprintf(p->error, p->error_size, format, args);
    va_end(args);
    return false;
}

static bool parser_eof(struct parser *p) {
    return p->position >= p->length;
}

static int parser_peek(struct parser *p) {
    return parser_eof(p) ? -1 : (unsigned char)p->text[p->position];
}

static int parser_take(struct parser *p) {
    int character = parser_peek(p);
    if (character < 0) {
        parse_fail(p, "Unexpected end of JSON input.");
        return -1;
    }
    p->position++;
    return character;
}

static void parser_skip(struct parser *p) {
    while (!parser_eof(p)) {
        char c = p->text[p->position];
        if (c != ' ' && c != '\t' && c != '\n' && c != '\r') break;
        p->position++;
    }
}

static bool parser_expect(struct parser *p, char expected) {
    int character = parser_take(p);
    if (character < 0) return false;
    if (character != (unsigned char)expected) return parse_fail(p, "Expected %c in JSON input.", expected);
    return true;
}

static bool parser_hex(struct parser *p, uint32_t *out) {
    uint32_t value = 0;
    for (int i = 0; i < 4; i++) {
        int c = parser_take(p);
        if (c < 0) return false;
        int digit;
        if (c >= '0' && c <= '9') digit = c - '0';
        else if (c >= 'a' && c <= 'f') digit = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') digit = c - 'A' + 10;
        else return parse_fail(p, "Invalid Unicode escape in JSON string.");
        value = value * 16 + (uint32_t)digit;
    }
    *out = value;
    return true;
}

static bool json_codepoints(const char *text, size_t length, size_t *count) {
    size_t position = 0, total = 0;
    while (position < length) {
        uint32_t code;
        int used = utf8_decode((const unsigned char *)text + position, length - position, &code);
        if (used < 0) return false;
        position += (size_t)used;
        total++;
    }
    if (count) *count = total;
    return true;
}

static bool parse_string(struct parser *p, lisp_value *out) {
    if (!parser_expect(p, '"')) return false;
    struct buffer buffer = {0};
    size_t codepoints = 0;
    for (;;) {
        if (parser_eof(p)) {
            buffer_free(&buffer);
            return parse_fail(p, "Unterminated JSON string.");
        }
        int character = parser_take(p);
        if (character < 0) {
            buffer_free(&buffer);
            return false;
        }
        if (character == '"') break;
        uint32_t code;
        if (character == '\\') {
            int escape = parser_take(p);
            if (escape < 0) {
                buffer_free(&buffer);
                return false;
            }
            switch (escape) {
            case '"': code = '"'; break;
            case '\\': code = '\\'; break;
            case '/': code = '/'; break;
            case 'b': code = '\b'; break;
            case 'f': code = '\f'; break;
            case 'n': code = '\n'; break;
            case 'r': code = '\r'; break;
            case 't': code = '\t'; break;
            case 'u': {
                uint32_t high;
                if (!parser_hex(p, &high)) {
                    buffer_free(&buffer);
                    return false;
                }
                if (high >= 0xd800 && high <= 0xdbff) {
                    if (!parser_expect(p, '\\') || !parser_expect(p, 'u')) {
                        buffer_free(&buffer);
                        return false;
                    }
                    uint32_t low;
                    if (!parser_hex(p, &low)) {
                        buffer_free(&buffer);
                        return false;
                    }
                    if (low < 0xdc00 || low > 0xdfff) {
                        buffer_free(&buffer);
                        return parse_fail(p, "Invalid low surrogate in JSON string.");
                    }
                    code = 0x10000 + ((high - 0xd800) << 10) + (low - 0xdc00);
                } else if (high >= 0xdc00 && high <= 0xdfff) {
                    buffer_free(&buffer);
                    return parse_fail(p, "Unpaired low surrogate in JSON string.");
                } else {
                    code = high;
                }
                break;
            }
            default:
                buffer_free(&buffer);
                return parse_fail(p, "Unknown JSON string escape %c.", escape);
            }
        } else if (character <= 0x1f) {
            buffer_free(&buffer);
            return parse_fail(p, "Unescaped control character in JSON string.");
        } else if (character < 0x80) {
            code = (uint32_t)character;
        } else {
            p->position--;
            int used = utf8_decode((const unsigned char *)p->text + p->position,
                p->length - p->position, &code);
            if (used < 0) {
                buffer_free(&buffer);
                return parse_fail(p, "JSON strings must contain Unicode scalar values.");
            }
            p->position += (size_t)used;
            if (!buffer_codepoint(&buffer, code)) {
                buffer_free(&buffer);
                return parse_fail(p, "JSON string exceeds %d characters.", JSON_MAX_STRING);
            }
            if (++codepoints > JSON_MAX_STRING) {
                buffer_free(&buffer);
                return parse_fail(p, "JSON string exceeds %d characters.", JSON_MAX_STRING);
            }
            continue;
        }
        if (!buffer_codepoint(&buffer, code)) {
            buffer_free(&buffer);
            return parse_fail(p, "JSON string exceeds %d characters.", JSON_MAX_STRING);
        }
        if (++codepoints > JSON_MAX_STRING) {
            buffer_free(&buffer);
            return parse_fail(p, "JSON string exceeds %d characters.", JSON_MAX_STRING);
        }
    }
    *out = lisp_string(buffer.data ? buffer.data : "", buffer.length);
    buffer_free(&buffer);
    return true;
}

static bool parse_literal(struct parser *p, const char *literal, lisp_value value, lisp_value *out) {
    size_t length = strlen(literal);
    if (p->position + length > p->length ||
            memcmp(p->text + p->position, literal, length)) {
        return parse_fail(p, "Invalid JSON literal.");
    }
    p->position += length;
    *out = value;
    return true;
}

static bool parse_number(struct parser *p, lisp_value *out) {
    size_t start = p->position, position = start;
    bool negative = false, decimal = false;
    if (position < p->length && p->text[position] == '-') {
        negative = true;
        position++;
    }
    if (position >= p->length) return parse_fail(p, "Invalid JSON number.");
    if (p->text[position] == '0') {
        position++;
        if (position < p->length && isdigit((unsigned char)p->text[position]))
            return parse_fail(p, "JSON numbers cannot have leading zeroes.");
    } else if (isdigit((unsigned char)p->text[position])) {
        while (position < p->length && isdigit((unsigned char)p->text[position])) {
            position++;
            if (position - start > JSON_MAX_NUMBER)
                return parse_fail(p, "JSON number lexeme exceeds %d characters.", JSON_MAX_NUMBER);
        }
    } else {
        return parse_fail(p, "Invalid JSON number.");
    }
    if (position < p->length && p->text[position] == '.') {
        decimal = true;
        position++;
        if (position >= p->length || !isdigit((unsigned char)p->text[position]))
            return parse_fail(p, "JSON fraction needs digits.");
        while (position < p->length && isdigit((unsigned char)p->text[position])) {
            position++;
            if (position - start > JSON_MAX_NUMBER)
                return parse_fail(p, "JSON number lexeme exceeds %d characters.", JSON_MAX_NUMBER);
        }
    }
    if (position < p->length && (p->text[position] == 'e' || p->text[position] == 'E')) {
        decimal = true;
        position++;
        if (position < p->length && (p->text[position] == '+' || p->text[position] == '-'))
            position++;
        if (position >= p->length || !isdigit((unsigned char)p->text[position]))
            return parse_fail(p, "JSON exponent needs digits.");
        while (position < p->length && isdigit((unsigned char)p->text[position])) {
            position++;
            if (position - start > JSON_MAX_NUMBER)
                return parse_fail(p, "JSON number lexeme exceeds %d characters.", JSON_MAX_NUMBER);
        }
    }
    size_t length = position - start;
    if (length > JSON_MAX_NUMBER)
        return parse_fail(p, "JSON number lexeme exceeds %d characters.", JSON_MAX_NUMBER);
    char text[JSON_MAX_NUMBER + 1];
    memcpy(text, p->text + start, length);
    text[length] = '\0';
    p->position = position;
    if (!decimal) {
        errno = 0;
        if (negative) {
            long long value = strtoll(text, NULL, 10);
            if (errno == ERANGE) return parse_fail(p, "JSON integer is outside the data range.");
            *out = lisp_int((int64_t)value);
        } else {
            unsigned long long value = strtoull(text, NULL, 10);
            if (errno == ERANGE) return parse_fail(p, "JSON integer is outside the data range.");
            *out = value <= (unsigned long long)INT64_MAX ? lisp_int((int64_t)value) : lisp_uint(value);
        }
        return true;
    }
    double value = strtod(text, NULL);
    if (!isfinite(value))
        return parse_fail(p, "JSON number is outside the finite double-float range.");
    *out = lisp_double(value);
    return true;
}

static bool parse_value(struct parser *p, int depth, lisp_value *out);

static bool parse_array(struct parser *p, int depth, lisp_value *out) {
    if (!parser_expect(p, '[')) return false;
    parser_skip(p);
    lisp_value values = lisp_nil();
    if (parser_peek(p) == ']') {
        p->position++;
        *out = lisp_cons(lisp_keyword("JSON-ARRAY"), lisp_cons(values, lisp_nil()));
        return true;
    }
    for (;;) {
        lisp_value value;
        if (!parse_value(p, depth + 1, &value)) return false;
        values = lisp_cons(value, values);
        parser_skip(p);
        int character = parser_peek(p);
        if (character == ',') {
            p->position++;
            parser_skip(p);
            if (parser_peek(p) == ']') return parse_fail(p, "Trailing comma in JSON array.");
            continue;
        }
        if (character == ']') {
            p->position++;
            *out = lisp_cons(lisp_keyword("JSON-ARRAY"), lisp_cons(lisp_nreverse(values), lisp_nil()));
            return true;
        }
        return parse_fail(p, "Expected comma or closing bracket in JSON array.");
    }
}

static bool parse_object(struct parser *p, int depth, lisp_value *out) {
    if (!parser_expect(p, '{')) return false;
    parser_skip(p);
    lisp_value members = lisp_nil();
    if (parser_peek(p) == '}') {
        p->position++;
        *out = lisp_cons(lisp_keyword("JSON-OBJECT"), lisp_cons(members, lisp_nil()));
        return true;
    }
    for (;;) {
        if (parser_peek(p) != '"') return parse_fail(p, "JSON object keys must be strings.");
        lisp_value key;
        if (!parse_string(p, &key)) return false;
        parser_skip(p);
        if (!parser_expect(p, ':')) return false;
        parser_skip(p);
        lisp_value value;
        if (!parse_value(p, depth + 1, &value)) return false;
        lisp_value existing = lisp_nil();
        bool duplicate = false;
        for (lisp_value tail = members; lisp_consp(tail); tail = lisp_cdr(tail)) {
            lisp_value entry = lisp_car(tail);
            size_t a = 0, b = 0;
            const char *left = lisp_get_string(lisp_car(entry), &a);
            const char *right = lisp_get_string(key, &b);
            if (left && right && a == b && !memcmp(left, right, a)) {
                existing = entry;
                duplicate = true;
                break;
            }
        }
        if (duplicate) {
            if (p->reject_duplicates)
                return parse_fail(p, "Duplicate JSON object key.");
            lisp_value replacement = lisp_cons(lisp_car(existing), value);
            lisp_value rebuilt = lisp_nil();
            for (lisp_value tail = members; lisp_consp(tail); tail = lisp_cdr(tail)) {
                lisp_value entry = lisp_car(tail);
                rebuilt = lisp_cons(entry == existing ? replacement : entry, rebuilt);
            }
            members = lisp_nreverse(rebuilt);
        } else {
            members = lisp_cons(lisp_cons(key, value), members);
        }
        parser_skip(p);
        int character = parser_peek(p);
        if (character == ',') {
            p->position++;
            parser_skip(p);
            if (parser_peek(p) == '}') return parse_fail(p, "Trailing comma in JSON object.");
            continue;
        }
        if (character == '}') {
            p->position++;
            *out = lisp_cons(lisp_keyword("JSON-OBJECT"), lisp_cons(lisp_nreverse(members), lisp_nil()));
            return true;
        }
        return parse_fail(p, "Expected comma or closing brace in JSON object.");
    }
}

static bool parse_value(struct parser *p, int depth, lisp_value *out) {
    if (depth > JSON_MAX_DEPTH) return parse_fail(p, "JSON nesting exceeds %d levels.", JSON_MAX_DEPTH);
    if (++p->nodes > JSON_MAX_NODES) return parse_fail(p, "JSON value exceeds %d nodes.", JSON_MAX_NODES);
    int character = parser_peek(p);
    switch (character) {
    case '{': return parse_object(p, depth, out);
    case '[': return parse_array(p, depth, out);
    case '"': return parse_string(p, out);
    case 't': return parse_literal(p, "true", lisp_t(), out);
    case 'f': return parse_literal(p, "false", lisp_keyword("JSON-FALSE"), out);
    case 'n': return parse_literal(p, "null", lisp_nil(), out);
    default:
        if (character == '-' || (character >= 0 && isdigit((unsigned char)character)))
            return parse_number(p, out);
        if (character < 0) return parse_fail(p, "JSON input is empty.");
        return parse_fail(p, "Unexpected character in JSON input.");
    }
}

int json_parse(const char *text, size_t length, bool reject_duplicate_keys, lisp_value *out,
        char *error, size_t error_size) {
    if (error && error_size) error[0] = '\0';
    if (length > JSON_MAX_TEXT) {
        fail(error, error_size, "JSON text exceeds %d UTF-8 bytes.", JSON_MAX_TEXT);
        return -1;
    }
    if (!json_codepoints(text, length, NULL)) {
        fail(error, error_size, "JSON text is not valid UTF-8.");
        return -1;
    }
    struct parser parser = { .text = text, .length = length,
        .reject_duplicates = reject_duplicate_keys, .error = error, .error_size = error_size };
    parser_skip(&parser);
    if (parser_eof(&parser)) {
        fail(error, error_size, "JSON input is empty.");
        return -1;
    }
    lisp_value value;
    if (!parse_value(&parser, 1, &value)) return -1;
    parser_skip(&parser);
    if (!parser_eof(&parser)) {
        fail(error, error_size, "Trailing data after JSON value.");
        return -1;
    }
    *out = value;
    return 0;
}

struct writer {
    struct buffer buffer;
    size_t nodes;
    bool pretty;
    char *error;
    size_t error_size;
};

static bool write_fail(struct writer *w, const char *format, ...) {
    if (!w->error || !w->error_size) return false;
    va_list args;
    va_start(args, format);
    vsnprintf(w->error, w->error_size, format, args);
    va_end(args);
    return false;
}

static bool write_indent(struct writer *w, int depth) {
    for (int i = 0; i < depth * 2; i++)
        if (!buffer_char(&w->buffer, ' ')) return write_fail(w, "JSON text exceeds %d UTF-8 bytes.", JSON_MAX_TEXT);
    return true;
}

static bool write_value(struct writer *w, lisp_value value, int depth, lisp_value *path, int path_length);

static bool write_string(struct writer *w, const char *text, size_t length) {
    if (!buffer_char(&w->buffer, '"')) return write_fail(w, "JSON text exceeds %d UTF-8 bytes.", JSON_MAX_TEXT);
    size_t position = 0, codepoints = 0;
    while (position < length) {
        uint32_t code;
        int used = utf8_decode((const unsigned char *)text + position, length - position, &code);
        if (used < 0) return write_fail(w, "JSON strings must contain Unicode scalar values.");
        position += (size_t)used;
        if (++codepoints > JSON_MAX_STRING)
            return write_fail(w, "JSON string exceeds %d characters.", JSON_MAX_STRING);
        char escaped[8];
        const char *piece = NULL;
        size_t piece_length = 0;
        switch (code) {
        case '"': piece = "\\\""; piece_length = 2; break;
        case '\\': piece = "\\\\"; piece_length = 2; break;
        case '\b': piece = "\\b"; piece_length = 2; break;
        case '\f': piece = "\\f"; piece_length = 2; break;
        case '\n': piece = "\\n"; piece_length = 2; break;
        case '\r': piece = "\\r"; piece_length = 2; break;
        case '\t': piece = "\\t"; piece_length = 2; break;
        default:
            if (code < 0x20) {
                snprintf(escaped, sizeof(escaped), "\\u%04X", code);
                piece = escaped;
                piece_length = 6;
            }
            break;
        }
        if (piece) {
            if (!buffer_add(&w->buffer, piece, piece_length))
                return write_fail(w, "JSON text exceeds %d UTF-8 bytes.", JSON_MAX_TEXT);
        } else if (!buffer_add(&w->buffer, text + position - (size_t)used, (size_t)used)) {
            return write_fail(w, "JSON text exceeds %d UTF-8 bytes.", JSON_MAX_TEXT);
        }
    }
    if (!buffer_char(&w->buffer, '"')) return write_fail(w, "JSON text exceeds %d UTF-8 bytes.", JSON_MAX_TEXT);
    return true;
}

static bool write_number(struct writer *w, lisp_value value) {
    char text[64];
    if (lisp_intp(value)) {
        if (lisp_get_double(value) < 0)
            snprintf(text, sizeof(text), "%lld", (long long)lisp_get_int(value));
        else
            snprintf(text, sizeof(text), "%llu", (unsigned long long)lisp_get_uint(value));
    } else {
        double number = lisp_get_double(value);
        if (!isfinite(number)) return write_fail(w, "JSON numbers must be finite.");
        char formatted[64];
        snprintf(formatted, sizeof(formatted), "%.17E", number);
        char *marker = strchr(formatted, 'E');
        if (!marker) return write_fail(w, "JSON number cannot be formatted.");
        *marker = 'e';
        char *exponent = marker + 1;
        char *digits = exponent;
        if (*digits == '+' || *digits == '-') digits++;
        while (digits[0] == '0' && digits[1]) digits++;
        char rebuilt[64];
        size_t prefix = (size_t)(digits - formatted);
        if (prefix >= sizeof(rebuilt)) return write_fail(w, "JSON number cannot be formatted.");
        memcpy(rebuilt, formatted, prefix);
        snprintf(rebuilt + prefix, sizeof(rebuilt) - prefix, "%s", digits);
        snprintf(text, sizeof(text), "%s", rebuilt);
    }
    if (!buffer_add(&w->buffer, text, strlen(text)))
        return write_fail(w, "JSON text exceeds %d UTF-8 bytes.", JSON_MAX_TEXT);
    return true;
}

static bool containerp(lisp_value value, const char *keyword) {
    if (!lisp_consp(value) || !keyword_named(lisp_car(value), keyword)) return false;
    lisp_value rest = lisp_cdr(value);
    return lisp_consp(rest) && lisp_nilp(lisp_cdr(rest));
}

static bool in_path(lisp_value *path, int path_length, lisp_value value) {
    for (int i = 0; i < path_length; i++) if (path[i] == value) return true;
    return false;
}

static bool write_array(struct writer *w, lisp_value items, int depth, lisp_value *path, int path_length) {
    if (!buffer_char(&w->buffer, '[')) return write_fail(w, "JSON text exceeds %d UTF-8 bytes.", JSON_MAX_TEXT);
    if (!lisp_nilp(items)) {
        if (w->pretty) {
            if (!buffer_char(&w->buffer, '\n')) return write_fail(w, "JSON text exceeds %d UTF-8 bytes.", JSON_MAX_TEXT);
        }
        for (lisp_value tail = items;; ) {
            if (!lisp_consp(tail))
                return write_fail(w, "JSON array members must form a proper list.");
            if (w->pretty && !write_indent(w, depth + 1))
                return false;
            if (!write_value(w, lisp_car(tail), depth + 1, path, path_length)) return false;
            tail = lisp_cdr(tail);
            if (lisp_nilp(tail)) break;
            if (!buffer_char(&w->buffer, ',')) return write_fail(w, "JSON text exceeds %d UTF-8 bytes.", JSON_MAX_TEXT);
            if (w->pretty && !buffer_char(&w->buffer, '\n')) return write_fail(w, "JSON text exceeds %d UTF-8 bytes.", JSON_MAX_TEXT);
        }
        if (w->pretty) {
            if (!buffer_char(&w->buffer, '\n')) return write_fail(w, "JSON text exceeds %d UTF-8 bytes.", JSON_MAX_TEXT);
            if (!write_indent(w, depth)) return false;
        }
    }
    if (!buffer_char(&w->buffer, ']')) return write_fail(w, "JSON text exceeds %d UTF-8 bytes.", JSON_MAX_TEXT);
    return true;
}

static bool write_object(struct writer *w, lisp_value members, int depth, lisp_value *path, int path_length) {
    if (!buffer_char(&w->buffer, '{')) return write_fail(w, "JSON text exceeds %d UTF-8 bytes.", JSON_MAX_TEXT);
    if (!lisp_nilp(members)) {
        if (w->pretty) {
            if (!buffer_char(&w->buffer, '\n')) return write_fail(w, "JSON text exceeds %d UTF-8 bytes.", JSON_MAX_TEXT);
        }
        for (lisp_value tail = members;; ) {
            if (!lisp_consp(tail))
                return write_fail(w, "JSON object members must form a proper list.");
            lisp_value entry = lisp_car(tail);
            if (!lisp_consp(entry))
                return write_fail(w, "JSON object members need string keys.");
            size_t key_length = 0;
            const char *key = lisp_get_string(lisp_car(entry), &key_length);
            if (!key) return write_fail(w, "JSON object members need string keys.");
            if (w->pretty && !write_indent(w, depth + 1)) return false;
            if (!write_string(w, key, key_length)) return false;
            if (w->pretty) {
                if (!buffer_add(&w->buffer, ": ", 2)) return write_fail(w, "JSON text exceeds %d UTF-8 bytes.", JSON_MAX_TEXT);
            } else if (!buffer_char(&w->buffer, ':')) {
                return write_fail(w, "JSON text exceeds %d UTF-8 bytes.", JSON_MAX_TEXT);
            }
            if (!write_value(w, lisp_cdr(entry), depth + 1, path, path_length)) return false;
            tail = lisp_cdr(tail);
            if (lisp_nilp(tail)) break;
            if (!buffer_char(&w->buffer, ',')) return write_fail(w, "JSON text exceeds %d UTF-8 bytes.", JSON_MAX_TEXT);
            if (w->pretty && !buffer_char(&w->buffer, '\n')) return write_fail(w, "JSON text exceeds %d UTF-8 bytes.", JSON_MAX_TEXT);
        }
        if (w->pretty) {
            if (!buffer_char(&w->buffer, '\n')) return write_fail(w, "JSON text exceeds %d UTF-8 bytes.", JSON_MAX_TEXT);
            if (!write_indent(w, depth)) return false;
        }
    }
    if (!buffer_char(&w->buffer, '}')) return write_fail(w, "JSON text exceeds %d UTF-8 bytes.", JSON_MAX_TEXT);
    return true;
}

static bool write_value(struct writer *w, lisp_value value, int depth, lisp_value *path, int path_length) {
    if (depth > JSON_MAX_DEPTH) return write_fail(w, "JSON nesting exceeds %d levels.", JSON_MAX_DEPTH);
    if (++w->nodes > JSON_MAX_NODES) return write_fail(w, "JSON value exceeds %d nodes.", JSON_MAX_NODES);
    if (lisp_nilp(value)) return buffer_add(&w->buffer, "null", 4) ||
        write_fail(w, "JSON text exceeds %d UTF-8 bytes.", JSON_MAX_TEXT);
    if (value == lisp_t()) return buffer_add(&w->buffer, "true", 4) ||
        write_fail(w, "JSON text exceeds %d UTF-8 bytes.", JSON_MAX_TEXT);
    if (keyword_named(value, "JSON-FALSE"))
        return buffer_add(&w->buffer, "false", 5) ||
            write_fail(w, "JSON text exceeds %d UTF-8 bytes.", JSON_MAX_TEXT);
    if (lisp_stringp(value)) {
        size_t length = 0;
        const char *text = lisp_get_string(value, &length);
        if (!text) return write_fail(w, "JSON strings must contain Unicode scalar values.");
        return write_string(w, text, length);
    }
    if (lisp_numberp(value)) return write_number(w, value);
    if (containerp(value, "JSON-ARRAY")) {
        if (in_path(path, path_length, value)) return write_fail(w, "Cyclic JSON array.");
        if (path_length >= JSON_MAX_DEPTH) return write_fail(w, "JSON nesting exceeds %d levels.", JSON_MAX_DEPTH);
        path[path_length] = value;
        return write_array(w, json_second(value), depth, path, path_length + 1);
    }
    if (containerp(value, "JSON-OBJECT")) {
        if (in_path(path, path_length, value)) return write_fail(w, "Cyclic JSON object.");
        if (path_length >= JSON_MAX_DEPTH) return write_fail(w, "JSON nesting exceeds %d levels.", JSON_MAX_DEPTH);
        path[path_length] = value;
        return write_object(w, json_second(value), depth, path, path_length + 1);
    }
    if (lisp_consp(value)) {
        if (keyword_named(lisp_car(value), "JSON-OBJECT"))
            return write_fail(w, "Malformed JSON object container.");
        if (keyword_named(lisp_car(value), "JSON-ARRAY"))
            return write_fail(w, "Malformed JSON array container.");
    }
    return write_fail(w, "Value of unsupported type is not JSON data.");
}

char *json_write(lisp_value value, bool pretty, char *error, size_t error_size) {
    if (error && error_size) error[0] = '\0';
    struct writer writer = { .pretty = pretty, .error = error, .error_size = error_size };
    lisp_value path[JSON_MAX_DEPTH + 1];
    if (!write_value(&writer, value, 0, path, 0)) {
        buffer_free(&writer.buffer);
        return NULL;
    }
    if (!buffer_reserve(&writer.buffer, 0)) {
        buffer_free(&writer.buffer);
        fail(error, error_size, "JSON text exceeds %d UTF-8 bytes.", JSON_MAX_TEXT);
        return NULL;
    }
    writer.buffer.data[writer.buffer.length] = '\0';
    return writer.buffer.data;
}
