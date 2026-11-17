#include "png.h"

#include <stdlib.h>
#include <string.h>

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


static void put32(unsigned char *bytes, uint32_t value) {
    bytes[0] = (unsigned char)(value >> 24);
    bytes[1] = (unsigned char)(value >> 16);
    bytes[2] = (unsigned char)(value >> 8);
    bytes[3] = (unsigned char)value;
}

enum { INF_RES_OK = 0, INF_RES_END = 1, INF_RES_BAD = -1, INF_RES_MEM = -2 };

enum inf_state {
    INF_HEAD_CMF, INF_HEAD_FLG, INF_BLOCK,
    INF_STORED_LEN, INF_STORED_COPY,
    INF_DYN_HDR, INF_DYN_CODELEN, INF_DYN_LENS, INF_DYN_REP,
    INF_LITLEN, INF_LEN_EXTRA, INF_DIST, INF_DIST_EXTRA, INF_MATCH_COPY,
    INF_TRAILER, INF_DONE, INF_BAD
};

enum huff_kind { HUFF_CODES, HUFF_LENS, HUFF_DISTS };

struct png_huff {
    uint16_t count[16];
    uint16_t syms[288];
    uint16_t fast[512];
    unsigned max_len;
};

struct png_inflate {
    enum inf_state state;
    uint64_t bitbuf;
    unsigned bitcnt;
    unsigned bfinal;
    unsigned wsize, whave, wpos;
    uint32_t adler;
    unsigned copy_len, copy_dist;
    unsigned len_sym, dist_sym;
    unsigned nlen, ndist, ncode;
    unsigned dyn_idx, dyn_sym;
    unsigned char code_lens[19];
    unsigned char sym_lens[316];
    struct png_huff code_huff;
    struct png_huff lit_huff;
    struct png_huff dist_huff;
    unsigned char window[32768];
};

static const uint32_t crc32_table[256] = {
    0x00000000u, 0x77073096u, 0xee0e612cu, 0x990951bau, 0x076dc419u, 0x706af48fu, 0xe963a535u, 0x9e6495a3u,
    0x0edb8832u, 0x79dcb8a4u, 0xe0d5e91eu, 0x97d2d988u, 0x09b64c2bu, 0x7eb17cbdu, 0xe7b82d07u, 0x90bf1d91u,
    0x1db71064u, 0x6ab020f2u, 0xf3b97148u, 0x84be41deu, 0x1adad47du, 0x6ddde4ebu, 0xf4d4b551u, 0x83d385c7u,
    0x136c9856u, 0x646ba8c0u, 0xfd62f97au, 0x8a65c9ecu, 0x14015c4fu, 0x63066cd9u, 0xfa0f3d63u, 0x8d080df5u,
    0x3b6e20c8u, 0x4c69105eu, 0xd56041e4u, 0xa2677172u, 0x3c03e4d1u, 0x4b04d447u, 0xd20d85fdu, 0xa50ab56bu,
    0x35b5a8fau, 0x42b2986cu, 0xdbbbc9d6u, 0xacbcf940u, 0x32d86ce3u, 0x45df5c75u, 0xdcd60dcfu, 0xabd13d59u,
    0x26d930acu, 0x51de003au, 0xc8d75180u, 0xbfd06116u, 0x21b4f4b5u, 0x56b3c423u, 0xcfba9599u, 0xb8bda50fu,
    0x2802b89eu, 0x5f058808u, 0xc60cd9b2u, 0xb10be924u, 0x2f6f7c87u, 0x58684c11u, 0xc1611dabu, 0xb6662d3du,
    0x76dc4190u, 0x01db7106u, 0x98d220bcu, 0xefd5102au, 0x71b18589u, 0x06b6b51fu, 0x9fbfe4a5u, 0xe8b8d433u,
    0x7807c9a2u, 0x0f00f934u, 0x9609a88eu, 0xe10e9818u, 0x7f6a0dbbu, 0x086d3d2du, 0x91646c97u, 0xe6635c01u,
    0x6b6b51f4u, 0x1c6c6162u, 0x856530d8u, 0xf262004eu, 0x6c0695edu, 0x1b01a57bu, 0x8208f4c1u, 0xf50fc457u,
    0x65b0d9c6u, 0x12b7e950u, 0x8bbeb8eau, 0xfcb9887cu, 0x62dd1ddfu, 0x15da2d49u, 0x8cd37cf3u, 0xfbd44c65u,
    0x4db26158u, 0x3ab551ceu, 0xa3bc0074u, 0xd4bb30e2u, 0x4adfa541u, 0x3dd895d7u, 0xa4d1c46du, 0xd3d6f4fbu,
    0x4369e96au, 0x346ed9fcu, 0xad678846u, 0xda60b8d0u, 0x44042d73u, 0x33031de5u, 0xaa0a4c5fu, 0xdd0d7cc9u,
    0x5005713cu, 0x270241aau, 0xbe0b1010u, 0xc90c2086u, 0x5768b525u, 0x206f85b3u, 0xb966d409u, 0xce61e49fu,
    0x5edef90eu, 0x29d9c998u, 0xb0d09822u, 0xc7d7a8b4u, 0x59b33d17u, 0x2eb40d81u, 0xb7bd5c3bu, 0xc0ba6cadu,
    0xedb88320u, 0x9abfb3b6u, 0x03b6e20cu, 0x74b1d29au, 0xead54739u, 0x9dd277afu, 0x04db2615u, 0x73dc1683u,
    0xe3630b12u, 0x94643b84u, 0x0d6d6a3eu, 0x7a6a5aa8u, 0xe40ecf0bu, 0x9309ff9du, 0x0a00ae27u, 0x7d079eb1u,
    0xf00f9344u, 0x8708a3d2u, 0x1e01f268u, 0x6906c2feu, 0xf762575du, 0x806567cbu, 0x196c3671u, 0x6e6b06e7u,
    0xfed41b76u, 0x89d32be0u, 0x10da7a5au, 0x67dd4accu, 0xf9b9df6fu, 0x8ebeeff9u, 0x17b7be43u, 0x60b08ed5u,
    0xd6d6a3e8u, 0xa1d1937eu, 0x38d8c2c4u, 0x4fdff252u, 0xd1bb67f1u, 0xa6bc5767u, 0x3fb506ddu, 0x48b2364bu,
    0xd80d2bdau, 0xaf0a1b4cu, 0x36034af6u, 0x41047a60u, 0xdf60efc3u, 0xa867df55u, 0x316e8eefu, 0x4669be79u,
    0xcb61b38cu, 0xbc66831au, 0x256fd2a0u, 0x5268e236u, 0xcc0c7795u, 0xbb0b4703u, 0x220216b9u, 0x5505262fu,
    0xc5ba3bbeu, 0xb2bd0b28u, 0x2bb45a92u, 0x5cb36a04u, 0xc2d7ffa7u, 0xb5d0cf31u, 0x2cd99e8bu, 0x5bdeae1du,
    0x9b64c2b0u, 0xec63f226u, 0x756aa39cu, 0x026d930au, 0x9c0906a9u, 0xeb0e363fu, 0x72076785u, 0x05005713u,
    0x95bf4a82u, 0xe2b87a14u, 0x7bb12baeu, 0x0cb61b38u, 0x92d28e9bu, 0xe5d5be0du, 0x7cdcefb7u, 0x0bdbdf21u,
    0x86d3d2d4u, 0xf1d4e242u, 0x68ddb3f8u, 0x1fda836eu, 0x81be16cdu, 0xf6b9265bu, 0x6fb077e1u, 0x18b74777u,
    0x88085ae6u, 0xff0f6a70u, 0x66063bcau, 0x11010b5cu, 0x8f659effu, 0xf862ae69u, 0x616bffd3u, 0x166ccf45u,
    0xa00ae278u, 0xd70dd2eeu, 0x4e048354u, 0x3903b3c2u, 0xa7672661u, 0xd06016f7u, 0x4969474du, 0x3e6e77dbu,
    0xaed16a4au, 0xd9d65adcu, 0x40df0b66u, 0x37d83bf0u, 0xa9bcae53u, 0xdebb9ec5u, 0x47b2cf7fu, 0x30b5ffe9u,
    0xbdbdf21cu, 0xcabac28au, 0x53b39330u, 0x24b4a3a6u, 0xbad03605u, 0xcdd70693u, 0x54de5729u, 0x23d967bfu,
    0xb3667a2eu, 0xc4614ab8u, 0x5d681b02u, 0x2a6f2b94u, 0xb40bbe37u, 0xc30c8ea1u, 0x5a05df1bu, 0x2d02ef8du,
};

