#ifndef TOMOE_PV_H
#define TOMOE_PV_H
#include <setjmp.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum pv_tag {
    PV_NIL = 0,
    PV_T = 1,
    PV_INT = 2,
    PV_DOUBLE = 3,
    PV_STRING = 4,
    PV_KEYWORD = 5,
    PV_SYMBOL = 6,
    PV_CONS = 7,
    PV_OPAQUE = 8,
    PV_READ = 9,
    PV_LIST = 10
};

struct pv_node;
typedef struct pv_node *pv;

struct pv_node {
    uint8_t tag;
    uint8_t number;
    uint32_t length;
    uint32_t handle;
    union {
        int64_t i;
        double d;
        const char *s;
        struct { pv car, cdr; } c;
    } u;
    double real;
};

struct pv_buf {
    uint8_t *data;
    size_t length, capacity;
    bool failed;
};

void pv_arena_reset(void);
size_t pv_arena_bytes(void);
void *pv_arena_alloc(size_t size);

pv pv_nil(void);
pv pv_t(void);
pv pv_bool(bool value);
pv pv_int(int64_t value);
pv pv_uint(uint64_t value);
pv pv_double(double value);
pv pv_string(const char *text, size_t length);
pv pv_cstring(const char *text);
pv pv_keyword(const char *name);
pv pv_read(const char *text, size_t length);
pv pv_cons(pv car, pv cdr);
void pv_set_cdr(pv cell, pv value);
pv pv_list(int count, ...);
pv pv_nreverse(pv list);

bool pv_nilp(pv value);
bool pv_consp(pv value);
bool pv_intp(pv value);
bool pv_doublep(pv value);
bool pv_numberp(pv value);
bool pv_stringp(pv value);
bool pv_keywordp(pv value);
bool pv_symbolp(pv value);

pv pv_car(pv value);
pv pv_cdr(pv value);
int64_t pv_get_int(pv value);
uint64_t pv_get_uint(pv value);
double pv_get_double(pv value);
const char *pv_get_string(pv value, size_t *length);
const char *pv_symbol_name(pv value);
pv pv_getf(pv plist, const char *keyword);
bool pv_eq_keyword(pv value, const char *keyword);

size_t pv_print(pv value, bool readably, char *buffer, size_t size);
char *pv_print_alloc(pv value);

_Noreturn void pv_error(const char *format, ...) __attribute__((format(printf, 1, 2)));
const char *pv_error_message(void);
extern jmp_buf *pv_error_target;

void pv_buf_put(struct pv_buf *buf, const void *data, size_t length);
void pv_buf_u8(struct pv_buf *buf, uint8_t value);
void pv_buf_u32(struct pv_buf *buf, uint32_t value);
void pv_buf_free(struct pv_buf *buf);
void pv_encode(struct pv_buf *buf, pv value);
int pv_decode(const uint8_t *data, size_t length, size_t *offset, pv *out);

#endif
