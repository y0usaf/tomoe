/* SPDX-License-Identifier: LGPL-2.1-only OR MPL-1.1; derived from cairo 1.18.4 cairo-polygon-intersect.c, see NOTICE */
#include "intersect.h"

#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

enum {
    BO_EXCESS = -1,
    BO_EXACT = 0,
    BO_DEFAULT = 1
};

struct bo_ordinate {
    int32_t ordinate;
    int approx;
};

struct bo_point {
    struct bo_ordinate x, y;
};

struct bo_edge {
    int a_or_b;
    struct dp_edge edge;
    struct bo_edge *prev;
    struct bo_edge *next;
    struct {
        struct bo_edge *other;
        int32_t top;
    } deferred;
};

enum {
    BO_STOP = -1,
    BO_INTERSECTION = 0,
    BO_START = 1
};

struct bo_event {
    int type;
    struct bo_point point;
};

struct bo_start_event {
    int type;
    struct bo_point point;
    struct bo_edge edge;
};

struct bo_queue_event {
    int type;
    struct bo_point point;
    struct bo_edge *e1, *e2;
};

#define PQ_PARENT_INDEX(i) ((i) >> 1)
#define PQ_FIRST_ENTRY 1
#define PQ_LEFT_CHILD_INDEX(i) ((i) << 1)

struct pqueue {
    int size, max_size;
    struct bo_event **elements;
};

struct bo_event_queue {
    struct pqueue pqueue;
    struct bo_event **start_events;
};

struct bo_sweep_line {
    struct bo_edge *head;
    int32_t current_y;
    struct bo_edge *current_edge;
};

static int32_t line_compute_intersection_x_for_y(const struct dp_edge *edge, int32_t y)
{
    int32_t x, dy;

    if (y == edge->p1.y) return edge->p1.x;
    if (y == edge->p2.y) return edge->p2.x;

    x = edge->p1.x;
    dy = edge->p2.y - edge->p1.y;
    if (dy != 0)
        x += (int32_t)(((int64_t)(y - edge->p1.y) *
            (int64_t)(edge->p2.x - edge->p1.x)) / dy);

    return x;
}

static int bo_point32_compare(const struct bo_point *a, const struct bo_point *b)
{
    int cmp = a->y.ordinate - b->y.ordinate;
    if (cmp) return cmp;
    cmp = a->y.approx - b->y.approx;
    if (cmp) return cmp;
    return a->x.ordinate - b->x.ordinate;
}

static int slope_compare(const struct bo_edge *a, const struct bo_edge *b)
{
    int32_t adx = a->edge.p2.x - a->edge.p1.x;
    int32_t bdx = b->edge.p2.x - b->edge.p1.x;

    if (adx == 0) return -bdx;
    if (bdx == 0) return adx;
    if ((adx ^ bdx) < 0) return adx;

    {
        int32_t ady = a->edge.p2.y - a->edge.p1.y;
        int32_t bdy = b->edge.p2.y - b->edge.p1.y;
        int64_t adx_bdy = (int64_t)adx * bdy;
        int64_t bdx_ady = (int64_t)bdx * ady;
        return adx_bdy == bdx_ady ? 0 : adx_bdy < bdx_ady ? -1 : 1;
    }
}

