#include "gif.h"

#include <stdlib.h>
#include <string.h>

struct gif_reader { const unsigned char *data; size_t length, pos; };

static int gif_u8(struct gif_reader *r) {
    return r->pos < r->length ? r->data[r->pos++] : -1;
}

static int gif_u16(struct gif_reader *r) {
    int a = gif_u8(r), b = gif_u8(r);
    return a < 0 || b < 0 ? -1 : a | (b << 8);
}

static int gif_skip_blocks(struct gif_reader *r) {
    for (;;) {
        int n = gif_u8(r);
        if (n < 0) return 0;
        if (!n) return 1;
        if (r->pos + (size_t)n > r->length) return 0;
        r->pos += (size_t)n;
    }
}

struct gif_bits { const unsigned char *data; size_t length, pos, bit; };

static int gif_code(struct gif_bits *b, int size) {
    unsigned v = 0;
    for (int i = 0; i < size; i++) {
        if (b->pos >= b->length) return -1;
        v |= (unsigned)((b->data[b->pos] >> b->bit) & 1u) << i;
        if (++b->bit == 8) { b->bit = 0; b->pos++; }
    }
    return (int)v;
}

static int gif_lzw(const unsigned char *data, size_t length, int min_code,
        unsigned char *out, size_t count) {
    if (min_code < 1 || min_code > 8) return 0;
    int clear = 1 << min_code, end = clear + 1;
    int size = min_code + 1, next = end + 1;
    unsigned short prefix[4096];
    unsigned char suffix[4096], first[4096];
    for (int i = 0; i < clear; i++) { suffix[i] = (unsigned char)i; first[i] = (unsigned char)i; }
    struct gif_bits bits = { data, length, 0, 0 };
    size_t n = 0;
    int prev = -1;
    unsigned char stack[4096];
    int sp = 0;
    for (;;) {
        int code = gif_code(&bits, size);
        if (code < 0 || code == end) break;
        if (code == clear) {
            size = min_code + 1;
            next = end + 1;
            prev = -1;
            continue;
        }
        if (prev < 0) {
            if (code >= clear) return 0;
            if (n < count) out[n++] = (unsigned char)code;
            prev = code;
            continue;
        }
        int cur = code;
        sp = 0;
        if (code >= next) {
            if (code > next) return 0;
            stack[sp++] = first[prev];
            cur = prev;
        }
        while (cur >= clear) {
            if (sp >= 4096) return 0;
            stack[sp++] = suffix[cur];
            cur = prefix[cur];
        }
        if (sp >= 4096) return 0;
        stack[sp++] = (unsigned char)cur;
        if (next < 4096) {
            prefix[next] = (unsigned short)prev;
            suffix[next] = (unsigned char)cur;
            first[next] = first[prev];
            next++;
            if (next == (1 << size) && size < 12) size++;
        }
        while (sp > 0) {
            if (n < count) out[n++] = stack[--sp];
            else sp--;
        }
        prev = code;
    }
    return n >= count;
}

