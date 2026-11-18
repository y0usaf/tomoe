/* SPDX-License-Identifier: AGPL-3.0-only AND MIT
   The IDCT, upsampling and YCbCr conversion are ported from zune-jpeg 0.5.8
   (MIT OR Apache-2.0 OR Zlib); see NOTICE. */
#include "jpeg.h"

#include <setjmp.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#define DCTSIZE 8
#define DCTSIZE2 64
#define MAX_COMPONENTS 10
#define MAX_COMPS_IN_SCAN 4
#define D_MAX_BLOCKS_IN_MCU 10
#define NUM_QUANT_TBLS 4
#define NUM_HUFF_TBLS 4
#define NUM_ARITH_TBLS 16
#define MAX_SAMP_FACTOR 4
#define JPEG_MAX_DIMENSION 65500
#define MIN_GET_BITS 57
#define MAXJSAMPLE 255
#define CENTERJSAMPLE 128
#define RANGE_MASK (MAXJSAMPLE * 4 + 3)
#define SAVED_COEFS 10

#define M_SOF0 0xc0
#define M_SOF1 0xc1
#define M_SOF2 0xc2
#define M_SOF3 0xc3
#define M_SOF5 0xc5
#define M_SOF6 0xc6
#define M_SOF7 0xc7
#define M_JPG 0xc8
#define M_SOF9 0xc9
#define M_SOF10 0xca
#define M_SOF11 0xcb
#define M_SOF13 0xcd
#define M_SOF14 0xce
#define M_SOF15 0xcf
#define M_DHT 0xc4
#define M_DAC 0xcc
#define M_RST0 0xd0
#define M_RST7 0xd7
#define M_SOI 0xd8
#define M_EOI 0xd9
#define M_SOS 0xda
#define M_DQT 0xdb
#define M_DNL 0xdc
#define M_DRI 0xdd
#define M_APP0 0xe0
#define M_APP14 0xee
#define M_APP15 0xef
#define M_COM 0xfe
#define M_TEM 0x01

enum color_space { CS_UNKNOWN, CS_GRAYSCALE, CS_RGB, CS_YCBCR, CS_CMYK, CS_YCCK };

enum entropy_kind { ENTROPY_HUFF, ENTROPY_PHUFF, ENTROPY_ARITH };

enum up_method { UP_FULL, UP_INT, UP_H2V1_BOX, UP_H2V1_FANCY, UP_H2V2_BOX, UP_H2V2_FANCY, UP_H1V2 };

static const int natural_order[DCTSIZE2 + 16] = {
    0, 1, 8, 16, 9, 2, 3, 10,
    17, 24, 32, 25, 18, 11, 4, 5,
    12, 19, 26, 33, 40, 48, 41, 34,
    27, 20, 13, 6, 7, 14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36,
    29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46,
    53, 60, 61, 54, 47, 55, 62, 63,
    63, 63, 63, 63, 63, 63, 63, 63,
    63, 63, 63, 63, 63, 63, 63, 63
};

static const uint8_t std_bits_dc_luminance[17] = { 0, 0, 1, 5, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0 };
static const uint8_t std_val_dc_luminance[12] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11 };
static const uint8_t std_bits_dc_chrominance[17] = { 0, 0, 3, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0 };
static const uint8_t std_val_dc_chrominance[12] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11 };
static const uint8_t std_bits_ac_luminance[17] = { 0, 0, 2, 1, 3, 3, 2, 4, 3, 5, 5, 4, 4, 0, 0, 1, 0x7d };
static const uint8_t std_val_ac_luminance[162] = {
    0x01, 0x02, 0x03, 0x00, 0x04, 0x11, 0x05, 0x12,
    0x21, 0x31, 0x41, 0x06, 0x13, 0x51, 0x61, 0x07,
    0x22, 0x71, 0x14, 0x32, 0x81, 0x91, 0xa1, 0x08,
    0x23, 0x42, 0xb1, 0xc1, 0x15, 0x52, 0xd1, 0xf0,
    0x24, 0x33, 0x62, 0x72, 0x82, 0x09, 0x0a, 0x16,
    0x17, 0x18, 0x19, 0x1a, 0x25, 0x26, 0x27, 0x28,
    0x29, 0x2a, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39,
    0x3a, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49,
    0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59,
    0x5a, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69,
    0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79,
    0x7a, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89,
    0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98,
    0x99, 0x9a, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7,
    0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6,
    0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3, 0xc4, 0xc5,
    0xc6, 0xc7, 0xc8, 0xc9, 0xca, 0xd2, 0xd3, 0xd4,
    0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda, 0xe1, 0xe2,
    0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea,
    0xf1, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8,
    0xf9, 0xfa
};
static const uint8_t std_bits_ac_chrominance[17] = { 0, 0, 2, 1, 2, 4, 4, 3, 4, 7, 5, 4, 4, 0, 1, 2, 0x77 };
static const uint8_t std_val_ac_chrominance[162] = {
    0x00, 0x01, 0x02, 0x03, 0x11, 0x04, 0x05, 0x21,
    0x31, 0x06, 0x12, 0x41, 0x51, 0x07, 0x61, 0x71,
    0x13, 0x22, 0x32, 0x81, 0x08, 0x14, 0x42, 0x91,
    0xa1, 0xb1, 0xc1, 0x09, 0x23, 0x33, 0x52, 0xf0,
    0x15, 0x62, 0x72, 0xd1, 0x0a, 0x16, 0x24, 0x34,
    0xe1, 0x25, 0xf1, 0x17, 0x18, 0x19, 0x1a, 0x26,
    0x27, 0x28, 0x29, 0x2a, 0x35, 0x36, 0x37, 0x38,
    0x39, 0x3a, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48,
    0x49, 0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58,
    0x59, 0x5a, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68,
    0x69, 0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78,
    0x79, 0x7a, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87,
    0x88, 0x89, 0x8a, 0x92, 0x93, 0x94, 0x95, 0x96,
    0x97, 0x98, 0x99, 0x9a, 0xa2, 0xa3, 0xa4, 0xa5,
    0xa6, 0xa7, 0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4,
    0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3,
    0xc4, 0xc5, 0xc6, 0xc7, 0xc8, 0xc9, 0xca, 0xd2,
    0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda,
    0xe2, 0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9,
    0xea, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8,
    0xf9, 0xfa
};

#define ARITH_ENTRY(a, b, c, d) (((int64_t)(a) << 16) | ((int64_t)(c) << 8) | ((int64_t)(d) << 7) | (b))
static const int64_t arith_table[114] = {
    ARITH_ENTRY(0x5a1d, 1, 1, 1),
    ARITH_ENTRY(0x2586, 14, 2, 0),
    ARITH_ENTRY(0x1114, 16, 3, 0),
    ARITH_ENTRY(0x080b, 18, 4, 0),
    ARITH_ENTRY(0x03d8, 20, 5, 0),
    ARITH_ENTRY(0x01da, 23, 6, 0),
    ARITH_ENTRY(0x00e5, 25, 7, 0),
    ARITH_ENTRY(0x006f, 28, 8, 0),
    ARITH_ENTRY(0x0036, 30, 9, 0),
    ARITH_ENTRY(0x001a, 33, 10, 0),
    ARITH_ENTRY(0x000d, 35, 11, 0),
    ARITH_ENTRY(0x0006, 9, 12, 0),
    ARITH_ENTRY(0x0003, 10, 13, 0),
    ARITH_ENTRY(0x0001, 12, 13, 0),
    ARITH_ENTRY(0x5a7f, 15, 15, 1),
    ARITH_ENTRY(0x3f25, 36, 16, 0),
    ARITH_ENTRY(0x2cf2, 38, 17, 0),
    ARITH_ENTRY(0x207c, 39, 18, 0),
    ARITH_ENTRY(0x17b9, 40, 19, 0),
    ARITH_ENTRY(0x1182, 42, 20, 0),
    ARITH_ENTRY(0x0cef, 43, 21, 0),
    ARITH_ENTRY(0x09a1, 45, 22, 0),
    ARITH_ENTRY(0x072f, 46, 23, 0),
    ARITH_ENTRY(0x055c, 48, 24, 0),
    ARITH_ENTRY(0x0406, 49, 25, 0),
    ARITH_ENTRY(0x0303, 51, 26, 0),
    ARITH_ENTRY(0x0240, 52, 27, 0),
    ARITH_ENTRY(0x01b1, 54, 28, 0),
    ARITH_ENTRY(0x0144, 56, 29, 0),
    ARITH_ENTRY(0x00f5, 57, 30, 0),
    ARITH_ENTRY(0x00b7, 59, 31, 0),
    ARITH_ENTRY(0x008a, 60, 32, 0),
    ARITH_ENTRY(0x0068, 62, 33, 0),
    ARITH_ENTRY(0x004e, 63, 34, 0),
    ARITH_ENTRY(0x003b, 32, 35, 0),
    ARITH_ENTRY(0x002c, 33, 9, 0),
    ARITH_ENTRY(0x5ae1, 37, 37, 1),
    ARITH_ENTRY(0x484c, 64, 38, 0),
    ARITH_ENTRY(0x3a0d, 65, 39, 0),
    ARITH_ENTRY(0x2ef1, 67, 40, 0),
    ARITH_ENTRY(0x261f, 68, 41, 0),
    ARITH_ENTRY(0x1f33, 69, 42, 0),
    ARITH_ENTRY(0x19a8, 70, 43, 0),
    ARITH_ENTRY(0x1518, 72, 44, 0),
    ARITH_ENTRY(0x1177, 73, 45, 0),
    ARITH_ENTRY(0x0e74, 74, 46, 0),
    ARITH_ENTRY(0x0bfb, 75, 47, 0),
    ARITH_ENTRY(0x09f8, 77, 48, 0),
    ARITH_ENTRY(0x0861, 78, 49, 0),
    ARITH_ENTRY(0x0706, 79, 50, 0),
    ARITH_ENTRY(0x05cd, 48, 51, 0),
    ARITH_ENTRY(0x04de, 50, 52, 0),
    ARITH_ENTRY(0x040f, 50, 53, 0),
    ARITH_ENTRY(0x0363, 51, 54, 0),
    ARITH_ENTRY(0x02d4, 52, 55, 0),
    ARITH_ENTRY(0x025c, 53, 56, 0),
    ARITH_ENTRY(0x01f8, 54, 57, 0),
    ARITH_ENTRY(0x01a4, 55, 58, 0),
    ARITH_ENTRY(0x0160, 56, 59, 0),
    ARITH_ENTRY(0x0125, 57, 60, 0),
    ARITH_ENTRY(0x00f6, 58, 61, 0),
    ARITH_ENTRY(0x00cb, 59, 62, 0),
    ARITH_ENTRY(0x00ab, 61, 63, 0),
    ARITH_ENTRY(0x008f, 61, 32, 0),
    ARITH_ENTRY(0x5b12, 65, 65, 1),
    ARITH_ENTRY(0x4d04, 80, 66, 0),
    ARITH_ENTRY(0x412c, 81, 67, 0),
    ARITH_ENTRY(0x37d8, 82, 68, 0),
    ARITH_ENTRY(0x2fe8, 83, 69, 0),
    ARITH_ENTRY(0x293c, 84, 70, 0),
    ARITH_ENTRY(0x2379, 86, 71, 0),
    ARITH_ENTRY(0x1edf, 87, 72, 0),
    ARITH_ENTRY(0x1aa9, 87, 73, 0),
    ARITH_ENTRY(0x174e, 72, 74, 0),
    ARITH_ENTRY(0x1424, 72, 75, 0),
    ARITH_ENTRY(0x119c, 74, 76, 0),
    ARITH_ENTRY(0x0f6b, 74, 77, 0),
    ARITH_ENTRY(0x0d51, 75, 78, 0),
    ARITH_ENTRY(0x0bb6, 77, 79, 0),
    ARITH_ENTRY(0x0a40, 77, 48, 0),
    ARITH_ENTRY(0x5832, 80, 81, 1),
    ARITH_ENTRY(0x4d1c, 88, 82, 0),
    ARITH_ENTRY(0x438e, 89, 83, 0),
    ARITH_ENTRY(0x3bdd, 90, 84, 0),
    ARITH_ENTRY(0x34ee, 91, 85, 0),
    ARITH_ENTRY(0x2eae, 92, 86, 0),
    ARITH_ENTRY(0x299a, 93, 87, 0),
    ARITH_ENTRY(0x2516, 86, 71, 0),
    ARITH_ENTRY(0x5570, 88, 89, 1),
    ARITH_ENTRY(0x4ca9, 95, 90, 0),
    ARITH_ENTRY(0x44d9, 96, 91, 0),
    ARITH_ENTRY(0x3e22, 97, 92, 0),
    ARITH_ENTRY(0x3824, 99, 93, 0),
    ARITH_ENTRY(0x32b4, 99, 94, 0),
    ARITH_ENTRY(0x2e17, 93, 86, 0),
    ARITH_ENTRY(0x56a8, 95, 96, 1),
    ARITH_ENTRY(0x4f46, 101, 97, 0),
    ARITH_ENTRY(0x47e5, 102, 98, 0),
    ARITH_ENTRY(0x41cf, 103, 99, 0),
    ARITH_ENTRY(0x3c3d, 104, 100, 0),
    ARITH_ENTRY(0x375e, 99, 93, 0),
    ARITH_ENTRY(0x5231, 105, 102, 0),
    ARITH_ENTRY(0x4c0f, 106, 103, 0),
    ARITH_ENTRY(0x4639, 107, 104, 0),
    ARITH_ENTRY(0x415e, 103, 99, 0),
    ARITH_ENTRY(0x5627, 105, 106, 1),
    ARITH_ENTRY(0x50e7, 108, 107, 0),
    ARITH_ENTRY(0x4b85, 109, 103, 0),
    ARITH_ENTRY(0x5597, 110, 109, 0),
    ARITH_ENTRY(0x504f, 111, 107, 0),
    ARITH_ENTRY(0x5a10, 110, 111, 1),
    ARITH_ENTRY(0x5522, 112, 109, 0),
    ARITH_ENTRY(0x59eb, 112, 111, 1),
    ARITH_ENTRY(0x5a1d, 113, 113, 0)
};

struct quant_table {
    uint16_t value[64];
    bool defined;
};

struct huff_table {
    uint8_t bits[17];
    uint8_t value[256];
    bool defined;
};

struct derived_table {
    int32_t maxcode[18];
    int32_t valoffset[18];
    uint16_t lookup[256];
    const uint8_t *value;
};

struct component {
    int id, h, v, tq, td, ta;
    uint32_t width_in_blocks, height_in_blocks;
    uint32_t downsampled_width, downsampled_height;
    uint32_t mcu_width, mcu_height, last_col_width, last_row_height;
    uint16_t quant[64];
    int16_t multiplier[64];
    bool quant_defined;
    int16_t *coeff;
    uint32_t coeff_stride, coeff_rows;
    uint8_t *samples;
    uint32_t sample_stride, sample_rows;
};

struct bit_reader {
    uint64_t buffer;
    int left;
};

struct jpeg {
    jmp_buf jump;
    bool hard;

    const uint8_t *data;
    size_t length;
    size_t offset;
    uint8_t fake[2];
    size_t fake_offset;
    bool fake_active;
    void *owned[64];
    int owned_count;

