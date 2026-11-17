/* SPDX-License-Identifier: AGPL-3.0-only AND MIT
   The lossless WebP decoder is ported from image-webp 0.2.4 (MIT OR
   Apache-2.0); see NOTICE. */
#include "webp.h"

#include <stdlib.h>
#include <string.h>

#define WHUFF_CODES 5
#define WMAX_BITS 10
#define WMAX_LEN 15
#define WCODE_LEN_CODES 19

static const int wcode_len_order[WCODE_LEN_CODES] = {
    17, 18, 0, 1, 2, 3, 4, 5, 16, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15,
};

static const signed char wdistance_map[120][2] = {
    { 0, 1 }, { 1, 0 }, { 1, 1 }, { -1, 1 }, { 0, 2 }, { 2, 0 }, { 1, 2 }, { -1, 2 },
    { 2, 1 }, { -2, 1 }, { 2, 2 }, { -2, 2 }, { 0, 3 }, { 3, 0 }, { 1, 3 }, { -1, 3 },
    { 3, 1 }, { -3, 1 }, { 2, 3 }, { -2, 3 }, { 3, 2 }, { -3, 2 }, { 0, 4 }, { 4, 0 },
    { 1, 4 }, { -1, 4 }, { 4, 1 }, { -4, 1 }, { 3, 3 }, { -3, 3 }, { 2, 4 }, { -2, 4 },
    { 4, 2 }, { -4, 2 }, { 0, 5 }, { 3, 4 }, { -3, 4 }, { 4, 3 }, { -4, 3 }, { 5, 0 },
    { 1, 5 }, { -1, 5 }, { 5, 1 }, { -5, 1 }, { 2, 5 }, { -2, 5 }, { 5, 2 }, { -5, 2 },
    { 4, 4 }, { -4, 4 }, { 3, 5 }, { -3, 5 }, { 5, 3 }, { -5, 3 }, { 0, 6 }, { 6, 0 },
    { 1, 6 }, { -1, 6 }, { 6, 1 }, { -6, 1 }, { 2, 6 }, { -2, 6 }, { 6, 2 }, { -6, 2 },
    { 4, 5 }, { -4, 5 }, { 5, 4 }, { -5, 4 }, { 3, 6 }, { -3, 6 }, { 6, 3 }, { -6, 3 },
    { 0, 7 }, { 7, 0 }, { 1, 7 }, { -1, 7 }, { 5, 5 }, { -5, 5 }, { 7, 1 }, { -7, 1 },
    { 4, 6 }, { -4, 6 }, { 6, 4 }, { -6, 4 }, { 2, 7 }, { -2, 7 }, { 7, 2 }, { -7, 2 },
    { 3, 7 }, { -3, 7 }, { 7, 3 }, { -7, 3 }, { 5, 6 }, { -5, 6 }, { 6, 5 }, { -6, 5 },
    { 8, 0 }, { 4, 7 }, { -4, 7 }, { 7, 4 }, { -7, 4 }, { 8, 1 }, { 8, 2 }, { 6, 6 },
    { -6, 6 }, { 8, 3 }, { 5, 7 }, { -5, 7 }, { 7, 5 }, { -7, 5 }, { 8, 4 }, { 6, 7 },
    { -6, 7 }, { 7, 6 }, { -7, 6 }, { 8, 5 }, { 7, 7 }, { -7, 7 }, { 8, 6 }, { 8, 7 },
};

struct wreader {
    const unsigned char *data;
    size_t length, pos;
    unsigned long long buffer;
    int nbits;
};

struct wnode {
    int kind;
    int offset;
    unsigned symbol;
};

struct whuff {
    int single;
    unsigned single_symbol;
    unsigned *table;
    unsigned table_mask;
    struct wnode *tree;
    int tree_length, tree_capacity;
};

struct wmeta {
    unsigned char *data;
    unsigned width, height;
};

struct wstate {
    struct wreader r;
    int error;
    unsigned width, height;
    int transform_kind[4];
    int transform_seen[4];
    unsigned transform_size_bits[4];
    unsigned char *transform_data[4];
    unsigned transform_table_size[4];
    int transform_order[4];
    int transform_count;
};

static int wfill(struct wreader *r) {
    if (r->length - r->pos >= 8) {
        unsigned long long v = 0;
        for (int i = 0; i < 8; i++) v |= (unsigned long long)r->data[r->pos + i] << (8 * i);
        r->pos += (size_t)((63 - r->nbits) / 8);
        r->buffer |= v << r->nbits;
        r->nbits |= 56;
        return 1;
    }
    while (r->pos < r->length && r->nbits < 56) {
        r->buffer |= (unsigned long long)r->data[r->pos] << r->nbits;
        r->nbits += 8;
        r->pos++;
    }
    return 1;
}

static unsigned long long wpeek(const struct wreader *r, int num) {
    if (num <= 0) return 0;
    return r->buffer & (((unsigned long long)1 << num) - 1);
}

static int wconsume(struct wreader *r, int num) {
    if (r->nbits < num) return 0;
    if (num <= 0) return 1;
    r->buffer >>= num;
    r->nbits -= num;
    return 1;
}

static unsigned wread_bits(struct wreader *r, int num, int *ok) {
    if (r->nbits < num && !wfill(r)) *ok = 0;
    if (!*ok) return 0;
    unsigned value = (unsigned)wpeek(r, num);
    if (!wconsume(r, num)) *ok = 0;
    return value;
}

static void whuff_free(struct whuff *h) {
    free(h->table);
    free(h->tree);
    memset(h, 0, sizeof(*h));
}

