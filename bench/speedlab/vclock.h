#pragma once

#include <stdatomic.h>
#include <stdint.h>

#define VCLOCK_PATH "/run/speedlab/clock"
#define VCLOCK_MAGIC 0x6b6c6376u
#define VCLOCK_SLOTS 256
#define VCLOCK_NS 1000000000LL

enum { VCLOCK_SYSCALL = 1 };

struct vclock_slot {
    _Atomic int32_t pid;
    int32_t violation_kind;
    _Atomic int64_t next;
    _Atomic uint32_t done;
    int32_t violation_number;
    _Atomic uint64_t violations;
    uint64_t heap_arena;
    uint64_t heap_used;
    uint64_t heap_free;
    uint64_t heap_mmap;
    char violation_where[64];
    _Atomic uint32_t settle_req;
    _Atomic uint32_t settle_ack;
    _Atomic uint32_t settles;
    uint32_t pad[13];
};

struct vclock_shm {
    _Atomic uint32_t gen;
    uint32_t magic;
    _Atomic int64_t now;
    int64_t epoch;
    _Atomic uint32_t probe;
    uint32_t pad;
    char reserved[32];
    struct vclock_slot slot[VCLOCK_SLOTS];
};

_Static_assert(sizeof(struct vclock_slot) == 192, "slot layout is shared with the driver");
_Static_assert(sizeof(struct vclock_shm) == 64 + 192 * VCLOCK_SLOTS, "clock layout is shared with the driver");
