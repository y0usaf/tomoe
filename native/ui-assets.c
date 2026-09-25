#include "internal.h"
#include "ui-assets.h"

#include <errno.h>
#include <fcntl.h>
#include <jpeglib.h>
#include <jerror.h>
#include <librsvg/rsvg.h>
#include <png.h>
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

struct png_decode {
    jmp_buf jump;
    png_structp png;
    png_infop info;
    const struct asset_data *data;
    size_t offset;
    unsigned char *pixels;
    png_bytep *rows;
    bool hard;
};

static void png_failure(png_structp png, png_const_charp message) {
    (void)message;
    struct png_decode *decode = png_get_error_ptr(png);
    longjmp(decode->jump, 1);
}

static void png_warning_ignore(png_structp png, png_const_charp message) {
    (void)png; (void)message;
}

static png_voidp png_allocate(png_structp png, png_alloc_size_t size) {
    struct png_decode *decode = png_get_mem_ptr(png);
    void *memory = malloc(size);
    if (!memory) decode->hard = true;
    return memory;
}

static void png_deallocate(png_structp png, png_voidp memory) {
    (void)png;
    free(memory);
}

static void png_read_bytes(png_structp png, png_bytep output, png_size_t length) {
    struct png_decode *decode = png_get_io_ptr(png);
    if (length > decode->data->length - decode->offset) png_error(png, "truncated PNG");
    memcpy(output, decode->data->bytes + decode->offset, length);
    decode->offset += length;
}

static enum asset_status decode_png(struct ui_asset *asset,
        const struct asset_data *data, uint64_t available) {
    struct png_decode *decode = calloc(1, sizeof(*decode));
    if (!decode) return ASSET_HARD;
    decode->data = data;
    volatile enum asset_status result = ASSET_MISSING;
    if (setjmp(decode->jump)) { result = decode->hard ? ASSET_HARD : ASSET_MISSING; goto done; }
    decode->png = png_create_read_struct_2(PNG_LIBPNG_VER_STRING, decode,
        png_failure, png_warning_ignore, decode, png_allocate, png_deallocate);
    if (!decode->png) { result = ASSET_HARD; goto done; }
    decode->info = png_create_info_struct(decode->png);
    if (!decode->info) { result = ASSET_HARD; goto done; }
    png_set_read_fn(decode->png, decode, png_read_bytes);
    if (data->length >= 24 && !memcmp(data->bytes + 12, "IHDR", 4)) {
        uint32_t width = png_get_uint_32(data->bytes + 16);
        uint32_t height = png_get_uint_32(data->bytes + 20);
        if (!width || !height) goto done;
        if (!raster_bytes(width, height, available, &asset->bytes)) { result = ASSET_HARD; goto done; }
    }
    png_set_user_limits(decode->png, ASSET_DIMENSION_LIMIT, ASSET_DIMENSION_LIMIT);
    png_set_chunk_malloc_max(decode->png, ASSET_FILE_LIMIT);
    png_read_info(decode->png, decode->info);
    asset->width = png_get_image_width(decode->png, decode->info);
    asset->height = png_get_image_height(decode->png, decode->info);
    if (!raster_bytes(asset->width, asset->height, available, &asset->bytes)) { result = ASSET_HARD; goto done; }
    int depth = png_get_bit_depth(decode->png, decode->info);
    int color = png_get_color_type(decode->png, decode->info);
    if (depth == 16) png_set_strip_16(decode->png);
    if (color == PNG_COLOR_TYPE_PALETTE) png_set_palette_to_rgb(decode->png);
    if (color == PNG_COLOR_TYPE_GRAY && depth < 8) png_set_expand_gray_1_2_4_to_8(decode->png);
    bool alpha = png_get_valid(decode->png, decode->info, PNG_INFO_tRNS);
    if (alpha) png_set_tRNS_to_alpha(decode->png);
    if (color == PNG_COLOR_TYPE_GRAY || color == PNG_COLOR_TYPE_GRAY_ALPHA) png_set_gray_to_rgb(decode->png);
    if (!(color & PNG_COLOR_MASK_ALPHA) && !alpha) png_set_add_alpha(decode->png, 255, PNG_FILLER_AFTER);
    png_set_interlace_handling(decode->png);
    png_read_update_info(decode->png, decode->info);
    if (png_get_rowbytes(decode->png, decode->info) != (size_t)asset->width * 4) goto done;
    decode->pixels = malloc((size_t)asset->bytes);
    decode->rows = malloc((size_t)asset->height * sizeof(*decode->rows));
    if (!decode->pixels || !decode->rows) { result = ASSET_HARD; goto done; }
    for (uint32_t y = 0; y < asset->height; y++) decode->rows[y] = decode->pixels + (size_t)y * asset->width * 4;
    png_read_image(decode->png, decode->rows);
    png_read_end(decode->png, NULL);
    for (uint64_t offset = 0; offset < asset->bytes; offset += 4) {
        unsigned char *pixel = decode->pixels + offset;
        unsigned a = pixel[3];
        uint32_t value = (a << 24) | (((pixel[0] * a + 127) / 255) << 16) |
            (((pixel[1] * a + 127) / 255) << 8) | ((pixel[2] * a + 127) / 255);
        memcpy(pixel, &value, sizeof(value));
    }
    asset->pixels = decode->pixels;
    decode->pixels = NULL;
    result = ASSET_OK;
done:
    png_destroy_read_struct(&decode->png, &decode->info, NULL);
    free(decode->rows); free(decode->pixels); free(decode);
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
        else if (data.length >= 8 && !png_sig_cmp(data.bytes, 0, 8)) status = decode_png(asset, &data, available);
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
