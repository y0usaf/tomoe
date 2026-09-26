#include "internal.h"
#include "ui-assets.h"

#include <errno.h>
#include <fcntl.h>
#include <jpeglib.h>
#include <jerror.h>
#include <librsvg/rsvg.h>
#include <setjmp.h>
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

enum asset_status { ASSET_OK, ASSET_MISSING, ASSET_HARD };

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
    cairo_surface_t *raster;
    RsvgHandle *svg;
    double svg_width, svg_height;
};

struct ui_asset_pool {
    struct ui_asset *assets;
    uint64_t count, bytes, loads, next_id;
};

struct asset_data { unsigned char *bytes; size_t length; };

static bool bounded_string(const char *text) {
    return text && strnlen(text, ASSET_STRING_LIMIT + 1) <= ASSET_STRING_LIMIT;
}

static bool raster_bytes(uint32_t width, uint32_t height,
        uint64_t available, uint64_t *bytes) {
    if (!width || !height || width > ASSET_DIMENSION_LIMIT ||
            height > ASSET_DIMENSION_LIMIT) return false;
    *bytes = (uint64_t)width * height * 4;
    return *bytes <= ASSET_RASTER_LIMIT && *bytes <= available;
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
    const char *dirs = getenv("XDG_DATA_DIRS");
    if (!dirs) dirs = "/usr/share:/usr/local/share";
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

struct png_read { const struct asset_data *data; size_t offset; };

static cairo_status_t png_read_bytes(void *closure, unsigned char *output, unsigned int length) {
    struct png_read *read = closure;
    if (length > read->data->length - read->offset) return CAIRO_STATUS_READ_ERROR;
    memcpy(output, read->data->bytes + read->offset, length);
    read->offset += length;
    return CAIRO_STATUS_SUCCESS;
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
    if (!raster_bytes(asset->width, asset->height, available, &asset->bytes)) return ASSET_HARD;
    cairo_surface_t *image = cairo_image_surface_create_from_png_stream(png_read_bytes,
        &(struct png_read){ .data = data });
    cairo_status_t status = cairo_surface_status(image);
    enum asset_status result = status == CAIRO_STATUS_NO_MEMORY ? ASSET_HARD : ASSET_MISSING;
    if (status == CAIRO_STATUS_SUCCESS &&
            (uint32_t)cairo_image_surface_get_width(image) == asset->width &&
            (uint32_t)cairo_image_surface_get_height(image) == asset->height)
        result = (asset->pixels = malloc((size_t)asset->bytes)) ? ASSET_OK : ASSET_HARD;
    if (result == ASSET_OK) {
        cairo_surface_t *target = cairo_image_surface_create_for_data(asset->pixels, CAIRO_FORMAT_ARGB32,
            (int)asset->width, (int)asset->height, (int)asset->width * 4);
        cairo_t *cairo = cairo_create(target);
        cairo_set_source_surface(cairo, image, 0, 0);
        cairo_set_operator(cairo, CAIRO_OPERATOR_SOURCE);
        cairo_paint(cairo);
        if (cairo_status(cairo) != CAIRO_STATUS_SUCCESS) result = ASSET_HARD;
        cairo_destroy(cairo);
        cairo_surface_destroy(target);
    }
    cairo_surface_destroy(image);
    return result;
}

struct jpeg_decode {
    struct jpeg_error_mgr error;
    struct jpeg_decompress_struct jpeg;
    jmp_buf jump;
    unsigned char *pixels, *row;
    bool hard, created;
};

static void jpeg_failure(j_common_ptr common) {
    struct jpeg_decode *decode = (struct jpeg_decode *)common->err;
    if (common->err->msg_code == JERR_OUT_OF_MEMORY ||
            common->err->msg_code == JERR_IMAGE_TOO_BIG ||
            common->err->msg_code == JERR_WIDTH_OVERFLOW) decode->hard = true;
    longjmp(decode->jump, 1);
}

static void jpeg_message_ignore(j_common_ptr common) { (void)common; }

static enum asset_status decode_jpeg(struct ui_asset *asset,
        const struct asset_data *data, uint64_t available) {
    struct jpeg_decode *decode = calloc(1, sizeof(*decode));
    if (!decode) return ASSET_HARD;
    decode->jpeg.err = jpeg_std_error(&decode->error);
    decode->error.error_exit = jpeg_failure;
    decode->error.output_message = jpeg_message_ignore;
    volatile enum asset_status result = ASSET_MISSING;
    if (setjmp(decode->jump)) { result = decode->hard ? ASSET_HARD : ASSET_MISSING; goto done; }
    decode->created = true;
    jpeg_create_decompress(&decode->jpeg);
    jpeg_mem_src(&decode->jpeg, data->bytes, (unsigned long)data->length);
    if (jpeg_read_header(&decode->jpeg, TRUE) != JPEG_HEADER_OK) goto done;
    asset->width = decode->jpeg.image_width; asset->height = decode->jpeg.image_height;
    if (!raster_bytes(asset->width, asset->height, available, &asset->bytes)) { result = ASSET_HARD; goto done; }
    bool cmyk = decode->jpeg.jpeg_color_space == JCS_CMYK || decode->jpeg.jpeg_color_space == JCS_YCCK;
    decode->jpeg.out_color_space = cmyk ? JCS_CMYK : JCS_RGB;
    if (!jpeg_start_decompress(&decode->jpeg)) goto done;
    if (decode->jpeg.output_width != asset->width || decode->jpeg.output_height != asset->height ||
            decode->jpeg.output_components != (cmyk ? 4 : 3)) goto done;
    decode->pixels = malloc((size_t)asset->bytes);
    decode->row = malloc((size_t)asset->width * decode->jpeg.output_components);
    if (!decode->pixels || !decode->row) { result = ASSET_HARD; goto done; }
    while (decode->jpeg.output_scanline < asset->height) {
        size_t y = decode->jpeg.output_scanline;
        if (jpeg_read_scanlines(&decode->jpeg, &decode->row, 1) != 1) goto done;
        for (uint32_t x = 0; x < asset->width; x++) {
            unsigned char *pixel = decode->row + x * decode->jpeg.output_components;
            unsigned r = pixel[0], g = pixel[1], b = pixel[2];
            if (cmyk) {
                unsigned k = pixel[3];
                if (decode->jpeg.saw_Adobe_marker) { r = r * k / 255; g = g * k / 255; b = b * k / 255; }
                else { r = (255 - r) * (255 - k) / 255; g = (255 - g) * (255 - k) / 255; b = (255 - b) * (255 - k) / 255; }
            }
            uint32_t value = UINT32_C(0xff000000) | (r << 16) | (g << 8) | b;
            memcpy(decode->pixels + (y * asset->width + x) * 4, &value, sizeof(value));
        }
    }
    if (!jpeg_finish_decompress(&decode->jpeg)) goto done;
    asset->pixels = decode->pixels; decode->pixels = NULL;
    result = ASSET_OK;
done:
    if (decode->created) jpeg_destroy_decompress(&decode->jpeg);
    free(decode->row); free(decode->pixels); free(decode);
    return result;
}

static enum asset_status decode_svg(struct ui_asset *asset,
        const struct asset_data *data, uint64_t available) {
    if (data->length > available) return ASSET_HARD;
    GError *error = NULL;
    asset->svg = rsvg_handle_new_from_data(data->bytes, data->length, &error);
    if (error) g_error_free(error);
    if (!asset->svg) return ASSET_MISSING;
    rsvg_handle_set_dpi(asset->svg, 96.0);
    if (!rsvg_handle_get_intrinsic_size_in_pixels(asset->svg, &asset->svg_width, &asset->svg_height)) {
        gboolean has_viewbox;
        RsvgRectangle viewbox;
        rsvg_handle_get_intrinsic_dimensions(asset->svg, NULL, NULL, NULL, NULL, &has_viewbox, &viewbox);
        asset->svg_width = has_viewbox ? viewbox.width : 300.0;
        asset->svg_height = has_viewbox ? viewbox.height : 150.0;
    }
    if (!isfinite(asset->svg_width) || !isfinite(asset->svg_height) ||
            asset->svg_width <= 0 || asset->svg_height <= 0) return ASSET_MISSING;
    if (asset->svg_width > UINT32_MAX || asset->svg_height > UINT32_MAX) return ASSET_HARD;
    asset->width = (uint32_t)ceil(asset->svg_width);
    asset->height = (uint32_t)ceil(asset->svg_height);
    if (asset->width == UINT32_MAX && asset->height == UINT32_MAX) return ASSET_HARD;
    asset->bytes = data->length;
    return ASSET_OK;
}

static void free_content(struct ui_asset *asset) {
    if (asset->raster) cairo_surface_destroy(asset->raster);
    if (asset->svg) g_object_unref(asset->svg);
    free(asset->pixels);
    asset->raster = NULL; asset->svg = NULL; asset->pixels = NULL;
    asset->width = asset->height = 0; asset->bytes = 0;
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

uint64_t tomoe_ui_asset_load(struct tomoe *s, const char *owner,
        uint64_t source_id, int kind, const char *path, const char *name) {
    if (!s || !source_id || !bounded_string(owner) || !*owner ||
            !bounded_string(path) || !bounded_string(name) ||
            (kind != 1 && kind != 2) || (*path && *path != '/') ||
            (kind == 1 && (!*path || *name)) || (kind == 2 && !*name)) return 0;
    if (!s->ui_assets) {
        s->ui_assets = calloc(1, sizeof(*s->ui_assets));
        if (!s->ui_assets) return 0;
        s->ui_assets->next_id = 1;
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
    asset->kind = kind; asset->source_id = source_id;
    struct asset_data data = {0};
    if (pool->loads != UINT64_MAX) pool->loads++;
    enum asset_status status = kind == 1 ? read_asset(path, ASSET_FILE_LIMIT, &data) : read_icon(path, name, &data);
    uint64_t available = ASSET_POOL_LIMIT - pool->bytes;
    if (status == ASSET_OK) {
        if (kind == 2) status = decode_svg(asset, &data, available);
        else if (data.length >= 8 && !memcmp(data.bytes, "\x89PNG\r\n\x1a\n", 8)) status = decode_png(asset, &data, available);
        else if (data.length >= 2 && data.bytes[0] == 0xff && data.bytes[1] == 0xd8) status = decode_jpeg(asset, &data, available);
        else status = ASSET_MISSING;
    }
    free(data.bytes);
    if (status == ASSET_OK && kind == 1) {
        asset->raster = cairo_image_surface_create_for_data(asset->pixels, CAIRO_FORMAT_ARGB32,
            (int)asset->width, (int)asset->height, (int)asset->width * 4);
        if (!asset->raster || cairo_surface_status(asset->raster) != CAIRO_STATUS_SUCCESS) status = ASSET_HARD;
    }
    if (status == ASSET_HARD) { free_asset(asset); return 0; }
    if (status == ASSET_MISSING) {
        free_content(asset);
        fprintf(stderr, "tomoe: shell %s asset unavailable: %s\n", kind == 1 ? "image" : "icon", *path ? path : name);
    }
    asset->pool = pool; asset->references = 1; asset->scratch = true;
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

static bool render_svg(struct ui_asset *asset, cairo_t *cairo, double width, double height) {
    cairo_save(cairo);
    cairo_scale(cairo, width / asset->svg_width, height / asset->svg_height);
    RsvgRectangle viewport = {0, 0, asset->svg_width, asset->svg_height};
    GError *error = NULL;
    bool result = rsvg_handle_render_document(asset->svg, cairo, &viewport, &error);
    if (error) g_error_free(error);
    cairo_restore(cairo);
    return result && cairo_status(cairo) == CAIRO_STATUS_SUCCESS;
}

bool ui_asset_paint(struct ui_asset *asset, cairo_t *cairo,
        double x, double y, double width, double height, bool tint, uint32_t rgba) {
    if (!asset || !cairo || cairo_status(cairo) != CAIRO_STATUS_SUCCESS ||
            !isfinite(x) || !isfinite(y) || !isfinite(width) || !isfinite(height) ||
            width < 0 || height < 0 || width > ASSET_DIMENSION_LIMIT || height > ASSET_DIMENSION_LIMIT) return false;
    if (!width || !height || (!asset->raster && !asset->svg)) return true;
    uint64_t bytes;
    if (!raster_bytes((uint32_t)ceil(width), (uint32_t)ceil(height), ASSET_RASTER_LIMIT, &bytes)) return false;
    cairo_surface_t *source = asset->raster;
    cairo_surface_t *temporary = NULL;
    if (tint) {
        int w = (int)ceil(width), h = (int)ceil(height);
        temporary = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
        if (!temporary || cairo_surface_status(temporary) != CAIRO_STATUS_SUCCESS) {
            if (temporary) cairo_surface_destroy(temporary);
            return false;
        }
        cairo_t *scratch = cairo_create(temporary);
        bool ok;
        if (asset->svg) ok = render_svg(asset, scratch, w, h);
        else {
            cairo_scale(scratch, (double)w / asset->width, (double)h / asset->height);
            cairo_set_source_surface(scratch, asset->raster, 0, 0);
            cairo_pattern_set_filter(cairo_get_source(scratch), CAIRO_FILTER_BILINEAR);
            cairo_paint(scratch);
            ok = cairo_status(scratch) == CAIRO_STATUS_SUCCESS;
        }
        cairo_destroy(scratch);
        if (!ok) { cairo_surface_destroy(temporary); return false; }
        cairo_surface_flush(temporary);
        unsigned char *pixels = cairo_image_surface_get_data(temporary);
        int stride = cairo_image_surface_get_stride(temporary);
        unsigned r = rgba >> 24, g = (rgba >> 16) & 255, b = (rgba >> 8) & 255, alpha = rgba & 255;
        for (int row = 0; row < h; row++) for (int column = 0; column < w; column++) {
            uint32_t pixel;
            unsigned char *position = pixels + (size_t)row * stride + (size_t)column * 4;
            memcpy(&pixel, position, sizeof(pixel));
            unsigned a = (pixel >> 24) * alpha / 255;
            pixel = (a << 24) | (((r * a + 127) / 255) << 16) |
                (((g * a + 127) / 255) << 8) | ((b * a + 127) / 255);
            memcpy(position, &pixel, sizeof(pixel));
        }
        cairo_surface_mark_dirty(temporary);
        source = temporary;
    }
    cairo_save(cairo);
    cairo_translate(cairo, x, y);
    cairo_rectangle(cairo, 0, 0, width, height);
    cairo_clip(cairo);
    bool result = true;
    if (source) {
        cairo_scale(cairo, width / cairo_image_surface_get_width(source),
            height / cairo_image_surface_get_height(source));
        cairo_set_source_surface(cairo, source, 0, 0);
        cairo_pattern_set_filter(cairo_get_source(cairo), CAIRO_FILTER_BILINEAR);
        cairo_pattern_set_extend(cairo_get_source(cairo), CAIRO_EXTEND_PAD);
        cairo_paint(cairo);
    } else result = render_svg(asset, cairo, width, height);
    cairo_restore(cairo);
    if (temporary) cairo_surface_destroy(temporary);
    return result && cairo_status(cairo) == CAIRO_STATUS_SUCCESS;
}

uint64_t ui_assets_count(struct tomoe *s) { return s && s->ui_assets ? s->ui_assets->count : 0; }
uint64_t ui_assets_bytes(struct tomoe *s) { return s && s->ui_assets ? s->ui_assets->bytes : 0; }
uint64_t ui_assets_loads(struct tomoe *s) { return s && s->ui_assets ? s->ui_assets->loads : 0; }

void ui_assets_finish(struct tomoe *s) {
    if (!s || !s->ui_assets) return;
    tomoe_ui_assets_discard(s);
    while (s->ui_assets->assets) {
        struct ui_asset *asset = s->ui_assets->assets;
        s->ui_assets->assets = asset->next;
        free_asset(asset);
    }
    free(s->ui_assets); s->ui_assets = NULL;
}
