#include "internal.h"
#include <stdarg.h>
#include <sys/eventfd.h>
#include <unistd.h>
#include <xf86drm.h>

int log_verbosity = LOG_ERROR;

void tomoe_log(int level, const char *fmt, ...) {
    if (level > log_verbosity) return;
    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    fputc('\n', stderr);
}

bool box_empty(const struct box *box) {
    return !box || box->width <= 0 || box->height <= 0;
}

bool box_equal(const struct box *a, const struct box *b) {
    if (box_empty(a) || box_empty(b)) return box_empty(a) && box_empty(b);
    return a->x == b->x && a->y == b->y && a->width == b->width && a->height == b->height;
}

bool box_intersection(struct box *dest, const struct box *a, const struct box *b) {
    struct box out = { 0 };
    if (!box_empty(a) && !box_empty(b)) {
        out.x = a->x > b->x ? a->x : b->x;
        out.y = a->y > b->y ? a->y : b->y;
        int x2 = a->x + a->width < b->x + b->width ? a->x + a->width : b->x + b->width;
        int y2 = a->y + a->height < b->y + b->height ? a->y + a->height : b->y + b->height;
        out.width = x2 - out.x;
        out.height = y2 - out.y;
        if (box_empty(&out)) out = (struct box){ 0 };
    }
    *dest = out;
    return !box_empty(&out);
}

void fbox_transform(struct fbox *dest, const struct fbox *box, enum wl_output_transform transform,
        double width, double height) {
    struct fbox s = *box;
    double right = width - s.x - s.width, bottom = height - s.y - s.height;
    struct fbox d = { .width = s.width, .height = s.height };
    if (transform & WL_OUTPUT_TRANSFORM_90) {
        d.width = s.height;
        d.height = s.width;
    }
    switch (transform) {
    case WL_OUTPUT_TRANSFORM_NORMAL: d.x = s.x; d.y = s.y; break;
    case WL_OUTPUT_TRANSFORM_90: d.x = bottom; d.y = s.x; break;
    case WL_OUTPUT_TRANSFORM_180: d.x = right; d.y = bottom; break;
    case WL_OUTPUT_TRANSFORM_270: d.x = s.y; d.y = right; break;
    case WL_OUTPUT_TRANSFORM_FLIPPED: d.x = right; d.y = s.y; break;
    case WL_OUTPUT_TRANSFORM_FLIPPED_90: d.x = s.y; d.y = s.x; break;
    case WL_OUTPUT_TRANSFORM_FLIPPED_180: d.x = s.x; d.y = bottom; break;
    case WL_OUTPUT_TRANSFORM_FLIPPED_270: d.x = bottom; d.y = right; break;
    }
    *dest = d;
}

void box_transform(struct box *dest, const struct box *box, enum wl_output_transform transform,
        int width, int height) {
    struct fbox f = { box->x, box->y, box->width, box->height };
    fbox_transform(&f, &f, transform, width, height);
    *dest = (struct box){ (int)f.x, (int)f.y, (int)f.width, (int)f.height };
}

enum wl_output_transform transform_invert(enum wl_output_transform transform) {
    if ((transform & WL_OUTPUT_TRANSFORM_90) && !(transform & WL_OUTPUT_TRANSFORM_FLIPPED))
        transform ^= WL_OUTPUT_TRANSFORM_180;
    return transform;
}

enum wl_output_transform transform_compose(enum wl_output_transform a,
        enum wl_output_transform b) {
    uint32_t flipped = (a ^ b) & WL_OUTPUT_TRANSFORM_FLIPPED;
    uint32_t rotation = WL_OUTPUT_TRANSFORM_90 | WL_OUTPUT_TRANSFORM_180;
    uint32_t rotated = (b & WL_OUTPUT_TRANSFORM_FLIPPED ? b - a : a + b) & rotation;
    return flipped | rotated;
}

void transform_coords(enum wl_output_transform transform, int *x, int *y) {
    if (!(transform & WL_OUTPUT_TRANSFORM_90)) return;
    int t = *x;
    *x = *y;
    *y = t;
}