    int unread_marker;
    int next_restart;
    bool saw_soi, saw_sof, saw_jfif, saw_adobe, in_headers;
    int adobe_transform;
    int width, height;
    int precision;
    int num_components;
    struct component comp[MAX_COMPONENTS];
    struct quant_table quant[NUM_QUANT_TBLS];
    struct huff_table dc_huff[NUM_HUFF_TBLS], ac_huff[NUM_HUFF_TBLS];
    struct derived_table dc_derived[NUM_HUFF_TBLS], ac_derived[NUM_HUFF_TBLS];
    uint8_t arith_dc_l[NUM_ARITH_TBLS], arith_dc_u[NUM_ARITH_TBLS], arith_ac_k[NUM_ARITH_TBLS];
    int restart_interval;
    int max_h, max_v;
    uint32_t total_iMCU_rows;
    uint32_t input_iMCU_row;
    int last_good_row;
    bool progressive, lossless, arith;
    bool has_multiple_scans;
    int input_scan_number;
    int comps_in_scan;
    int scan_comp[MAX_COMPS_IN_SCAN];
    int ss, se, ah, al;
    uint32_t mcus_per_row, mcu_rows_in_scan;
    int blocks_in_MCU;
    int membership[D_MAX_BLOCKS_IN_MCU];
    int mcu_rows_per_iMCU;
    uint32_t mcu_col, mcu_vertical;
    int32_t *diff_rows[MAX_COMPONENTS];
    uint32_t diff_stride[MAX_COMPONENTS];
    uint32_t *undiff_prev[MAX_COMPONENTS];
    uint32_t *undiff_row;
    enum entropy_kind entropy;
    bool insufficient_data;
    struct bit_reader bits;
    int restarts_to_go;
    unsigned int restart_rows_to_go;
    int last_dc[MAX_COMPS_IN_SCAN];
    bool undiff_first[MAX_COMPONENTS];
    unsigned eobrun;
    uint8_t dc_stats[NUM_ARITH_TBLS][64];
    uint8_t ac_stats[NUM_ARITH_TBLS][256];
    uint8_t fixed_bin[4];
    int64_t arith_c, arith_a;
    int arith_ct;
    int arith_last_dc[MAX_COMPS_IN_SCAN];
    int arith_dc_context[MAX_COMPS_IN_SCAN];
    int coef_bits[2 * MAX_COMPONENTS][DCTSIZE2];

    uint32_t output_width, output_height;
    enum color_space jpeg_color, out_color;
    int out_components;
    uint8_t range_limit[5 * (MAXJSAMPLE + 1) + CENTERJSAMPLE];
    uint8_t *range_base;

    int cr_r[256], cb_b[256];
    int64_t cr_g[256], cb_g[256];

    enum up_method up_method[MAX_COMPONENTS];
    uint8_t up_h_expand[MAX_COMPONENTS], up_v_expand[MAX_COMPONENTS];
    uint8_t *up_buffer[MAX_COMPONENTS];
    const uint8_t *rows[MAX_COMPONENTS][MAX_SAMP_FACTOR];
    uint32_t up_stride;
    int max_rows;

    int smooth;
    int smooth_cur[MAX_COMPONENTS][SAVED_COEFS];
    int smooth_prev[MAX_COMPONENTS][SAVED_COEFS];

    int conversion;
    int zune;
    unsigned char *pixels;
};

enum conversion_kind {
    CONV_NULL3, CONV_NULL4, CONV_YCC_RGB, CONV_GRAY_RGB, CONV_YCCK_CMYK
};

static int left_shift(int value, int shift);
static void update_progression(struct jpeg *j);

static void fail(struct jpeg *j) { j->hard = false; longjmp(j->jump, 1); }
static void fail_hard(struct jpeg *j) { j->hard = true; longjmp(j->jump, 1); }

static void *own(struct jpeg *j, size_t size) {
    void *block = calloc(1, size ? size : 1);
    if (!block) fail_hard(j);
    if (j->owned_count >= (int)(sizeof(j->owned) / sizeof(j->owned[0]))) fail_hard(j);
    j->owned[j->owned_count++] = block;
    return block;
}

static void release(struct jpeg *j) {
    for (int i = j->owned_count - 1; i >= 0; i--) free(j->owned[i]);
    j->owned_count = 0;
}

static int source_byte(struct jpeg *j) {
    if (j->fake_active) {
        if (j->fake_offset < 2) return j->fake[j->fake_offset++];
        j->fake_offset = 0;
        return j->fake[j->fake_offset++];
    }
    if (j->offset >= j->length) {
        j->fake_active = true;
        j->fake_offset = 0;
        return j->fake[j->fake_offset++];
    }
    return j->data[j->offset++];
}

static int read_u16(struct jpeg *j) {
    int high = source_byte(j);
    return (high << 8) | source_byte(j);
}

static void skip_bytes(struct jpeg *j, long count) {
    while (count-- > 0) (void)source_byte(j);
}

static void next_marker(struct jpeg *j) {
    for (;;) {
        int c = source_byte(j);
        while (c != 0xff) c = source_byte(j);
        do { c = source_byte(j); } while (c == 0xff);
        if (c) { j->unread_marker = c; return; }
    }
}

static void first_marker(struct jpeg *j) {
    int c = source_byte(j);
    int c2 = source_byte(j);
    if (c != 0xff || c2 != M_SOI) fail(j);
    j->unread_marker = c2;
}

static void fill_bits(struct jpeg *j, int need) {
    if (j->unread_marker == 0) {
        while (j->bits.left < MIN_GET_BITS) {
            int c = source_byte(j);
            if (c == 0xff) {
                do { c = source_byte(j); } while (c == 0xff);
                if (c == 0) {
                    c = 0xff;
                } else {
                    j->unread_marker = c;
                    goto stuffed;
                }
            }
            j->bits.buffer = (j->bits.buffer << 8) | (uint64_t)c;
            j->bits.left += 8;
        }
    } else {
stuffed:
        if (need > j->bits.left) {
            j->insufficient_data = true;
            j->bits.buffer <<= MIN_GET_BITS - j->bits.left;
            j->bits.left = MIN_GET_BITS;
        }
    }
}

static int get_bits(struct jpeg *j, int count) {
    if (j->bits.left < count) fill_bits(j, count);
    j->bits.left -= count;
    return (int)((j->bits.buffer >> j->bits.left) & ((UINT64_C(1) << count) - 1));
}

static void drop_bits(struct jpeg *j, int count) { j->bits.left -= count; }

static int huff_decode_slow(struct jpeg *j, const struct derived_table *table, int min_bits) {
    int length = min_bits;
    int64_t code;
    if (j->bits.left < length) fill_bits(j, length);
    code = get_bits(j, length);
    while (code > table->maxcode[length]) {
        code = (code << 1) | get_bits(j, 1);
        length++;
    }
    if (length > 16) return 0;
    return table->value[(int)(code + table->valoffset[length])];
}

static int huff_decode(struct jpeg *j, const struct derived_table *table) {
    if (j->bits.left < 8) fill_bits(j, 0);
    if (j->bits.left >= 8) {
        int look = (int)((j->bits.buffer >> (j->bits.left - 8)) & 0xff);
        int entry = table->lookup[look];
        int length = entry >> 8;
        if (length <= 8) {
            drop_bits(j, length);
            return entry & 0xff;
        }
        return huff_decode_slow(j, table, length);
    }
    return huff_decode_slow(j, table, 1);
}

static int extend_value(int value, int size) {
    if (value < (1 << (size - 1))) return value + ((((unsigned int)-1) << size) + 1);
    return value;
}

static void build_derived(struct jpeg *j, int is_dc, int slot, struct derived_table *derived) {
    const struct huff_table *table;
    char sizes[257];
    int32_t codes[257];
    int p = 0, count = 0, length;
    int32_t code = 0;
    int size;
    if (slot < 0 || slot >= NUM_HUFF_TBLS) fail(j);
    table = is_dc ? &j->dc_huff[slot] : &j->ac_huff[slot];
    if (!table->defined) fail(j);
    derived->value = table->value;
    for (length = 1; length <= 16; length++) {
        int number = table->bits[length];
        if (number < 0 || p + number > 256) fail(j);
        while (number--) sizes[p++] = (char)length;
    }
    sizes[p] = 0;
    count = p;
    p = 0;
    size = sizes[0];
    while (sizes[p]) {
        while ((int)sizes[p] == size) {
            codes[p++] = code;
            code++;
        }
        if (code >= ((int64_t)1 << size)) fail(j);
        code <<= 1;
        size++;
    }
    p = 0;
    for (length = 1; length <= 16; length++) {
        if (table->bits[length]) {
            derived->valoffset[length] = p - codes[p];
            p += table->bits[length];
            derived->maxcode[length] = codes[p - 1];
        } else {
            derived->maxcode[length] = -1;
        }
    }
    derived->valoffset[17] = 0;
    derived->maxcode[17] = 0xfffff;
    for (int i = 0; i < 256; i++) derived->lookup[i] = (uint16_t)(9 << 8);
    p = 0;
    for (length = 1; length <= 8; length++) {
        for (int i = 1; i <= table->bits[length]; i++, p++) {
            int lookbits = codes[p] << (8 - length);
            for (int ctr = 1 << (8 - length); ctr > 0; ctr--) {
                derived->lookup[lookbits] = (uint16_t)((length << 8) | table->value[p]);
                lookbits++;
            }
        }
    }
    if (is_dc) {
        for (int i = 0; i < count; i++) {
            int symbol = table->value[i];
            if (symbol < 0 || symbol > (j->lossless ? 16 : 15)) fail(j);
        }
    }
}

static void build_scan_derived(struct jpeg *j, bool is_dc, int slot) {
    if (slot < 0 || slot >= NUM_HUFF_TBLS) fail(j);
    build_derived(j, is_dc ? 1 : 0, slot,
            is_dc ? &j->dc_derived[slot] : &j->ac_derived[slot]);
}

static void install_std_tables(struct jpeg *j) {
    struct huff_table *dc = &j->dc_huff[0];
    struct huff_table *ac = &j->ac_huff[0];
    if (!dc->defined) {
        memcpy(dc->bits, std_bits_dc_luminance, sizeof(dc->bits));
        memcpy(dc->value, std_val_dc_luminance, sizeof(std_val_dc_luminance));
        dc->defined = true;
    }
    if (!ac->defined) {
        memcpy(ac->bits, std_bits_ac_luminance, sizeof(ac->bits));
        memcpy(ac->value, std_val_ac_luminance, sizeof(std_val_ac_luminance));
        ac->defined = true;
    }
    dc = &j->dc_huff[1];
    ac = &j->ac_huff[1];
    if (!dc->defined) {
        memcpy(dc->bits, std_bits_dc_chrominance, sizeof(dc->bits));
        memcpy(dc->value, std_val_dc_chrominance, sizeof(std_val_dc_chrominance));
        dc->defined = true;
    }
    if (!ac->defined) {
        memcpy(ac->bits, std_bits_ac_chrominance, sizeof(ac->bits));
        memcpy(ac->value, std_val_ac_chrominance, sizeof(std_val_ac_chrominance));
        ac->defined = true;
    }
}

static void get_soi(struct jpeg *j) {
    if (j->saw_soi) fail(j);
    for (int i = 0; i < NUM_ARITH_TBLS; i++) {
        j->arith_dc_l[i] = 0;
        j->arith_dc_u[i] = 1;
        j->arith_ac_k[i] = 5;
    }
    j->restart_interval = 0;
    j->jpeg_color = CS_UNKNOWN;
    j->saw_jfif = false;
    j->saw_adobe = false;
    j->adobe_transform = 0;
    j->saw_soi = true;
}

static void get_sof(struct jpeg *j, bool progressive, bool lossless, bool arith) {
    int length, precision, height, width, count;
    if (j->saw_sof) fail(j);
    j->progressive = progressive;
    j->lossless = lossless;
    j->arith = arith;
    length = read_u16(j);
    precision = source_byte(j);
    height = read_u16(j);
    width = read_u16(j);
    count = source_byte(j);
    j->precision = precision;
    j->height = height;
    j->width = width;
    j->num_components = count;
    if (height <= 0 || width <= 0 || count <= 0) fail(j);
    if (length != count * 3 + 8) fail(j);
    for (int ci = 0; ci < count; ci++) {
        int id = source_byte(j);
        int hv = source_byte(j);
        int tq = source_byte(j);
        if (ci < MAX_COMPONENTS) {
            j->comp[ci].id = id;
            j->comp[ci].h = (hv >> 4) & 15;
            j->comp[ci].v = hv & 15;
            j->comp[ci].tq = tq;
        }
    }
    j->saw_sof = true;
}

static void get_sos(struct jpeg *j) {
    int length, count;
    if (!j->saw_sof) fail(j);
    length = read_u16(j);
    count = source_byte(j);
    if (length != count * 2 + 6 || count < 1 || count > MAX_COMPS_IN_SCAN) fail(j);
    j->comps_in_scan = count;
    for (int i = 0; i < count; i++) {
        int id = source_byte(j);
        int tables = source_byte(j);
        int found = -1;
        for (int ci = 0; ci < j->num_components && ci < MAX_COMPONENTS; ci++) {
            if (id == j->comp[ci].id) { found = ci; break; }
        }
        if (found < 0) fail(j);
        for (int previous = 0; previous < i; previous++) {
            if (j->scan_comp[previous] == found) fail(j);
        }
        j->scan_comp[i] = found;
        j->comp[found].td = (tables >> 4) & 15;
        j->comp[found].ta = tables & 15;
    }
    j->ss = source_byte(j);
    j->se = source_byte(j);
    int approximate = source_byte(j);
    j->ah = (approximate >> 4) & 15;
    j->al = approximate & 15;
    j->next_restart = 0;
    j->input_scan_number++;
}

static void get_dqt(struct jpeg *j) {
    int length = read_u16(j);
    length -= 2;
    while (length > 0) {
        int packed = source_byte(j);
        int precision = packed >> 4;
        int slot = packed & 15;
        if (slot >= NUM_QUANT_TBLS) fail(j);
        for (int i = 0; i < DCTSIZE2; i++) {
            int value = precision ? read_u16(j) : source_byte(j);
            j->quant[slot].value[natural_order[i]] = (uint16_t)value;
        }
        j->quant[slot].defined = true;
        length -= DCTSIZE2 + 1;
        if (precision) length -= DCTSIZE2;
    }
    if (length != 0) fail(j);
}

static void get_dht(struct jpeg *j) {
    int length = read_u16(j);
    length -= 2;
    while (length > 16) {
        int packed = source_byte(j);
        uint8_t bits[17];
        uint8_t values[256];
        int count = 0;
        bits[0] = 0;
        for (int i = 1; i <= 16; i++) {
            bits[i] = (uint8_t)source_byte(j);
            count += bits[i];
        }
        length -= 17;
        if (count > 256 || count > length) fail(j);
        for (int i = 0; i < count; i++) values[i] = (uint8_t)source_byte(j);
        memset(&values[count], 0, (size_t)(256 - count));
        length -= count;
        struct huff_table *table;
        if (packed & 0x10) {
            int slot = packed - 0x10;
            if (slot < 0 || slot >= NUM_HUFF_TBLS) fail(j);
            table = &j->ac_huff[slot];
        } else {
            int slot = packed;
            if (slot < 0 || slot >= NUM_HUFF_TBLS) fail(j);
            table = &j->dc_huff[slot];
        }
        memcpy(table->bits, bits, sizeof(table->bits));
        memcpy(table->value, values, sizeof(table->value));
        table->defined = true;
    }
    if (length != 0) fail(j);
}

