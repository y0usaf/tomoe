#include "internal.h"

#define CONTENT_GAMMA 2.2
#define ICC_FILE_LIMIT (16u * 1024u * 1024u)
#define ICC_CURVE_LIMIT 65536u
#define TAG_CURV 0x63757276u
#define TAG_PARA 0x70617261u
#define TAG_VCGT 0x76636774u
#define TAG_XYZ 0x58595a20u
#define TAG_RXYZ 0x7258595au
#define TAG_GXYZ 0x6758595au
#define TAG_BXYZ 0x6258595au
#define TAG_RTRC 0x72545243u
#define TAG_GTRC 0x67545243u
#define TAG_BTRC 0x62545243u
#define CLASS_DISPLAY 0x6d6e7472u
#define SPACE_RGB 0x52474220u

static const double srgb_colorants[9] = {
    0.43604125161605084, 0.38511291079815568, 0.14304583758579362,
    0.22248454022947742, 0.71690507860845787, 0.060610381162065304,
    0.013920187471375375, 0.097067238697124067, 0.71391257383150097,
};

enum curve_kind { CURVE_IDENTITY, CURVE_GAMMA, CURVE_PARAMETRIC, CURVE_TABLE };

struct curve {
    enum curve_kind kind;
    int function;
    double value[7];
    const unsigned char *table;
    uint32_t count;
};

struct vcgt {
    bool formula;
    const unsigned char *table;
    uint32_t count, width;
    double gamma[3], low[3], high[3];
};

struct profile *profile_ref(struct profile *profile) {
    if (profile) profile->refs++;
    return profile;
}

void profile_unref(struct profile *profile) {
    if (!profile || --profile->refs > 0) return;
    free(profile->path);
    free(profile->degamma);
    free(profile->gamma);
    free(profile);
}

static uint16_t unorm16(double value) {
    if (!(value > 0)) return 0;
    if (value >= 1) return UINT16_MAX;
    return (uint16_t)lround(value * UINT16_MAX);
}

static uint64_t fixed(double value) {
    uint64_t magnitude = (uint64_t)llround(fabs(value) * 4294967296.0);
    return value < 0 ? magnitude | (1ULL << 63) : magnitude;
}

static uint16_t icc_u16(const unsigned char *bytes) {
    return (uint16_t)(bytes[0] << 8 | bytes[1]);
}

static uint32_t icc_u32(const unsigned char *bytes) {
    return (uint32_t)bytes[0] << 24 | (uint32_t)bytes[1] << 16 | (uint32_t)bytes[2] << 8 | bytes[3];
}

static double icc_fixed(const unsigned char *bytes) {
    return (double)(int32_t)icc_u32(bytes) / 65536.0;
}

static unsigned char *icc_read(const char *path, size_t *size) {
    FILE *file = fopen(path, "rbe");
    if (!file) return NULL;
    unsigned char *bytes = malloc(ICC_FILE_LIMIT + 1);
    size_t length = bytes ? fread(bytes, 1, ICC_FILE_LIMIT + 1, file) : 0;
    bool failed = !bytes || ferror(file) || length > ICC_FILE_LIMIT;
    fclose(file);
    if (failed) {
        free(bytes);
        return NULL;
    }
    *size = length;
    return bytes;
}

static const unsigned char *icc_tag(const unsigned char *bytes, size_t size, uint32_t signature,
        uint32_t *length) {
    if (size < 132) return NULL;
    uint32_t count = icc_u32(bytes + 128);
    if (count > (size - 132) / 12) return NULL;
    for (uint32_t i = 0; i < count; i++) {
        const unsigned char *entry = bytes + 132 + (size_t)i * 12;
        if (icc_u32(entry) != signature) continue;
        uint32_t offset = icc_u32(entry + 4), length_value = icc_u32(entry + 8);
        if (offset > size || length_value > size - offset) return NULL;
        *length = length_value;
        return bytes + offset;
    }
    return NULL;
}