static int whuff_build_two(struct whuff *h, unsigned zero, unsigned one) {
    memset(h, 0, sizeof(*h));
    h->table = malloc(sizeof(unsigned) * 2);
    h->tree = malloc(sizeof(struct wnode) * 3);
    if (!h->table || !h->tree) return 0;
    h->tree[0].kind = 2; h->tree[0].symbol = zero;
    h->tree[1].kind = 2; h->tree[1].symbol = one;
    h->tree[2].kind = 0;
    h->tree_length = 3;
    h->table[0] = (1u << 16) | zero;
    h->table[1] = (1u << 16) | one;
    h->table_mask = 1;
    return 1;
}

static int whuff_build_single(struct whuff *h, unsigned symbol) {
    memset(h, 0, sizeof(*h));
    h->single = 1;
    h->single_symbol = symbol;
    return 1;
}

static unsigned short wreverse_bits(unsigned short v) {
    unsigned short r = 0;
    for (int i = 0; i < 16; i++) {
        r = (unsigned short)((r << 1) | (v & 1));
        v = (unsigned short)(v >> 1);
    }
    return r;
}

static int whuff_build_implicit(struct whuff *h, const unsigned short *lengths, unsigned count) {
    memset(h, 0, sizeof(*h));
    unsigned histogram[WMAX_LEN + 1];
    memset(histogram, 0, sizeof(histogram));
    unsigned symbols = 0;
    for (unsigned i = 0; i < count; i++) {
        if (!lengths[i]) continue;
        histogram[lengths[i]]++;
        symbols++;
    }
    if (!symbols) return 0;
    if (symbols == 1) {
        for (unsigned i = 0; i < count; i++)
            if (lengths[i]) return whuff_build_single(h, i);
        return 0;
    }
    unsigned next_codes[WMAX_LEN + 1];
    memset(next_codes, 0, sizeof(next_codes));
    int max_length = 0;
    for (int i = WMAX_LEN; i >= 1; i--)
        if (histogram[i]) { max_length = i; break; }
    if (!max_length) return 0;
    unsigned current = 0;
    for (int len = 1; len <= max_length; len++) {
        next_codes[len] = current;
        current = (current + histogram[len]) << 1;
    }
    if (current != (2u << max_length)) return 0;
    unsigned table_bits = (unsigned)max_length < WMAX_BITS ? (unsigned)max_length : WMAX_BITS;
    unsigned table_size = 1u << table_bits;
    h->table_mask = table_size - 1;
    h->table = calloc(table_size, sizeof(unsigned));
    unsigned tree_size = 0;
    for (int i = (int)table_bits + 1; i <= max_length; i++) tree_size += histogram[i];
    h->tree_capacity = (int)(2 * tree_size) + 4;
    h->tree = calloc((size_t)h->tree_capacity, sizeof(struct wnode));
    if (!h->table || !h->tree) return 0;
    for (unsigned symbol = 0; symbol < count; symbol++) {
        unsigned length = lengths[symbol];
        if (!length) continue;
        unsigned code = next_codes[length];
        next_codes[length]++;
        if (length <= table_bits) {
            unsigned j = (unsigned)(wreverse_bits((unsigned short)code) >> (16 - length));
            unsigned entry = (length << 16) | symbol;
            while (j < table_size) {
                h->table[j] = entry;
                j += 1u << length;
            }
        } else {
            unsigned table_index =
                (unsigned)(wreverse_bits((unsigned short)code) >> (16 - length)) & h->table_mask;
            unsigned table_value = h->table[table_index];
            int node_index;
            if (table_value == 0) {
                node_index = h->tree_length;
                h->table[table_index] = (unsigned)(node_index + 1);
                if (h->tree_length >= h->tree_capacity) return 0;
                h->tree[h->tree_length++].kind = 0;
            } else {
                node_index = (int)(table_value - 1);
            }
            unsigned code_value = code;
            for (int depth = (int)length - (int)table_bits - 1; depth >= 0; depth--) {
                struct wnode *node = &h->tree[node_index];
                int offset;
                if (node->kind == 0) {
                    offset = h->tree_length - node_index;
                    node->kind = 1;
                    node->offset = offset;
                    if (h->tree_length + 2 > h->tree_capacity) return 0;
                    h->tree[h->tree_length].kind = 0;
                    h->tree[h->tree_length + 1].kind = 0;
                    h->tree_length += 2;
                } else if (node->kind == 2) {
                    return 0;
                } else {
                    offset = node->offset;
                }
                node_index += offset + (int)((code_value >> depth) & 1);
                if (node_index >= h->tree_length) return 0;
            }
            if (h->tree[node_index].kind != 0) return 0;
            h->tree[node_index].kind = 2;
            h->tree[node_index].symbol = symbol;
        }
    }
    return 1;
}

static int whuff_slow(struct whuff *h, struct wreader *r, unsigned v, int start) {
    int depth = WMAX_BITS;
    int index = start;
    for (;;) {
        if (index < 0 || index >= h->tree_length) return 0;
        struct wnode *node = &h->tree[index];
        if (node->kind == 1) {
            index += node->offset + (int)(v & 1);
            depth++;
            v >>= 1;
        } else if (node->kind == 2) {
            if (!wconsume(r, depth)) return 0;
            return (int)node->symbol;
        } else {
            return 0;
        }
    }
}

static int whuff_read(struct whuff *h, struct wreader *r) {
    if (h->single) return (int)h->single_symbol;
    unsigned v = (unsigned short)r->buffer;
    unsigned entry = h->table[v & h->table_mask];
    if (entry >> 16) {
        if (!wconsume(r, (int)(entry >> 16))) return -1;
        return (int)(entry & 0xffffu);
    }
    return whuff_slow(h, r, v >> WMAX_BITS, (int)(entry & 0xffffu) - 1);
}

