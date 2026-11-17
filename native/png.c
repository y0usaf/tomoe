#include "png.h"

#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#define MODE_IHDR 0x01u
#define MODE_PLTE 0x02u
#define MODE_IDAT 0x04u
#define MODE_AFTER_IDAT 0x08u
#define MODE_IEND 0x10u
#define MODE_ACTL 0x20u
#define MODE_FCTL 0x40u
#define MODE_COLOR (MODE_PLTE | MODE_IDAT)

#define DIMENSION_LIMIT 1000000u
#define CHUNK_LIMIT 8000000u
#define MAX_PALETTE 256u
#define IDAT_BUFFER 8192u
#define INFLATE_BUFFER 1024u
#define LZMIN 14u

enum png_color {
    COLOR_GRAY = 0, COLOR_RGB = 2, COLOR_PALETTE = 3, COLOR_GRAY_ALPHA = 4, COLOR_RGB_ALPHA = 6,
};

enum png_kind { KIND_SKIP, KIND_IHDR, KIND_PLTE, KIND_IEND, KIND_TRNS, KIND_BKGD, KIND_ACTL };

enum png_output { OUT_ARGB32, OUT_RGB24, OUT_RGBA128F, OUT_RGB96F, OUT_RGBA8 };

struct png_rule {
    uint32_t name;
    uint32_t longest, shortest;
    unsigned before, after;
    int multiple;
    enum png_kind kind;
};

#define CHUNK(a, b, c, d) ((uint32_t)(a) << 24 | (uint32_t)(b) << 16 | \
    (uint32_t)(c) << 8 | (uint32_t)(d))
#define NO_MAX UINT32_MAX

static const struct png_rule rules[] = {
    {CHUNK('I', 'H', 'D', 'R'), 13, 13, MODE_IHDR, 0, 0, KIND_IHDR},
    {CHUNK('P', 'L', 'T', 'E'), NO_MAX, 0, 0, MODE_IHDR, 1, KIND_PLTE},
    {CHUNK('I', 'E', 'N', 'D'), NO_MAX, 0, 0, MODE_AFTER_IDAT, 0, KIND_IEND},
    {CHUNK('t', 'R', 'N', 'S'), 256, 0, MODE_IDAT, MODE_IHDR, 0, KIND_TRNS},
    {CHUNK('c', 'H', 'R', 'M'), 32, 32, MODE_COLOR, MODE_IHDR, 0, KIND_SKIP},
    {CHUNK('g', 'A', 'M', 'A'), 4, 4, MODE_COLOR, MODE_IHDR, 0, KIND_SKIP},
    {CHUNK('i', 'C', 'C', 'P'), CHUNK_LIMIT, LZMIN, MODE_COLOR, MODE_IHDR, 0, KIND_SKIP},
    {CHUNK('s', 'B', 'I', 'T'), 4, 1, MODE_COLOR, MODE_IHDR, 0, KIND_SKIP},
    {CHUNK('s', 'R', 'G', 'B'), 1, 1, MODE_COLOR, MODE_IHDR, 0, KIND_SKIP},
    {CHUNK('c', 'I', 'C', 'P'), 4, 4, MODE_COLOR, MODE_IHDR, 0, KIND_SKIP},
    {CHUNK('m', 'D', 'C', 'V'), 24, 24, MODE_COLOR, MODE_IHDR, 0, KIND_SKIP},
    {CHUNK('e', 'X', 'I', 'f'), CHUNK_LIMIT, 4, 0, MODE_IHDR, 0, KIND_SKIP},
    {CHUNK('c', 'L', 'L', 'I'), 8, 8, MODE_COLOR, MODE_IHDR, 0, KIND_SKIP},
    {CHUNK('t', 'E', 'X', 't'), NO_MAX, 2, 0, MODE_IHDR, 1, KIND_SKIP},
    {CHUNK('z', 'T', 'X', 't'), CHUNK_LIMIT, LZMIN, 0, MODE_IHDR, 1, KIND_SKIP},
    {CHUNK('i', 'T', 'X', 't'), NO_MAX, 6, 0, MODE_IHDR, 1, KIND_SKIP},
    {CHUNK('b', 'K', 'G', 'D'), 6, 1, MODE_IDAT, MODE_IHDR, 0, KIND_BKGD},
    {CHUNK('h', 'I', 'S', 'T'), 1024, 0, MODE_PLTE, MODE_IHDR, 0, KIND_SKIP},
    {CHUNK('p', 'H', 'Y', 's'), 9, 9, MODE_IDAT, MODE_IHDR, 0, KIND_SKIP},
    {CHUNK('s', 'P', 'L', 'T'), NO_MAX, 3, MODE_IDAT, MODE_IHDR, 1, KIND_SKIP},
    {CHUNK('t', 'I', 'M', 'E'), 7, 7, 0, MODE_IHDR, 0, KIND_SKIP},
    {CHUNK('o', 'F', 'F', 's'), 9, 9, MODE_IDAT, MODE_IHDR, 0, KIND_SKIP},
    {CHUNK('p', 'C', 'A', 'L'), NO_MAX, 14, MODE_IDAT, MODE_IHDR, 0, KIND_SKIP},
    {CHUNK('s', 'C', 'A', 'L'), CHUNK_LIMIT, 4, MODE_IDAT, MODE_IHDR, 0, KIND_SKIP},
    {CHUNK('a', 'c', 'T', 'L'), 8, 8, MODE_IDAT, MODE_IHDR, 0, KIND_ACTL},
};

#define RULE_COUNT (sizeof(rules) / sizeof(*rules))