static void get_dri(struct jpeg *j) {
    int length = read_u16(j);
    if (length != 4) fail(j);
    j->restart_interval = read_u16(j);
}

static void get_dac(struct jpeg *j) {
    int length = read_u16(j);
    length -= 2;
    while (length > 0) {
        int index = source_byte(j);
        int value = source_byte(j);
        length -= 2;
        if (index < 0 || index >= 2 * NUM_ARITH_TBLS) fail(j);
        if (index >= NUM_ARITH_TBLS) {
            j->arith_ac_k[index - NUM_ARITH_TBLS] = (uint8_t)value;
        } else {
            j->arith_dc_l[index] = (uint8_t)(value & 15);
            j->arith_dc_u[index] = (uint8_t)(value >> 4);
            if (j->arith_dc_l[index] > j->arith_dc_u[index]) fail(j);
        }
    }
    if (length != 0) fail(j);
}

static void skip_variable(struct jpeg *j) {
    int length = read_u16(j);
    length -= 2;
    if (length > 0) skip_bytes(j, length);
}

static void examine_app0(struct jpeg *j, const uint8_t *data, int length) {
    if (length >= 14 && data[0] == 'J' && data[1] == 'F' && data[2] == 'I' &&
            data[3] == 'F' && data[4] == 0) {
        j->saw_jfif = true;
    }
}

static void examine_app14(struct jpeg *j, const uint8_t *data, int length) {
    if (length >= 12 && data[0] == 'A' && data[1] == 'd' && data[2] == 'o' &&
            data[3] == 'b' && data[4] == 'e') {
        j->saw_adobe = true;
        j->adobe_transform = data[11];
    }
}

static void get_interesting_appn(struct jpeg *j) {
    int length = read_u16(j);
    uint8_t data[14];
    int payload;
    length -= 2;
    payload = length >= 14 ? 14 : (length > 0 ? length : 0);
    for (int i = 0; i < payload; i++) data[i] = (uint8_t)source_byte(j);
    length -= payload;
    if (j->unread_marker == M_APP0) examine_app0(j, data, payload);
    else if (j->unread_marker == M_APP14) examine_app14(j, data, payload);
    else fail(j);
    if (length > 0) skip_bytes(j, length);
}

static void initial_setup(struct jpeg *j) {
    int data_unit = j->lossless ? 1 : DCTSIZE;
    if (j->height > JPEG_MAX_DIMENSION || j->width > JPEG_MAX_DIMENSION) fail_hard(j);
    if (j->lossless) {
        if (j->precision < 2 || j->precision > 16) fail(j);
    } else {
        if (j->precision != 8 && j->precision != 12) fail(j);
    }
    if (j->num_components > MAX_COMPONENTS) fail(j);
    j->max_h = 1;
    j->max_v = 1;
    for (int ci = 0; ci < j->num_components; ci++) {
        struct component *comp = &j->comp[ci];
        if (comp->h <= 0 || comp->h > MAX_SAMP_FACTOR ||
                comp->v <= 0 || comp->v > MAX_SAMP_FACTOR) fail(j);
        if (comp->h > j->max_h) j->max_h = comp->h;
        if (comp->v > j->max_v) j->max_v = comp->v;
    }
    for (int ci = 0; ci < j->num_components; ci++) {
        struct component *comp = &j->comp[ci];
        comp->width_in_blocks = (uint32_t)(((int64_t)j->width * comp->h +
                (int64_t)j->max_h * data_unit - 1) / ((int64_t)j->max_h * data_unit));
        comp->height_in_blocks = (uint32_t)(((int64_t)j->height * comp->v +
                (int64_t)j->max_v * data_unit - 1) / ((int64_t)j->max_v * data_unit));
        comp->downsampled_width = (uint32_t)(((int64_t)j->width * comp->h + j->max_h - 1) / j->max_h);
        comp->downsampled_height = (uint32_t)(((int64_t)j->height * comp->v + j->max_v - 1) / j->max_v);
        comp->quant_defined = false;
    }
    j->total_iMCU_rows = (uint32_t)(((int64_t)j->height +
            (int64_t)j->max_v * data_unit - 1) / ((int64_t)j->max_v * data_unit));
    j->has_multiple_scans = j->comps_in_scan < j->num_components || j->progressive;
}

static void default_parms(struct jpeg *j) {
    switch (j->num_components) {
    case 1:
        j->jpeg_color = CS_GRAYSCALE;
        j->out_color = CS_GRAYSCALE;
        break;
    case 3:
        if (j->saw_jfif) {
            j->jpeg_color = CS_YCBCR;
        } else if (j->saw_adobe) {
            j->jpeg_color = j->adobe_transform == 0 ? CS_RGB : CS_YCBCR;
        } else {
            int cid0 = j->comp[0].id, cid1 = j->comp[1].id, cid2 = j->comp[2].id;
            if (cid0 == 1 && cid1 == 2 && cid2 == 3) j->jpeg_color = j->lossless ? CS_RGB : CS_YCBCR;
            else if (cid0 == 'R' && cid1 == 'G' && cid2 == 'B') j->jpeg_color = CS_RGB;
            else j->jpeg_color = j->lossless ? CS_RGB : CS_YCBCR;
        }
        j->out_color = CS_RGB;
        break;
    case 4:
        if (j->saw_adobe) {
            if (j->adobe_transform == 0) j->jpeg_color = CS_CMYK;
            else j->jpeg_color = CS_YCCK;
        } else {
            j->jpeg_color = CS_CMYK;
        }
        j->out_color = CS_CMYK;
        break;
    default:
        j->jpeg_color = CS_UNKNOWN;
        j->out_color = CS_UNKNOWN;
        break;
    }
    if (j->jpeg_color == CS_CMYK || j->jpeg_color == CS_YCCK) j->out_color = CS_CMYK;
    else j->out_color = CS_RGB;
}

static int read_markers(struct jpeg *j) {
    for (;;) {
        if (j->unread_marker == 0) {
            if (!j->saw_soi) first_marker(j);
            else next_marker(j);
        }
        switch (j->unread_marker) {
        case M_SOI: get_soi(j); break;
        case M_SOF0: get_sof(j, false, false, false); break;
        case M_SOF1: get_sof(j, false, false, false); break;
        case M_SOF2: get_sof(j, true, false, false); break;
        case M_SOF3: get_sof(j, false, true, false); break;
        case M_SOF9: get_sof(j, false, false, true); break;
        case M_SOF10: get_sof(j, true, false, true); break;
        case M_SOF11: get_sof(j, false, true, true); break;
        case M_SOF5: case M_SOF6: case M_SOF7: case M_JPG:
        case M_SOF13: case M_SOF14: case M_SOF15:
            fail(j);
            break;
        case M_SOS:
            get_sos(j);
            j->unread_marker = 0;
            if (j->in_headers) {
                initial_setup(j);
                j->in_headers = false;
            }
            return 1;
        case M_EOI:
            j->unread_marker = 0;
            return 0;
        case M_DAC: get_dac(j); break;
        case M_DHT: get_dht(j); break;
        case M_DQT: get_dqt(j); break;
        case M_DRI: get_dri(j); break;
        case M_APP0: case M_APP14: get_interesting_appn(j); break;
        case M_APP0 + 1: case M_APP0 + 2: case M_APP0 + 3: case M_APP0 + 4:
        case M_APP0 + 5: case M_APP0 + 6: case M_APP0 + 7: case M_APP0 + 8:
        case M_APP0 + 9: case M_APP0 + 10: case M_APP0 + 11: case M_APP0 + 12:
        case M_APP0 + 13: case M_APP15:
        case M_COM:
            skip_variable(j);
            break;
        case M_RST0: case M_RST0 + 1: case M_RST0 + 2: case M_RST0 + 3:
        case M_RST0 + 4: case M_RST0 + 5: case M_RST0 + 6: case M_RST7:
        case M_TEM:
            break;
        case M_DNL:
            skip_variable(j);
            break;
        default:
            fail(j);
            break;
        }
        j->unread_marker = 0;
    }
}

static void read_restart_marker(struct jpeg *j) {
    if (j->unread_marker == 0) next_marker(j);
    if (j->unread_marker == M_RST0 + j->next_restart) {
        j->unread_marker = 0;
    } else {
        int marker = j->unread_marker;
        int desired = j->next_restart;
        int action = 1;
        for (;;) {
            if (marker < M_SOF0) action = 2;
            else if (marker < M_RST0 || marker > M_RST7) action = 3;
            else if (marker == M_RST0 + ((desired + 1) & 7) ||
                     marker == M_RST0 + ((desired + 2) & 7)) action = 3;
            else if (marker == M_RST0 + ((desired - 1) & 7) ||
                     marker == M_RST0 + ((desired - 2) & 7)) action = 2;
            else action = 1;
            if (action == 1) {
                j->unread_marker = 0;
                break;
            }
            if (action == 2) {
                next_marker(j);
                marker = j->unread_marker;
                continue;
            }
            break;
        }
    }
    j->next_restart = (j->next_restart + 1) & 7;
}

static void per_scan_setup(struct jpeg *j) {
    int data_unit = j->lossless ? 1 : DCTSIZE;
    if (j->comps_in_scan == 1) {
        struct component *comp = &j->comp[j->scan_comp[0]];
        j->mcus_per_row = comp->width_in_blocks;
        j->mcu_rows_in_scan = comp->height_in_blocks;
        comp->mcu_width = 1;
        comp->mcu_height = 1;
        comp->last_col_width = 1;
        int tmp = (int)(comp->height_in_blocks % (uint32_t)comp->v);
        comp->last_row_height = tmp ? (uint32_t)tmp : (uint32_t)comp->v;
        j->blocks_in_MCU = 1;
        j->membership[0] = 0;
    } else {
        if (j->comps_in_scan > MAX_COMPS_IN_SCAN) fail(j);
        j->mcus_per_row = (uint32_t)(((int64_t)j->width +
                (int64_t)j->max_h * data_unit - 1) / ((int64_t)j->max_h * data_unit));
        j->mcu_rows_in_scan = (uint32_t)(((int64_t)j->height +
                (int64_t)j->max_v * data_unit - 1) / ((int64_t)j->max_v * data_unit));
        j->blocks_in_MCU = 0;
        for (int ci = 0; ci < j->comps_in_scan; ci++) {
            struct component *comp = &j->comp[j->scan_comp[ci]];
            comp->mcu_width = (uint32_t)comp->h;
            comp->mcu_height = (uint32_t)comp->v;
            int tmp = (int)(comp->width_in_blocks % (uint32_t)comp->h);
            comp->last_col_width = tmp ? (uint32_t)tmp : (uint32_t)comp->h;
            tmp = (int)(comp->height_in_blocks % (uint32_t)comp->v);
            comp->last_row_height = tmp ? (uint32_t)tmp : (uint32_t)comp->v;
            int blocks = (int)(comp->mcu_width * comp->mcu_height);
            if (j->blocks_in_MCU + blocks > D_MAX_BLOCKS_IN_MCU) fail(j);
            for (int b = 0; b < blocks; b++) j->membership[j->blocks_in_MCU++] = ci;
        }
    }
}

static void latch_quant(struct jpeg *j) {
    for (int i = 0; i < j->comps_in_scan; i++) {
        struct component *comp = &j->comp[j->scan_comp[i]];
        if (comp->quant_defined) continue;
        int slot = comp->tq;
        if (slot < 0 || slot >= NUM_QUANT_TBLS || !j->quant[slot].defined) fail(j);
        memcpy(comp->quant, j->quant[slot].value, sizeof(comp->quant));
        for (int k = 0; k < DCTSIZE2; k++) comp->multiplier[k] = (int16_t)comp->quant[k];
        comp->quant_defined = true;
    }
}

static void entropy_start(struct jpeg *j) {
    j->bits.buffer = 0;
    j->bits.left = 0;
    j->insufficient_data = false;
    j->eobrun = 0;
    j->restarts_to_go = j->restart_interval;
    if (j->lossless) {
        if (j->arith) fail(j);
        if (j->ss < 1 || j->ss > 7 || j->se != 0 || j->ah != 0 ||
                j->al < 0 || j->al >= j->precision) fail(j);
        for (int ci = 0; ci < j->comps_in_scan; ci++) {
            struct component *comp = &j->comp[j->scan_comp[ci]];
            build_scan_derived(j, true, comp->td);
            j->last_dc[ci] = 0;
        }
        for (int ci = 0; ci < j->num_components; ci++) j->undiff_first[ci] = true;
        if (j->mcus_per_row == 0 || (j->restart_interval % (int)j->mcus_per_row) != 0) fail(j);
        j->restart_rows_to_go = j->mcus_per_row ?
                (unsigned int)(j->restart_interval / (int)j->mcus_per_row) : 0;
        return;
    }
    if (j->arith) {
        if (j->progressive) {
            bool bad = false;
            if (j->ss == 0) { if (j->se != 0) bad = true; }
            else {
                if (j->se < j->ss || j->se > DCTSIZE2 - 1) bad = true;
                if (j->comps_in_scan != 1) bad = true;
            }
            if (j->ah != 0 && j->ah - 1 != j->al) bad = true;
            if (j->al > 13) bad = true;
            if (bad) fail(j);
            update_progression(j);
            j->entropy = ENTROPY_ARITH;
        } else {
            j->entropy = ENTROPY_ARITH;
        }
        for (int i = 0; i < j->comps_in_scan; i++) {
            struct component *comp = &j->comp[j->scan_comp[i]];
            if (!j->progressive || (j->ss == 0 && j->ah == 0)) {
                int tbl = comp->td;
                if (tbl < 0 || tbl >= NUM_ARITH_TBLS) fail(j);
                memset(j->dc_stats[tbl], 0, sizeof(j->dc_stats[tbl]));
                j->arith_last_dc[i] = 0;
                j->arith_dc_context[i] = 0;
            }
            if (!j->progressive || j->ss) {
                int tbl = comp->ta;
                if (tbl < 0 || tbl >= NUM_ARITH_TBLS) fail(j);
                memset(j->ac_stats[tbl], 0, sizeof(j->ac_stats[tbl]));
            }
        }
        j->arith_c = 0;
        j->arith_a = 0;
        j->arith_ct = -16;
        j->fixed_bin[0] = 113;
        return;
    }
    if (j->progressive) {
        bool bad = false;
        if (j->ss == 0) { if (j->se != 0) bad = true; }
        else {
            if (j->ss > j->se || j->se >= DCTSIZE2) bad = true;
            if (j->comps_in_scan != 1) bad = true;
        }
        if (j->ah != 0 && j->al != j->ah - 1) bad = true;
        if (j->al > 13) bad = true;
        if (bad) fail(j);
        update_progression(j);
        for (int i = 0; i < j->comps_in_scan; i++) {
            struct component *comp = &j->comp[j->scan_comp[i]];
            if (j->ss == 0) {
                if (j->ah == 0) build_scan_derived(j, true, comp->td);
            } else {
                build_scan_derived(j, false, comp->ta);
            }
            j->last_dc[i] = 0;
        }
        j->entropy = ENTROPY_PHUFF;
        return;
    }
    for (int i = 0; i < j->comps_in_scan; i++) {
        struct component *comp = &j->comp[j->scan_comp[i]];
        build_scan_derived(j, true, comp->td);
        build_scan_derived(j, false, comp->ta);
        j->last_dc[i] = 0;
    }
    j->entropy = ENTROPY_HUFF;
}