static bool icc_colorants(const unsigned char *bytes, size_t size,
        const uint32_t signatures[3], double colorants[9]) {
    for (int i = 0; i < 3; i++) {
        uint32_t length;
        const unsigned char *tag = icc_tag(bytes, size, signatures[i], &length);
        if (!tag || length < 20 || icc_u32(tag) != TAG_XYZ) return false;
        colorants[i] = icc_fixed(tag + 8);
        colorants[3 + i] = icc_fixed(tag + 12);
        colorants[6 + i] = icc_fixed(tag + 16);
    }
    return true;
}

static bool curve_read(const unsigned char *bytes, size_t size, uint32_t signature,
        struct curve *curve) {
    uint32_t length;
    const unsigned char *tag = icc_tag(bytes, size, signature, &length);
    if (!tag || length < 12) return false;
    uint32_t type = icc_u32(tag);
    if (type == TAG_CURV) {
        uint32_t count = icc_u32(tag + 8);
        if (!count) {
            curve->kind = CURVE_IDENTITY;
            return true;
        }
        if (count == 1) {
            if (length < 14) return false;
            curve->kind = CURVE_GAMMA;
            curve->value[0] = icc_u16(tag + 12) / 256.0;
            return true;
        }
        if (count > ICC_CURVE_LIMIT || length < 12 + 2 * (size_t)count) return false;
        curve->kind = CURVE_TABLE;
        curve->table = tag + 12;
        curve->count = count;
        return true;
    }
    if (type != TAG_PARA || length < 12) return false;
    static const int arguments[5] = { 1, 3, 4, 5, 7 };
    int function = icc_u16(tag + 8);
    if (function < 0 || function > 4) return false;
    int count = arguments[function];
    if (length < 12 + 4 * (size_t)count) return false;
    curve->kind = CURVE_PARAMETRIC;
    curve->function = function;
    for (int i = 0; i < count; i++) curve->value[i] = icc_fixed(tag + 12 + 4 * i);
    return true;
}

static double curve_eval(const struct curve *curve, double x) {
    if (!(x > 0)) x = 0;
    if (x > 1) x = 1;
    switch (curve->kind) {
    case CURVE_IDENTITY: return x;
    case CURVE_GAMMA: return pow(x, curve->value[0]);
    case CURVE_TABLE: {
        double position = x * (double)(curve->count - 1);
        size_t index = (size_t)position;
        if (index + 1 >= curve->count) return icc_u16(curve->table + 2 * index) / 65535.0;
        double low = icc_u16(curve->table + 2 * index), high = icc_u16(curve->table + 2 * index + 2);
        return (low + (high - low) * (position - (double)index)) / 65535.0;
    }
    default: break;
    }
    double g = curve->value[0], a = curve->value[1], b = curve->value[2];
    switch (curve->function) {
    case 0: return pow(x, g);
    case 1: {
        double base = a * x + b;
        return base > 0 ? pow(base, g) : 0;
    }
    case 2: {
        double base = a * x + b;
        return (base > 0 ? pow(base, g) : 0) + curve->value[3];
    }
    case 3:
        if (x >= curve->value[3]) {
            double base = a * x + b;
            return base > 0 ? pow(base, g) : 0;
        }
        return curve->value[2] * x;
    default:
        if (x >= curve->value[3]) {
            double base = a * x + b;
            return (base > 0 ? pow(base, g) : 0) + curve->value[4];
        }
        return curve->value[2] * x + curve->value[5];
    }
}

