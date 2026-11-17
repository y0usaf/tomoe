#include "internal.h"
#include "ui.h"
#include "ui-assets.h"
#include "jpeg.h"
#include "png.h"
#include "draw.h"

#include <drm/drm_fourcc.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <resvg.h>
#include <signal.h>
#include <sys/eventfd.h>
#include <sys/stat.h>
#include <unistd.h>

#define ASSET_FILE_LIMIT (16u * 1024u * 1024u)
#define ASSET_SVG_LIMIT (1024u * 1024u)
#define ASSET_RASTER_LIMIT (UINT64_C(64) * 1024 * 1024)
#define ASSET_POOL_LIMIT (UINT64_C(128) * 1024 * 1024)
#define ASSET_DIMENSION_LIMIT 16384u
#define ASSET_COUNT_LIMIT 256u
#define ASSET_STRING_LIMIT 65536u
#define ASSET_XDG_DIR_LIMIT 64u
#define BACKDROP_FILE_LIMIT (UINT64_C(128) * 1024 * 1024)
#define BACKDROP_SOURCE_LIMIT (UINT64_C(512) * 1024 * 1024)
#define BACKDROP_DIMENSION_LIMIT 32767u
#define SHADER_FILE_LIMIT (256u * 1024u)
#define SHADER_POLL_MS 20

enum asset_status { ASSET_OK, ASSET_MISSING, ASSET_HARD, ASSET_PENDING };
enum { FIT_COVER, FIT_CONTAIN, FIT_FILL };

struct backdrop_job {
    struct backdrop_job *next;
    struct ui_asset *asset;
    char *path;
    uint32_t width, height;
    int fit;
    unsigned char *result;
    int result_width, result_height, result_stride;
    const char *reason;
};

struct ui_asset {
    struct ui_asset_pool *pool;
    struct ui_asset *next;
    char *owner, *path, *name;
    uint64_t id, source_id, bytes;
    size_t references;
    int kind;
    bool scratch;
    uint32_t width, height;
    unsigned char *pixels;
    int raster_format;
    resvg_render_tree *svg;
    double svg_width, svg_height;
    struct backdrop_job *job;
    struct texture *texture;
    int x, y;
    struct program *shader;
    char *shader_source;
    bool shader_waiting;
    double epoch;
};

struct ui_asset_pool {
    struct tomoe *server;
    struct ui_asset *assets;
    uint64_t count, bytes, loads, next_id;
    pthread_t worker;
    pthread_mutex_t lock;
    pthread_cond_t wake;
    struct backdrop_job *queue, *done;
    struct wl_event_source *source;
    struct wl_event_source *timer;
    size_t waiting;
    int wakeup;
    bool working, stopping;
};

static void shader_wait_end(struct ui_asset *asset);

struct asset_data { unsigned char *bytes; size_t length; };

static bool bounded_string(const char *text) {
    return text && strnlen(text, ASSET_STRING_LIMIT + 1) <= ASSET_STRING_LIMIT;
}

static bool raster_bytes(uint32_t width, uint32_t height, uint32_t dimension,
        uint64_t available, uint64_t *bytes) {
    if (!width || !height || width > dimension || height > dimension) return false;
    *bytes = (uint64_t)width * height * 4;
    return *bytes <= available;
}

static enum asset_status read_asset(const char *path, size_t limit,
        struct asset_data *data) {
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0) return errno == ENOMEM ? ASSET_HARD : ASSET_MISSING;
    struct stat info;
    enum asset_status result = ASSET_MISSING;
    if (fstat(fd, &info) != 0 || !S_ISREG(info.st_mode)) goto done;
    if (info.st_size < 0 || (uint64_t)info.st_size > limit) {
        result = ASSET_HARD;
        goto done;
    }
    size_t capacity = (size_t)info.st_size;
    unsigned char *bytes = malloc(capacity + 1);
    if (!bytes) { result = ASSET_HARD; goto done; }
    size_t length = 0;
    while (length <= capacity) {
        ssize_t count = read(fd, bytes + length, capacity + 1 - length);
        if (count < 0 && errno == EINTR) continue;
        if (count < 0) {
            result = errno == ENOMEM ? ASSET_HARD : ASSET_MISSING;
            free(bytes);
            goto done;
        }
        if (!count) break;
        length += (size_t)count;
    }
    if (length > capacity) {
        free(bytes);
        result = ASSET_HARD;
        goto done;
    }
    *data = (struct asset_data){ .bytes = bytes, .length = length };
    result = ASSET_OK;
done:
    close(fd);
    return result;
}