static int edges_compare_x_for_y_general(const struct bo_edge *a,
    const struct bo_edge *b, int32_t y)
{
    int32_t dx;
    int32_t adx, ady;
    int32_t bdx, bdy;
    enum {
        HAVE_NONE = 0x0,
        HAVE_DX = 0x1,
        HAVE_ADX = 0x2,
        HAVE_DX_ADX = HAVE_DX | HAVE_ADX,
        HAVE_BDX = 0x4,
        HAVE_DX_BDX = HAVE_DX | HAVE_BDX,
        HAVE_ADX_BDX = HAVE_ADX | HAVE_BDX,
        HAVE_ALL = HAVE_DX | HAVE_ADX | HAVE_BDX
    } have_dx_adx_bdx = HAVE_ALL;

    {
        int32_t amin, amax, bmin, bmax;
        if (a->edge.p1.x < a->edge.p2.x) {
            amin = a->edge.p1.x;
            amax = a->edge.p2.x;
        } else {
            amin = a->edge.p2.x;
            amax = a->edge.p1.x;
        }
        if (b->edge.p1.x < b->edge.p2.x) {
            bmin = b->edge.p1.x;
            bmax = b->edge.p2.x;
        } else {
            bmin = b->edge.p2.x;
            bmax = b->edge.p1.x;
        }
        if (amax < bmin) return -1;
        if (amin > bmax) return +1;
    }

    ady = a->edge.p2.y - a->edge.p1.y;
    adx = a->edge.p2.x - a->edge.p1.x;
    if (adx == 0) have_dx_adx_bdx &= ~HAVE_ADX;

    bdy = b->edge.p2.y - b->edge.p1.y;
    bdx = b->edge.p2.x - b->edge.p1.x;
    if (bdx == 0) have_dx_adx_bdx &= ~HAVE_BDX;

    dx = a->edge.p1.x - b->edge.p1.x;
    if (dx == 0) have_dx_adx_bdx &= ~HAVE_DX;

#define L ((__int128)((int64_t)ady * bdy) * dx)
#define A ((__int128)((int64_t)adx * bdy) * (y - a->edge.p1.y))
#define B ((__int128)((int64_t)bdx * ady) * (y - b->edge.p1.y))
    switch (have_dx_adx_bdx) {
    default:
    case HAVE_NONE:
        return 0;
    case HAVE_DX:
        return dx;
    case HAVE_ADX:
        return adx;
    case HAVE_BDX:
        return -bdx;
    case HAVE_ADX_BDX:
        if ((adx ^ bdx) < 0) {
            return adx;
        } else if (a->edge.p1.y == b->edge.p1.y) {
            int64_t adx_bdy = (int64_t)adx * bdy;
            int64_t bdx_ady = (int64_t)bdx * ady;
            return adx_bdy == bdx_ady ? 0 : adx_bdy < bdx_ady ? -1 : 1;
        } else {
            __int128 l = A, r = B;
            return l == r ? 0 : l < r ? -1 : 1;
        }
    case HAVE_DX_ADX:
        if ((-adx ^ dx) < 0) {
            return dx;
        } else {
            int64_t ady_dx = (int64_t)ady * dx;
            int64_t dy_adx = (int64_t)(a->edge.p1.y - y) * adx;
            return ady_dx == dy_adx ? 0 : ady_dx < dy_adx ? -1 : 1;
        }
    case HAVE_DX_BDX:
        if ((bdx ^ dx) < 0) {
            return dx;
        } else {
            int64_t bdy_dx = (int64_t)bdy * dx;
            int64_t dy_bdx = (int64_t)(y - b->edge.p1.y) * bdx;
            return bdy_dx == dy_bdx ? 0 : bdy_dx < dy_bdx ? -1 : 1;
        }
    case HAVE_ALL: {
        __int128 l = L, r = B - A;
        return l == r ? 0 : l < r ? -1 : 1;
    }
    }
#undef B
#undef A
#undef L
}

static int edge_compare_for_y_against_x(const struct bo_edge *a, int32_t y, int32_t x)
{
    int32_t adx, ady;
    int32_t dx, dy;
    int64_t L, R;

    if (x < a->edge.p1.x && x < a->edge.p2.x) return 1;
    if (x > a->edge.p1.x && x > a->edge.p2.x) return -1;

    adx = a->edge.p2.x - a->edge.p1.x;
    dx = x - a->edge.p1.x;

    if (adx == 0) return -dx;
    if (dx == 0 || (adx ^ dx) < 0) return adx;

    dy = y - a->edge.p1.y;
    ady = a->edge.p2.y - a->edge.p1.y;

    L = (int64_t)dy * adx;
    R = (int64_t)dx * ady;

    return L == R ? 0 : L < R ? -1 : 1;
}

