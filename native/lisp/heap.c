#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <sys/mman.h>
#include <time.h>
#include <errno.h>

#include "lisp-internal.h"

struct lisp_runtime lisp;

static char *reservation;
static uint32_t free_block = UINT32_MAX;
static void *memo_src[1024];
static void *memo_dst[1024];
static int memo_count;

static lisp_value deep_copy(lisp_value value, struct lisp_region *dst);

uint64_t lisp_now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static void *block_base(uint32_t index)
{
    return reservation + (size_t)index * LISP_BLOCK_SIZE;
}

void buf_init(struct lisp_buf *buf, char *data, size_t cap)
{
    buf->data = data;
    buf->len = 0;
    buf->cap = cap;
    buf->owned = false;
    if (cap > 0)
        data[0] = '\0';
}

void buf_putc(struct lisp_buf *buf, char c)
{
    if (buf->len + 1 < buf->cap)
        buf->data[buf->len++] = c;
    if (buf->cap > 0)
        buf->data[buf->len < buf->cap ? buf->len : buf->cap - 1] = '\0';
}

void buf_putn(struct lisp_buf *buf, const char *s, size_t n)
{
    for (size_t i = 0; i < n; i++)
        buf_putc(buf, s[i]);
}

void buf_puts(struct lisp_buf *buf, const char *s)
{
    buf_putn(buf, s, strlen(s));
}