static void process_restart(struct jpeg *j) {
    if (j->arith) {
        read_restart_marker(j);
        for (int i = 0; i < j->comps_in_scan; i++) {
            struct component *comp = &j->comp[j->scan_comp[i]];
            if (!j->progressive || (j->ss == 0 && j->ah == 0)) {
                memset(j->dc_stats[comp->td], 0, sizeof(j->dc_stats[comp->td]));
                j->arith_last_dc[i] = 0;
                j->arith_dc_context[i] = 0;
            }
            if (!j->progressive || j->ss) {
                memset(j->ac_stats[comp->ta], 0, sizeof(j->ac_stats[comp->ta]));
            }
        }
        j->arith_c = 0;
        j->arith_a = 0;
        j->arith_ct = -16;
        j->restarts_to_go = j->restart_interval;
        return;
    }
    j->bits.left = 0;
    read_restart_marker(j);
    for (int i = 0; i < j->comps_in_scan; i++) j->last_dc[i] = 0;
    j->eobrun = 0;
    j->restarts_to_go = j->restart_interval;
    if (j->unread_marker == 0) j->insufficient_data = false;
    if (j->lossless) {
        for (int ci = 0; ci < j->num_components; ci++) j->undiff_first[ci] = true;
        j->restart_rows_to_go = j->mcus_per_row ?
                (unsigned int)(j->restart_interval / (int)j->mcus_per_row) : 0;
    }
}

#define FAST_BYTES_PER_BLOCK 512

static void fast_fill(struct jpeg *j, uint64_t *buffer, int *left, size_t *position) {
    if (*left > 16) return;
    for (int i = 0; i < 6; i++) {
        int first = j->data[*position];
        int second = j->data[*position + 1];
        (*position)++;
        *buffer = (*buffer << 8) | (uint64_t)first;
        *left += 8;
        if (first == 0xff) {
            (*position)++;
            if (second != 0) {
                j->unread_marker = second;
                *position -= 2;
                *buffer &= ~(uint64_t)0xff;
            }
        }
    }
}

static int fast_get_bits(uint64_t buffer, int *left, int count) {
    *left -= count;
    return (int)((buffer >> *left) & ((UINT64_C(1) << count) - 1));
}

static int huff_decode_fast(struct jpeg *j, uint64_t *buffer, int *left,
        size_t *position, const struct derived_table *table) {
    fast_fill(j, buffer, left, position);
    int entry = table->lookup[(int)((*buffer >> (*left - 8)) & 0xff)];
    int length = entry >> 8;
    int symbol = entry & 0xff;
    *left -= length;
    if (length > 8) {
        int code = (int)((*buffer >> *left) & ((UINT64_C(1) << length) - 1));
        while (code > table->maxcode[length]) {
            code = (code << 1) | fast_get_bits(*buffer, left, 1);
            length++;
        }
        if (length > 16) symbol = 0;
        else symbol = table->value[(code + table->valoffset[length]) & 0xff];
    }
    return symbol;
}

static bool decode_sequential_fast_mcu(struct jpeg *j, int16_t **blocks,
        const int *comps, int count) {
    uint64_t buffer = j->bits.buffer;
    int left = j->bits.left;
    size_t position = j->offset;
    for (int blkn = 0; blkn < count; blkn++) {
        struct component *comp = &j->comp[j->scan_comp[comps[blkn]]];
        const struct derived_table *dc = &j->dc_derived[comp->td];
        const struct derived_table *ac = &j->ac_derived[comp->ta];
        int16_t *block = blocks[blkn];
        int s = huff_decode_fast(j, &buffer, &left, &position, dc);
        if (s) {
            fast_fill(j, &buffer, &left, &position);
            s = extend_value(fast_get_bits(buffer, &left, s), s);
        }
        j->last_dc[comps[blkn]] = (int32_t)((uint32_t)j->last_dc[comps[blkn]] + (uint32_t)s);
        block[0] = (int16_t)j->last_dc[comps[blkn]];
        for (int k = 1; k < DCTSIZE2; k++) {
            s = huff_decode_fast(j, &buffer, &left, &position, ac);
            int r = s >> 4;
            s &= 15;
            if (s) {
                k += r;
                fast_fill(j, &buffer, &left, &position);
                block[natural_order[k]] = (int16_t)extend_value(fast_get_bits(buffer, &left, s), s);
            } else {
                if (r != 15) break;
                k += 15;
            }
        }
    }
    if (j->unread_marker != 0) {
        j->unread_marker = 0;
        return false;
    }
    j->bits.buffer = buffer;
    j->bits.left = left;
    j->offset = position;
    return true;
}

static void decode_sequential_mcu(struct jpeg *j, int16_t **blocks, const int *comps, int count) {
    if (j->restart_interval) {
        if (j->restarts_to_go == 0) process_restart(j);
    }
    if (!j->insufficient_data) {
        size_t remaining = j->fake_active ? 0 : j->length - j->offset;
        if (!j->restart_interval && j->unread_marker == 0 &&
                remaining >= (size_t)FAST_BYTES_PER_BLOCK * (size_t)count) {
            struct bit_reader saved_bits = j->bits;
            size_t saved_offset = j->offset;
            int saved_dc[MAX_COMPS_IN_SCAN];
            memcpy(saved_dc, j->last_dc, sizeof(saved_dc));
            if (decode_sequential_fast_mcu(j, blocks, comps, count)) return;
            j->bits = saved_bits;
            j->offset = saved_offset;
            memcpy(j->last_dc, saved_dc, sizeof(saved_dc));
        }
        for (int blkn = 0; blkn < count; blkn++) {
            struct component *comp = &j->comp[j->scan_comp[comps[blkn]]];
            const struct derived_table *dc = &j->dc_derived[comp->td];
            const struct derived_table *ac = &j->ac_derived[comp->ta];
            int16_t *block = blocks[blkn];
            int s = huff_decode(j, dc);
            if (s) s = extend_value(get_bits(j, s), s);
            j->last_dc[comps[blkn]] = (int32_t)((uint32_t)j->last_dc[comps[blkn]] + (uint32_t)s);
            *block = (int16_t)j->last_dc[comps[blkn]];
            for (int k = 1; k < DCTSIZE2; k++) {
                s = huff_decode(j, ac);
                int r = s >> 4;
                s &= 15;
                if (s) {
                    k += r;
                    block[natural_order[k]] = (int16_t)extend_value(get_bits(j, s), s);
                } else {
                    if (r != 15) break;
                    k += 15;
                }
            }
        }
    }
    if (j->restart_interval) j->restarts_to_go--;
}

static void decode_progressive_mcu(struct jpeg *j, int16_t **blocks, const int *comps) {
    if (j->restart_interval) {
        if (j->restarts_to_go == 0) process_restart(j);
    }
    if (j->ss == 0 && j->ah == 0) {
        if (!j->insufficient_data) {
            for (int blkn = 0; blkn < j->blocks_in_MCU; blkn++) {
                struct component *comp = &j->comp[j->scan_comp[comps[blkn]]];
                const struct derived_table *dc = &j->dc_derived[comp->td];
                int s = huff_decode(j, dc);
                if (s) s = extend_value(get_bits(j, s), s);
                j->last_dc[comps[blkn]] = (int32_t)((uint32_t)j->last_dc[comps[blkn]] + (uint32_t)s);
                blocks[blkn][0] = (int16_t)left_shift(j->last_dc[comps[blkn]], j->al);
            }
        }
    } else if (j->ss == 0) {
        int p1 = 1 << j->al;
        for (int blkn = 0; blkn < j->blocks_in_MCU; blkn++) {
            if (get_bits(j, 1)) blocks[blkn][0] = (int16_t)(blocks[blkn][0] | p1);
        }
    } else if (j->ah == 0) {
        unsigned int eobrun;
        int16_t *block;
        const struct derived_table *ac;
        int se = j->se, al = j->al;
        if (!j->insufficient_data) {
            eobrun = j->eobrun;
            if (eobrun > 0) {
                eobrun--;
            } else {
                block = blocks[0];
                ac = &j->ac_derived[j->comp[j->scan_comp[0]].ta];
                for (int k = j->ss; k <= se; k++) {
                    int s = huff_decode(j, ac);
                    int r = s >> 4;
                    s &= 15;
                    if (s) {
                        k += r;
                        block[natural_order[k]] = (int16_t)left_shift(extend_value(get_bits(j, s), s), al);
                    } else {
                        if (r == 15) {
                            k += 15;
                        } else {
                            eobrun = (unsigned)(1u << r);
                            if (r) eobrun += (unsigned)get_bits(j, r);
                            eobrun--;
                            break;
                        }
                    }
                }
            }
            j->eobrun = eobrun;
        }
    } else {
        int p1 = 1 << j->al;
        int m1 = (int)(((unsigned int)-1) << j->al);
        unsigned int eobrun;
        int16_t *block;
        const struct derived_table *ac;
        int se = j->se;
        int k;
        if (!j->insufficient_data) {
            eobrun = j->eobrun;
            block = blocks[0];
            ac = &j->ac_derived[j->comp[j->scan_comp[0]].ta];
            k = j->ss;
            if (eobrun == 0) {
                for (; k <= se; k++) {
                    int s = huff_decode(j, ac);
                    int r = s >> 4;
                    s &= 15;
                    if (s) {
                        s = get_bits(j, 1) ? p1 : m1;
                    } else {
                        if (r != 15) {
                            eobrun = (unsigned)(1u << r);
                            if (r) eobrun += (unsigned)get_bits(j, r);
                            break;
                        }
                    }
                    do {
                        int16_t *thiscoef = block + natural_order[k];
                        if (*thiscoef != 0) {
                            if (get_bits(j, 1)) {
                                if ((*thiscoef & p1) == 0) {
                                    if (*thiscoef >= 0) *thiscoef = (int16_t)(*thiscoef + p1);
                                    else *thiscoef = (int16_t)(*thiscoef + m1);
                                }
                            }
                        } else {
                            if (--r < 0) break;
                        }
                        k++;
                    } while (k <= se);
                    if (s) block[natural_order[k]] = (int16_t)s;
                }
            }
            if (eobrun > 0) {
                for (; k <= se; k++) {
                    int16_t *thiscoef = block + natural_order[k];
                    if (*thiscoef != 0) {
                        if (get_bits(j, 1)) {
                            if ((*thiscoef & p1) == 0) {
                                if (*thiscoef >= 0) *thiscoef = (int16_t)(*thiscoef + p1);
                                else *thiscoef = (int16_t)(*thiscoef + m1);
                            }
                        }
                    }
                }
                eobrun--;
            }
            j->eobrun = eobrun;
        }
    }
    if (j->restart_interval) j->restarts_to_go--;
}

static int arith_decode(struct jpeg *j, uint8_t *state) {
    int64_t qe, temp;
    int sv, data;
    uint8_t nl, nm;
    while (j->arith_a < 0x8000L) {
        if (--j->arith_ct < 0) {
            if (j->unread_marker) {
                data = 0;
            } else {
                data = source_byte(j);
                if (data == 0xff) {
                    do { data = source_byte(j); } while (data == 0xff);
                    if (data == 0) {
                        data = 0xff;
                    } else {
                        j->unread_marker = data;
                        data = 0;
                    }
                }
            }
            j->arith_c = (j->arith_c << 8) | data;
            if ((j->arith_ct += 8) < 0) {
                if (++j->arith_ct == 0) j->arith_a = 0x8000L;
            }
        }
        j->arith_a <<= 1;
    }
    sv = *state;
    qe = arith_table[sv & 0x7f];
    nl = (uint8_t)(qe & 0xff);
    qe >>= 8;
    nm = (uint8_t)(qe & 0xff);
    qe >>= 8;
    temp = j->arith_a - qe;
    j->arith_a = temp;
    temp <<= j->arith_ct;
    if (j->arith_c >= temp) {
        j->arith_c -= temp;
        if (j->arith_a < qe) {
            j->arith_a = qe;
            *state = (uint8_t)((sv & 0x80) ^ nm);
        } else {
            j->arith_a = qe;
            *state = (uint8_t)((sv & 0x80) ^ nl);
            sv ^= 0x80;
        }
    } else if (j->arith_a < 0x8000L) {
        if (j->arith_a < qe) {
            *state = (uint8_t)((sv & 0x80) ^ nl);
            sv ^= 0x80;
        } else {
            *state = (uint8_t)((sv & 0x80) ^ nm);
        }
    }
    return sv >> 7;
}

static bool arith_decode_dc(struct jpeg *j, int index, uint8_t *state, int *value) {
    int sign;
    int m;
    if (arith_decode(j, state) == 0) {
        j->arith_dc_context[index] = 0;
    } else {
        sign = arith_decode(j, state + 1);
        state += 2 + sign;
        if ((m = arith_decode(j, state)) != 0) {
            state = j->dc_stats[j->comp[j->scan_comp[index]].td] + 20;
            while (arith_decode(j, state)) {
                if ((m <<= 1) == 0x8000) {
                    j->arith_ct = -1;
                    return false;
                }
                state += 1;
            }
        }
        int tbl = j->comp[j->scan_comp[index]].td;
        if (m < (int)((1L << j->arith_dc_l[tbl]) >> 1))
            j->arith_dc_context[index] = 0;
        else if (m > (int)((1L << j->arith_dc_u[tbl]) >> 1))
            j->arith_dc_context[index] = 12 + sign * 4;
        else
            j->arith_dc_context[index] = 4 + sign * 4;
        int v = m;
        state += 14;
        while (m >>= 1) if (arith_decode(j, state)) v |= m;
        v += 1;
        if (sign) v = -v;
        j->arith_last_dc[index] = (j->arith_last_dc[index] + v) & 0xffff;
    }
    *value = j->arith_last_dc[index];
    return true;
}