static int edges_compare_x_for_y(const struct bo_edge *a, const struct bo_edge *b,
    int32_t y)
{
    enum {
        HAVE_NEITHER = 0x0,
        HAVE_AX = 0x1,
        HAVE_BX = 0x2,
        HAVE_BOTH = HAVE_AX | HAVE_BX
    } have_ax_bx = HAVE_BOTH;
    int32_t ax = 0, bx = 0;

    if (y == a->edge.p1.y)
        ax = a->edge.p1.x;
    else if (y == a->edge.p2.y)
        ax = a->edge.p2.x;
    else
        have_ax_bx &= ~HAVE_AX;

    if (y == b->edge.p1.y)
        bx = b->edge.p1.x;
    else if (y == b->edge.p2.y)
        bx = b->edge.p2.x;
    else
        have_ax_bx &= ~HAVE_BX;

    switch (have_ax_bx) {
    default:
    case HAVE_NEITHER:
        return edges_compare_x_for_y_general(a, b, y);
    case HAVE_AX:
        return -edge_compare_for_y_against_x(b, y, ax);
    case HAVE_BX:
        return edge_compare_for_y_against_x(a, y, bx);
    case HAVE_BOTH:
        return ax - bx;
    }
}

static int line_equal(const struct dp_edge *a, const struct dp_edge *b)
{
    return a->p1.x == b->p1.x && a->p1.y == b->p1.y &&
        a->p2.x == b->p2.x && a->p2.y == b->p2.y;
}

static int sweep_line_compare_edges(struct bo_sweep_line *sweep_line,
    const struct bo_edge *a, const struct bo_edge *b)
{
    int cmp;

    if (!line_equal(&a->edge, &b->edge)) {
        cmp = edges_compare_x_for_y(a, b, sweep_line->current_y);
        if (cmp) return cmp;

        cmp = slope_compare(a, b);
        if (cmp) return cmp;
    }

    return b->edge.bottom - a->edge.bottom;
}

static int64_t det32_64(int32_t a, int32_t b, int32_t c, int32_t d)
{
    return (int64_t)a * d - (int64_t)b * c;
}

static __int128 det64x32_128(int64_t a, int32_t b, int64_t c, int32_t d)
{
    return (__int128)a * d - (__int128)c * b;
}

static struct bo_ordinate round_to_nearest(int64_t quo, int64_t rem, int64_t den)
{
    struct bo_ordinate ordinate;
    int32_t q = (int32_t)quo;
    int64_t rem_2 = rem * 2;

    if (rem_2 < -den) {
        q -= 1;
        rem_2 = -rem_2;
    } else if (den <= rem_2) {
        q += 1;
        rem_2 = -rem_2;
    }

    ordinate.ordinate = q;
    ordinate.approx = rem_2 == 0 ? BO_EXACT : rem_2 < 0 ? BO_EXCESS : BO_DEFAULT;
    return ordinate;
}

static int int_96by64_divrem(__int128 num, int64_t den, int64_t *quo, int64_t *rem)
{
    int num_neg = num < 0;
    int den_neg = den < 0;
    uint64_t d = den_neg ? (uint64_t)0 - (uint64_t)den : (uint64_t)den;
    unsigned __int128 n = num_neg ? (unsigned __int128)0 - (unsigned __int128)num :
        (unsigned __int128)num;

    if (d == 0) return 0;
    if ((n >> 32) >= d) return 0;

    uint64_t q = (uint64_t)(n / d);
    uint64_t r = (uint64_t)(n % d);

    *rem = (int64_t)(num_neg ? (unsigned __int128)0 - r : r);
    *quo = (int64_t)((num_neg != den_neg) ? (unsigned __int128)0 - q : q);
    return 1;
}

