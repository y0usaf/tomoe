#include "pv.h"
#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { PV_CHUNK = 64 * 1024, PV_DEPTH = 512, PV_INTERN_MAX = 1 << 16, PV_INTERN_DECODED = (1 << 15) - 4096 };

struct pv_chunk {
    struct pv_chunk *next;
    size_t used, size;
    _Alignas(16) unsigned char data[];
};

static struct pv_chunk *arena_head;
static struct pv_chunk *arena_spare;
static size_t arena_total;
static struct pv_node nil_node = { .tag = PV_NIL };
static struct pv_node t_node = { .tag = PV_T, .u.s = "T", .length = 1 };
static char error_text[512];
jmp_buf *pv_error_target;

static struct pv_node **intern_table;
static size_t intern_capacity, intern_count;

void pv_arena_reset(void) {
    struct pv_chunk *chunk = arena_head;
    while (chunk) {
        struct pv_chunk *next = chunk->next;
        if (chunk->size == PV_CHUNK && !arena_spare) {
            chunk->next = NULL;
            chunk->used = 0;
            arena_spare = chunk;
        } else {
            free(chunk);
        }
        chunk = next;
    }
    arena_head = NULL;
    arena_total = 0;
}

size_t pv_arena_bytes(void) {
    return arena_total;
}

void *pv_arena_alloc(size_t size) {
    size = (size + 15) & ~(size_t)15;
    if (!arena_head || arena_head->size - arena_head->used < size) {
        struct pv_chunk *chunk;
        size_t want = size > PV_CHUNK ? size : PV_CHUNK;
        if (want == PV_CHUNK && arena_spare) {
            chunk = arena_spare;
            arena_spare = NULL;
        } else {
            chunk = malloc(sizeof(*chunk) + want);
            if (!chunk) pv_error("Out of memory decoding a policy value.");
            chunk->size = want;
        }
        chunk->used = 0;
        chunk->next = arena_head;
        arena_head = chunk;
    }
    void *out = arena_head->data + arena_head->used;
    arena_head->used += size;
    arena_total += size;
    return out;
}

static pv node(uint8_t tag) {
    pv value = pv_arena_alloc(sizeof(*value));
    memset(value, 0, sizeof(*value));
    value->tag = tag;
    return value;
}

pv pv_nil(void) { return &nil_node; }
pv pv_t(void) { return &t_node; }
pv pv_bool(bool value) { return value ? &t_node : &nil_node; }

pv pv_int(int64_t value) {
    pv out = node(PV_INT);
    out->u.i = value;
    return out;
}

pv pv_uint(uint64_t value) {
    if (value <= (uint64_t)INT64_MAX) return pv_int((int64_t)value);
    return pv_double((double)value);
}

pv pv_double(double value) {
    pv out = node(PV_DOUBLE);
    out->u.d = value;
    return out;
}

static pv text_node(uint8_t tag, const char *text, size_t length) {
    pv out = node(tag);
    char *copy = pv_arena_alloc(length + 1);
    if (length) memcpy(copy, text, length);
    copy[length] = 0;
    out->u.s = copy;
    out->length = (uint32_t)length;
    return out;
}

pv pv_string(const char *text, size_t length) { return text_node(PV_STRING, text, length); }
pv pv_cstring(const char *text) { return text_node(PV_STRING, text, strlen(text)); }
pv pv_read(const char *text, size_t length) { return text_node(PV_READ, text, length); }

static uint64_t name_hash(const char *name, size_t length) {
    uint64_t hash = 1469598103934665603ull;
    for (size_t i = 0; i < length; i++) hash = (hash ^ (unsigned char)name[i]) * 1099511628211ull;
    return hash;
}

static pv intern(const char *name, size_t length, bool decoded) {
    char upper[256];
    if (length >= sizeof(upper)) length = sizeof(upper) - 1;
    for (size_t i = 0; i < length; i++) upper[i] = (char)toupper((unsigned char)name[i]);
    upper[length] = 0;
    if (intern_capacity) {
        size_t slot = name_hash(upper, length) & (intern_capacity - 1);
        while (intern_table[slot]) {
            pv existing = intern_table[slot];
            if (existing->length == length && !memcmp(existing->u.s, upper, length)) return existing;
            slot = (slot + 1) & (intern_capacity - 1);
        }
    }
    if (decoded && intern_count >= PV_INTERN_DECODED) pv_error("Too many keywords.");
    if (intern_count * 2 >= intern_capacity) {
        size_t capacity = intern_capacity ? intern_capacity * 2 : 256;
        if (capacity > PV_INTERN_MAX) pv_error("Too many keywords.");
        struct pv_node **table = calloc(capacity, sizeof(*table));
        if (!table) pv_error("Out of memory interning a keyword.");
        for (size_t i = 0; i < intern_capacity; i++) {
            if (!intern_table[i]) continue;
            size_t slot = name_hash(intern_table[i]->u.s, intern_table[i]->length) & (capacity - 1);
            while (table[slot]) slot = (slot + 1) & (capacity - 1);
            table[slot] = intern_table[i];
        }
        free(intern_table);
        intern_table = table;
        intern_capacity = capacity;
    }
    size_t slot = name_hash(upper, length) & (intern_capacity - 1);
    while (intern_table[slot]) slot = (slot + 1) & (intern_capacity - 1);
    pv out = calloc(1, sizeof(*out) + length + 1);
    if (!out) pv_error("Out of memory interning a keyword.");
    char *copy = (char *)(out + 1);
    memcpy(copy, upper, length + 1);
    out->tag = PV_KEYWORD;
    out->u.s = copy;
    out->length = (uint32_t)length;
    intern_table[slot] = out;
    intern_count++;
    return out;
}

