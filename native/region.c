#include "region.h"
#include <stdlib.h>
#include <string.h>

#define REGION_MIN INT32_MIN
#define REGION_MAX INT32_MAX

struct build {
    struct rect *rects;
    int count, capacity;
};

static const struct rect *region_rects(const struct region *region) {
    return region->rects ? region->rects : &region->extents;
}

static bool build_add(struct build *build, int32_t x1, int32_t y1, int32_t x2, int32_t y2) {
    if (x1 >= x2 || y1 >= y2) return true;
    if (build->count == build->capacity) {
        int capacity = build->capacity ? build->capacity * 2 : 8;
        struct rect *rects = realloc(build->rects, (size_t)capacity * sizeof(*rects));
        if (!rects) return false;
        build->rects = rects;
        build->capacity = capacity;
    }
    build->rects[build->count++] = (struct rect){ x1, y1, x2, y2 };
    return true;
}

static bool build_clipped(struct build *build, const struct rect *first, const struct rect *last,
        int32_t y1, int32_t y2) {
    for (const struct rect *rect = first; rect != last; rect++)
        if (!build_add(build, rect->x1, y1, rect->x2, y2)) return false;
    return true;
}

static bool build_append(struct build *build, const struct rect *first, const struct rect *last) {
    for (const struct rect *rect = first; rect != last; rect++)
        if (!build_add(build, rect->x1, rect->y1, rect->x2, rect->y2)) return false;
    return true;
}

static bool build_union(struct build *build, const struct rect *first, const struct rect *last,
        const struct rect *other, const struct rect *other_last, int32_t y1, int32_t y2) {
    int32_t x1, x2;
    if (first->x1 < other->x1) {
        x1 = first->x1;
        x2 = first->x2;
        first++;
    } else {
        x1 = other->x1;
        x2 = other->x2;
        other++;
    }
    while (first != last && other != other_last) {
        const struct rect *next;
        if (first->x1 < other->x1) next = first++;
        else next = other++;
        if (next->x1 <= x2) {
            if (next->x2 > x2) x2 = next->x2;
        } else {
            if (!build_add(build, x1, y1, x2, y2)) return false;
            x1 = next->x1;
            x2 = next->x2;
        }
    }
    for (const struct rect *rect = first; rect != last; rect++) {
        if (rect->x1 <= x2) {
            if (rect->x2 > x2) x2 = rect->x2;
        } else {
            if (!build_add(build, x1, y1, x2, y2)) return false;
            x1 = rect->x1;
            x2 = rect->x2;
        }
    }
    for (const struct rect *rect = other; rect != other_last; rect++) {
        if (rect->x1 <= x2) {
            if (rect->x2 > x2) x2 = rect->x2;
        } else {
            if (!build_add(build, x1, y1, x2, y2)) return false;
            x1 = rect->x1;
            x2 = rect->x2;
        }
    }
    return build_add(build, x1, y1, x2, y2);
}

static bool build_intersect(struct build *build, const struct rect *first, const struct rect *last,
        const struct rect *other, const struct rect *other_last, int32_t y1, int32_t y2) {
    do {
        int32_t x1 = first->x1 > other->x1 ? first->x1 : other->x1;
        int32_t x2 = first->x2 < other->x2 ? first->x2 : other->x2;
        if (x1 < x2 && !build_add(build, x1, y1, x2, y2)) return false;
        if (first->x2 == x2) first++;
        if (other->x2 == x2) other++;
    } while (first != last && other != other_last);
    return true;
}

static bool build_subtract(struct build *build, const struct rect *first, const struct rect *last,
        const struct rect *other, const struct rect *other_last, int32_t y1, int32_t y2) {
    int32_t x1 = first->x1;
    do {
        if (other->x2 <= x1) {
            other++;
        } else if (other->x1 <= x1) {
            x1 = other->x2;
            if (x1 >= first->x2) {
                first++;
                if (first != last) x1 = first->x1;
            } else {
                other++;
            }
        } else if (other->x1 < first->x2) {
            if (!build_add(build, x1, y1, other->x1, y2)) return false;
            x1 = other->x2;
            if (x1 >= first->x2) {
                first++;
                if (first != last) x1 = first->x1;
            } else {
                other++;
            }
        } else {
            if (first->x2 > x1 && !build_add(build, x1, y1, first->x2, y2)) return false;
            first++;
            if (first != last) x1 = first->x1;
        }
    } while (first != last && other != other_last);
    while (first != last) {
        if (!build_add(build, x1, y1, first->x2, y2)) return false;
        first++;
        if (first != last) x1 = first->x1;
    }
    return true;
}

