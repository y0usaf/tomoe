#ifndef TOMOE_LISP_H
#define TOMOE_LISP_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef uintptr_t lisp_value;

struct lisp_heap_stats {
    uint64_t region_bytes;
    uint64_t heap_bytes;
    uint64_t generation_bytes;
    uint64_t arena_peak;
    uint64_t persistent_used;
    uint64_t persistent_high_water;
    uint64_t arena_used;
    uint64_t arena_high_water;
    uint64_t generations;
    uint64_t reclaim_count;
    uint64_t reclaim_longest_us;
    uint64_t steps_last;
    uint64_t budget_failures;
};

struct lisp_builtin_source {
    const char *name;
    const char *text;
    size_t length;
};

typedef lisp_value (*lisp_primitive)(int argc, lisp_value *argv);

int lisp_init(size_t region_bytes);
void lisp_shutdown(void);
int lisp_set_region_limit(size_t region_bytes);
void lisp_heap_stats(struct lisp_heap_stats *out);

void lisp_define_primitive(const char *name, lisp_primitive fn, int min_args, int max_args);
int lisp_load_builtins(const struct lisp_builtin_source *sources, size_t count, char *error, size_t error_size);

void lisp_set_limits(uint64_t steps, uint64_t deadline_ns);
void lisp_check_deadline(void);
_Noreturn void lisp_error(const char *format, ...) __attribute__((format(printf, 1, 2)));

int lisp_call(const char *function, int argc, const lisp_value *argv, lisp_value *result,
    char *error, size_t error_size);
void lisp_end_turn(void);

lisp_value lisp_nil(void);
lisp_value lisp_t(void);
lisp_value lisp_bool(bool value);
lisp_value lisp_int(int64_t value);
lisp_value lisp_uint(uint64_t value);
lisp_value lisp_double(double value);
lisp_value lisp_string(const char *text, size_t length);
lisp_value lisp_cstring(const char *text);
lisp_value lisp_keyword(const char *name);
lisp_value lisp_cons(lisp_value car, lisp_value cdr);
void lisp_set_cdr(lisp_value cell, lisp_value value);
lisp_value lisp_list(int count, ...);
lisp_value lisp_nreverse(lisp_value list);
lisp_value lisp_copy_data(lisp_value value);

bool lisp_nilp(lisp_value value);
bool lisp_consp(lisp_value value);
bool lisp_intp(lisp_value value);
bool lisp_doublep(lisp_value value);
bool lisp_numberp(lisp_value value);
bool lisp_stringp(lisp_value value);
bool lisp_keywordp(lisp_value value);
bool lisp_symbolp(lisp_value value);
bool lisp_functionp(lisp_value value);

lisp_value lisp_car(lisp_value value);
lisp_value lisp_cdr(lisp_value value);
int64_t lisp_get_int(lisp_value value);
uint64_t lisp_get_uint(lisp_value value);
double lisp_get_double(lisp_value value);
const char *lisp_get_string(lisp_value value, size_t *length);
const char *lisp_symbol_name(lisp_value value);
lisp_value lisp_getf(lisp_value plist, const char *keyword);
bool lisp_eq_keyword(lisp_value value, const char *keyword);

int lisp_read_data(const char *text, size_t length, lisp_value *out, char *error, size_t error_size);
size_t lisp_print(lisp_value value, bool readably, char *buffer, size_t size);
char *lisp_print_alloc(lisp_value value, bool readably);

#endif