static enum asset_status read_icon(const char *path, const char *name,
        struct asset_data *data) {
    if (*path) return read_asset(path, ASSET_SVG_LIMIT, data);
    const char *dirs = tomoe_data_dirs();
    if (!bounded_string(dirs)) return ASSET_HARD;
    size_t directory_count = 1;
    for (const char *cursor = dirs; *cursor; cursor++)
        if (*cursor == ':' && ++directory_count > ASSET_XDG_DIR_LIMIT) return ASSET_HARD;
    static const char *themes[] = {"hicolor", "Adwaita", "breeze"};
    static const char *categories[] = {"actions", "apps", "categories",
        "devices", "emblems", "mimetypes", "places", "status", "panel", ""};
    size_t capacity = strlen(dirs) + strlen(name) + 80;
    char *candidate = malloc(capacity);
    if (!candidate) return ASSET_HARD;
    enum asset_status result = ASSET_MISSING;
    const char *base = dirs;
    do {
        const char *end = strchr(base, ':');
        size_t length = end ? (size_t)(end - base) : strlen(base);
        for (size_t i = 0; i < sizeof(themes) / sizeof(*themes); i++) {
            for (size_t j = 0; j < sizeof(categories) / sizeof(*categories); j++) {
                int count = snprintf(candidate, capacity, "%.*s/icons/%s/scalable/%s%s%s.svg",
                    (int)length, base, themes[i], categories[j],
                    *categories[j] ? "/" : "", name);
                if (count < 0 || (size_t)count >= capacity) { result = ASSET_HARD; goto done; }
                result = read_asset(candidate, ASSET_SVG_LIMIT, data);
                if (result != ASSET_MISSING) goto done;
            }
        }
        if (!end) break;
        base = end + 1;
    } while (true);
done:
    free(candidate);
    return result;
}

static bool png_image_dp(const struct png_image *image, struct dp_image *out) {
    switch (image->format) {
    case PNG_FORMAT_ARGB32: out->format = DP_IMAGE_ARGB32; break;
    case PNG_FORMAT_RGB24: out->format = DP_IMAGE_RGB24; break;
    case PNG_FORMAT_RGBA128F: out->format = DP_IMAGE_RGBA_FLOAT; break;
    case PNG_FORMAT_RGB96F: out->format = DP_IMAGE_RGB_FLOAT; break;
    default: return false;
    }
    out->pixels = image->pixels;
    out->width = (int)image->width;
    out->height = (int)image->height;
    out->stride = (int)image->stride;
    return out->pixels != NULL && out->width > 0 && out->height > 0;
}

static uint32_t big_endian(const unsigned char *bytes) {
    return (uint32_t)bytes[0] << 24 | (uint32_t)bytes[1] << 16 | (uint32_t)bytes[2] << 8 | bytes[3];
}

static enum asset_status decode_png(struct ui_asset *asset,
        const struct asset_data *data, uint64_t available) {
    if (data->length < 24 || memcmp(data->bytes + 12, "IHDR", 4)) return ASSET_MISSING;
    asset->width = big_endian(data->bytes + 16);
    asset->height = big_endian(data->bytes + 20);
    if (!asset->width || !asset->height) return ASSET_MISSING;
    if (!raster_bytes(asset->width, asset->height, ASSET_DIMENSION_LIMIT, available, &asset->bytes))
        return ASSET_HARD;
    struct png_image image;
    enum png_status decoded = tomoe_png_decode(data->bytes, data->length, &image);
    if (decoded == PNG_HARD) return ASSET_HARD;
    enum asset_status result = ASSET_MISSING;
    if (decoded == PNG_OK && image.width == asset->width && image.height == asset->height)
        result = (asset->pixels = malloc((size_t)asset->bytes)) ? ASSET_OK : ASSET_HARD;
    if (result == ASSET_OK) {
        struct dp_image decoded;
        if (!png_image_dp(&image, &decoded)) result = ASSET_HARD;
        else {
            struct dp_pattern pattern = dp_surface_pattern(0, 0, 1, 1, 0, 0);
            dp_paint_surface((uint32_t *)asset->pixels, (int)asset->width,
                (struct drect){ 0, 0, (int)asset->width, (int)asset->height }, &decoded,
                &pattern, DP_FILTER_NEAREST, DP_EXTEND_PAD, DP_OP_SOURCE, NULL,
                (int)asset->width, (int)asset->height, 0, 0, (int)asset->width,
                (int)asset->height);
        }
    }
    free(image.pixels);
    return result;
}

