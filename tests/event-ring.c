#include "internal.h"
#include "budget.h"
#include <stdio.h>

static size_t allocations;
static uint64_t limits[BUDGET_COUNT];
static size_t dispatch_calls;
static size_t dispatch_events;

uint64_t budget_limit(enum budget_kind kind) {
    return limits[kind];
}

bool budget_admit(enum budget_kind kind, uint64_t amount) {
    (void)kind;
    (void)amount;
    return true;
}

void budget_release(enum budget_kind kind, uint64_t amount) {
    (void)kind;
    (void)amount;
}

extern void *__libc_malloc(size_t size);
extern void *__libc_calloc(size_t count, size_t size);
extern void *__libc_realloc(void *pointer, size_t size);

void *malloc(size_t size) {
    allocations++;
    return __libc_malloc(size);
}

void *calloc(size_t count, size_t size) {
    allocations++;
    return __libc_calloc(count, size);
}

void *realloc(void *pointer, size_t size) {
    allocations++;
    return __libc_realloc(pointer, size);
}

void tomoe_log(int level, const char *format, ...) {
    (void)level;
    (void)format;
}

void motion_drag_flush(struct tomoe *s) {
    (void)s;
}

void screens_grab_waiting(struct tomoe *s) {
    (void)s;
}

void ipc_broadcast(const char *name, lisp_value value) {
    (void)name;
    (void)value;
}

struct test_cell {
    lisp_value car, cdr;
};

static struct test_cell cells[1 << 16];
static size_t cell_count;

lisp_value lisp_nil(void) {
    return 0;
}

lisp_value lisp_t(void) {
    return 0x7000;
}

bool lisp_nilp(lisp_value value) {
    return value == 0;
}

lisp_value lisp_cons(lisp_value car, lisp_value cdr) {
    if (cell_count == sizeof(cells) / sizeof(cells[0])) abort();
    cells[cell_count] = (struct test_cell){ car, cdr };
    return (lisp_value)(cell_count++ + 1);
}

lisp_value lisp_car(lisp_value value) {
    return value ? cells[value - 1].car : 0;
}

lisp_value lisp_cdr(lisp_value value) {
    return value ? cells[value - 1].cdr : 0;
}

static char keywords[128][32];
static size_t keyword_count;

lisp_value lisp_keyword(const char *name) {
    for (size_t i = 0; i < keyword_count; i++)
        if (!strcmp(keywords[i], name)) return 0x1000 + (lisp_value)i;
    if (keyword_count == sizeof(keywords) / sizeof(keywords[0])) abort();
    snprintf(keywords[keyword_count], sizeof(keywords[0]), "%s", name);
    return 0x1000 + (lisp_value)keyword_count++;
}

lisp_value lisp_cstring(const char *text) {
    return text ? 0x8000 + strlen(text) : 0;
}

lisp_value lisp_string(const char *text, size_t length) {
    (void)text;
    return 0x9000 + (lisp_value)length;
}

lisp_value lisp_int(int64_t value) {
    return 0x2000 + (lisp_value)(value & 0xff);
}

lisp_value lisp_uint(uint64_t value) {
    return 0x3000 + (lisp_value)(value & 0xff);
}

lisp_value lisp_double(double value) {
    return 0x4000 + (lisp_value)value;
}

lisp_value lisp_bool(bool value) {
    return value ? 0x5000 : 0;
}

lisp_value lisp_list(int count, ...) {
    va_list args;
    va_start(args, count);
    lisp_value list = lisp_nil();
    for (int i = 0; i < count; i++) list = lisp_cons(va_arg(args, lisp_value), list);
    va_end(args);
    return list;
}

lisp_value lisp_nreverse(lisp_value list) {
    lisp_value result = lisp_nil();
    for (lisp_value tail = list; lisp_nilp(tail) == false; tail = lisp_cdr(tail))
        result = lisp_cons(lisp_car(tail), result);
    return result;
}

static size_t list_length(lisp_value list) {
    size_t length = 0;
    for (lisp_value tail = list; !lisp_nilp(tail); tail = lisp_cdr(tail)) length++;
    return length;
}

int lisp_call(const char *function, int argc, const lisp_value *argv, lisp_value *result,
        char *error, size_t error_size) {
    (void)function;
    (void)error;
    (void)error_size;
    dispatch_calls++;
    dispatch_events = argc > 0 ? list_length(argv[0]) : 0;
    if (result) *result = lisp_nil();
    return 0;
}

void lisp_set_limits(uint64_t steps, uint64_t deadline_ns) {
    (void)steps;
    (void)deadline_ns;
}

static struct tomoe server;

static void reset(void) {
    free(server.event_ring);
    free(server.event_arena);
    memset(&server, 0, sizeof(server));
    server.event_read_mask = UINT64_MAX;
    cell_count = 0;
    keyword_count = 0;
    dispatch_calls = dispatch_events = 0;
    limits[BUDGET_EVENTS] = 16;
}

static size_t failures;

static void check(bool condition, const char *what) {
    if (condition) return;
    fprintf(stderr, "event-ring: FAIL %s\n", what);
    failures++;
}