static int intersect_lines(struct bo_edge *a, struct bo_edge *b,
    struct bo_point *intersection)
{
    int64_t a_det, b_det;
    int32_t dx1 = a->edge.p1.x - a->edge.p2.x;
    int32_t dy1 = a->edge.p1.y - a->edge.p2.y;
    int32_t dx2 = b->edge.p1.x - b->edge.p2.x;
    int32_t dy2 = b->edge.p1.y - b->edge.p2.y;
    int64_t den_det;
    int64_t R;
    int64_t quo, rem;

    den_det = det32_64(dx1, dy1, dx2, dy2);

    R = det32_64(dx2, dy2, b->edge.p1.x - a->edge.p1.x, b->edge.p1.y - a->edge.p1.y);
    if (den_det <= R) return 0;

    R = det32_64(dy1, dx1, a->edge.p1.y - b->edge.p1.y, a->edge.p1.x - b->edge.p1.x);
    if (den_det <= R) return 0;

    a_det = det32_64(a->edge.p1.x, a->edge.p1.y, a->edge.p2.x, a->edge.p2.y);
    b_det = det32_64(b->edge.p1.x, b->edge.p1.y, b->edge.p2.x, b->edge.p2.y);

    if (!int_96by64_divrem(det64x32_128(a_det, dx1, b_det, dx2), den_det, &quo, &rem))
        return 0;
    if (rem == den_det) return 0;

    intersection->x = round_to_nearest(quo, rem, den_det);

    if (!int_96by64_divrem(det64x32_128(a_det, dy1, b_det, dy2), den_det, &quo, &rem))
        return 0;
    if (rem == den_det) return 0;

    intersection->y = round_to_nearest(quo, rem, den_det);

    return 1;
}

static int bo_intersect_ordinate_32_compare(struct bo_ordinate a, int32_t b)
{
    if (a.ordinate > b) return +1;
    if (a.ordinate < b) return -1;
    return a.approx;
}

static int bo_edge_contains_intersect_point(struct bo_edge *edge,
    struct bo_point *point)
{
    return bo_intersect_ordinate_32_compare(point->y, edge->edge.bottom) < 0;
}

static int bo_edge_intersect(struct bo_edge *a, struct bo_edge *b,
    struct bo_point *intersection)
{
    if (!intersect_lines(a, b, intersection)) return 0;
    if (!bo_edge_contains_intersect_point(a, intersection)) return 0;
    if (!bo_edge_contains_intersect_point(b, intersection)) return 0;
    return 1;
}

static int bo_event_compare(const struct bo_event *a, const struct bo_event *b)
{
    int cmp = bo_point32_compare(&a->point, &b->point);
    if (cmp) return cmp;
    cmp = a->type - b->type;
    if (cmp) return cmp;
    return a < b ? -1 : a == b ? 0 : 1;
}

static void pqueue_init(struct pqueue *pq)
{
    pq->max_size = 64;
    pq->size = 0;
    pq->elements = calloc((size_t)pq->max_size, sizeof(*pq->elements));
}

static void pqueue_fini(struct pqueue *pq)
{
    free(pq->elements);
}

static int pqueue_grow(struct pqueue *pq)
{
    int max = pq->max_size * 2;
    struct bo_event **elements = realloc(pq->elements, (size_t)max * sizeof(*elements));
    if (!elements) return 0;
    pq->elements = elements;
    pq->max_size = max;
    return 1;
}

static int pqueue_push(struct pqueue *pq, struct bo_event *event)
{
    struct bo_event **elements;
    int i, parent;

    if (!pq->elements) {
        pqueue_init(pq);
        if (!pq->elements) return 0;
    }
    if (pq->size + 1 == pq->max_size && !pqueue_grow(pq)) return 0;

    elements = pq->elements;

    for (i = ++pq->size;
         i != PQ_FIRST_ENTRY &&
         bo_event_compare(event, elements[parent = PQ_PARENT_INDEX(i)]) < 0;
         i = parent) {
        elements[i] = elements[parent];
    }

    elements[i] = event;
    return 1;
}