static double curve_invert(const struct curve *curve, double y) {
    if (!(y > 0)) return 0;
    double g = curve->value[0], a = curve->value[1], b = curve->value[2];
    if (curve->kind == CURVE_IDENTITY) return y > 1 ? 1 : y;
    if (curve->kind == CURVE_GAMMA) return y >= 1 ? 1 : g > 0 ? pow(y, 1 / g) : 0;
    if (curve->kind == CURVE_PARAMETRIC) {
        if (fabs(g) < 1e-9) return 0;
        if (curve->function == 0 && y >= 1) return 1;
        switch (curve->function) {
        case 0: return pow(y, 1 / g);
        case 1:
            if (fabs(a) < 1e-9) return 0;
            return (pow(y, 1 / g) - b) / a;
        case 2:
            if (fabs(a) < 1e-9) return 0;
            if (y < curve->value[3]) return -b / a;
            return (pow(y - curve->value[3], 1 / g) - b) / a;
        case 3: {
            if (fabs(a) < 1e-9) return 0;
            double edge = a * curve->value[4] + b;
            double split = edge < 0 ? 0 : pow(edge, g);
            if (y >= split) return (pow(y, 1 / g) - b) / a;
            return fabs(curve->value[3]) < 1e-9 ? 0 : y / curve->value[3];
        }
        default: {
            if (fabs(curve->value[3]) < 1e-9) return 0;
            double split = curve->value[3] * curve->value[4] + curve->value[6];
            if (y < split) return (y - curve->value[6]) / curve->value[3];
            double shifted = y - curve->value[5];
            if (shifted < 0 || fabs(a) < 1e-9) return 0;
            return (pow(shifted, 1 / g) - b) / a;
        }
        }
    }
    bool falling = curve_eval(curve, 0) > curve_eval(curve, 1);
    double low = 0, high = 1;
    for (int step = 0; step < 52; step++) {
        double middle = (low + high) / 2;
        if ((curve_eval(curve, middle) < y) != falling) low = middle;
        else high = middle;
    }
    return (low + high) / 2;
}

static uint16_t saturate(double value) {
    value += 0.5;
    if (!(value > 0)) return 0;
    if (value >= 65535) return UINT16_MAX;
    return (uint16_t)value;
}

static int curve_interval(const struct curve *curve, double y) {
    int count = (int)curve->count;
    int low = (int)icc_u16(curve->table + 2 * (count - 1));
    if ((int)icc_u16(curve->table) < low) {
        for (int i = count - 2; i >= 0; i--) {
            int first = icc_u16(curve->table + 2 * i), second = icc_u16(curve->table + 2 * i + 2);
            if (first <= second ? y >= first && y <= second : y >= second && y <= first) return i;
        }
        return -1;
    }
    for (int i = 0; i < count - 1; i++) {
        int first = icc_u16(curve->table + 2 * i), second = icc_u16(curve->table + 2 * i + 2);
        if (first <= second ? y >= first && y <= second : y >= second && y <= first) return i;
    }
    return -1;
}

static void curve_reverse(const struct curve *curve, uint16_t *reversed) {
    enum { SAMPLES = 4096 };
    bool falling = icc_u16(curve->table + 2 * (curve->count - 1)) < icc_u16(curve->table);
    double slope = 0, offset = 0;
    for (int i = 0; i < SAMPLES; i++) {
        double y = (double)i * 65535.0 / (SAMPLES - 1);
        int j = curve_interval(curve, y);
        if (j >= 0) {
            double first = icc_u16(curve->table + 2 * j), second = icc_u16(curve->table + 2 * j + 2);
            double low = (double)j * 65535.0 / (double)(curve->count - 1);
            double high = (double)(j + 1) * 65535.0 / (double)(curve->count - 1);
            if (first == second) {
                reversed[i] = saturate(falling ? low : high);
                continue;
            }
            slope = (high - low) / (second - first);
            offset = high - slope * second;
        }
        reversed[i] = saturate(slope * y + offset);
    }
}

static uint32_t table_position(int domain, uint16_t in, uint32_t *cell) {
    uint32_t fixed = (uint32_t)domain * in;
    fixed += (fixed + 0x7fff) / 0xffff;
    *cell = fixed >> 16;
    return fixed & 0xffff;
}

static double lerp16(uint16_t low, uint16_t high, uint32_t rest) {
    uint32_t difference = (uint32_t)(int32_t)(high - low) * rest;
    difference = (difference + 0x8000) >> 16;
    return (uint16_t)(low + difference) / 65535.0;
}

static double curve_reversed_eval(const uint16_t *reversed, uint16_t in) {
    enum { SAMPLES = 4096 };
    if (in == UINT16_MAX) return reversed[SAMPLES - 1] / 65535.0;
    uint32_t cell;
    uint32_t rest = table_position(SAMPLES - 1, in, &cell);
    return lerp16(reversed[cell], reversed[cell + 1], rest);
}

