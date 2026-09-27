#include "internal.h"
#include <lcms2.h>

#define CONTENT_GAMMA 2.2

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

static bool colorants(cmsHPROFILE icc, double m[9]) {
    static const cmsTagSignature tags[3] = {
        cmsSigRedColorantTag, cmsSigGreenColorantTag, cmsSigBlueColorantTag };
    for (int i = 0; i < 3; i++) {
        const cmsCIEXYZ *xyz = icc ? cmsReadTag(icc, tags[i]) : NULL;
        if (!xyz) return false;
        m[i] = xyz->X;
        m[3 + i] = xyz->Y;
        m[6 + i] = xyz->Z;
    }
    return true;
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

static void fill(struct profile *profile, const double to_display[9], const double source[9],
        cmsToneCurve *const inverse[3], cmsToneCurve *const *vcgt) {
    double knot = 1.0 / (double)(profile->gamma_size - 1);
    double toe = pow(knot, 1 / CONTENT_GAMMA);
    for (size_t i = 0; i < profile->degamma_size; i++) {
        double v = (double)i / (double)(profile->degamma_size - 1);
        profile->degamma[i] = unorm16(v < toe ? v * knot / toe : pow(v, CONTENT_GAMMA));
    }
    for (int c = 0; c < 3; c++)
        for (size_t i = 0; i < profile->gamma_size; i++) {
            float v = cmsEvalToneCurveFloat(inverse[c], (float)i * (float)knot);
            if (vcgt) v = cmsEvalToneCurveFloat(vcgt[c], v);
            profile->gamma[c * profile->gamma_size + i] = unorm16(v);
        }
    for (int row = 0; row < 3; row++)
        for (int column = 0; column < 3; column++)
            profile->ctm[row * 3 + column] = fixed(to_display[row * 3] * source[column] +
                to_display[row * 3 + 1] * source[3 + column] +
                to_display[row * 3 + 2] * source[6 + column]);
}

const char *profile_load(const char *path, size_t degamma_size, size_t gamma_size,
        struct profile **out) {
    static const cmsTagSignature curves[3] = {
        cmsSigRedTRCTag, cmsSigGreenTRCTag, cmsSigBlueTRCTag };
    *out = NULL;
    if (degamma_size < 2 || gamma_size < 2) return "the output has no degamma, CTM and gamma LUTs";
    cmsHPROFILE icc = cmsOpenProfileFromFile(path, "r");
    if (!icc) return "cannot read an ICC profile there";
    cmsHPROFILE srgb = cmsCreate_sRGBProfile();
    cmsToneCurve *inverse[3] = { NULL, NULL, NULL };
    double display[9], source[9], to_display[9];
    const char *error = NULL;
    if (cmsGetDeviceClass(icc) != cmsSigDisplayClass ||
            cmsGetColorSpace(icc) != cmsSigRgbData)
        error = "not an RGB display profile";
    else if (!colorants(icc, display) || !invert(display, to_display))
        error = "no invertible RGB colorants";
    else if (!colorants(srgb, source))
        error = "cannot build the sRGB source";
    for (int c = 0; !error && c < 3; c++) {
        const cmsToneCurve *curve = cmsReadTag(icc, curves[c]);
        inverse[c] = curve ? cmsReverseToneCurve(curve) : NULL;
        if (!inverse[c]) error = "no RGB tone curves";
    }
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
    if (!error) fill(profile, to_display, source, inverse, cmsReadTag(icc, cmsSigVcgtTag));
    for (int c = 0; c < 3; c++) if (inverse[c]) cmsFreeToneCurve(inverse[c]);
    if (srgb) cmsCloseProfile(srgb);
    cmsCloseProfile(icc);
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
