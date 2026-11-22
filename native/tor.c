/* SPDX-License-Identifier: MIT; derived from cairo cairo-tor-scan-converter.c, see NOTICE */


#include "tor.h"

#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <setjmp.h>

#define I static

#define GLITTER_HAVE_STATUS_T 1
#define GLITTER_STATUS_SUCCESS PX_STATUS_SUCCESS
#define GLITTER_STATUS_NO_MEMORY PX_STATUS_NO_MEMORY
typedef px_status_t glitter_status_t;

#define GLITTER_INPUT_BITS PX_FIXED_FRAC_BITS
#define GRID_X_BITS PX_FIXED_FRAC_BITS
#define GRID_Y 15

struct pool;
struct cell_list;

#ifndef GLITTER_INPUT_BITS
#  define GLITTER_INPUT_BITS 8
#endif
#define GLITTER_INPUT_SCALE (1<<GLITTER_INPUT_BITS)
typedef int glitter_input_scaled_t;

#if !GLITTER_HAVE_STATUS_T
typedef enum {
    GLITTER_STATUS_SUCCESS = 0,
    GLITTER_STATUS_NO_MEMORY
} glitter_status_t;
#endif

#ifndef I
# define I
#endif

typedef struct glitter_scan_converter glitter_scan_converter_t;

I glitter_status_t
glitter_scan_converter_reset(
    glitter_scan_converter_t *converter,
    int xmin, int ymin,
    int xmax, int ymax);

#include <stdlib.h>
#include <string.h>
#include <limits.h>

typedef int grid_scaled_t;
typedef int grid_scaled_x_t;
typedef int grid_scaled_y_t;

#if !defined(GRID_X) && !defined(GRID_X_BITS)
#  define GRID_X_BITS 8
#endif
#if !defined(GRID_Y) && !defined(GRID_Y_BITS)
#  define GRID_Y 15
#endif

#ifdef GRID_X_BITS
#  define GRID_X (1 << GRID_X_BITS)
#endif
#ifdef GRID_Y_BITS
#  define GRID_Y (1 << GRID_Y_BITS)
#endif

#if defined(GRID_X_TO_INT_FRAC)

#elif defined(GRID_X_BITS)
#  define GRID_X_TO_INT_FRAC(x, i, f) \
	_GRID_TO_INT_FRAC_shift(x, i, f, GRID_X_BITS)
#else
#  define GRID_X_TO_INT_FRAC(x, i, f) \
	_GRID_TO_INT_FRAC_general(x, i, f, GRID_X)
#endif

#define _GRID_TO_INT_FRAC_general(t, i, f, m) do {	\
    (i) = (t) / (m);					\
    (f) = (t) % (m);					\
    if ((f) < 0) {					\
	--(i);						\
	(f) += (m);					\
    }							\
} while (0)

#define _GRID_TO_INT_FRAC_shift(t, i, f, b) do {	\
    (f) = (t) & ((1 << (b)) - 1);			\
    (i) = (t) >> (b);					\
} while (0)

#define GRID_XY (2*GRID_X*GRID_Y)

#if GRID_XY == 510
#  define GRID_AREA_TO_ALPHA(c)	  (((c)+1) >> 1)
#elif GRID_XY == 255
#  define  GRID_AREA_TO_ALPHA(c)  (c)
#elif GRID_XY == 64
#  define  GRID_AREA_TO_ALPHA(c)  (((c) << 2) | -(((c) & 0x40) >> 6))
#elif GRID_XY == 128
#  define  GRID_AREA_TO_ALPHA(c)  ((((c) << 1) | -((c) >> 7)) & 255)
#elif GRID_XY == 256
#  define  GRID_AREA_TO_ALPHA(c)  (((c) | -((c) >> 8)) & 255)
#elif GRID_XY == 15
#  define  GRID_AREA_TO_ALPHA(c)  (((c) << 4) + (c))
#elif GRID_XY == 2*256*15
#  define  GRID_AREA_TO_ALPHA(c)  (((c) + ((c)<<4) + 256) >> 9)
#else
#  define  GRID_AREA_TO_ALPHA(c)  (((c)*255 + GRID_XY/2) / GRID_XY)
#endif

#define UNROLL3(x) x x x

struct quorem {
    int32_t quo;
    int64_t rem;
};

struct _pool_chunk {

    size_t size;

    size_t capacity;

    struct _pool_chunk *prev_chunk;

    int64_t data;
};

#define SIZEOF_POOL_CHUNK (sizeof(struct _pool_chunk) - sizeof(int64_t))

struct pool {

    struct _pool_chunk *current;

    jmp_buf *jmp;

    struct _pool_chunk *first_free;

    size_t default_capacity;

    char sentinel[SIZEOF_POOL_CHUNK];
};

struct edge {

    struct edge *next, *prev;

    grid_scaled_y_t ytop;

    grid_scaled_y_t height_left;

    int dir;
    int cell;

    struct quorem x;

    struct quorem dxdy;

    struct quorem dxdy_full;

    int64_t dy;
};

#define EDGE_Y_BUCKET_INDEX(y, ymin) (((y) - (ymin))/GRID_Y)

struct polygon {

    grid_scaled_y_t ymin, ymax;

    struct edge **y_buckets;
    struct edge *y_buckets_embedded[64];

    struct {
	struct pool base[1];
	struct edge embedded[32];
    } edge_pool;
};