static void region_map(pixman_region32_t *dst, const pixman_region32_t *src, double sx, double sy,
        enum wl_output_transform transform, int width, int height) {
    int count = 0;
    const pixman_box32_t *rects = pixman_region32_rectangles(src, &count);
    pixman_box32_t *out = malloc((count ? count : 1) * sizeof(*out));
    if (!out) return;
    for (int i = 0; i < count; i++) {
        struct fbox f = { floor(rects[i].x1 * sx), floor(rects[i].y1 * sy), 0, 0 };
        f.width = ceil(rects[i].x2 * sx) - f.x;
        f.height = ceil(rects[i].y2 * sy) - f.y;
        fbox_transform(&f, &f, transform, width, height);
        out[i] = (pixman_box32_t){ f.x, f.y, f.x + f.width, f.y + f.height };
    }
    pixman_region32_fini(dst);
    pixman_region32_init_rects(dst, out, count);
    free(out);
}

void region_scale_xy(pixman_region32_t *dst, const pixman_region32_t *src, float sx, float sy) {
    if (sx == 1 && sy == 1) pixman_region32_copy(dst, src);
    else region_map(dst, src, sx, sy, WL_OUTPUT_TRANSFORM_NORMAL, 0, 0);
}

void region_scale(pixman_region32_t *dst, const pixman_region32_t *src, float scale) {
    region_scale_xy(dst, src, scale, scale);
}

void region_transform(pixman_region32_t *dst, const pixman_region32_t *src,
        enum wl_output_transform transform, int width, int height) {
    if (transform == WL_OUTPUT_TRANSFORM_NORMAL) pixman_region32_copy(dst, src);
    else region_map(dst, src, 1, 1, transform, width, height);
}

void addon_init(struct addon *addon, struct wl_list *addons, const void *owner,
        void (*destroy)(struct addon *addon)) {
    addon->owner = owner;
    addon->destroy = destroy;
    wl_list_insert(addons, &addon->link);
}

void addon_finish(struct addon *addon) {
    wl_list_remove(&addon->link);
}

struct addon *addon_find(struct wl_list *addons, const void *owner,
        void (*destroy)(struct addon *addon)) {
    struct addon *addon;
    wl_list_for_each(addon, addons, link)
        if (addon->owner == owner && addon->destroy == destroy) return addon;
    return NULL;
}

void dmabuf_attributes_finish(struct dmabuf_attributes *attributes) {
    for (int i = 0; i < attributes->n_planes; i++)
        if (attributes->fd[i] >= 0) close(attributes->fd[i]);
    attributes->n_planes = 0;
}

void buffer_init(struct buffer *buffer, const struct buffer_impl *impl, int width, int height) {
    *buffer = (struct buffer){ .impl = impl, .width = width, .height = height };
    wl_list_init(&buffer->addons);
    wl_signal_init(&buffer->events.destroy);
    wl_signal_init(&buffer->events.release);
}

void buffer_finish(struct buffer *buffer) {
    wl_signal_emit_mutable(&buffer->events.destroy, NULL);
    struct addon *addon, *next;
    wl_list_for_each_safe(addon, next, &buffer->addons, link) addon->destroy(addon);
}

static void buffer_consider(struct buffer *buffer) {
    if (buffer->dropped && !buffer->n_locks && !buffer->accessing) buffer->impl->destroy(buffer);
}

void buffer_drop(struct buffer *buffer) {
    if (!buffer) return;
    buffer->dropped = true;
    buffer_consider(buffer);
}

struct buffer *buffer_lock(struct buffer *buffer) {
    buffer->n_locks++;
    return buffer;
}

void buffer_unlock(struct buffer *buffer) {
    if (!buffer) return;
    if (--buffer->n_locks == 0) wl_signal_emit_mutable(&buffer->events.release, NULL);
    buffer_consider(buffer);
}

bool buffer_get_dmabuf(struct buffer *buffer, struct dmabuf_attributes *attributes) {
    return buffer->impl->get_dmabuf && buffer->impl->get_dmabuf(buffer, attributes);
}

bool buffer_begin_access(struct buffer *buffer, uint32_t flags, void **data, uint32_t *format,
        size_t *stride) {
    if (!buffer->impl->begin_access ||
            !buffer->impl->begin_access(buffer, flags, data, format, stride)) return false;
    buffer->accessing = true;
    return true;
}

void buffer_end_access(struct buffer *buffer) {
    if (buffer->impl->end_access) buffer->impl->end_access(buffer);
    buffer->accessing = false;
    buffer_consider(buffer);
}