pv pv_keyword(const char *name) {
    if (name[0] == ':') name++;
    return intern(name, strlen(name), false);
}

pv pv_cons(pv car, pv cdr) {
    pv out = node(PV_CONS);
    out->u.c.car = car;
    out->u.c.cdr = cdr;
    return out;
}

void pv_set_cdr(pv cell, pv value) {
    if (cell->tag == PV_CONS) cell->u.c.cdr = value;
}

pv pv_list(int count, ...) {
    pv items[64];
    int n = count > 64 ? 64 : count;
    va_list ap;
    va_start(ap, count);
    for (int i = 0; i < n; i++) items[i] = va_arg(ap, pv);
    va_end(ap);
    pv list = &nil_node;
    for (int i = n - 1; i >= 0; i--) list = pv_cons(items[i], list);
    return list;
}

pv pv_nreverse(pv list) {
    pv result = &nil_node;
    while (list->tag == PV_CONS) {
        pv next = list->u.c.cdr;
        list->u.c.cdr = result;
        result = list;
        list = next;
    }
    return result;
}

bool pv_nilp(pv value) { return value->tag == PV_NIL; }
bool pv_consp(pv value) { return value->tag == PV_CONS; }
bool pv_intp(pv value) { return value->tag == PV_INT; }
bool pv_doublep(pv value) { return value->tag == PV_DOUBLE; }
bool pv_numberp(pv value) {
    return value->tag == PV_INT || value->tag == PV_DOUBLE ||
        (value->tag == PV_OPAQUE && value->number);
}
bool pv_stringp(pv value) { return value->tag == PV_STRING; }
bool pv_keywordp(pv value) { return value->tag == PV_KEYWORD; }
bool pv_symbolp(pv value) {
    return value->tag == PV_KEYWORD || value->tag == PV_SYMBOL || value->tag == PV_T ||
        value->tag == PV_NIL;
}

pv pv_car(pv value) { return value->tag == PV_CONS ? value->u.c.car : &nil_node; }
pv pv_cdr(pv value) { return value->tag == PV_CONS ? value->u.c.cdr : &nil_node; }

int64_t pv_get_int(pv value) {
    if (value->tag == PV_INT) return value->u.i;
    if (value->tag == PV_DOUBLE) return (int64_t)value->u.d;
    if (value->tag == PV_OPAQUE && value->number) return (int64_t)value->real;
    return 0;
}

uint64_t pv_get_uint(pv value) {
    if (value->tag == PV_INT) return (uint64_t)value->u.i;
    if (value->tag == PV_DOUBLE) return (uint64_t)value->u.d;
    return 0;
}

double pv_get_double(pv value) {
    if (value->tag == PV_INT) return (double)value->u.i;
    if (value->tag == PV_DOUBLE) return value->u.d;
    if (value->tag == PV_OPAQUE && value->number) return value->real;
    return 0.0;
}

const char *pv_get_string(pv value, size_t *length) {
    if (value->tag != PV_STRING) return NULL;
    if (length) *length = value->length;
    return value->u.s;
}

const char *pv_symbol_name(pv value) {
    if (value->tag == PV_KEYWORD || value->tag == PV_SYMBOL || value->tag == PV_T) return value->u.s;
    if (value->tag == PV_NIL) return "NIL";
    return "";
}

pv pv_getf(pv plist, const char *keyword) {
    pv key = pv_keyword(keyword);
    for (; plist->tag == PV_CONS && plist->u.c.cdr->tag == PV_CONS; plist = plist->u.c.cdr->u.c.cdr)
        if (plist->u.c.car == key) return plist->u.c.cdr->u.c.car;
    return &nil_node;
}