struct cell {
    struct cell		*next;
    int			 x;
    int16_t		 uncovered_area;
    int16_t		 covered_height;
};

struct cell_list {

    struct cell head, tail;

    struct cell *cursor, *rewind;

    struct {
	struct pool base[1];
	struct cell embedded[32];
    } cell_pool;
};

struct cell_pair {
    struct cell *cell1;
    struct cell *cell2;
};

struct active_list {

    struct edge head, tail;

    grid_scaled_y_t min_height;
    int is_vertical;
};

struct glitter_scan_converter {
    struct polygon	polygon[1];
    struct active_list	active[1];
    struct cell_list	coverages[1];

    px_half_open_span_t *spans;
    px_half_open_span_t spans_embedded[64];

    grid_scaled_x_t xmin, xmax;
    grid_scaled_y_t ymin, ymax;
};

static struct _pool_chunk *
_pool_chunk_init(
    struct _pool_chunk *p,
    struct _pool_chunk *prev_chunk,
    size_t capacity)
{
    p->prev_chunk = prev_chunk;
    p->size = 0;
    p->capacity = capacity;
    return p;
}

static struct _pool_chunk *
_pool_chunk_create(struct pool *pool, size_t size)
{
    struct _pool_chunk *p;

    p = px_malloc (SIZEOF_POOL_CHUNK + size);
    if (unlikely (NULL == p))
	longjmp (*pool->jmp, px_error (PX_STATUS_NO_MEMORY));

    return _pool_chunk_init(p, pool->current, size);
}

static void
pool_init(struct pool *pool,
	  jmp_buf *jmp,
	  size_t default_capacity,
	  size_t embedded_capacity)
{
    pool->jmp = jmp;
    pool->current = (void*) pool->sentinel;
    pool->first_free = NULL;
    pool->default_capacity = default_capacity;
    _pool_chunk_init(pool->current, NULL, embedded_capacity);
}

static void
pool_fini(struct pool *pool)
{
    struct _pool_chunk *p = pool->current;
    do {
	while (NULL != p) {
	    struct _pool_chunk *prev = p->prev_chunk;
	    if (p != (void *) pool->sentinel)
		free(p);
	    p = prev;
	}
	p = pool->first_free;
	pool->first_free = NULL;
    } while (NULL != p);
}

static void *
_pool_alloc_from_new_chunk(
    struct pool *pool,
    size_t size)
{
    struct _pool_chunk *chunk;
    void *obj;
    size_t capacity;

    capacity = size;
    chunk = NULL;
    if (size < pool->default_capacity) {
	capacity = pool->default_capacity;
	chunk = pool->first_free;
	if (chunk) {
	    pool->first_free = chunk->prev_chunk;
	    _pool_chunk_init(chunk, pool->current, chunk->capacity);
	}
    }

    if (NULL == chunk)
	chunk = _pool_chunk_create (pool, capacity);
    pool->current = chunk;

    obj = ((unsigned char*)&chunk->data + chunk->size);
    chunk->size += size;
    return obj;
}

inline static void *
pool_alloc (struct pool *pool, size_t size)
{
    struct _pool_chunk *chunk = pool->current;

    if (size <= chunk->capacity - chunk->size) {
	void *obj = ((unsigned char*)&chunk->data + chunk->size);
	chunk->size += size;
	return obj;
    } else {
	return _pool_alloc_from_new_chunk(pool, size);
    }
}

static void
pool_reset (struct pool *pool)
{

    struct _pool_chunk *chunk = pool->current;
    if (chunk != (void *) pool->sentinel) {
	while (chunk->prev_chunk != (void *) pool->sentinel) {
	    chunk = chunk->prev_chunk;
	}
	chunk->prev_chunk = pool->first_free;
	pool->first_free = pool->current;
    }

    pool->current = (void *) pool->sentinel;
    pool->current->size = 0;
}

inline static void
cell_list_rewind (struct cell_list *cells)
{
    cells->cursor = &cells->head;
}

inline static void
cell_list_maybe_rewind (struct cell_list *cells, int x)
{
    if (x < cells->cursor->x) {
	cells->cursor = cells->rewind;
	if (x < cells->cursor->x)
	    cells->cursor = &cells->head;
    }
}

inline static void
cell_list_set_rewind (struct cell_list *cells)
{
    cells->rewind = cells->cursor;
}

static void
cell_list_init(struct cell_list *cells, jmp_buf *jmp)
{
    pool_init(cells->cell_pool.base, jmp,
	      256*sizeof(struct cell),
	      sizeof(cells->cell_pool.embedded));
    cells->tail.next = NULL;
    cells->tail.x = INT_MAX;
    cells->head.x = INT_MIN;
    cells->head.next = &cells->tail;
    cell_list_rewind (cells);
}

static void
cell_list_fini(struct cell_list *cells)
{
    pool_fini (cells->cell_pool.base);
}

inline static void
cell_list_reset (struct cell_list *cells)
{
    cell_list_rewind (cells);
    cells->head.next = &cells->tail;
    pool_reset (cells->cell_pool.base);
}

inline static struct cell *
cell_list_alloc (struct cell_list *cells,
		 struct cell *tail,
		 int x)
{
    struct cell *cell;

    cell = pool_alloc (cells->cell_pool.base, sizeof (struct cell));
    cell->next = tail->next;
    tail->next = cell;
    cell->x = x;
    cell->uncovered_area = 0;
    cell->covered_height = 0;

    return cell;
}