struct format *format_set_get(const struct format_set *set, uint32_t format) {
    for (size_t i = 0; i < set->len; i++)
        if (set->formats[i].format == format) return &set->formats[i];
    return NULL;
}

bool format_has(const struct format *format, uint64_t modifier) {
    for (size_t i = 0; format && i < format->len; i++)
        if (format->modifiers[i] == modifier) return true;
    return false;
}

bool format_set_has(const struct format_set *set, uint32_t format, uint64_t modifier) {
    return format_has(format_set_get(set, format), modifier);
}

bool format_set_add(struct format_set *set, uint32_t format, uint64_t modifier) {
    struct format *f = format_set_get(set, format);
    if (!f) {
        struct format *formats = realloc(set->formats, (set->len + 1) * sizeof(*formats));
        if (!formats) return false;
        set->formats = formats;
        f = &formats[set->len++];
        *f = (struct format){ .format = format };
    }
    if (format_has(f, modifier)) return true;
    uint64_t *modifiers = realloc(f->modifiers, (f->len + 1) * sizeof(*modifiers));
    if (!modifiers) return false;
    f->modifiers = modifiers;
    f->modifiers[f->len++] = modifier;
    return true;
}

void format_set_finish(struct format_set *set) {
    for (size_t i = 0; i < set->len; i++) free(set->formats[i].modifiers);
    free(set->formats);
    *set = (struct format_set){ 0 };
}

static struct timeline *timeline_wrap(int drm_fd, uint32_t handle) {
    struct timeline *timeline = calloc(1, sizeof(*timeline));
    if (!timeline) {
        drmSyncobjDestroy(drm_fd, handle);
        return NULL;
    }
    *timeline = (struct timeline){ .drm_fd = drm_fd, .handle = handle, .refs = 1 };
    return timeline;
}

struct timeline *timeline_create(int drm_fd) {
    uint32_t handle = 0;
    if (drmSyncobjCreate(drm_fd, 0, &handle) == 0) return timeline_wrap(drm_fd, handle);
    tomoe_log(LOG_ERROR, "tomoe: drmSyncobjCreate failed: %s", strerror(errno));
    return NULL;
}

struct timeline *timeline_import(int drm_fd, int syncobj_fd) {
    uint32_t handle = 0;
    if (drmSyncobjFDToHandle(drm_fd, syncobj_fd, &handle) == 0) return timeline_wrap(drm_fd, handle);
    tomoe_log(LOG_ERROR, "tomoe: drmSyncobjFDToHandle failed: %s", strerror(errno));
    return NULL;
}

struct timeline *timeline_ref(struct timeline *timeline) {
    timeline->refs++;
    return timeline;
}

void timeline_unref(struct timeline *timeline) {
    if (!timeline || --timeline->refs) return;
    drmSyncobjDestroy(timeline->drm_fd, timeline->handle);
    free(timeline);
}

int timeline_export_sync_file(struct timeline *timeline, uint64_t point) {
    uint32_t handle;
    int fd = -1;
    if (drmSyncobjCreate(timeline->drm_fd, 0, &handle) != 0) return -1;
    if (drmSyncobjTransfer(timeline->drm_fd, handle, 0, timeline->handle, point, 0) != 0 ||
            drmSyncobjExportSyncFile(timeline->drm_fd, handle, &fd) != 0) {
        tomoe_log(LOG_ERROR, "tomoe: sync file export failed: %s", strerror(errno));
        fd = -1;
    }
    drmSyncobjDestroy(timeline->drm_fd, handle);
    return fd;
}

bool timeline_import_sync_file(struct timeline *timeline, uint64_t point, int fd) {
    uint32_t handle;
    if (drmSyncobjCreate(timeline->drm_fd, 0, &handle) != 0) return false;
    bool ok = drmSyncobjImportSyncFile(timeline->drm_fd, handle, fd) == 0 &&
        drmSyncobjTransfer(timeline->drm_fd, timeline->handle, point, handle, 0, 0) == 0;
    if (!ok) tomoe_log(LOG_ERROR, "tomoe: sync file import failed: %s", strerror(errno));
    drmSyncobjDestroy(timeline->drm_fd, handle);
    return ok;
}