uint32_t tomoe_png_crc32(uint32_t crc, const unsigned char *data, size_t length) {
    if (!data) return 0;
    crc = ~crc;
    for (size_t i = 0; i < length; i++)
        crc = (crc >> 8) ^ crc32_table[(crc ^ data[i]) & 0xffu];
    return ~crc;
}

static uint32_t png_adler32(uint32_t adler, const unsigned char *data, size_t length) {
    uint32_t s1 = adler & 0xffffu;
    uint32_t s2 = (adler >> 16) & 0xffffu;
    while (length) {
        size_t chunk = length > 5552 ? 5552 : length;
        length -= chunk;
        while (chunk--) {
            s1 += *data++;
            s2 += s1;
        }
        s1 %= 65521u;
        s2 %= 65521u;
    }
    return (s2 << 16) | s1;
}

static int fill_bits(struct png_inflate *s, unsigned need,
        const unsigned char **in, size_t *avail) {
    while (s->bitcnt < need) {
        if (!*avail) return 0;
        s->bitbuf |= (uint64_t)(*(*in)++) << s->bitcnt;
        (*avail)--;
        s->bitcnt += 8;
    }
    return 1;
}

static void align_bits(struct png_inflate *s) {
    unsigned drop = s->bitcnt & 7u;
    s->bitbuf >>= drop;
    s->bitcnt -= drop;
}

static int huff_build(struct png_huff *h, const unsigned char *lens, unsigned n,
        enum huff_kind kind) {
    uint16_t offs[16];
    int left = 1;
    unsigned max_len = 0;
    memset(h->count, 0, sizeof(h->count));
    for (unsigned i = 0; i < n; i++) {
        if (lens[i] > 15) return -1;
        h->count[lens[i]]++;
        if (lens[i] > max_len) max_len = lens[i];
    }
    h->max_len = max_len;
    if (max_len == 0) {
        memset(h->fast, 0, sizeof(h->fast));
        return kind == HUFF_DISTS ? 0 : -1;
    }
    for (unsigned len = 1; len <= 15; len++) {
        left <<= 1;
        left -= h->count[len];
        if (left < 0) return -1;
    }
    if (left > 0 && (kind == HUFF_CODES || max_len != 1)) return -1;
    offs[1] = 0;
    for (unsigned len = 1; len < 15; len++)
        offs[len + 1] = offs[len] + h->count[len];
    for (unsigned i = 0; i < n; i++)
        if (lens[i]) h->syms[offs[lens[i]]++] = (uint16_t)i;
    memset(h->fast, 0, sizeof(h->fast));
    unsigned idx = 0, code = 0;
    for (unsigned len = 1; len <= 9 && len <= max_len; len++) {
        for (uint16_t i = 0; i < h->count[len]; i++) {
            uint16_t sym = h->syms[idx++];
            unsigned rev = 0;
            for (unsigned b = 0; b < len; b++)
                if (code & (1u << (len - 1 - b))) rev |= 1u << b;
            uint16_t entry = (uint16_t)((len << 10) | sym);
            for (unsigned fill = rev; fill < 512; fill += (1u << len))
                h->fast[fill] = entry;
            code++;
        }
        code <<= 1;
    }
    return 0;
}

static int huff_decode(struct png_inflate *s, const struct png_huff *h,
        const unsigned char **in, size_t *avail) {
    if (!h->max_len) return -2;
    if (s->bitcnt < 9) fill_bits(s, 9, in, avail);
    if (s->bitcnt >= 9) {
        uint16_t entry = h->fast[s->bitbuf & 511u];
        if (entry) {
            unsigned len = entry >> 10;
            s->bitbuf >>= len;
            s->bitcnt -= len;
            return entry & 0x3ffu;
        }
    }
    unsigned code = 0;
    int first = 0, index = 0;
    for (unsigned len = 1; len <= h->max_len; len++) {
        if (s->bitcnt < len && !fill_bits(s, len, in, avail)) return -1;
        code = (code << 1) | (unsigned)((s->bitbuf >> (len - 1)) & 1u);
        int count = h->count[len];
        if ((int)code - count < first) {
            s->bitbuf >>= len;
            s->bitcnt -= len;
            return h->syms[index + ((int)code - first)];
        }
        index += count;
        first = (first + count) << 1;
    }
    return -2;
}

static void huff_build_fixed(struct png_huff *lit, struct png_huff *dist) {
    unsigned char lens[288];
    unsigned i = 0;
    for (; i < 144; i++) lens[i] = 8;
    for (; i < 256; i++) lens[i] = 9;
    for (; i < 280; i++) lens[i] = 7;
    for (; i < 288; i++) lens[i] = 8;
    huff_build(lit, lens, 288, HUFF_LENS);
    memset(lens, 5, 32);
    huff_build(dist, lens, 32, HUFF_DISTS);
}

static struct png_inflate *png_inflate_create(void) {
    struct png_inflate *s = calloc(1, sizeof(*s));
    if (!s) return NULL;
    s->state = INF_HEAD_CMF;
    s->adler = 1;
    return s;
}

static void png_inflate_destroy(struct png_inflate *s) {
    free(s);
}