inline static struct cell *
cell_list_find (struct cell_list *cells, int x)
{
    struct cell *tail = cells->cursor;

    if (tail->x == x)
	return tail;

    while (1) {
	UNROLL3({
		if (tail->next->x > x)
			break;
		tail = tail->next;
	});
    }

    if (tail->x != x)
	tail = cell_list_alloc (cells, tail, x);
    return cells->cursor = tail;

}

inline static struct cell_pair
cell_list_find_pair(struct cell_list *cells, int x1, int x2)
{
    struct cell_pair pair;

    pair.cell1 = cells->cursor;
    while (1) {
	UNROLL3({
		if (pair.cell1->next->x > x1)
			break;
		pair.cell1 = pair.cell1->next;
	});
    }
    if (pair.cell1->x != x1)
	pair.cell1 = cell_list_alloc (cells, pair.cell1, x1);

    pair.cell2 = pair.cell1;
    while (1) {
	UNROLL3({
		if (pair.cell2->next->x > x2)
			break;
		pair.cell2 = pair.cell2->next;
	});
    }
    if (pair.cell2->x != x2)
	pair.cell2 = cell_list_alloc (cells, pair.cell2, x2);

    cells->cursor = pair.cell2;
    return pair;
}

inline static void
cell_list_add_subspan(struct cell_list *cells,
		      grid_scaled_x_t x1,
		      grid_scaled_x_t x2)
{
    int ix1, fx1;
    int ix2, fx2;

    if (x1 == x2)
	return;

    GRID_X_TO_INT_FRAC(x1, ix1, fx1);
    GRID_X_TO_INT_FRAC(x2, ix2, fx2);

    if (ix1 != ix2) {
	struct cell_pair p;
	p = cell_list_find_pair(cells, ix1, ix2);
	p.cell1->uncovered_area += 2*fx1;
	++p.cell1->covered_height;
	p.cell2->uncovered_area -= 2*fx2;
	--p.cell2->covered_height;
    } else {
	struct cell *cell = cell_list_find(cells, ix1);
	cell->uncovered_area += 2*(fx1-fx2);
    }
}

inline static void full_step (struct edge *e)
{
    if (e->dy == 0)
	return;

    e->x.quo += e->dxdy_full.quo;
    e->x.rem += e->dxdy_full.rem;
    if (e->x.rem < 0) {
	e->x.quo--;
	e->x.rem += e->dy;
    } else if (e->x.rem >= e->dy) {
	++e->x.quo;
	e->x.rem -= e->dy;
    }

    e->cell = e->x.quo + (e->x.rem >= e->dy/2);
}

static void
cell_list_render_edge(struct cell_list *cells,
		      struct edge *edge,
		      int sign)
{
    struct quorem x1, x2;
    grid_scaled_x_t fx1, fx2;
    int ix1, ix2;

    x1 = edge->x;
    full_step (edge);
    x2 = edge->x;

    if (edge->dy) {
	x1.quo -= edge->dxdy.quo / 2;
	x1.rem -= edge->dxdy.rem / 2;
	if (x1.rem < 0) {
	    --x1.quo;
	    x1.rem += edge->dy;
	} else if (x1.rem >= edge->dy) {
	    ++x1.quo;
	    x1.rem -= edge->dy;
	}

	x2.quo -= edge->dxdy.quo / 2;
	x2.rem -= edge->dxdy.rem / 2;
	if (x2.rem < 0) {
	    --x2.quo;
	    x2.rem += edge->dy;
	} else if (x2.rem >= edge->dy) {
	    ++x2.quo;
	    x2.rem -= edge->dy;
	}
    }

    GRID_X_TO_INT_FRAC(x1.quo, ix1, fx1);
    GRID_X_TO_INT_FRAC(x2.quo, ix2, fx2);

    cell_list_maybe_rewind(cells, MIN(ix1, ix2));

    if (ix1 == ix2) {

	struct cell *cell = cell_list_find(cells, ix1);
	cell->covered_height += sign*GRID_Y;
	cell->uncovered_area += sign*(fx1 + fx2)*GRID_Y;
	return;
    }

    if (ix2 < ix1) {
	struct quorem tx;
	int t;

	t = ix1;
	ix1 = ix2;
	ix2 = t;

	t = fx1;
	fx1 = fx2;
	fx2 = t;

	tx = x1;
	x1 = x2;
	x2 = tx;
    }

    {
	struct cell_pair pair;
	struct quorem y;
	int64_t tmp, dx;
	int y_last;

	dx = (x2.quo - x1.quo) * edge->dy + (x2.rem - x1.rem);

	tmp = (ix1 + 1) * GRID_X * edge->dy;
	tmp -= x1.quo * edge->dy + x1.rem;
	tmp *= GRID_Y;

	y.quo = tmp / dx;
	y.rem = tmp % dx;

	pair = cell_list_find_pair(cells, ix1, ix1+1);
	pair.cell1->uncovered_area += sign*y.quo*(GRID_X + fx1);
	pair.cell1->covered_height += sign*y.quo;
	y_last = y.quo;

	if (ix1+1 < ix2) {
	    struct cell *cell = pair.cell2;
	    struct quorem dydx_full;

	    dydx_full.quo = GRID_Y * GRID_X * edge->dy / dx;
	    dydx_full.rem = GRID_Y * GRID_X * edge->dy % dx;

	    ++ix1;
	    do {
		y.quo += dydx_full.quo;
		y.rem += dydx_full.rem;
		if (y.rem >= dx) {
		    y.quo++;
		    y.rem -= dx;
		}

		cell->uncovered_area += sign*(y.quo - y_last)*GRID_X;
		cell->covered_height += sign*(y.quo - y_last);
		y_last = y.quo;

		++ix1;
		cell = cell_list_find(cells, ix1);
	    } while (ix1 != ix2);

	    pair.cell2 = cell;
	}
	pair.cell2->uncovered_area += sign*(GRID_Y - y_last)*fx2;
	pair.cell2->covered_height += sign*(GRID_Y - y_last);
    }
}