struct png_reader {
    const unsigned char *bytes;
    size_t length, offset;
    uint32_t width, height;
    unsigned depth, color, interlace;
    unsigned mode;
    unsigned char seen[RULE_COUNT];
    int have_trns, have_bkgd;
    unsigned char palette[3 * MAX_PALETTE];
    uint32_t palette_count;
    unsigned char trans[MAX_PALETTE];
    uint32_t trans_count, trans_gray, trans_rgb[3];
    uint32_t chunk_length, chunk_name, crc;
    uint32_t idat_size, next_sequence;
    z_stream stream;
    int stream_ready, stream_ended;
    unsigned char idat[IDAT_BUFFER];
    enum png_output output;
    unsigned char *pixels;
    uint32_t stride;
    unsigned char **rows;
    unsigned char *row, *previous, *line;
};

static uint32_t be32(const unsigned char *bytes) {
    return (uint32_t)bytes[0] << 24 | (uint32_t)bytes[1] << 16 |
        (uint32_t)bytes[2] << 8 | bytes[3];
}

static uint32_t be16(const unsigned char *bytes) {
    return (uint32_t)bytes[0] << 8 | bytes[1];
}

static void put32(unsigned char *bytes, uint32_t value) {
    bytes[0] = (unsigned char)(value >> 24);
    bytes[1] = (unsigned char)(value >> 16);
    bytes[2] = (unsigned char)(value >> 8);
    bytes[3] = (unsigned char)value;
}

static int read_bytes(struct png_reader *png, void *output, size_t length) {
    if (length > png->length - png->offset) return 0;
    memcpy(output, png->bytes + png->offset, length);
    png->offset += length;
    return 1;
}

static int chunk_bytes(struct png_reader *png, unsigned char *output, uint32_t length) {
    if (!read_bytes(png, output, length)) return 0;
    png->crc = (uint32_t)crc32(png->crc, output, length);
    return 1;
}

static int name_valid(uint32_t name) {
    for (unsigned i = 0; i < 4; i++) {
        unsigned byte = (name >> (24 - 8 * i)) & 0xff;
        if (byte < 'A' || (byte > 'Z' && byte < 'a') || byte > 'z') return 0;
        if (i == 2 && byte > 'Z') return 0;
    }
    return 1;
}

static int read_header(struct png_reader *png) {
    unsigned char header[8];
    if (!read_bytes(png, header, sizeof(header))) return 0;
    if (header[0] & 0x80) return 0;
    png->chunk_length = be32(header);
    png->chunk_name = be32(header + 4);
    if (!name_valid(png->chunk_name)) return 0;
    png->crc = (uint32_t)crc32(0, header + 4, 4);
    return 1;
}

static int finish_chunk(struct png_reader *png, uint32_t skip, int as_ancillary) {
    unsigned char buffer[256], stored[4];
    while (skip) {
        uint32_t count = skip < sizeof(buffer) ? skip : (uint32_t)sizeof(buffer);
        if (!chunk_bytes(png, buffer, count)) return 1;
        skip -= count;
    }
    if (!read_bytes(png, stored, sizeof(stored))) return 1;
    if (be32(stored) == png->crc) return 0;
    return (as_ancillary || (png->chunk_name & 0x20000000u)) ? 2 : 1;
}

static const struct png_rule *rule_for(uint32_t name) {
    for (size_t i = 0; i < RULE_COUNT; i++)
        if (rules[i].name == name) return &rules[i];
    return NULL;
}

static size_t rule_index(uint32_t name) {
    for (size_t i = 0; i < RULE_COUNT; i++)
        if (rules[i].name == name) return i;
    return RULE_COUNT;
}

static int handle_ihdr(struct png_reader *png) {
    unsigned char header[13];
    uint32_t width, height;
    unsigned depth, color, compression, filter, interlace;
    if (!chunk_bytes(png, header, sizeof(header))) return 1;
    if (finish_chunk(png, 0, 0)) return 1;
    if (header[0] & 0x80 || header[4] & 0x80) return 1;
    width = be32(header);
    height = be32(header + 4);
    depth = header[8];
    color = header[9];
    compression = header[10];
    filter = header[11];
    interlace = header[12];
    if (!width || width > DIMENSION_LIMIT || !height || height > DIMENSION_LIMIT) return 1;
    if (depth != 1 && depth != 2 && depth != 4 && depth != 8 && depth != 16) return 1;
    if (color != COLOR_GRAY && color != COLOR_RGB && color != COLOR_PALETTE &&
            color != COLOR_GRAY_ALPHA && color != COLOR_RGB_ALPHA) return 1;
    if ((color == COLOR_PALETTE && depth > 8) ||
            ((color == COLOR_RGB || color == COLOR_GRAY_ALPHA ||
              color == COLOR_RGB_ALPHA) && depth < 8)) return 1;
    if (interlace > 1 || compression || filter) return 1;
    png->width = width;
    png->height = height;
    png->depth = depth;
    png->color = color;
    png->interlace = interlace;
    png->mode |= MODE_IHDR;
    return 0;
}

static int handle_plte(struct png_reader *png) {
    unsigned char palette[3 * MAX_PALETTE];
    unsigned limit = png->color == COLOR_PALETTE ? 1u << png->depth : MAX_PALETTE;
    unsigned count;
    const char *error = NULL;
    if (png->mode & MODE_PLTE) error = "duplicate";
    else if (png->mode & MODE_IDAT) error = "out of place";
    else if (!(png->color & COLOR_RGB)) error = "ignored in grayscale PNG";
    else if (png->chunk_length > 3 * MAX_PALETTE || png->chunk_length % 3) error = "invalid";
    else if (png->color != COLOR_PALETTE && (png->have_trns || png->have_bkgd)) error = "out of place";
    if (error) {
        if (finish_chunk(png, png->chunk_length, png->color != COLOR_PALETTE) == 1) return 1;
        return png->color == COLOR_PALETTE ? 1 : -1;
    }
    count = png->chunk_length / 3;
    if (count > limit) count = limit;
    if (!chunk_bytes(png, palette, count * 3)) return 1;
    if (finish_chunk(png, png->chunk_length - count * 3,
            png->color != COLOR_PALETTE) == 1) return 1;
    if (!count) return 1;
    memcpy(png->palette, palette, count * 3);
    png->palette_count = count;
    png->mode |= MODE_PLTE;
    return 0;
}