void buf_printf(struct lisp_buf *buf, const char *fmt, ...)
{
    char tmp[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if (n <= 0)
        return;
    if ((size_t)n >= sizeof(tmp))
        n = (int)sizeof(tmp) - 1;
    buf_putn(buf, tmp, (size_t)n);
}

void heap_reset_blocks(void)
{
    free_block = 0;
}

void heap_set_reservation(void *base)
{
    reservation = base;
}

struct lisp_region *heap_current_region(void)
{
    return lisp.cur_region;
}

void heap_set_region(struct lisp_region *region)
{
    lisp.cur_region = region;
}

static void map_block(uint32_t index)
{
    if (lisp.blocks[index].mapped)
        return;
    if (mprotect(block_base(index), LISP_BLOCK_SIZE, PROT_READ | PROT_WRITE) != 0)
        lisp_error("lisp heap: cannot map block: %s", strerror(errno));
    lisp.blocks[index].mapped = 1;
}

static uint32_t take_block(void)
{
    if (free_block == UINT32_MAX)
        lisp_oom();
    uint32_t index = free_block;
    free_block = lisp.blocks[index].next;
    map_block(index);
    memset(&lisp.blocks[index], 0, sizeof(lisp.blocks[index]));
    lisp.blocks[index].in_use = 1;
    lisp.blocks[index].mapped = 1;
    lisp.heap_used += LISP_BLOCK_SIZE;
    if (lisp.heap_used > lisp.region_limit) {
        lisp.heap_used -= LISP_BLOCK_SIZE;
        lisp.blocks[index].in_use = 0;
        lisp.blocks[index].next = free_block;
        free_block = index;
        lisp_oom();
    }
    return index;
}

static void give_block(uint32_t index)
{
    struct lisp_block *b = &lisp.blocks[index];
    b->in_use = 0;
    b->owner = NULL;
    b->used = 0;
    b->kind = 0;
    b->next = free_block;
    free_block = index;
    lisp.heap_used -= LISP_BLOCK_SIZE;
}

void *heap_alloc_in(struct lisp_region *region, size_t bytes)
{
    if (bytes == 0)
        bytes = sizeof(struct lisp_obj);
    bytes = (bytes + 7u) & ~(size_t)7u;
    if (bytes > LISP_BLOCK_SIZE)
        lisp_error("lisp allocation too large");
    if (!region)
        region = lisp.cur_region;
    struct lisp_block *b = region->last != UINT32_MAX ? &lisp.blocks[region->last] : NULL;
    if (!b || b->used + bytes > LISP_BLOCK_SIZE) {
        uint32_t index = take_block();
        struct lisp_block *nb = &lisp.blocks[index];
        nb->owner = region;
        nb->kind = (uint8_t)region->kind;
        nb->next = UINT32_MAX;
        if (b)
            b->next = index;
        else
            region->first = index;
        region->last = index;
        b = nb;
    }
    void *ptr = (char *)block_base(region->last) + b->used;
    b->used += (uint32_t)bytes;
    region->used += bytes;
    if (region->used > region->high_water)
        region->high_water = region->used;
    return ptr;
}

void *heap_alloc(size_t bytes)
{
    return heap_alloc_in(lisp.cur_region, bytes);
}

void *heap_alloc_raw(size_t bytes)
{
    return heap_alloc(bytes);
}

lisp_value lisp_frame_alloc(struct lisp_frame *parent, uint32_t count)
{
    size_t size = offsetof(struct lisp_frame, entry) + (size_t)count * sizeof(struct lisp_binding);
    struct lisp_frame *frame = heap_alloc(size);
    if (!frame)
        return NIL_VAL;
    frame->h.type = LT_FRAME;
    frame->h.flags = 0;
    frame->h.aux = 0;
    frame->h.size = (uint32_t)((size + 7u) & ~(size_t)7u);
    frame->parent = parent;
    frame->count = count;
    for (uint32_t i = 0; i < count; i++) {
        frame->entry[i].sym = NIL_VAL;
        frame->entry[i].value = VAL(lisp.sym_unbound);
        frame->entry[i].kind = BIND_VAR;
    }
    return VAL(frame);
}

struct lisp_region *region_new(int kind, const char *name, uint32_t package)
{
    struct lisp_region *region = NULL;
    for (uint32_t i = 0; i < LISP_MAX_REGIONS; i++) {
        if (!lisp.regions[i].in_use) {
            region = &lisp.regions[i];
            break;
        }
    }
    if (!region)
        lisp_error("lisp heap: too many live regions");
    memset(region, 0, sizeof(*region));
    region->in_use = true;
    region->kind = kind;
    region->first = UINT32_MAX;
    region->last = UINT32_MAX;
    region->package = package;
    if (name) {
        size_t n = strlen(name);
        if (n >= sizeof(region->name))
            n = sizeof(region->name) - 1;
        memcpy(region->name, name, n);
        region->name[n] = '\0';
    }
    return region;
}

void heap_note_reclaim(uint64_t start_ns)
{
    uint64_t elapsed = lisp_now_ns() - start_ns;
    lisp.reclaim_count++;
    if (elapsed > lisp.reclaim_longest_us)
        lisp.reclaim_longest_us = elapsed;
}

static void free_region_blocks(struct lisp_region *region)
{
    uint32_t index = region->first;
    while (index != UINT32_MAX) {
        uint32_t next = lisp.blocks[index].next;
        give_block(index);
        index = next;
    }
    region->first = UINT32_MAX;
    region->last = UINT32_MAX;
    region->used = 0;
}

void region_reset(struct lisp_region *region)
{
    uint64_t start = lisp_now_ns();
    if (region->used > 0)
        heap_note_reclaim(start);
    free_region_blocks(region);
    region->high_water = 0;
}

void region_destroy(struct lisp_region *region)
{
    if (!region->in_use)
        return;
    if (region->kind == REG_GENERATION) {
        struct lisp_region **slot = &lisp.generations;
        while (*slot && *slot != region)
            slot = &(*slot)->next;
        if (*slot == region)
            *slot = region->next;
    }
    if (region->kind == REG_PERSISTENT && lisp.persistent_used >= region->used)
        lisp.persistent_used -= region->used;
    region_reset(region);
    region->in_use = false;
    region->next = NULL;
    region->pin_count = 0;
    region->dropped = false;
}

struct lisp_region *persistent_begin(void)
{
    return region_new(REG_PERSISTENT, "keep", PKG_TOMOE);
}

void persistent_end(struct lisp_region *region)
{
    lisp.persistent_used += region->used;
    if (lisp.persistent_used > lisp.persistent_high_water)
        lisp.persistent_high_water = lisp.persistent_used;
}

uint64_t heap_region_used(struct lisp_region *region)
{
    return region ? region->used : 0;
}

struct lisp_region *heap_region_of(const void *ptr)
{
    if (!reservation || (const char *)ptr < reservation)
        return NULL;
    size_t offset = (const char *)ptr - reservation;
    if (offset >= LISP_RESERVE_BYTES)
        return NULL;
    uint32_t index = (uint32_t)(offset / LISP_BLOCK_SIZE);
    if (!lisp.blocks[index].in_use)
        return NULL;
    return lisp.blocks[index].owner;
}

void heap_arena_reset(void)
{
    region_reset(&lisp.arena);
}

static lisp_value memo_lookup(void *src)
{
    for (int i = 0; i < memo_count; i++)
        if (memo_src[i] == src)
            return VAL(memo_dst[i]);
    return 0;
}

static void memo_add(void *src, void *dst)
{
    if (memo_count < (int)(sizeof(memo_src) / sizeof(memo_src[0]))) {
        memo_src[memo_count] = src;
        memo_dst[memo_count] = dst;
        memo_count++;
    }
}

static lisp_value copy_frame(struct lisp_frame *frame, struct lisp_region *dst);

static lisp_value deep_copy(lisp_value value, struct lisp_region *dst)
{
    if (!heap_region_of((void *)(uintptr_t)value))
        return value;
    switch (TYPE(value)) {
    case LT_SYMBOL:
        return value;
    case LT_CONS: {
        lisp_value hit = memo_lookup((void *)(uintptr_t)value);
        if (hit)
            return hit;
        struct lisp_cons *src = PTR(value, lisp_cons);
        struct lisp_cons *out = heap_alloc_in(dst, sizeof(*out));
        out->h.type = LT_CONS;
        out->h.flags = 0;
        out->h.aux = 0;
        out->h.size = sizeof(*out);
        out->car = NIL_VAL;
        out->cdr = NIL_VAL;
        memo_add(src, out);
        out->car = deep_copy(src->car, dst);
        out->cdr = deep_copy(src->cdr, dst);
        return VAL(out);
    }
    case LT_INT: {
        struct lisp_int *src = PTR(value, lisp_int);
        struct lisp_int *out = heap_alloc_in(dst, sizeof(*out));
        *out = *src;
        return VAL(out);
    }
    case LT_RATIO: {
        struct lisp_ratio *src = PTR(value, lisp_ratio);
        struct lisp_ratio *out = heap_alloc_in(dst, sizeof(*out));
        *out = *src;
        return VAL(out);
    }
    case LT_DOUBLE: {
        struct lisp_double *src = PTR(value, lisp_double);
        struct lisp_double *out = heap_alloc_in(dst, sizeof(*out));
        *out = *src;
        return VAL(out);
    }
    case LT_CHAR: {
        struct lisp_char *src = PTR(value, lisp_char);
        struct lisp_char *out = heap_alloc_in(dst, sizeof(*out));
        *out = *src;
        return VAL(out);
    }
    case LT_STRING: {
        struct lisp_string *src = PTR(value, lisp_string);
        if (memo_lookup(src))
            return memo_lookup(src);
        size_t size = offsetof(struct lisp_string, data) + src->byte_len + 1;
        struct lisp_string *out = heap_alloc_in(dst, size);
        out->h.type = LT_STRING;
        out->h.flags = 0;
        out->h.aux = 0;
        out->h.size = (uint32_t)((size + 7u) & ~(size_t)7u);
        out->len = src->len;
        out->byte_len = src->byte_len;
        memcpy(out->data, src->data, src->byte_len);
        out->data[src->byte_len] = '\0';
        memo_add(src, out);
        return VAL(out);
    }
    case LT_FUNCTION: {
        struct lisp_function *src = PTR(value, lisp_function);
        if (memo_lookup(src))
            return memo_lookup(src);
        struct lisp_function *out = heap_alloc_in(dst, sizeof(*out));
        *out = *src;
        memo_add(src, out);
        if (src->kind == FN_CLOSURE) {
            if (src->env)
                out->env = PTR(copy_frame(src->env, dst), lisp_frame);
            else
                out->env = NULL;
            if (src->gen) {
                src->gen->pin_count++;
                out->params = src->params;
                out->body = src->body;
            } else {
                out->params = deep_copy(src->params, dst);
                out->body = deep_copy(src->body, dst);
                out->gen = NULL;
            }
        }
        return VAL(out);
    }
    case LT_FRAME:
        return copy_frame(PTR(value, lisp_frame), dst);
    case LT_HASHTABLE: {
        struct lisp_hashtable *src = PTR(value, lisp_hashtable);
        if (memo_lookup(src))
            return memo_lookup(src);
        struct lisp_hashtable *out = heap_alloc_in(dst, sizeof(*out));
        *out = *src;
        memo_add(src, out);
        out->entries = deep_copy(src->entries, dst);
        return VAL(out);
    }
    case LT_CONDITION: {
        struct lisp_condition *src = PTR(value, lisp_condition);
        struct lisp_condition *out = heap_alloc_in(dst, sizeof(*out));
        *out = *src;
        out->message = deep_copy(src->message, dst);
        return VAL(out);
    }
    default:
        return value;
    }
}

static lisp_value copy_frame(struct lisp_frame *frame, struct lisp_region *dst)
{
    lisp_value hit = memo_lookup(frame);
    if (hit)
        return hit;
    size_t size = offsetof(struct lisp_frame, entry) + (size_t)frame->count * sizeof(struct lisp_binding);
    struct lisp_frame *out = heap_alloc_in(dst, size);
    out->h.type = LT_FRAME;
    out->h.flags = 0;
    out->h.aux = 0;
    out->h.size = (uint32_t)((size + 7u) & ~(size_t)7u);
    out->parent = NULL;
    out->count = frame->count;
    memo_add(frame, out);
    out->parent = frame->parent ? PTR(copy_frame(frame->parent, dst), lisp_frame) : NULL;
    for (uint32_t i = 0; i < frame->count; i++) {
        out->entry[i].sym = frame->entry[i].sym;
        out->entry[i].kind = frame->entry[i].kind;
        out->entry[i].value = deep_copy(frame->entry[i].value, dst);
    }
    return VAL(out);
}

lisp_value heap_deep_copy(lisp_value value, struct lisp_region *dst)
{
    int saved = memo_count;
    memo_count = 0;
    lisp_value out = deep_copy(value, dst);
    memo_count = saved;
    return out;
}

lisp_value lisp_keep(lisp_value value)
{
    struct lisp_region *region = persistent_begin();
    struct lisp_region *saved = lisp.cur_region;
    lisp.cur_region = region;
    lisp_value copy = heap_deep_copy(value, region);
    lisp.cur_region = saved;
    persistent_end(region);
    return copy;
}

static void release_pins(struct lisp_region *region)
{
    for (uint32_t index = region->first; index != UINT32_MAX; index = lisp.blocks[index].next) {
        char *base = block_base(index);
        uint32_t used = lisp.blocks[index].used;
        uint32_t off = 0;
        while (off + sizeof(struct lisp_obj) <= used) {
            struct lisp_obj *obj = (struct lisp_obj *)(base + off);
            if (obj->size < sizeof(struct lisp_obj))
                break;
            if (obj->type == LT_FUNCTION) {
                struct lisp_function *fn = (struct lisp_function *)obj;
                if (fn->kind == FN_CLOSURE && fn->gen && fn->gen->pin_count > 0)
                    fn->gen->pin_count--;
            }
            off += obj->size;
        }
    }
}

void lisp_release(lisp_value value)
{
    struct lisp_region *region = heap_region_of((void *)(uintptr_t)value);
    if (!region || region->kind != REG_PERSISTENT)
        return;
    release_pins(region);
    region_destroy(region);
    struct lisp_region *gen = lisp.generations;
    while (gen) {
        struct lisp_region *next = gen->next;
        if (gen->dropped && gen->pin_count == 0)
            region_destroy(gen);
        gen = next;
    }
}