static void pqueue_pop(struct pqueue *pq)
{
    struct bo_event **elements = pq->elements;
    struct bo_event *tail;
    int child, i;

    tail = elements[pq->size--];
    if (pq->size == 0) {
        elements[PQ_FIRST_ENTRY] = NULL;
        return;
    }

    for (i = PQ_FIRST_ENTRY;
         (child = PQ_LEFT_CHILD_INDEX(i)) <= pq->size;
         i = child) {
        if (child != pq->size && bo_event_compare(elements[child + 1], elements[child]) < 0)
            child++;
        if (bo_event_compare(elements[child], tail) >= 0)
            break;
        elements[i] = elements[child];
    }
    elements[i] = tail;
}

static int bo_event_queue_insert(struct bo_event_queue *queue, int type,
    struct bo_edge *e1, struct bo_edge *e2, const struct bo_point *point)
{
    struct bo_queue_event *event = malloc(sizeof(*event));
    if (!event) return 0;

    event->type = type;
    event->e1 = e1;
    event->e2 = e2;
    event->point = *point;

    if (!pqueue_push(&queue->pqueue, (struct bo_event *)event)) {
        free(event);
        return 0;
    }
    return 1;
}

static void bo_event_queue_delete(struct bo_event *event)
{
    free(event);
}

static struct bo_event *bo_event_dequeue(struct bo_event_queue *event_queue)
{
    struct bo_event *event, *cmp;

    event = event_queue->pqueue.elements[PQ_FIRST_ENTRY];
    cmp = *event_queue->start_events;
    if (event == NULL || (cmp != NULL && bo_event_compare(cmp, event) < 0)) {
        event = cmp;
        event_queue->start_events++;
    } else {
        pqueue_pop(&event_queue->pqueue);
    }

    return event;
}

static unsigned combsort_newgap(unsigned gap)
{
    gap = 10 * gap / 13;
    if (gap == 9 || gap == 10) gap = 11;
    if (gap < 1) gap = 1;
    return gap;
}

static void bo_event_queue_sort(struct bo_event **base, unsigned nmemb)
{
    unsigned gap = nmemb;
    unsigned i, j;
    int swapped;

    do {
        gap = combsort_newgap(gap);
        swapped = gap > 1;
        for (i = 0; i < nmemb - gap; i++) {
            j = i + gap;
            if (bo_event_compare(base[i], base[j]) > 0) {
                struct bo_event *tmp = base[i];
                base[i] = base[j];
                base[j] = tmp;
                swapped = 1;
            }
        }
    } while (swapped);
}

static int bo_event_queue_init(struct bo_event_queue *event_queue,
    struct bo_event **start_events, int num_events)
{
    bo_event_queue_sort(start_events, (unsigned)num_events);
    start_events[num_events] = NULL;

    event_queue->start_events = start_events;
    pqueue_init(&event_queue->pqueue);
    if (!event_queue->pqueue.elements) return 0;
    return 1;
}

static int event_queue_insert_stop(struct bo_event_queue *event_queue,
    struct bo_edge *edge)
{
    struct bo_point point;

    point.y.ordinate = edge->edge.bottom;
    point.y.approx = BO_EXACT;
    point.x.ordinate = line_compute_intersection_x_for_y(&edge->edge,
        point.y.ordinate);
    point.x.approx = BO_EXACT;

    return bo_event_queue_insert(event_queue, BO_STOP, edge, NULL, &point);
}

static void bo_event_queue_fini(struct bo_event_queue *event_queue)
{
    pqueue_fini(&event_queue->pqueue);
}

static int event_queue_insert_if_intersect_below_current_y(
    struct bo_event_queue *event_queue, struct bo_edge *left, struct bo_edge *right)
{
    struct bo_point intersection;

    if (line_equal(&left->edge, &right->edge)) return 1;
    if (slope_compare(left, right) <= 0) return 1;
    if (!bo_edge_intersect(left, right, &intersection)) return 1;