static void ring_capacity_and_drop(void) {
    reset();
    for (int i = 0; i < 16; i++)
        check(policy_event_push(&server, EV_KEY) != NULL, "key queued");
    check(server.event_count == 16, "ring filled");
    size_t before = allocations;
    check(policy_event_push(&server, EV_KEY) == NULL, "full ring refuses");
    check(allocations == before, "refusal allocates nothing");
    check(server.event_count == 16, "refusal keeps count");
    check(policy_events_dropped(&server) == 1, "refusal counted");
}

static void ring_drops_optional(void) {
    reset();
    for (int i = 0; i < 8; i++) policy_event_push(&server, EV_KEY);
    for (int i = 0; i < 8; i++)
        policy_event_push(&server, EV_POINTER)->id = 100 + (uint32_t)i;
    size_t before = allocations;
    struct tomoe_event *pushed = policy_event_push(&server, EV_POINTER);
    check(pushed != NULL, "optional replaces");
    if (pushed) pushed->id = 200;
    check(allocations == before, "replacement allocates nothing");
    check(server.event_count == 16, "capacity kept");
    check(policy_events_dropped(&server) == 1, "replacement counted");
    check(server.event_ring[0].type == EV_KEY, "first key kept");
    check(server.event_ring[8].id == 101, "oldest pointer dropped");
    check(server.event_ring[15].id == 200, "new pointer last");
}

static void ring_protects_facts(void) {
    reset();
    for (int i = 0; i < 16; i++) check(policy_event_push(&server, EV_MAP) != NULL, "fact queued");
    check(policy_event_push(&server, EV_KEY) == NULL, "incoming dropped");
    check(server.event_count == 16, "facts kept");
    check(policy_events_dropped(&server) == 1, "drop counted");
}

static void ring_respects_read_mask(void) {
    reset();
    server.event_read_mask = 0;
    check(policy_event_push(&server, EV_POINTER) == NULL, "unread pointer skipped");
    check(policy_event_push(&server, EV_MAP) != NULL, "fact always queued");
    server.event_read_mask = UINT64_MAX;
    check(policy_event_push(&server, EV_POINTER) != NULL, "read pointer queued");
}

static void grab_coalesces(void) {
    reset();
    queue_grab_motion(&server, 7, 1, 10, 20, 1, 2);
    size_t before = allocations;
    queue_grab_motion(&server, 7, 1, 11, 21, 2, 3);
    check(allocations == before, "coalescing allocates nothing");
    check(server.event_count == 1, "grab coalesced");
    check(server.event_ring[0].type == EV_GRAB, "grab kept");
    check(server.event_ring[0].y == 11, "grab x updated");
    check(server.event_ring[0].dx == 21, "grab y updated");
    check((int)server.event_ring[0].fx == 3, "grab dx accumulated");
    check((int)server.event_ring[0].fy == 5, "grab dy accumulated");
    queue_grab_motion(&server, 8, 1, 1, 1, 0, 0);
    check(server.event_count == 2, "other grab queued");
}

static void arena_round_trip(void) {
    reset();
    policy_event_push(&server, EV_KEY);
    uint32_t handle = policy_event_cstring(&server, "hello");
    size_t length = 0;
    const char *text = policy_event_string_at(&server, handle, &length);
    check(text && length == 5 && memcmp(text, "hello", 5) == 0, "string stored");
    size_t before = allocations;
    for (int i = 0; i < 16; i++) policy_event_cstring(&server, "x");
    check(allocations == before, "arena reuses its block");
    check(policy_event_string_at(&server, EVENT_NO_STRING, &length) == NULL, "no handle");
}

static void drain_empties(void) {
    reset();
    policy_event_push(&server, EV_KEY);
    policy_event_push(&server, EV_MAP);
    policy_event_cstring(&server, "title");
    check(policy_events_pending(&server), "events pending");
    policy_drain(&server);
    check(server.event_count == 0, "drain empties ring");
    check(server.event_arena_used == 0, "drain resets arena");
    check(dispatch_calls == 1, "one dispatch call");
    check(dispatch_events == 2, "both events dispatched");
    check(policy_events_pending(&server) == false, "nothing pending");
    policy_drain(&server);
    check(dispatch_calls == 1, "empty drain dispatches nothing");
}

static void drain_holds_after_grab(void) {
    reset();
    server.display = wl_display_create();
    check(server.display != NULL, "display created");
    queue_grab_motion(&server, 3, 2, 4, 5, 0, 0);
    policy_event_push(&server, EV_KEY);
    policy_drain(&server);
    check(dispatch_events == 1, "grab dispatched alone");
    check(server.event_count == 1, "later event held");
    check(server.grab_held, "grab hold armed");
    server.grab_held = false;
    wl_display_destroy(server.display);
    server.display = NULL;
}

int main(void) {
    ring_capacity_and_drop();
    ring_drops_optional();
    ring_protects_facts();
    ring_respects_read_mask();
    grab_coalesces();
    arena_round_trip();
    drain_empties();
    drain_holds_after_grab();
    free(server.event_ring);
    free(server.event_arena);
    if (failures) return 1;
    printf("event-ring: ok\n");
    return 0;
}