bool timeline_check(struct timeline *timeline, uint64_t point, uint32_t flags, bool *ready) {
    int ret = drmSyncobjTimelineWait(timeline->drm_fd, &timeline->handle, &point, 1, 0, flags,
        NULL);
    if (ret != 0 && ret != -ETIME) {
        tomoe_log(LOG_ERROR, "tomoe: drmSyncobjTimelineWait failed: %s", strerror(errno));
        return false;
    }
    *ready = ret == 0;
    return true;
}

bool timeline_signal(struct timeline *timeline, uint64_t point) {
    if (drmSyncobjTimelineSignal(timeline->drm_fd, &timeline->handle, &point, 1) == 0) return true;
    tomoe_log(LOG_ERROR, "tomoe: drmSyncobjTimelineSignal failed");
    return false;
}

static int waiter_ready(int fd, uint32_t mask, void *data) {
    struct timeline_waiter *waiter = data;
    uint64_t value;
    if ((mask & WL_EVENT_READABLE) && read(fd, &value, sizeof(value)) <= 0)
        tomoe_log(LOG_ERROR, "tomoe: timeline eventfd read failed");
    waiter->callback(waiter);
    return 0;
}

bool timeline_waiter_init(struct timeline_waiter *waiter, struct timeline *timeline,
        uint64_t point, uint32_t flags, struct wl_event_loop *loop,
        void (*callback)(struct timeline_waiter *waiter)) {
    int fd = eventfd(0, EFD_CLOEXEC);
    if (fd < 0) return false;
    struct drm_syncobj_eventfd request = { .handle = timeline->handle, .flags = flags,
        .point = point, .fd = fd };
    struct wl_event_source *source = drmIoctl(timeline->drm_fd, DRM_IOCTL_SYNCOBJ_EVENTFD,
        &request) == 0 ? wl_event_loop_add_fd(loop, fd, WL_EVENT_READABLE, waiter_ready, waiter) :
        NULL;
    if (!source) {
        tomoe_log(LOG_ERROR, "tomoe: timeline wait setup failed: %s", strerror(errno));
        close(fd);
        return false;
    }
    *waiter = (struct timeline_waiter){ .fd = fd, .source = source, .callback = callback };
    return true;
}

void timeline_waiter_finish(struct timeline_waiter *waiter) {
    wl_event_source_remove(waiter->source);
    close(waiter->fd);
}

static const uint32_t anchor_edges[] = {
    [XDG_POSITIONER_ANCHOR_NONE] = EDGE_NONE,
    [XDG_POSITIONER_ANCHOR_TOP] = EDGE_TOP,
    [XDG_POSITIONER_ANCHOR_BOTTOM] = EDGE_BOTTOM,
    [XDG_POSITIONER_ANCHOR_LEFT] = EDGE_LEFT,
    [XDG_POSITIONER_ANCHOR_RIGHT] = EDGE_RIGHT,
    [XDG_POSITIONER_ANCHOR_TOP_LEFT] = EDGE_TOP | EDGE_LEFT,
    [XDG_POSITIONER_ANCHOR_BOTTOM_LEFT] = EDGE_BOTTOM | EDGE_LEFT,
    [XDG_POSITIONER_ANCHOR_TOP_RIGHT] = EDGE_TOP | EDGE_RIGHT,
    [XDG_POSITIONER_ANCHOR_BOTTOM_RIGHT] = EDGE_BOTTOM | EDGE_RIGHT,
};

static uint32_t edges_invert(uint32_t anchor, uint32_t axis) {
    uint32_t edges = anchor_edges[anchor];
    if (edges & axis) edges ^= axis;
    for (uint32_t i = 0; i < sizeof(anchor_edges) / sizeof(anchor_edges[0]); i++)
        if (anchor_edges[i] == edges) return i;
    return anchor;
}

void positioner_geometry(const struct positioner_rules *rules, struct box *box) {
    const struct box *a = &rules->anchor_rect;
    *box = (struct box){ rules->offset.x, rules->offset.y, rules->size.width, rules->size.height };
    uint32_t edges = anchor_edges[rules->anchor];
    box->y += edges & EDGE_TOP ? a->y : edges & EDGE_BOTTOM ? a->y + a->height :
        a->y + a->height / 2;
    box->x += edges & EDGE_LEFT ? a->x : edges & EDGE_RIGHT ? a->x + a->width :
        a->x + a->width / 2;
    edges = anchor_edges[rules->gravity];
    if (edges & EDGE_TOP) box->y -= box->height;
    else if (!(edges & EDGE_BOTTOM)) box->y -= box->height / 2;
    if (edges & EDGE_LEFT) box->x -= box->width;
    else if (!(edges & EDGE_RIGHT)) box->x -= box->width / 2;
}