int tomoe_gif_first_frame(const unsigned char *data, size_t length, struct gif_frame *frame) {
    if (length < 13) return 0;
    if (memcmp(data, "GIF87a", 6) && memcmp(data, "GIF89a", 6)) return 0;
    struct gif_reader r = { data, length, 6 };
    int width = gif_u16(&r), height = gif_u16(&r);
    int flags = gif_u8(&r), background = gif_u8(&r), aspect = gif_u8(&r);
    (void)background;
    (void)aspect;
    if (width < 0 || height < 0 || flags < 0) return 0;
    unsigned char palette[256 * 3];
    memset(palette, 0, sizeof(palette));
    int palette_count = 0;
    if (flags & 0x80) {
        palette_count = 2 << (flags & 7);
        if (r.pos + (size_t)palette_count * 3 > r.length) return 0;
        memcpy(palette, data + r.pos, (size_t)palette_count * 3);
        r.pos += (size_t)palette_count * 3;
    }
    int transparent = -1;
    for (;;) {
        int block = gif_u8(&r);
        if (block < 0) return 0;
        if (block == 0x3b) return 0;
        if (block == 0x21) {
            int label = gif_u8(&r);
            if (label == 0xf9) {
                int size = gif_u8(&r);
                if (size != 4 || r.pos + 5 > r.length) return 0;
                int packed = data[r.pos];
                transparent = packed & 1 ? data[r.pos + 3] : -1;
                r.pos += 5;
            } else if (!gif_skip_blocks(&r)) return 0;
            continue;
        }
        if (block != 0x2c) return 0;
        int left = gif_u16(&r), top = gif_u16(&r);
        int fw = gif_u16(&r), fh = gif_u16(&r);
        int packed = gif_u8(&r);
        if (left < 0 || top < 0 || fw < 0 || fh < 0 || packed < 0) return 0;
        if (left + fw > width || top + fh > height) return 0;
        if (packed & 0x80) {
            palette_count = 2 << (packed & 7);
            if (r.pos + (size_t)palette_count * 3 > r.length) return 0;
            memcpy(palette, data + r.pos, (size_t)palette_count * 3);
            r.pos += (size_t)palette_count * 3;
        }
        if (fw == 0 || fh == 0) return 0;
        if (!palette_count) return 0;
        int min_code = gif_u8(&r);
        if (min_code < 0) return 0;
        size_t stream_length = 0;
        size_t first_block = r.pos;
        for (;;) {
            int n = gif_u8(&r);
            if (n < 0) return 0;
            if (!n) break;
            if (r.pos + (size_t)n > r.length) return 0;
            stream_length += (size_t)n;
            r.pos += (size_t)n;
        }
        unsigned char *stream = malloc(stream_length ? stream_length : 1);
        unsigned char *indices = malloc((size_t)fw * fh);
        unsigned char *pixels = calloc((size_t)fw * fh, 4);
        if (!stream || !indices || !pixels) {
            free(stream);
            free(indices);
            free(pixels);
            return 0;
        }
        size_t pos = first_block;
        size_t offset = 0;
        for (;;) {
            int n = data[pos++];
            if (!n) break;
            memcpy(stream + offset, data + pos, (size_t)n);
            offset += (size_t)n;
            pos += (size_t)n;
        }
        int ok = gif_lzw(stream, stream_length, min_code, indices, (size_t)fw * fh);
        free(stream);
        if (!ok) {
            free(indices);
            free(pixels);
            return 0;
        }
        int interlaced = packed & 0x40;
        static const int starts[4] = { 0, 4, 2, 1 };
        static const int steps[4] = { 8, 8, 4, 2 };
        size_t row = 0;
        int pass = 0;
        for (int k = 0; k < fh; k++) {
            size_t dst = (size_t)k;
            if (interlaced) {
                while (row >= (size_t)fh) {
                    if (++pass >= 4) {
                        free(indices);
                        free(pixels);
                        return 0;
                    }
                    row = starts[pass];
                }
                dst = row;
                row += (size_t)steps[pass];
            }
            for (int x = 0; x < fw; x++) {
                int idx = indices[(size_t)k * (size_t)fw + (size_t)x];
                unsigned char *p = pixels + (dst * (size_t)fw + (size_t)x) * 4;
                if (idx < palette_count) {
                    p[0] = palette[idx * 3 + 0];
                    p[1] = palette[idx * 3 + 1];
                    p[2] = palette[idx * 3 + 2];
                    p[3] = idx == transparent ? 0 : 255;
                }
            }
        }
        free(indices);
        frame->width = (unsigned)fw;
        frame->height = (unsigned)fh;
        frame->canvas_width = (unsigned)width;
        frame->canvas_height = (unsigned)height;
        frame->pixels = pixels;
        return 1;
    }
}