static int whuff_peek(struct whuff *h, const struct wreader *r, int *bits, int *symbol) {
    if (h->single) { *bits = 0; *symbol = (int)h->single_symbol; return 1; }
    unsigned v = (unsigned short)r->buffer;
    unsigned entry = h->table[v & h->table_mask];
    if (entry >> 16) { *bits = (int)(entry >> 16); *symbol = (int)(entry & 0xffffu); return 1; }
    return 0;
}

static unsigned wsubsample(unsigned size, unsigned bits) {
    return (size + (1u << bits) - 1) >> bits;
}

static int wread_huffman_code_lengths(struct wstate *s, const unsigned short *code_lengths,
        unsigned num_symbols, unsigned short *out);
static int wread_huffman_code(struct wstate *s, unsigned alphabet, struct whuff *out);

static int wdecode_image_stream(struct wstate *s, unsigned xsize, unsigned ysize, int is_argb,
        unsigned char *data);

static int wread_huffman_codes(struct wstate *s, int read_meta, unsigned xsize, unsigned ysize,
        int cache_bits, unsigned char **entropy, unsigned *entropy_width, unsigned *huff_bits,
        unsigned *huff_mask, struct whuff **groups, unsigned *group_count);

static void wapply_predictor(unsigned char *d, unsigned width, unsigned height, unsigned bits,
        const unsigned char *pred) {
    size_t w4 = (size_t)width * 4;
    unsigned block_xsize = wsubsample(width, bits);
    d[3] = (unsigned char)(d[3] + 255);
    for (size_t i = 4; i < w4; i++) d[i] = (unsigned char)(d[i] + d[i - 4]);
    for (unsigned y = 1; y < height; y++)
        for (int k = 0; k < 4; k++)
            d[(size_t)y * w4 + k] =
                (unsigned char)(d[(size_t)y * w4 + k] + d[(size_t)(y - 1) * w4 + k]);
    for (unsigned y = 1; y < height; y++) {
        for (unsigned block_x = 0; block_x < block_xsize; block_x++) {
            size_t block_index = (size_t)(y >> bits) * block_xsize + block_x;
            unsigned predictor = pred[block_index * 4 + 1];
            unsigned start_x = (block_x << bits) > 1 ? (block_x << bits) : 1;
            unsigned end_x = ((block_x + 1) << bits) < width ? ((block_x + 1) << bits) : width;
            size_t start = ((size_t)y * width + start_x) * 4;
            size_t end = ((size_t)y * width + end_x) * 4;
            switch (predictor) {
            case 0:
                for (size_t i = start + 3; i < end; i += 4) d[i] = (unsigned char)(d[i] + 255);
                break;
            case 1:
                for (size_t i = start; i < end; i++) d[i] = (unsigned char)(d[i] + d[i - 4]);
                break;
            case 2:
                for (size_t i = start; i < end; i++)
                    d[i] = (unsigned char)(d[i] + d[i - w4]);
                break;
            case 3:
                for (size_t i = start; i < end; i++)
                    d[i] = (unsigned char)(d[i] + d[i - w4 + 4]);
                break;
            case 4:
                for (size_t i = start; i < end; i++)
                    d[i] = (unsigned char)(d[i] + d[i - w4 - 4]);
                break;
            case 5: {
                unsigned char prev[4];
                memcpy(prev, d + start - 4, 4);
                for (size_t i = start; i < end; i += 4) {
                    for (int k = 0; k < 4; k++) {
                        unsigned char a = (unsigned char)((prev[k] & d[i - w4 + 4 + k]) +
                            ((prev[k] ^ d[i - w4 + 4 + k]) >> 1));
                        unsigned char b = (unsigned char)((a & d[i - w4 + k]) +
                            ((a ^ d[i - w4 + k]) >> 1));
                        prev[k] = (unsigned char)(d[i + k] + b);
                        d[i + k] = prev[k];
                    }
                }
                break;
            }
            case 6:
                for (size_t i = start; i < end; i++)
                    d[i] = (unsigned char)(d[i] + (unsigned char)(((unsigned)d[i - 4] +
                        (unsigned)d[i - w4 - 4]) / 2));
                break;
            case 7: {
                unsigned char prev[4];
                memcpy(prev, d + start - 4, 4);
                for (size_t i = start; i < end; i += 4) {
                    for (int k = 0; k < 4; k++) {
                        unsigned char a = (unsigned char)((prev[k] & d[i - w4 + k]) +
                            ((prev[k] ^ d[i - w4 + k]) >> 1));
                        prev[k] = (unsigned char)(d[i + k] + a);
                        d[i + k] = prev[k];
                    }
                }
                break;
            }
            case 8:
                for (size_t i = start; i < end; i++)
                    d[i] = (unsigned char)(d[i] + (unsigned char)(((unsigned)d[i - w4 - 4] +
                        (unsigned)d[i - w4]) / 2));
                break;
            case 9:
                for (size_t i = start; i < end; i++)
                    d[i] = (unsigned char)(d[i] + (unsigned char)(((unsigned)d[i - w4] +
                        (unsigned)d[i - w4 + 4]) / 2));
                break;
            case 10: {
                unsigned char prev[4];
                memcpy(prev, d + start - 4, 4);
                for (size_t i = start; i < end; i += 4) {
                    for (int k = 0; k < 4; k++) {
                        unsigned char a = (unsigned char)(((unsigned)prev[k] +
                            (unsigned)d[i - w4 - 4 + k]) / 2);
                        unsigned char b = (unsigned char)(((unsigned)d[i - w4 + k] +
                            (unsigned)d[i - w4 + 4 + k]) / 2);
                        prev[k] = (unsigned char)(d[i + k] + (unsigned char)(((unsigned)a +
                            (unsigned)b) / 2));
                        d[i + k] = prev[k];
                    }
                }
                break;
            }
            case 11: {
                int l[4], tl[4];
                for (int k = 0; k < 4; k++) {
                    l[k] = d[start - 4 + k];
                    tl[k] = d[start - w4 - 4 + k];
                }
                for (size_t i = start; i < end; i += 4) {
                    int t[4];
                    for (int k = 0; k < 4; k++) t[k] = d[i - w4 + k];
                    int predict_left = 0, predict_top = 0;
                    for (int k = 0; k < 4; k++) {
                        int v = l[k] + t[k] - tl[k];
                        int dl = v - l[k], dt = v - t[k];
                        predict_left += dl < 0 ? -dl : dl;
                        predict_top += dt < 0 ? -dt : dt;
                    }
                    if (predict_left < predict_top) {
                        for (int k = 0; k < 4; k++)
                            d[i + k] = (unsigned char)(d[i + k] + (unsigned char)l[k]);
                    } else {
                        for (int k = 0; k < 4; k++)
                            d[i + k] = (unsigned char)(d[i + k] + (unsigned char)t[k]);
                    }
                    for (int k = 0; k < 4; k++) { tl[k] = t[k]; l[k] = d[i + k]; }
                }
                break;
            }
            case 12: {
                unsigned char prev[4];
                memcpy(prev, d + start - 4, 4);
                for (size_t i = start; i < end; i += 4) {
                    for (int k = 0; k < 4; k++) {
                        int v = (int)prev[k] + (int)d[i - w4 + k] - (int)d[i - w4 - 4 + k];
                        if (v < 0) v = 0;
                        if (v > 255) v = 255;
                        prev[k] = (unsigned char)(d[i + k] + (unsigned char)v);
                        d[i + k] = prev[k];
                    }
                }
                break;
            }
            case 13: {
                unsigned char prev[4];
                memcpy(prev, d + start - 4, 4);
                for (size_t i = start; i < end; i += 4) {
                    for (int k = 0; k < 4; k++) {
                        int a = ((int)prev[k] + (int)d[i - w4 + k]) / 2;
                        int v = a + (a - (int)d[i - w4 - 4 + k]) / 2;
                        if (v < 0) v = 0;
                        if (v > 255) v = 255;
                        prev[k] = (unsigned char)(d[i + k] + (unsigned char)v);
                        d[i + k] = prev[k];
                    }
                }
                break;
            }
            default:
                break;
            }
        }
    }
}