static void
polygon_init (struct polygon *polygon, jmp_buf *jmp)
{
    polygon->ymin = polygon->ymax = 0;
    polygon->y_buckets = polygon->y_buckets_embedded;
    pool_init (polygon->edge_pool.base, jmp,
	       8192 - sizeof (struct _pool_chunk),
	       sizeof (polygon->edge_pool.embedded));
}

static void
polygon_fini (struct polygon *polygon)
{
    if (polygon->y_buckets != polygon->y_buckets_embedded)
	free (polygon->y_buckets);

    pool_fini (polygon->edge_pool.base);
}

static glitter_status_t
polygon_reset (struct polygon *polygon,
	       grid_scaled_y_t ymin,
	       grid_scaled_y_t ymax)
{
    unsigned h = ymax - ymin;
    unsigned num_buckets = EDGE_Y_BUCKET_INDEX(ymax + GRID_Y-1, ymin);

    pool_reset(polygon->edge_pool.base);

    if (unlikely (h > 0x7FFFFFFFU - GRID_Y))
	goto bail_no_mem;

    if (polygon->y_buckets != polygon->y_buckets_embedded)
	free (polygon->y_buckets);

    polygon->y_buckets =  polygon->y_buckets_embedded;
    if (num_buckets > ARRAY_LENGTH (polygon->y_buckets_embedded)) {
	polygon->y_buckets = px_malloc_ab (num_buckets,
					       sizeof (struct edge *));
	if (unlikely (NULL == polygon->y_buckets))
	    goto bail_no_mem;
    }
    memset (polygon->y_buckets, 0, num_buckets * sizeof (struct edge *));

    polygon->ymin = ymin;
    polygon->ymax = ymax;
    return GLITTER_STATUS_SUCCESS;

bail_no_mem:
    polygon->ymin = 0;
    polygon->ymax = 0;
    return GLITTER_STATUS_NO_MEMORY;
}

static void
_polygon_insert_edge_into_its_y_bucket(struct polygon *polygon,
				       struct edge *e)
{
    unsigned ix = EDGE_Y_BUCKET_INDEX(e->ytop, polygon->ymin);
    struct edge **ptail = &polygon->y_buckets[ix];
    e->next = *ptail;
    *ptail = e;
}

static void
active_list_reset (struct active_list *active)
{
    active->head.height_left = INT_MAX;
    active->head.dy = 0;
    active->head.cell = INT_MIN;
    active->head.prev = NULL;
    active->head.next = &active->tail;
    active->tail.prev = &active->head;
    active->tail.next = NULL;
    active->tail.cell = INT_MAX;
    active->tail.height_left = INT_MAX;
    active->tail.dy = 0;
    active->min_height = 0;
    active->is_vertical = 1;
}

static void
active_list_init(struct active_list *active)
{
    active_list_reset(active);
}

static struct edge *
merge_sorted_edges (struct edge *head_a, struct edge *head_b)
{
    struct edge *head, **next, *prev;
    int32_t x;

    prev = head_a->prev;
    next = &head;
    if (head_a->cell <= head_b->cell) {
	head = head_a;
    } else {
	head = head_b;
	head_b->prev = prev;
	goto start_with_b;
    }

    do {
	x = head_b->cell;
	while (head_a != NULL && head_a->cell <= x) {
	    prev = head_a;
	    next = &head_a->next;
	    head_a = head_a->next;
	}

	head_b->prev = prev;
	*next = head_b;
	if (head_a == NULL)
	    return head;

start_with_b:
	x = head_a->cell;
	while (head_b != NULL && head_b->cell <= x) {
	    prev = head_b;
	    next = &head_b->next;
	    head_b = head_b->next;
	}

	head_a->prev = prev;
	*next = head_a;
	if (head_b == NULL)
	    return head;
    } while (1);
}

static struct edge *
sort_edges (struct edge *list,
	    unsigned int level,
	    struct edge **head_out)
{
    struct edge *head_other, *remaining;
    unsigned int i;

    head_other = list->next;

    if (head_other == NULL) {
	*head_out = list;
	return NULL;
    }

    remaining = head_other->next;
    if (list->cell <= head_other->cell) {
	*head_out = list;
	head_other->next = NULL;
    } else {
	*head_out = head_other;
	head_other->prev = list->prev;
	head_other->next = list;
	list->prev = head_other;
	list->next = NULL;
    }

    for (i = 0; i < level && remaining; i++) {
	remaining = sort_edges (remaining, i, &head_other);
	*head_out = merge_sorted_edges (*head_out, head_other);
    }

    return remaining;
}

 static struct edge *