static enum asset_status decode_jpeg(struct ui_asset *asset,
        const struct asset_data *data, uint32_t dimension, uint64_t available) {
    struct jpeg_decoded decoded;
    enum jpeg_decode_status status = tomoe_jpeg_decode(data->bytes, data->length,
        dimension, available, &decoded);
    if (status != JPEG_DECODE_OK) return status == JPEG_DECODE_HARD ? ASSET_HARD : ASSET_MISSING;
    asset->width = decoded.width;
    asset->height = decoded.height;
    asset->bytes = decoded.bytes;
    asset->pixels = decoded.pixels;
    return ASSET_OK;
}

static enum asset_status decode_svg(struct ui_asset *asset,
        const struct asset_data *data, uint64_t available) {
    if (data->length > available) return ASSET_HARD;
    resvg_options *options = resvg_options_create();
    int32_t error = resvg_parse_tree_from_data((const char *)data->bytes, data->length, options, &asset->svg);
    resvg_options_destroy(options);
    if (error != RESVG_OK) return ASSET_MISSING;
    resvg_size size = resvg_get_image_size(asset->svg);
    asset->svg_width = size.width;
    asset->svg_height = size.height;
    if (!isfinite(asset->svg_width) || !isfinite(asset->svg_height) ||
            asset->svg_width <= 0 || asset->svg_height <= 0) return ASSET_MISSING;
    if (asset->svg_width > UINT32_MAX || asset->svg_height > UINT32_MAX) return ASSET_HARD;
    asset->width = (uint32_t)ceil(asset->svg_width);
    asset->height = (uint32_t)ceil(asset->svg_height);
    if (asset->width == UINT32_MAX && asset->height == UINT32_MAX) return ASSET_HARD;
    asset->bytes = data->length;
    return ASSET_OK;
}

static void job_free(struct backdrop_job *job) {
    free(job->result);
    free(job->path);
    free(job);
}

static void backdrop_cancel(struct ui_asset *asset) {
    struct ui_asset_pool *pool = asset->pool;
    struct backdrop_job *job = asset->job, **link = &pool->queue;
    asset->job = NULL;
    job->asset = NULL;
    pthread_mutex_lock(&pool->lock);
    while (*link && *link != job) link = &(*link)->next;
    bool queued = *link != NULL;
    if (queued) *link = job->next;
    pthread_mutex_unlock(&pool->lock);
    if (queued) job_free(job);
}

static void free_content(struct ui_asset *asset) {
    if (asset->job) backdrop_cancel(asset);
    if (asset->texture) texture_destroy(asset->texture);
    render_shader_destroy(asset->shader);
    if (asset->svg) resvg_tree_destroy(asset->svg);
    free(asset->pixels);
    asset->texture = NULL; asset->shader = NULL;
    asset->raster_format = 0; asset->svg = NULL; asset->pixels = NULL;
    asset->width = asset->height = 0; asset->bytes = 0;
}

static void free_asset(struct ui_asset *asset) {
    if (asset->shader_waiting) shader_wait_end(asset);
    free_content(asset);
    free(asset->owner); free(asset->path); free(asset->name);
    free(asset);
}

static struct ui_asset *find_asset(struct tomoe *s, uint64_t id) {
    if (!s || !s->ui_assets || !id) return NULL;
    for (struct ui_asset *asset = s->ui_assets->assets; asset; asset = asset->next)
        if (asset->id == id) return asset;
    return NULL;
}

static unsigned char *backdrop_scale(const struct dp_image *source, uint32_t width,
        uint32_t height, int fit, int *out_width, int *out_height) {
    double iw = source->width, ih = source->height;
    double sx = width / iw, sy = height / ih;
    if (fit == FIT_COVER) sx = sy = fmax(sx, sy);
    else if (fit == FIT_CONTAIN) sx = sy = fmin(sx, sy);
    int w = fit == FIT_CONTAIN ? (int)fmax(1, round(iw * sx)) : (int)width;
    int h = fit == FIT_CONTAIN ? (int)fmax(1, round(ih * sy)) : (int)height;
    unsigned char *target = calloc((size_t)w * (size_t)h, 4);
    if (!target) return NULL;
    struct dp_pattern pattern = dp_surface_pattern((w - iw * sx) / 2, (h - ih * sy) / 2,
        sx, sy, 0, 0);
    dp_paint_surface((uint32_t *)target, w, (struct drect){ 0, 0, w, h }, source, &pattern,
        DP_FILTER_GOOD, DP_EXTEND_PAD, DP_OP_SOURCE, NULL, w, h, 0, 0, w, h);
    *out_width = w;
    *out_height = h;
    return target;
}