static int handle_trns(struct png_reader *png) {
    unsigned char data[MAX_PALETTE];
    uint32_t length = png->chunk_length;
    if (png->color == COLOR_GRAY) {
        unsigned char buf[2];
        if (length != 2) return finish_chunk(png, length, 0) == 1 ? 1 : -1;
        if (!chunk_bytes(png, buf, 2)) return 1;
        png->trans_gray = be16(buf);
        png->trans_count = 1;
    } else if (png->color == COLOR_RGB) {
        unsigned char buf[6];
        if (length != 6) return finish_chunk(png, length, 0) == 1 ? 1 : -1;
        if (!chunk_bytes(png, buf, 6)) return 1;
        png->trans_count = 1;
        png->trans_rgb[0] = be16(buf);
        png->trans_rgb[1] = be16(buf + 2);
        png->trans_rgb[2] = be16(buf + 4);
    } else if (png->color == COLOR_PALETTE) {
        if (!(png->mode & MODE_PLTE)) return finish_chunk(png, length, 0) == 1 ? 1 : -1;
        if (length > png->palette_count || length > MAX_PALETTE || !length)
            return finish_chunk(png, length, 0) == 1 ? 1 : -1;
        if (!chunk_bytes(png, data, length)) return 1;
        png->trans_count = length;
    } else
        return finish_chunk(png, length, 0) == 1 ? 1 : -1;
    {
        int result = finish_chunk(png, 0, 0);
        if (result == 1) return 1;
        if (result != 0) { png->trans_count = 0; return -1; }
    }
    if (png->color == COLOR_PALETTE) memcpy(png->trans, data, png->trans_count);
    png->have_trns = 1;
    return 0;
}

static int handle_actl(struct png_reader *png) {
    unsigned char data[8];
    uint32_t length = png->chunk_length;
    if (!(png->mode & MODE_IHDR)) return 1;
    if (png->mode & (MODE_IDAT | MODE_ACTL)) return finish_chunk(png, length, 0) == 1;
    if (length != 8) return finish_chunk(png, length, 0) == 1;
    if (!chunk_bytes(png, data, 8)) return 1;
    if (finish_chunk(png, 0, 0) == 1) return 1;
    if ((data[0] & 0x80) || (data[4] & 0x80)) return 1;
    if (data[0] || data[1] || data[2] || data[3]) png->mode |= MODE_ACTL;
    return 0;
}

static int next_sequence(struct png_reader *png, uint32_t length) {
    unsigned char data[4];
    if (length < 4) return 1;
    if (!chunk_bytes(png, data, 4)) return 1;
    if (data[0] & 0x80) return 1;
    if (be32(data) != png->next_sequence) return 1;
    png->next_sequence++;
    return 0;
}

static int handle_fctl(struct png_reader *png) {
    unsigned char data[22];
    uint32_t length = png->chunk_length;
    if (next_sequence(png, length)) return 1;
    if (!(png->mode & MODE_IHDR)) return 1;
    if (png->mode & (MODE_IDAT | MODE_FCTL)) return finish_chunk(png, length - 4, 0) == 1;
    if (length != 26) return finish_chunk(png, length - 4, 0) == 1;
    if (!chunk_bytes(png, data, 22)) return 1;
    if (finish_chunk(png, 0, 0) == 1) return 1;
    if ((data[0] & 0x80) || (data[4] & 0x80) || (data[8] & 0x80) || (data[12] & 0x80))
        return 1;
    if (be32(data + 8) || be32(data + 12)) return 0;
    if (be32(data) != png->width || be32(data + 4) != png->height) return 0;
    if (data[20] > 2 || data[21] > 1) return 1;
    png->mode |= MODE_FCTL;
    return 0;
}

static int handle_fdat(struct png_reader *png) {
    if (next_sequence(png, png->chunk_length)) return 1;
    return finish_chunk(png, png->chunk_length - 4, 0) == 1;
}

static int handle_bkgd(struct png_reader *png) {
    unsigned char buf[6];
    unsigned length = png->chunk_length, wanted;
    if (png->color == COLOR_PALETTE) {
        if (!(png->mode & MODE_PLTE)) return finish_chunk(png, length, 0) == 1 ? 1 : -1;
        wanted = 1;
    } else
        wanted = (png->color & COLOR_RGB) ? 6 : 2;
    if (length != wanted) return finish_chunk(png, length, 0) == 1 ? 1 : -1;
    if (!chunk_bytes(png, buf, length)) return 1;
    {
        int result = finish_chunk(png, 0, 0);
        if (result == 1) return 1;
        if (result != 0) return -1;
    }
    if (png->color == COLOR_PALETTE) {
        if (png->palette_count && buf[0] >= png->palette_count) return -1;
    } else if (!(png->color & COLOR_RGB)) {
        if (png->depth <= 8 && (buf[0] || buf[1] >= (1u << png->depth))) return -1;
    } else if (png->depth <= 8 && (buf[0] || buf[2] || buf[4])) return -1;
    png->have_bkgd = 1;
    return 0;
}

static int handle_chunk(struct png_reader *png) {
    const struct png_rule *rule = rule_for(png->chunk_name);
    uint32_t length = png->chunk_length;
    const char *error = NULL;
    if (!rule)
        return (png->chunk_name & 0x20000000u) ? finish_chunk(png, length, 0) == 1 : 1;
    if (rule->kind != KIND_IHDR && !(png->mode & MODE_IHDR)) return 1;
    if ((png->mode & rule->before) || (png->mode & rule->after) != rule->after)
        error = "out of place";
    else if (!rule->multiple && png->seen[rule_index(png->chunk_name)])
        error = "duplicate";
    else if (length < rule->shortest) error = "too short";
    else if (length > rule->longest) error = "too long";
    if (error) {
        if (!(png->chunk_name & 0x20000000u)) return 1;
        return finish_chunk(png, length, 0) == 1;
    }
    int result = 0;
    switch (rule->kind) {
    case KIND_IHDR: result = handle_ihdr(png); break;
    case KIND_PLTE: result = handle_plte(png); break;
    case KIND_IEND:
        if (finish_chunk(png, length, 1) == 1) return 1;
        png->mode |= MODE_AFTER_IDAT | MODE_IEND;
        return 0;
    case KIND_TRNS: result = handle_trns(png); break;
    case KIND_BKGD: result = handle_bkgd(png); break;
    case KIND_ACTL: result = handle_actl(png); break;
    default: result = finish_chunk(png, length, 0) == 0 ? 0 : -1; break;
    }
    if (result) return result > 0 ? 1 : 0;
    png->seen[rule_index(png->chunk_name)] = 1;
    return 0;
}

