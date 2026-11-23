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

pv tomoe_memory_report(struct tomoe *s) {
    uint64_t rss, pss, anon, file;
    rollup(&rss, &pss, &anon, &file);
    struct mallinfo2 heap = mallinfo2();
    size_t textures, images;
    uint64_t texture_bytes;
    render_usage(s->renderer, &textures, &texture_bytes, &images);
    pv outputs = pv_nil();
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
        outputs = pv_cons(pv_list(6, pv_keyword("NAME"), pv_cstring(o->screen->name),
            pv_keyword("BUFFERS"), pv_uint(buffers), pv_keyword("BUFFER-BYTES"),
            pv_uint(bytes)), outputs);
    }
    pv gpu = pv_list(10, pv_keyword("TEXTURES"), pv_uint(textures),
        pv_keyword("TEXTURE-BYTES"), pv_uint(texture_bytes),
        pv_keyword("IMAGES"), pv_uint(images),
        pv_keyword("BUFFER-BYTES"), pv_uint(buffer_total),
        pv_keyword("OUTPUTS"), outputs);
    pv region = pv_nil();
    pv tail = pv_list(16,
        pv_keyword("POLICY-GENERATION"), pv_uint(policy_link_generation(s)),
        pv_keyword("GPU"), gpu,
        pv_keyword("LISP"), region,
        pv_keyword("BUDGETS"), budget_report(),
        pv_keyword("HOT-ALLOCS"), pv_uint(budget_hot_allocs()),
        pv_keyword("HOT-ALLOCS-ALL"), pv_uint(budget_hot_allocs_all()),
        pv_keyword("EVENTS-DROPPED"), pv_uint(policy_events_dropped(s)),
        pv_keyword("SURFACES"), pv_int(wl_list_length(&s->surfaces)));
    pv head = pv_list(12,
        pv_keyword("RSS-BYTES"), pv_uint(rss),
        pv_keyword("PSS-BYTES"), pv_uint(pss),
        pv_keyword("ANON-BYTES"), pv_uint(anon),
        pv_keyword("FILE-BYTES"), pv_uint(file),
        pv_keyword("MALLOC-HEAP-USED"), pv_uint(heap.uordblks + heap.hblkhd),
        pv_keyword("MALLOC-HEAP-HELD"), pv_uint(heap.arena + heap.hblkhd));
    pv result = pv_nil();
    for (pv p = head; !pv_nilp(p); p = pv_cdr(p)) result = pv_cons(pv_car(p), result);
    for (pv p = tail; !pv_nilp(p); p = pv_cdr(p)) result = pv_cons(pv_car(p), result);
    return pv_nreverse(result);
}