static int png_inflate_step(struct png_inflate *s, const unsigned char **in_ptr,
        size_t *in_avail_ptr, unsigned char **out_ptr, size_t *out_avail_ptr) {
    static const unsigned char order[19] = {
        16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15
    };
    static const uint16_t len_base[29] = {
        3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
        35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258
    };
    static const unsigned char len_extra[29] = {
        0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
        3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0
    };
    static const uint16_t dist_base[30] = {
        1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
        257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577
    };
    static const unsigned char dist_extra[30] = {
        0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
        7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13
    };
    const unsigned char *in = *in_ptr;
    size_t in_avail = *in_avail_ptr;
    unsigned char *out = *out_ptr;
    unsigned char *out_start = out;
    size_t out_avail = *out_avail_ptr;
    int status = INF_RES_OK;

    for (;;) {
        if (s->state == INF_DONE) { status = INF_RES_END; break; }
        if (s->state == INF_BAD) { status = INF_RES_BAD; break; }
        switch (s->state) {
        case INF_HEAD_CMF:
            if (!in_avail) goto pause;
            {
                unsigned cmf = *in++;
                in_avail--;
                if ((cmf & 0x0fu) != 8u || (cmf >> 4) > 7u) { s->state = INF_BAD; break; }
                s->wsize = 1u << ((cmf >> 4) + 8u);
                s->copy_len = cmf;
                s->state = INF_HEAD_FLG;
            }
            break;
        case INF_HEAD_FLG:
            if (!in_avail) goto pause;
            {
                unsigned flg = *in++;
                in_avail--;
                if (((s->copy_len << 8) + flg) % 31u != 0u || (flg & 0x20u)) {
                    s->state = INF_BAD;
                    break;
                }
                s->copy_len = 0;
                s->state = INF_BLOCK;
            }
            break;
        case INF_BLOCK:
            if (!fill_bits(s, 3, &in, &in_avail)) goto pause;
            s->bfinal = (unsigned)(s->bitbuf & 1u);
            {
                unsigned btype = (unsigned)((s->bitbuf >> 1) & 3u);
                s->bitbuf >>= 3;
                s->bitcnt -= 3;
                if (btype == 0u) {
                    align_bits(s);
                    s->state = INF_STORED_LEN;
                } else if (btype == 1u) {
                    huff_build_fixed(&s->lit_huff, &s->dist_huff);
                    s->state = INF_LITLEN;
                } else if (btype == 2u) {
                    s->state = INF_DYN_HDR;
                } else {
                    s->state = INF_BAD;
                }
            }
            break;
        case INF_STORED_LEN:
            if (!fill_bits(s, 32, &in, &in_avail)) goto pause;
            {
                unsigned len = (unsigned)(s->bitbuf & 0xffffu);
                unsigned nlen = (unsigned)((s->bitbuf >> 16) & 0xffffu);
                s->bitbuf >>= 32;
                s->bitcnt -= 32;
                if ((len ^ 0xffffu) != nlen) { s->state = INF_BAD; break; }
                s->copy_len = len;
                s->state = len ? INF_STORED_COPY : (s->bfinal ? INF_TRAILER : INF_BLOCK);
            }
            break;
        case INF_STORED_COPY:
            if (!out_avail) goto pause;
            while (s->copy_len && out_avail && s->bitcnt >= 8) {
                unsigned char b = (unsigned char)(s->bitbuf & 0xffu);
                s->bitbuf >>= 8;
                s->bitcnt -= 8;
                *out++ = b;
                out_avail--;
                s->window[s->wpos] = b;
                s->wpos = (s->wpos + 1u) & (s->wsize - 1u);
                if (s->whave < s->wsize) s->whave++;
                s->copy_len--;
            }
            if (s->copy_len && out_avail) {
                size_t take = s->copy_len;
                if (take > in_avail) take = in_avail;
                if (take > out_avail) take = out_avail;
                if (!take) goto pause;
                memcpy(out, in, take);
                for (size_t i = 0; i < take; i++) {
                    s->window[s->wpos] = in[i];
                    s->wpos = (s->wpos + 1u) & (s->wsize - 1u);
                }
                if (s->whave < s->wsize) {
                    size_t add = s->wsize - s->whave;
                    s->whave += (unsigned)(take < add ? take : add);
                }
                in += take;
                in_avail -= take;
                out += take;
                out_avail -= take;
                s->copy_len -= (unsigned)take;
            }
            if (!s->copy_len) s->state = s->bfinal ? INF_TRAILER : INF_BLOCK;
            else if (!out_avail) goto pause;
            break;
        case INF_DYN_HDR:
            if (!fill_bits(s, 14, &in, &in_avail)) goto pause;
            s->nlen = (unsigned)(s->bitbuf & 31u) + 257u;
            s->ndist = (unsigned)((s->bitbuf >> 5) & 31u) + 1u;
            s->ncode = (unsigned)((s->bitbuf >> 10) & 15u) + 4u;
            s->bitbuf >>= 14;
            s->bitcnt -= 14;
            if (s->nlen > 286u || s->ndist > 30u) { s->state = INF_BAD; break; }
            memset(s->code_lens, 0, sizeof(s->code_lens));
            s->dyn_idx = 0;
            s->state = INF_DYN_CODELEN;
            break;
        case INF_DYN_CODELEN:
            while (s->dyn_idx < s->ncode) {
                if (!fill_bits(s, 3, &in, &in_avail)) goto pause;
                s->code_lens[order[s->dyn_idx++]] = (unsigned char)(s->bitbuf & 7u);
                s->bitbuf >>= 3;
                s->bitcnt -= 3;
            }
            if (huff_build(&s->code_huff, s->code_lens, 19, HUFF_CODES)) {
                s->state = INF_BAD;
                break;
            }
            s->dyn_idx = 0;
            s->state = INF_DYN_LENS;
            break;
        case INF_DYN_LENS:
            while (s->dyn_idx < s->nlen + s->ndist) {
                int sym = huff_decode(s, &s->code_huff, &in, &in_avail);
                if (sym == -1) goto pause;
                if (sym < 0) { s->state = INF_BAD; break; }
                if (sym < 16) {
                    s->sym_lens[s->dyn_idx++] = (unsigned char)sym;
                } else {
                    s->dyn_sym = (unsigned)sym;
                    s->state = INF_DYN_REP;
                    break;
                }
            }
            if (s->state != INF_DYN_LENS) break;
            if (!s->sym_lens[256] ||
                    huff_build(&s->lit_huff, s->sym_lens, s->nlen, HUFF_LENS) ||
                    huff_build(&s->dist_huff, s->sym_lens + s->nlen, s->ndist, HUFF_DISTS)) {
                s->state = INF_BAD;
                break;
            }
            s->state = INF_LITLEN;
            break;
        case INF_DYN_REP:
            {
                unsigned need = s->dyn_sym == 16 ? 2u : s->dyn_sym == 17 ? 3u : 7u;
                unsigned base = s->dyn_sym == 18 ? 11u : 3u;
                if (!fill_bits(s, need, &in, &in_avail)) goto pause;
                unsigned count = base + (unsigned)(s->bitbuf & ((1u << need) - 1u));
                s->bitbuf >>= need;
                s->bitcnt -= need;
                unsigned char val = 0;
                if (s->dyn_sym == 16) {
                    if (!s->dyn_idx) { s->state = INF_BAD; break; }
                    val = s->sym_lens[s->dyn_idx - 1u];
                }
                if (s->dyn_idx + count > s->nlen + s->ndist) { s->state = INF_BAD; break; }
                while (count--) s->sym_lens[s->dyn_idx++] = val;
                s->state = INF_DYN_LENS;
            }
            break;
        case INF_LITLEN:
            while (out_avail) {
                int sym = huff_decode(s, &s->lit_huff, &in, &in_avail);
                if (sym == -1) goto pause;
                if (sym < 0 || sym > 285) { s->state = INF_BAD; break; }
                if (sym < 256) {
                    unsigned char b = (unsigned char)sym;
                    *out++ = b;
                    out_avail--;
                    s->window[s->wpos] = b;
                    s->wpos = (s->wpos + 1u) & (s->wsize - 1u);
                    if (s->whave < s->wsize) s->whave++;
                } else if (sym == 256) {
                    s->state = s->bfinal ? INF_TRAILER : INF_BLOCK;
                    break;
                } else {
                    s->len_sym = (unsigned)(sym - 257);
                    s->state = INF_LEN_EXTRA;
                    break;
                }
            }
            if (s->state == INF_LITLEN && !out_avail) goto pause;
            break;
        case INF_LEN_EXTRA:
            {
                unsigned eb = len_extra[s->len_sym];
                if (eb && !fill_bits(s, eb, &in, &in_avail)) goto pause;
                s->copy_len = len_base[s->len_sym] +
                    (eb ? (unsigned)(s->bitbuf & ((1u << eb) - 1u)) : 0u);
                s->bitbuf >>= eb;
                s->bitcnt -= eb;
                s->state = INF_DIST;
            }
            break;
        case INF_DIST:
            {
                int dsym = huff_decode(s, &s->dist_huff, &in, &in_avail);
                if (dsym == -1) goto pause;
                if (dsym < 0 || dsym >= 30) { s->state = INF_BAD; break; }
                s->dist_sym = (unsigned)dsym;
                s->state = INF_DIST_EXTRA;
            }
            break;
        case INF_DIST_EXTRA:
            {
                unsigned eb = dist_extra[s->dist_sym];
                if (eb && !fill_bits(s, eb, &in, &in_avail)) goto pause;
                s->copy_dist = dist_base[s->dist_sym] +
                    (eb ? (unsigned)(s->bitbuf & ((1u << eb) - 1u)) : 0u);
                s->bitbuf >>= eb;
                s->bitcnt -= eb;
                if (!s->copy_dist || s->copy_dist > s->whave || s->copy_dist > s->wsize) {
                    s->state = INF_BAD;
                    break;
                }
                s->state = INF_MATCH_COPY;
            }
            break;
        case INF_MATCH_COPY:
            if (!out_avail) goto pause;
            {
                unsigned mask = s->wsize - 1u;
                while (s->copy_len && out_avail) {
                    unsigned char b = s->window[(s->wpos - s->copy_dist) & mask];
                    s->window[s->wpos] = b;
                    s->wpos = (s->wpos + 1u) & mask;
                    if (s->whave < s->wsize) s->whave++;
                    *out++ = b;
                    out_avail--;
                    s->copy_len--;
                }
                if (!s->copy_len) s->state = INF_LITLEN;
                else goto pause;
            }
            break;
        case INF_TRAILER:
            if (out > out_start) {
                s->adler = png_adler32(s->adler, out_start, (size_t)(out - out_start));
                out_start = out;
            }
            align_bits(s);
            if (!fill_bits(s, 32, &in, &in_avail)) goto pause;
            {
                uint32_t expected =
                    ((uint32_t)(s->bitbuf & 0xffu) << 24) |
                    ((uint32_t)((s->bitbuf >> 8) & 0xffu) << 16) |
                    ((uint32_t)((s->bitbuf >> 16) & 0xffu) << 8) |
                    (uint32_t)((s->bitbuf >> 24) & 0xffu);
                s->bitbuf >>= 32;
                s->bitcnt -= 32;
                if (expected != s->adler) { s->state = INF_BAD; break; }
                s->state = INF_DONE;
            }
            break;
        case INF_DONE:
        case INF_BAD:
            break;
        }
    }

pause:
    if (out > out_start)
        s->adler = png_adler32(s->adler, out_start, (size_t)(out - out_start));
    *in_ptr = in;
    *in_avail_ptr = in_avail;
    *out_ptr = out;
    *out_avail_ptr = out_avail;
    return status;
}