static void wapply_color_rows(unsigned char *d, unsigned width, unsigned height, unsigned bits,
        const unsigned char *transform) {
    size_t w4 = (size_t)width * 4;
    unsigned block_xsize = wsubsample(width, bits);
    for (unsigned y = 0; y < height; y++) {
        const unsigned char *row = transform + (size_t)(y >> bits) * block_xsize * 4;
        for (unsigned block = 0; block < block_xsize; block++) {
            unsigned red_to_blue = row[block * 4 + 0];
            unsigned green_to_blue = row[block * 4 + 1];
            unsigned green_to_red = row[block * 4 + 2];
            size_t block_start = (size_t)y * w4 + ((size_t)block << bits) * 4;
            size_t block_end = block_start + ((size_t)1 << bits) * 4;
            if (block_end > (size_t)y * w4 + w4) block_end = (size_t)y * w4 + w4;
            for (size_t i = block_start; i < block_end; i += 4) {
                unsigned green = d[i + 1];
                unsigned temp_red = d[i + 0];
                unsigned temp_blue = d[i + 2];
                temp_red += (unsigned)((int)(signed char)green_to_red * (int)(signed char)green) >> 5;
                temp_blue += (unsigned)((int)(signed char)green_to_blue * (int)(signed char)green) >> 5;
                temp_blue += (unsigned)((int)(signed char)red_to_blue * (int)(signed char)temp_red) >> 5;
                d[i + 0] = (unsigned char)(temp_red & 0xff);
                d[i + 2] = (unsigned char)(temp_blue & 0xff);
            }
        }
    }
}

static void wapply_subtract_green(unsigned char *d, size_t pixels) {
    for (size_t i = 0; i < pixels; i++) {
        d[i * 4 + 0] = (unsigned char)(d[i * 4 + 0] + d[i * 4 + 1]);
        d[i * 4 + 2] = (unsigned char)(d[i * 4 + 2] + d[i * 4 + 1]);
    }
}

static void wapply_color_indexing(unsigned char *d, unsigned width, unsigned height,
        unsigned table_size, const unsigned char *table) {
    if (table_size > 16) {
        unsigned char colors[256][4];
        memset(colors, 0, sizeof(colors));
        for (unsigned i = 0; i < table_size && i < 256; i++)
            memcpy(colors[i], table + (size_t)i * 4, 4);
        size_t pixels = (size_t)width * height;
        for (size_t i = 0; i < pixels; i++) memcpy(d + i * 4, colors[d[i * 4 + 1]], 4);
        return;
    }
    unsigned w_bits = table_size <= 2 ? 3 : table_size <= 4 ? 2 : 1;
    unsigned pixels_per_byte = 1u << w_bits;
    unsigned bits_per_entry = 8 / pixels_per_byte;
    unsigned mask = (1u << bits_per_entry) - 1;
    unsigned char lookup[256][4 * 8];
    memset(lookup, 0, sizeof(lookup));
    for (unsigned value = 0; value < 256; value++) {
        for (unsigned sub = 0; sub < pixels_per_byte; sub++) {
            unsigned shift = sub * bits_per_entry;
            unsigned k = (value >> shift) & mask;
            if (k < table_size) memcpy(lookup[value] + sub * 4, table + (size_t)k * 4, 4);
        }
    }
    unsigned packed_blocks = wsubsample(width, w_bits);
    if (!width || !height || !packed_blocks) return;
    size_t full_blocks = packed_blocks - 1;
    size_t block_bytes = (size_t)4 * pixels_per_byte;
    size_t final_bytes = (size_t)width * 4 - full_blocks * block_bytes;
    size_t packed_stride = (size_t)packed_blocks * 4;
    size_t out_stride = (size_t)width * 4;
    unsigned char *indices = malloc(packed_blocks);
    if (!indices) return;
    for (unsigned y_rev = 0; y_rev < height; y_rev++) {
        unsigned y = height - 1 - y_rev;
        const unsigned char *packed = d + (size_t)y * packed_stride;
        for (unsigned b = 0; b < packed_blocks; b++) indices[b] = packed[b * 4 + 1];
        unsigned char *out = d + (size_t)y * out_stride;
        for (size_t b = 0; b < full_blocks; b++) memcpy(out + b * block_bytes, lookup[indices[b]], block_bytes);
        memcpy(out + full_blocks * block_bytes, lookup[indices[packed_blocks - 1]], final_bytes);
    }
    free(indices);
}