static void decode_arith_mcu(struct jpeg *j, int16_t **blocks, const int *comps) {
    if (j->restart_interval) {
        if (j->restarts_to_go == 0) process_restart(j);
        j->restarts_to_go--;
    }
    if (j->arith_ct == -1 && !(j->progressive && j->ss == 0 && j->ah != 0)) return;
    if (j->progressive) {
        if (j->ss == 0 && j->ah == 0) {
            for (int blkn = 0; blkn < j->blocks_in_MCU; blkn++) {
                int index = comps[blkn];
                uint8_t *state = j->dc_stats[j->comp[j->scan_comp[index]].td] +
                        j->arith_dc_context[index];
                int value = 0;
                if (!arith_decode_dc(j, index, state, &value)) return;
                blocks[blkn][0] = (int16_t)left_shift(value, j->al);
            }
        } else if (j->ss == 0) {
            int p1 = 1 << j->al;
            uint8_t *state = j->fixed_bin;
            for (int blkn = 0; blkn < j->blocks_in_MCU; blkn++) {
                if (arith_decode(j, state)) blocks[blkn][0] = (int16_t)(blocks[blkn][0] | p1);
            }
        } else if (j->ah == 0) {
            int16_t *block = blocks[0];
            int tbl = j->comp[j->scan_comp[0]].ta;
            for (int k = j->ss; k <= j->se; k++) {
                uint8_t *state = j->ac_stats[tbl] + 3 * (k - 1);
                if (arith_decode(j, state)) break;
                while (arith_decode(j, state + 1) == 0) {
                    state += 3;
                    k++;
                    if (k > j->se) { j->arith_ct = -1; return; }
                }
                int sign = arith_decode(j, j->fixed_bin);
                state += 2;
                int m;
                if ((m = arith_decode(j, state)) != 0) {
                    if (arith_decode(j, state)) {
                        m <<= 1;
                        state = j->ac_stats[tbl] +
                                (k <= j->arith_ac_k[tbl] ? 189 : 217);
                        while (arith_decode(j, state)) {
                            if ((m <<= 1) == 0x8000) { j->arith_ct = -1; return; }
                            state += 1;
                        }
                    }
                }
                int v = m;
                state += 14;
                while (m >>= 1) if (arith_decode(j, state)) v |= m;
                v += 1;
                if (sign) v = -v;
                block[natural_order[k]] = (int16_t)((unsigned)v << j->al);
            }
        } else {
            int16_t *block = blocks[0];
            int tbl = j->comp[j->scan_comp[0]].ta;
            int p1 = 1 << j->al;
            int m1 = (int)(((unsigned int)-1) << j->al);
            int kex = j->se;
            for (; kex > 0; kex--) if (block[natural_order[kex]]) break;
            for (int k = j->ss; k <= j->se; k++) {
                uint8_t *state = j->ac_stats[tbl] + 3 * (k - 1);
                if (k > kex) if (arith_decode(j, state)) break;
                for (;;) {
                    int16_t *thiscoef = block + natural_order[k];
                    if (*thiscoef) {
                        if (arith_decode(j, state + 2)) {
                            if (*thiscoef < 0) *thiscoef = (int16_t)(*thiscoef + m1);
                            else *thiscoef = (int16_t)(*thiscoef + p1);
                        }
                        break;
                    }
                    if (arith_decode(j, state + 1)) {
                        if (arith_decode(j, j->fixed_bin)) *thiscoef = (int16_t)m1;
                        else *thiscoef = (int16_t)p1;
                        break;
                    }
                    state += 3;
                    k++;
                    if (k > j->se) { j->arith_ct = -1; return; }
                }
            }
        }
        return;
    }
    for (int blkn = 0; blkn < j->blocks_in_MCU; blkn++) {
        int index = comps[blkn];
        struct component *comp = &j->comp[j->scan_comp[index]];
        int16_t *block = blocks[blkn];
        uint8_t *state = j->dc_stats[comp->td] + j->arith_dc_context[index];
        int value = 0;
        if (!arith_decode_dc(j, index, state, &value)) return;
        *block = (int16_t)j->arith_last_dc[index];
        int tbl = comp->ta;
        for (int k = 1; k < DCTSIZE2; k++) {
            state = j->ac_stats[tbl] + 3 * (k - 1);
            if (arith_decode(j, state)) break;
            while (arith_decode(j, state + 1) == 0) {
                state += 3;
                k++;
                if (k > DCTSIZE2 - 1) { j->arith_ct = -1; return; }
            }
            int sign = arith_decode(j, j->fixed_bin);
            state += 2;
            int m;
            if ((m = arith_decode(j, state)) != 0) {
                if (arith_decode(j, state)) {
                    m <<= 1;
                    state = j->ac_stats[tbl] +
                            (k <= j->arith_ac_k[tbl] ? 189 : 217);
                    while (arith_decode(j, state)) {
                        if ((m <<= 1) == 0x8000) { j->arith_ct = -1; return; }
                        state += 1;
                    }
                }
            }
            int v = m;
            state += 14;
            while (m >>= 1) if (arith_decode(j, state)) v |= m;
            v += 1;
            if (sign) v = -v;
            block[natural_order[k]] = (int16_t)v;
        }
    }
}

static int16_t *component_block(struct jpeg *j, struct component *comp,
        uint32_t row, uint32_t col) {
    (void)j;
    return comp->coeff + ((size_t)row * comp->coeff_stride + col) * DCTSIZE2;
}

static uint8_t *component_sample(struct jpeg *j, struct component *comp,
        uint32_t row, uint32_t col) {
    (void)j;
    return comp->samples + (size_t)row * comp->sample_stride + col;
}

static void decode_lossy_mcu(struct jpeg *j) {
    int16_t *blocks[D_MAX_BLOCKS_IN_MCU];
    int comps[D_MAX_BLOCKS_IN_MCU];
    int count = 0;
    for (int i = 0; i < j->comps_in_scan; i++) {
        struct component *comp = &j->comp[j->scan_comp[i]];
        for (uint32_t y = 0; y < comp->mcu_height; y++) {
            for (uint32_t x = 0; x < comp->mcu_width; x++) {
                uint32_t row = j->input_iMCU_row * (uint32_t)comp->v + j->mcu_vertical + y;
                uint32_t col = j->mcu_col * comp->mcu_width + x;
                blocks[count] = component_block(j, comp, row, col);
                comps[count] = i;
                count++;
            }
        }
    }
    if (j->entropy == ENTROPY_HUFF) {
        decode_sequential_mcu(j, blocks, comps, count);
    } else if (j->entropy == ENTROPY_PHUFF) {
        decode_progressive_mcu(j, blocks, comps);
    } else {
        decode_arith_mcu(j, blocks, comps);
    }
}

static void decode_lossless_mcu_row(struct jpeg *j, uint32_t yoff) {
    bool exhausted = j->insufficient_data;
    for (uint32_t i = 0; i < j->mcus_per_row; i++) {
        for (int c = 0; c < j->comps_in_scan; c++) {
            int ci = j->scan_comp[c];
            struct component *comp = &j->comp[ci];
            const struct derived_table *table = &j->dc_derived[comp->td];
            for (uint32_t y = 0; y < comp->mcu_height; y++) {
                int32_t *diff = j->diff_rows[ci] + (yoff + y) * j->diff_stride[ci];
                for (uint32_t x = 0; x < comp->mcu_width; x++) {
                    int32_t value = 0;
                    if (!exhausted) {
                        int s = huff_decode(j, table);
                        if (s == 16) value = 32768;
                        else if (s) value = extend_value(get_bits(j, s), s);
                    }
                    diff[i * comp->mcu_width + x] = value;
                }
            }
        }
    }
}

static void decode_lossless_imcu(struct jpeg *j, uint32_t row) {
    for (int yoff = 0; yoff < j->mcu_rows_per_iMCU; yoff++) {
        if (j->restart_interval && j->restart_rows_to_go == 0) process_restart(j);
        bool exhausted = j->insufficient_data;
        decode_lossless_mcu_row(j, (uint32_t)yoff);
        if (exhausted) {
            for (int ci = 0; ci < j->num_components; ci++) j->undiff_first[ci] = true;
        }
        if (j->restart_interval) j->restart_rows_to_go--;
    }
    for (int c = 0; c < j->comps_in_scan; c++) {
        int ci = j->scan_comp[c];
        struct component *comp = &j->comp[ci];
        uint32_t rows = row == j->total_iMCU_rows - 1 ?
                comp->last_row_height : (uint32_t)comp->v;
        uint32_t width = comp->width_in_blocks;
        uint32_t *prev = j->undiff_prev[ci];
        uint32_t *cur = j->undiff_row;
        for (uint32_t y = 0; y < rows; y++) {
            uint32_t prow = row * (uint32_t)comp->v + y;
            int32_t *diff = j->diff_rows[ci] + y * j->diff_stride[ci];
            uint8_t *out = component_sample(j, comp, prow, 0);
            if (j->undiff_first[ci]) {
                uint32_t predictor = (uint32_t)(1 << (j->precision - j->al - 1));
                uint32_t ra = ((uint32_t)diff[0] + predictor) & 0xffffu;
                cur[0] = ra;
                for (uint32_t x = 1; x < width; x++) {
                    ra = ((uint32_t)diff[x] + ra) & 0xffffu;
                    cur[x] = ra;
                }
                j->undiff_first[ci] = false;
            } else if (j->ss == 1) {
                uint32_t ra = ((uint32_t)diff[0] + prev[0]) & 0xffffu;
                cur[0] = ra;
                for (uint32_t x = 1; x < width; x++) {
                    ra = ((uint32_t)diff[x] + ra) & 0xffffu;
                    cur[x] = ra;
                }
            } else {
                uint32_t rb = prev[0];
                uint32_t ra = ((uint32_t)diff[0] + rb) & 0xffffu;
                cur[0] = ra;
                for (uint32_t x = 1; x < width; x++) {
                    uint32_t rc = rb;
                    rb = prev[x];
                    int64_t predictor;
                    switch (j->ss) {
                    case 2: predictor = (int64_t)rb; break;
                    case 3: predictor = (int64_t)rc; break;
                    case 4: predictor = (int64_t)ra + rb - rc; break;
                    case 5: predictor = (int64_t)ra + (((int64_t)rb - rc) >> 1); break;
                    case 6: predictor = (int64_t)rb + (((int64_t)ra - rc) >> 1); break;
                    default: predictor = ((int64_t)ra + rb) >> 1; break;
                    }
                    ra = ((uint32_t)diff[x] + (uint32_t)(int)predictor) & 0xffffu;
                    cur[x] = ra;
                }
            }
            for (uint32_t x = 0; x < width; x++) out[x] = (uint8_t)(cur[x] << j->al);
            memcpy(prev, cur, (size_t)width * sizeof(uint32_t));
        }
    }
}

static void decode_scan(struct jpeg *j) {
    for (uint32_t row = 0; row < j->total_iMCU_rows; row++) {
        j->input_iMCU_row = row;
        if (!j->insufficient_data) j->last_good_row = (int)row;
        if (j->comps_in_scan > 1) {
            j->mcu_rows_per_iMCU = 1;
        } else {
            struct component *first = &j->comp[j->scan_comp[0]];
            j->mcu_rows_per_iMCU = (int)(row + 1 < j->total_iMCU_rows ?
                    (uint32_t)first->v : first->last_row_height);
        }
        if (j->lossless) {
            decode_lossless_imcu(j, row);
        } else {
            for (int yoff = 0; yoff < j->mcu_rows_per_iMCU; yoff++) {
                j->mcu_vertical = yoff;
                for (uint32_t col = 0; col < j->mcus_per_row; col++) {
                    j->mcu_col = col;
                    decode_lossy_mcu(j);
                }
            }
        }
    }
}

static int left_shift(int value, int shift) {
    return (int)((uint32_t)value << shift);
}

static void update_progression(struct jpeg *j) {
    for (int ci = 0; ci < j->comps_in_scan; ci++) {
        int cindex = j->scan_comp[ci];
        int *current = j->coef_bits[cindex];
        int *previous = j->coef_bits[cindex + j->num_components];
        int low = j->ss < 1 ? j->ss : 1;
        int high = j->se > 9 ? j->se : 9;
        for (int coefi = low; coefi <= high; coefi++)
            previous[coefi] = j->input_scan_number > 1 ? current[coefi] : 0;
        for (int coefi = j->ss; coefi <= j->se; coefi++) current[coefi] = j->al;
    }
}

static int16_t mul_low16(int16_t left, int16_t right) {
    return (int16_t)(uint16_t)((uint16_t)left * (uint16_t)right);
}

static int32_t pmadd16(int16_t first, int second, int16_t third, int fourth) {
    return (int32_t)((uint32_t)(int32_t)((int)first * second) +
            (uint32_t)(int32_t)((int)third * fourth));
}

static int32_t add32(int32_t left, int32_t right) {
    return (int32_t)((uint32_t)left + (uint32_t)right);
}

static int32_t sub32(int32_t left, int32_t right) {
    return (int32_t)((uint32_t)left - (uint32_t)right);
}

static int32_t sar32(int32_t value, int bits) {
    uint32_t raw = (uint32_t)value;
    if (value >= 0) return (int32_t)(raw >> bits);
    return (int32_t)~(uint32_t)((~raw) >> bits);
}

static int32_t descale32(int32_t value, int bits) {
    return sar32(add32(value, (int32_t)(UINT32_C(1) << (bits - 1))), bits);
}

static int16_t packssdw(int32_t value) {
    if (value < -32768) return -32768;
    if (value > 32767) return 32767;
    return (int16_t)value;
}

static uint8_t pack_output(int32_t value) {
    if (value < -128) value = -128;
    else if (value > 127) value = 127;
    return (uint8_t)(value + 128);
}

static void idct_1d(const int16_t in[8], int bits, int32_t out[8]) {
    int32_t tmp2 = pmadd16(in[2], 4433, in[6], -10704);
    int32_t tmp3 = pmadd16(in[2], 10703, in[6], 4433);
    int32_t tmp0 = (int32_t)(int16_t)(in[0] + in[4]) * 8192;
    int32_t tmp1 = (int32_t)(int16_t)(in[0] - in[4]) * 8192;
    int32_t tmp10 = add32(tmp0, tmp3);
    int32_t tmp13 = sub32(tmp0, tmp3);
    int32_t tmp11 = add32(tmp1, tmp2);
    int32_t tmp12 = sub32(tmp1, tmp2);
    int16_t z3s = (int16_t)(in[3] + in[7]);
    int16_t z4s = (int16_t)(in[1] + in[5]);
    int32_t z3 = pmadd16(z3s, -6436, z4s, 9633);
    int32_t z4 = pmadd16(z3s, 9633, z4s, 6437);
    int32_t t0 = add32(pmadd16(in[7], -4927, in[1], -7373), z3);
    int32_t t3 = add32(pmadd16(in[7], -7373, in[1], 4926), z4);
    int32_t t1 = add32(pmadd16(in[5], -4176, in[3], -20995), z4);
    int32_t t2 = add32(pmadd16(in[5], -20995, in[3], 4177), z3);
    out[0] = descale32(add32(tmp10, t3), bits);
    out[7] = descale32(sub32(tmp10, t3), bits);
    out[1] = descale32(add32(tmp11, t2), bits);
    out[6] = descale32(sub32(tmp11, t2), bits);
    out[2] = descale32(add32(tmp12, t1), bits);
    out[5] = descale32(sub32(tmp12, t1), bits);
    out[3] = descale32(add32(tmp13, t0), bits);
    out[4] = descale32(sub32(tmp13, t0), bits);
}

static void idct_islow(const int16_t *block, const int16_t *quant,
        uint8_t *out, uint32_t stride) {
    int16_t work[DCTSIZE][DCTSIZE];
    bool ac_zero = true;
    int32_t values[DCTSIZE];
    for (int k = 1; k < DCTSIZE && ac_zero; k++)
        for (int c = 0; c < DCTSIZE; c++)
            if (block[k * DCTSIZE + c] != 0) { ac_zero = false; break; }
    if (ac_zero) {
        for (int c = 0; c < DCTSIZE; c++) {
            int16_t dc = (int16_t)(uint16_t)((uint16_t)mul_low16(block[c], quant[c]) << 2);
            for (int r = 0; r < DCTSIZE; r++) work[c][r] = dc;
        }
    } else {
        for (int c = 0; c < DCTSIZE; c++) {
            int16_t in[DCTSIZE];
            for (int k = 0; k < DCTSIZE; k++)
                in[k] = mul_low16(block[k * DCTSIZE + c], quant[k * DCTSIZE + c]);
            idct_1d(in, 11, values);
            for (int r = 0; r < DCTSIZE; r++) work[c][r] = packssdw(values[r]);
        }
    }
    for (int r = 0; r < DCTSIZE; r++) {
        int16_t in[DCTSIZE];
        uint8_t *row = out + (size_t)r * stride;
        for (int k = 0; k < DCTSIZE; k++) in[k] = work[k][r];
        idct_1d(in, 18, values);
        for (int c = 0; c < DCTSIZE; c++) row[c] = pack_output(values[c]);
    }
}