enum png_status tomoe_png_inflate(const unsigned char *in, size_t in_len,
        unsigned char **out, size_t *out_len) {
    struct png_inflate *s = png_inflate_create();
    size_t cap = in_len < 1024 ? 4096 : in_len * 4;
    if (cap < 4096) cap = 4096;
    unsigned char *buf = malloc(cap);
    size_t len = 0;
    if (!s || !buf) {
        png_inflate_destroy(s);
        free(buf);
        return PNG_HARD;
    }
    const unsigned char *in_cur = in;
    size_t in_rem = in_len;
    for (;;) {
        unsigned char *out_cur = buf + len;
        size_t out_rem = cap - len;
        int res = png_inflate_step(s, &in_cur, &in_rem, &out_cur, &out_rem);
        len = (size_t)(out_cur - buf);
        if (res == INF_RES_END) {
            png_inflate_destroy(s);
            *out = buf;
            *out_len = len;
            return PNG_OK;
        }
        if (res != INF_RES_OK) {
            png_inflate_destroy(s);
            free(buf);
            return PNG_BAD;
        }
        if (out_rem == 0) {
            size_t next_cap = cap * 2;
            if (next_cap < cap) {
                png_inflate_destroy(s);
                free(buf);
                return PNG_HARD;
            }
            unsigned char *grown = realloc(buf, next_cap);
            if (!grown) {
                png_inflate_destroy(s);
                free(buf);
                return PNG_HARD;
            }
            buf = grown;
            cap = next_cap;
        } else {
            png_inflate_destroy(s);
            free(buf);
            return PNG_BAD;
        }
    }
}

#define DEF_MIN_MATCH 3u
#define DEF_MAX_MATCH 258u
#define DEF_MIN_LOOKAHEAD 262u
#define DEF_WIN_INIT 258u
#define DEF_LENGTH_CODES 29
#define DEF_LITERALS 256
#define DEF_L_CODES 286
#define DEF_D_CODES 30
#define DEF_BL_CODES 19
#define DEF_HEAP_SIZE 573
#define DEF_MAX_BITS 15
#define DEF_MAX_BL_BITS 7
#define DEF_END_BLOCK 256
#define DEF_LIT_BUFSIZE 16384u
#define DEF_HASH_SIZE 32768u
#define DEF_HASH_MASK 32767u
#define DEF_HASH_SHIFT 5u
#define DEF_TOO_FAR 4096u

struct def_ct {
    union { uint16_t freq, code; } fc;
    union { uint16_t dad, len; } dl;
};

struct def_static_desc {
    const struct def_ct *static_tree;
    const int *extra_bits;
    int extra_base;
    int elems;
    int max_length;
};

struct def_tree_desc {
    struct def_ct *dyn_tree;
    int max_code;
    const struct def_static_desc *stat_desc;
};

struct png_deflate {
    const unsigned char *in;
    size_t in_len, in_pos;
    unsigned char *out;
    size_t out_len, out_cap;
    unsigned w_bits, w_size, w_mask;
    size_t window_size, high_water;
    unsigned char *window;
    uint16_t *prev, *head;
    unsigned ins_h;
    int64_t block_start;
    unsigned strstart, match_start, lookahead, insert;
    unsigned match_length, prev_length, prev_match;
    int match_available, filtered;
    unsigned char *pending_buf, *sym_buf;
    size_t pending;
    unsigned sym_next, sym_end;
    uint16_t bi_buf;
    int bi_valid;
    struct def_ct dyn_ltree[DEF_HEAP_SIZE];
    struct def_ct dyn_dtree[2 * DEF_D_CODES + 1];
    struct def_ct bl_tree[2 * DEF_BL_CODES + 1];
    struct def_ct static_ltree[DEF_L_CODES + 2];
    struct def_ct static_dtree[DEF_D_CODES];
    unsigned char dist_code[512];
    unsigned char length_code[DEF_MAX_MATCH - DEF_MIN_MATCH + 1];
    int base_length[DEF_LENGTH_CODES];
    int base_dist[DEF_D_CODES];
    struct def_static_desc static_l_desc, static_d_desc, static_bl_desc;
    struct def_tree_desc l_desc, d_desc, bl_desc;
    uint16_t bl_count[DEF_MAX_BITS + 1];
    int heap[DEF_HEAP_SIZE];
    int heap_len, heap_max;
    unsigned char depth[DEF_HEAP_SIZE];
    unsigned long opt_len, static_len;
    uint32_t adler;
};

static const int def_extra_lbits[DEF_LENGTH_CODES] = {
    0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0
};

static const int def_extra_dbits[DEF_D_CODES] = {
    0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13
};

static const int def_extra_blbits[DEF_BL_CODES] = {
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,2,3,7
};

static const unsigned char def_bl_order[DEF_BL_CODES] = {
    16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15
};

static void def_put_byte(struct png_deflate *s, unsigned char c) {
    s->pending_buf[s->pending++] = c;
}

static void def_put_short(struct png_deflate *s, uint16_t w) {
    def_put_byte(s, (unsigned char)(w & 0xffu));
    def_put_byte(s, (unsigned char)(w >> 8));
}

static void def_put_short_msb(struct png_deflate *s, uint16_t w) {
    def_put_byte(s, (unsigned char)(w >> 8));
    def_put_byte(s, (unsigned char)(w & 0xffu));
}

static unsigned def_bi_reverse(unsigned code, int len) {
    unsigned res = 0;
    do {
        res |= code & 1u;
        code >>= 1;
        res <<= 1;
    } while (--len > 0);
    return res >> 1;
}

static void def_send_bits(struct png_deflate *s, int value, int length) {
    unsigned uval = (unsigned)value;
    if (s->bi_valid > 16 - length) {
        s->bi_buf |= (uint16_t)(uval << s->bi_valid);
        def_put_short(s, s->bi_buf);
        s->bi_buf = (uint16_t)(uval >> (16 - s->bi_valid));
        s->bi_valid += length - 16;
    } else {
        s->bi_buf |= (uint16_t)(uval << s->bi_valid);
        s->bi_valid += length;
    }
}

static void def_bi_windup(struct png_deflate *s) {
    if (s->bi_valid > 8) {
        def_put_short(s, s->bi_buf);
    } else if (s->bi_valid > 0) {
        def_put_byte(s, (unsigned char)s->bi_buf);
    }
    s->bi_buf = 0;
    s->bi_valid = 0;
}