static int wread_color_cache(struct wstate *s, int *bits) {
    int ok = !s->error;
    unsigned present = wread_bits(&s->r, 1, &ok);
    if (!ok) { s->error = 1; return 0; }
    if (!present) { *bits = -1; return 1; }
    unsigned code_bits = wread_bits(&s->r, 4, &ok);
    if (!ok || code_bits < 1 || code_bits > 11) { s->error = 1; return 0; }
    *bits = (int)code_bits;
    return 1;
}

static int wread_transforms(struct wstate *s, unsigned *xsize_out) {
    unsigned xsize = s->width;
    int ok = 1;
    for (;;) {
        unsigned present = wread_bits(&s->r, 1, &ok);
        s->error = !ok;
        if (s->error || !present) break;
        unsigned type = wread_bits(&s->r, 2, &ok);
        s->error = !ok;
        if (s->error) return 0;
        if (s->transform_seen[type]) { s->error = 1; return 0; }
        s->transform_seen[type] = 1;
        s->transform_order[s->transform_count++] = (int)type;
        switch (type) {
        case 0:
        case 1: {
            unsigned bits = wread_bits(&s->r, 3, &ok) + 2;
            s->error = !ok;
            if (s->error) return 0;
            unsigned bx = wsubsample(xsize, bits);
            unsigned by = wsubsample(s->height, bits);
            unsigned char *data = calloc((size_t)bx * by * 4 + 4, 1);
            if (!data) { s->error = 1; return 0; }
            if (!wdecode_image_stream(s, bx, by, 0, data)) { free(data); return 0; }
            s->transform_kind[type] = (int)type;
            s->transform_size_bits[type] = bits;
            s->transform_data[type] = data;
            break;
        }
        case 2:
            s->transform_kind[2] = 2;
            break;
        case 3: {
            unsigned table_size = wread_bits(&s->r, 8, &ok) + 1;
            s->error = !ok;
            if (s->error) return 0;
            unsigned char *map = calloc((size_t)table_size * 4, 1);
            if (!map) { s->error = 1; return 0; }
            if (!wdecode_image_stream(s, table_size, 1, 0, map)) { free(map); return 0; }
            unsigned bits = table_size <= 2 ? 3 : table_size <= 4 ? 2 : table_size <= 16 ? 1 : 0;
            xsize = wsubsample(xsize, bits);
            for (size_t i = 4; i < (size_t)table_size * 4; i++)
                map[i] = (unsigned char)(map[i] + map[i - 4]);
            s->transform_kind[3] = 3;
            s->transform_table_size[3] = table_size;
            s->transform_data[3] = map;
            break;
        }
        default:
            s->error = 1;
            return 0;
        }
    }
    *xsize_out = xsize;
    return !s->error;
}

static int wread_huffman_code_lengths(struct wstate *s, const unsigned short *code_lengths,
        unsigned num_symbols, unsigned short *out) {
    struct whuff table;
    if (!whuff_build_implicit(&table, code_lengths, WCODE_LEN_CODES)) { s->error = 1; return 0; }
    int ok = 1;
    unsigned max_symbol = num_symbols;
    if (wread_bits(&s->r, 1, &ok)) {
        unsigned length_nbits = 2 + 2 * wread_bits(&s->r, 3, &ok);
        unsigned max_minus_two = wread_bits(&s->r, (int)length_nbits, &ok);
        if (!ok || max_minus_two > num_symbols - 2) { whuff_free(&table); s->error = 1; return 0; }
        max_symbol = 2 + max_minus_two;
    }
    memset(out, 0, sizeof(unsigned short) * num_symbols);
    unsigned prev_code_len = 8;
    unsigned symbol = 0;
    while (symbol < num_symbols) {
        if (!max_symbol) break;
        max_symbol--;
        wfill(&s->r);
        int code_len = whuff_read(&table, &s->r);
        if (code_len < 0) { whuff_free(&table); s->error = 1; return 0; }
        if (code_len < 16) {
            out[symbol++] = (unsigned short)code_len;
            if (code_len) prev_code_len = (unsigned)code_len;
        } else {
            int use_prev = code_len == 16;
            int slot = code_len - 16;
            int extra_bits, repeat_offset;
            if (slot == 0) { extra_bits = 2; repeat_offset = 3; }
            else if (slot == 1) { extra_bits = 3; repeat_offset = 3; }
            else if (slot == 2) { extra_bits = 7; repeat_offset = 11; }
            else { whuff_free(&table); s->error = 1; return 0; }
            unsigned repeat = wread_bits(&s->r, extra_bits, &ok) + (unsigned)repeat_offset;
            if (!ok || symbol + repeat > num_symbols) { whuff_free(&table); s->error = 1; return 0; }
            unsigned length = use_prev ? prev_code_len : 0;
            while (repeat > 0) {
                repeat--;
                out[symbol++] = (unsigned short)length;
            }
        }
    }
    whuff_free(&table);
    return 1;
}