    return bo_event_queue_insert(event_queue, BO_INTERSECTION, left, right,
        &intersection);
}

static void bo_sweep_line_init(struct bo_sweep_line *sweep_line)
{
    sweep_line->head = NULL;
    sweep_line->current_y = INT32_MIN;
    sweep_line->current_edge = NULL;
}

static int sweep_line_insert(struct bo_sweep_line *sweep_line, struct bo_edge *edge)
{
    if (sweep_line->current_edge != NULL) {
        struct bo_edge *prev, *next;
        int cmp;

        cmp = sweep_line_compare_edges(sweep_line, sweep_line->current_edge, edge);
        if (cmp < 0) {
            prev = sweep_line->current_edge;
            next = prev->next;
            while (next != NULL &&
                   sweep_line_compare_edges(sweep_line, next, edge) < 0) {
                prev = next, next = prev->next;
            }

            prev->next = edge;
            edge->prev = prev;
            edge->next = next;
            if (next != NULL)
                next->prev = edge;
        } else if (cmp > 0) {
            next = sweep_line->current_edge;
            prev = next->prev;
            while (prev != NULL &&
                   sweep_line_compare_edges(sweep_line, prev, edge) > 0) {
                next = prev, prev = next->prev;
            }

            next->prev = edge;
            edge->next = next;
            edge->prev = prev;
            if (prev != NULL)
                prev->next = edge;
            else
                sweep_line->head = edge;
        } else {
            prev = sweep_line->current_edge;
            edge->prev = prev;
            edge->next = prev->next;
            if (prev->next != NULL)
                prev->next->prev = edge;
            prev->next = edge;
        }
    } else {
        sweep_line->head = edge;
    }

    sweep_line->current_edge = edge;
    return 1;
}

static void bo_sweep_line_delete(struct bo_sweep_line *sweep_line,
    struct bo_edge *edge)
{
    if (edge->prev != NULL)
        edge->prev->next = edge->next;
    else
        sweep_line->head = edge->next;

    if (edge->next != NULL)
        edge->next->prev = edge->prev;

    if (sweep_line->current_edge == edge)
        sweep_line->current_edge = edge->prev ? edge->prev : edge->next;
}

static void bo_sweep_line_swap(struct bo_sweep_line *sweep_line,
    struct bo_edge *left, struct bo_edge *right)
{
    if (left->prev != NULL)
        left->prev->next = right;
    else
        sweep_line->head = right;

    if (right->next != NULL)
        right->next->prev = left;

    left->next = right->next;
    right->next = left;

    right->prev = left->prev;
    left->prev = right;
}

static int edges_colinear(const struct bo_edge *a, const struct bo_edge *b)
{
    if (line_equal(&a->edge, &b->edge)) return 1;
    if (slope_compare(a, b)) return 0;

    if (a->edge.p1.y == b->edge.p1.y) {
        return a->edge.p1.x == b->edge.p1.x;
    } else if (a->edge.p1.y < b->edge.p1.y) {
        return edge_compare_for_y_against_x(b, a->edge.p1.y, a->edge.p1.x) == 0;
    } else {
        return edge_compare_for_y_against_x(a, b->edge.p1.y, b->edge.p1.x) == 0;
    }
}

static void edges_end(struct bo_edge *left, int32_t bot, struct dp_polygon *polygon)
{
    struct bo_edge *right = left->deferred.other;

    if (left->deferred.top < bot) {
        dp_polygon_add_line(polygon, left->edge.p1, left->edge.p2,
            left->deferred.top, bot, 1);
        dp_polygon_add_line(polygon, right->edge.p1, right->edge.p2,
            left->deferred.top, bot, -1);
    }

    left->deferred.other = NULL;
}