static void def_gen_codes(struct def_ct *tree, int max_code, const uint16_t *bl_count) {
    uint16_t next_code[DEF_MAX_BITS + 1];
    unsigned code = 0;
    for (int bits = 1; bits <= DEF_MAX_BITS; bits++) {
        code = (code + bl_count[bits - 1]) << 1;
        next_code[bits] = (uint16_t)code;
    }
    for (int n = 0; n <= max_code; n++) {
        int len = tree[n].dl.len;
        if (len == 0) continue;
        tree[n].fc.code = (uint16_t)def_bi_reverse(next_code[len]++, len);
    }
}

static void def_init_block(struct png_deflate *s) {
    for (int n = 0; n < DEF_L_CODES; n++) s->dyn_ltree[n].fc.freq = 0;
    for (int n = 0; n < DEF_D_CODES; n++) s->dyn_dtree[n].fc.freq = 0;
    for (int n = 0; n < DEF_BL_CODES; n++) s->bl_tree[n].fc.freq = 0;
    s->dyn_ltree[DEF_END_BLOCK].fc.freq = 1;
    s->opt_len = s->static_len = 0;
    s->sym_next = 0;
}

static void def_tr_init(struct png_deflate *s) {
    uint16_t bl_count[DEF_MAX_BITS + 1];
    int length = 0;
    int code = 0;
    for (; code < DEF_LENGTH_CODES - 1; code++) {
        s->base_length[code] = length;
        for (int n = 0; n < (1 << def_extra_lbits[code]); n++)
            s->length_code[length++] = (unsigned char)code;
    }
    s->length_code[length - 1] = (unsigned char)code;
    int dist = 0;
    for (code = 0; code < 16; code++) {
        s->base_dist[code] = dist;
        for (int n = 0; n < (1 << def_extra_dbits[code]); n++)
            s->dist_code[dist++] = (unsigned char)code;
    }
    dist >>= 7;
    for (; code < DEF_D_CODES; code++) {
        s->base_dist[code] = dist << 7;
        for (int n = 0; n < (1 << (def_extra_dbits[code] - 7)); n++)
            s->dist_code[256 + dist++] = (unsigned char)code;
    }
    for (int bits = 0; bits <= DEF_MAX_BITS; bits++) bl_count[bits] = 0;
    int n = 0;
    while (n <= 143) s->static_ltree[n++].dl.len = 8, bl_count[8]++;
    while (n <= 255) s->static_ltree[n++].dl.len = 9, bl_count[9]++;
    while (n <= 279) s->static_ltree[n++].dl.len = 7, bl_count[7]++;
    while (n <= 287) s->static_ltree[n++].dl.len = 8, bl_count[8]++;
    def_gen_codes(s->static_ltree, DEF_L_CODES + 1, bl_count);
    for (n = 0; n < DEF_D_CODES; n++) {
        s->static_dtree[n].dl.len = 5;
        s->static_dtree[n].fc.code = (uint16_t)def_bi_reverse((unsigned)n, 5);
    }
    s->static_l_desc = (struct def_static_desc){
        s->static_ltree, def_extra_lbits, DEF_LITERALS + 1, DEF_L_CODES, DEF_MAX_BITS
    };
    s->static_d_desc = (struct def_static_desc){
        s->static_dtree, def_extra_dbits, 0, DEF_D_CODES, DEF_MAX_BITS
    };
    s->static_bl_desc = (struct def_static_desc){
        NULL, def_extra_blbits, 0, DEF_BL_CODES, DEF_MAX_BL_BITS
    };
    s->l_desc.dyn_tree = s->dyn_ltree;
    s->l_desc.stat_desc = &s->static_l_desc;
    s->d_desc.dyn_tree = s->dyn_dtree;
    s->d_desc.stat_desc = &s->static_d_desc;
    s->bl_desc.dyn_tree = s->bl_tree;
    s->bl_desc.stat_desc = &s->static_bl_desc;
    s->bi_buf = 0;
    s->bi_valid = 0;
    def_init_block(s);
}

static int def_smaller(const struct def_ct *tree, int n, int m, const unsigned char *depth) {
    return tree[n].fc.freq < tree[m].fc.freq ||
        (tree[n].fc.freq == tree[m].fc.freq && depth[n] <= depth[m]);
}

static void def_pqdownheap(struct png_deflate *s, const struct def_ct *tree, int k) {
    int v = s->heap[k];
    int j = k << 1;
    while (j <= s->heap_len) {
        if (j < s->heap_len && def_smaller(tree, s->heap[j + 1], s->heap[j], s->depth))
            j++;
        if (def_smaller(tree, v, s->heap[j], s->depth)) break;
        s->heap[k] = s->heap[j];
        k = j;
        j <<= 1;
    }
    s->heap[k] = v;
}

static void def_gen_bitlen(struct png_deflate *s, struct def_tree_desc *desc) {
    struct def_ct *tree = desc->dyn_tree;
    int max_code = desc->max_code;
    const struct def_ct *stree = desc->stat_desc->static_tree;
    const int *extra = desc->stat_desc->extra_bits;
    int base = desc->stat_desc->extra_base;
    int max_length = desc->stat_desc->max_length;
    int overflow = 0;
    for (int bits = 0; bits <= DEF_MAX_BITS; bits++) s->bl_count[bits] = 0;
    tree[s->heap[s->heap_max]].dl.len = 0;
    int h;
    for (h = s->heap_max + 1; h < DEF_HEAP_SIZE; h++) {
        int n = s->heap[h];
        int bits = tree[tree[n].dl.dad].dl.len + 1;
        if (bits > max_length) bits = max_length, overflow++;
        tree[n].dl.len = (uint16_t)bits;
        if (n > max_code) continue;
        s->bl_count[bits]++;
        int xbits = n >= base ? extra[n - base] : 0;
        uint16_t f = tree[n].fc.freq;
        s->opt_len += (unsigned long)f * (unsigned)(bits + xbits);
        if (stree) s->static_len += (unsigned long)f * (unsigned)(stree[n].dl.len + xbits);
    }
    if (overflow == 0) return;
    do {
        int bits = max_length - 1;
        while (s->bl_count[bits] == 0) bits--;
        s->bl_count[bits]--;
        s->bl_count[bits + 1] += 2;
        s->bl_count[max_length]--;
        overflow -= 2;
    } while (overflow > 0);
    for (int bits = max_length; bits != 0; bits--) {
        int n = s->bl_count[bits];
        while (n != 0) {
            int m = s->heap[--h];
            if (m > max_code) continue;
            if ((unsigned)tree[m].dl.len != (unsigned)bits) {
                s->opt_len += ((unsigned long)bits - tree[m].dl.len) * tree[m].fc.freq;
                tree[m].dl.len = (uint16_t)bits;
            }
            n--;
        }
    }
}

static void def_build_tree(struct png_deflate *s, struct def_tree_desc *desc) {
    struct def_ct *tree = desc->dyn_tree;
    const struct def_ct *stree = desc->stat_desc->static_tree;
    int elems = desc->stat_desc->elems;
    int max_code = -1;
    s->heap_len = 0;
    s->heap_max = DEF_HEAP_SIZE;
    for (int n = 0; n < elems; n++) {
        if (tree[n].fc.freq != 0) {
            s->heap[++(s->heap_len)] = max_code = n;
            s->depth[n] = 0;
        } else {
            tree[n].dl.len = 0;
        }
    }
    while (s->heap_len < 2) {
        int node = s->heap[++(s->heap_len)] = (max_code < 2 ? ++max_code : 0);
        tree[node].fc.freq = 1;
        s->depth[node] = 0;
        s->opt_len--;
        if (stree) s->static_len -= stree[node].dl.len;
    }
    desc->max_code = max_code;
    for (int n = s->heap_len / 2; n >= 1; n--) def_pqdownheap(s, tree, n);
    int node = elems;
    do {
        int n = s->heap[1];
        s->heap[1] = s->heap[s->heap_len--];
        def_pqdownheap(s, tree, 1);
        int m = s->heap[1];
        s->heap[--(s->heap_max)] = n;
        s->heap[--(s->heap_max)] = m;
        tree[node].fc.freq = (uint16_t)(tree[n].fc.freq + tree[m].fc.freq);
        s->depth[node] = (unsigned char)((s->depth[n] >= s->depth[m] ? s->depth[n] : s->depth[m]) + 1);
        tree[n].dl.dad = tree[m].dl.dad = (uint16_t)node;
        s->heap[1] = node++;
        def_pqdownheap(s, tree, 1);
    } while (s->heap_len >= 2);
    s->heap[--(s->heap_max)] = s->heap[1];
    def_gen_bitlen(s, desc);
    def_gen_codes(tree, max_code, s->bl_count);
}