static int wread_huffman_code(struct wstate *s, unsigned alphabet, struct whuff *out) {
    int ok = 1;
    if (wread_bits(&s->r, 1, &ok)) {
        unsigned num_symbols = wread_bits(&s->r, 1, &ok) + 1;
        unsigned is_first_8bits = wread_bits(&s->r, 1, &ok);
        unsigned zero_symbol = wread_bits(&s->r, (int)(1 + 7 * is_first_8bits), &ok);
        if (!ok || zero_symbol >= alphabet) { s->error = 1; return 0; }
        if (num_symbols == 1) return whuff_build_single(out, zero_symbol);
        unsigned one_symbol = wread_bits(&s->r, 8, &ok);
        if (!ok || one_symbol >= alphabet) { s->error = 1; return 0; }
        return whuff_build_two(out, zero_symbol, one_symbol);
    }
    if (!ok) { s->error = 1; return 0; }
    unsigned short code_length_code_lengths[WCODE_LEN_CODES];
    memset(code_length_code_lengths, 0, sizeof(code_length_code_lengths));
    unsigned num_code_lengths = 4 + wread_bits(&s->r, 4, &ok);
    if (!ok) { s->error = 1; return 0; }
    for (unsigned i = 0; i < num_code_lengths; i++) {
        unsigned v = wread_bits(&s->r, 3, &ok);
        if (!ok) { s->error = 1; return 0; }
        code_length_code_lengths[wcode_len_order[i]] = (unsigned short)v;
    }
    unsigned short *lengths = calloc(alphabet ? alphabet : 1, sizeof(unsigned short));
    if (!lengths) { s->error = 1; return 0; }
    if (!wread_huffman_code_lengths(s, code_length_code_lengths, alphabet, lengths)) {
        free(lengths);
        return 0;
    }
    int built = whuff_build_implicit(out, lengths, alphabet);
    free(lengths);
    if (!built) { s->error = 1; return 0; }
    return 1;
}

static int wread_huffman_codes(struct wstate *s, int read_meta, unsigned xsize, unsigned ysize,
        int cache_bits, unsigned char **entropy, unsigned *entropy_width, unsigned *huff_bits,
        unsigned *huff_mask, struct whuff **groups, unsigned *group_count) {
    unsigned num_groups = 1;
    unsigned bits = 0;
    unsigned hxsize = 1, hysize = 1;
    unsigned char *meta = NULL;
    int ok = 1;
    if (read_meta && wread_bits(&s->r, 1, &ok)) {
        bits = wread_bits(&s->r, 3, &ok) + 2;
        if (!ok) { s->error = 1; return 0; }
        hxsize = wsubsample(xsize, bits);
        hysize = wsubsample(ysize, bits);
        meta = calloc((size_t)hxsize * hysize * 4 + 4, 1);
        if (!meta) { s->error = 1; return 0; }
        if (!wdecode_image_stream(s, hxsize, hysize, 0, meta)) { free(meta); return 0; }
        for (size_t i = 0; i < (size_t)hxsize * hysize; i++) {
            unsigned code = ((unsigned)meta[i * 4] << 8) | meta[i * 4 + 1];
            if (code >= num_groups) num_groups = code + 1;
        }
    } else if (!ok) {
        s->error = 1;
        return 0;
    }
    if (num_groups > 4096) { free(meta); s->error = 1; return 0; }
    struct whuff *table = calloc((size_t)num_groups * WHUFF_CODES, sizeof(struct whuff));
    if (!table) { free(meta); s->error = 1; return 0; }
    for (unsigned g = 0; g < num_groups; g++) {
        for (int j = 0; j < WHUFF_CODES; j++) {
            unsigned alphabet = j == 0 ? 256u + 24u : j == 4 ? 40u : 256u;
            if (j == 0 && cache_bits > 0) alphabet += 1u << cache_bits;
            if (!wread_huffman_code(s, alphabet, &table[(size_t)g * WHUFF_CODES + j])) {
                for (unsigned q = 0; q < num_groups; q++)
                    for (int k = 0; k < WHUFF_CODES; k++)
                        whuff_free(&table[(size_t)q * WHUFF_CODES + k]);
                free(table);
                free(meta);
                return 0;
            }
        }
    }
    *entropy = meta;
    *entropy_width = hxsize;
    *huff_bits = bits;
    *huff_mask = bits ? ((1u << bits) - 1) : 0xffffffffu;
    *groups = table;
    *group_count = num_groups;
    return 1;
}

static unsigned wget_copy_distance(struct wreader *r, unsigned prefix, int *ok) {
    if (prefix < 4) return prefix + 1;
    unsigned extra_bits = (prefix - 2) >> 1;
    unsigned offset = (2 + (prefix & 1)) << extra_bits;
    unsigned bits = (unsigned)wpeek(r, (int)extra_bits);
    if (!wconsume(r, (int)extra_bits)) { *ok = 0; return 0; }
    return offset + bits + 1;
}

static unsigned wplane_code_to_distance(unsigned xsize, unsigned plane_code) {
    if (plane_code > 120) return plane_code - 120;
    int xoffset = wdistance_map[plane_code - 1][0];
    int yoffset = wdistance_map[plane_code - 1][1];
    long dist = (long)xoffset + (long)yoffset * (long)xsize;
    if (dist < 1) return 1;
    return (unsigned)dist;
}