struct overflow { int top, bottom, left, right; };

static struct overflow overflow_of(const struct box *c, const struct box *b) {
    return (struct overflow){ .left = c->x - b->x, .right = b->x + b->width - c->x - c->width,
        .top = c->y - b->y, .bottom = b->y + b->height - c->y - c->height };
}

static bool contained(const struct overflow *o) {
    return o->top <= 0 && o->bottom <= 0 && o->left <= 0 && o->right <= 0;
}

static int slide(int low, int high, bool toward_low) {
    if (low > 0 && high > 0) return toward_low ? -high : low;
    return abs(low) < abs(high) ? low : -high;
}

void positioner_unconstrain(const struct positioner_rules *rules, const struct box *constraint,
        struct box *box) {
    struct overflow o = overflow_of(constraint, box);
    if (contained(&o)) return;
    uint32_t adjust = rules->constraint_adjustment;
    bool flip_x = ((o.left > 0) ^ (o.right > 0)) &&
        (adjust & XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_FLIP_X);
    bool flip_y = ((o.top > 0) ^ (o.bottom > 0)) &&
        (adjust & XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_FLIP_Y);
    if (flip_x || flip_y) {
        struct positioner_rules flipped = *rules;
        uint32_t axis = (flip_x ? EDGE_LEFT | EDGE_RIGHT : 0) | (flip_y ? EDGE_TOP | EDGE_BOTTOM : 0);
        flipped.anchor = edges_invert(flipped.anchor, axis);
        flipped.gravity = edges_invert(flipped.gravity, axis);
        struct box f;
        positioner_geometry(&flipped, &f);
        struct overflow fo = overflow_of(constraint, &f);
        if (fo.left <= 0 && fo.right <= 0) {
            box->x = f.x;
            o.left = fo.left;
            o.right = fo.right;
        }
        if (fo.top <= 0 && fo.bottom <= 0) {
            box->y = f.y;
            o.top = fo.top;
            o.bottom = fo.bottom;
        }
        if (contained(&o)) return;
    }
    bool slide_x = (o.left > 0 || o.right > 0) &&
        (adjust & XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_X);
    bool slide_y = (o.top > 0 || o.bottom > 0) &&
        (adjust & XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_Y);
    if (slide_x || slide_y) {
        uint32_t gravity = anchor_edges[rules->gravity];
        if (slide_x) box->x += slide(o.left, o.right, gravity & EDGE_LEFT);
        if (slide_y) box->y += slide(o.top, o.bottom, gravity & EDGE_TOP);
        o = overflow_of(constraint, box);
        if (contained(&o)) return;
    }
    bool resize_x = (o.left > 0 || o.right > 0) &&
        (adjust & XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_RESIZE_X);
    bool resize_y = (o.top > 0 || o.bottom > 0) &&
        (adjust & XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_RESIZE_Y);
    if (!resize_x && !resize_y) return;
    struct box resized = *box;
    if (resize_x) {
        resized.x += o.left > 0 ? o.left : 0;
        resized.width -= (o.left > 0 ? o.left : 0) + (o.right > 0 ? o.right : 0);
    }
    if (resize_y) {
        resized.y += o.top > 0 ? o.top : 0;
        resized.height -= (o.top > 0 ? o.top : 0) + (o.bottom > 0 ? o.bottom : 0);
    }
    if (!box_empty(&resized)) *box = resized;
}

static uint32_t le32(const uint8_t *p) {
    return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
}