static bool vcgt_read(const unsigned char *bytes, size_t size, struct vcgt *vcgt) {
    uint32_t length;
    const unsigned char *tag = icc_tag(bytes, size, TAG_VCGT, &length);
    if (!tag || length < 16 || icc_u32(tag) != TAG_VCGT) return false;
    uint32_t kind = icc_u32(tag + 8);
    if (kind == 0 && length >= 18) {
        uint16_t channels = icc_u16(tag + 12), count = icc_u16(tag + 14), width = icc_u16(tag + 16);
        if (width == 1 && count == 256 && length == 1576) width = 2;
        if (channels != 3 || !count || (width != 1 && width != 2)) return false;
        if (length < 18 + (size_t)3 * count * width) return false;
        vcgt->table = tag + 18;
        vcgt->count = count;
        vcgt->width = width;
        return true;
    }
    if (kind != 1 || length < 12 + 36) return false;
    for (int c = 0; c < 3; c++) {
        vcgt->gamma[c] = icc_fixed(tag + 12 + 12 * c);
        vcgt->low[c] = icc_fixed(tag + 16 + 12 * c);
        vcgt->high[c] = icc_fixed(tag + 20 + 12 * c);
    }
    vcgt->formula = true;
    return true;
}

static uint16_t vcgt_entry(const struct vcgt *vcgt, int channel, int index) {
    const unsigned char *entries = vcgt->table + (size_t)channel * vcgt->count * vcgt->width;
    return vcgt->width == 1 ? (uint16_t)(entries[index] * 257) : icc_u16(entries + 2 * index);
}

static double vcgt_eval(const struct vcgt *vcgt, int channel, double x) {
    if (!(x > 0)) x = 0;
    if (x > 1) x = 1;
    if (vcgt->formula) {
        if (!(vcgt->gamma[channel] > 0)) return x;
        return (vcgt->high[channel] - vcgt->low[channel]) * pow(x, vcgt->gamma[channel]) +
            vcgt->low[channel];
    }
    if (vcgt->count < 2) return vcgt_entry(vcgt, channel, 0) / 65535.0;
    uint16_t in = saturate(x * 65535.0);
    if (in == UINT16_MAX) return vcgt_entry(vcgt, channel, (int)vcgt->count - 1) / 65535.0;
    uint32_t cell;
    uint32_t rest = table_position((int)vcgt->count - 1, in, &cell);
    return lerp16(vcgt_entry(vcgt, channel, (int)cell), vcgt_entry(vcgt, channel, (int)cell + 1),
        rest);
}

static bool invert(const double m[9], double out[9]) {
    double det = m[0] * (m[4] * m[8] - m[5] * m[7]) -
        m[1] * (m[3] * m[8] - m[5] * m[6]) + m[2] * (m[3] * m[7] - m[4] * m[6]);
    if (!isnormal(det)) return false;
    out[0] = (m[4] * m[8] - m[5] * m[7]) / det;
    out[1] = (m[2] * m[7] - m[1] * m[8]) / det;
    out[2] = (m[1] * m[5] - m[2] * m[4]) / det;
    out[3] = (m[5] * m[6] - m[3] * m[8]) / det;
    out[4] = (m[0] * m[8] - m[2] * m[6]) / det;
    out[5] = (m[2] * m[3] - m[0] * m[5]) / det;
    out[6] = (m[3] * m[7] - m[4] * m[6]) / det;
    out[7] = (m[1] * m[6] - m[0] * m[7]) / det;
    out[8] = (m[0] * m[4] - m[1] * m[3]) / det;
    return true;
}