bool pv_eq_keyword(pv value, const char *keyword) {
    if (value->tag != PV_KEYWORD) return false;
    if (keyword[0] == ':') keyword++;
    size_t length = strlen(keyword);
    if (length != value->length) return false;
    for (size_t i = 0; i < length; i++)
        if (toupper((unsigned char)keyword[i]) != (unsigned char)value->u.s[i]) return false;
    return true;
}

struct print_out {
    char *buffer;
    size_t size, length;
};

static void out_put(struct print_out *out, const char *text, size_t length) {
    for (size_t i = 0; i < length; i++) {
        if (out->buffer && out->length + 1 < out->size) out->buffer[out->length] = text[i];
        out->length++;
    }
}

static void out_cstr(struct print_out *out, const char *text) {
    out_put(out, text, strlen(text));
}

static void print_value(struct print_out *out, pv value, bool readably, int depth) {
    char number[64];
    if (depth > PV_DEPTH) {
        out_cstr(out, "...");
        return;
    }
    switch (value->tag) {
    case PV_NIL: out_cstr(out, "NIL"); return;
    case PV_T: out_cstr(out, "T"); return;
    case PV_INT:
        snprintf(number, sizeof(number), "%lld", (long long)value->u.i);
        out_cstr(out, number);
        return;
    case PV_DOUBLE: {
        snprintf(number, sizeof(number), "%.17g", value->u.d);
        out_cstr(out, number);
        if (!strpbrk(number, ".eEni")) out_cstr(out, ".0");
        return;
    }
    case PV_STRING:
    case PV_READ:
        if (!readably || value->tag == PV_READ) {
            out_put(out, value->u.s, value->length);
            return;
        }
        out_cstr(out, "\"");
        for (uint32_t i = 0; i < value->length; i++) {
            char c = value->u.s[i];
            if (c == '"' || c == '\\') out_cstr(out, "\\");
            out_put(out, &c, 1);
        }
        out_cstr(out, "\"");
        return;
    case PV_KEYWORD:
        out_cstr(out, ":");
        out_put(out, value->u.s, value->length);
        return;
    case PV_SYMBOL:
        out_put(out, value->u.s, value->length);
        return;
    case PV_OPAQUE:
        if (value->number) {
            snprintf(number, sizeof(number), "%.17g", value->real);
            out_cstr(out, number);
        } else {
            snprintf(number, sizeof(number), "#<object %u>", value->handle);
            out_cstr(out, number);
        }
        return;
    case PV_CONS: {
        out_cstr(out, "(");
        bool first = true;
        while (value->tag == PV_CONS) {
            if (!first) out_cstr(out, " ");
            first = false;
            print_value(out, value->u.c.car, readably, depth + 1);
            value = value->u.c.cdr;
        }
        if (value->tag != PV_NIL) {
            out_cstr(out, " . ");
            print_value(out, value, readably, depth + 1);
        }
        out_cstr(out, ")");
        return;
    }
    }
}

size_t pv_print(pv value, bool readably, char *buffer, size_t size) {
    struct print_out out = { buffer, size, 0 };
    print_value(&out, value, readably, 0);
    if (buffer && size) buffer[out.length < size ? out.length : size - 1] = 0;
    return out.length;
}

char *pv_print_alloc(pv value) {
    size_t length = pv_print(value, true, NULL, 0);
    char *text = malloc(length + 1);
    if (!text) return NULL;
    pv_print(value, true, text, length + 1);
    return text;
}

void pv_error(const char *format, ...) {
    va_list ap;
    va_start(ap, format);
    vsnprintf(error_text, sizeof(error_text), format, ap);
    va_end(ap);
    if (pv_error_target) longjmp(*pv_error_target, 1);
    abort();
}

const char *pv_error_message(void) {
    return error_text;
}

void pv_buf_put(struct pv_buf *buf, const void *data, size_t length) {
    if (buf->failed) return;
    if (buf->capacity - buf->length < length) {
        size_t capacity = buf->capacity ? buf->capacity : 4096;
        while (capacity - buf->length < length) capacity *= 2;
        uint8_t *grown = realloc(buf->data, capacity);
        if (!grown) {
            buf->failed = true;
            return;
        }
        buf->data = grown;
        buf->capacity = capacity;
    }
    if (length) memcpy(buf->data + buf->length, data, length);
    buf->length += length;
}

void pv_buf_u8(struct pv_buf *buf, uint8_t value) {
    pv_buf_put(buf, &value, 1);
}

void pv_buf_u32(struct pv_buf *buf, uint32_t value) {
    pv_buf_put(buf, &value, 4);
}

void pv_buf_free(struct pv_buf *buf) {
    free(buf->data);
    *buf = (struct pv_buf){ 0 };
}