static void def_scan_tree(struct png_deflate *s, struct def_ct *tree, int max_code) {
    int prevlen = -1;
    int nextlen = tree[0].dl.len;
    int count = 0;
    int max_count = 7;
    int min_count = 4;
    if (nextlen == 0) max_count = 138, min_count = 3;
    tree[max_code + 1].dl.len = 0xffffu;
    for (int n = 0; n <= max_code; n++) {
        int curlen = nextlen;
        nextlen = tree[n + 1].dl.len;
        if (++count < max_count && curlen == nextlen) {
            continue;
        } else if (count < min_count) {
            s->bl_tree[curlen].fc.freq += (uint16_t)count;
        } else if (curlen != 0) {
            if (curlen != prevlen) s->bl_tree[curlen].fc.freq++;
            s->bl_tree[16].fc.freq++;
        } else if (count <= 10) {
            s->bl_tree[17].fc.freq++;
        } else {
            s->bl_tree[18].fc.freq++;
        }
        count = 0;
        prevlen = curlen;
        if (nextlen == 0) {
            max_count = 138, min_count = 3;
        } else if (curlen == nextlen) {
            max_count = 6, min_count = 3;
        } else {
            max_count = 7, min_count = 4;
        }
    }
}

static void def_send_tree(struct png_deflate *s, const struct def_ct *tree, int max_code) {
    int prevlen = -1;
    int nextlen = tree[0].dl.len;
    int count = 0;
    int max_count = 7;
    int min_count = 4;
    if (nextlen == 0) max_count = 138, min_count = 3;
    for (int n = 0; n <= max_code; n++) {
        int curlen = nextlen;
        nextlen = tree[n + 1].dl.len;
        if (++count < max_count && curlen == nextlen) {
            continue;
        } else if (count < min_count) {
            do {
                def_send_bits(s, s->bl_tree[curlen].fc.code, s->bl_tree[curlen].dl.len);
            } while (--count != 0);
        } else if (curlen != 0) {
            if (curlen != prevlen) {
                def_send_bits(s, s->bl_tree[curlen].fc.code, s->bl_tree[curlen].dl.len);
                count--;
            }
            def_send_bits(s, s->bl_tree[16].fc.code, s->bl_tree[16].dl.len);
            def_send_bits(s, count - 3, 2);
        } else if (count <= 10) {
            def_send_bits(s, s->bl_tree[17].fc.code, s->bl_tree[17].dl.len);
            def_send_bits(s, count - 3, 3);
        } else {
            def_send_bits(s, s->bl_tree[18].fc.code, s->bl_tree[18].dl.len);
            def_send_bits(s, count - 11, 7);
        }
        count = 0;
        prevlen = curlen;
        if (nextlen == 0) {
            max_count = 138, min_count = 3;
        } else if (curlen == nextlen) {
            max_count = 6, min_count = 3;
        } else {
            max_count = 7, min_count = 4;
        }
    }
}

static int def_build_bl_tree(struct png_deflate *s) {
    def_scan_tree(s, s->dyn_ltree, s->l_desc.max_code);
    def_scan_tree(s, s->dyn_dtree, s->d_desc.max_code);
    def_build_tree(s, &s->bl_desc);
    int max_blindex;
    for (max_blindex = DEF_BL_CODES - 1; max_blindex >= 3; max_blindex--) {
        if (s->bl_tree[def_bl_order[max_blindex]].dl.len != 0) break;
    }
    s->opt_len += 3UL * ((unsigned long)max_blindex + 1UL) + 5UL + 5UL + 4UL;
    return max_blindex;
}

static void def_send_all_trees(struct png_deflate *s, int lcodes, int dcodes, int blcodes) {
    def_send_bits(s, lcodes - 257, 5);
    def_send_bits(s, dcodes - 1, 5);
    def_send_bits(s, blcodes - 4, 4);
    for (int rank = 0; rank < blcodes; rank++)
        def_send_bits(s, s->bl_tree[def_bl_order[rank]].dl.len, 3);
    def_send_tree(s, s->dyn_ltree, lcodes - 1);
    def_send_tree(s, s->dyn_dtree, dcodes - 1);
}

static void def_compress_block(struct png_deflate *s, const struct def_ct *ltree,
        const struct def_ct *dtree) {
    unsigned sx = 0;
    if (s->sym_next != 0) do {
        unsigned dist = s->sym_buf[sx++];
        dist += (unsigned)s->sym_buf[sx++] << 8;
        int lc = s->sym_buf[sx++];
        if (dist == 0) {
            def_send_bits(s, ltree[lc].fc.code, ltree[lc].dl.len);
        } else {
            unsigned code = s->length_code[lc];
            def_send_bits(s, ltree[code + DEF_LITERALS + 1].fc.code,
                ltree[code + DEF_LITERALS + 1].dl.len);
            int extra = def_extra_lbits[code];
            if (extra != 0) {
                lc -= s->base_length[code];
                def_send_bits(s, lc, extra);
            }
            dist--;
            code = dist < 256 ? s->dist_code[dist] : s->dist_code[256 + (dist >> 7)];
            def_send_bits(s, dtree[code].fc.code, dtree[code].dl.len);
            extra = def_extra_dbits[code];
            if (extra != 0) {
                dist -= (unsigned)s->base_dist[code];
                def_send_bits(s, (int)dist, extra);
            }
        }
    } while (sx < s->sym_next);
    def_send_bits(s, ltree[DEF_END_BLOCK].fc.code, ltree[DEF_END_BLOCK].dl.len);
}

static int def_append_out(struct png_deflate *s, const unsigned char *data, size_t len) {
    if (!len) return 1;
    if (len > SIZE_MAX - s->out_len) return 0;
    size_t needed = s->out_len + len;
    if (needed > s->out_cap) {
        size_t ncap = s->out_cap ? s->out_cap * 2 : 8192;
        while (ncap < needed) {
            if (ncap > SIZE_MAX / 2) { ncap = needed; break; }
            ncap *= 2;
        }
        unsigned char *nb = realloc(s->out, ncap);
        if (!nb) return 0;
        s->out = nb;
        s->out_cap = ncap;
    }
    memcpy(s->out + s->out_len, data, len);
    s->out_len += len;
    return 1;
}

static int def_flush_pending(struct png_deflate *s) {
    if (!def_append_out(s, s->pending_buf, s->pending)) return 0;
    s->pending = 0;
    return 1;
}

