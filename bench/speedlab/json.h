#pragma once

#include <stddef.h>

enum json_kind { JSON_NULL, JSON_FALSE, JSON_TRUE, JSON_INTEGER, JSON_FLOAT, JSON_STRING, JSON_ARRAY, JSON_OBJECT };

struct json;

struct json_member {
    char *key;
    size_t key_length;
    struct json *value;
};

struct json {
    enum json_kind kind;
    union {
        long long integer;
        double number;
        struct {
            char *text;
            size_t length;
        } string;
        struct {
            struct json **items;
            size_t count, capacity;
        } array;
        struct {
            struct json_member *members;
            size_t count, capacity;
        } object;
    } as;
};

struct buffer {
    char *data;
    size_t length, capacity;
};

void out_of_memory(void);
void *allocate(size_t size);
void *allocate_more(void *data, size_t *capacity, size_t needed, size_t element);

void buffer_append(struct buffer *out, const char *text, size_t length);
void buffer_text(struct buffer *out, const char *text);
void buffer_printf(struct buffer *out, const char *format, ...);

struct json *json_object(void);
struct json *json_array(void);
struct json *json_string(const char *text);
struct json *json_string_length(const char *text, size_t length);
struct json *json_integer(long long value);
struct json *json_float(double value);
struct json *json_boolean(int value);
struct json *json_null(void);
void json_push(struct json *array, struct json *value);
void json_set(struct json *object, const char *key, struct json *value);
void json_set_json(struct json *object, const char *key, struct json *value);
struct json *json_get(const struct json *object, const char *key);
struct json *json_at(const struct json *value, size_t index);
size_t json_length(const struct json *value);
const char *json_text(const struct json *value);
long long json_int_value(const struct json *value);
double json_double_value(const struct json *value);
int json_truth(const struct json *value);
void json_free(struct json *value);

struct json *json_parse(const char *text, size_t length, char *error, size_t error_length);
void json_format(struct buffer *out, const struct json *value, int indent);
void json_format_depth(struct buffer *out, const struct json *value, int indent, int depth);
void json_format_number(struct buffer *out, double value);
double json_round(double value, int digits);