merge_unsorted_edges (struct edge *head, struct edge *unsorted)
{
    sort_edges (unsorted, UINT_MAX, &unsorted);
    return merge_sorted_edges (head, unsorted);
}

inline static int
can_do_full_row (struct active_list *active)
{
    const struct edge *e;
    int prev_x = INT_MIN;

    if (active->min_height <= 0) {
	int min_height = INT_MAX;
	int is_vertical = 1;

	e = active->head.next;
	while (NULL != e) {
	    if (e->height_left < min_height)
		min_height = e->height_left;
	    is_vertical &= e->dy == 0;
	    e = e->next;
	}

	active->is_vertical = is_vertical;
	active->min_height = min_height;
    }

    if (active->min_height < GRID_Y)
	return 0;

    for (e = active->head.next; e != &active->tail; e = e->next) {
	int cell;

	if (e->dy) {
	    struct quorem x = e->x;
	    x.quo += e->dxdy_full.quo;
	    x.rem += e->dxdy_full.rem;
	    if (x.rem < 0) {
		x.quo--;
		x.rem += e->dy;
	    } else if (x.rem >= e->dy) {
		x.quo++;
		x.rem -= e->dy;
	    }
	    cell = x.quo + (x.rem >= e->dy/2);
	} else
	    cell = e->cell;

	if (cell < prev_x)
	    return 0;

	prev_x = cell;
    }

    return 1;
}

inline static void
active_list_merge_edges_from_bucket(struct active_list *active,
				    struct edge *edges)
{
    active->head.next = merge_unsorted_edges (active->head.next, edges);
}

inline static int
polygon_fill_buckets (struct active_list *active,
		      struct edge *edge,
		      int y,
		      struct edge **buckets)
{
    grid_scaled_y_t min_height = active->min_height;
    int is_vertical = active->is_vertical;
    int max_suby = 0;

    while (edge) {
	struct edge *next = edge->next;
	int suby = edge->ytop - y;
	if (buckets[suby])
	    buckets[suby]->prev = edge;
	edge->next = buckets[suby];
	edge->prev = NULL;
	buckets[suby] = edge;
	if (edge->height_left < min_height)
	    min_height = edge->height_left;
	is_vertical &= edge->dy == 0;
	edge = next;
	if (suby > max_suby)
		max_suby = suby;
    }

    active->is_vertical = is_vertical;
    active->min_height = min_height;

    return max_suby;
}

static void step (struct edge *edge)
{
    if (edge->dy == 0)
	return;

    edge->x.quo += edge->dxdy.quo;
    edge->x.rem += edge->dxdy.rem;
    if (edge->x.rem < 0) {
	--edge->x.quo;
	edge->x.rem += edge->dy;
    } else if (edge->x.rem >= edge->dy) {
	++edge->x.quo;
	edge->x.rem -= edge->dy;
    }

    edge->cell = edge->x.quo + (edge->x.rem >= edge->dy/2);
}

inline static void
sub_row (struct active_list *active,
	 struct cell_list *coverages,
	 unsigned int mask)
{
    struct edge *edge = active->head.next;
    int xstart = INT_MIN, prev_x = INT_MIN;
    int winding = 0;

    cell_list_rewind (coverages);

    while (&active->tail != edge) {
	struct edge *next = edge->next;
	int xend = edge->cell;

	if (--edge->height_left) {
	    step (edge);

	    if (edge->cell < prev_x) {
		struct edge *pos = edge->prev;
		pos->next = next;
		next->prev = pos;
		do {
		    pos = pos->prev;
		} while (edge->cell < pos->cell);
		pos->next->prev = edge;
		edge->next = pos->next;
		edge->prev = pos;
		pos->next = edge;
	    } else
		prev_x = edge->cell;
	    active->min_height = -1;
	} else {
	    edge->prev->next = next;
	    next->prev = edge->prev;
	}

	winding += edge->dir;
	if ((winding & mask) == 0) {
	    if (next->cell != xend) {
		cell_list_add_subspan (coverages, xstart, xend);
		xstart = INT_MIN;
	    }
	} else if (xstart == INT_MIN)
	    xstart = xend;

	edge = next;
    }
}

inline static void dec (struct active_list *a, struct edge *e, int h)
{
    e->height_left -= h;
    if (e->height_left == 0) {
	e->prev->next = e->next;
	e->next->prev = e->prev;
	a->min_height = -1;
    }
}

static void
full_row (struct active_list *active,
	  struct cell_list *coverages,
	  unsigned int mask)
{
    struct edge *left = active->head.next;

    while (&active->tail != left) {
	struct edge *right;
	int winding;

	dec (active, left, GRID_Y);

	winding = left->dir;
	right = left->next;
	do {
	    dec (active, right, GRID_Y);

	    winding += right->dir;
	    if ((winding & mask) == 0 && right->next->cell != right->cell)
		break;

	    full_step (right);

	    right = right->next;
	} while (1);

	cell_list_set_rewind (coverages);
	cell_list_render_edge (coverages, left, +1);
	cell_list_render_edge (coverages, right, -1);

	left = right->next;
    }
}

static void
_glitter_scan_converter_init(glitter_scan_converter_t *converter, jmp_buf *jmp)
{
    polygon_init(converter->polygon, jmp);
    active_list_init(converter->active);
    cell_list_init(converter->coverages, jmp);
    converter->xmin=0;
    converter->ymin=0;
    converter->xmax=0;
    converter->ymax=0;
}