static int wdecode_image_data(struct wstate *s, unsigned width, unsigned height,
        unsigned char *entropy, unsigned entropy_width, unsigned bits, unsigned mask,
        struct whuff *groups, unsigned group_count, int cache_bits, unsigned char *cache,
        unsigned char *data) {
    size_t num_values = (size_t)width * height;
    unsigned group_index = bits ? (((unsigned)entropy[0] << 8) | entropy[1]) : 0;
    struct whuff *tree = groups + (size_t)group_index * WHUFF_CODES;
    size_t index = 0;
    size_t next_block_start = 0;
    int ok = 1;
    while (index < num_values) {
        wfill(&s->r);
        if (index >= next_block_start) {
            unsigned x = (unsigned)(index % width);
            unsigned y = (unsigned)(index / width);
            next_block_start = (size_t)((x | mask) < (unsigned)(width - 1) ? (x | mask) : (width - 1)) +
                (size_t)y * width + 1;
            if (bits) {
                size_t position = (size_t)(y >> bits) * entropy_width + (x >> bits);
                group_index = ((unsigned)entropy[position * 4] << 8) | entropy[position * 4 + 1];
            } else {
                group_index = 0;
            }
            if (group_index >= group_count) { s->error = 1; return 0; }
            tree = groups + (size_t)group_index * WHUFF_CODES;
            int all_single = 1;
            for (int i = 0; i < 4; i++) if (!tree[i].single) all_single = 0;
            if (all_single) {
                int code = whuff_read(&tree[0], &s->r);
                if (code < 0) { s->error = 1; return 0; }
                if (code < 256) {
                    size_t n = bits == 0 ? num_values : next_block_start - index;
                    int red = whuff_read(&tree[1], &s->r);
                    int blue = whuff_read(&tree[2], &s->r);
                    int alpha = whuff_read(&tree[3], &s->r);
                    if (red < 0 || blue < 0 || alpha < 0) { s->error = 1; return 0; }
                    unsigned char value[4] = { (unsigned char)red, (unsigned char)code,
                        (unsigned char)blue, (unsigned char)alpha };
                    for (size_t i = 0; i < n; i++) memcpy(data + (index + i) * 4, value, 4);
                    if (cache_bits > 0) {
                        unsigned color = ((unsigned)value[0] << 16) | ((unsigned)value[1] << 8) |
                            value[2] | ((unsigned)value[3] << 24);
                        unsigned slot = (unsigned)((0x1e35a7bdu * color) >> (32 - cache_bits));
                        memcpy(cache + (size_t)slot * 4, value, 4);
                    }
                    index += n;
                    continue;
                }
            }
        }
        int code = whuff_read(&tree[0], &s->r);
        if (code < 0) { s->error = 1; return 0; }
        if (code < 256) {
            unsigned char green = (unsigned char)code;
            int red = whuff_read(&tree[1], &s->r);
            int blue = whuff_read(&tree[2], &s->r);
            if (red < 0 || blue < 0) { s->error = 1; return 0; }
            if (s->r.nbits < 15) wfill(&s->r);
            int alpha = whuff_read(&tree[3], &s->r);
            if (alpha < 0) { s->error = 1; return 0; }
            data[index * 4 + 0] = (unsigned char)red;
            data[index * 4 + 1] = green;
            data[index * 4 + 2] = (unsigned char)blue;
            data[index * 4 + 3] = (unsigned char)alpha;
            if (cache_bits > 0) {
                unsigned color = ((unsigned)red << 16) | ((unsigned)green << 8) |
                    (unsigned)blue | ((unsigned)alpha << 24);
                unsigned slot = (unsigned)((0x1e35a7bdu * color) >> (32 - cache_bits));
                data[index * 4 + 0] = (unsigned char)red;
                memcpy(cache + (size_t)slot * 4, data + index * 4, 4);
            }
            index++;
        } else if (code < 256 + 24) {
            unsigned length_symbol = (unsigned)(code - 256);
            unsigned length = wget_copy_distance(&s->r, length_symbol, &ok);
            if (!ok) { s->error = 1; return 0; }
            int dist_symbol = whuff_read(&tree[4], &s->r);
            if (dist_symbol < 0) { s->error = 1; return 0; }
            unsigned dist_code = wget_copy_distance(&s->r, (unsigned)dist_symbol, &ok);
            if (!ok) { s->error = 1; return 0; }
            size_t dist = (size_t)wplane_code_to_distance(width, dist_code);
            if (index < dist || num_values - index < length) { s->error = 1; return 0; }
            if (dist == 1) {
                unsigned char value[4];
                memcpy(value, data + (index - dist) * 4, 4);
                for (size_t i = 0; i < length; i++) memcpy(data + (index + i) * 4, value, 4);
            } else {
                size_t bytes = (size_t)length * 4;
                for (size_t i = 0; i < bytes; i++)
                    data[index * 4 + i] = data[index * 4 + i - dist * 4];
            }
            if (cache_bits > 0) {
                for (size_t i = 0; i < length; i++) {
                    unsigned char *pixel = data + (index + i) * 4;
                    unsigned color = ((unsigned)pixel[0] << 16) | ((unsigned)pixel[1] << 8) |
                        pixel[2] | ((unsigned)pixel[3] << 24);
                    unsigned slot = (unsigned)((0x1e35a7bdu * color) >> (32 - cache_bits));
                    memcpy(cache + (size_t)slot * 4, pixel, 4);
                }
            }
            index += length;
        } else {
            if (cache_bits <= 0) { s->error = 1; return 0; }
            unsigned slot = (unsigned)(code - 280);
            memcpy(data + index * 4, cache + (size_t)slot * 4, 4);
            index++;
            if (index < next_block_start) {
                int pbits, psym;
                if (whuff_peek(&tree[0], &s->r, &pbits, &psym) && psym >= 280) {
                    if (!wconsume(&s->r, pbits)) { s->error = 1; return 0; }
                    memcpy(data + index * 4, cache + (size_t)(psym - 280) * 4, 4);
                    index++;
                }
            }
        }
    }
    return 1;
}