static struct buffer *xcursor_file(const char *path, int size, int *hotspot_x, int *hotspot_y) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    uint8_t header[16], entry[12], image[36];
    struct buffer *buffer = NULL;
    uint32_t best = 0, position = 0;
    if (fread(header, 1, 16, f) == 16 && le32(header) == 0x72756358 && le32(header + 12) < 4096 &&
            fseek(f, le32(header + 4), SEEK_SET) == 0) {
        for (uint32_t i = 0, n = le32(header + 12); i < n && fread(entry, 1, 12, f) == 12; i++) {
            uint32_t nominal = le32(entry + 4);
            if (le32(entry) != 0xfffd0002) continue;
            if (!position || abs((int)nominal - size) < abs((int)best - size)) {
                best = nominal;
                position = le32(entry + 8);
            }
        }
    }
    uint32_t width, height;
    if (position && fseek(f, position, SEEK_SET) == 0 && fread(image, 1, 36, f) == 36 &&
            (width = le32(image + 16)) && (height = le32(image + 20)) && width <= 0x7fff &&
            height <= 0x7fff) {
        size_t bytes = (size_t)width * height * 4;
        uint8_t *pixels = malloc(bytes);
        if (pixels && fread(pixels, 1, bytes, f) == bytes) {
            *hotspot_x = (int)le32(image + 24);
            *hotspot_y = (int)le32(image + 28);
            buffer = pixel_buffer_create(width, height, width * 4, DRM_FORMAT_ARGB8888, pixels);
        }
        free(pixels);
    }
    fclose(f);
    return buffer;
}

static char *xcursor_path(void) {
    const char *env = getenv("XCURSOR_PATH");
    if (env) return strdup(env);
    const char *home = getenv("HOME"), *data = getenv("XDG_DATA_HOME");
    char *dirs = strdup(getenv("XDG_DATA_DIRS") ? getenv("XDG_DATA_DIRS") : ""), *save = NULL;
    char *path = NULL, *next;
    if (asprintf(&path, "%s%s:%s/.icons:/usr/share/icons:/usr/share/pixmaps",
            data ? data : home ? home : "", data ? "/icons" : "/.local/share/icons",
            home ? home : "") < 0) path = NULL;
    for (char *dir = dirs ? strtok_r(dirs, ":", &save) : NULL; dir && path;
            dir = strtok_r(NULL, ":", &save), free(path), path = next)
        if (asprintf(&next, "%s:%s/icons", path, dir) < 0) next = NULL;
    free(dirs);
    return path;
}

static struct buffer *xcursor_theme(const char *path, const char *theme, const char *name,
        int size, int *hotspot_x, int *hotspot_y, int depth) {
    char *dirs = strdup(path), *save = NULL, *file = NULL, *inherits = NULL;
    struct buffer *buffer = NULL;
    for (char *dir = strtok_r(dirs, ":", &save); dir && !buffer; dir = strtok_r(NULL, ":", &save)) {
        if (asprintf(&file, "%s/%s/cursors/%s", dir, theme, name) < 0) break;
        buffer = xcursor_file(file, size, hotspot_x, hotspot_y);
        free(file);
        if (buffer || inherits || asprintf(&file, "%s/%s/index.theme", dir, theme) < 0)
            continue;
        FILE *index = fopen(file, "r");
        free(file);
        char line[512];
        while (index && !inherits && fgets(line, sizeof(line), index))
            if (!strncmp(line, "Inherits", 8) && strchr(line, '='))
                inherits = strdup(strchr(line, '=') + 1);
        if (index) fclose(index);
    }
    free(dirs);
    save = NULL;
    for (char *parent = inherits && depth < 8 ? strtok_r(inherits, " \t\n,;", &save) : NULL;
            parent && !buffer; parent = strtok_r(NULL, " \t\n,;", &save))
        if (strcmp(parent, theme))
            buffer = xcursor_theme(path, parent, name, size, hotspot_x, hotspot_y, depth + 1);
    free(inherits);
    return buffer;
}

struct buffer *xcursor_load(const char *name, float scale, int *hotspot_x, int *hotspot_y) {
    const char *theme = getenv("XCURSOR_THEME"), *size_env = getenv("XCURSOR_SIZE");
    int size = size_env && atoi(size_env) > 0 ? atoi(size_env) : 24;
    size = (int)lround(size * scale);
    char *path = xcursor_path();
    struct buffer *buffer = NULL;
    const char *themes[] = { theme ? theme : "default", "default" };
    for (int i = 0; path && !buffer && i < 2; i++)
        buffer = xcursor_theme(path, themes[i], name, size, hotspot_x, hotspot_y, 0);
    free(path);
    return buffer;
}