static void
_glitter_scan_converter_fini(glitter_scan_converter_t *self)
{
    if (self->spans != self->spans_embedded)
	free (self->spans);

    polygon_fini(self->polygon);
    cell_list_fini(self->coverages);

    self->xmin=0;
    self->ymin=0;
    self->xmax=0;
    self->ymax=0;
}

static grid_scaled_t
int_to_grid_scaled(int i, int scale)
{

    if (i >= 0) {
	if (i >= INT_MAX/scale)
	    i = INT_MAX/scale;
    }
    else {
	if (i <= INT_MIN/scale)
	    i = INT_MIN/scale;
    }
    return i*scale;
}

#define int_to_grid_scaled_x(x) int_to_grid_scaled((x), GRID_X)
#define int_to_grid_scaled_y(x) int_to_grid_scaled((x), GRID_Y)

I glitter_status_t
glitter_scan_converter_reset(
			     glitter_scan_converter_t *converter,
			     int xmin, int ymin,
			     int xmax, int ymax)
{
    glitter_status_t status;
    int max_num_spans;

    converter->xmin = 0; converter->xmax = 0;
    converter->ymin = 0; converter->ymax = 0;

    max_num_spans = 2 * (xmax - xmin + 1) + 2;

    if (max_num_spans > ARRAY_LENGTH(converter->spans_embedded)) {
	converter->spans = px_malloc_ab (max_num_spans,
					     sizeof (px_half_open_span_t));
	if (unlikely (converter->spans == NULL))
	    return px_error (PX_STATUS_NO_MEMORY);
    } else
	converter->spans = converter->spans_embedded;

    xmin = int_to_grid_scaled_x(xmin);
    ymin = int_to_grid_scaled_y(ymin);
    xmax = int_to_grid_scaled_x(xmax);
    ymax = int_to_grid_scaled_y(ymax);

    active_list_reset(converter->active);
    cell_list_reset(converter->coverages);
    status = polygon_reset(converter->polygon, ymin, ymax);
    if (status)
	return status;

    converter->xmin = xmin;
    converter->xmax = xmax;
    converter->ymin = ymin;
    converter->ymax = ymax;
    return GLITTER_STATUS_SUCCESS;
}

#if !defined(INPUT_TO_GRID_Y) && defined(GRID_Y_BITS) && GRID_Y_BITS <= GLITTER_INPUT_BITS
#  define INPUT_TO_GRID_Y(in, out) (out) = (in) >> (GLITTER_INPUT_BITS - GRID_Y_BITS)
#else
#  define INPUT_TO_GRID_Y(in, out) INPUT_TO_GRID_general(in, out, GRID_Y)
#endif

#if !defined(INPUT_TO_GRID_X) && defined(GRID_X_BITS) && GRID_X_BITS <= GLITTER_INPUT_BITS
#  define INPUT_TO_GRID_X(in, out) (out) = (in) >> (GLITTER_INPUT_BITS - GRID_X_BITS)
#else
#  define INPUT_TO_GRID_X(in, out) INPUT_TO_GRID_general(in, out, GRID_X)
#endif

#define INPUT_TO_GRID_general(in, out, grid_scale) do {		\
    long long tmp__ = (long long)(grid_scale) * (in);	\
    tmp__ += 1 << (GLITTER_INPUT_BITS-1);			\
    tmp__ >>= GLITTER_INPUT_BITS;				\
    (out) = tmp__;						\
} while (0)

inline static void
polygon_add_edge (struct polygon *polygon,
		  const px_edge_t *edge)
{
    struct edge *e;
    grid_scaled_y_t ytop, ybot;
    const px_point_t *p1, *p2;

    INPUT_TO_GRID_Y (edge->top, ytop);
    if (ytop < polygon->ymin)
	    ytop = polygon->ymin;

    INPUT_TO_GRID_Y (edge->bottom, ybot);
    if (ybot > polygon->ymax)
	    ybot = polygon->ymax;

    if (ybot <= ytop)
	    return;

    e = pool_alloc (polygon->edge_pool.base, sizeof (struct edge));

    e->ytop = ytop;
    e->height_left = ybot - ytop;
    if (edge->line.p2.y > edge->line.p1.y) {
	    e->dir = edge->dir;
	    p1 = &edge->line.p1;
	    p2 = &edge->line.p2;
    } else {
	    e->dir = -edge->dir;
	    p1 = &edge->line.p2;
	    p2 = &edge->line.p1;
    }

    if (p2->x == p1->x) {
	e->cell = p1->x;
	e->x.quo = p1->x;
	e->x.rem = 0;
	e->dxdy.quo = e->dxdy.rem = 0;
	e->dxdy_full.quo = e->dxdy_full.rem = 0;
	e->dy = 0;
    } else {
	int64_t Ex, Ey, tmp;

	Ex = (int64_t)(p2->x - p1->x) * GRID_X;
	Ey = (int64_t)(p2->y - p1->y) * GRID_Y * (2 << GLITTER_INPUT_BITS);

	e->dxdy.quo = Ex * (2 << GLITTER_INPUT_BITS) / Ey;
	e->dxdy.rem = Ex * (2 << GLITTER_INPUT_BITS) % Ey;

	tmp = (int64_t)(2*ytop + 1) << GLITTER_INPUT_BITS;
	tmp -= (int64_t)p1->y * GRID_Y * 2;
	tmp *= Ex;
	e->x.quo = tmp / Ey;
	e->x.rem = tmp % Ey;

#if GRID_X_BITS == GLITTER_INPUT_BITS
	e->x.quo += p1->x;
#else
	tmp = (int64_t)p1->x * GRID_X;
	e->x.quo += tmp >> GLITTER_INPUT_BITS;
	e->x.rem += ((tmp & ((1 << GLITTER_INPUT_BITS) - 1)) * Ey) / (1 << GLITTER_INPUT_BITS);
#endif

	if (e->x.rem < 0) {
		e->x.quo--;
		e->x.rem += Ey;
	} else  if (e->x.rem >= Ey) {
		e->x.quo++;
		e->x.rem -= Ey;
	}

	if (e->height_left >= GRID_Y) {
	    tmp = Ex * (2 * GRID_Y << GLITTER_INPUT_BITS);
	    e->dxdy_full.quo = tmp / Ey;
	    e->dxdy_full.rem = tmp % Ey;
	} else
	    e->dxdy_full.quo = e->dxdy_full.rem = 0;

	e->cell = e->x.quo + (e->x.rem >= Ey/2);
	e->dy = Ey;
    }

    _polygon_insert_edge_into_its_y_bucket (polygon, e);
}