static void encode(struct pv_buf *buf, pv value, int depth) {
    if (depth > PV_DEPTH) {
        pv_buf_u8(buf, PV_NIL);
        return;
    }
    switch (value->tag) {
    case PV_NIL:
    case PV_T:
        pv_buf_u8(buf, value->tag);
        return;
    case PV_INT:
        pv_buf_u8(buf, PV_INT);
        pv_buf_put(buf, &value->u.i, 8);
        return;
    case PV_DOUBLE:
        pv_buf_u8(buf, PV_DOUBLE);
        pv_buf_put(buf, &value->u.d, 8);
        return;
    case PV_STRING:
    case PV_READ:
    case PV_KEYWORD:
        pv_buf_u8(buf, value->tag);
        pv_buf_u32(buf, value->length);
        pv_buf_put(buf, value->u.s, value->length);
        return;
    case PV_SYMBOL:
        pv_buf_u8(buf, PV_SYMBOL);
        pv_buf_u32(buf, value->handle);
        pv_buf_u32(buf, value->length);
        pv_buf_put(buf, value->u.s, value->length);
        return;
    case PV_OPAQUE:
        pv_buf_u8(buf, PV_OPAQUE);
        pv_buf_u32(buf, value->handle);
        pv_buf_u8(buf, value->number);
        pv_buf_put(buf, &value->real, 8);
        return;
    case PV_CONS: {
        uint32_t count = 0;
        pv tail = value;
        while (tail->tag == PV_CONS) {
            count++;
            tail = tail->u.c.cdr;
        }
        pv_buf_u8(buf, PV_LIST);
        pv_buf_u32(buf, count);
        for (pv cell = value; cell->tag == PV_CONS; cell = cell->u.c.cdr)
            encode(buf, cell->u.c.car, depth + 1);
        encode(buf, tail, depth + 1);
        return;
    }
    }
    pv_buf_u8(buf, PV_NIL);
}

void pv_encode(struct pv_buf *buf, pv value) {
    encode(buf, value, 0);
}

static bool take(const uint8_t *data, size_t length, size_t *offset, void *out, size_t size) {
    if (length - *offset < size) return false;
    memcpy(out, data + *offset, size);
    *offset += size;
    return true;
}

static int decode(const uint8_t *data, size_t length, size_t *offset, pv *out, int depth) {
    uint8_t tag;
    if (depth > PV_DEPTH || !take(data, length, offset, &tag, 1)) return -1;
    uint32_t count, handle;
    switch (tag) {
    case PV_NIL: *out = &nil_node; return 0;
    case PV_T: *out = &t_node; return 0;
    case PV_INT: {
        int64_t value;
        if (!take(data, length, offset, &value, 8)) return -1;
        *out = pv_int(value);
        return 0;
    }
    case PV_DOUBLE: {
        double value;
        if (!take(data, length, offset, &value, 8)) return -1;
        *out = pv_double(value);
        return 0;
    }
    case PV_STRING:
    case PV_READ:
    case PV_KEYWORD:
        if (!take(data, length, offset, &count, 4) || length - *offset < count) return -1;
        if (tag == PV_KEYWORD) *out = intern((const char *)data + *offset, count, true);
        else *out = text_node(tag, (const char *)data + *offset, count);
        *offset += count;
        return 0;
    case PV_SYMBOL:
        if (!take(data, length, offset, &handle, 4) || !take(data, length, offset, &count, 4) ||
                length - *offset < count) return -1;
        *out = text_node(PV_SYMBOL, (const char *)data + *offset, count);
        (*out)->handle = handle;
        *offset += count;
        return 0;
    case PV_OPAQUE: {
        pv value = node(PV_OPAQUE);
        if (!take(data, length, offset, &value->handle, 4) ||
                !take(data, length, offset, &value->number, 1) ||
                !take(data, length, offset, &value->real, 8)) return -1;
        *out = value;
        return 0;
    }
    case PV_LIST: {
        if (!take(data, length, offset, &count, 4) || count > length - *offset) return -1;
        pv head = &nil_node, last = NULL;
        for (uint32_t i = 0; i < count; i++) {
            pv item;
            if (decode(data, length, offset, &item, depth + 1)) return -1;
            pv cell = pv_cons(item, &nil_node);
            if (last) last->u.c.cdr = cell;
            else head = cell;
            last = cell;
        }
        pv tail;
        if (decode(data, length, offset, &tail, depth + 1)) return -1;
        if (last) last->u.c.cdr = tail;
        else head = tail;
        *out = head;
        return 0;
    }
    }
    return -1;
}

int pv_decode(const uint8_t *data, size_t length, size_t *offset, pv *out) {
    return decode(data, length, offset, out, 0);
}
