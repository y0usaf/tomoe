#include "internal.h"
#include "ui.h"
#include "ui-assets.h"

#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#define ASSET_POOL_LIMIT (UINT64_C(128) * 1024 * 1024)
#define ASSET_RASTER_LIMIT (UINT64_C(64) * 1024 * 1024)
#define ASSET_COUNT_LIMIT 256u
#define ASSET_STRING_LIMIT 65536u
#define SHADER_FILE_LIMIT (256u * 1024u)
#define SHADER_POLL_MS 20

enum asset_status { ASSET_OK, ASSET_MISSING, ASSET_HARD, ASSET_PENDING };

struct ui_asset {
    struct ui_asset_pool *pool;
    struct ui_asset *next;
    char *owner, *path, *name;
    uint64_t id, source_id, bytes;
    size_t references;
    int kind;
    bool scratch;
    uint32_t width, height;
    struct program *shader;
    char *shader_source;
    bool shader_waiting;
    double epoch;
};

struct ui_asset_pool {
    struct tomoe *server;
    struct ui_asset *assets;
    uint64_t count, bytes, loads, next_id;
    struct wl_event_source *timer;
    size_t waiting;
};

struct asset_data { unsigned char *bytes; size_t length; };

static void shader_wait_end(struct ui_asset *asset);

static bool bounded_string(const char *text) {
    return text && strnlen(text, ASSET_STRING_LIMIT + 1) <= ASSET_STRING_LIMIT;
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

static void free_content(struct ui_asset *asset) {
    if (asset->shader_waiting) shader_wait_end(asset);
    render_shader_destroy(asset->shader);
    asset->shader = NULL;
    asset->bytes = 0;
}

static void free_asset(struct ui_asset *asset) {
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
    bool glsl = kind == 4 && name && !strcmp(name, "glsl");
    if (!s || !bounded_string(owner) || !*owner ||
            !bounded_string(path) || !bounded_string(name) ||
            kind < 1 || kind > 4 || (*path && *path != '/' && !glsl) ||
            (kind == 1 && (!*path || *name)) || (kind == 4 && (!*path || (*name && !glsl))) ||
            (kind == 2 && !*name) || (kind == 3 && !*path)) return 0;
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
    if (pool->loads != UINT64_MAX) pool->loads++;
    uint64_t available = ASSET_POOL_LIMIT - pool->bytes;
    if (available > ASSET_RASTER_LIMIT) available = ASSET_RASTER_LIMIT;
    enum asset_status status = kind == 4 ? shader_start(s, asset, available) : ASSET_MISSING;
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
    (void)asset; (void)x; (void)y;
    return NULL;
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

bool ui_asset_paint(struct ui_asset *asset, uint32_t *dst, int dst_width, int dst_height,
        int dst_stride, double x, double y, double width, double height,
        bool tint, uint32_t rgba) {
    (void)asset; (void)dst; (void)dst_width; (void)dst_height; (void)dst_stride;
    (void)x; (void)y; (void)width; (void)height; (void)tint; (void)rgba;
    return true;
}

uint64_t ui_assets_count(struct tomoe *s) { return s && s->ui_assets ? s->ui_assets->count : 0; }
uint64_t ui_assets_bytes(struct tomoe *s) { return s && s->ui_assets ? s->ui_assets->bytes : 0; }
uint64_t ui_assets_loads(struct tomoe *s) { return s && s->ui_assets ? s->ui_assets->loads : 0; }

void ui_assets_finish(struct tomoe *s) {
    if (!s || !s->ui_assets) return;
    if (s->ui_assets->timer) wl_event_source_remove(s->ui_assets->timer);
    struct ui_asset *asset = s->ui_assets->assets;
    while (asset) {
        struct ui_asset *next = asset->next;
        free_asset(asset);
        asset = next;
    }
    free(s->ui_assets);
    s->ui_assets = NULL;
}
