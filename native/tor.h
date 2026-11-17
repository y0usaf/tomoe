#ifndef TOR_H
#define TOR_H

#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef int px_status_t;
#define PX_STATUS_SUCCESS 0
#define PX_STATUS_NO_MEMORY 1

#define PX_FIXED_FRAC_BITS 8
typedef int32_t px_fixed_t;

typedef struct { px_fixed_t x, y; } px_point_t;

typedef struct {
    px_point_t p1, p2;
} px_line_t;

typedef struct {
    px_line_t line;
    px_fixed_t top, bottom;
    int dir;
} px_edge_t;

typedef struct {
    px_point_t p1, p2;
} px_box_t;

typedef enum {
    PX_FILL_RULE_WINDING,
    PX_FILL_RULE_EVEN_ODD
} px_fill_rule_t;

typedef enum {
    PX_ANTIALIAS_DEFAULT,
    PX_ANTIALIAS_NONE,
    PX_ANTIALIAS_GRAY,
    PX_ANTIALIAS_SUBPIXEL,
    PX_ANTIALIAS_FAST,
    PX_ANTIALIAS_GOOD,
    PX_ANTIALIAS_BEST
} px_antialias_t;

typedef struct _cairo_half_open_span {
    int32_t x;
    uint8_t coverage;
    uint8_t inverse;
} px_half_open_span_t;

typedef struct _cairo_span_renderer px_span_renderer_t;
struct _cairo_span_renderer {
    px_status_t status;
    void (*destroy) (void *);
    px_status_t (*render_rows) (void *abstract_renderer, int y, int height,
        const px_half_open_span_t *coverages, unsigned num_coverages);
    px_status_t (*finish) (void *abstract_renderer);
};

typedef struct _cairo_scan_converter px_scan_converter_t;
struct _cairo_scan_converter {
    void (*destroy) (void *);
    px_status_t (*generate) (void *abstract_converter, px_span_renderer_t *renderer);
    px_status_t status;
};

px_scan_converter_t *tor_create(int xmin, int ymin, int xmax, int ymax,
    px_fill_rule_t fill_rule, px_antialias_t antialias);
px_status_t tor_add_edges(void *converter, const px_edge_t *edges, int count);

static inline px_status_t px_error(px_status_t status) { return status; }

static inline px_status_t
px_scan_converter_set_error(void *abstract_converter, px_status_t status)
{
    px_scan_converter_t *converter = abstract_converter;
    converter->status = status;
    return status;
}


#define px_malloc_ab(n, s) malloc((size_t)(n) * (s))
#define px_malloc(n) malloc(n)
#define px_calloc(n) calloc(1, (n))

#ifndef unlikely
#define unlikely(x) __builtin_expect(!!(x), 0)
#endif

#ifndef ARRAY_LENGTH
#define ARRAY_LENGTH(a) ((int)(sizeof(a) / sizeof((a)[0])))
#endif

#ifndef MAX
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#endif

#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif

#endif