#define ZSCALE_BITS 512 + 65536 + (128 << 17)
#define ZADD(a, b) ((int32_t)((uint32_t)(a) + (uint32_t)(b)))
#define ZSUB(a, b) ((int32_t)((uint32_t)(a) - (uint32_t)(b)))
#define ZMUL(a, b) ((int32_t)((uint32_t)(a) * (uint32_t)(b)))
#define ZSHL(a) ((int32_t)((uint32_t)(a) << 12))

static int16_t zune_clamp(int32_t a) {
    if (a < 0) return 0;
    if (a > 255) return 255;
    return (int16_t)a;
}

static void zune_idct_int(int32_t *v, uint8_t *out, uint32_t stride) {
    for (int ptr = 0; ptr < 8; ptr++) {
        int32_t p2 = v[ptr + 16], p3 = v[ptr + 48];
        int32_t p1 = ZMUL(ZADD(p2, p3), 2217);
        int32_t t2 = ZADD(p1, ZMUL(p3, -7567));
        int32_t t3 = ZADD(p1, ZMUL(p2, 3135));
        p2 = v[ptr];
        p3 = v[ptr + 32];
        int32_t t0 = ZSHL(ZADD(p2, p3));
        int32_t t1 = ZSHL(ZSUB(p2, p3));
        int32_t x0 = ZADD(ZADD(t0, t3), 512);
        int32_t x3 = ZADD(ZSUB(t0, t3), 512);
        int32_t x1 = ZADD(ZADD(t1, t2), 512);
        int32_t x2 = ZADD(ZSUB(t1, t2), 512);
        t0 = v[ptr + 56];
        t1 = v[ptr + 40];
        t2 = v[ptr + 24];
        t3 = v[ptr + 8];
        p3 = ZADD(t0, t2);
        int32_t p4 = ZADD(t1, t3);
        p1 = ZADD(t0, t3);
        p2 = ZADD(t1, t2);
        int32_t p5 = ZMUL(ZADD(p3, p4), 4816);
        t0 = ZMUL(t0, 1223);
        t1 = ZMUL(t1, 8410);
        t2 = ZMUL(t2, 12586);
        t3 = ZMUL(t3, 6149);
        p1 = ZADD(p5, ZMUL(p1, -3685));
        p2 = ZADD(p5, ZMUL(p2, -10497));
        p3 = ZMUL(p3, -8034);
        p4 = ZMUL(p4, -1597);
        t3 = ZADD(t3, ZADD(p1, p4));
        t2 = ZADD(t2, ZADD(p2, p3));
        t1 = ZADD(t1, ZADD(p2, p4));
        t0 = ZADD(t0, ZADD(p1, p3));
        v[ptr] = ZSUB(ZADD(x0, t3), 0) >> 10;
        v[ptr + 8] = ZSUB(ZADD(x1, t2), 0) >> 10;
        v[ptr + 16] = ZSUB(ZADD(x2, t1), 0) >> 10;
        v[ptr + 24] = ZSUB(ZADD(x3, t0), 0) >> 10;
        v[ptr + 32] = ZSUB(ZSUB(x3, t0), 0) >> 10;
        v[ptr + 40] = ZSUB(ZSUB(x2, t1), 0) >> 10;
        v[ptr + 48] = ZSUB(ZSUB(x1, t2), 0) >> 10;
        v[ptr + 56] = ZSUB(ZSUB(x0, t3), 0) >> 10;
    }
    for (int i = 0, pos = 0; i < 64; i += 8, pos += (int)stride) {
        int32_t p2 = v[i + 2], p3 = v[i + 6];
        int32_t p1 = ZMUL(ZADD(p2, p3), 2217);
        int32_t t2 = ZADD(p1, ZMUL(p3, -7567));
        int32_t t3 = ZADD(p1, ZMUL(p2, 3135));
        p2 = v[i];
        p3 = v[i + 4];
        int32_t t0 = ZADD(ZSHL(ZADD(p2, p3)), ZSCALE_BITS);
        int32_t t1 = ZADD(ZSHL(ZSUB(p2, p3)), ZSCALE_BITS);
        int32_t x0 = ZADD(ZADD(t0, t3), 0);
        int32_t x3 = ZADD(ZSUB(t0, t3), 0);
        int32_t x1 = ZADD(ZADD(t1, t2), 0);
        int32_t x2 = ZADD(ZSUB(t1, t2), 0);
        t0 = v[i + 7];
        t1 = v[i + 5];
        t2 = v[i + 3];
        t3 = v[i + 1];
        p3 = ZADD(t0, t2);
        int32_t p4 = ZADD(t1, t3);
        p1 = ZADD(t0, t3);
        p2 = ZADD(t1, t2);
        int32_t p5 = ZMUL(ZADD(p3, p4), 4816);
        t0 = ZMUL(t0, 1223);
        t1 = ZMUL(t1, 8410);
        t2 = ZMUL(t2, 12586);
        t3 = ZMUL(t3, 6149);
        p1 = ZADD(p5, ZMUL(p1, -3685));
        p2 = ZADD(p5, ZMUL(p2, -10497));
        p3 = ZMUL(p3, -8034);
        p4 = ZMUL(p4, -1597);
        t3 = ZADD(t3, ZADD(p1, p4));
        t2 = ZADD(t2, ZADD(p2, p3));
        t1 = ZADD(t1, ZADD(p2, p4));
        t0 = ZADD(t0, ZADD(p1, p3));
        uint8_t *row = out + (size_t)pos;
        row[0] = (uint8_t)zune_clamp(ZADD(x0, t3) >> 17);
        row[1] = (uint8_t)zune_clamp(ZADD(x1, t2) >> 17);
        row[2] = (uint8_t)zune_clamp(ZADD(x2, t1) >> 17);
        row[3] = (uint8_t)zune_clamp(ZADD(x3, t0) >> 17);
        row[4] = (uint8_t)zune_clamp(ZSUB(x3, t0) >> 17);
        row[5] = (uint8_t)zune_clamp(ZSUB(x2, t1) >> 17);
        row[6] = (uint8_t)zune_clamp(ZSUB(x1, t2) >> 17);
        row[7] = (uint8_t)zune_clamp(ZSUB(x0, t3) >> 17);
    }
}

static void zune_idct_block(const int16_t *block, const int16_t *quant,
        uint8_t *out, uint32_t stride) {
    int32_t v[64];
    int nonzero = 0;
    for (int i = 0; i < 64; i++) {
        v[i] = (int32_t)block[i] * (int32_t)quant[i];
        if (i) nonzero |= v[i];
    }
    if (!nonzero) {
        int32_t coeff = (v[0] + 4 + 1024) >> 3;
        coeff = zune_clamp(coeff);
        for (int r = 0; r < 8; r++)
            for (int c = 0; c < 8; c++) out[(size_t)r * stride + (size_t)c] = (uint8_t)coeff;
        return;
    }
    zune_idct_int(v, out, stride);
}

static uint32_t round_up(uint32_t value, uint32_t multiple) {
    return (value + multiple - 1) / multiple * multiple;
}

static bool smoothing_ok(struct jpeg *j) {
    bool useful = false;
    if (!j->progressive) return false;
    for (int ci = 0; ci < j->num_components; ci++) {
        struct component *comp = &j->comp[ci];
        const uint16_t *q = comp->quant;
        int *current = j->coef_bits[ci];
        if (!comp->quant_defined) return false;
        if (!q[0] || !q[1] || !q[8] || !q[16] || !q[9] || !q[2] ||
                !q[3] || !q[10] || !q[17] || !q[24]) return false;
        if (current[0] < 0) return false;
        j->smooth_cur[ci][0] = current[0];
        for (int coefi = 1; coefi < SAVED_COEFS; coefi++) {
            j->smooth_prev[ci][coefi] = j->input_scan_number > 1 ?
                    j->coef_bits[ci + j->num_components][coefi] : -1;
            j->smooth_cur[ci][coefi] = current[coefi];
            if (current[coefi] != 0) useful = true;
        }
    }
    return useful;
}

static void idct_component(struct jpeg *j, int ci) {
    struct component *comp = &j->comp[ci];
    uint32_t stride = comp->coeff_stride * DCTSIZE;
    uint8_t *out = comp->samples;
    const int16_t *quant = comp->multiplier;
    if (j->zune && !j->smooth) {
        for (uint32_t y = 0; y < comp->height_in_blocks; y++) {
            for (uint32_t x = 0; x < comp->width_in_blocks; x++) {
                const int16_t *block = comp->coeff +
                        ((size_t)y * comp->coeff_stride + x) * DCTSIZE2;
                zune_idct_block(block, quant, out + (size_t)y * DCTSIZE * stride +
                        (size_t)x * DCTSIZE, stride);
            }
        }
        return;
    }
    if (!j->smooth) {
        for (uint32_t y = 0; y < comp->height_in_blocks; y++) {
            for (uint32_t x = 0; x < comp->width_in_blocks; x++) {
                const int16_t *block = comp->coeff +
                        ((size_t)y * comp->coeff_stride + x) * DCTSIZE2;
                idct_islow(block, quant, out + (size_t)y * DCTSIZE * stride +
                        (size_t)x * DCTSIZE, stride);
            }
        }
        return;
    }
    int64_t Q00, Q01, Q02, Q03 = 0, Q10, Q11, Q12 = 0, Q20, Q21 = 0, Q30 = 0;
    int DC01, DC02, DC03, DC04, DC05, DC06, DC07, DC08, DC09, DC10;
    int DC11, DC12, DC13, DC14, DC15, DC16, DC17, DC18, DC19, DC20;
    int DC21, DC22, DC23, DC24, DC25;
    for (uint32_t rowi = 0; rowi < j->total_iMCU_rows; rowi++) {
        uint32_t block_rows = (rowi + 1 < j->total_iMCU_rows) ? (uint32_t)comp->v :
                (comp->height_in_blocks % (uint32_t)comp->v ?
                 comp->height_in_blocks % (uint32_t)comp->v : (uint32_t)comp->v);
        uint32_t image_block_rows = block_rows * j->total_iMCU_rows;
        for (uint32_t block_row = 0; block_row < block_rows; block_row++) {
            uint32_t image_block_row = rowi * block_rows + block_row;
            uint32_t y = rowi * (uint32_t)comp->v + block_row;
            const int16_t *blockrow = comp->coeff +
                    (size_t)y * comp->coeff_stride * DCTSIZE2;
            const int16_t *prevrow = comp->coeff +
                    (size_t)(image_block_row > 0 ? y - 1 : y) * comp->coeff_stride * DCTSIZE2;
            const int16_t *prevprevrow = image_block_row > 1 ? comp->coeff +
                    (size_t)(y - 2) * comp->coeff_stride * DCTSIZE2 : prevrow;
            const int16_t *nextrow = image_block_row + 1 < image_block_rows ? comp->coeff +
                    (size_t)(y + 1) * comp->coeff_stride * DCTSIZE2 : blockrow;
            const int16_t *nextnextrow = image_block_row + 2 < image_block_rows ? comp->coeff +
                    (size_t)(y + 2) * comp->coeff_stride * DCTSIZE2 : nextrow;
            const int *coef_bits = rowi > (uint32_t)j->last_good_row ?
                    j->smooth_prev[ci] : j->smooth_cur[ci];
            bool change_dc = coef_bits[1] == -1 && coef_bits[2] == -1 &&
                    coef_bits[3] == -1 && coef_bits[4] == -1 && coef_bits[5] == -1 &&
                    coef_bits[6] == -1 && coef_bits[7] == -1 && coef_bits[8] == -1 &&
                    coef_bits[9] == -1;
            const uint16_t *qt = comp->quant;
            Q00 = qt[0]; Q01 = qt[1]; Q10 = qt[8]; Q20 = qt[16]; Q11 = qt[9]; Q02 = qt[2];
            if (change_dc) { Q03 = qt[3]; Q12 = qt[10]; Q21 = qt[17]; Q30 = qt[24]; }
            uint32_t last_column = comp->width_in_blocks - 1;
            DC01 = DC02 = DC03 = DC04 = DC05 = prevprevrow[0];
            DC06 = DC07 = DC08 = DC09 = DC10 = prevrow[0];
            DC11 = DC12 = DC13 = DC14 = DC15 = blockrow[0];
            DC16 = DC17 = DC18 = DC19 = DC20 = nextrow[0];
            DC21 = DC22 = DC23 = DC24 = DC25 = nextnextrow[0];
            for (uint32_t x = 0; x < comp->width_in_blocks; x++) {
                const int16_t *prev = prevrow + (size_t)x * DCTSIZE2;
                const int16_t *prevprev = prevprevrow + (size_t)x * DCTSIZE2;
                const int16_t *next = nextrow + (size_t)x * DCTSIZE2;
                const int16_t *nextnext = nextnextrow + (size_t)x * DCTSIZE2;
                int16_t ws[DCTSIZE2];
                int Al, pred;
                int64_t num;
                memcpy(ws, blockrow + (size_t)x * DCTSIZE2, sizeof(ws));
                if (x == 0 && x < last_column) {
                    DC04 = DC05 = prevprev[1 * DCTSIZE2];
                    DC09 = DC10 = prev[1 * DCTSIZE2];
                    DC14 = DC15 = blockrow[(x + 1) * DCTSIZE2];
                    DC19 = DC20 = next[1 * DCTSIZE2];
                    DC24 = DC25 = nextnext[1 * DCTSIZE2];
                }
                if (x + 1 < last_column) {
                    DC05 = prevprev[2 * DCTSIZE2];
                    DC10 = prev[2 * DCTSIZE2];
                    DC15 = blockrow[(x + 2) * DCTSIZE2];
                    DC20 = next[2 * DCTSIZE2];
                    DC25 = nextnext[2 * DCTSIZE2];
                }
                if ((Al = coef_bits[1]) != 0 && ws[1] == 0) {
                    num = Q00 * (change_dc ?
                            (-DC01 - DC02 + DC04 + DC05 - 3 * DC06 + 13 * DC07 -
                             13 * DC09 + 3 * DC10 - 3 * DC11 + 38 * DC12 - 38 * DC14 +
                             3 * DC15 - 3 * DC16 + 13 * DC17 - 13 * DC19 + 3 * DC20 -
                             DC21 - DC22 + DC24 + DC25) :
                            (-7 * DC11 + 50 * DC12 - 50 * DC14 + 7 * DC15));
                    if (num >= 0) {
                        pred = (int)(((Q01 << 7) + num) / (Q01 << 8));
                        if (Al > 0 && pred >= (1 << Al)) pred = (1 << Al) - 1;
                    } else {
                        pred = (int)(((Q01 << 7) - num) / (Q01 << 8));
                        if (Al > 0 && pred >= (1 << Al)) pred = (1 << Al) - 1;
                        pred = -pred;
                    }
                    ws[1] = (int16_t)pred;
                }
                if ((Al = coef_bits[2]) != 0 && ws[8] == 0) {
                    num = Q00 * (change_dc ?
                            (-DC01 - 3 * DC02 - 3 * DC03 - 3 * DC04 - DC05 - DC06 +
                             13 * DC07 + 38 * DC08 + 13 * DC09 - DC10 + DC16 -
                             13 * DC17 - 38 * DC18 - 13 * DC19 + DC20 + DC21 +
                             3 * DC22 + 3 * DC23 + 3 * DC24 + DC25) :
                            (-7 * DC03 + 50 * DC08 - 50 * DC18 + 7 * DC23));
                    if (num >= 0) {
                        pred = (int)(((Q10 << 7) + num) / (Q10 << 8));
                        if (Al > 0 && pred >= (1 << Al)) pred = (1 << Al) - 1;
                    } else {
                        pred = (int)(((Q10 << 7) - num) / (Q10 << 8));
                        if (Al > 0 && pred >= (1 << Al)) pred = (1 << Al) - 1;
                        pred = -pred;
                    }
                    ws[8] = (int16_t)pred;
                }
                if ((Al = coef_bits[3]) != 0 && ws[16] == 0) {
                    num = Q00 * (change_dc ?
                            (DC03 + 2 * DC07 + 7 * DC08 + 2 * DC09 - 5 * DC12 - 14 * DC13 -
                             5 * DC14 + 2 * DC17 + 7 * DC18 + 2 * DC19 + DC23) :
                            (-DC03 + 13 * DC08 - 24 * DC13 + 13 * DC18 - DC23));
                    if (num >= 0) {
                        pred = (int)(((Q20 << 7) + num) / (Q20 << 8));
                        if (Al > 0 && pred >= (1 << Al)) pred = (1 << Al) - 1;
                    } else {
                        pred = (int)(((Q20 << 7) - num) / (Q20 << 8));
                        if (Al > 0 && pred >= (1 << Al)) pred = (1 << Al) - 1;
                        pred = -pred;
                    }
                    ws[16] = (int16_t)pred;
                }
                if ((Al = coef_bits[4]) != 0 && ws[9] == 0) {
                    num = Q00 * (change_dc ?
                            (-DC01 + DC05 + 9 * DC07 - 9 * DC09 - 9 * DC17 +
                             9 * DC19 + DC21 - DC25) :
                            (DC10 + DC16 - 10 * DC17 + 10 * DC19 - DC02 - DC20 + DC22 -
                             DC24 + DC04 - DC06 + 10 * DC07 - 10 * DC09));
                    if (num >= 0) {
                        pred = (int)(((Q11 << 7) + num) / (Q11 << 8));
                        if (Al > 0 && pred >= (1 << Al)) pred = (1 << Al) - 1;
                    } else {
                        pred = (int)(((Q11 << 7) - num) / (Q11 << 8));
                        if (Al > 0 && pred >= (1 << Al)) pred = (1 << Al) - 1;
                        pred = -pred;
                    }
                    ws[9] = (int16_t)pred;
                }
                if ((Al = coef_bits[5]) != 0 && ws[2] == 0) {
                    num = Q00 * (change_dc ?
                            (2 * DC07 - 5 * DC08 + 2 * DC09 + DC11 + 7 * DC12 - 14 * DC13 +
                             7 * DC14 + DC15 + 2 * DC17 - 5 * DC18 + 2 * DC19) :
                            (-DC11 + 13 * DC12 - 24 * DC13 + 13 * DC14 - DC15));
                    if (num >= 0) {
                        pred = (int)(((Q02 << 7) + num) / (Q02 << 8));
                        if (Al > 0 && pred >= (1 << Al)) pred = (1 << Al) - 1;
                    } else {
                        pred = (int)(((Q02 << 7) - num) / (Q02 << 8));
                        if (Al > 0 && pred >= (1 << Al)) pred = (1 << Al) - 1;
                        pred = -pred;
                    }
                    ws[2] = (int16_t)pred;
                }
                if (change_dc) {
                    if ((Al = coef_bits[6]) != 0 && ws[3] == 0) {
                        num = Q00 * (DC07 - DC09 + 2 * DC12 - 2 * DC14 + DC17 - DC19);
                        if (num >= 0) {
                            pred = (int)(((Q03 << 7) + num) / (Q03 << 8));
                            if (Al > 0 && pred >= (1 << Al)) pred = (1 << Al) - 1;
                        } else {
                            pred = (int)(((Q03 << 7) - num) / (Q03 << 8));
                            if (Al > 0 && pred >= (1 << Al)) pred = (1 << Al) - 1;
                            pred = -pred;
                        }
                        ws[3] = (int16_t)pred;
                    }
                    if ((Al = coef_bits[7]) != 0 && ws[10] == 0) {
                        num = Q00 * (DC07 - 3 * DC08 + DC09 - DC17 + 3 * DC18 - DC19);
                        if (num >= 0) {
                            pred = (int)(((Q12 << 7) + num) / (Q12 << 8));
                            if (Al > 0 && pred >= (1 << Al)) pred = (1 << Al) - 1;
                        } else {
                            pred = (int)(((Q12 << 7) - num) / (Q12 << 8));
                            if (Al > 0 && pred >= (1 << Al)) pred = (1 << Al) - 1;
                            pred = -pred;
                        }
                        ws[10] = (int16_t)pred;
                    }
                    if ((Al = coef_bits[8]) != 0 && ws[17] == 0) {
                        num = Q00 * (DC07 - DC09 - 3 * DC12 + 3 * DC14 + DC17 - DC19);
                        if (num >= 0) {
                            pred = (int)(((Q21 << 7) + num) / (Q21 << 8));
                            if (Al > 0 && pred >= (1 << Al)) pred = (1 << Al) - 1;
                        } else {
                            pred = (int)(((Q21 << 7) - num) / (Q21 << 8));
                            if (Al > 0 && pred >= (1 << Al)) pred = (1 << Al) - 1;
                            pred = -pred;
                        }
                        ws[17] = (int16_t)pred;
                    }
                    if ((Al = coef_bits[9]) != 0 && ws[24] == 0) {
                        num = Q00 * (DC07 + 2 * DC08 + DC09 - DC17 - 2 * DC18 - DC19);
                        if (num >= 0) {
                            pred = (int)(((Q30 << 7) + num) / (Q30 << 8));
                            if (Al > 0 && pred >= (1 << Al)) pred = (1 << Al) - 1;
                        } else {
                            pred = (int)(((Q30 << 7) - num) / (Q30 << 8));
                            if (Al > 0 && pred >= (1 << Al)) pred = (1 << Al) - 1;
                            pred = -pred;
                        }
                        ws[24] = (int16_t)pred;
                    }
                    num = Q00 *
                            (-2 * DC01 - 6 * DC02 - 8 * DC03 - 6 * DC04 - 2 * DC05 -
                             6 * DC06 + 6 * DC07 + 42 * DC08 + 6 * DC09 - 6 * DC10 -
                             8 * DC11 + 42 * DC12 + 152 * DC13 + 42 * DC14 - 8 * DC15 -
                             6 * DC16 + 6 * DC17 + 42 * DC18 + 6 * DC19 - 6 * DC20 -
                             2 * DC21 - 6 * DC22 - 8 * DC23 - 6 * DC24 - 2 * DC25);
                    if (num >= 0) {
                        pred = (int)(((Q00 << 7) + num) / (Q00 << 8));
                    } else {
                        pred = (int)(((Q00 << 7) - num) / (Q00 << 8));
                        pred = -pred;
                    }
                    ws[0] = (int16_t)pred;
                }
                idct_islow(ws, quant, out + (size_t)y * DCTSIZE * stride +
                        (size_t)x * DCTSIZE, stride);
                DC01 = DC02; DC02 = DC03; DC03 = DC04; DC04 = DC05;
                DC06 = DC07; DC07 = DC08; DC08 = DC09; DC09 = DC10;
                DC11 = DC12; DC12 = DC13; DC13 = DC14; DC14 = DC15;
                DC16 = DC17; DC17 = DC18; DC18 = DC19; DC19 = DC20;
                DC21 = DC22; DC22 = DC23; DC23 = DC24; DC24 = DC25;
            }
        }
    }
}