static void build_coalesce(struct build *build, int *previous, int current) {
    int count = current - *previous;
    if (!count || count != build->count - current) {
        *previous = current;
        return;
    }
    if (build->rects[*previous].y2 != build->rects[current].y1) {
        *previous = current;
        return;
    }
    for (int i = 0; i < count; i++)
        if (build->rects[*previous + i].x1 != build->rects[current + i].x1 ||
                build->rects[*previous + i].x2 != build->rects[current + i].x2) {
            *previous = current;
            return;
        }
    int32_t y2 = build->rects[current].y2;
    for (int i = 0; i < count; i++) build->rects[*previous + i].y2 = y2;
    build->count = current;
}

enum combine { COMBINE_UNION, COMBINE_INTERSECT, COMBINE_SUBTRACT };

static void region_install(struct region *region, struct build *build) {
    free(region->rects);
    region->rects = build->rects;
    region->count = build->count;
    region->capacity = build->capacity;
    if (!build->count) {
        region->extents = (struct rect){ 0, 0, 0, 0 };
    } else if (build->count == 1) {
        region->extents = build->rects[0];
        free(build->rects);
        region->rects = NULL;
        region->capacity = 0;
    } else {
        struct rect extents = build->rects[0];
        for (int i = 1; i < build->count; i++) {
            struct rect rect = build->rects[i];
            if (rect.x1 < extents.x1) extents.x1 = rect.x1;
            if (rect.y1 < extents.y1) extents.y1 = rect.y1;
            if (rect.x2 > extents.x2) extents.x2 = rect.x2;
            if (rect.y2 > extents.y2) extents.y2 = rect.y2;
        }
        region->extents = extents;
    }
    build->rects = NULL;
    build->count = build->capacity = 0;
}

static void region_combine(struct region *dst, const struct region *a, const struct region *b,
        enum combine op) {
    struct build build = { NULL, 0, 0 };
    const struct rect *first = region_rects(a), *first_end = first + a->count;
    const struct rect *other = region_rects(b), *other_end = other + b->count;
    bool append_first = op == COMBINE_UNION || op == COMBINE_SUBTRACT;
    bool append_other = op == COMBINE_UNION;
    if (!a->count || !b->count) {
        if (append_first && !build_append(&build, first, first_end)) goto out;
        if (append_other && !build_append(&build, other, other_end)) goto out;
        region_install(dst, &build);
        return;
    }
    int32_t bottom = first->y1 < other->y1 ? first->y1 : other->y1;
    int previous = 0;
    while (first != first_end && other != other_end) {
        int32_t first_y1 = first->y1, other_y1 = other->y1;
        const struct rect *first_band = first + 1, *other_band = other + 1;
        while (first_band != first_end && first_band->y1 == first_y1) first_band++;
        while (other_band != other_end && other_band->y1 == other_y1) other_band++;
        int32_t top;
        if (first_y1 < other_y1) {
            if (append_first) {
                int32_t start = first_y1 > bottom ? first_y1 : bottom;
                int32_t end = first->y2 < other_y1 ? first->y2 : other_y1;
                if (start != end) {
                    int current = build.count;
                    if (!build_clipped(&build, first, first_band, start, end)) goto out;
                    build_coalesce(&build, &previous, current);
                }
            }
            top = other_y1;
        } else if (other_y1 < first_y1) {
            if (append_other) {
                int32_t start = other_y1 > bottom ? other_y1 : bottom;
                int32_t end = other->y2 < first_y1 ? other->y2 : first_y1;
                if (start != end) {
                    int current = build.count;
                    if (!build_clipped(&build, other, other_band, start, end)) goto out;
                    build_coalesce(&build, &previous, current);
                }
            }
            top = first_y1;
        } else {
            top = first_y1;
        }
        bottom = first->y2 < other->y2 ? first->y2 : other->y2;
        if (bottom > top) {
            int current = build.count;
            bool ok = op == COMBINE_UNION
                ? build_union(&build, first, first_band, other, other_band, top, bottom)
                : op == COMBINE_INTERSECT
                ? build_intersect(&build, first, first_band, other, other_band, top, bottom)
                : build_subtract(&build, first, first_band, other, other_band, top, bottom);
            if (!ok) goto out;
            build_coalesce(&build, &previous, current);
        }
        if (first->y2 == bottom) first = first_band;
        if (other->y2 == bottom) other = other_band;
    }
    if (first != first_end && append_first) {
        const struct rect *band_end = first + 1;
        while (band_end != first_end && band_end->y1 == first->y1) band_end++;
        int32_t start = first->y1 > bottom ? first->y1 : bottom;
        int current = build.count;
        if (!build_clipped(&build, first, band_end, start, first->y2)) goto out;
        build_coalesce(&build, &previous, current);
        if (!build_append(&build, band_end, first_end)) goto out;
    }
    if (other != other_end && append_other) {
        const struct rect *band_end = other + 1;
        while (band_end != other_end && band_end->y1 == other->y1) band_end++;
        int32_t start = other->y1 > bottom ? other->y1 : bottom;
        int current = build.count;
        if (!build_clipped(&build, other, band_end, start, other->y2)) goto out;
        build_coalesce(&build, &previous, current);
        if (!build_append(&build, band_end, other_end)) goto out;
    }
    region_install(dst, &build);
    return;
out:
    free(build.rects);
    region_install(dst, &(struct build){ NULL, 0, 0 });
}