static void backdrop_decode(struct backdrop_job *job) {
    struct asset_data data = {0};
    struct ui_asset decoded = {0};
    struct png_image image = {0};
    struct dp_image source = {0};
    uint64_t bytes;
    bool have = false;
    if (read_asset(job->path, BACKDROP_FILE_LIMIT, &data) == ASSET_OK) {
        if (data.length >= 24 && !memcmp(data.bytes, "\x89PNG\r\n\x1a\n", 8) &&
                !memcmp(data.bytes + 12, "IHDR", 4) &&
                raster_bytes(big_endian(data.bytes + 16), big_endian(data.bytes + 20),
                    BACKDROP_DIMENSION_LIMIT, BACKDROP_SOURCE_LIMIT, &bytes) &&
                tomoe_png_decode(data.bytes, data.length, &image) == PNG_OK)
            have = png_image_dp(&image, &source);
        else if (data.length >= 2 && data.bytes[0] == 0xff && data.bytes[1] == 0xd8 &&
                decode_jpeg(&decoded, &data, BACKDROP_DIMENSION_LIMIT, BACKDROP_SOURCE_LIMIT) == ASSET_OK) {
            source = (struct dp_image){ decoded.pixels, (int)decoded.width,
                (int)decoded.height, (int)decoded.width * 4, DP_IMAGE_ARGB32 };
            have = true;
        }
    }
    free(data.bytes);
    if (have) {
        job->result = backdrop_scale(&source, job->width, job->height, job->fit,
            &job->result_width, &job->result_height);
        job->result_stride = job->result_width * 4;
    }
    free(decoded.pixels);
    free(image.pixels);
}

static void *backdrop_worker(void *data) {
    struct ui_asset_pool *pool = data;
    pthread_mutex_lock(&pool->lock);
    while (!pool->stopping) {
        struct backdrop_job *job = pool->queue;
        if (!job) {
            pthread_cond_wait(&pool->wake, &pool->lock);
            continue;
        }
        pool->queue = job->next;
        pthread_mutex_unlock(&pool->lock);
        backdrop_decode(job);
        pthread_mutex_lock(&pool->lock);
        job->next = pool->done;
        pool->done = job;
        ssize_t written = write(pool->wakeup, &(uint64_t){ 1 }, sizeof(uint64_t));
        (void)written;
    }
    pthread_mutex_unlock(&pool->lock);
    return NULL;
}

static int backdrops_done(int fd, uint32_t mask, void *data) {
    struct tomoe *s = data;
    struct ui_asset_pool *pool = s->ui_assets;
    uint64_t count;
    if (read(fd, &count, sizeof(count)) < 0 && errno != EAGAIN)
        fprintf(stderr, "tomoe: shell background wakeup failed: %s\n", strerror(errno));
    pthread_mutex_lock(&pool->lock);
    struct backdrop_job *job = pool->done;
    pool->done = NULL;
    pthread_mutex_unlock(&pool->lock);
    while (job) {
        struct backdrop_job *next = job->next;
        struct ui_asset *asset = job->asset;
        if (asset) {
            asset->job = NULL;
            if (job->result && s->renderer)
                asset->texture = texture_from_pixels(s->renderer, DRM_FORMAT_ARGB8888,
                    (uint32_t)job->result_stride, (uint32_t)job->result_width,
                    (uint32_t)job->result_height, job->result);
            if (asset->texture) {
                asset->x = ((int)asset->width - (int)asset->texture->width) / 2;
                asset->y = ((int)asset->height - (int)asset->texture->height) / 2;
            } else {
                fprintf(stderr, "tomoe: shell background asset unavailable: %s\n", asset->path);
            }
        }
        job_free(job);
        job = next;
    }
    ui_backdrops_ready(s);
    return 0;
}

static bool backdrop_worker_start(struct tomoe *s, struct ui_asset_pool *pool) {
    if (pool->working) return true;
    int wakeup = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    struct wl_event_source *source = wakeup < 0 ? NULL : wl_event_loop_add_fd(
        wl_display_get_event_loop(s->display), wakeup, WL_EVENT_READABLE, backdrops_done, s);
    if (!source) {
        if (wakeup >= 0) close(wakeup);
        return false;
    }
    pool->wakeup = wakeup;
    pool->source = source;
    pthread_mutex_init(&pool->lock, NULL);
    pthread_cond_init(&pool->wake, NULL);
    sigset_t all, previous;
    sigfillset(&all);
    pthread_sigmask(SIG_SETMASK, &all, &previous);
    pool->working = pthread_create(&pool->worker, NULL, backdrop_worker, pool) == 0;
    pthread_sigmask(SIG_SETMASK, &previous, NULL);
    if (pool->working) return true;
    pthread_cond_destroy(&pool->wake);
    pthread_mutex_destroy(&pool->lock);
    wl_event_source_remove(source);
    close(wakeup);
    return false;
}