I void
glitter_scan_converter_add_edge (glitter_scan_converter_t *converter,
				 const px_edge_t *edge)
{
    polygon_add_edge (converter->polygon, edge);
}

static void
step_edges (struct active_list *active, int count)
{
    struct edge *edge;

    count *= GRID_Y;
    for (edge = active->head.next; edge != &active->tail; edge = edge->next) {
	edge->height_left -= count;
	if (! edge->height_left) {
	    edge->prev->next = edge->next;
	    edge->next->prev = edge->prev;
	    active->min_height = -1;
	}
    }
}

static glitter_status_t
blit_a8 (struct cell_list *cells,
	 px_span_renderer_t *renderer,
	 px_half_open_span_t *spans,
	 int y, int height,
	 int xmin, int xmax)
{
    struct cell *cell = cells->head.next;
    int prev_x = xmin, last_x = -1;
    int16_t cover = 0, last_cover = 0;
    unsigned num_spans;

    if (cell == &cells->tail)
	return PX_STATUS_SUCCESS;

    while (cell->x < xmin) {
	cover += cell->covered_height;
	cell = cell->next;
    }
    cover *= GRID_X*2;

    num_spans = 0;
    for (; cell->x < xmax; cell = cell->next) {
	int x = cell->x;
	int16_t area;

	if (x > prev_x && cover != last_cover) {
	    spans[num_spans].x = prev_x;
	    spans[num_spans].coverage = GRID_AREA_TO_ALPHA (cover);
	    last_cover = cover;
	    last_x = prev_x;
	    ++num_spans;
	}

	cover += cell->covered_height*GRID_X*2;
	area = cover - cell->uncovered_area;

	if (area != last_cover) {
	    spans[num_spans].x = x;
	    spans[num_spans].coverage = GRID_AREA_TO_ALPHA (area);
	    last_cover = area;
	    last_x = x;
	    ++num_spans;
	}

	prev_x = x+1;
    }

    if (prev_x <= xmax && cover != last_cover) {
	spans[num_spans].x = prev_x;
	spans[num_spans].coverage = GRID_AREA_TO_ALPHA (cover);
	last_cover = cover;
	last_x = prev_x;
	++num_spans;
    }

    if (last_x < xmax && last_cover) {
	spans[num_spans].x = xmax;
	spans[num_spans].coverage = 0;
	++num_spans;
    }

    return renderer->render_rows (renderer, y, height, spans, num_spans);
}

#define GRID_AREA_TO_A1(A)  ((GRID_AREA_TO_ALPHA (A) > 127) ? 255 : 0)
static glitter_status_t
blit_a1 (struct cell_list *cells,
	 px_span_renderer_t *renderer,
	 px_half_open_span_t *spans,
	 int y, int height,
	 int xmin, int xmax)
{
    struct cell *cell = cells->head.next;
    int prev_x = xmin, last_x = -1;
    int16_t cover = 0;
    uint8_t coverage, last_cover = 0;
    unsigned num_spans;

    if (cell == &cells->tail)
	return PX_STATUS_SUCCESS;

    while (cell->x < xmin) {
	cover += cell->covered_height;
	cell = cell->next;
    }
    cover *= GRID_X*2;

    num_spans = 0;
    for (; cell->x < xmax; cell = cell->next) {
	int x = cell->x;
	int16_t area;

	coverage = GRID_AREA_TO_A1 (cover);
	if (x > prev_x && coverage != last_cover) {
	    last_x = spans[num_spans].x = prev_x;
	    last_cover = spans[num_spans].coverage = coverage;
	    ++num_spans;
	}

	cover += cell->covered_height*GRID_X*2;
	area = cover - cell->uncovered_area;

	coverage = GRID_AREA_TO_A1 (area);
	if (coverage != last_cover) {
	    last_x = spans[num_spans].x = x;
	    last_cover = spans[num_spans].coverage = coverage;
	    ++num_spans;
	}

	prev_x = x+1;
    }

    coverage = GRID_AREA_TO_A1 (cover);
    if (prev_x <= xmax && coverage != last_cover) {
	last_x = spans[num_spans].x = prev_x;
	last_cover = spans[num_spans].coverage = coverage;
	++num_spans;
    }

    if (last_x < xmax && last_cover) {
	spans[num_spans].x = xmax;
	spans[num_spans].coverage = 0;
	++num_spans;
    }
    if (num_spans == 1)
	return PX_STATUS_SUCCESS;

    return renderer->render_rows (renderer, y, height, spans, num_spans);
}