void region_init(struct region *region) {
    *region = (struct region){ { 0, 0, 0, 0 }, NULL, 0, 0 };
}

void region_init_rect(struct region *region, int x, int y, uint32_t width, uint32_t height) {
    int32_t x1 = x, y1 = y;
    int32_t x2 = (int32_t)((uint32_t)x + width), y2 = (int32_t)((uint32_t)y + height);
    region_init(region);
    if (x1 >= x2 || y1 >= y2) return;
    region->extents = (struct rect){ x1, y1, x2, y2 };
    region->count = 1;
}

static int compare_rects(const void *left, const void *right) {
    const struct rect *a = left, *b = right;
    if (a->y1 != b->y1) return a->y1 < b->y1 ? -1 : 1;
    if (a->x1 != b->x1) return a->x1 < b->x1 ? -1 : 1;
    if (a->y2 != b->y2) return a->y2 < b->y2 ? -1 : 1;
    if (a->x2 != b->x2) return a->x2 < b->x2 ? -1 : 1;
    return 0;
}

void region_init_rects(struct region *region, const struct rect *rects, int count) {
    region_init(region);
    if (count <= 0) return;
    struct rect *sorted = malloc((size_t)count * sizeof(*sorted));
    struct region *parts = calloc((size_t)count, sizeof(*parts));
    if (!sorted || !parts) {
        free(sorted);
        free(parts);
        return;
    }
    int kept = 0;
    for (int i = 0; i < count; i++)
        if (rects[i].x1 < rects[i].x2 && rects[i].y1 < rects[i].y2) sorted[kept++] = rects[i];
    if (kept > 1) qsort(sorted, (size_t)kept, sizeof(*sorted), compare_rects);
    for (int i = 0; i < kept; i++)
        region_init_rect(&parts[i], sorted[i].x1, sorted[i].y1,
            (uint32_t)(sorted[i].x2 - sorted[i].x1), (uint32_t)(sorted[i].y2 - sorted[i].y1));
    for (int num = kept; num > 1;) {
        int half = num / 2, first = num & 1;
        for (int j = first; j < half + first; j++) {
            region_union(&parts[j], &parts[j], &parts[j + half]);
            region_fini(&parts[j + half]);
        }
        num -= half;
    }
    if (kept) {
        *region = parts[0];
        parts[0] = (struct region){ { 0, 0, 0, 0 }, NULL, 0, 0 };
    }
    free(parts);
    free(sorted);
}

void region_fini(struct region *region) {
    free(region->rects);
    region_init(region);
}

void region_copy(struct region *dst, const struct region *src) {
    if (dst == src) return;
    free(dst->rects);
    dst->count = src->count;
    dst->capacity = src->count > 1 ? src->count : 0;
    dst->extents = src->extents;
    dst->rects = NULL;
    if (src->count > 1) {
        dst->rects = malloc((size_t)src->count * sizeof(*dst->rects));
        if (!dst->rects) {
            dst->count = dst->capacity = 0;
            dst->extents = (struct rect){ 0, 0, 0, 0 };
            return;
        }
        memcpy(dst->rects, src->rects, (size_t)src->count * sizeof(*dst->rects));
    }
}

void region_clear(struct region *region) {
    free(region->rects);
    region->rects = NULL;
    region->count = region->capacity = 0;
    region->extents = (struct rect){ 0, 0, 0, 0 };
}

bool region_empty(const struct region *region) { return !region->count; }

bool region_not_empty(const struct region *region) { return region->count != 0; }

struct rect *region_extents(const struct region *region) {
    return (struct rect *)&region->extents;
}

struct rect *region_rectangles(const struct region *region, int *count) {
    if (count) *count = region->count;
    return (struct rect *)region_rects(region);
}