static int def_flush_block(struct png_deflate *s, int last) {
    const unsigned char *buf = s->block_start >= 0 ?
        s->window + (unsigned)s->block_start : NULL;
    unsigned long stored_len = (unsigned long)((int64_t)s->strstart - s->block_start);
    def_build_tree(s, &s->l_desc);
    def_build_tree(s, &s->d_desc);
    int max_blindex = def_build_bl_tree(s);
    unsigned long opt_lenb = (s->opt_len + 3UL + 7UL) >> 3;
    unsigned long static_lenb = (s->static_len + 3UL + 7UL) >> 3;
    if (static_lenb <= opt_lenb) opt_lenb = static_lenb;
    if (stored_len <= 65535UL && stored_len + 4UL <= opt_lenb && buf != NULL) {
        def_send_bits(s, last, 3);
        def_bi_windup(s);
        def_put_short(s, (uint16_t)stored_len);
        def_put_short(s, (uint16_t)~stored_len);
        if (!def_flush_pending(s)) return 0;
        if (!def_append_out(s, buf, stored_len)) return 0;
    } else if (static_lenb == opt_lenb) {
        def_send_bits(s, 2 + last, 3);
        def_compress_block(s, s->static_ltree, s->static_dtree);
    } else {
        def_send_bits(s, 4 + last, 3);
        def_send_all_trees(s, s->l_desc.max_code + 1, s->d_desc.max_code + 1, max_blindex + 1);
        def_compress_block(s, s->dyn_ltree, s->dyn_dtree);
    }
    def_init_block(s);
    if (last) def_bi_windup(s);
    s->block_start = (int64_t)s->strstart;
    return def_flush_pending(s);
}

static int def_tally_lit(struct png_deflate *s, unsigned char c) {
    s->sym_buf[s->sym_next++] = 0;
    s->sym_buf[s->sym_next++] = 0;
    s->sym_buf[s->sym_next++] = c;
    s->dyn_ltree[c].fc.freq++;
    return s->sym_next == s->sym_end;
}

static int def_tally_dist(struct png_deflate *s, unsigned dist, unsigned lc) {
    s->sym_buf[s->sym_next++] = (unsigned char)dist;
    s->sym_buf[s->sym_next++] = (unsigned char)(dist >> 8);
    s->sym_buf[s->sym_next++] = (unsigned char)lc;
    dist--;
    s->dyn_ltree[s->length_code[lc] + DEF_LITERALS + 1].fc.freq++;
    unsigned dcode = dist < 256 ? s->dist_code[dist] : s->dist_code[256 + (dist >> 7)];
    s->dyn_dtree[dcode].fc.freq++;
    return s->sym_next == s->sym_end;
}

static void def_insert_string(struct png_deflate *s, unsigned str, unsigned *match_head) {
    s->ins_h = ((s->ins_h << DEF_HASH_SHIFT) ^ s->window[str + 2u]) & DEF_HASH_MASK;
    *match_head = s->prev[str & s->w_mask] = s->head[s->ins_h];
    s->head[s->ins_h] = (uint16_t)str;
}

static void def_slide_hash(struct png_deflate *s) {
    unsigned wsize = s->w_size;
    for (unsigned i = 0; i < DEF_HASH_SIZE; i++) {
        unsigned m = s->head[i];
        s->head[i] = (uint16_t)(m >= wsize ? m - wsize : 0);
    }
    for (unsigned i = 0; i < wsize; i++) {
        unsigned m = s->prev[i];
        s->prev[i] = (uint16_t)(m >= wsize ? m - wsize : 0);
    }
}

static void def_fill_window(struct png_deflate *s) {
    unsigned wsize = s->w_size;
    unsigned max_dist = wsize - DEF_MIN_LOOKAHEAD;
    do {
        unsigned more = (unsigned)(s->window_size - (size_t)s->lookahead - (size_t)s->strstart);
        if (s->strstart >= wsize + max_dist) {
            memcpy(s->window, s->window + wsize, wsize - more);
            s->match_start -= wsize;
            s->strstart -= wsize;
            s->block_start -= (int64_t)wsize;
            if (s->insert > s->strstart) s->insert = s->strstart;
            def_slide_hash(s);
            more += wsize;
        }
        if (s->in_pos == s->in_len) break;
        size_t avail = s->in_len - s->in_pos;
        unsigned n = avail > more ? more : (unsigned)avail;
        memcpy(s->window + s->strstart + s->lookahead, s->in + s->in_pos, n);
        s->adler = png_adler32(s->adler, s->window + s->strstart + s->lookahead, n);
        s->in_pos += n;
        s->lookahead += n;
        if (s->lookahead + s->insert >= DEF_MIN_MATCH) {
            unsigned str = s->strstart - s->insert;
            s->ins_h = s->window[str];
            s->ins_h = ((s->ins_h << DEF_HASH_SHIFT) ^ s->window[str + 1u]) & DEF_HASH_MASK;
            while (s->insert) {
                s->ins_h = ((s->ins_h << DEF_HASH_SHIFT) ^ s->window[str + 2u]) & DEF_HASH_MASK;
                s->prev[str & s->w_mask] = s->head[s->ins_h];
                s->head[s->ins_h] = (uint16_t)str;
                str++;
                s->insert--;
                if (s->lookahead + s->insert < DEF_MIN_MATCH) break;
            }
        }
    } while (s->lookahead < DEF_MIN_LOOKAHEAD && s->in_pos < s->in_len);
    if (s->high_water < s->window_size) {
        size_t curr = (size_t)s->strstart + s->lookahead;
        if (s->high_water < curr) {
            size_t init = s->window_size - curr;
            if (init > DEF_WIN_INIT) init = DEF_WIN_INIT;
            memset(s->window + curr, 0, init);
            s->high_water = curr + init;
        } else if (s->high_water < curr + DEF_WIN_INIT) {
            size_t init = curr + DEF_WIN_INIT - s->high_water;
            if (init > s->window_size - s->high_water) init = s->window_size - s->high_water;
            memset(s->window + s->high_water, 0, init);
            s->high_water += init;
        }
    }
}

static unsigned def_longest_match(struct png_deflate *s, unsigned cur_match) {
    unsigned chain_length = 128u;
    const unsigned char *scan = s->window + s->strstart;
    int best_len = (int)s->prev_length;
    int nice_match = 128;
    unsigned max_dist = s->w_size - DEF_MIN_LOOKAHEAD;
    unsigned limit = s->strstart > max_dist ? s->strstart - max_dist : 0u;
    const uint16_t *prev = s->prev;
    unsigned wmask = s->w_mask;
    const unsigned char *strend = s->window + s->strstart + DEF_MAX_MATCH;
    unsigned char scan_end1 = scan[best_len - 1];
    unsigned char scan_end = scan[best_len];
    if (s->prev_length >= 8u) chain_length >>= 2;
    if ((unsigned)nice_match > s->lookahead) nice_match = (int)s->lookahead;
    do {
        const unsigned char *match = s->window + cur_match;
        if (match[best_len] != scan_end ||
            match[best_len - 1] != scan_end1 ||
            *match != *scan ||
            *++match != scan[1]) continue;
        scan += 2;
        match++;
        do {
        } while (*++scan == *++match && *++scan == *++match &&
                 *++scan == *++match && *++scan == *++match &&
                 *++scan == *++match && *++scan == *++match &&
                 *++scan == *++match && *++scan == *++match &&
                 scan < strend);
        int len = (int)DEF_MAX_MATCH - (int)(strend - scan);
        scan = strend - DEF_MAX_MATCH;
        if (len > best_len) {
            s->match_start = cur_match;
            best_len = len;
            if (len >= nice_match) break;
            scan_end1 = scan[best_len - 1];
            scan_end = scan[best_len];
        }
    } while ((cur_match = prev[cur_match & wmask]) > limit && --chain_length != 0u);
    return (unsigned)best_len <= s->lookahead ? (unsigned)best_len : s->lookahead;
}