static void edges_start_or_continue(struct bo_edge *left, struct bo_edge *right,
    int top, struct dp_polygon *polygon)
{
    if (left->deferred.other == right) return;

    if (left->deferred.other != NULL) {
        if (edges_colinear(left->deferred.other, right)) {
            struct bo_edge *old = left->deferred.other;

            if (old->edge.p1.y < right->edge.p1.y)
                right->edge.p1 = old->edge.p1;
            if (old->edge.p2.y > right->edge.p2.y)
                right->edge.p2 = old->edge.p2;
            left->deferred.other = right;
            return;
        }

        edges_end(left, top, polygon);
    }

    if (!edges_colinear(left, right)) {
        left->deferred.top = top;
        left->deferred.other = right;
    }
}

#define is_zero(w) ((w)[0] == 0 || (w)[1] == 0)

static void active_edges(struct bo_edge *left, int32_t top, struct dp_polygon *polygon)
{
    struct bo_edge *right;
    int winding[2] = { 0, 0 };

    while (left != NULL) {
        do {
            winding[left->a_or_b] += left->edge.dir;
            if (!is_zero(winding)) break;

            if (left->deferred.other)
                edges_end(left, top, polygon);

            left = left->next;
            if (!left) return;
        } while (1);

        right = left->next;
        do {
            if (right->deferred.other)
                edges_end(right, top, polygon);

            winding[right->a_or_b] += right->edge.dir;
            if (is_zero(winding)) {
                if (right->next == NULL || !edges_colinear(right, right->next))
                    break;
            }

            right = right->next;
        } while (1);

        edges_start_or_continue(left, right, top, polygon);

        left = right->next;
    }
}

static int intersection_sweep(struct bo_event **start_events, int num_events,
    struct dp_polygon *polygon)
{
    struct bo_event_queue event_queue;
    struct bo_sweep_line sweep_line;
    struct bo_event *event;
    struct bo_edge *left, *right, *e1, *e2;
    int status = 1;

    if (!bo_event_queue_init(&event_queue, start_events, num_events)) return 0;
    bo_sweep_line_init(&sweep_line);

    while ((event = bo_event_dequeue(&event_queue))) {
        if (event->point.y.ordinate != sweep_line.current_y) {
            active_edges(sweep_line.head, sweep_line.current_y, polygon);
            sweep_line.current_y = event->point.y.ordinate;
        }

        switch (event->type) {
        case BO_START:
            e1 = &((struct bo_start_event *)event)->edge;

            sweep_line_insert(&sweep_line, e1);

            if (!event_queue_insert_stop(&event_queue, e1)) goto unwind;

            left = e1->prev;
            right = e1->next;

            if (left != NULL) {
                if (!event_queue_insert_if_intersect_below_current_y(&event_queue, left, e1))
                    goto unwind;
            }

            if (right != NULL) {
                if (!event_queue_insert_if_intersect_below_current_y(&event_queue, e1, right))
                    goto unwind;
            }
            break;

        case BO_STOP:
            e1 = ((struct bo_queue_event *)event)->e1;
            bo_event_queue_delete(event);

            if (e1->deferred.other)
                edges_end(e1, sweep_line.current_y, polygon);

            left = e1->prev;
            right = e1->next;

            bo_sweep_line_delete(&sweep_line, e1);

            if (left != NULL && right != NULL) {
                if (!event_queue_insert_if_intersect_below_current_y(&event_queue, left, right))
                    goto unwind;
            }
            break;

        case BO_INTERSECTION:
            e1 = ((struct bo_queue_event *)event)->e1;
            e2 = ((struct bo_queue_event *)event)->e2;
            bo_event_queue_delete(event);

            if (e2 != e1->next) break;

            if (e1->deferred.other)
                edges_end(e1, sweep_line.current_y, polygon);
            if (e2->deferred.other)
                edges_end(e2, sweep_line.current_y, polygon);

            left = e1->prev;
            right = e2->next;

            bo_sweep_line_swap(&sweep_line, e1, e2);

            if (left != NULL) {
                if (!event_queue_insert_if_intersect_below_current_y(&event_queue, left, e2))
                    goto unwind;
            }

            if (right != NULL) {
                if (!event_queue_insert_if_intersect_below_current_y(&event_queue, e1, right))
                    goto unwind;
            }
            break;
        }
    }

unwind:
    bo_event_queue_fini(&event_queue);
    return status;
}