I void
glitter_scan_converter_render(glitter_scan_converter_t *converter,
			      unsigned int winding_mask,
			      int antialias,
			      px_span_renderer_t *renderer)
{
    int i, j;
    int ymax_i = converter->ymax / GRID_Y;
    int ymin_i = converter->ymin / GRID_Y;
    int xmin_i, xmax_i;
    int h = ymax_i - ymin_i;
    struct polygon *polygon = converter->polygon;
    struct cell_list *coverages = converter->coverages;
    struct active_list *active = converter->active;
    struct edge *buckets[GRID_Y] = { 0 };

    xmin_i = converter->xmin / GRID_X;
    xmax_i = converter->xmax / GRID_X;
    if (xmin_i >= xmax_i)
	return;

    for (i = 0; i < h; i = j) {
	int do_full_row = 0;

	j = i + 1;

	if (polygon_fill_buckets (active,
				  polygon->y_buckets[i],
				  (i+ymin_i)*GRID_Y,
				  buckets) == 0) {
	    if (buckets[0]) {
		active_list_merge_edges_from_bucket (active, buckets[0]);
		buckets[0] = NULL;
	    }

	    if (active->head.next == &active->tail) {
		active->min_height = INT_MAX;
		active->is_vertical = 1;
		for (; j < h && ! polygon->y_buckets[j]; j++)
		    ;
		continue;
	    }

	    do_full_row = can_do_full_row (active);
	}

	if (do_full_row) {

	    full_row (active, coverages, winding_mask);

	    if (active->is_vertical) {
		while (j < h &&
		       polygon->y_buckets[j] == NULL &&
		       active->min_height >= 2*GRID_Y)
		{
		    active->min_height -= GRID_Y;
		    j++;
		}
		if (j != i + 1)
		    step_edges (active, j - (i + 1));
	    }
	} else {
	    int sub;

	    for (sub = 0; sub < GRID_Y; sub++) {
		if (buckets[sub]) {
		    active_list_merge_edges_from_bucket (active, buckets[sub]);
		    buckets[sub] = NULL;
		}
		sub_row (active, coverages, winding_mask);
	    }
	}

	if (antialias)
	    blit_a8 (coverages, renderer, converter->spans,
		     i+ymin_i, j-i, xmin_i, xmax_i);
	else
	    blit_a1 (coverages, renderer, converter->spans,
		     i+ymin_i, j-i, xmin_i, xmax_i);
	cell_list_reset (coverages);

	active->min_height -= GRID_Y;
    }
}

struct _cairo_tor_scan_converter {
    px_scan_converter_t base;

    glitter_scan_converter_t converter[1];
    px_fill_rule_t fill_rule;
    px_antialias_t antialias;

    jmp_buf jmp;
};

typedef struct _cairo_tor_scan_converter cairo_tor_scan_converter_t;

static void
tor_destroy (void *converter)
{
    cairo_tor_scan_converter_t *self = converter;
    if (self == NULL) {
	return;
    }
    _glitter_scan_converter_fini (self->converter);
    free(self);
}

px_status_t
tor_add_edges (void *converter, const px_edge_t *edges, int count)
{
    cairo_tor_scan_converter_t *self = converter;
    px_status_t status;
    int i;

    if ((status = setjmp (self->jmp)))
	return px_scan_converter_set_error (self, status);

    for (i = 0; i < count; i++)
	 glitter_scan_converter_add_edge (self->converter, &edges[i]);

    return PX_STATUS_SUCCESS;
}

static px_status_t
tor_generate (void			*converter,
				    px_span_renderer_t	*renderer)
{
    cairo_tor_scan_converter_t *self = converter;
    px_status_t status;

    if ((status = setjmp (self->jmp)))
	return px_scan_converter_set_error (self, px_error (status));

    glitter_scan_converter_render (self->converter,
				   self->fill_rule == PX_FILL_RULE_WINDING ? ~0 : 1,
				   self->antialias != PX_ANTIALIAS_NONE,
				   renderer);
    return PX_STATUS_SUCCESS;
}

px_scan_converter_t *
tor_create (int			xmin,
				  int			ymin,
				  int			xmax,
				  int			ymax,
				  px_fill_rule_t	fill_rule,
				  px_antialias_t	antialias)
{
    cairo_tor_scan_converter_t *self;
    px_status_t status;

    self = px_calloc (sizeof(struct _cairo_tor_scan_converter));
    if (unlikely (self == NULL)) {
	status = px_error (PX_STATUS_NO_MEMORY);
	goto bail_nomem;
    }

    self->base.destroy = tor_destroy;
    self->base.generate = tor_generate;

    _glitter_scan_converter_init (self->converter, &self->jmp);
    status = glitter_scan_converter_reset (self->converter,
					   xmin, ymin, xmax, ymax);
    if (unlikely (status))
	goto bail;

    self->fill_rule = fill_rule;
    self->antialias = antialias;

    return &self->base;

 bail:
    self->base.destroy(&self->base);
 bail_nomem:
    return NULL;
}