static int wdecode_image_stream(struct wstate *s, unsigned xsize, unsigned ysize, int is_argb,
        unsigned char *data) {
    int cache_bits = -1;
    unsigned char *cache = NULL;
    if (!wread_color_cache(s, &cache_bits)) return 0;
    if (cache_bits > 0) {
        cache = calloc((size_t)1 << cache_bits, 4);
        if (!cache) { s->error = 1; return 0; }
    }
    unsigned char *entropy = NULL;
    unsigned entropy_width = 1, bits = 0, mask = 0, group_count = 0;
    struct whuff *groups = NULL;
    if (!wread_huffman_codes(s, is_argb, xsize, ysize, cache_bits, &entropy, &entropy_width,
            &bits, &mask, &groups, &group_count)) {
        free(cache);
        return 0;
    }
    int ok = wdecode_image_data(s, xsize, ysize, entropy, entropy_width, bits, mask, groups,
        group_count, cache_bits, cache, data);
    for (unsigned g = 0; g < group_count; g++)
        for (int j = 0; j < WHUFF_CODES; j++) whuff_free(&groups[(size_t)g * WHUFF_CODES + j]);
    free(groups);
    free(entropy);
    free(cache);
    return ok && !s->error;
}

enum webp_status tomoe_webp_decode(const unsigned char *data, size_t length,
        struct webp_decoded *image) {
    image->width = image->height = 0;
    image->bytes = 0;
    image->pixels = NULL;
    if (!data || length < 21) return WEBP_DECODE_ERROR;
    if (memcmp(data, "RIFF", 4) || memcmp(data + 8, "WEBP", 4)) return WEBP_DECODE_ERROR;
    size_t offset = 12;
    while (offset + 8 <= length) {
        unsigned chunk_length = (unsigned)data[offset + 4] | ((unsigned)data[offset + 5] << 8) |
            ((unsigned)data[offset + 6] << 16) | ((unsigned)data[offset + 7] << 24);
        if (!memcmp(data + offset, "VP8L", 4)) {
            if (offset + 8 + chunk_length > length || chunk_length < 5) return WEBP_DECODE_ERROR;
            const unsigned char *payload = data + offset + 8;
            if (payload[0] != 0x2f) return WEBP_DECODE_ERROR;
            unsigned header = (unsigned)payload[1] | ((unsigned)payload[2] << 8) |
                ((unsigned)payload[3] << 16) | ((unsigned)payload[4] << 24);
            if (header >> 29) return WEBP_DECODE_ERROR;
            unsigned width = (1 + header) & 0x3fff;
            unsigned height = (1 + (header >> 14)) & 0x3fff;
            int has_alpha = (int)((header >> 28) & 1);
            if (!width || !height || width > 16384 || height > 16384) return WEBP_DECODE_ERROR;
            size_t pixels = (size_t)width * height;
            if (pixels > 268435456 / 4) return WEBP_DECODE_ERROR;
            unsigned char *rgba = calloc(pixels, 4);
            if (!rgba) return WEBP_DECODE_ERROR;
            struct wstate s;
            memset(&s, 0, sizeof(s));
            s.r.data = payload;
            s.r.length = chunk_length;
            s.r.pos = 0;
            int ok = 1;
            unsigned signature = wread_bits(&s.r, 8, &ok);
            unsigned hw = wread_bits(&s.r, 14, &ok) + 1;
            unsigned hh = wread_bits(&s.r, 14, &ok) + 1;
            unsigned alpha_used = wread_bits(&s.r, 1, &ok);
            unsigned version = wread_bits(&s.r, 3, &ok);
            (void)alpha_used;
            if (!ok || signature != 0x2f || version) {
                free(rgba);
                return WEBP_DECODE_ERROR;
            }
            s.width = (unsigned)hw;
            s.height = (unsigned)hh;
            unsigned transformed_width = 0;
            if (!wread_transforms(&s, &transformed_width)) {
                for (int i = 0; i < 4; i++) free(s.transform_data[i]);
                free(rgba);
                return WEBP_DECODE_ERROR;
            }
            size_t transformed_size = (size_t)transformed_width * s.height * 4;
            if (transformed_size > pixels * 4) {
                for (int i = 0; i < 4; i++) free(s.transform_data[i]);
                free(rgba);
                return WEBP_DECODE_ERROR;
            }
            if (!wdecode_image_stream(&s, transformed_width, s.height, 1, rgba)) {
                for (int i = 0; i < 4; i++) free(s.transform_data[i]);
                free(rgba);
                return WEBP_DECODE_ERROR;
            }
            unsigned image_width = transformed_width;
            for (int i = s.transform_count - 1; i >= 0; i--) {
                int type = s.transform_order[i];
                if (type == 0) {
                    wapply_predictor(rgba, image_width, s.height, s.transform_size_bits[0],
                        s.transform_data[0]);
                } else if (type == 1) {
                    wapply_color_rows(rgba, image_width, s.height, s.transform_size_bits[1],
                        s.transform_data[1]);
                } else if (type == 2) {
                    wapply_subtract_green(rgba, (size_t)image_width * s.height);
                } else {
                    image_width = s.width;
                    wapply_color_indexing(rgba, image_width, s.height,
                        s.transform_table_size[3], s.transform_data[3]);
                }
            }
            for (int i = 0; i < 4; i++) free(s.transform_data[i]);
            image->width = width;
            image->height = height;
            image->bytes = pixels * 4;
            image->pixels = rgba;
            image->has_alpha = has_alpha;
            return WEBP_DECODE_OK;
        }
        offset += 8 + chunk_length + (chunk_length & 1);
    }
    return WEBP_DECODE_ERROR;
}