static void backdrop_worker_stop(struct ui_asset_pool *pool) {
    if (!pool->working) return;
    pthread_mutex_lock(&pool->lock);
    pool->stopping = true;
    pthread_cond_signal(&pool->wake);
    pthread_mutex_unlock(&pool->lock);
    pthread_join(pool->worker, NULL);
    struct backdrop_job *lists[] = { pool->queue, pool->done };
    for (size_t i = 0; i < 2; i++) {
        for (struct backdrop_job *job = lists[i], *next; job; job = next) {
            next = job->next;
            if (job->asset) job->asset->job = NULL;
            job_free(job);
        }
    }
    pool->queue = pool->done = NULL;
    wl_event_source_remove(pool->source);
    close(pool->wakeup);
    pthread_cond_destroy(&pool->wake);
    pthread_mutex_destroy(&pool->lock);
    pool->working = false;
}

static bool backdrop_name(const char *name, uint32_t *width, uint32_t *height, int *fit) {
    static const char *const fits[] = { "cover", "contain", "fill" };
    char mode[8];
    int end = 0;
    if (sscanf(name, "%ux%u:%7[a-z]%n", width, height, mode, &end) != 3 || name[end] ||
            !*width || !*height || *width > ASSET_DIMENSION_LIMIT ||
            *height > ASSET_DIMENSION_LIMIT) return false;
    for (*fit = FIT_COVER; *fit <= FIT_FILL; ++*fit)
        if (!strcmp(mode, fits[*fit])) return true;
    return false;
}

static enum asset_status backdrop_start(struct tomoe *s, struct ui_asset *asset,
        uint32_t width, uint32_t height, int fit, uint64_t available) {
    struct ui_asset_pool *pool = asset->pool;
    uint64_t bytes;
    struct stat info;
    if (!raster_bytes(width, height, ASSET_DIMENSION_LIMIT, available, &bytes)) return ASSET_HARD;
    if (stat(asset->path, &info) != 0 || !S_ISREG(info.st_mode)) return ASSET_MISSING;
    if ((uint64_t)info.st_size > BACKDROP_FILE_LIMIT || !backdrop_worker_start(s, pool))
        return ASSET_HARD;
    struct backdrop_job *job = calloc(1, sizeof(*job));
    char *path = strdup(asset->path);
    if (!job || !path) {
        free(job);
        free(path);
        return ASSET_HARD;
    }
    *job = (struct backdrop_job){ .asset = asset, .path = path, .width = width,
        .height = height, .fit = fit };
    asset->width = width; asset->height = height; asset->bytes = bytes; asset->job = job;
    pthread_mutex_lock(&pool->lock);
    job->next = pool->queue;
    pool->queue = job;
    pthread_cond_signal(&pool->wake);
    pthread_mutex_unlock(&pool->lock);
    return ASSET_OK;
}

static void shader_repaint(struct ui_asset_pool *pool) {
    struct tomoe *s = pool->server;
    struct output *o;
    if (!s) return;
    wl_list_for_each(o, &s->outputs, link)
        if (o->screen && output_is_active(o)) screen_schedule_frame(o->screen);
}

static int shader_timer(void *data);

static void shader_watch(struct ui_asset_pool *pool) {
    struct tomoe *s = pool->server;
    if (!s || !s->display) return;
    if (!pool->timer)
        pool->timer = wl_event_loop_add_timer(wl_display_get_event_loop(s->display),
            shader_timer, pool);
    if (pool->timer) wl_event_source_timer_update(pool->timer, SHADER_POLL_MS);
}

static void shader_wait_end(struct ui_asset *asset) {
    free(asset->shader_source);
    asset->shader_source = NULL;
    asset->shader_waiting = false;
    if (asset->pool->waiting) asset->pool->waiting--;
}

static bool shader_retry(struct ui_asset *asset) {
    struct ui_asset_pool *pool = asset->pool;
    if (!asset->shader_source || !pool->server || !pool->server->renderer) {
        shader_wait_end(asset);
        return false;
    }
    bool waiting = false;
    asset->shader = render_shader_create(pool->server->renderer, asset->shader_source,
        &waiting);
    if (asset->shader) {
        uint64_t bytes = strlen(asset->shader_source);
        asset->epoch = shader_clock_now(pool->server, animation_now());
        shader_wait_end(asset);
        asset->bytes = bytes;
        pool->bytes += bytes;
        return true;
    }
    if (!waiting) shader_wait_end(asset);
    return false;
}