static const struct rect *find_band(const struct rect *first, const struct rect *last, int32_t y) {
    int low = 0, high = (int)(last - first);
    while (low < high) {
        int middle = (low + high) / 2;
        if (first[middle].y2 <= y) low = middle + 1;
        else high = middle;
    }
    return first + low;
}

bool region_contains_point(const struct region *region, int x, int y, struct rect *box) {
    if (!region->count) return false;
    if (x < region->extents.x1 || x >= region->extents.x2 ||
            y < region->extents.y1 || y >= region->extents.y2) return false;
    if (region->count == 1) {
        if (box) *box = region->extents;
        return true;
    }
    const struct rect *rect = region->rects, *last = rect + region->count;
    rect = find_band(rect, last, y);
    for (; rect != last; rect++) {
        if (y < rect->y1 || x < rect->x1) break;
        if (x >= rect->x2) continue;
        if (box) *box = *rect;
        return true;
    }
    return false;
}

enum region_overlap region_contains_rectangle(const struct region *region, const struct rect *rect) {
    if (!region->count) return REGION_OUT;
    const struct rect *extents = &region->extents;
    if (rect->x2 <= extents->x1 || rect->x1 >= extents->x2 ||
            rect->y2 <= extents->y1 || rect->y1 >= extents->y2) return REGION_OUT;
    if (region->count == 1)
        return rect->x1 >= extents->x1 && rect->x2 <= extents->x2 &&
            rect->y1 >= extents->y1 && rect->y2 <= extents->y2 ? REGION_IN : REGION_PART;
    bool part_in = false, part_out = false;
    int32_t x = rect->x1, y = rect->y1;
    const struct rect *box = region->rects, *last = box + region->count;
    for (; box != last; box++) {
        if (box->y2 <= y) {
            box = find_band(box, last, y);
            if (box == last) break;
        }
        if (box->y1 > y) {
            part_out = true;
            if (part_in || box->y1 >= rect->y2) break;
            y = box->y1;
            x = rect->x1;
        }
        if (box->x2 <= x) continue;
        if (box->x1 > x) {
            part_out = true;
            if (part_in) break;
        }
        if (box->x1 < rect->x2) {
            part_in = true;
            if (part_out) break;
        }
        if (box->x2 >= rect->x2) {
            y = box->y2;
            if (y >= rect->y2) break;
            x = rect->x1;
        } else {
            part_out = true;
            break;
        }
    }
    if (!part_in) return REGION_OUT;
    return y < rect->y2 ? REGION_PART : REGION_IN;
}

void region_intersect(struct region *dst, const struct region *a, const struct region *b) {
    region_combine(dst, a, b, COMBINE_INTERSECT);
}

void region_union(struct region *dst, const struct region *a, const struct region *b) {
    region_combine(dst, a, b, COMBINE_UNION);
}

void region_subtract(struct region *dst, const struct region *a, const struct region *b) {
    region_combine(dst, a, b, COMBINE_SUBTRACT);
}

void region_intersect_rect(struct region *dst, const struct region *src,
        int x, int y, uint32_t width, uint32_t height) {
    struct region rect;
    region_init_rect(&rect, x, y, width, height);
    region_intersect(dst, src, &rect);
}

void region_union_rect(struct region *dst, const struct region *src,
        int x, int y, uint32_t width, uint32_t height) {
    struct region rect;
    region_init_rect(&rect, x, y, width, height);
    region_union(dst, src, &rect);
}

void region_translate(struct region *region, int x, int y) {
    if (!x && !y) return;
    int64_t x1 = (int64_t)region->extents.x1 + x, y1 = (int64_t)region->extents.y1 + y;
    int64_t x2 = (int64_t)region->extents.x2 + x, y2 = (int64_t)region->extents.y2 + y;
    if (x2 <= REGION_MIN || y2 <= REGION_MIN || x1 >= REGION_MAX || y1 >= REGION_MAX) {
        region_clear(region);
        return;
    }
    if (region->count > 1)
        for (int i = 0; i < region->count; i++) {
            struct rect *rect = &region->rects[i];
            rect->x1 = (int32_t)(rect->x1 + (int64_t)x);
            rect->y1 = (int32_t)(rect->y1 + (int64_t)y);
            rect->x2 = (int32_t)(rect->x2 + (int64_t)x);
            rect->y2 = (int32_t)(rect->y2 + (int64_t)y);
        }
    region->extents = (struct rect){ (int32_t)x1, (int32_t)y1, (int32_t)x2, (int32_t)y2 };
}