static void fill(struct profile *profile, const double to_display[9],
        const struct curve inverse[3], const struct vcgt *vcgt) {
    double knot = 1.0 / (double)(profile->gamma_size - 1);
    double toe = pow(knot, 1 / CONTENT_GAMMA);
    for (size_t i = 0; i < profile->degamma_size; i++) {
        double v = (double)i / (double)(profile->degamma_size - 1);
        profile->degamma[i] = unorm16(v < toe ? v * knot / toe : pow(v, CONTENT_GAMMA));
    }
    uint16_t *reversed = malloc(4096 * sizeof(*reversed));
    for (int c = 0; c < 3; c++) {
        bool table = inverse[c].kind == CURVE_TABLE;
        if (table && reversed) curve_reverse(&inverse[c], reversed);
        for (size_t i = 0; !table || reversed ? i < profile->gamma_size : false; i++) {
            double at = (double)((float)i * (float)knot);
            double v = table && reversed ? curve_reversed_eval(reversed, saturate(at * 65535.0)) :
                curve_invert(&inverse[c], at);
            if (vcgt) v = vcgt_eval(vcgt, c, (float)v);
            profile->gamma[c * profile->gamma_size + i] = unorm16((float)v);
        }
    }
    free(reversed);
    for (int row = 0; row < 3; row++)
        for (int column = 0; column < 3; column++)
            profile->ctm[row * 3 + column] = fixed(to_display[row * 3] *
                srgb_colorants[column] + to_display[row * 3 + 1] * srgb_colorants[3 + column] +
                to_display[row * 3 + 2] * srgb_colorants[6 + column]);
}

const char *profile_load(const char *path, size_t degamma_size, size_t gamma_size,
        struct profile **out) {
    static const uint32_t colorant_tags[3] = { TAG_RXYZ, TAG_GXYZ, TAG_BXYZ };
    static const uint32_t curve_tags[3] = { TAG_RTRC, TAG_GTRC, TAG_BTRC };
    *out = NULL;
    if (degamma_size < 2 || gamma_size < 2) return "the output has no degamma, CTM and gamma LUTs";
    size_t size = 0;
    unsigned char *bytes = icc_read(path, &size);
    if (!bytes || size < 132 || icc_u32(bytes + 36) != 0x61637370u ||
            icc_u32(bytes + 128) > (size - 132) / 12) {
        free(bytes);
        return "cannot read an ICC profile there";
    }
    struct curve inverse[3];
    struct vcgt vcgt;
    memset(inverse, 0, sizeof(inverse));
    memset(&vcgt, 0, sizeof(vcgt));
    double display[9], to_display[9];
    const char *error = NULL;
    if (icc_u32(bytes + 12) != CLASS_DISPLAY || icc_u32(bytes + 16) != SPACE_RGB)
        error = "not an RGB display profile";
    else if (!icc_colorants(bytes, size, colorant_tags, display) ||
            !invert(display, to_display))
        error = "no invertible RGB colorants";
    for (int c = 0; !error && c < 3; c++)
        if (!curve_read(bytes, size, curve_tags[c], &inverse[c])) error = "no RGB tone curves";
    bool has_vcgt = !error && vcgt_read(bytes, size, &vcgt);
    struct profile *profile = error ? NULL : calloc(1, sizeof(*profile));
    if (profile) {
        *profile = (struct profile){ .refs = 1, .path = strdup(path),
            .degamma = calloc(degamma_size, sizeof(uint16_t)),
            .gamma = calloc(gamma_size * 3, sizeof(uint16_t)),
            .degamma_size = degamma_size, .gamma_size = gamma_size };
        if (!profile->path || !profile->degamma || !profile->gamma) error = "out of memory";
    } else if (!error) {
        error = "out of memory";
    }
    if (!error) fill(profile, to_display, inverse, has_vcgt ? &vcgt : NULL);
    free(bytes);
    if (error) {
        profile_unref(profile);
        return error;
    }
    *out = profile;
    return NULL;
}

static uint16_t lookup(const uint16_t *ramp, size_t size, uint16_t value) {
    double x = (double)value / UINT16_MAX * (double)(size - 1);
    size_t i = (size_t)x;
    if (i + 1 >= size) return ramp[size - 1];
    return (uint16_t)lround(ramp[i] + (ramp[i + 1] - ramp[i]) * (x - (double)i));
}

uint16_t *profile_ramps(const struct profile *profile, const uint16_t *ramps, size_t size) {
    size_t n = profile->gamma_size;
    uint16_t *out = malloc(n * 3 * sizeof(*out));
    if (!out) return NULL;
    for (int c = 0; c < 3; c++)
        for (size_t i = 0; i < n; i++) {
            uint16_t value = profile->gamma[c * n + i];
            out[c * n + i] = ramps && size > 1 ? lookup(ramps + c * size, size, value) : value;
        }
    return out;
}