static int shader_timer(void *data) {
    struct ui_asset_pool *pool = data;
    struct render *renderer = pool->server ? pool->server->renderer : NULL;
    bool ready = false;
    for (struct ui_asset *asset = pool->assets; asset; asset = asset->next)
        if (asset->shader_waiting && shader_retry(asset)) ready = true;
    if (ready) shader_repaint(pool);
    bool busy = pool->waiting > 0 || (renderer && render_shader_reap(renderer));
    if (busy && pool->timer) wl_event_source_timer_update(pool->timer, SHADER_POLL_MS);
    return 0;
}

static enum asset_status shader_start(struct tomoe *s, struct ui_asset *asset,
        uint64_t available) {
    struct asset_data data = {0};
    enum asset_status status = ASSET_OK;
    bool waiting = false;
    if (!strcmp(asset->name, "glsl")) {
        data.length = strlen(asset->path);
        data.bytes = (unsigned char *)strdup(asset->path);
        if (!data.bytes) return ASSET_HARD;
    } else {
        status = read_asset(asset->path, SHADER_FILE_LIMIT, &data);
    }
    if (status != ASSET_OK) return status;
    data.bytes[data.length] = '\0';
    if (data.length > available) status = ASSET_HARD;
    else if (s->renderer && strlen((char *)data.bytes) == data.length)
        asset->shader = render_shader_create(s->renderer, (char *)data.bytes, &waiting);
    if (status == ASSET_OK && !asset->shader && waiting) {
        asset->shader_source = (char *)data.bytes;
        data.bytes = NULL;
        asset->shader_waiting = true;
        if (asset->pool->waiting != SIZE_MAX) asset->pool->waiting++;
        shader_watch(asset->pool);
        status = ASSET_PENDING;
    }
    free(data.bytes);
    if (status != ASSET_OK) return status;
    if (!asset->shader) return ASSET_MISSING;
    asset->epoch = shader_clock_now(s, animation_now());
    asset->bytes = data.length;
    return ASSET_OK;
}

uint64_t tomoe_ui_asset_load(struct tomoe *s, const char *owner,
        uint64_t source_id, int kind, const char *path, const char *name) {
    static const char *const labels[] = { "", "image", "icon", "background", "shader" };
    uint32_t width = 0, height = 0;
    int fit = FIT_COVER;
    bool glsl = kind == 4 && name && !strcmp(name, "glsl");
    if (!s || !source_id || !bounded_string(owner) || !*owner ||
            !bounded_string(path) || !bounded_string(name) ||
            kind < 1 || kind > 4 || (*path && *path != '/' && !glsl) ||
            (kind == 1 && (!*path || *name)) || (kind == 4 && (!*path || (*name && !glsl))) ||
            (kind == 2 && !*name) ||
            (kind == 3 && (!*path || !backdrop_name(name, &width, &height, &fit)))) return 0;
    if (!s->ui_assets) {
        s->ui_assets = calloc(1, sizeof(*s->ui_assets));
        if (!s->ui_assets) return 0;
        s->ui_assets->next_id = 1;
        s->ui_assets->server = s;
    }
    struct ui_asset_pool *pool = s->ui_assets;
    for (struct ui_asset *asset = pool->assets; asset; asset = asset->next) {
        if (asset->source_id == source_id && asset->kind == kind &&
                !strcmp(asset->owner, owner) && !strcmp(asset->path, path) && !strcmp(asset->name, name)) {
            if (!asset->scratch) {
                if (asset->references == SIZE_MAX) return 0;
                asset->references++; asset->scratch = true;
            }
            return asset->id;
        }
    }
    if (pool->count == ASSET_COUNT_LIMIT || !pool->next_id) return 0;
    struct ui_asset *asset = calloc(1, sizeof(*asset));
    if (!asset) return 0;
    asset->owner = strdup(owner); asset->path = strdup(path); asset->name = strdup(name);
    if (!asset->owner || !asset->path || !asset->name) { free_asset(asset); return 0; }
    asset->kind = kind; asset->source_id = source_id; asset->pool = pool;
    struct asset_data data = {0};
    if (pool->loads != UINT64_MAX) pool->loads++;
    uint64_t available = ASSET_POOL_LIMIT - pool->bytes;
    if (available > ASSET_RASTER_LIMIT) available = ASSET_RASTER_LIMIT;
    enum asset_status status = kind == 4 ? shader_start(s, asset, available) :
        kind == 3 ? backdrop_start(s, asset, width, height, fit, available) :
        kind == 1 ? read_asset(path, ASSET_FILE_LIMIT, &data) : read_icon(path, name, &data);
    if (status == ASSET_OK && kind < 3) {
        if (kind == 2) status = decode_svg(asset, &data, available);
        else if (data.length >= 8 && !memcmp(data.bytes, "\x89PNG\r\n\x1a\n", 8)) status = decode_png(asset, &data, available);
        else if (data.length >= 2 && data.bytes[0] == 0xff && data.bytes[1] == 0xd8)
            status = decode_jpeg(asset, &data, ASSET_DIMENSION_LIMIT, available);
        else status = ASSET_MISSING;
    }
    free(data.bytes);
    if (status == ASSET_OK && kind == 1) {
        if (asset->pixels) asset->raster_format = DP_IMAGE_ARGB32;
    }
    if (status == ASSET_HARD) { free_asset(asset); return 0; }
    if (status == ASSET_MISSING) {
        free_content(asset);
        fprintf(stderr, "tomoe: shell %s asset unavailable: %s\n", labels[kind],
            glsl ? "inline GLSL" : *path ? path : name);
    }
    asset->references = 1; asset->scratch = true;
    asset->id = pool->next_id;
    pool->next_id = pool->next_id == UINT64_MAX ? 0 : pool->next_id + 1;
    asset->next = pool->assets; pool->assets = asset;
    pool->count++; pool->bytes += asset->bytes;
    return asset->id;
}