static const uint8_t *plane_row(struct jpeg *j, struct component *comp, int64_t row) {
    if (row < 0) row = 0;
    if (row >= (int64_t)comp->downsampled_height) row = (int64_t)comp->downsampled_height - 1;
    return comp->samples + (size_t)row * comp->sample_stride;
}

static void zune_horizontal(const uint8_t *in, uint32_t width, uint32_t stop, uint8_t *out) {
    uint32_t sample = (uint32_t)in[0] * 3 + 2;
    out[0] = in[0];
    if (stop > 1) out[1] = (uint8_t)((sample + in[1]) >> 2);
    for (uint32_t i = 1; 2 * i < stop && i + 1 < width; i++) {
        sample = (uint32_t)in[i] * 3 + 2;
        out[2 * i] = (uint8_t)((sample + in[i - 1]) >> 2);
        if (2 * i + 1 < stop) out[2 * i + 1] = (uint8_t)((sample + in[i + 1]) >> 2);
    }
    if (2 * width - 2 < stop) {
        sample = (uint32_t)in[width - 1] * 3 + 2;
        out[2 * width - 2] = (uint8_t)((sample + in[width - 2]) >> 2);
        if (2 * width - 1 < stop) out[2 * width - 1] = in[width - 1];
    }
}

static void zune_expand(uint8_t *row, uint32_t width, uint32_t stop) {
    if (2 * width - 2 < stop) {
        row[2 * width - 2] = (uint8_t)(((uint32_t)row[width - 1] * 3 + 2 + row[width - 2]) >> 2);
        if (2 * width - 1 < stop) row[2 * width - 1] = row[width - 1];
    }
    uint32_t top = (stop - 1) / 2;
    if (top > width - 2) top = width - 2;
    for (uint32_t i = top; i > 0; i--) {
        uint32_t sample = (uint32_t)row[i] * 3 + 2;
        if (2 * i + 1 < stop) row[2 * i + 1] = (uint8_t)((sample + row[i + 1]) >> 2);
        row[2 * i] = (uint8_t)((sample + row[i - 1]) >> 2);
    }
    if (stop > 1) {
        uint32_t sample = (uint32_t)row[0] * 3 + 2;
        row[1] = (uint8_t)((sample + row[1]) >> 2);
    }
}

static void zune_upsample_rows(struct jpeg *j, int ci, struct component *comp, uint32_t group) {
    uint32_t stride = j->up_stride;
    uint8_t *buffer = j->up_buffer[ci];
    uint32_t max_v = (uint32_t)j->max_v;
    uint32_t width = comp->sample_stride;
    uint32_t stop = j->output_width < 2 * width ? j->output_width : 2 * width;
    uint32_t base = group * (uint32_t)comp->v;
    if (j->up_method[ci] == UP_H2V1_FANCY) {
        for (uint32_t r = 0; r < max_v; r++)
            zune_horizontal(plane_row(j, comp, (int64_t)base + r), width, stop,
                    buffer + (size_t)r * stride);
        for (uint32_t r = 0; r < max_v; r++) j->rows[ci][r] = buffer + (size_t)r * stride;
        return;
    }
    uint32_t per_group = 8u * (uint32_t)comp->v;
    uint32_t last_group = (comp->sample_rows + per_group - 1) / per_group - 1;
    uint32_t outrow = 0;
    for (uint32_t k = 0; k < (uint32_t)comp->v && outrow < max_v; k++) {
        const uint8_t *cur = plane_row(j, comp, (int64_t)base + k);
        int64_t next = (int64_t)base + k + 1;
        if ((uint32_t)next % per_group == 0 && (uint32_t)next / per_group - 1 == last_group)
            next = (int64_t)base + k;
        if (next > (int64_t)comp->sample_rows - 1) next = (int64_t)comp->sample_rows - 1;
        for (uint32_t half = 0; half < 2 && outrow < max_v; half++) {
            const uint8_t *near = half ? comp->samples + (size_t)next * comp->sample_stride :
                    plane_row(j, comp, (int64_t)base + k - 1);
            uint8_t *out = buffer + (size_t)outrow++ * stride;
            for (uint32_t col = 0; col < width && col < stop; col++)
                out[col] = (uint8_t)(((uint32_t)cur[col] * 3 + 2 + near[col]) >> 2);
            if (j->up_method[ci] != UP_H1V2) zune_expand(out, width, stop);
        }
    }
    for (uint32_t r = 0; r < max_v; r++) j->rows[ci][r] = buffer + (size_t)r * stride;
}

static void fill_upsample_rows(struct jpeg *j, int ci, uint32_t group) {
    struct component *comp = &j->comp[ci];
    uint32_t max_v = (uint32_t)j->max_v;
    uint32_t base = group * (uint32_t)comp->v;
    if (j->up_method[ci] == UP_FULL) {
        for (uint32_t r = 0; r < max_v; r++)
            j->rows[ci][r] = plane_row(j, comp, (int64_t)base + r);
        return;
    }
    if (j->zune && (j->up_method[ci] == UP_H2V1_FANCY || j->up_method[ci] == UP_H2V2_FANCY ||
            j->up_method[ci] == UP_H1V2)) {
        zune_upsample_rows(j, ci, comp, group);
        return;
    }
    uint8_t *buffer = j->up_buffer[ci];
    uint32_t stride = j->up_stride;
    switch (j->up_method[ci]) {
    case UP_INT: {
        uint32_t h = j->up_h_expand[ci];
        uint32_t v = j->up_v_expand[ci];
        uint32_t outrow = 0, inrow = 0;
        while (outrow < max_v) {
            const uint8_t *in = plane_row(j, comp, (int64_t)base + inrow);
            uint8_t *out = buffer + (size_t)outrow * stride;
            for (uint32_t col = 0; col < j->output_width; ) {
                uint8_t value = in[col / h];
                for (uint32_t k = 0; k < h; k++) out[col++] = value;
            }
            for (uint32_t k = 1; k < v; k++)
                memcpy(buffer + (size_t)(outrow + k) * stride, out, j->output_width);
            inrow++;
            outrow += v;
        }
        break;
    }
    case UP_H2V1_BOX:
        for (uint32_t inrow = 0; inrow < max_v; inrow++) {
            const uint8_t *in = plane_row(j, comp, (int64_t)base + inrow);
            uint8_t *out = buffer + (size_t)inrow * stride;
            for (uint32_t col = 0; col < j->output_width; col += 2) {
                uint8_t value = in[col / 2];
                out[col] = value;
                out[col + 1] = value;
            }
        }
        break;
    case UP_H2V1_FANCY:
        for (uint32_t inrow = 0; inrow < max_v; inrow++) {
            const uint8_t *in = plane_row(j, comp, (int64_t)base + inrow);
            uint8_t *out = buffer + (size_t)inrow * stride;
            int value = *in++;
            *out++ = (uint8_t)value;
            *out++ = (uint8_t)((value * 3 + in[0] + 2) >> 2);
            for (uint32_t col = comp->downsampled_width - 2; col > 0; col--) {
                value = (*in++) * 3;
                *out++ = (uint8_t)((value + in[-2] + 1) >> 2);
                *out++ = (uint8_t)((value + in[0] + 2) >> 2);
            }
            value = *in;
            *out++ = (uint8_t)((value * 3 + in[-1] + 1) >> 2);
            *out++ = (uint8_t)value;
        }
        break;
    case UP_H2V2_BOX: {
        uint32_t outrow = 0, inrow = 0;
        while (outrow < max_v) {
            const uint8_t *in = plane_row(j, comp, (int64_t)base + inrow);
            uint8_t *out = buffer + (size_t)outrow * stride;
            for (uint32_t col = 0; col < j->output_width; col += 2) {
                uint8_t value = in[col / 2];
                out[col] = value;
                out[col + 1] = value;
            }
            memcpy(buffer + (size_t)(outrow + 1) * stride, out, j->output_width);
            inrow++;
            outrow += 2;
        }
        break;
    }
    case UP_H2V2_FANCY: {
        uint32_t outrow = 0, inrow = 0;
        uint32_t width = comp->downsampled_width;
        while (outrow < max_v) {
            for (int v = 0; v < 2; v++) {
                const uint8_t *in0 = plane_row(j, comp, (int64_t)base + inrow);
                const uint8_t *in1 = plane_row(j, comp,
                        (int64_t)base + inrow + (v == 0 ? -1 : 1));
                uint8_t *out = buffer + (size_t)outrow * stride;
                outrow++;
                int thissum = in0[0] * 3 + in1[0];
                int nextsum = in0[1] * 3 + in1[1];
                out[0] = (uint8_t)((thissum * 4 + 8) >> 4);
                out[1] = (uint8_t)((thissum * 3 + nextsum + 7) >> 4);
                int lastsum = thissum;
                thissum = nextsum;
                for (uint32_t col = 1; col + 1 < width; col++) {
                    nextsum = in0[col + 1] * 3 + in1[col + 1];
                    out[2 * col] = (uint8_t)((thissum * 3 + lastsum + 8) >> 4);
                    out[2 * col + 1] = (uint8_t)((thissum * 3 + nextsum + 7) >> 4);
                    lastsum = thissum;
                    thissum = nextsum;
                }
                out[2 * (width - 1)] = (uint8_t)((thissum * 3 + lastsum + 8) >> 4);
                out[2 * width - 1] = (uint8_t)((thissum * 4 + 7) >> 4);
            }
            inrow++;
        }
        break;
    }
    case UP_H1V2: {
        uint32_t outrow = 0, inrow = 0;
        uint32_t width = comp->downsampled_width;
        while (outrow < max_v) {
            for (int v = 0; v < 2; v++) {
                const uint8_t *in0 = plane_row(j, comp, (int64_t)base + inrow);
                const uint8_t *in1 = plane_row(j, comp,
                        (int64_t)base + inrow + (v == 0 ? -1 : 1));
                int bias = v == 0 ? 1 : 2;
                uint8_t *out = buffer + (size_t)outrow * stride;
                outrow++;
                for (uint32_t col = 0; col < width; col++)
                    out[col] = (uint8_t)((in0[col] * 3 + in1[col] + bias) >> 2);
            }
            inrow++;
        }
        break;
    }
    default:
        break;
    }
    for (uint32_t r = 0; r < max_v; r++) j->rows[ci][r] = buffer + (size_t)r * stride;
}

