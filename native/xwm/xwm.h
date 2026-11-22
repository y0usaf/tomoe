#ifndef TOMOE_XWM_SIDECAR_H
#define TOMOE_XWM_SIDECAR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/epoll.h>
#include "xwm_proto.h"

struct xlist {
    struct xlist *prev;
    struct xlist *next;
};

static inline void xlist_init(struct xlist *l) {
    l->prev = l;
    l->next = l;
}

static inline bool xlist_empty(const struct xlist *l) {
    return l->next == l;
}

static inline void xlist_insert(struct xlist *l, struct xlist *elm) {
    elm->prev = l;
    elm->next = l->next;
    l->next = elm;
    elm->next->prev = elm;
}

static inline void xlist_remove(struct xlist *elm) {
    elm->prev->next = elm->next;
    elm->next->prev = elm->prev;
    elm->prev = elm;
    elm->next = elm;
}

#define xcontainer_of(ptr, sample, member) \
    ((__typeof__(sample))((char *)(ptr) - offsetof(__typeof__(*sample), member)))

#define xlist_for_each(pos, head, member) \
    for (pos = xcontainer_of((head)->next, pos, member); \
         &pos->member != (head); \
         pos = xcontainer_of(pos->member.next, pos, member))

#define xlist_for_each_safe(pos, tmp, head, member) \
    for (pos = xcontainer_of((head)->next, pos, member), \
         tmp = xcontainer_of(pos->member.next, tmp, member); \
         &pos->member != (head); \
         pos = tmp, tmp = xcontainer_of(pos->member.next, tmp, member))

typedef void (*xev_fn)(int fd, uint32_t events, void *data);

struct xev_source {
    int fd;
    xev_fn fn;
    void *data;
    bool removed;
    struct xlist link;
};

struct xwm_sidecar;
struct xwm;

struct xev_source *xev_add(struct xwm_sidecar *sc, int fd, uint32_t events, xev_fn fn, void *data);
void xev_mod(struct xwm_sidecar *sc, struct xev_source *src, uint32_t events);
void xev_del(struct xwm_sidecar *sc, struct xev_source *src);

bool sidecar_send(struct xwm_sidecar *sc, uint16_t type, const void *payload, uint32_t len, int fd);
void sidecar_wm_failed(struct xwm_sidecar *sc);

struct xwm *xwm_create(struct xwm_sidecar *sc, int wmfd);
void xwm_destroy(struct xwm *x);
void xwm_handle_cmd(struct xwm *x, const struct xwm_hdr *hdr, const uint8_t *payload, int fd);

#endif