static int png_deflate(const unsigned char *in, size_t in_len, int window,
        int filtered, unsigned char **out, size_t *out_len) {
    if (window == 8) window = 9;
    struct png_deflate *s = calloc(1, sizeof(*s));
    if (!s) return 0;
    s->in = in;
    s->in_len = in_len;
    s->w_bits = (unsigned)window;
    s->w_size = 1u << s->w_bits;
    s->w_mask = s->w_size - 1u;
    s->window_size = 2u * (size_t)s->w_size;
    s->filtered = filtered;
    s->adler = 1u;
    s->match_length = s->prev_length = DEF_MIN_MATCH - 1u;
    s->window = calloc(1, s->window_size + 8u);
    s->prev = malloc(s->w_size * sizeof(uint16_t));
    s->head = calloc(DEF_HASH_SIZE, sizeof(uint16_t));
    s->pending_buf = malloc(DEF_LIT_BUFSIZE * 8u);
    s->sym_buf = malloc(DEF_LIT_BUFSIZE * 3u);
    if (!s->window || !s->prev || !s->head || !s->pending_buf || !s->sym_buf) goto fail;
    s->sym_end = (DEF_LIT_BUFSIZE - 1u) * 3u;
    def_tr_init(s);
    unsigned header = (8u + ((s->w_bits - 8u) << 4)) << 8;
    header |= (2u << 6);
    header += 31u - (header % 31u);
    def_put_short_msb(s, (uint16_t)header);
    if (!def_flush_pending(s)) goto fail;
    unsigned max_dist = s->w_size - DEF_MIN_LOOKAHEAD;
    for (;;) {
        if (s->lookahead < DEF_MIN_LOOKAHEAD) {
            def_fill_window(s);
            if (s->lookahead == 0) break;
        }
        unsigned hash_head = 0;
        if (s->lookahead >= DEF_MIN_MATCH)
            def_insert_string(s, s->strstart, &hash_head);
        s->prev_length = s->match_length;
        s->prev_match = s->match_start;
        s->match_length = DEF_MIN_MATCH - 1u;
        if (hash_head != 0 && s->prev_length < 16u && s->strstart - hash_head <= max_dist) {
            s->match_length = def_longest_match(s, hash_head);
            if (s->match_length <= 5u && (s->filtered ||
                    (s->match_length == DEF_MIN_MATCH &&
                    s->strstart - s->match_start > DEF_TOO_FAR))) {
                s->match_length = DEF_MIN_MATCH - 1u;
            }
        }
        if (s->prev_length >= DEF_MIN_MATCH && s->match_length <= s->prev_length) {
            unsigned max_insert = s->strstart + s->lookahead - DEF_MIN_MATCH;
            int bflush = def_tally_dist(s, s->strstart - 1u - s->prev_match,
                s->prev_length - DEF_MIN_MATCH);
            s->lookahead -= s->prev_length - 1u;
            s->prev_length -= 2u;
            do {
                if (++s->strstart <= max_insert) {
                    unsigned dummy;
                    def_insert_string(s, s->strstart, &dummy);
                }
            } while (--s->prev_length != 0u);
            s->match_available = 0;
            s->match_length = DEF_MIN_MATCH - 1u;
            s->strstart++;
            if (bflush && !def_flush_block(s, 0)) goto fail;
        } else if (s->match_available) {
            int bflush = def_tally_lit(s, s->window[s->strstart - 1u]);
            if (bflush && !def_flush_block(s, 0)) goto fail;
            s->strstart++;
            s->lookahead--;
        } else {
            s->match_available = 1;
            s->strstart++;
            s->lookahead--;
        }
    }
    if (s->match_available) {
        def_tally_lit(s, s->window[s->strstart - 1u]);
        s->match_available = 0;
    }
    if (!def_flush_block(s, 1)) goto fail;
    def_put_short_msb(s, (uint16_t)(s->adler >> 16));
    def_put_short_msb(s, (uint16_t)(s->adler & 0xffffu));
    if (!def_flush_pending(s)) goto fail;
    *out = s->out;
    *out_len = s->out_len;
    free(s->window);
    free(s->prev);
    free(s->head);
    free(s->pending_buf);
    free(s->sym_buf);
    free(s);
    return 1;
fail:
    free(s->out);
    free(s->window);
    free(s->prev);
    free(s->head);
    free(s->pending_buf);
    free(s->sym_buf);
    free(s);
    return 0;
}

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
    struct png_inflate *inflate;
    const unsigned char *in_next;
    size_t in_avail;
    int stream_ended;
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

static int read_bytes(struct png_reader *png, void *output, size_t length) {
    if (length > png->length - png->offset) return 0;
    memcpy(output, png->bytes + png->offset, length);
    png->offset += length;
    return 1;
}

static int chunk_bytes(struct png_reader *png, unsigned char *output, uint32_t length) {
    if (!read_bytes(png, output, length)) return 0;
    png->crc = tomoe_png_crc32(png->crc, output, length);
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
    png->crc = tomoe_png_crc32(0, header + 4, 4);
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
        unsigned avail;
        if (!remaining) return 0;
        if (png->in_avail == 0) {
            while (png->idat_size == 0) {
                if (finish_chunk(png, 0, 0)) return 1;
                if (!read_header(png)) return 1;
                if (png->chunk_name != CHUNK('I', 'D', 'A', 'T')) return 1;
                png->idat_size = png->chunk_length;
            }
            avail = png->idat_size > IDAT_BUFFER ? IDAT_BUFFER : png->idat_size;
            if (!chunk_bytes(png, png->idat, avail)) return 1;
            png->idat_size -= avail;
            png->in_next = png->idat;
            png->in_avail = avail;
        }
        result = png_inflate_step(png->inflate, &png->in_next, &png->in_avail,
            &cursor, &remaining);
        if (result == INF_RES_END) {
            png->mode |= MODE_AFTER_IDAT;
            png->stream_ended = 1;
            return remaining != 0;
        }
        if (result != INF_RES_OK) return 1;
    }
}

static int finish_idat(struct png_reader *png) {
    if (!png->stream_ended) {
        unsigned char buffer[INFLATE_BUFFER];
        size_t total = 0;
        for (;;) {
            int result;
            unsigned avail;
            unsigned char *out;
            size_t out_avail;
            if (png->in_avail == 0) {
                while (png->idat_size == 0) {
                    if (finish_chunk(png, 0, 0)) return 1;
                    if (!read_header(png)) return 1;
                    if (png->chunk_name != CHUNK('I', 'D', 'A', 'T')) return 1;
                    png->idat_size = png->chunk_length;
                }
                avail = png->idat_size > IDAT_BUFFER ? IDAT_BUFFER : png->idat_size;
                if (!chunk_bytes(png, png->idat, avail)) return 1;
                png->idat_size -= avail;
                png->in_next = png->idat;
                png->in_avail = avail;
            }
            out = buffer;
            out_avail = sizeof(buffer);
            result = png_inflate_step(png->inflate, &png->in_next, &png->in_avail,
                &out, &out_avail);
            if (result == INF_RES_END) {
                png->mode |= MODE_AFTER_IDAT;
                png->stream_ended = 1;
                break;
            }
            total += (size_t)(out - buffer);
            if (result != INF_RES_OK || !total) break;
        }
        if (!png->stream_ended) {
            png->mode |= MODE_AFTER_IDAT;
            png->stream_ended = 1;
        }
    }
    if (png->inflate) {
        png->in_next = NULL;
        png->in_avail = 0;
        png_inflate_destroy(png->inflate);
        png->inflate = NULL;
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
    if (png->inflate) png_inflate_destroy(png->inflate);
    free(png->row);
    free(png->previous);
    free(png->line);
    png->row = png->previous = png->line = NULL;
    png->inflate = NULL;
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
    png->inflate = png_inflate_create();
    if (!png->inflate) return PNG_HARD;
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
        png->stride = (uint32_t)png->width *
            (png->output == OUT_RGB96F ? 12u : png->output == OUT_RGBA128F ? 16u : 4u);
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
    image->format = reader.output == OUT_RGB24 ? PNG_FORMAT_RGB24 :
        reader.output == OUT_RGB96F ? PNG_FORMAT_RGB96F :
        reader.output == OUT_RGBA128F ? PNG_FORMAT_RGBA128F : PNG_FORMAT_ARGB32;
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
    crc = tomoe_png_crc32(0, header + 4, 4);
    crc = tomoe_png_crc32(crc, data, count);
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
    if (!width || !height) return PNG_BAD;
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
        int window = 15;
        if (uncompressed <= 16384) {
            unsigned half = 1u << (window - 1);
            while (uncompressed + 262 <= half) { half >>= 1; window--; }
        }
        if (!png_deflate(filtered, total, window, filters != 0x08u, &compressed, &compressed_length)) goto done;
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