static int read_info(struct png_reader *png) {
    static const unsigned char signature[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
    unsigned char actual[8];
    if (!read_bytes(png, actual, sizeof(actual)) || memcmp(actual, signature, sizeof(actual)))
        return 1;
    for (;;) {
        if (!read_header(png)) return 1;
        if (png->chunk_name == CHUNK('I', 'D', 'A', 'T')) {
            if (!(png->mode & MODE_IHDR)) return 1;
            if (png->color == COLOR_PALETTE && !(png->mode & MODE_PLTE)) return 1;
            png->mode |= MODE_IDAT;
            png->idat_size = png->chunk_length;
            return 0;
        }
        if (png->chunk_name == CHUNK('a', 'c', 'T', 'L')) {
            if (handle_actl(png)) return 1;
            continue;
        }
        if (png->chunk_name == CHUNK('f', 'c', 'T', 'L')) {
            if (handle_fctl(png)) return 1;
            continue;
        }
        if (png->chunk_name == CHUNK('f', 'd', 'A', 'T')) {
            if (handle_fdat(png)) return 1;
            continue;
        }
        if (handle_chunk(png)) return 1;
    }
}

static int source_alpha(const struct png_reader *png) {
    if (png->color == COLOR_RGB_ALPHA || png->color == COLOR_GRAY_ALPHA) return 1;
    return png->have_trns;
}

static unsigned pass_start_x(unsigned pass) {
    static const unsigned values[7] = {0, 4, 0, 2, 0, 1, 0};
    return values[pass];
}

static unsigned pass_start_y(unsigned pass) {
    static const unsigned values[7] = {0, 0, 4, 0, 2, 0, 1};
    return values[pass];
}

static unsigned pass_step_x(unsigned pass) {
    static const unsigned values[7] = {8, 8, 4, 4, 2, 2, 1};
    return values[pass];
}

static unsigned pass_step_y(unsigned pass) {
    static const unsigned values[7] = {8, 8, 8, 4, 4, 2, 2};
    return values[pass];
}

static uint32_t pass_count(uint32_t extent, unsigned start, unsigned step) {
    return extent > start ? (extent - start + step - 1) / step : 0;
}

static void pass_extent(const struct png_reader *png, unsigned pass,
        uint32_t *width, uint32_t *height) {
    if (!png->interlace) {
        *width = png->width;
        *height = png->height;
        return;
    }
    *width = pass_count(png->width, pass_start_x(pass), pass_step_x(pass));
    *height = pass_count(png->height, pass_start_y(pass), pass_step_y(pass));
}

static void put8(unsigned char *row, uint32_t x, unsigned red, unsigned green,
        unsigned blue, unsigned alpha, enum png_output output) {
    if (output == OUT_RGBA8) {
        unsigned char *pixel = row + (size_t)x * 4;
        pixel[0] = (unsigned char)red;
        pixel[1] = (unsigned char)green;
        pixel[2] = (unsigned char)blue;
        pixel[3] = (unsigned char)alpha;
    } else if (output == OUT_RGB24) {
        uint32_t pixel = UINT32_C(0xff000000) | red << 16 | green << 8 | blue;
        memcpy(row + (size_t)x * 4, &pixel, sizeof(pixel));
    } else {
        uint32_t pixel;
        if (!alpha) pixel = 0;
        else {
            if (alpha != 0xff) {
                unsigned temp = alpha * red + 0x80;
                red = (temp + (temp >> 8)) >> 8;
                temp = alpha * green + 0x80;
                green = (temp + (temp >> 8)) >> 8;
                temp = alpha * blue + 0x80;
                blue = (temp + (temp >> 8)) >> 8;
            }
            pixel = alpha << 24 | red << 16 | green << 8 | blue;
        }
        memcpy(row + (size_t)x * 4, &pixel, sizeof(pixel));
    }
}

static void put16(unsigned char *row, uint32_t x, unsigned red, unsigned green,
        unsigned blue, unsigned alpha, enum png_output output) {
    if (output == OUT_RGB96F) {
        float *pixel = (float *)row + (size_t)x * 3;
        pixel[0] = (float)red / 65535.f;
        pixel[1] = (float)green / 65535.f;
        pixel[2] = (float)blue / 65535.f;
    } else {
        float *pixel = (float *)row + (size_t)x * 4;
        float a = (float)alpha / 65535.f;
        pixel[0] = (float)red / 65535.f * a;
        pixel[1] = (float)green / 65535.f * a;
        pixel[2] = (float)blue / 65535.f * a;
        pixel[3] = a;
    }
}

static unsigned paeth(unsigned left, unsigned above, unsigned corner) {
    int p = (int)above - (int)corner;
    int pa = p < 0 ? -p : p;
    int pb = (int)left - (int)corner;
    int pc = p + pb;
    if (pb < 0) pb = -pb;
    if (pc < 0) pc = -pc;
    if (pb < pa) { pa = pb; left = above; }
    if (pc < pa) left = corner;
    return left;
}

static void paint_row(struct png_reader *png, const unsigned char *source, uint32_t y,
        unsigned pass, uint32_t pass_width) {
    unsigned channels = png->color == COLOR_GRAY_ALPHA ? 2 : png->color == COLOR_RGB ? 3 :
        png->color == COLOR_RGB_ALPHA ? 4 : 1;
    unsigned depth = png->depth, alpha = (unsigned)source_alpha(png);
    unsigned char *target = png->rows ? png->rows[y] : png->pixels + (size_t)y * png->stride;
    int eight = depth != 16 || png->output == OUT_RGBA8;
    for (uint32_t i = 0; i < pass_width; i++) {
        uint32_t x = png->interlace ? pass_start_x(pass) + i * pass_step_x(pass) : i;
        unsigned red = 0, green = 0, blue = 0, other = depth == 16 ? 65535u : 255u;
        if (png->color == COLOR_PALETTE) {
            unsigned index = depth == 8 ? source[i] :
                (source[(i * depth) >> 3] >> (8 - depth - ((i * depth) & 7))) &
                    ((1u << depth) - 1);
            red = png->palette[index * 3];
            green = png->palette[index * 3 + 1];
            blue = png->palette[index * 3 + 2];
            other = png->have_trns && index < png->trans_count ? png->trans[index] : 255;
        } else if (depth < 8) {
            unsigned value = (source[(i * depth) >> 3] >>
                (8 - depth - ((i * depth) & 7))) & ((1u << depth) - 1);
            unsigned scaled = value * (depth == 1 ? 255 : depth == 2 ? 85 : 17);
            unsigned key = depth == 1 ? (png->trans_gray & 1) * 255 :
                depth == 2 ? (png->trans_gray & 3) * 85 : (png->trans_gray & 15) * 17;
            red = green = blue = scaled;
            other = png->have_trns && scaled == key ? 0 : 255;
        } else if (depth == 8) {
            const unsigned char *pixel = source + (size_t)i * channels;
            if (channels <= 2) {
                red = green = blue = pixel[0];
                if (channels == 1) {
                    if (png->have_trns && red == (png->trans_gray & 0xff)) other = 0;
                } else other = pixel[1];
            } else {
                red = pixel[0];
                green = pixel[1];
                blue = pixel[2];
                if (channels == 4) other = pixel[3];
                else if (png->have_trns && red == (png->trans_rgb[0] & 0xff) &&
                        green == (png->trans_rgb[1] & 0xff) &&
                        blue == (png->trans_rgb[2] & 0xff)) other = 0;
            }
        } else {
            const unsigned char *pixel = source + (size_t)i * channels * 2;
            if (channels <= 2) {
                red = green = blue = be16(pixel);
                if (channels == 1) {
                    if (png->have_trns && red == png->trans_gray) other = 0;
                } else other = be16(pixel + 2);
            } else {
                red = be16(pixel);
                green = be16(pixel + 2);
                blue = be16(pixel + 4);
                if (channels == 4) other = be16(pixel + 6);
                else if (png->have_trns && red == png->trans_rgb[0] &&
                        green == png->trans_rgb[1] && blue == png->trans_rgb[2]) other = 0;
            }
        }
        if (eight) {
            if (depth == 16) {
                red >>= 8;
                green >>= 8;
                blue >>= 8;
                other = alpha ? other >> 8 : 255;
            }
            put8(target, x, red, green, blue, other, png->output);
        } else
            put16(target, x, red, green, blue, other, png->output);
    }
}

static int read_idat(struct png_reader *png, unsigned char *output, size_t count) {
    unsigned char *cursor = output;
    size_t remaining = count;
    for (;;) {
        int result;
        unsigned space, produced, avail;
        if (!remaining) return 0;
        if (png->stream.avail_in == 0) {
            while (png->idat_size == 0) {
                if (finish_chunk(png, 0, 0)) return 1;
                if (!read_header(png)) return 1;
                if (png->chunk_name != CHUNK('I', 'D', 'A', 'T')) return 1;
                png->idat_size = png->chunk_length;
            }
            avail = png->idat_size > IDAT_BUFFER ? IDAT_BUFFER : png->idat_size;
            if (!chunk_bytes(png, png->idat, avail)) return 1;
            png->idat_size -= avail;
            png->stream.next_in = png->idat;
            png->stream.avail_in = avail;
        }
        space = remaining > 0x40000000u ? 0x40000000u : (unsigned)remaining;
        png->stream.next_out = cursor;
        png->stream.avail_out = space;
        result = inflate(&png->stream, Z_NO_FLUSH);
        produced = space - png->stream.avail_out;
        cursor += produced;
        remaining -= produced;
        png->stream.avail_out = 0;
        if (result == Z_STREAM_END) {
            png->stream.next_out = NULL;
            png->mode |= MODE_AFTER_IDAT;
            png->stream_ended = 1;
            return remaining != 0;
        }
        if (result != Z_OK) return 1;
    }
}

static int finish_idat(struct png_reader *png) {
    if (!png->stream_ended) {
        unsigned char buffer[INFLATE_BUFFER];
        size_t total = 0;
        for (;;) {
            int result;
            unsigned produced, avail;
            if (png->stream.avail_in == 0) {
                while (png->idat_size == 0) {
                    if (finish_chunk(png, 0, 0)) return 1;
                    if (!read_header(png)) return 1;
                    if (png->chunk_name != CHUNK('I', 'D', 'A', 'T')) return 1;
                    png->idat_size = png->chunk_length;
                }
                avail = png->idat_size > IDAT_BUFFER ? IDAT_BUFFER : png->idat_size;
                if (!chunk_bytes(png, png->idat, avail)) return 1;
                png->idat_size -= avail;
                png->stream.next_in = png->idat;
                png->stream.avail_in = avail;
            }
            png->stream.next_out = buffer;
            png->stream.avail_out = sizeof(buffer);
            result = inflate(&png->stream, Z_NO_FLUSH);
            produced = sizeof(buffer) - png->stream.avail_out;
            png->stream.avail_out = 0;
            if (result == Z_STREAM_END) {
                png->mode |= MODE_AFTER_IDAT;
                png->stream_ended = 1;
                break;
            }
            total += produced;
            if (result != Z_OK || !total) break;
        }
        if (!png->stream_ended) {
            png->mode |= MODE_AFTER_IDAT;
            png->stream_ended = 1;
        }
    }
    if (png->stream_ready) {
        png->stream.next_in = NULL;
        png->stream.avail_in = 0;
        png->stream_ready = 0;
        if (finish_chunk(png, png->idat_size, 0)) return 1;
    }
    return 0;
}

static int read_end(struct png_reader *png) {
    for (;;) {
        if (!read_header(png)) return 1;
        if (png->chunk_name != CHUNK('I', 'D', 'A', 'T')) png->mode |= MODE_AFTER_IDAT;
        if (png->chunk_name == CHUNK('I', 'D', 'A', 'T')) {
            if (finish_chunk(png, png->chunk_length, 0)) return 1;
            continue;
        }
        if (handle_chunk(png)) return 1;
        if (png->mode & MODE_IEND) return 0;
    }
}

static int unfilter_row(struct png_reader *png, unsigned char *output,
        const unsigned char *previous, size_t rowbytes, unsigned bytes_per_pixel) {
    unsigned filter = png->row[0];
    const unsigned char *source = png->row + 1;
    if (filter > 4) return 1;
    for (size_t i = 0; i < rowbytes; i++) {
        unsigned left = i >= bytes_per_pixel ? output[i - bytes_per_pixel] : 0;
        unsigned above = previous[i];
        unsigned corner = i >= bytes_per_pixel ? previous[i - bytes_per_pixel] : 0;
        unsigned value = source[i];
        switch (filter) {
        case 1: value += left; break;
        case 2: value += above; break;
        case 3: value += (left + above) >> 1; break;
        case 4: value += paeth(left, above, corner); break;
        default: break;
        }
        output[i] = (unsigned char)value;
    }
    return 0;
}

static void release(struct png_reader *png) {
    if (png->stream_ready) inflateEnd(&png->stream);
    free(png->row);
    free(png->previous);
    free(png->line);
    png->row = png->previous = png->line = NULL;
    png->stream_ready = 0;
}

static enum png_status decode_rows(struct png_reader *png) {
    unsigned channels = png->color == COLOR_GRAY_ALPHA ? 2 : png->color == COLOR_RGB ? 3 :
        png->color == COLOR_RGB_ALPHA ? 4 : 1;
    unsigned pixel_bits = channels * png->depth;
    unsigned bytes_per_pixel = (pixel_bits + 7) / 8;
    size_t largest = 0;
    for (unsigned pass = 0; pass < 7; pass++) {
        uint32_t width, height;
        size_t rowbytes;
        pass_extent(png, pass, &width, &height);
        if (!width || !height) continue;
        rowbytes = ((size_t)width * pixel_bits + 7) / 8;
        if (rowbytes > largest) largest = rowbytes;
    }
    if (!largest) return PNG_BAD;
    png->row = malloc(largest + 1);
    png->previous = calloc(1, largest + 1);
    png->line = malloc(largest + 1);
    if (!png->row || !png->previous || !png->line) return PNG_HARD;
    memset(&png->stream, 0, sizeof(png->stream));
    if (inflateInit2(&png->stream, 0) != Z_OK) return PNG_HARD;
    png->stream_ready = 1;
    for (unsigned pass = png->interlace ? 0 : 6; pass < 7; pass++) {
        uint32_t width, height;
        size_t rowbytes;
        pass_extent(png, pass, &width, &height);
        if (!width || !height) continue;
        rowbytes = ((size_t)width * pixel_bits + 7) / 8;
        memset(png->previous, 0, rowbytes + 1);
        for (uint32_t y = 0; y < height; y++) {
            if (read_idat(png, png->row, rowbytes + 1)) return PNG_BAD;
            if (unfilter_row(png, png->line, png->previous, rowbytes, bytes_per_pixel))
                return PNG_BAD;
            memcpy(png->previous, png->line, rowbytes);
            paint_row(png, png->line, png->interlace ? pass_start_y(pass) + y * pass_step_y(pass) : y,
                pass, width);
        }
    }
    if (finish_idat(png)) return PNG_BAD;
    return PNG_OK;
}

static enum png_status decode(struct png_reader *png) {
    enum png_status status;
    if (read_info(png)) return PNG_BAD;
    if (png->output != OUT_RGBA8) {
        if (png->width > 32767 || png->height > 32767) return PNG_BAD;
        int alpha = source_alpha(png);
        if (!alpha) png->output = png->depth == 16 ? OUT_RGB96F : OUT_RGB24;
        else png->output = png->depth == 16 ? OUT_RGBA128F : OUT_ARGB32;
        png->stride = (uint32_t)cairo_format_stride_for_width(
            png->output == OUT_RGB24 ? CAIRO_FORMAT_RGB24 :
            png->output == OUT_RGB96F ? CAIRO_FORMAT_RGB96F :
            png->output == OUT_RGBA128F ? CAIRO_FORMAT_RGBA128F : CAIRO_FORMAT_ARGB32,
            (int)png->width);
        if (!png->stride) return PNG_BAD;
        png->pixels = malloc((size_t)png->stride * png->height);
        if (!png->pixels) return PNG_HARD;
    }
    status = decode_rows(png);
    if (status != PNG_OK) return status;
    if (read_end(png)) return PNG_BAD;
    return PNG_OK;
}

enum png_status tomoe_png_decode(const unsigned char *bytes, size_t length,
        struct png_image *image) {
    struct png_reader reader;
    enum png_status status;
    memset(&reader, 0, sizeof(reader));
    memset(image, 0, sizeof(*image));
    reader.bytes = bytes;
    reader.length = length;
    reader.output = OUT_ARGB32;
    status = decode(&reader);
    release(&reader);
    if (status != PNG_OK) {
        free(reader.pixels);
        return status;
    }
    image->width = reader.width;
    image->height = reader.height;
    image->stride = reader.stride;
    image->format = reader.output == OUT_RGB24 ? CAIRO_FORMAT_RGB24 :
        reader.output == OUT_RGB96F ? CAIRO_FORMAT_RGB96F :
        reader.output == OUT_RGBA128F ? CAIRO_FORMAT_RGBA128F : CAIRO_FORMAT_ARGB32;
    image->pixels = reader.pixels;
    return PNG_OK;
}

enum png_status tomoe_png_geometry(const unsigned char *bytes, size_t length,
        struct png_geometry *geometry) {
    struct png_reader reader;
    int alpha;
    memset(&reader, 0, sizeof(reader));
    reader.bytes = bytes;
    reader.length = length;
    if (read_info(&reader)) {
        release(&reader);
        return PNG_BAD;
    }
    alpha = source_alpha(&reader);
    geometry->width = reader.width;
    geometry->height = reader.height;
    geometry->alpha = alpha;
    release(&reader);
    return PNG_OK;
}

enum png_status tomoe_png_rgba8(const unsigned char *bytes, size_t length,
        unsigned char *const *rows, size_t rowbytes) {
    struct png_reader reader;
    enum png_status status;
    memset(&reader, 0, sizeof(reader));
    reader.bytes = bytes;
    reader.length = length;
    reader.output = OUT_RGBA8;
    reader.rows = (unsigned char **)rows;
    reader.stride = (uint32_t)rowbytes;
    status = decode(&reader);
    release(&reader);
    return status;
}

static size_t filter_bytes(unsigned filter, const unsigned char *row,
        const unsigned char *previous, unsigned char *output, size_t length,
        unsigned bytes_per_pixel) {
    size_t sum = 0;
    for (size_t i = 0; i < length; i++) {
        unsigned left = i >= bytes_per_pixel ? row[i - bytes_per_pixel] : 0;
        unsigned above = previous[i];
        unsigned corner = i >= bytes_per_pixel ? previous[i - bytes_per_pixel] : 0;
        unsigned value = row[i];
        switch (filter) {
        case 1: value -= left; break;
        case 2: value -= above; break;
        case 3: value -= (left + above) >> 1; break;
        case 4: value -= paeth(left, above, corner); break;
        default: break;
        }
        unsigned char byte = (unsigned char)value;
        output[i] = byte;
        sum += byte < 128 ? byte : 256 - byte;
    }
    return sum;
}

static int reserve(unsigned char **buffer, size_t *capacity, size_t needed) {
    unsigned char *grown;
    size_t size = *capacity ? *capacity : 4096;
    if (needed <= *capacity) return 1;
    while (size < needed) size *= 2;
    grown = realloc(*buffer, size);
    if (!grown) return 0;
    *buffer = grown;
    *capacity = size;
    return 1;
}

static int append(unsigned char **buffer, size_t *length, size_t *capacity,
        const void *data, size_t count) {
    if (!reserve(buffer, capacity, *length + count)) return 0;
    memcpy(*buffer + *length, data, count);
    *length += count;
    return 1;
}

static int append_chunk(unsigned char **buffer, size_t *length, size_t *capacity,
        const char *name, const unsigned char *data, size_t count) {
    unsigned char header[8], trailer[4];
    uint32_t crc;
    if (count > UINT32_MAX) return 0;
    put32(header, (uint32_t)count);
    memcpy(header + 4, name, 4);
    crc = (uint32_t)crc32(0, header + 4, 4);
    crc = (uint32_t)crc32(crc, data, count);
    put32(trailer, crc);
    return append(buffer, length, capacity, header, 8) &&
        append(buffer, length, capacity, data, count) &&
        append(buffer, length, capacity, trailer, 4);
}

static void optimize_cmf(unsigned char *data, size_t size) {
    unsigned cmf, cinfo, half;
    if (size > 16384) return;
    cmf = data[0];
    if ((cmf & 0x0f) != 8 || (cmf & 0xf0) > 0x70) return;
    cinfo = cmf >> 4;
    half = 1u << (cinfo + 7);
    if (size > half) return;
    do {
        half >>= 1;
        --cinfo;
    } while (cinfo > 0 && size <= half);
    cmf = (cmf & 0x0f) | (cinfo << 4);
    data[0] = (unsigned char)cmf;
    unsigned temp = data[1] & 0xe0;
    temp += 0x1f - ((cmf << 8) + temp) % 0x1f;
    data[1] = (unsigned char)temp;
}

static size_t image_size(size_t rowbytes, size_t height) {
    if (rowbytes < 32768 && height < 32768) return (rowbytes + 1) * height;
    return 0xffffffffu;
}

enum png_status tomoe_png_encode(const unsigned char *argb32, uint32_t width,
        uint32_t height, size_t stride, unsigned char **data, size_t *length) {
    static const unsigned char signature[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
    static const unsigned char background[6] = {0, 255, 0, 255, 0, 255};
    unsigned char *filtered = NULL, *raw = NULL, *previous = NULL;
    unsigned char *trial[5] = {NULL, NULL, NULL, NULL, NULL};
    unsigned char *compressed = NULL, *file = NULL;
    unsigned char header[13], scratch[4];
    size_t capacity = 0, file_length = 0, rowbytes, total, compressed_length = 0;
    unsigned bpp, filters, alpha = 0;
    z_stream stream;
    if (!width || !height) return PNG_BAD;
    memset(&stream, 0, sizeof(stream));
    for (uint32_t y = 0; y < height && !alpha; y++) {
        const unsigned char *row = argb32 + (size_t)y * stride;
        for (uint32_t x = 0; x < width; x++) {
            uint32_t pixel;
            memcpy(&pixel, row + (size_t)x * 4, sizeof(pixel));
            if ((pixel >> 24) != 0xff) { alpha = 1; break; }
        }
    }
    bpp = alpha ? 4 : 3;
    rowbytes = (size_t)width * bpp;
    filters = 0xf8;
    if (height == 1) filters &= ~(0x20u | 0x40u | 0x80u);
    if (width == 1) filters &= ~(0x10u | 0x40u | 0x80u);
    if (!filters) filters = 0x08;
    total = (rowbytes + 1) * height;
    filtered = malloc(total);
    raw = malloc(rowbytes + 1);
    previous = calloc(1, rowbytes + 1);
    for (unsigned i = 0; i < 5; i++) trial[i] = malloc(rowbytes + 1);
    if (!filtered || !raw || !previous || !trial[0] || !trial[1] || !trial[2] ||
            !trial[3] || !trial[4]) goto done;
    for (uint32_t y = 0; y < height; y++) {
        const unsigned char *source = argb32 + (size_t)y * stride;
        unsigned char *destination = filtered + (size_t)y * (rowbytes + 1);
        unsigned best = 0;
        size_t limit;
        for (uint32_t x = 0; x < width; x++) {
            uint32_t pixel;
            size_t out = 1 + (size_t)x * bpp;
            memcpy(&pixel, source + (size_t)x * 4, sizeof(pixel));
            if (!alpha) {
                raw[out] = (unsigned char)(pixel >> 16);
                raw[out + 1] = (unsigned char)(pixel >> 8);
                raw[out + 2] = (unsigned char)pixel;
            } else if (!(pixel >> 24)) {
                raw[out] = raw[out + 1] = raw[out + 2] = raw[out + 3] = 0;
            } else {
                unsigned a = pixel >> 24;
                raw[out] = (unsigned char)((((pixel >> 16) & 0xff) * 255 + a / 2) / a);
                raw[out + 1] = (unsigned char)((((pixel >> 8) & 0xff) * 255 + a / 2) / a);
                raw[out + 2] = (unsigned char)(((pixel & 0xff) * 255 + a / 2) / a);
                raw[out + 3] = (unsigned char)a;
            }
        }
        limit = filter_bytes(0, raw + 1, previous + 1, trial[0] + 1, rowbytes, bpp);
        memcpy(trial[0] + 1, raw + 1, rowbytes);
        for (unsigned filter = 1; filter <= 4; filter++) {
            size_t sum;
            if (!(filters & (0x08u << filter))) continue;
            sum = filter_bytes(filter, raw + 1, previous + 1, trial[filter] + 1,
                rowbytes, bpp);
            if (sum < limit) { limit = sum; best = filter; }
        }
        destination[0] = (unsigned char)best;
        memcpy(destination + 1, trial[best] + 1, rowbytes);
        memcpy(previous + 1, raw + 1, rowbytes);
    }
    {
        size_t uncompressed = image_size(rowbytes, height);
        int window = 15, ret;
        if (uncompressed <= 16384) {
            unsigned half = 1u << (window - 1);
            while (uncompressed + 262 <= half) { half >>= 1; window--; }
        }
        if (deflateInit2(&stream, Z_DEFAULT_COMPRESSION, Z_DEFLATED, window, 8,
                filters == 0x08 ? Z_DEFAULT_STRATEGY : Z_FILTERED) != Z_OK) goto done;
        compressed_length = deflateBound(&stream, total) + 64;
        compressed = malloc(compressed_length);
        if (!compressed) goto done;
        stream.next_out = compressed;
        stream.avail_out = (unsigned)compressed_length;
        {
            size_t offset = 0;
            for (;;) {
                size_t left = total - offset;
                unsigned chunk = left > 0x40000000u ? 0x40000000u : (unsigned)left;
                stream.next_in = filtered + offset;
                stream.avail_in = chunk;
                offset += chunk;
                ret = deflate(&stream, offset == total ? Z_FINISH : Z_NO_FLUSH);
                if (ret == Z_STREAM_END) break;
                if (ret != Z_OK) goto done;
            }
        }
        compressed_length -= stream.avail_out;
        deflateEnd(&stream);
        memset(&stream, 0, sizeof(stream));
        optimize_cmf(compressed, uncompressed);
    }
    put32(header, width);
    put32(header + 4, height);
    header[8] = 8;
    header[9] = alpha ? COLOR_RGB_ALPHA : COLOR_RGB;
    header[10] = header[11] = header[12] = 0;
    if (!append(&file, &file_length, &capacity, signature, sizeof(signature)) ||
            !append_chunk(&file, &file_length, &capacity, "IHDR", header, 13) ||
            !append_chunk(&file, &file_length, &capacity, "bKGD", background, 6))
        goto done;
    for (size_t offset = 0; offset < compressed_length; offset += IDAT_BUFFER) {
        size_t count = compressed_length - offset;
        if (count > IDAT_BUFFER) count = IDAT_BUFFER;
        if (!append_chunk(&file, &file_length, &capacity, "IDAT", compressed + offset, count))
            goto done;
    }
    put32(scratch, 0);
    if (!append_chunk(&file, &file_length, &capacity, "IEND", scratch, 0)) goto done;
    free(filtered);
    free(raw);
    free(previous);
    for (unsigned i = 0; i < 5; i++) free(trial[i]);
    free(compressed);
    *data = file;
    *length = file_length;
    return PNG_OK;
done:
    if (stream.state) deflateEnd(&stream);
    free(filtered);
    free(raw);
    free(previous);
    for (unsigned i = 0; i < 5; i++) free(trial[i]);
    free(compressed);
    free(file);
    return PNG_HARD;
}

int tomoe_png_glyph_info(const unsigned char *bytes, unsigned long length,
        unsigned long *width, unsigned long *height, int *alpha) {
    struct png_geometry geometry;
    if (tomoe_png_geometry(bytes, length, &geometry) != PNG_OK) return 1;
    *width = geometry.width;
    *height = geometry.height;
    *alpha = geometry.alpha;
    return 0;
}

int tomoe_png_glyph_rows(const unsigned char *bytes, unsigned long length,
        unsigned char *const *rows, unsigned long rowbytes) {
    return tomoe_png_rgba8(bytes, length, rows, rowbytes) != PNG_OK;
}