uint64_t tomoe_ui_asset_size(struct tomoe *s, uint64_t id) {
    struct ui_asset *asset = find_asset(s, id);
    return asset ? ((uint64_t)asset->width << 32) | asset->height : UINT64_MAX;
}

struct ui_asset *ui_asset_acquire(struct tomoe *s, uint64_t id) {
    struct ui_asset *asset = find_asset(s, id);
    if (!asset || asset->references == SIZE_MAX) return NULL;
    asset->references++;
    return asset;
}

struct ui_asset *ui_asset_retain(struct ui_asset *asset) {
    if (!asset || asset->references == SIZE_MAX) return NULL;
    asset->references++;
    return asset;
}

struct texture *ui_asset_backdrop(const struct ui_asset *asset, int *x, int *y) {
    if (!asset || !asset->texture) return NULL;
    if (x) *x = asset->x;
    if (y) *y = asset->y;
    return asset->texture;
}

struct program *ui_asset_shader(const struct ui_asset *asset, double *epoch) {
    if (!asset || !asset->shader) return NULL;
    if (epoch) *epoch = asset->epoch;
    return asset->shader;
}

bool ui_asset_owned_by(const struct ui_asset *asset, const char *owner, uint64_t source_id) {
    return asset && owner && asset->source_id == source_id && !strcmp(asset->owner, owner);
}

void ui_asset_release(struct ui_asset *asset) {
    if (!asset || --asset->references) return;
    struct ui_asset_pool *pool = asset->pool;
    struct ui_asset **link = &pool->assets;
    while (*link != asset) link = &(*link)->next;
    *link = asset->next;
    pool->count--; pool->bytes -= asset->bytes;
    free_asset(asset);
}

void tomoe_ui_assets_discard(struct tomoe *s) {
    if (!s || !s->ui_assets) return;
    struct ui_asset *asset = s->ui_assets->assets;
    while (asset) {
        struct ui_asset *next = asset->next;
        if (asset->scratch) { asset->scratch = false; ui_asset_release(asset); }
        asset = next;
    }
}

static unsigned char *render_svg(const struct ui_asset *asset, int width, int height) {
    unsigned char *pixels = calloc((size_t)width * (size_t)height, 4);
    if (!pixels) return NULL;
    resvg_render(asset->svg, (resvg_transform){ width / asset->svg_width, 0, 0,
        height / asset->svg_height, 0, 0 }, width, height, (char *)pixels);
    for (int i = 0; i < width * height * 4; i += 4) {
        uint32_t pixel = (uint32_t)pixels[i + 3] << 24 | (uint32_t)pixels[i] << 16 |
            (uint32_t)pixels[i + 1] << 8 | pixels[i + 2];
        memcpy(pixels + i, &pixel, sizeof(pixel));
    }
    return pixels;
}