static int64_t fix_constant(double value) {
    return (int64_t)(value * 65536.0 + 0.5);
}

static void build_ycc_tables(struct jpeg *j) {
    int64_t cr_r = fix_constant(1.40200);
    int64_t cb_b = fix_constant(1.77200);
    int64_t cr_g = -fix_constant(0.71414);
    int64_t cb_g = -fix_constant(0.34414);
    for (int i = 0; i < 256; i++) {
        int64_t x = i - 128;
        j->cr_r[i] = (int)((cr_r * x + ((int64_t)1 << 15)) >> 16);
        j->cb_b[i] = (int)((cb_b * x + ((int64_t)1 << 15)) >> 16);
        j->cr_g[i] = cr_g * x;
        j->cb_g[i] = cb_g * x + ((int64_t)1 << 15);
    }
}

static void convert_row(struct jpeg *j, uint32_t image_row, uint32_t buffer_row) {
    unsigned char *dst = j->pixels + (size_t)image_row * j->output_width * 4;
    const uint8_t *in0 = j->rows[0][buffer_row];
    const uint8_t *in1 = j->rows[1][buffer_row];
    const uint8_t *in2 = j->rows[2][buffer_row];
    const uint8_t *in3 = j->conversion == CONV_NULL4 || j->conversion == CONV_YCCK_CMYK ?
            j->rows[3][buffer_row] : NULL;
    const uint8_t *range = j->range_base;
    for (uint32_t x = 0; x < j->output_width; x++) {
        unsigned red = 0, green = 0, blue = 0;
        if (j->conversion == CONV_NULL3) {
            red = in0[x]; green = in1[x]; blue = in2[x];
        } else if (j->conversion == CONV_GRAY_RGB) {
            red = green = blue = in0[x];
        } else if (j->conversion == CONV_YCC_RGB) {
            if (j->zune) {
                int yy = in0[x] * 16384 + 8191;
                int cbd = in1[x] - 128, crd = in2[x] - 128;
                int r = (yy + crd * 22970) >> 14;
                int g = (yy - crd * 11700 - cbd * 5638) >> 14;
                int b = (yy + cbd * 29032) >> 14;
                red = (unsigned)(r < 0 ? 0 : r > 255 ? 255 : r);
                green = (unsigned)(g < 0 ? 0 : g > 255 ? 255 : g);
                blue = (unsigned)(b < 0 ? 0 : b > 255 ? 255 : b);
            } else {
                int y = in0[x], cb = in1[x], cr = in2[x];
                red = range[y + j->cr_r[cr]];
                green = range[y + (int)((j->cb_g[cb] + j->cr_g[cr]) >> 16)];
                blue = range[y + j->cb_b[cb]];
            }
        } else {
            unsigned c, m, yy, k;
            if (j->conversion == CONV_NULL4) {
                c = in0[x]; m = in1[x]; yy = in2[x]; k = in3[x];
            } else {
                int y = in0[x], cb = in1[x], cr = in2[x];
                c = range[255 - (y + j->cr_r[cr])];
                m = range[255 - (y + (int)((j->cb_g[cb] + j->cr_g[cr]) >> 16))];
                yy = range[255 - (y + j->cb_b[cb])];
                k = in3[x];
            }
            if (j->saw_adobe) {
                red = c * k / 255; green = m * k / 255; blue = yy * k / 255;
            } else {
                red = (255 - c) * (255 - k) / 255;
                green = (255 - m) * (255 - k) / 255;
                blue = (255 - yy) * (255 - k) / 255;
            }
        }
        uint32_t value = UINT32_C(0xff000000) | (red << 16) | (green << 8) | blue;
        memcpy(dst + (size_t)x * 4, &value, sizeof(value));
    }
}

static void render_rows(struct jpeg *j) {
    for (uint32_t group = 0; group * (uint32_t)j->max_v < j->output_height; group++) {
        for (int ci = 0; ci < j->num_components; ci++) fill_upsample_rows(j, ci, group);
        uint32_t count = j->output_height - group * (uint32_t)j->max_v;
        if (count > (uint32_t)j->max_v) count = (uint32_t)j->max_v;
        for (uint32_t r = 0; r < count; r++)
            convert_row(j, group * (uint32_t)j->max_v + r, r);
    }
}

static void prepare_range_limit(struct jpeg *j) {
    uint8_t *base = j->range_limit;
    uint8_t *table = base + (MAXJSAMPLE + 1);
    j->range_base = table;
    memset(table - (MAXJSAMPLE + 1), 0, MAXJSAMPLE + 1);
    for (int i = 0; i <= MAXJSAMPLE; i++) table[i] = (uint8_t)i;
    table += CENTERJSAMPLE;
    for (int i = CENTERJSAMPLE; i < 2 * (MAXJSAMPLE + 1); i++) table[i] = MAXJSAMPLE;
    memset(table + 2 * (MAXJSAMPLE + 1), 0, 2 * (MAXJSAMPLE + 1) - CENTERJSAMPLE);
    memcpy(table + (4 * (MAXJSAMPLE + 1) - CENTERJSAMPLE), j->range_base, CENTERJSAMPLE);
}

static void check_precision(struct jpeg *j, bool lossless) {
    if (lossless) {
        if (j->precision > 8 || j->precision < 2) fail(j);
    } else {
        if (j->precision != 8) fail(j);
    }
}

static void init_color(struct jpeg *j) {
    int count = j->num_components;
    switch (j->jpeg_color) {
    case CS_GRAYSCALE: if (count != 1) fail(j); break;
    case CS_RGB: case CS_YCBCR: if (count != 3) fail(j); break;
    case CS_CMYK: case CS_YCCK: if (count != 4) fail(j); break;
    default: if (count < 1) fail(j); break;
    }
    if (j->out_color == CS_RGB) {
        if (j->lossless && j->jpeg_color != CS_RGB) fail(j);
        if (j->jpeg_color == CS_YCBCR) {
            j->conversion = CONV_YCC_RGB;
            build_ycc_tables(j);
        } else if (j->jpeg_color == CS_GRAYSCALE) {
            j->conversion = CONV_GRAY_RGB;
        } else if (j->jpeg_color == CS_RGB) {
            j->conversion = CONV_NULL3;
        } else {
            fail(j);
        }
        j->out_components = 3;
    } else if (j->out_color == CS_CMYK) {
        if (j->lossless && j->jpeg_color != CS_CMYK) fail(j);
        if (j->jpeg_color == CS_YCCK) {
            j->conversion = CONV_YCCK_CMYK;
            build_ycc_tables(j);
        } else if (j->jpeg_color == CS_CMYK) {
            j->conversion = CONV_NULL4;
        } else {
            fail(j);
        }
        j->out_components = 4;
    } else {
        fail(j);
    }
}

static void init_upsampler(struct jpeg *j) {
    bool fancy = !j->lossless;
    for (int ci = 0; ci < j->num_components; ci++) {
        struct component *comp = &j->comp[ci];
        int h_in = comp->h, v_in = comp->v;
        int h_out = j->max_h, v_out = j->max_v;
        if (h_in == h_out && v_in == v_out) {
            j->up_method[ci] = UP_FULL;
        } else if (h_in * 2 == h_out && v_in == v_out) {
            j->up_method[ci] = fancy && comp->downsampled_width > 2 ? UP_H2V1_FANCY : UP_H2V1_BOX;
        } else if (h_in == h_out && v_in * 2 == v_out && fancy) {
            j->up_method[ci] = UP_H1V2;
        } else if (h_in * 2 == h_out && v_in * 2 == v_out) {
            j->up_method[ci] = fancy && comp->downsampled_width > 2 ? UP_H2V2_FANCY : UP_H2V2_BOX;
        } else if (h_out % h_in == 0 && v_out % v_in == 0) {
            j->up_method[ci] = UP_INT;
            j->up_h_expand[ci] = (uint8_t)(h_out / h_in);
            j->up_v_expand[ci] = (uint8_t)(v_out / v_in);
        } else {
            fail(j);
        }
    }
}

static void start_scan(struct jpeg *j) {
    per_scan_setup(j);
    if (!j->lossless) latch_quant(j);
    entropy_start(j);
}

static void decode_all(struct jpeg *j) {
    start_scan(j);
    for (;;) {
        decode_scan(j);
        int reached = read_markers(j);
        if (!reached) break;
        if (!j->has_multiple_scans) fail(j);
        start_scan(j);
    }
}

static void allocate_planes(struct jpeg *j) {
    uint32_t widest = 0;
    for (int ci = 0; ci < j->num_components; ci++)
        if (j->comp[ci].width_in_blocks > widest) widest = j->comp[ci].width_in_blocks;
    j->undiff_row = own(j, sizeof(uint32_t) * widest);
    for (int ci = 0; ci < j->num_components; ci++) {
        struct component *comp = &j->comp[ci];
        if (j->lossless) {
            comp->sample_stride = round_up(comp->width_in_blocks, (uint32_t)comp->h);
            comp->sample_rows = round_up(comp->height_in_blocks, (uint32_t)comp->v);
            size_t size = (size_t)comp->sample_stride * comp->sample_rows;
            comp->samples = own(j, size);
            j->diff_stride[ci] = comp->sample_stride;
            j->diff_rows[ci] = own(j, sizeof(int32_t) * comp->sample_stride *
                    (size_t)comp->v);
            j->undiff_prev[ci] = own(j, sizeof(uint32_t) * comp->sample_stride);
        } else {
            comp->coeff_stride = round_up(comp->width_in_blocks, (uint32_t)comp->h);
            comp->coeff_rows = round_up(comp->height_in_blocks, (uint32_t)comp->v);
            comp->coeff = own(j, sizeof(int16_t) * DCTSIZE2 *
                    (size_t)comp->coeff_stride * comp->coeff_rows);
            comp->sample_stride = comp->coeff_stride * DCTSIZE;
            comp->sample_rows = comp->coeff_rows * DCTSIZE;
            comp->samples = own(j, (size_t)comp->sample_stride * comp->sample_rows);
        }
    }
}

static void allocate_upsample(struct jpeg *j) {
    j->up_stride = round_up(j->output_width, (uint32_t)j->max_h);
    for (int ci = 0; ci < j->num_components; ci++)
        j->up_buffer[ci] = own(j, (size_t)j->up_stride * (uint32_t)j->max_v);
}

static enum jpeg_decode_status run(struct jpeg *j, uint32_t dimension, uint64_t available,
        struct jpeg_decoded *image) {
    if (!read_markers(j)) fail(j);
    default_parms(j);
    uint64_t bytes = 0;
    uint32_t width = (uint32_t)j->width;
    uint32_t height = (uint32_t)j->height;
    if (!width || !height || width > dimension || height > dimension) fail_hard(j);
    bytes = (uint64_t)width * height * 4;
    if (bytes > available) fail_hard(j);
    j->output_width = width;
    j->output_height = height;
    prepare_range_limit(j);
    init_color(j);
    check_precision(j, j->lossless);
    init_upsampler(j);
    if (j->lossless) {
        if (j->arith) fail(j);
    } else {
        if (!j->progressive && !j->arith) install_std_tables(j);
        if (j->progressive) {
            for (int ci = 0; ci < j->num_components; ci++)
                for (int k = 0; k < DCTSIZE2; k++)
                    j->coef_bits[ci][k] = j->coef_bits[ci + j->num_components][k] = -1;
        }
    }
    allocate_planes(j);
    allocate_upsample(j);
    decode_all(j);
    if (!j->lossless) {
        j->smooth = smoothing_ok(j) ? 1 : 0;
        for (int ci = 0; ci < j->num_components; ci++) idct_component(j, ci);
    }
    j->pixels = malloc((size_t)bytes);
    if (!j->pixels) fail_hard(j);
    render_rows(j);
    image->width = width;
    image->height = height;
    image->bytes = bytes;
    image->pixels = j->pixels;
    j->pixels = NULL;
    return JPEG_DECODE_OK;
}

static enum jpeg_decode_status jpeg_decode_with(const unsigned char *data, size_t length,
        uint32_t dimension, uint64_t available, struct jpeg_decoded *image, int zune) {
    struct jpeg *j = calloc(1, sizeof(*j));
    if (!j) return JPEG_DECODE_HARD;
    j->zune = zune;
    j->data = data;
    j->length = length;
    j->fake[0] = 0xff;
    j->fake[1] = (uint8_t)M_EOI;
    j->in_headers = true;
    image->width = image->height = 0;
    image->bytes = 0;
    image->pixels = NULL;
    if (setjmp(j->jump)) {
        enum jpeg_decode_status status = j->hard ? JPEG_DECODE_HARD : JPEG_DECODE_MISSING;
        free(j->pixels);
        release(j);
        free(j);
        return status;
    }
    if (!length) fail(j);
    enum jpeg_decode_status status = run(j, dimension, available, image);
    release(j);
    free(j);
    return status;
}

enum jpeg_decode_status tomoe_jpeg_decode(const unsigned char *data, size_t length,
        uint32_t dimension, uint64_t available, struct jpeg_decoded *image) {
    return jpeg_decode_with(data, length, dimension, available, image, 0);
}

enum jpeg_decode_status tomoe_jpeg_decode_zune(const unsigned char *data, size_t length,
        uint32_t dimension, uint64_t available, struct jpeg_decoded *image) {
    return jpeg_decode_with(data, length, dimension, available, image, 1);
}
