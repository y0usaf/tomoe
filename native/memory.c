#include "internal.h"
#include "budget.h"
#include "policy.h"
#include <malloc.h>
#include <unistd.h>

static void rollup(uint64_t *rss, uint64_t *pss, uint64_t *anon, uint64_t *file) {
    *rss = *pss = *anon = *file = 0;
    FILE *in = fopen("/proc/self/smaps_rollup", "re");
    if (in) {
        char line[256];
        unsigned long long kb;
        while (fgets(line, sizeof(line), in)) {
            if (sscanf(line, "Rss: %llu", &kb) == 1) *rss = kb * 1024;
            else if (sscanf(line, "Pss: %llu", &kb) == 1) *pss = kb * 1024;
            else if (sscanf(line, "Anonymous: %llu", &kb) == 1) *anon = kb * 1024;
        }
        fclose(in);
        *file = *rss > *anon ? *rss - *anon : 0;
        return;
    }
    in = fopen("/proc/self/statm", "re");
    if (!in) return;
    unsigned long long size, resident, shared;
    if (fscanf(in, "%llu %llu %llu", &size, &resident, &shared) == 3) {
        long page = sysconf(_SC_PAGESIZE);
        *rss = resident * page;
        *pss = *rss;
        *file = shared * page;
        *anon = *rss > *file ? *rss - *file : 0;
    }
    fclose(in);
}

lisp_value tomoe_memory_report(struct tomoe *s) {
    uint64_t rss, pss, anon, file;
    rollup(&rss, &pss, &anon, &file);
    struct mallinfo2 heap = mallinfo2();
    size_t textures, images;
    uint64_t texture_bytes;
    render_usage(s->renderer, &textures, &texture_bytes, &images);
    lisp_value outputs = lisp_nil();
    uint64_t buffer_total = 0;
    struct output *o;
    wl_list_for_each_reverse(o, &s->outputs, link) {
        size_t buffers = 0;
        uint64_t bytes = 0;
        for (size_t i = 0; i < RING_SLOTS; i++) {
            struct dmabuf_attributes dmabuf;
            if (!o->ring.slots[i] || !buffer_get_dmabuf(o->ring.slots[i], &dmabuf)) continue;
            buffers++;
            for (int plane = 0; plane < dmabuf.n_planes; plane++)
                bytes += (uint64_t)dmabuf.stride[plane] * dmabuf.height;
        }
        buffer_total += bytes;
        outputs = lisp_cons(lisp_list(6, lisp_keyword("NAME"), lisp_cstring(o->screen->name),
            lisp_keyword("BUFFERS"), lisp_uint(buffers), lisp_keyword("BUFFER-BYTES"),
            lisp_uint(bytes)), outputs);
    }
    struct lisp_heap_stats lisp;
    lisp_heap_stats(&lisp);
    lisp_value gpu = lisp_list(10, lisp_keyword("TEXTURES"), lisp_uint(textures),
        lisp_keyword("TEXTURE-BYTES"), lisp_uint(texture_bytes),
        lisp_keyword("IMAGES"), lisp_uint(images),
        lisp_keyword("BUFFER-BYTES"), lisp_uint(buffer_total),
        lisp_keyword("OUTPUTS"), outputs);
    lisp_value region = lisp_list(18, lisp_keyword("REGION-BYTES"), lisp_uint(lisp.region_bytes),
        lisp_keyword("PERSISTENT-USED"), lisp_uint(lisp.persistent_used),
        lisp_keyword("PERSISTENT-HIGH-WATER"), lisp_uint(lisp.persistent_high_water),
        lisp_keyword("ARENA-USED"), lisp_uint(lisp.arena_used),
        lisp_keyword("ARENA-HIGH-WATER"), lisp_uint(lisp.arena_high_water),
        lisp_keyword("GENERATIONS"), lisp_uint(lisp.generations),
        lisp_keyword("RECLAIM-COUNT"), lisp_uint(lisp.reclaim_count),
        lisp_keyword("RECLAIM-LONGEST-US"), lisp_uint(lisp.reclaim_longest_us),
        lisp_keyword("BUDGET-FAILURES"), lisp_uint(lisp.budget_failures));
    lisp_value tail = lisp_list(14,
        lisp_keyword("GPU"), gpu,
        lisp_keyword("LISP"), region,
        lisp_keyword("BUDGETS"), budget_report(),
        lisp_keyword("HOT-ALLOCS"), lisp_uint(budget_hot_allocs()),
        lisp_keyword("HOT-ALLOCS-ALL"), lisp_uint(budget_hot_allocs_all()),
        lisp_keyword("EVENTS-DROPPED"), lisp_uint(policy_events_dropped(s)),
        lisp_keyword("SURFACES"), lisp_int(wl_list_length(&s->surfaces)));
    lisp_value head = lisp_list(12,
        lisp_keyword("RSS-BYTES"), lisp_uint(rss),
        lisp_keyword("PSS-BYTES"), lisp_uint(pss),
        lisp_keyword("ANON-BYTES"), lisp_uint(anon),
        lisp_keyword("FILE-BYTES"), lisp_uint(file),
        lisp_keyword("MALLOC-HEAP-USED"), lisp_uint(heap.uordblks + heap.hblkhd),
        lisp_keyword("MALLOC-HEAP-HELD"), lisp_uint(heap.arena + heap.hblkhd));
    lisp_value result = lisp_nil();
    for (lisp_value p = head; !lisp_nilp(p); p = lisp_cdr(p)) result = lisp_cons(lisp_car(p), result);
    for (lisp_value p = tail; !lisp_nilp(p); p = lisp_cdr(p)) result = lisp_cons(lisp_car(p), result);
    return lisp_nreverse(result);
}