void dp_polygon_intersect(struct dp_polygon *a, struct dp_polygon *b)
{
    struct bo_start_event *events;
    struct bo_event **event_ptrs;
    int num_events;
    int i, j;

    if (a->count == 0) return;

    if (b->count == 0) {
        a->count = 0;
        return;
    }

    num_events = a->count + b->count;
    events = malloc((size_t)num_events * sizeof(*events));
    event_ptrs = malloc((size_t)(num_events + 1) * sizeof(*event_ptrs));
    if (!events || !event_ptrs) {
        free(events);
        free(event_ptrs);
        a->status = PX_STATUS_NO_MEMORY;
        a->count = 0;
        return;
    }

    j = 0;
    for (i = 0; i < a->count; i++) {
        event_ptrs[j] = (struct bo_event *)&events[j];

        events[j].type = BO_START;
        events[j].point.y.ordinate = a->edges[i].top;
        events[j].point.y.approx = BO_EXACT;
        events[j].point.x.ordinate = line_compute_intersection_x_for_y(
            &a->edges[i], events[j].point.y.ordinate);
        events[j].point.x.approx = BO_EXACT;

        events[j].edge.a_or_b = 0;
        events[j].edge.edge = a->edges[i];
        events[j].edge.deferred.other = NULL;
        events[j].edge.prev = NULL;
        events[j].edge.next = NULL;
        j++;
    }

    for (i = 0; i < b->count; i++) {
        event_ptrs[j] = (struct bo_event *)&events[j];

        events[j].type = BO_START;
        events[j].point.y.ordinate = b->edges[i].top;
        events[j].point.y.approx = BO_EXACT;
        events[j].point.x.ordinate = line_compute_intersection_x_for_y(
            &b->edges[i], events[j].point.y.ordinate);
        events[j].point.x.approx = BO_EXACT;

        events[j].edge.a_or_b = 1;
        events[j].edge.edge = b->edges[i];
        events[j].edge.deferred.other = NULL;
        events[j].edge.prev = NULL;
        events[j].edge.next = NULL;
        j++;
    }

    a->count = 0;
    intersection_sweep(event_ptrs, num_events, a);
    free(events);
    free(event_ptrs);
}

void dp_polygon_intersect_with_boxes(struct dp_polygon *polygon, const px_box_t *boxes,
    int num_boxes)
{
    struct dp_polygon b;
    int n;

    if (num_boxes == 0) {
        polygon->count = 0;
        return;
    }

    for (n = 0; n < num_boxes; n++) {
        if (polygon->extents.p1.x >= boxes[n].p1.x &&
                polygon->extents.p2.x <= boxes[n].p2.x &&
                polygon->extents.p1.y >= boxes[n].p1.y &&
                polygon->extents.p2.y <= boxes[n].p2.y)
            return;
    }

    dp_polygon_init(&b);
    for (n = 0; n < num_boxes; n++) {
        if (boxes[n].p2.x > polygon->extents.p1.x && boxes[n].p1.x < polygon->extents.p2.x &&
                boxes[n].p2.y > polygon->extents.p1.y && boxes[n].p1.y < polygon->extents.p2.y) {
            dp_polygon_add_edge(&b, (px_point_t){ boxes[n].p1.x, boxes[n].p1.y },
                (px_point_t){ boxes[n].p1.x, boxes[n].p2.y }, 1);
            dp_polygon_add_edge(&b, (px_point_t){ boxes[n].p2.x, boxes[n].p1.y },
                (px_point_t){ boxes[n].p2.x, boxes[n].p2.y }, -1);
        }
    }

    dp_polygon_intersect(polygon, &b);
    dp_polygon_fini(&b);
}