bool ui_asset_paint(struct ui_asset *asset, uint32_t *dst, int dst_width, int dst_height,
        int dst_stride, double x, double y, double width, double height, bool tint,
        uint32_t rgba) {
    if (!asset || !dst || !isfinite(x) || !isfinite(y) || !isfinite(width) ||
            !isfinite(height) || width < 0 || height < 0 ||
            width > ASSET_DIMENSION_LIMIT || height > ASSET_DIMENSION_LIMIT) return false;
    if (!width || !height || (!asset->raster_format && !asset->svg)) return true;
    uint64_t bytes;
    if (!raster_bytes((uint32_t)ceil(width), (uint32_t)ceil(height), ASSET_DIMENSION_LIMIT,
            ASSET_RASTER_LIMIT, &bytes)) return false;
    int w = (int)ceil(width), h = (int)ceil(height);
    unsigned char *temporary = asset->svg ? render_svg(asset, w, h) : NULL;
    if (asset->svg && !temporary) return false;
    if (tint) {
        if (!temporary) {
            temporary = calloc((size_t)w * (size_t)h, 4);
            if (!temporary) return false;
            struct dp_image raster = { asset->pixels, (int)asset->width,
                (int)asset->height, (int)asset->width * 4, asset->raster_format };
            struct dp_pattern pattern = dp_surface_pattern(0, 0,
                (double)w / raster.width, (double)h / raster.height, 0, 0);
            dp_paint_surface((uint32_t *)temporary, w, (struct drect){ 0, 0, w, h },
                &raster, &pattern, DP_FILTER_BILINEAR, DP_EXTEND_PAD, DP_OP_SOURCE, NULL,
                w, h, 0, 0, w, h);
        }
        int stride = w * 4;
        unsigned r = rgba >> 24, g = (rgba >> 16) & 255, b = (rgba >> 8) & 255, alpha = rgba & 255;
        for (int row = 0; row < h; row++) for (int column = 0; column < w; column++) {
            uint32_t pixel;
            unsigned char *position = temporary + (size_t)row * stride + (size_t)column * 4;
            memcpy(&pixel, position, sizeof(pixel));
            unsigned a = (pixel >> 24) * alpha / 255;
            pixel = (a << 24) | (((r * a + 127) / 255) << 16) |
                (((g * a + 127) / 255) << 8) | ((b * a + 127) / 255);
            memcpy(position, &pixel, sizeof(pixel));
        }
    }
    struct dp_image image = temporary
        ? (struct dp_image){ temporary, w, h, w * 4, DP_IMAGE_ARGB32 }
        : (struct dp_image){ asset->pixels, (int)asset->width, (int)asset->height,
            (int)asset->width * 4, asset->raster_format };
    struct dpath path;
    dpath_init(&path);
    dp_rectangle(&path, x, y, width, height);
    struct drect clip = dp_path_extents(&path);
    if (clip.x0 < 0) clip.x0 = 0;
    if (clip.y0 < 0) clip.y0 = 0;
    if (clip.x1 > dst_width) clip.x1 = dst_width;
    if (clip.y1 > dst_height) clip.y1 = dst_height;
    bool ok = true;
    if (clip.x0 < clip.x1 && clip.y0 < clip.y1) {
        int span = clip.x1 - clip.x0;
        unsigned char *coverage = calloc((size_t)span * (size_t)(clip.y1 - clip.y0), 1);
        if (!coverage) ok = false;
        else {
            ok = dp_fill_mask(&path, clip, coverage, span, clip.x0, clip.y0);
            if (ok) {
                struct dp_pattern pattern = dp_surface_pattern(x, y,
                    width / image.width, height / image.height, 0, 0);
                struct dp_coverage mask = { coverage, span, clip.x0, clip.y0, NULL, 0 };
                dp_paint_surface(dst, dst_stride, clip, &image, &pattern, DP_FILTER_BILINEAR,
                    DP_EXTEND_PAD, DP_OP_OVER, &mask, dst_width, dst_height,
                    clip.x0, clip.y0, clip.x1 - clip.x0, clip.y1 - clip.y0);
            }
            free(coverage);
        }
    }
    dpath_fini(&path);
    free(temporary);
    return ok;
}

uint64_t ui_assets_count(struct tomoe *s) { return s && s->ui_assets ? s->ui_assets->count : 0; }
uint64_t ui_assets_bytes(struct tomoe *s) { return s && s->ui_assets ? s->ui_assets->bytes : 0; }
uint64_t ui_assets_loads(struct tomoe *s) { return s && s->ui_assets ? s->ui_assets->loads : 0; }

void ui_assets_finish(struct tomoe *s) {
    if (!s || !s->ui_assets) return;
    tomoe_ui_assets_discard(s);
    if (s->ui_assets->timer) {
        wl_event_source_remove(s->ui_assets->timer);
        s->ui_assets->timer = NULL;
    }
    backdrop_worker_stop(s->ui_assets);
    while (s->ui_assets->assets) {
        struct ui_asset *asset = s->ui_assets->assets;
        s->ui_assets->assets = asset->next;
        free_asset(asset);
    }
    free(s->ui_assets); s->ui_assets = NULL;
}
