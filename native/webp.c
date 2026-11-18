/* SPDX-License-Identifier: AGPL-3.0-only AND MIT
   The WebP decoders are ported from image-webp 0.2.4 (MIT OR
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

static const unsigned char vcoeff_update_probs[4][8][3][11] = {
  {
    {
      { 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255 },
      { 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255 },
      { 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255 },
    },
    {
      { 176, 246, 255, 255, 255, 255, 255, 255, 255, 255, 255 },
      { 223, 241, 252, 255, 255, 255, 255, 255, 255, 255, 255 },
      { 249, 253, 253, 255, 255, 255, 255, 255, 255, 255, 255 },
    },
    {
      { 255, 244, 252, 255, 255, 255, 255, 255, 255, 255, 255 },
      { 234, 254, 254, 255, 255, 255, 255, 255, 255, 255, 255 },
      { 253, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255 },
    },
    {
      { 255, 246, 254, 255, 255, 255, 255, 255, 255, 255, 255 },
      { 239, 253, 254, 255, 255, 255, 255, 255, 255, 255, 255 },
      { 254, 255, 254, 255, 255, 255, 255, 255, 255, 255, 255 },
    },
    {
      { 255, 248, 254, 255, 255, 255, 255, 255, 255, 255, 255 },
      { 251, 255, 254, 255, 255, 255, 255, 255, 255, 255, 255 },
      { 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255 },
    },
    {
      { 255, 253, 254, 255, 255, 255, 255, 255, 255, 255, 255 },
      { 251, 254, 254, 255, 255, 255, 255, 255, 255, 255, 255 },
      { 254, 255, 254, 255, 255, 255, 255, 255, 255, 255, 255 },
    },
    {
      { 255, 254, 253, 255, 254, 255, 255, 255, 255, 255, 255 },
      { 250, 255, 254, 255, 254, 255, 255, 255, 255, 255, 255 },
      { 254, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255 },
    },
    {
      { 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255 },
      { 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255 },
      { 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255 },
    },
  },
  {
    {
      { 217, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255 },
      { 225, 252, 241, 253, 255, 255, 254, 255, 255, 255, 255 },
      { 234, 250, 241, 250, 253, 255, 253, 254, 255, 255, 255 },
    },
    {
      { 255, 254, 255, 255, 255, 255, 255, 255, 255, 255, 255 },
      { 223, 254, 254, 255, 255, 255, 255, 255, 255, 255, 255 },
      { 238, 253, 254, 254, 255, 255, 255, 255, 255, 255, 255 },
    },
    {
      { 255, 248, 254, 255, 255, 255, 255, 255, 255, 255, 255 },
      { 249, 254, 255, 255, 255, 255, 255, 255, 255, 255, 255 },
      { 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255 },
    },
    {
      { 255, 253, 255, 255, 255, 255, 255, 255, 255, 255, 255 },
      { 247, 254, 255, 255, 255, 255, 255, 255, 255, 255, 255 },
      { 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255 },
    },
    {
      { 255, 253, 254, 255, 255, 255, 255, 255, 255, 255, 255 },
      { 252, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255 },
      { 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255 },
    },
    {
      { 255, 254, 254, 255, 255, 255, 255, 255, 255, 255, 255 },
      { 253, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255 },
      { 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255 },
    },
    {
      { 255, 254, 253, 255, 255, 255, 255, 255, 255, 255, 255 },
      { 250, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255 },
      { 254, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255 },
    },
    {
      { 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255 },
      { 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255 },
      { 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255 },
    },
  },
  {
    {
      { 186, 251, 250, 255, 255, 255, 255, 255, 255, 255, 255 },
      { 234, 251, 244, 254, 255, 255, 255, 255, 255, 255, 255 },
      { 251, 251, 243, 253, 254, 255, 254, 255, 255, 255, 255 },
    },
    {
      { 255, 253, 254, 255, 255, 255, 255, 255, 255, 255, 255 },
      { 236, 253, 254, 255, 255, 255, 255, 255, 255, 255, 255 },
      { 251, 253, 253, 254, 254, 255, 255, 255, 255, 255, 255 },
    },
    {
      { 255, 254, 254, 255, 255, 255, 255, 255, 255, 255, 255 },
      { 254, 254, 254, 255, 255, 255, 255, 255, 255, 255, 255 },
      { 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255 },
    },
    {
      { 255, 254, 255, 255, 255, 255, 255, 255, 255, 255, 255 },
      { 254, 254, 255, 255, 255, 255, 255, 255, 255, 255, 255 },
      { 254, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255 },
    },
    {
      { 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255 },
      { 254, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255 },
      { 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255 },
    },
    {
      { 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255 },
      { 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255 },
      { 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255 },
    },
    {
      { 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255 },
      { 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255 },
      { 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255 },
    },
    {
      { 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255 },
      { 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255 },
      { 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255 },
    },
  },
  {
    {
      { 248, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255 },
      { 250, 254, 252, 254, 255, 255, 255, 255, 255, 255, 255 },
      { 248, 254, 249, 253, 255, 255, 255, 255, 255, 255, 255 },
    },
    {
      { 255, 253, 253, 255, 255, 255, 255, 255, 255, 255, 255 },
      { 246, 253, 253, 255, 255, 255, 255, 255, 255, 255, 255 },
      { 252, 254, 251, 254, 254, 255, 255, 255, 255, 255, 255 },
    },
    {
      { 255, 254, 252, 255, 255, 255, 255, 255, 255, 255, 255 },
      { 248, 254, 253, 255, 255, 255, 255, 255, 255, 255, 255 },
      { 253, 255, 254, 254, 255, 255, 255, 255, 255, 255, 255 },
    },
    {
      { 255, 251, 254, 255, 255, 255, 255, 255, 255, 255, 255 },
      { 245, 251, 254, 255, 255, 255, 255, 255, 255, 255, 255 },
      { 253, 253, 254, 255, 255, 255, 255, 255, 255, 255, 255 },
    },
    {
      { 255, 251, 253, 255, 255, 255, 255, 255, 255, 255, 255 },
      { 252, 253, 254, 255, 255, 255, 255, 255, 255, 255, 255 },
      { 255, 254, 255, 255, 255, 255, 255, 255, 255, 255, 255 },
    },
    {
      { 255, 252, 255, 255, 255, 255, 255, 255, 255, 255, 255 },
      { 249, 255, 254, 255, 255, 255, 255, 255, 255, 255, 255 },
      { 255, 255, 254, 255, 255, 255, 255, 255, 255, 255, 255 },
    },
    {
      { 255, 255, 253, 255, 255, 255, 255, 255, 255, 255, 255 },
      { 250, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255 },
      { 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255 },
    },
    {
      { 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255 },
      { 254, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255 },
      { 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255 },
    },
  },
};

static const unsigned char vcoeff_probs[4][8][3][11] = {
  {
    {
      { 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128 },
      { 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128 },
      { 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128 },
    },
    {
      { 253, 136, 254, 255, 228, 219, 128, 128, 128, 128, 128 },
      { 189, 129, 242, 255, 227, 213, 255, 219, 128, 128, 128 },
      { 106, 126, 227, 252, 214, 209, 255, 255, 128, 128, 128 },
    },
    {
      { 1, 98, 248, 255, 236, 226, 255, 255, 128, 128, 128 },
      { 181, 133, 238, 254, 221, 234, 255, 154, 128, 128, 128 },
      { 78, 134, 202, 247, 198, 180, 255, 219, 128, 128, 128 },
    },
    {
      { 1, 185, 249, 255, 243, 255, 128, 128, 128, 128, 128 },
      { 184, 150, 247, 255, 236, 224, 128, 128, 128, 128, 128 },
      { 77, 110, 216, 255, 236, 230, 128, 128, 128, 128, 128 },
    },
    {
      { 1, 101, 251, 255, 241, 255, 128, 128, 128, 128, 128 },
      { 170, 139, 241, 252, 236, 209, 255, 255, 128, 128, 128 },
      { 37, 116, 196, 243, 228, 255, 255, 255, 128, 128, 128 },
    },
    {
      { 1, 204, 254, 255, 245, 255, 128, 128, 128, 128, 128 },
      { 207, 160, 250, 255, 238, 128, 128, 128, 128, 128, 128 },
      { 102, 103, 231, 255, 211, 171, 128, 128, 128, 128, 128 },
    },
    {
      { 1, 152, 252, 255, 240, 255, 128, 128, 128, 128, 128 },
      { 177, 135, 243, 255, 234, 225, 128, 128, 128, 128, 128 },
      { 80, 129, 211, 255, 194, 224, 128, 128, 128, 128, 128 },
    },
    {
      { 1, 1, 255, 128, 128, 128, 128, 128, 128, 128, 128 },
      { 246, 1, 255, 128, 128, 128, 128, 128, 128, 128, 128 },
      { 255, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128 },
    },
  },
  {
    {
      { 198, 35, 237, 223, 193, 187, 162, 160, 145, 155, 62 },
      { 131, 45, 198, 221, 172, 176, 220, 157, 252, 221, 1 },
      { 68, 47, 146, 208, 149, 167, 221, 162, 255, 223, 128 },
    },
    {
      { 1, 149, 241, 255, 221, 224, 255, 255, 128, 128, 128 },
      { 184, 141, 234, 253, 222, 220, 255, 199, 128, 128, 128 },
      { 81, 99, 181, 242, 176, 190, 249, 202, 255, 255, 128 },
    },
    {
      { 1, 129, 232, 253, 214, 197, 242, 196, 255, 255, 128 },
      { 99, 121, 210, 250, 201, 198, 255, 202, 128, 128, 128 },
      { 23, 91, 163, 242, 170, 187, 247, 210, 255, 255, 128 },
    },
    {
      { 1, 200, 246, 255, 234, 255, 128, 128, 128, 128, 128 },
      { 109, 178, 241, 255, 231, 245, 255, 255, 128, 128, 128 },
      { 44, 130, 201, 253, 205, 192, 255, 255, 128, 128, 128 },
    },
    {
      { 1, 132, 239, 251, 219, 209, 255, 165, 128, 128, 128 },
      { 94, 136, 225, 251, 218, 190, 255, 255, 128, 128, 128 },
      { 22, 100, 174, 245, 186, 161, 255, 199, 128, 128, 128 },
    },
    {
      { 1, 182, 249, 255, 232, 235, 128, 128, 128, 128, 128 },
      { 124, 143, 241, 255, 227, 234, 128, 128, 128, 128, 128 },
      { 35, 77, 181, 251, 193, 211, 255, 205, 128, 128, 128 },
    },
    {
      { 1, 157, 247, 255, 236, 231, 255, 255, 128, 128, 128 },
      { 121, 141, 235, 255, 225, 227, 255, 255, 128, 128, 128 },
      { 45, 99, 188, 251, 195, 217, 255, 224, 128, 128, 128 },
    },
    {
      { 1, 1, 251, 255, 213, 255, 128, 128, 128, 128, 128 },
      { 203, 1, 248, 255, 255, 128, 128, 128, 128, 128, 128 },
      { 137, 1, 177, 255, 224, 255, 128, 128, 128, 128, 128 },
    },
  },
  {
    {
      { 253, 9, 248, 251, 207, 208, 255, 192, 128, 128, 128 },
      { 175, 13, 224, 243, 193, 185, 249, 198, 255, 255, 128 },
      { 73, 17, 171, 221, 161, 179, 236, 167, 255, 234, 128 },
    },
    {
      { 1, 95, 247, 253, 212, 183, 255, 255, 128, 128, 128 },
      { 239, 90, 244, 250, 211, 209, 255, 255, 128, 128, 128 },
      { 155, 77, 195, 248, 188, 195, 255, 255, 128, 128, 128 },
    },
    {
      { 1, 24, 239, 251, 218, 219, 255, 205, 128, 128, 128 },
      { 201, 51, 219, 255, 196, 186, 128, 128, 128, 128, 128 },
      { 69, 46, 190, 239, 201, 218, 255, 228, 128, 128, 128 },
    },
    {
      { 1, 191, 251, 255, 255, 128, 128, 128, 128, 128, 128 },
      { 223, 165, 249, 255, 213, 255, 128, 128, 128, 128, 128 },
      { 141, 124, 248, 255, 255, 128, 128, 128, 128, 128, 128 },
    },
    {
      { 1, 16, 248, 255, 255, 128, 128, 128, 128, 128, 128 },
      { 190, 36, 230, 255, 236, 255, 128, 128, 128, 128, 128 },
      { 149, 1, 255, 128, 128, 128, 128, 128, 128, 128, 128 },
    },
    {
      { 1, 226, 255, 128, 128, 128, 128, 128, 128, 128, 128 },
      { 247, 192, 255, 128, 128, 128, 128, 128, 128, 128, 128 },
      { 240, 128, 255, 128, 128, 128, 128, 128, 128, 128, 128 },
    },
    {
      { 1, 134, 252, 255, 255, 128, 128, 128, 128, 128, 128 },
      { 213, 62, 250, 255, 255, 128, 128, 128, 128, 128, 128 },
      { 55, 93, 255, 128, 128, 128, 128, 128, 128, 128, 128 },
    },
    {
      { 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128 },
      { 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128 },
      { 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128 },
    },
  },
  {
    {
      { 202, 24, 213, 235, 186, 191, 220, 160, 240, 175, 255 },
      { 126, 38, 182, 232, 169, 184, 228, 174, 255, 187, 128 },
      { 61, 46, 138, 219, 151, 178, 240, 170, 255, 216, 128 },
    },
    {
      { 1, 112, 230, 250, 199, 191, 247, 159, 255, 255, 128 },
      { 166, 109, 228, 252, 211, 215, 255, 174, 128, 128, 128 },
      { 39, 77, 162, 232, 172, 180, 245, 178, 255, 255, 128 },
    },
    {
      { 1, 52, 220, 246, 198, 199, 249, 220, 255, 255, 128 },
      { 124, 74, 191, 243, 183, 193, 250, 221, 255, 255, 128 },
      { 24, 71, 130, 219, 154, 170, 243, 182, 255, 255, 128 },
    },
    {
      { 1, 182, 225, 249, 219, 240, 255, 224, 128, 128, 128 },
      { 149, 150, 226, 252, 216, 205, 255, 171, 128, 128, 128 },
      { 28, 108, 170, 242, 183, 194, 254, 223, 255, 255, 128 },
    },
    {
      { 1, 81, 230, 252, 204, 203, 255, 192, 128, 128, 128 },
      { 123, 102, 209, 247, 188, 196, 255, 233, 128, 128, 128 },
      { 20, 95, 153, 243, 164, 173, 255, 203, 128, 128, 128 },
    },
    {
      { 1, 222, 248, 255, 216, 213, 128, 128, 128, 128, 128 },
      { 168, 175, 246, 252, 235, 205, 255, 255, 128, 128, 128 },
      { 47, 116, 215, 255, 211, 212, 255, 255, 128, 128, 128 },
    },
    {
      { 1, 121, 236, 253, 212, 214, 255, 255, 128, 128, 128 },
      { 141, 84, 213, 252, 201, 202, 255, 219, 128, 128, 128 },
      { 42, 80, 160, 240, 162, 185, 255, 205, 128, 128, 128 },
    },
    {
      { 1, 1, 255, 128, 128, 128, 128, 128, 128, 128, 128 },
      { 244, 1, 255, 128, 128, 128, 128, 128, 128, 128, 128 },
      { 238, 1, 255, 128, 128, 128, 128, 128, 128, 128, 128 },
    },
  },
};

static const unsigned char vbpred_probs[10][10][9] = {
  {
    { 231, 120, 48, 89, 115, 113, 120, 152, 112 },
    { 152, 179, 64, 126, 170, 118, 46, 70, 95 },
    { 175, 69, 143, 80, 85, 82, 72, 155, 103 },
    { 56, 58, 10, 171, 218, 189, 17, 13, 152 },
    { 144, 71, 10, 38, 171, 213, 144, 34, 26 },
    { 114, 26, 17, 163, 44, 195, 21, 10, 173 },
    { 121, 24, 80, 195, 26, 62, 44, 64, 85 },
    { 170, 46, 55, 19, 136, 160, 33, 206, 71 },
    { 63, 20, 8, 114, 114, 208, 12, 9, 226 },
    { 81, 40, 11, 96, 182, 84, 29, 16, 36 },
  },
  {
    { 134, 183, 89, 137, 98, 101, 106, 165, 148 },
    { 72, 187, 100, 130, 157, 111, 32, 75, 80 },
    { 66, 102, 167, 99, 74, 62, 40, 234, 128 },
    { 41, 53, 9, 178, 241, 141, 26, 8, 107 },
    { 104, 79, 12, 27, 217, 255, 87, 17, 7 },
    { 74, 43, 26, 146, 73, 166, 49, 23, 157 },
    { 65, 38, 105, 160, 51, 52, 31, 115, 128 },
    { 87, 68, 71, 44, 114, 51, 15, 186, 23 },
    { 47, 41, 14, 110, 182, 183, 21, 17, 194 },
    { 66, 45, 25, 102, 197, 189, 23, 18, 22 },
  },
  {
    { 88, 88, 147, 150, 42, 46, 45, 196, 205 },
    { 43, 97, 183, 117, 85, 38, 35, 179, 61 },
    { 39, 53, 200, 87, 26, 21, 43, 232, 171 },
    { 56, 34, 51, 104, 114, 102, 29, 93, 77 },
    { 107, 54, 32, 26, 51, 1, 81, 43, 31 },
    { 39, 28, 85, 171, 58, 165, 90, 98, 64 },
    { 34, 22, 116, 206, 23, 34, 43, 166, 73 },
    { 68, 25, 106, 22, 64, 171, 36, 225, 114 },
    { 34, 19, 21, 102, 132, 188, 16, 76, 124 },
    { 62, 18, 78, 95, 85, 57, 50, 48, 51 },
  },
  {
    { 193, 101, 35, 159, 215, 111, 89, 46, 111 },
    { 60, 148, 31, 172, 219, 228, 21, 18, 111 },
    { 112, 113, 77, 85, 179, 255, 38, 120, 114 },
    { 40, 42, 1, 196, 245, 209, 10, 25, 109 },
    { 100, 80, 8, 43, 154, 1, 51, 26, 71 },
    { 88, 43, 29, 140, 166, 213, 37, 43, 154 },
    { 61, 63, 30, 155, 67, 45, 68, 1, 209 },
    { 142, 78, 78, 16, 255, 128, 34, 197, 171 },
    { 41, 40, 5, 102, 211, 183, 4, 1, 221 },
    { 51, 50, 17, 168, 209, 192, 23, 25, 82 },
  },
  {
    { 125, 98, 42, 88, 104, 85, 117, 175, 82 },
    { 95, 84, 53, 89, 128, 100, 113, 101, 45 },
    { 75, 79, 123, 47, 51, 128, 81, 171, 1 },
    { 57, 17, 5, 71, 102, 57, 53, 41, 49 },
    { 115, 21, 2, 10, 102, 255, 166, 23, 6 },
    { 38, 33, 13, 121, 57, 73, 26, 1, 85 },
    { 41, 10, 67, 138, 77, 110, 90, 47, 114 },
    { 101, 29, 16, 10, 85, 128, 101, 196, 26 },
    { 57, 18, 10, 102, 102, 213, 34, 20, 43 },
    { 117, 20, 15, 36, 163, 128, 68, 1, 26 },
  },
  {
    { 138, 31, 36, 171, 27, 166, 38, 44, 229 },
    { 67, 87, 58, 169, 82, 115, 26, 59, 179 },
    { 63, 59, 90, 180, 59, 166, 93, 73, 154 },
    { 40, 40, 21, 116, 143, 209, 34, 39, 175 },
    { 57, 46, 22, 24, 128, 1, 54, 17, 37 },
    { 47, 15, 16, 183, 34, 223, 49, 45, 183 },
    { 46, 17, 33, 183, 6, 98, 15, 32, 183 },
    { 65, 32, 73, 115, 28, 128, 23, 128, 205 },
    { 40, 3, 9, 115, 51, 192, 18, 6, 223 },
    { 87, 37, 9, 115, 59, 77, 64, 21, 47 },
  },
  {
    { 104, 55, 44, 218, 9, 54, 53, 130, 226 },
    { 64, 90, 70, 205, 40, 41, 23, 26, 57 },
    { 54, 57, 112, 184, 5, 41, 38, 166, 213 },
    { 30, 34, 26, 133, 152, 116, 10, 32, 134 },
    { 75, 32, 12, 51, 192, 255, 160, 43, 51 },
    { 39, 19, 53, 221, 26, 114, 32, 73, 255 },
    { 31, 9, 65, 234, 2, 15, 1, 118, 73 },
    { 88, 31, 35, 67, 102, 85, 55, 186, 85 },
    { 56, 21, 23, 111, 59, 205, 45, 37, 192 },
    { 55, 38, 70, 124, 73, 102, 1, 34, 98 },
  },
  {
    { 102, 61, 71, 37, 34, 53, 31, 243, 192 },
    { 69, 60, 71, 38, 73, 119, 28, 222, 37 },
    { 68, 45, 128, 34, 1, 47, 11, 245, 171 },
    { 62, 17, 19, 70, 146, 85, 55, 62, 70 },
    { 75, 15, 9, 9, 64, 255, 184, 119, 16 },
    { 37, 43, 37, 154, 100, 163, 85, 160, 1 },
    { 63, 9, 92, 136, 28, 64, 32, 201, 85 },
    { 86, 6, 28, 5, 64, 255, 25, 248, 1 },
    { 56, 8, 17, 132, 137, 255, 55, 116, 128 },
    { 58, 15, 20, 82, 135, 57, 26, 121, 40 },
  },
  {
    { 164, 50, 31, 137, 154, 133, 25, 35, 218 },
    { 51, 103, 44, 131, 131, 123, 31, 6, 158 },
    { 86, 40, 64, 135, 148, 224, 45, 183, 128 },
    { 22, 26, 17, 131, 240, 154, 14, 1, 209 },
    { 83, 12, 13, 54, 192, 255, 68, 47, 28 },
    { 45, 16, 21, 91, 64, 222, 7, 1, 197 },
    { 56, 21, 39, 155, 60, 138, 23, 102, 213 },
    { 85, 26, 85, 85, 128, 128, 32, 146, 171 },
    { 18, 11, 7, 63, 144, 171, 4, 4, 246 },
    { 35, 27, 10, 146, 174, 171, 12, 26, 128 },
  },
  {
    { 190, 80, 35, 99, 180, 80, 126, 54, 45 },
    { 85, 126, 47, 87, 176, 51, 41, 20, 32 },
    { 101, 75, 128, 139, 118, 146, 116, 128, 85 },
    { 56, 41, 15, 176, 236, 85, 37, 9, 62 },
    { 146, 36, 19, 30, 171, 255, 97, 27, 20 },
    { 71, 30, 17, 119, 118, 255, 17, 18, 138 },
    { 101, 38, 60, 138, 55, 70, 43, 26, 142 },
    { 138, 45, 61, 62, 219, 1, 81, 188, 64 },
    { 32, 41, 20, 117, 151, 142, 20, 21, 163 },
    { 112, 19, 12, 61, 195, 128, 48, 4, 24 },
  },
};

static const short vdc_quant_table[128] = {
    4, 5, 6, 7, 8, 9, 10, 10,
    11, 12, 13, 14, 15, 16, 17, 17,
    18, 19, 20, 20, 21, 21, 22, 22,
    23, 23, 24, 25, 25, 26, 27, 28,
    29, 30, 31, 32, 33, 34, 35, 36,
    37, 37, 38, 39, 40, 41, 42, 43,
    44, 45, 46, 46, 47, 48, 49, 50,
    51, 52, 53, 54, 55, 56, 57, 58,
    59, 60, 61, 62, 63, 64, 65, 66,
    67, 68, 69, 70, 71, 72, 73, 74,
    75, 76, 76, 77, 78, 79, 80, 81,
    82, 83, 84, 85, 86, 87, 88, 89,
    91, 93, 95, 96, 98, 100, 101, 102,
    104, 106, 108, 110, 112, 114, 116, 118,
    122, 124, 126, 128, 130, 132, 134, 136,
    138, 140, 143, 145, 148, 151, 154, 157,
};

static const short vac_quant_table[128] = {
    4, 5, 6, 7, 8, 9, 10, 11,
    12, 13, 14, 15, 16, 17, 18, 19,
    20, 21, 22, 23, 24, 25, 26, 27,
    28, 29, 30, 31, 32, 33, 34, 35,
    36, 37, 38, 39, 40, 41, 42, 43,
    44, 45, 46, 47, 48, 49, 50, 51,
    52, 53, 54, 55, 56, 57, 58, 60,
    62, 64, 66, 68, 70, 72, 74, 76,
    78, 80, 82, 84, 86, 88, 90, 92,
    94, 96, 98, 100, 102, 104, 106, 108,
    110, 112, 114, 116, 119, 122, 125, 128,
    131, 134, 137, 140, 143, 146, 149, 152,
    155, 158, 161, 164, 167, 170, 173, 177,
    181, 185, 189, 193, 197, 201, 205, 209,
    213, 217, 221, 225, 229, 234, 239, 245,
    249, 254, 259, 264, 269, 274, 279, 284,
};

#define VSEGMENTS 4
#define VTOKEN_PROBS 11

enum { VDC_PRED = 0, VV_PRED = 1, VH_PRED = 2, VTM_PRED = 3, VBP_PRED = 4 };
enum { VBDC = 0, VBTM, VBVE, VBHE, VBLD, VBRD, VBVR, VBVL, VBHD, VBHU };

struct vbool {
    const unsigned char *data;
    size_t nchunks, chunk_index;
    unsigned long long value;
    unsigned range;
    int bit_count;
    unsigned char final_bytes[3];
    int final_remaining;
    int eof;
};

static void vb_init(struct vbool *b, const unsigned char *data, size_t length) {
    size_t full = length / 4;
    int rem = (int)(length % 4);
    b->data = data;
    b->nchunks = full;
    b->chunk_index = 0;
    b->value = 0;
    b->range = 255;
    b->bit_count = -8;
    memset(b->final_bytes, 0, 3);
    for (int i = 0; i < rem; i++) b->final_bytes[i] = data[full * 4 + i];
    b->final_remaining = rem;
    b->eof = 0;
}

static int vb_fill(struct vbool *b) {
    if (b->chunk_index < b->nchunks) {
        const unsigned char *c = b->data + 4 * b->chunk_index;
        unsigned v = ((unsigned)c[0] << 24) | ((unsigned)c[1] << 16) | ((unsigned)c[2] << 8) | c[3];
        b->chunk_index++;
        b->value = (b->value << 32) | v;
        b->bit_count += 32;
        return 1;
    }
    if (b->final_remaining > 0) {
        unsigned byte = b->final_bytes[0];
        b->final_remaining--;
        b->final_bytes[0] = b->final_bytes[1];
        b->final_bytes[1] = b->final_bytes[2];
        b->final_bytes[2] = 0;
        b->value = (b->value << 8) | byte;
        b->bit_count += 8;
        return 1;
    }
    if (b->final_remaining == 0) {
        b->final_remaining--;
        b->value <<= 8;
        b->bit_count += 8;
        return 1;
    }
    b->final_remaining = -0xe;
    b->eof = 1;
    return 0;
}

static void vb_normalize(struct vbool *b) {
    int shift = 0;
    if (b->range) {
        shift = __builtin_clz(b->range);
        shift = shift > 24 ? shift - 24 : 0;
    }
    b->range <<= shift;
    b->bit_count -= shift;
}

static int vb_bit(struct vbool *b, unsigned prob) {
    if (b->bit_count < 0 && !vb_fill(b)) return 0;
    unsigned split = 1 + (((b->range - 1) * prob) >> 8);
    unsigned long long bigsplit = (unsigned long long)split << b->bit_count;
    int ret;
    if (b->value >= bigsplit) {
        b->value -= bigsplit;
        b->range -= split;
        ret = 1;
    } else {
        b->range = split;
        ret = 0;
    }
    vb_normalize(b);
    return ret;
}

static int vb_flag(struct vbool *b) {
    if (b->bit_count < 0 && !vb_fill(b)) return 0;
    unsigned half = b->range / 2;
    unsigned split = b->range - half;
    unsigned long long bigsplit = (unsigned long long)split << b->bit_count;
    int ret;
    if (b->value >= bigsplit) {
        b->value -= bigsplit;
        b->range = half;
        ret = 1;
    } else {
        b->range = split;
        ret = 0;
    }
    vb_normalize(b);
    return ret;
}

static unsigned vb_literal(struct vbool *b, int n) {
    unsigned v = 0;
    for (int i = 0; i < n; i++) v = (v << 1) + (unsigned)vb_flag(b);
    return v & 0xff;
}

static int vb_signed(struct vbool *b, int n) {
    if (!vb_flag(b)) return 0;
    int magnitude = (int)vb_literal(b, n);
    return vb_flag(b) ? -magnitude : magnitude;
}

static int vb_tree(struct vbool *b, const signed char *tree, const unsigned char *probs, int start) {
    int i = start;
    for (;;) {
        int t = vb_bit(b, probs[i]) ? tree[2 * i + 1] : tree[2 * i];
        if (t > 0) i = t / 2;
        else return -t;
    }
}

static const signed char vsegment_tree[] = { 2, 4, 0, -1, -2, -3 };
static const signed char vymode_tree[] = { -4, 2, 4, 6, 0, -1, -2, -3 };
static const unsigned char vymode_probs[] = { 145, 156, 163, 128 };
static const signed char vuvmode_tree[] = { 0, 2, -1, 4, -2, -3 };
static const unsigned char vuvmode_probs[] = { 142, 114, 183 };
static const signed char vbpred_tree[] = { 0, 2, -1, 4, -2, 6, 8, 12, -3, 10, -5, -6, -4, 14, -7, 16, -8, -9 };
static const signed char vtoken_tree[] = { -11, 2, 0, 4, -1, 6, 8, 12, -2, 10, -3, -4, 14, 16, -5, -6, 18, 20, -7, -8, -9, -10 };
static const unsigned char vcat_probs[6][12] = {
    { 159, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
    { 165, 145, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
    { 173, 148, 140, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
    { 176, 155, 140, 135, 0, 0, 0, 0, 0, 0, 0, 0 },
    { 180, 157, 141, 134, 130, 0, 0, 0, 0, 0, 0, 0 },
    { 254, 254, 243, 230, 196, 177, 153, 140, 133, 130, 129, 0 },
};
static const unsigned char vcat_base[6] = { 5, 7, 11, 19, 35, 67 };
static const unsigned char vcoeff_bands[16] = { 0, 1, 2, 3, 6, 4, 5, 6, 6, 6, 6, 6, 6, 6, 6, 7 };
static const unsigned char vzigzag[16] = { 0, 1, 4, 8, 5, 2, 3, 6, 9, 12, 13, 10, 7, 11, 14, 15 };

struct vreader {
    const unsigned char *data;
    size_t length, pos;
    int error;
};

static unsigned vr_u8(struct vreader *r) {
    if (r->pos >= r->length) { r->error = 1; return 0; }
    return r->data[r->pos++];
}

static unsigned vr_u16(struct vreader *r) {
    unsigned a = vr_u8(r);
    return a | (vr_u8(r) << 8);
}

static unsigned vr_u24(struct vreader *r) {
    unsigned a = vr_u8(r);
    unsigned b = vr_u8(r);
    return a | (b << 8) | (vr_u8(r) << 16);
}

static int vr_bytes(struct vreader *r, unsigned char *dst, size_t n) {
    if (n > r->length - r->pos) { r->error = 1; return 0; }
    memcpy(dst, r->data + r->pos, n);
    r->pos += n;
    return 1;
}

struct vseg {
    short ydc, yac, y2dc, y2ac, uvdc, uvac;
    int delta_values;
    signed char quantizer_level, loopfilter_level;
};

struct vmb {
    signed char bpred[16];
    unsigned char complexity[9];
    signed char luma_mode, chroma_mode;
    unsigned char segmentid;
    unsigned char coeffs_skipped, non_zero_dct;
};

struct vframe {
    unsigned width, height, buffer_width;
    unsigned char *ybuf, *ubuf, *vbuf;
    int keyframe;
    unsigned char version;
    int for_display;
    unsigned char pixel_type;
    int filter_type;
    unsigned char filter_level, sharpness_level;
};

struct vp8 {
    struct vreader r;
    struct vbool b;
    unsigned mbwidth, mbheight;
    struct vmb *macroblocks;
    struct vframe frame;
    int segments_enabled, segments_update_map;
    struct vseg segment[VSEGMENTS];
    int loop_filter_adjustments_enabled;
    int ref_delta[4], mode_delta[4];
    struct vbool partitions[8];
    unsigned num_partitions;
    unsigned char segment_probs[3];
    unsigned char token_probs[4][8][3][VTOKEN_PROBS];
    unsigned char prob_intra;
    int prob_skip_false;
    struct vmb *top;
    struct vmb left;
    unsigned char *top_border_y, *left_border_y;
    unsigned char *top_border_u, *left_border_u;
    unsigned char *top_border_v, *left_border_v;
    unsigned top_border_y_len, left_border_y_len, top_border_u_len;
    int error;
};

static unsigned vbuffer_width(const struct vframe *f) {
    unsigned difference = f->width % 16;
    return difference ? f->width + (16 - difference % 16) : f->width;
}

static void vupdate_token_probabilities(struct vp8 *v) {
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 8; j++)
            for (int k = 0; k < 3; k++)
                for (int t = 0; t < VTOKEN_PROBS; t++)
                    if (vb_bit(&v->b, vcoeff_update_probs[i][j][k][t]))
                        v->token_probs[i][j][k][t] = (unsigned char)vb_literal(&v->b, 8);
    if (v->b.eof) v->error = 1;
}

static int vinit_partitions(struct vp8 *v, unsigned n) {
    unsigned char *buf;
    if (n > 1) {
        unsigned char sizes[21];
        if (!vr_bytes(&v->r, sizes, 3 * n - 3)) return 0;
        for (unsigned i = 0; i < n - 1; i++) {
            unsigned size = (unsigned)sizes[i * 3] | ((unsigned)sizes[i * 3 + 1] << 8) |
                ((unsigned)sizes[i * 3 + 2] << 16);
            buf = malloc(size ? size : 1);
            if (!buf) return 0;
            if (!vr_bytes(&v->r, buf, size)) { free(buf); return 0; }
            vb_init(&v->partitions[i], buf, size);
            v->partitions[i].data = buf;
        }
    }
    size_t remain = v->r.length - v->r.pos;
    buf = malloc(remain ? remain : 1);
    if (!buf) return 0;
    if (!vr_bytes(&v->r, buf, remain)) { free(buf); return 0; }
    vb_init(&v->partitions[n - 1], buf, remain);
    v->partitions[n - 1].data = buf;
    return 1;
}

static short vdc_quant(int index) {
    if (index < 0) index = 0;
    if (index > 127) index = 127;
    return vdc_quant_table[index];
}

static short vac_quant(int index) {
    if (index < 0) index = 0;
    if (index > 127) index = 127;
    return vac_quant_table[index];
}

static int vread_quantization_indices(struct vp8 *v) {
    unsigned yac_abs = vb_literal(&v->b, 7);
    int ydc_delta = vb_signed(&v->b, 4);
    int y2dc_delta = vb_signed(&v->b, 4);
    int y2ac_delta = vb_signed(&v->b, 4);
    int uvdc_delta = vb_signed(&v->b, 4);
    int uvac_delta = vb_signed(&v->b, 4);
    unsigned n = v->segments_enabled ? VSEGMENTS : 1;
    for (unsigned i = 0; i < n; i++) {
        int base = v->segments_enabled
            ? (v->segment[i].delta_values ? v->segment[i].quantizer_level + (int)yac_abs
                                          : v->segment[i].quantizer_level)
            : (int)yac_abs;
        v->segment[i].ydc = vdc_quant(base + ydc_delta);
        v->segment[i].yac = vac_quant(base);
        v->segment[i].y2dc = (short)(vdc_quant(base + y2dc_delta) * 2);
        v->segment[i].y2ac = (short)((int)vac_quant(base + y2ac_delta) * 155 / 100);
        v->segment[i].uvdc = vdc_quant(base + uvdc_delta);
        v->segment[i].uvac = vac_quant(base + uvac_delta);
        if (v->segment[i].y2ac < 8) v->segment[i].y2ac = 8;
        if (v->segment[i].uvdc > 132) v->segment[i].uvdc = 132;
    }
    if (v->b.eof) v->error = 1;
    return !v->error;
}

static void vread_loop_filter_adjustments(struct vp8 *v) {
    if (vb_flag(&v->b)) {
        for (int i = 0; i < 4; i++) v->ref_delta[i] = vb_signed(&v->b, 6);
        for (int i = 0; i < 4; i++) v->mode_delta[i] = vb_signed(&v->b, 6);
    }
    if (v->b.eof) v->error = 1;
}

static void vread_segment_updates(struct vp8 *v) {
    v->segments_update_map = vb_flag(&v->b);
    int update_data = vb_flag(&v->b);
    if (update_data) {
        int mode = vb_flag(&v->b);
        for (int i = 0; i < VSEGMENTS; i++) v->segment[i].delta_values = !mode;
        for (int i = 0; i < VSEGMENTS; i++)
            v->segment[i].quantizer_level = (signed char)vb_signed(&v->b, 7);
        for (int i = 0; i < VSEGMENTS; i++)
            v->segment[i].loopfilter_level = (signed char)vb_signed(&v->b, 6);
    }
    if (v->segments_update_map) {
        for (int i = 0; i < 3; i++) {
            int update = vb_flag(&v->b);
            v->segment_probs[i] = update ? (unsigned char)vb_literal(&v->b, 8) : 255;
        }
    }
    if (v->b.eof) v->error = 1;
}

static int vread_frame_header(struct vp8 *v) {
    unsigned tag = vr_u24(&v->r);
    if (v->r.error) return 0;
    v->frame.keyframe = (tag & 1) == 0;
    v->frame.version = (unsigned char)((tag >> 1) & 7);
    v->frame.for_display = (int)((tag >> 4) & 1);
    unsigned first_partition_size = tag >> 5;
    if (v->frame.keyframe) {
        unsigned char magic[3];
        if (!vr_bytes(&v->r, magic, 3)) return 0;
        if (magic[0] != 0x9d || magic[1] != 0x01 || magic[2] != 0x2a) return 0;
        unsigned w = vr_u16(&v->r);
        unsigned h = vr_u16(&v->r);
        if (v->r.error) return 0;
        v->frame.width = w & 0x3fff;
        v->frame.height = h & 0x3fff;
        v->mbwidth = (v->frame.width + 15) / 16;
        v->mbheight = (v->frame.height + 15) / 16;
        v->frame.buffer_width = vbuffer_width(&v->frame);
        size_t mbcount = (size_t)v->mbwidth * v->mbheight;
        v->macroblocks = calloc(mbcount ? mbcount : 1, sizeof(struct vmb));
        v->top = calloc(v->mbwidth ? v->mbwidth : 1, sizeof(struct vmb));
        size_t ysize = (size_t)v->mbwidth * 16 * v->mbheight * 16;
        size_t csize = (size_t)v->mbwidth * 8 * v->mbheight * 8;
        v->frame.ybuf = calloc(ysize ? ysize : 1, 1);
        v->frame.ubuf = calloc(csize ? csize : 1, 1);
        v->frame.vbuf = calloc(csize ? csize : 1, 1);
        v->top_border_y_len = v->frame.width + 4 + 16;
        v->top_border_y = malloc(v->top_border_y_len);
        v->left_border_y_len = 17;
        v->left_border_y = malloc(v->left_border_y_len);
        v->top_border_u_len = 8 * v->mbwidth;
        v->top_border_u = malloc(v->top_border_u_len ? v->top_border_u_len : 1);
        v->top_border_v = malloc(v->top_border_u_len ? v->top_border_u_len : 1);
        v->left_border_u = malloc(9);
        v->left_border_v = malloc(9);
        if (!v->macroblocks || !v->top || !v->frame.ybuf || !v->frame.ubuf || !v->frame.vbuf ||
                !v->top_border_y || !v->left_border_y || !v->top_border_u || !v->top_border_v ||
                !v->left_border_u || !v->left_border_v)
            return 0;
        memset(v->top_border_y, 127, v->top_border_y_len);
        memset(v->left_border_y, 129, v->left_border_y_len);
        memset(v->top_border_u, 127, v->top_border_u_len);
        memset(v->top_border_v, 127, v->top_border_u_len);
        memset(v->left_border_u, 129, 9);
        memset(v->left_border_v, 129, 9);
        for (unsigned i = 0; i < v->mbwidth; i++) {
            for (int k = 0; k < 16; k++) v->top[i].bpred[k] = VBDC;
            v->top[i].luma_mode = VDC_PRED;
        }
        memcpy(&v->left, &v->top[0], sizeof(struct vmb));
    }
    size_t size = first_partition_size;
    unsigned char *buf = malloc(size ? size : 1);
    if (!buf) return 0;
    if (!vr_bytes(&v->r, buf, size)) { free(buf); return 0; }
    vb_init(&v->b, buf, size);
    v->b.data = buf;
    if (size == 0) { v->b.final_remaining = -0xe; v->b.eof = 1; }
    if (v->frame.keyframe) {
        unsigned color_space = vb_literal(&v->b, 1);
        v->frame.pixel_type = (unsigned char)vb_literal(&v->b, 1);
        if (color_space != 0) { v->error = 1; return 0; }
    }
    v->segments_enabled = vb_flag(&v->b);
    if (v->segments_enabled) vread_segment_updates(v);
    v->frame.filter_type = vb_flag(&v->b);
    v->frame.filter_level = (unsigned char)vb_literal(&v->b, 6);
    v->frame.sharpness_level = (unsigned char)vb_literal(&v->b, 3);
    v->loop_filter_adjustments_enabled = vb_flag(&v->b);
    if (v->loop_filter_adjustments_enabled) vread_loop_filter_adjustments(v);
    unsigned num_partitions = 1u << vb_literal(&v->b, 2);
    if (v->b.eof || v->error) return 0;
    v->num_partitions = num_partitions;
    if (!vinit_partitions(v, num_partitions)) return 0;
    if (!vread_quantization_indices(v)) return 0;
    if (!v->frame.keyframe) return 0;
    (void)vb_literal(&v->b, 1);
    vupdate_token_probabilities(v);
    if (v->error) return 0;
    unsigned mb_no_skip_coeff = vb_literal(&v->b, 1);
    v->prob_skip_false = mb_no_skip_coeff == 1 ? (int)vb_literal(&v->b, 8) : -1;
    if (v->b.eof) return 0;
    return 1;
}

static int vread_macroblock_header(struct vp8 *v, unsigned mbx, struct vmb *mb) {
    memset(mb, 0, sizeof *mb);
    if (v->segments_enabled && v->segments_update_map)
        mb->segmentid = (unsigned char)vb_tree(&v->b, vsegment_tree, v->segment_probs, 0);
    mb->coeffs_skipped = v->prob_skip_false >= 0 ? (unsigned char)vb_bit(&v->b, (unsigned)v->prob_skip_false) : 0;
    int luma = vb_tree(&v->b, vymode_tree, vymode_probs, 0);
    if (luma < 0 || luma > 4) return 0;
    mb->luma_mode = (signed char)luma;
    if (luma != VBP_PRED) {
        int mode = luma == VDC_PRED ? VBDC : luma == VV_PRED ? VBVE : luma == VH_PRED ? VBHE : VBTM;
        for (int i = 0; i < 4; i++) {
            mb->bpred[12 + i] = (signed char)mode;
            v->left.bpred[i] = (signed char)mode;
        }
    } else {
        for (int y = 0; y < 4; y++) {
            for (int x = 0; x < 4; x++) {
                int top = v->top[mbx].bpred[12 + x];
                int left = v->left.bpred[y];
                int intra = vb_tree(&v->b, vbpred_tree, vbpred_probs[top][left], 0);
                if (intra < 0 || intra > 9) return 0;
                mb->bpred[x + y * 4] = (signed char)intra;
                v->top[mbx].bpred[12 + x] = (signed char)intra;
                v->left.bpred[y] = (signed char)intra;
            }
        }
    }
    int chroma = vb_tree(&v->b, vuvmode_tree, vuvmode_probs, 0);
    if (chroma < 0 || chroma > 3) return 0;
    mb->chroma_mode = (signed char)chroma;
    v->top[mbx].chroma_mode = mb->chroma_mode;
    v->top[mbx].luma_mode = mb->luma_mode;
    memcpy(v->top[mbx].bpred, mb->bpred, 16);
    if (v->b.eof) return 0;
    return 1;
}

static void viwht4x4(int *block) {
    for (int i = 0; i < 4; i++) {
        int a1 = block[i] + block[12 + i];
        int b1 = block[4 + i] + block[8 + i];
        int c1 = block[4 + i] - block[8 + i];
        int d1 = block[i] - block[12 + i];
        block[i] = a1 + b1;
        block[4 + i] = c1 + d1;
        block[8 + i] = a1 - b1;
        block[12 + i] = d1 - c1;
    }
    for (int i = 0; i < 4; i++) {
        int *b = block + 4 * i;
        int a1 = b[0] + b[3];
        int b1 = b[1] + b[2];
        int c1 = b[1] - b[2];
        int d1 = b[0] - b[3];
        int a2 = a1 + b1;
        int b2 = c1 + d1;
        int c2 = a1 - b1;
        int d2 = d1 - c1;
        b[0] = (a2 + 3) >> 3;
        b[1] = (b2 + 3) >> 3;
        b[2] = (c2 + 3) >> 3;
        b[3] = (d2 + 3) >> 3;
    }
}

static void vidct4x4(int *block) {
    for (int i = 0; i < 4; i++) {
        long long a1 = (long long)block[i] + block[8 + i];
        long long b1 = (long long)block[i] - block[8 + i];
        long long t1 = ((long long)block[4 + i] * 35468) >> 16;
        long long t2 = (long long)block[12 + i] + (((long long)block[12 + i] * 20091) >> 16);
        long long c1 = t1 - t2;
        t1 = (long long)block[4 + i] + (((long long)block[4 + i] * 20091) >> 16);
        t2 = ((long long)block[12 + i] * 35468) >> 16;
        long long d1 = t1 + t2;
        block[i] = (int)(a1 + d1);
        block[4 + i] = (int)(b1 + c1);
        block[12 + i] = (int)(a1 - d1);
        block[8 + i] = (int)(b1 - c1);
    }
    for (int i = 0; i < 4; i++) {
        long long a1 = (long long)block[4 * i] + block[4 * i + 2];
        long long b1 = (long long)block[4 * i] - block[4 * i + 2];
        long long t1 = ((long long)block[4 * i + 1] * 35468) >> 16;
        long long t2 = (long long)block[4 * i + 3] + (((long long)block[4 * i + 3] * 20091) >> 16);
        long long c1 = t1 - t2;
        t1 = (long long)block[4 * i + 1] + (((long long)block[4 * i + 1] * 20091) >> 16);
        t2 = ((long long)block[4 * i + 3] * 35468) >> 16;
        long long d1 = t1 + t2;
        block[4 * i] = (int)((a1 + d1 + 4) >> 3);
        block[4 * i + 3] = (int)((a1 - d1 + 4) >> 3);
        block[4 * i + 1] = (int)((b1 + c1 + 4) >> 3);
        block[4 * i + 2] = (int)((b1 - c1 + 4) >> 3);
    }
}

static int vread_coefficients(struct vp8 *v, int *block, unsigned p, int plane, int complexity,
        short dcq, short acq) {
    struct vbool *d = &v->partitions[p];
    int first = plane == 0 ? 1 : 0;
    int skip = 0;
    int has_coefficients = 0;
    for (int i = first; i < 16; i++) {
        int band = vcoeff_bands[i];
        const unsigned char *probs = v->token_probs[plane][band][complexity];
        int token = vb_tree(d, vtoken_tree, probs, skip);
        int abs_value;
        if (token == 11) break;
        if (token == 0) {
            skip = 1;
            has_coefficients = 1;
            complexity = 0;
            continue;
        }
        if (token >= 1 && token <= 4) {
            abs_value = token;
        } else if (token >= 5 && token <= 10) {
            const unsigned char *cat = vcat_probs[token - 5];
            int extra = 0;
            for (int t = 0; t < 12; t++) {
                if (cat[t] == 0) break;
                int bit = vb_bit(d, cat[t]);
                extra = extra + extra + bit;
            }
            abs_value = vcat_base[token - 5] + extra;
        } else {
            return 0;
        }
        skip = 0;
        complexity = abs_value == 0 ? 0 : abs_value == 1 ? 1 : 2;
        if (vb_flag(d)) abs_value = -abs_value;
        int zigzag = vzigzag[i];
        block[zigzag] = abs_value * (zigzag > 0 ? acq : dcq);
        has_coefficients = 1;
    }
    if (d->eof) v->error = 1;
    return has_coefficients;
}

static int vread_residual_data(struct vp8 *v, struct vmb *mb, unsigned mbx, unsigned p, int *blocks) {
    unsigned sindex = mb->segmentid < VSEGMENTS ? mb->segmentid : 0;
    int plane = mb->luma_mode == VBP_PRED ? 3 : 1;
    memset(blocks, 0, 384 * sizeof(int));
    if (plane == 1) {
        int complexity = v->top[mbx].complexity[0] + v->left.complexity[0];
        int block[16] = { 0 };
        int n = vread_coefficients(v, block, p, plane, complexity, v->segment[sindex].y2dc,
            v->segment[sindex].y2ac);
        if (v->error) return 0;
        v->left.complexity[0] = (unsigned char)n;
        v->top[mbx].complexity[0] = (unsigned char)n;
        viwht4x4(block);
        for (int k = 0; k < 16; k++) blocks[16 * k] = block[k];
        plane = 0;
    }
    for (int y = 0; y < 4; y++) {
        int left = v->left.complexity[y + 1];
        for (int x = 0; x < 4; x++) {
            int i = x + y * 4;
            int *block = blocks + i * 16;
            int complexity = v->top[mbx].complexity[x + 1] + left;
            int n = vread_coefficients(v, block, p, plane, complexity, v->segment[sindex].ydc,
                v->segment[sindex].yac);
            if (v->error) return 0;
            if (block[0] != 0 || n) {
                mb->non_zero_dct = 1;
                vidct4x4(block);
            }
            left = n;
            v->top[mbx].complexity[x + 1] = (unsigned char)n;
        }
        v->left.complexity[y + 1] = (unsigned char)left;
    }
    plane = 2;
    for (int ji = 0; ji < 2; ji++) {
        int j = ji == 0 ? 5 : 7;
        for (int y = 0; y < 2; y++) {
            int left = v->left.complexity[y + j];
            for (int x = 0; x < 2; x++) {
                int i = x + y * 2 + (j == 5 ? 16 : 20);
                int *block = blocks + i * 16;
                int complexity = v->top[mbx].complexity[x + j] + left;
                int n = vread_coefficients(v, block, p, plane, complexity, v->segment[sindex].uvdc,
                    v->segment[sindex].uvac);
                if (v->error) return 0;
                if (block[0] != 0 || n) {
                    mb->non_zero_dct = 1;
                    vidct4x4(block);
                }
                left = n;
                v->top[mbx].complexity[x + j] = (unsigned char)n;
            }
            v->left.complexity[y + j] = (unsigned char)left;
        }
    }
    return 1;
}

static unsigned char vavg3(unsigned char left, unsigned char this_, unsigned char right) {
    return (unsigned char)((left + 2 * this_ + right + 2) >> 2);
}

static unsigned char vavg2(unsigned char this_, unsigned char right) {
    return (unsigned char)((this_ + right + 1) >> 1);
}

static void vadd_residue(unsigned char *p, const int *r, unsigned y0, unsigned x0, unsigned stride) {
    unsigned pos = y0 * stride + x0;
    for (int y = 0; y < 4; y++) {
        for (int x = 0; x < 4; x++) {
            int val = r[y * 4 + x] + p[pos + x];
            p[pos + x] = (unsigned char)(val < 0 ? 0 : val > 255 ? 255 : val);
        }
        pos += stride;
    }
}

static void vpredict_v(unsigned char *a, unsigned size, unsigned x0, unsigned y0, unsigned stride) {
    const unsigned char *above = a + (y0 - 1) * stride + x0;
    for (unsigned y = 0; y < size; y++) {
        unsigned char *row = a + (y0 + y) * stride;
        for (unsigned x = 1; x < stride; x++) row[x] = above[x - x0];
    }
}

static void vpredict_h(unsigned char *a, unsigned size, unsigned x0, unsigned y0, unsigned stride) {
    for (unsigned y = 0; y < size; y++) {
        unsigned char *row = a + (y0 + y) * stride;
        unsigned char left = row[x0 - 1];
        for (unsigned x = x0; x < stride; x++) row[x] = left;
    }
}

static void vpredict_dc(unsigned char *a, unsigned size, unsigned stride, int above, int left) {
    unsigned sum = 0;
    int shf = size == 8 ? 2 : 3;
    if (left) {
        for (unsigned y = 0; y < size; y++) sum += a[(y + 1) * stride];
        shf += 1;
    }
    if (above) {
        for (unsigned x = 1; x <= size; x++) sum += a[x];
        shf += 1;
    }
    unsigned dcval = (!left && !above) ? 128 : (sum + (1u << (shf - 1))) >> shf;
    for (unsigned y = 0; y < size; y++) {
        unsigned char *row = a + (y + 1) * stride + 1;
        for (unsigned x = 0; x < size; x++) row[x] = (unsigned char)dcval;
    }
}

static void vpredict_tm(unsigned char *a, unsigned size, unsigned x0, unsigned y0, unsigned stride) {
    const unsigned char *above = a + (y0 - 1) * stride;
    int p = above[x0 - 1];
    for (unsigned y = 0; y < size; y++) {
        unsigned char *row = a + (y0 + y) * stride;
        int left_minus_p = (int)row[x0 - 1] - p;
        for (unsigned x = 0; x < size; x++) {
            int val = left_minus_p + above[x0 + x];
            row[x0 + x] = (unsigned char)(val < 0 ? 0 : val > 255 ? 255 : val);
        }
    }
}

static void vpredict_bdc(unsigned char *a, unsigned x0, unsigned y0, unsigned stride) {
    unsigned v = 4;
    const unsigned char *top = a + (y0 - 1) * stride + x0;
    for (int i = 0; i < 4; i++) v += top[i];
    for (int i = 0; i < 4; i++) v += a[(y0 + i) * stride + x0 - 1];
    v >>= 3;
    for (int y = 0; y < 4; y++) {
        unsigned char *row = a + (y0 + y) * stride + x0;
        memset(row, (int)v, 4);
    }
}

static void vpredict_bve(unsigned char *a, unsigned x0, unsigned y0, unsigned stride) {
    unsigned char p = a[(y0 - 1) * stride + x0 - 1];
    const unsigned char *t = a + (y0 - 1) * stride + x0;
    unsigned char avg[4];
    avg[0] = vavg3(p, t[0], t[1]);
    avg[1] = vavg3(t[0], t[1], t[2]);
    avg[2] = vavg3(t[1], t[2], t[3]);
    avg[3] = vavg3(t[2], t[3], t[4]);
    for (int y = 0; y < 4; y++) memcpy(a + (y0 + y) * stride + x0, avg, 4);
}

static void vpredict_bhe(unsigned char *a, unsigned x0, unsigned y0, unsigned stride) {
    unsigned char p = a[(y0 - 1) * stride + x0 - 1];
    unsigned char l0 = a[y0 * stride + x0 - 1];
    unsigned char l1 = a[(y0 + 1) * stride + x0 - 1];
    unsigned char l2 = a[(y0 + 2) * stride + x0 - 1];
    unsigned char l3 = a[(y0 + 3) * stride + x0 - 1];
    unsigned char avg[4];
    avg[0] = vavg3(p, l0, l1);
    avg[1] = vavg3(l0, l1, l2);
    avg[2] = vavg3(l1, l2, l3);
    avg[3] = vavg3(l2, l3, l3);
    for (int y = 0; y < 4; y++) memset(a + (y0 + y) * stride + x0, avg[y], 4);
}

static void vpredict_bld(unsigned char *a, unsigned x0, unsigned y0, unsigned stride) {
    const unsigned char *t = a + (y0 - 1) * stride + x0;
    unsigned char avg[7];
    avg[0] = vavg3(t[0], t[1], t[2]);
    avg[1] = vavg3(t[1], t[2], t[3]);
    avg[2] = vavg3(t[2], t[3], t[4]);
    avg[3] = vavg3(t[3], t[4], t[5]);
    avg[4] = vavg3(t[4], t[5], t[6]);
    avg[5] = vavg3(t[5], t[6], t[7]);
    avg[6] = vavg3(t[6], t[7], t[7]);
    for (int y = 0; y < 4; y++) memcpy(a + (y0 + y) * stride + x0, avg + y, 4);
}

static void vedge_pixels(unsigned char *a, unsigned x0, unsigned y0, unsigned stride,
        unsigned char *e) {
    unsigned pos = (y0 - 1) * stride + x0 - 1;
    e[0] = a[pos + 4 * stride];
    e[1] = a[pos + 3 * stride];
    e[2] = a[pos + 2 * stride];
    e[3] = a[pos + stride];
    e[4] = a[pos];
    e[5] = a[pos + 1];
    e[6] = a[pos + 2];
    e[7] = a[pos + 3];
    e[8] = a[pos + 4];
}

static void vpredict_brd(unsigned char *a, unsigned x0, unsigned y0, unsigned stride) {
    unsigned char e[9];
    vedge_pixels(a, x0, y0, stride, e);
    unsigned char avg[7];
    avg[0] = vavg3(e[0], e[1], e[2]);
    avg[1] = vavg3(e[1], e[2], e[3]);
    avg[2] = vavg3(e[2], e[3], e[4]);
    avg[3] = vavg3(e[3], e[4], e[5]);
    avg[4] = vavg3(e[4], e[5], e[6]);
    avg[5] = vavg3(e[5], e[6], e[7]);
    avg[6] = vavg3(e[6], e[7], e[8]);
    for (int y = 0; y < 4; y++) memcpy(a + (y0 + y) * stride + x0, avg + (3 - y), 4);
}

static void vpredict_bvr(unsigned char *a, unsigned x0, unsigned y0, unsigned stride) {
    unsigned char e[9];
    vedge_pixels(a, x0, y0, stride, e);
    a[(y0 + 3) * stride + x0] = vavg3(e[1], e[2], e[3]);
    a[(y0 + 2) * stride + x0] = vavg3(e[2], e[3], e[4]);
    a[(y0 + 3) * stride + x0 + 1] = vavg3(e[3], e[4], e[5]);
    a[(y0 + 1) * stride + x0] = vavg3(e[3], e[4], e[5]);
    a[(y0 + 2) * stride + x0 + 1] = vavg2(e[4], e[5]);
    a[y0 * stride + x0] = vavg2(e[4], e[5]);
    a[(y0 + 3) * stride + x0 + 2] = vavg3(e[4], e[5], e[6]);
    a[(y0 + 1) * stride + x0 + 1] = vavg3(e[4], e[5], e[6]);
    a[(y0 + 2) * stride + x0 + 2] = vavg2(e[5], e[6]);
    a[y0 * stride + x0 + 1] = vavg2(e[5], e[6]);
    a[(y0 + 3) * stride + x0 + 3] = vavg3(e[5], e[6], e[7]);
    a[(y0 + 1) * stride + x0 + 2] = vavg3(e[5], e[6], e[7]);
    a[(y0 + 2) * stride + x0 + 3] = vavg2(e[6], e[7]);
    a[y0 * stride + x0 + 2] = vavg2(e[6], e[7]);
    a[(y0 + 1) * stride + x0 + 3] = vavg3(e[6], e[7], e[8]);
    a[y0 * stride + x0 + 3] = vavg2(e[7], e[8]);
}

static void vpredict_bvl(unsigned char *a, unsigned x0, unsigned y0, unsigned stride) {
    const unsigned char *t = a + (y0 - 1) * stride + x0;
    a[y0 * stride + x0] = vavg2(t[0], t[1]);
    a[(y0 + 1) * stride + x0] = vavg3(t[0], t[1], t[2]);
    a[(y0 + 2) * stride + x0] = vavg2(t[1], t[2]);
    a[y0 * stride + x0 + 1] = vavg2(t[1], t[2]);
    a[(y0 + 1) * stride + x0 + 1] = vavg3(t[1], t[2], t[3]);
    a[(y0 + 3) * stride + x0] = vavg3(t[1], t[2], t[3]);
    a[(y0 + 2) * stride + x0 + 1] = vavg2(t[2], t[3]);
    a[y0 * stride + x0 + 2] = vavg2(t[2], t[3]);
    a[(y0 + 3) * stride + x0 + 1] = vavg3(t[2], t[3], t[4]);
    a[(y0 + 1) * stride + x0 + 2] = vavg3(t[2], t[3], t[4]);
    a[(y0 + 2) * stride + x0 + 2] = vavg2(t[3], t[4]);
    a[y0 * stride + x0 + 3] = vavg2(t[3], t[4]);
    a[(y0 + 3) * stride + x0 + 2] = vavg3(t[3], t[4], t[5]);
    a[(y0 + 1) * stride + x0 + 3] = vavg3(t[3], t[4], t[5]);
    a[(y0 + 2) * stride + x0 + 3] = vavg3(t[4], t[5], t[6]);
    a[(y0 + 3) * stride + x0 + 3] = vavg3(t[5], t[6], t[7]);
}

static void vpredict_bhd(unsigned char *a, unsigned x0, unsigned y0, unsigned stride) {
    unsigned char e[9];
    vedge_pixels(a, x0, y0, stride, e);
    a[(y0 + 3) * stride + x0] = vavg2(e[0], e[1]);
    a[(y0 + 3) * stride + x0 + 1] = vavg3(e[0], e[1], e[2]);
    a[(y0 + 2) * stride + x0] = vavg2(e[1], e[2]);
    a[(y0 + 3) * stride + x0 + 2] = vavg2(e[1], e[2]);
    a[(y0 + 2) * stride + x0 + 1] = vavg3(e[1], e[2], e[3]);
    a[(y0 + 3) * stride + x0 + 3] = vavg3(e[1], e[2], e[3]);
    a[(y0 + 2) * stride + x0 + 2] = vavg2(e[2], e[3]);
    a[(y0 + 1) * stride + x0] = vavg2(e[2], e[3]);
    a[(y0 + 2) * stride + x0 + 3] = vavg3(e[2], e[3], e[4]);
    a[(y0 + 1) * stride + x0 + 1] = vavg3(e[2], e[3], e[4]);
    a[(y0 + 1) * stride + x0 + 2] = vavg2(e[3], e[4]);
    a[y0 * stride + x0] = vavg2(e[3], e[4]);
    a[(y0 + 1) * stride + x0 + 3] = vavg3(e[3], e[4], e[5]);
    a[y0 * stride + x0 + 1] = vavg3(e[3], e[4], e[5]);
    a[y0 * stride + x0 + 2] = vavg3(e[4], e[5], e[6]);
    a[y0 * stride + x0 + 3] = vavg3(e[5], e[6], e[7]);
}

static void vpredict_bhu(unsigned char *a, unsigned x0, unsigned y0, unsigned stride) {
    unsigned char l0 = a[y0 * stride + x0 - 1];
    unsigned char l1 = a[(y0 + 1) * stride + x0 - 1];
    unsigned char l2 = a[(y0 + 2) * stride + x0 - 1];
    unsigned char l3 = a[(y0 + 3) * stride + x0 - 1];
    a[y0 * stride + x0] = vavg2(l0, l1);
    a[y0 * stride + x0 + 1] = vavg3(l0, l1, l2);
    a[y0 * stride + x0 + 2] = vavg2(l1, l2);
    a[(y0 + 1) * stride + x0] = vavg2(l1, l2);
    a[y0 * stride + x0 + 3] = vavg3(l1, l2, l3);
    a[(y0 + 1) * stride + x0 + 1] = vavg3(l1, l2, l3);
    a[(y0 + 1) * stride + x0 + 2] = vavg2(l2, l3);
    a[(y0 + 2) * stride + x0] = vavg2(l2, l3);
    a[(y0 + 1) * stride + x0 + 3] = vavg3(l2, l3, l3);
    a[(y0 + 2) * stride + x0 + 1] = vavg3(l2, l3, l3);
    a[(y0 + 2) * stride + x0 + 2] = l3;
    a[(y0 + 2) * stride + x0 + 3] = l3;
    a[(y0 + 3) * stride + x0] = l3;
    a[(y0 + 3) * stride + x0 + 1] = l3;
    a[(y0 + 3) * stride + x0 + 2] = l3;
    a[(y0 + 3) * stride + x0 + 3] = l3;
}

static void vpredict_4x4(unsigned char *ws, unsigned stride, const signed char *modes,
        const int *resdata) {
    for (int sby = 0; sby < 4; sby++) {
        for (int sbx = 0; sbx < 4; sbx++) {
            int i = sbx + sby * 4;
            unsigned y0 = (unsigned)sby * 4 + 1;
            unsigned x0 = (unsigned)sbx * 4 + 1;
            switch (modes[i]) {
                case VBTM: vpredict_tm(ws, 4, x0, y0, stride); break;
                case VBVE: vpredict_bve(ws, x0, y0, stride); break;
                case VBHE: vpredict_bhe(ws, x0, y0, stride); break;
                case VBDC: vpredict_bdc(ws, x0, y0, stride); break;
                case VBLD: vpredict_bld(ws, x0, y0, stride); break;
                case VBRD: vpredict_brd(ws, x0, y0, stride); break;
                case VBVR: vpredict_bvr(ws, x0, y0, stride); break;
                case VBVL: vpredict_bvl(ws, x0, y0, stride); break;
                case VBHD: vpredict_bhd(ws, x0, y0, stride); break;
                case VBHU: vpredict_bhu(ws, x0, y0, stride); break;
                default: break;
            }
            vadd_residue(ws, resdata + i * 16, y0, x0, stride);
        }
    }
}

static void vcreate_border_luma(unsigned mbx, unsigned mby, unsigned mbw,
        const unsigned char *top, const unsigned char *left, unsigned char *ws) {
    unsigned stride = 1 + 16 + 4;
    memset(ws, 0, (1 + 16) * stride);
    if (mby == 0) {
        memset(ws + 1, 127, stride - 1);
    } else {
        memcpy(ws + 1, top + mbx * 16, 16);
        if (mbx == mbw - 1)
            memset(ws + 1 + 16, top[mbx * 16 + 15], stride - 1 - 16);
        else
            memcpy(ws + 1 + 16, top + mbx * 16 + 16, stride - 1 - 16);
    }
    for (unsigned i = 17; i < stride; i++) {
        ws[4 * stride + i] = ws[i];
        ws[8 * stride + i] = ws[i];
        ws[12 * stride + i] = ws[i];
    }
    if (mbx == 0) {
        for (unsigned i = 0; i < 16; i++) ws[(i + 1) * stride] = 129;
    } else {
        for (unsigned i = 0; i < 16; i++) ws[(i + 1) * stride] = left[1 + i];
    }
    ws[0] = mby == 0 ? 127 : mbx == 0 ? 129 : left[0];
}

static void vcreate_border_chroma(unsigned mbx, unsigned mby, const unsigned char *top,
        const unsigned char *left, unsigned char *ws) {
    unsigned stride = 1 + 8;
    memset(ws, 0, stride * stride);
    if (mby == 0) {
        memset(ws + 1, 127, 8);
    } else {
        memcpy(ws + 1, top + mbx * 8, 8);
    }
    if (mbx == 0) {
        for (unsigned y = 0; y < 8; y++) ws[(y + 1) * stride] = 129;
    } else {
        for (unsigned y = 0; y < 8; y++) ws[(y + 1) * stride] = left[1 + y];
    }
    ws[0] = mby == 0 ? 127 : mbx == 0 ? 129 : left[0];
}

static void vset_chroma_border(unsigned char *left, unsigned char *top,
        const unsigned char *ws, unsigned mbx) {
    unsigned stride = 1 + 8;
    left[0] = ws[8];
    for (unsigned i = 0; i < 8; i++) left[1 + i] = ws[(i + 1) * stride + 8];
    memcpy(top + mbx * 8, ws + 8 * stride + 1, 8);
}

static void vintra_predict_luma(struct vp8 *v, unsigned mbx, unsigned mby, const struct vmb *mb,
        const int *resdata) {
    unsigned stride = 1 + 16 + 4;
    unsigned mw = v->mbwidth;
    unsigned char ws[(1 + 16) * (1 + 16 + 4)];
    vcreate_border_luma(mbx, mby, mw, v->top_border_y, v->left_border_y, ws);
    switch (mb->luma_mode) {
        case VV_PRED: vpredict_v(ws, 16, 1, 1, stride); break;
        case VH_PRED: vpredict_h(ws, 16, 1, 1, stride); break;
        case VTM_PRED: vpredict_tm(ws, 16, 1, 1, stride); break;
        case VDC_PRED: vpredict_dc(ws, 16, stride, mby != 0, mbx != 0); break;
        case VBP_PRED: vpredict_4x4(ws, stride, mb->bpred, resdata); break;
        default: break;
    }
    if (mb->luma_mode != VBP_PRED) {
        for (unsigned y = 0; y < 4; y++)
            for (unsigned x = 0; x < 4; x++)
                vadd_residue(ws, resdata + (x + y * 4) * 16, 1 + y * 4, 1 + x * 4, stride);
    }
    v->left_border_y[0] = ws[16];
    for (unsigned i = 0; i < 16; i++) v->left_border_y[1 + i] = ws[(i + 1) * stride + 16];
    for (unsigned i = 0; i < 16; i++) v->top_border_y[mbx * 16 + i] = ws[16 * stride + 1 + i];
    for (unsigned y = 0; y < 16; y++)
        for (unsigned x = 0; x < 16; x++)
            v->frame.ybuf[(mby * 16 + y) * mw * 16 + mbx * 16 + x] = ws[(1 + y) * stride + 1 + x];
}

static void vintra_predict_chroma(struct vp8 *v, unsigned mbx, unsigned mby, const struct vmb *mb,
        const int *resdata) {
    unsigned stride = 1 + 8;
    unsigned mw = v->mbwidth;
    unsigned char uws[(1 + 8) * (1 + 8)];
    unsigned char vws[(1 + 8) * (1 + 8)];
    vcreate_border_chroma(mbx, mby, v->top_border_u, v->left_border_u, uws);
    vcreate_border_chroma(mbx, mby, v->top_border_v, v->left_border_v, vws);
    switch (mb->chroma_mode) {
        case VDC_PRED:
            vpredict_dc(uws, 8, stride, mby != 0, mbx != 0);
            vpredict_dc(vws, 8, stride, mby != 0, mbx != 0);
            break;
        case VV_PRED:
            vpredict_v(uws, 8, 1, 1, stride);
            vpredict_v(vws, 8, 1, 1, stride);
            break;
        case VH_PRED:
            vpredict_h(uws, 8, 1, 1, stride);
            vpredict_h(vws, 8, 1, 1, stride);
            break;
        case VTM_PRED:
            vpredict_tm(uws, 8, 1, 1, stride);
            vpredict_tm(vws, 8, 1, 1, stride);
            break;
        default: break;
    }
    for (unsigned y = 0; y < 2; y++) {
        for (unsigned x = 0; x < 2; x++) {
            unsigned i = x + y * 2;
            vadd_residue(uws, resdata + 16 * 16 + i * 16, 1 + y * 4, 1 + x * 4, stride);
            vadd_residue(vws, resdata + 20 * 16 + i * 16, 1 + y * 4, 1 + x * 4, stride);
        }
    }
    vset_chroma_border(v->left_border_u, v->top_border_u, uws, mbx);
    vset_chroma_border(v->left_border_v, v->top_border_v, vws, mbx);
    for (unsigned y = 0; y < 8; y++) {
        for (unsigned x = 0; x < 8; x++) {
            unsigned index = (mby * 8 + y) * mw * 8 + mbx * 8 + x;
            v->frame.ubuf[index] = uws[(1 + y) * stride + 1 + x];
            v->frame.vbuf[index] = vws[(1 + y) * stride + 1 + x];
        }
    }
}

static int vdiff(unsigned char a, unsigned char b) { return a > b ? a - b : b - a; }

static int vc(int val) { return val < -128 ? -128 : val > 127 ? 127 : val; }

static int vu2s(unsigned char val) { return (int)val - 128; }

static unsigned char vs2u(int val) { return (unsigned char)(vc(val) + 128); }

static int vcommon_adjust_vertical(int use_outer_taps, unsigned char *pixels, unsigned point,
        unsigned stride) {
    int p1 = vu2s(pixels[point - 2 * stride]);
    int p0 = vu2s(pixels[point - stride]);
    int q0 = vu2s(pixels[point]);
    int q1 = vu2s(pixels[point + stride]);
    int outer = use_outer_taps ? vc(p1 - q1) : 0;
    int a = vc(outer + 3 * (q0 - p0));
    int b = vc(a + 3) >> 3;
    a = vc(a + 4) >> 3;
    pixels[point] = vs2u(q0 - a);
    pixels[point - stride] = vs2u(p0 + b);
    return a;
}

static int vcommon_adjust_horizontal(int use_outer_taps, unsigned char *pixels) {
    int p1 = vu2s(pixels[2]);
    int p0 = vu2s(pixels[3]);
    int q0 = vu2s(pixels[4]);
    int q1 = vu2s(pixels[5]);
    int outer = use_outer_taps ? vc(p1 - q1) : 0;
    int a = vc(outer + 3 * (q0 - p0));
    int b = vc(a + 3) >> 3;
    a = vc(a + 4) >> 3;
    pixels[4] = vs2u(q0 - a);
    pixels[3] = vs2u(p0 + b);
    return a;
}

static int vsimple_threshold_vertical(int limit, const unsigned char *pixels, unsigned point,
        unsigned stride) {
    return vdiff(pixels[point - stride], pixels[point]) * 2 +
        vdiff(pixels[point - 2 * stride], pixels[point + stride]) / 2 <= limit;
}

static int vsimple_threshold_horizontal(int limit, const unsigned char *pixels) {
    return vdiff(pixels[3], pixels[4]) * 2 + vdiff(pixels[2], pixels[5]) / 2 <= limit;
}

static int vshould_filter_vertical(unsigned char interior_limit, unsigned char edge_limit,
        const unsigned char *pixels, unsigned point, unsigned stride) {
    return vsimple_threshold_vertical(edge_limit, pixels, point, stride) &&
        vdiff(pixels[point - 4 * stride], pixels[point - 3 * stride]) <= interior_limit &&
        vdiff(pixels[point - 3 * stride], pixels[point - 2 * stride]) <= interior_limit &&
        vdiff(pixels[point - 2 * stride], pixels[point - stride]) <= interior_limit &&
        vdiff(pixels[point + 3 * stride], pixels[point + 2 * stride]) <= interior_limit &&
        vdiff(pixels[point + 2 * stride], pixels[point + stride]) <= interior_limit &&
        vdiff(pixels[point + stride], pixels[point]) <= interior_limit;
}

static int vshould_filter_horizontal(unsigned char interior_limit, unsigned char edge_limit,
        const unsigned char *pixels) {
    return vsimple_threshold_horizontal(edge_limit, pixels) &&
        vdiff(pixels[0], pixels[1]) <= interior_limit &&
        vdiff(pixels[1], pixels[2]) <= interior_limit &&
        vdiff(pixels[2], pixels[3]) <= interior_limit &&
        vdiff(pixels[7], pixels[6]) <= interior_limit &&
        vdiff(pixels[6], pixels[5]) <= interior_limit &&
        vdiff(pixels[5], pixels[4]) <= interior_limit;
}

static int vhigh_edge_variance_vertical(unsigned char threshold, const unsigned char *pixels,
        unsigned point, unsigned stride) {
    return vdiff(pixels[point - 2 * stride], pixels[point - stride]) > threshold ||
        vdiff(pixels[point + stride], pixels[point]) > threshold;
}

static int vhigh_edge_variance_horizontal(unsigned char threshold, const unsigned char *pixels) {
    return vdiff(pixels[2], pixels[3]) > threshold || vdiff(pixels[5], pixels[4]) > threshold;
}

static void vsimple_segment_vertical(unsigned char edge_limit, unsigned char *pixels,
        unsigned point, unsigned stride) {
    if (vsimple_threshold_vertical(edge_limit, pixels, point, stride))
        vcommon_adjust_vertical(1, pixels, point, stride);
}

static void vsimple_segment_horizontal(unsigned char edge_limit, unsigned char *pixels) {
    if (vsimple_threshold_horizontal(edge_limit, pixels))
        vcommon_adjust_horizontal(1, pixels);
}

static void vsubblock_filter_vertical(unsigned char hev_threshold, unsigned char interior_limit,
        unsigned char edge_limit, unsigned char *pixels, unsigned point, unsigned stride) {
    if (vshould_filter_vertical(interior_limit, edge_limit, pixels, point, stride)) {
        int hv = vhigh_edge_variance_vertical(hev_threshold, pixels, point, stride);
        int a = (vcommon_adjust_vertical(hv, pixels, point, stride) + 1) >> 1;
        if (!hv) {
            pixels[point + stride] = vs2u(vu2s(pixels[point + stride]) - a);
            pixels[point - 2 * stride] = vs2u(vu2s(pixels[point - 2 * stride]) + a);
        }
    }
}

static void vsubblock_filter_horizontal(unsigned char hev_threshold, unsigned char interior_limit,
        unsigned char edge_limit, unsigned char *pixels) {
    if (vshould_filter_horizontal(interior_limit, edge_limit, pixels)) {
        int hv = vhigh_edge_variance_horizontal(hev_threshold, pixels);
        int a = (vcommon_adjust_horizontal(hv, pixels) + 1) >> 1;
        if (!hv) {
            pixels[5] = vs2u(vu2s(pixels[5]) - a);
            pixels[2] = vs2u(vu2s(pixels[2]) + a);
        }
    }
}

static void vmacroblock_filter_vertical(unsigned char hev_threshold, unsigned char interior_limit,
        unsigned char edge_limit, unsigned char *pixels, unsigned point, unsigned stride) {
    if (vshould_filter_vertical(interior_limit, edge_limit, pixels, point, stride)) {
        if (!vhigh_edge_variance_vertical(hev_threshold, pixels, point, stride)) {
            int p2 = vu2s(pixels[point - 3 * stride]);
            int p1 = vu2s(pixels[point - 2 * stride]);
            int p0 = vu2s(pixels[point - stride]);
            int q0 = vu2s(pixels[point]);
            int q1 = vu2s(pixels[point + stride]);
            int q2 = vu2s(pixels[point + 2 * stride]);
            int w = vc(vc(p1 - q1) + 3 * (q0 - p0));
            int a = vc((27 * w + 63) >> 7);
            pixels[point] = vs2u(q0 - a);
            pixels[point - stride] = vs2u(p0 + a);
            a = vc((18 * w + 63) >> 7);
            pixels[point + stride] = vs2u(q1 - a);
            pixels[point - 2 * stride] = vs2u(p1 + a);
            a = vc((9 * w + 63) >> 7);
            pixels[point + 2 * stride] = vs2u(q2 - a);
            pixels[point - 3 * stride] = vs2u(p2 + a);
        } else {
            vcommon_adjust_vertical(1, pixels, point, stride);
        }
    }
}

static void vmacroblock_filter_horizontal(unsigned char hev_threshold, unsigned char interior_limit,
        unsigned char edge_limit, unsigned char *pixels) {
    if (vshould_filter_horizontal(interior_limit, edge_limit, pixels)) {
        if (!vhigh_edge_variance_horizontal(hev_threshold, pixels)) {
            int p2 = vu2s(pixels[1]);
            int p1 = vu2s(pixels[2]);
            int p0 = vu2s(pixels[3]);
            int q0 = vu2s(pixels[4]);
            int q1 = vu2s(pixels[5]);
            int q2 = vu2s(pixels[6]);
            int w = vc(vc(p1 - q1) + 3 * (q0 - p0));
            int a = vc((27 * w + 63) >> 7);
            pixels[4] = vs2u(q0 - a);
            pixels[3] = vs2u(p0 + a);
            a = vc((18 * w + 63) >> 7);
            pixels[5] = vs2u(q1 - a);
            pixels[2] = vs2u(p1 + a);
            a = vc((9 * w + 63) >> 7);
            pixels[6] = vs2u(q2 - a);
            pixels[1] = vs2u(p2 + a);
        } else {
            vcommon_adjust_horizontal(1, pixels);
        }
    }
}

static void vcalculate_filter_parameters(const struct vp8 *v, const struct vmb *mb,
        unsigned char *filter_level_out, unsigned char *interior_limit_out,
        unsigned char *hev_threshold_out) {
    const struct vseg *segment = &v->segment[mb->segmentid < VSEGMENTS ? mb->segmentid : 0];
    int filter_level = v->frame.filter_level;
    if (filter_level == 0) {
        *filter_level_out = 0;
        *interior_limit_out = 0;
        *hev_threshold_out = 0;
        return;
    }
    if (v->segments_enabled) {
        if (segment->delta_values) filter_level += segment->loopfilter_level;
        else filter_level = segment->loopfilter_level;
    }
    filter_level = filter_level < 0 ? 0 : filter_level > 63 ? 63 : filter_level;
    if (v->loop_filter_adjustments_enabled) {
        filter_level += v->ref_delta[0];
        if (mb->luma_mode == VBP_PRED) filter_level += v->mode_delta[0];
    }
    filter_level = filter_level < 0 ? 0 : filter_level > 63 ? 63 : filter_level;
    unsigned char filter_level_u = (unsigned char)filter_level;
    int interior_limit = filter_level_u;
    if (v->frame.sharpness_level > 0) {
        interior_limit >>= v->frame.sharpness_level > 4 ? 2 : 1;
        if (interior_limit > 9 - v->frame.sharpness_level)
            interior_limit = 9 - v->frame.sharpness_level;
    }
    if (interior_limit == 0) interior_limit = 1;
    int hev_threshold = 0;
    if (v->frame.keyframe) {
        if (filter_level_u >= 40) hev_threshold = 2;
        else if (filter_level_u >= 15) hev_threshold = 1;
    } else {
        if (filter_level_u >= 40) hev_threshold = 3;
        else if (filter_level_u >= 20) hev_threshold = 2;
        else if (filter_level_u >= 15) hev_threshold = 1;
    }
    *filter_level_out = filter_level_u;
    *interior_limit_out = (unsigned char)interior_limit;
    *hev_threshold_out = (unsigned char)hev_threshold;
}

static void vloop_filter(struct vp8 *v, unsigned mbx, unsigned mby, const struct vmb *mb) {
    unsigned luma_w = v->mbwidth * 16;
    unsigned chroma_w = v->mbwidth * 8;
    unsigned char filter_level, interior_limit, hev_threshold;
    vcalculate_filter_parameters(v, mb, &filter_level, &interior_limit, &hev_threshold);
    if (!filter_level) return;
    unsigned mbedge_limit = (filter_level + 2) * 2 + interior_limit;
    unsigned sub_bedge_limit = filter_level * 2 + interior_limit;
    int do_subblock = mb->luma_mode == VBP_PRED || (!mb->coeffs_skipped && mb->non_zero_dct);
    if (mbx > 0) {
        if (v->frame.filter_type) {
            for (unsigned y = 0; y < 16; y++)
                vsimple_segment_horizontal((unsigned char)mbedge_limit,
                    v->frame.ybuf + (mby * 16 + y) * luma_w + mbx * 16 - 4);
        } else {
            for (unsigned y = 0; y < 16; y++)
                vmacroblock_filter_horizontal(hev_threshold, interior_limit,
                    (unsigned char)mbedge_limit,
                    v->frame.ybuf + (mby * 16 + y) * luma_w + mbx * 16 - 4);
            for (unsigned y = 0; y < 8; y++) {
                vmacroblock_filter_horizontal(hev_threshold, interior_limit,
                    (unsigned char)mbedge_limit,
                    v->frame.ubuf + (mby * 8 + y) * chroma_w + mbx * 8 - 4);
                vmacroblock_filter_horizontal(hev_threshold, interior_limit,
                    (unsigned char)mbedge_limit,
                    v->frame.vbuf + (mby * 8 + y) * chroma_w + mbx * 8 - 4);
            }
        }
    }
    if (do_subblock) {
        if (v->frame.filter_type) {
            for (unsigned x = 4; x < 13; x += 4)
                for (unsigned y = 0; y < 16; y++)
                    vsimple_segment_horizontal((unsigned char)sub_bedge_limit,
                        v->frame.ybuf + (mby * 16 + y) * luma_w + mbx * 16 + x - 4);
        } else {
            for (unsigned x = 4; x < 13; x += 4) {
                for (unsigned y = 0; y < 16; y++)
                    vsubblock_filter_horizontal(hev_threshold, interior_limit,
                        (unsigned char)sub_bedge_limit,
                        v->frame.ybuf + (mby * 16 + y) * luma_w + mbx * 16 + x - 4);
            }
            for (unsigned y = 0; y < 8; y++) {
                vsubblock_filter_horizontal(hev_threshold, interior_limit,
                    (unsigned char)sub_bedge_limit,
                    v->frame.ubuf + (mby * 8 + y) * chroma_w + mbx * 8);
                vsubblock_filter_horizontal(hev_threshold, interior_limit,
                    (unsigned char)sub_bedge_limit,
                    v->frame.vbuf + (mby * 8 + y) * chroma_w + mbx * 8);
            }
        }
    }
    if (mby > 0) {
        if (v->frame.filter_type) {
            for (unsigned x = 0; x < 16; x++)
                vsimple_segment_vertical((unsigned char)mbedge_limit, v->frame.ybuf,
                    mby * 16 * luma_w + mbx * 16 + x, luma_w);
        } else {
            for (unsigned x = 0; x < 16; x++)
                vmacroblock_filter_vertical(hev_threshold, interior_limit,
                    (unsigned char)mbedge_limit, v->frame.ybuf,
                    mby * 16 * luma_w + mbx * 16 + x, luma_w);
            for (unsigned x = 0; x < 8; x++) {
                vmacroblock_filter_vertical(hev_threshold, interior_limit,
                    (unsigned char)mbedge_limit, v->frame.ubuf,
                    mby * 8 * chroma_w + mbx * 8 + x, chroma_w);
                vmacroblock_filter_vertical(hev_threshold, interior_limit,
                    (unsigned char)mbedge_limit, v->frame.vbuf,
                    mby * 8 * chroma_w + mbx * 8 + x, chroma_w);
            }
        }
    }
    if (do_subblock) {
        if (v->frame.filter_type) {
            for (unsigned y = 4; y < 13; y += 4)
                for (unsigned x = 0; x < 16; x++)
                    vsimple_segment_vertical((unsigned char)sub_bedge_limit, v->frame.ybuf,
                        (mby * 16 + y) * luma_w + mbx * 16 + x, luma_w);
        } else {
            for (unsigned y = 4; y < 13; y += 4) {
                for (unsigned x = 0; x < 16; x++)
                    vsubblock_filter_vertical(hev_threshold, interior_limit,
                        (unsigned char)sub_bedge_limit, v->frame.ybuf,
                        (mby * 16 + y) * luma_w + mbx * 16 + x, luma_w);
            }
            for (unsigned x = 0; x < 8; x++) {
                vsubblock_filter_vertical(hev_threshold, interior_limit,
                    (unsigned char)sub_bedge_limit, v->frame.ubuf,
                    (mby * 8 + 4) * chroma_w + mbx * 8 + x, chroma_w);
                vsubblock_filter_vertical(hev_threshold, interior_limit,
                    (unsigned char)sub_bedge_limit, v->frame.vbuf,
                    (mby * 8 + 4) * chroma_w + mbx * 8 + x, chroma_w);
            }
        }
    }
}

static int vmulhi(unsigned char v, unsigned coeff) { return (int)(((unsigned)v * coeff) >> 8); }

static unsigned char vclip(int v) {
    v >>= 6;
    return (unsigned char)(v < 0 ? 0 : v > 255 ? 255 : v);
}

static void vset_pixel(unsigned char *p, unsigned char y, unsigned char u, unsigned char v) {
    int yy = vmulhi(y, 19077);
    p[0] = vclip(yy + vmulhi(v, 26149) - 14234);
    p[1] = vclip(yy - vmulhi(u, 6419) - vmulhi(v, 13320) + 8708);
    p[2] = vclip(yy + vmulhi(u, 33050) - 17685);
}

static void vfill_rgba(const struct vframe *f, unsigned char *buf, int has_alpha) {
    unsigned width = f->width, height = f->height;
    unsigned bw = f->buffer_width;
    unsigned cw = bw / 2;
    unsigned chroma_width = (width + 1) / 2;
    unsigned chroma_height = (height + 1) / 2;
    for (unsigned y = 0; y < height; y++) {
        unsigned mr = y / 2;
        unsigned sr = (y & 1) ? mr + 1 : (mr ? mr - 1 : 0);
        if (sr >= chroma_height) sr = chroma_height - 1;
        for (unsigned x = 0; x < width; x++) {
            unsigned xm, xs;
            if (x == 0) {
                xm = 0;
                xs = 0;
            } else if (x & 1) {
                xm = x / 2;
                xs = xm + 1;
                if (xs >= chroma_width) xs = chroma_width - 1;
            } else {
                xm = x / 2;
                xs = xm - 1;
            }
            unsigned u = (9u * f->ubuf[mr * cw + xm] + 3u * f->ubuf[mr * cw + xs] +
                3u * f->ubuf[sr * cw + xm] + f->ubuf[sr * cw + xs] + 8) / 16;
            unsigned v = (9u * f->vbuf[mr * cw + xm] + 3u * f->vbuf[mr * cw + xs] +
                3u * f->vbuf[sr * cw + xm] + f->vbuf[sr * cw + xs] + 8) / 16;
            unsigned char *p = buf + ((size_t)y * width + x) * 4;
            vset_pixel(p, f->ybuf[y * bw + x], (unsigned char)u, (unsigned char)v);
            if (!has_alpha) p[3] = 255;
        }
    }
}

static void vfree_frame(struct vframe *f) {
    free(f->ybuf);
    free(f->ubuf);
    free(f->vbuf);
    memset(f, 0, sizeof *f);
}

static int vdecode(const unsigned char *data, size_t length, struct vframe *out) {
    struct vp8 v;
    memset(&v, 0, sizeof v);
    v.r.data = data;
    v.r.length = length;
    v.segment_probs[0] = v.segment_probs[1] = v.segment_probs[2] = 255;
    memcpy(v.token_probs, vcoeff_probs, sizeof v.token_probs);
    v.prob_skip_false = -1;
    int ok = 0;
    if (!vread_frame_header(&v)) goto done;
    for (unsigned mby = 0; mby < v.mbheight && !v.error; mby++) {
        unsigned p = mby % v.num_partitions;
        memset(&v.left, 0, sizeof v.left);
        for (unsigned mbx = 0; mbx < v.mbwidth; mbx++) {
            struct vmb mb;
            int blocks[384];
            if (!vread_macroblock_header(&v, mbx, &mb)) goto done;
            if (!mb.coeffs_skipped) {
                if (!vread_residual_data(&v, &mb, mbx, p, blocks)) goto done;
            } else {
                memset(blocks, 0, sizeof blocks);
                if (mb.luma_mode != VBP_PRED) {
                    v.left.complexity[0] = 0;
                    v.top[mbx].complexity[0] = 0;
                }
                for (int i = 1; i < 9; i++) {
                    v.left.complexity[i] = 0;
                    v.top[mbx].complexity[i] = 0;
                }
            }
            vintra_predict_luma(&v, mbx, mby, &mb, blocks);
            vintra_predict_chroma(&v, mbx, mby, &mb, blocks);
            v.macroblocks[mby * v.mbwidth + mbx] = mb;
        }
        memset(v.left_border_y, 129, v.left_border_y_len);
        memset(v.left_border_u, 129, 9);
        memset(v.left_border_v, 129, 9);
    }
    if (v.error) goto done;
    for (unsigned mby = 0; mby < v.mbheight; mby++)
        for (unsigned mbx = 0; mbx < v.mbwidth; mbx++)
            vloop_filter(&v, mbx, mby, &v.macroblocks[mby * v.mbwidth + mbx]);
    *out = v.frame;
    memset(&v.frame, 0, sizeof v.frame);
    ok = 1;
done:
    free(v.macroblocks);
    free(v.top);
    free(v.top_border_y);
    free(v.left_border_y);
    free(v.top_border_u);
    free(v.top_border_v);
    free(v.left_border_u);
    free(v.left_border_v);
    free((void *)v.b.data);
    for (unsigned i = 0; i < v.num_partitions && i < 8; i++) free((void *)v.partitions[i].data);
    vfree_frame(&v.frame);
    return ok;
}

static int wdecode_vp8l(const unsigned char *payload, size_t chunk_length, int implicit,
        unsigned implicit_width, unsigned implicit_height, unsigned *out_width,
        unsigned *out_height, unsigned char **out_pixels, int *out_alpha) {
    struct wstate s;
    memset(&s, 0, sizeof(s));
    s.r.data = payload;
    s.r.length = chunk_length;
    s.r.pos = 0;
    int ok = 1;
    unsigned width, height, alpha_used = 0;
    if (implicit) {
        width = implicit_width;
        height = implicit_height;
    } else {
        unsigned signature = wread_bits(&s.r, 8, &ok);
        unsigned hw = wread_bits(&s.r, 14, &ok) + 1;
        unsigned hh = wread_bits(&s.r, 14, &ok) + 1;
        alpha_used = wread_bits(&s.r, 1, &ok);
        unsigned version = wread_bits(&s.r, 3, &ok);
        if (!ok || signature != 0x2f || version) return WEBP_DECODE_ERROR;
        width = hw;
        height = hh;
    }
    if (!width || !height || width > 16384 || height > 16384) return WEBP_DECODE_ERROR;
    size_t pixels = (size_t)width * height;
    if (pixels > 268435456 / 4) return WEBP_DECODE_ERROR;
    unsigned char *rgba = calloc(pixels, 4);
    if (!rgba) return WEBP_DECODE_ERROR;
    s.width = width;
    s.height = height;
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
            wapply_color_indexing(rgba, image_width, s.height, s.transform_table_size[3],
                s.transform_data[3]);
        }
    }
    for (int i = 0; i < 4; i++) free(s.transform_data[i]);
    *out_width = width;
    *out_height = height;
    *out_pixels = rgba;
    *out_alpha = (int)alpha_used;
    return WEBP_DECODE_OK;
}

static unsigned char walpha_predictor(unsigned x, unsigned y, unsigned width, unsigned method,
        const unsigned char *image) {
    if (method == 0) return 0;
    if (method == 1) {
        if (x == 0 && y == 0) return 0;
        if (x == 0) return image[((size_t)(y - 1) * width + x) * 4 + 3];
        return image[((size_t)y * width + x - 1) * 4 + 3];
    }
    if (method == 2) {
        if (x == 0 && y == 0) return 0;
        if (y == 0) return image[((size_t)y * width + x - 1) * 4 + 3];
        return image[((size_t)(y - 1) * width + x) * 4 + 3];
    }
    int left, top, top_left;
    if (x == 0 && y == 0) {
        left = top = top_left = 0;
    } else if (x == 0) {
        int val = image[((size_t)(y - 1) * width + x) * 4 + 3];
        left = top = top_left = val;
    } else if (y == 0) {
        int val = image[((size_t)y * width + x - 1) * 4 + 3];
        left = top = top_left = val;
    } else {
        left = image[((size_t)y * width + x - 1) * 4 + 3];
        top = image[((size_t)(y - 1) * width + x) * 4 + 3];
        top_left = image[((size_t)(y - 1) * width + x - 1) * 4 + 3];
    }
    int combination = left + top - top_left;
    return (unsigned char)(combination < 0 ? 0 : combination > 255 ? 255 : combination);
}

enum webp_status tomoe_webp_decode(const unsigned char *data, size_t length,
        struct webp_decoded *image) {
    image->width = image->height = 0;
    image->bytes = 0;
    image->pixels = NULL;
    image->has_alpha = 0;
    if (!data || length < 21) return WEBP_DECODE_ERROR;
    if (memcmp(data, "RIFF", 4) || memcmp(data + 8, "WEBP", 4)) return WEBP_DECODE_ERROR;
    const unsigned char *vp8l = NULL, *vp8 = NULL, *alph = NULL;
    size_t vp8l_length = 0, vp8_length = 0, alph_length = 0;
    int extended = 0, alpha_flag = 0;
    size_t offset = 12;
    while (offset + 8 <= length) {
        unsigned chunk_length = (unsigned)data[offset + 4] | ((unsigned)data[offset + 5] << 8) |
            ((unsigned)data[offset + 6] << 16) | ((unsigned)data[offset + 7] << 24);
        if ((size_t)chunk_length > length - offset - 8) return WEBP_DECODE_ERROR;
        const unsigned char *payload = data + offset + 8;
        if (!memcmp(data + offset, "VP8L", 4)) {
            vp8l = payload;
            vp8l_length = chunk_length;
        } else if (!memcmp(data + offset, "VP8 ", 4)) {
            vp8 = payload;
            vp8_length = chunk_length;
        } else if (!memcmp(data + offset, "VP8X", 4)) {
            if (chunk_length < 10) return WEBP_DECODE_ERROR;
            alpha_flag = (payload[0] & 0x10) != 0;
            extended = 1;
        } else if (!memcmp(data + offset, "ALPH", 4)) {
            alph = payload;
            alph_length = chunk_length;
        }
        offset += 8 + chunk_length + (chunk_length & 1);
    }
    if (vp8l) {
        unsigned width, height;
        unsigned char *pixels;
        int has_alpha;
        if (wdecode_vp8l(vp8l, vp8l_length, 0, 0, 0, &width, &height, &pixels, &has_alpha) !=
                WEBP_DECODE_OK) return WEBP_DECODE_ERROR;
        image->width = width;
        image->height = height;
        image->bytes = (uint64_t)width * height * 4;
        image->pixels = pixels;
        image->has_alpha = has_alpha;
        return WEBP_DECODE_OK;
    }
    if (!vp8) return WEBP_DECODE_ERROR;
    if (vp8_length < 10) return WEBP_DECODE_ERROR;
    if (vp8[3] != 0x9d || vp8[4] != 0x01 || vp8[5] != 0x2a) return WEBP_DECODE_ERROR;
    unsigned width = ((unsigned)vp8[6] | ((unsigned)vp8[7] << 8)) & 0x3fff;
    unsigned height = ((unsigned)vp8[8] | ((unsigned)vp8[9] << 8)) & 0x3fff;
    if (!width || !height) return WEBP_DECODE_ERROR;
    size_t pixels_count = (size_t)width * height;
    if (pixels_count > 268435456 / 4) return WEBP_DECODE_ERROR;
    struct vframe frame;
    if (!vdecode(vp8, vp8_length, &frame)) return WEBP_DECODE_ERROR;
    if (frame.width != width || frame.height != height) {
        vfree_frame(&frame);
        return WEBP_DECODE_ERROR;
    }
    int has_alpha = extended && alpha_flag;
    unsigned char *pixels = malloc(pixels_count * 4);
    if (!pixels) {
        vfree_frame(&frame);
        return WEBP_DECODE_ERROR;
    }
    vfill_rgba(&frame, pixels, has_alpha);
    vfree_frame(&frame);
    if (has_alpha) {
        if (!alph || alph_length < 1) {
            free(pixels);
            return WEBP_DECODE_ERROR;
        }
        unsigned info = alph[0];
        unsigned preprocessing = (info >> 4) & 3;
        unsigned filtering = (info >> 2) & 3;
        unsigned compression = info & 3;
        if (preprocessing > 1 || compression > 1) {
            free(pixels);
            return WEBP_DECODE_ERROR;
        }
        unsigned char *green = malloc(pixels_count);
        if (!green) {
            free(pixels);
            return WEBP_DECODE_ERROR;
        }
        if (compression) {
            unsigned aw, ah;
            unsigned char *argba;
            int aalpha;
            if (wdecode_vp8l(alph + 1, alph_length - 1, 1, width, height, &aw, &ah, &argba,
                    &aalpha) != WEBP_DECODE_OK) {
                free(green);
                free(pixels);
                return WEBP_DECODE_ERROR;
            }
            if (aw != width || ah != height) {
                free(argba);
                free(green);
                free(pixels);
                return WEBP_DECODE_ERROR;
            }
            for (size_t i = 0; i < pixels_count; i++) green[i] = argba[i * 4 + 1];
            free(argba);
        } else {
            if (alph_length - 1 < pixels_count) {
                free(green);
                free(pixels);
                return WEBP_DECODE_ERROR;
            }
            memcpy(green, alph + 1, pixels_count);
        }
        for (unsigned y = 0; y < height; y++) {
            for (unsigned x = 0; x < width; x++) {
                size_t index = (size_t)y * width + x;
                unsigned char predictor = walpha_predictor(x, y, width, filtering, pixels);
                pixels[index * 4 + 3] = (unsigned char)(predictor + green[index]);
            }
        }
        free(green);
    }
    image->width = width;
    image->height = height;
    image->bytes = (uint64_t)pixels_count * 4;
    image->pixels = pixels;
    image->has_alpha = has_alpha;
    return WEBP_DECODE_OK;
}
