#ifndef TOMOE_JSON_H
#define TOMOE_JSON_H
#include <stdbool.h>
#include <stddef.h>
#include "lisp.h"

enum {
    JSON_MAX_TEXT = 1048576,
    JSON_MAX_STRING = 65536,
    JSON_MAX_DEPTH = 64,
    JSON_MAX_NODES = 32768,
    JSON_MAX_NUMBER = 256
};

bool json_objectp(lisp_value value);
bool json_arrayp(lisp_value value);
lisp_value json_second(lisp_value value);
bool json_has(lisp_value object, const char *key, lisp_value *out);
lisp_value json_get(lisp_value object, const char *key);
lisp_value json_build_object(int count, const char **keys, const lisp_value *values);
lisp_value json_build_array(int count, const lisp_value *values);
int json_parse(const char *text, size_t length, bool reject_duplicate_keys, lisp_value *out,
    char *error, size_t error_size);
char *json_write(lisp_value value, bool pretty, char *error, size_t error_size);

#endif
