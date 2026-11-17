#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "spa.h"

static uint8_t buffer[1024];
static size_t used;

static void put_u32(uint32_t value) {
    memcpy(buffer + used, &value, 4);
    used += 4;
}

static void put_u64(uint64_t value) {
    memcpy(buffer + used, &value, 8);
    used += 8;
}

static void pad(void) {
    while (used & 7u) buffer[used++] = 0;
}

static size_t pod_begin(void) {
    size_t at = used;
    put_u32(0);
    put_u32(0);
    return at;
}

static void pod_end(size_t at, uint32_t type) {
    uint32_t size = (uint32_t)(used - at - 8);
    memcpy(buffer + at, &size, 4);
    memcpy(buffer + at + 4, &type, 4);
    pad();
}

static void pod_id(uint32_t value) {
    size_t at = pod_begin();
    put_u32(value);
    put_u32(0);
    pod_end(at, SPA_TYPE_ID);
}

static void pod_long(int64_t value) {
    size_t at = pod_begin();
    put_u64((uint64_t)value);
    pod_end(at, SPA_TYPE_LONG);
}

static void pod_rectangle(uint32_t width, uint32_t height) {
    size_t at = pod_begin();
    put_u32(width);
    put_u32(height);
    pod_end(at, SPA_TYPE_RECTANGLE);
}

static void pod_fraction(uint32_t num, uint32_t denom) {
    size_t at = pod_begin();
    put_u32(num);
    put_u32(denom);
    pod_end(at, SPA_TYPE_FRACTION);
}

static void pod_choice(uint32_t child, uint32_t child_size, const uint32_t *values, size_t count) {
    size_t at = pod_begin();
    put_u32(SPA_CHOICE_RANGE);
    put_u32(0);
    put_u32(child_size);
    put_u32(child);
    for (size_t i = 0; i < count; i++) put_u32(values[i]);
    pod_end(at, SPA_TYPE_CHOICE);
}

static void prop(uint32_t key) {
    put_u32(key);
    put_u32(0);
}

static size_t format_begin(void) {
    used = 0;
    size_t object = pod_begin();
    put_u32(SPA_TYPE_OBJECT_FORMAT);
    put_u32(SPA_PARAM_ENUMFORMAT);
    prop(SPA_FORMAT_MEDIATYPE);
    pod_id(SPA_MEDIA_TYPE_VIDEO);
    prop(SPA_FORMAT_MEDIASUBTYPE);
    pod_id(SPA_MEDIA_SUBTYPE_RAW);
    prop(SPA_FORMAT_VIDEO_FORMAT);
    pod_id(SPA_VIDEO_FORMAT_BGRX);
    return object;
}

static void fail(const char *what) {
    fprintf(stderr, "portal-format: %s\n", what);
    exit(1);
}

int main(void) {
    static const uint64_t modifier = 0x1122334455667788ull;
    static const uint32_t rates[] = { 60, 1, 0, 1, 60, 1 };
    static const uint32_t sizes[] = { 960, 540, 1, 1, 8192, 8192 };
    struct spa_video_info_raw info;

    size_t object = format_begin();
    prop(SPA_FORMAT_VIDEO_MODIFIER);
    pod_long((int64_t)modifier);
    prop(SPA_FORMAT_VIDEO_SIZE);
    pod_rectangle(960, 540);
    prop(SPA_FORMAT_VIDEO_FRAMERATE);
    pod_fraction(30, 1);
    prop(SPA_FORMAT_VIDEO_MAXFRAMERATE);
    pod_choice(SPA_TYPE_FRACTION, 8, rates, 3);
    pod_end(object, SPA_TYPE_OBJECT);
    if (spa_parse_video_format(buffer, used, &info) != 0) fail("the format pod was refused");
    if (info.format != SPA_VIDEO_FORMAT_BGRX) fail("format");
    if (!(info.flags & SPA_VIDEO_FLAG_MODIFIER)) fail("modifier flag");
    if (info.modifier != modifier) fail("modifier");
    if (info.size.width != 960 || info.size.height != 540) fail("size");
    if (info.framerate.num != 30 || info.framerate.denom != 1) fail("framerate");
    if (info.max_framerate.num != 60 || info.max_framerate.denom != 1) fail("max framerate");

    object = format_begin();
    prop(SPA_FORMAT_VIDEO_SIZE);
    pod_rectangle(640, 480);
    prop(SPA_FORMAT_VIDEO_FRAMERATE);
    pod_fraction(0, 1);
    pod_end(object, SPA_TYPE_OBJECT);
    if (spa_parse_video_format(buffer, used, &info) != 0) fail("the plain format pod was refused");
    if (info.format != SPA_VIDEO_FORMAT_BGRX) fail("format without a modifier");
    if (info.flags & SPA_VIDEO_FLAG_MODIFIER) fail("a modifier that was not offered");
    if (info.modifier != 0) fail("modifier value without a modifier");
    if (info.size.width != 640 || info.size.height != 480) fail("size without a modifier");

    object = format_begin();
    prop(SPA_FORMAT_VIDEO_MODIFIER);
    pod_long(0);
    prop(SPA_FORMAT_VIDEO_SIZE);
    pod_choice(SPA_TYPE_RECTANGLE, 8, sizes, 3);
    prop(SPA_FORMAT_VIDEO_FRAMERATE);
    pod_fraction(240, 1);
    pod_end(object, SPA_TYPE_OBJECT);
    if (spa_parse_video_format(buffer, used, &info) != 0) fail("the range format pod was refused");
    if (!(info.flags & SPA_VIDEO_FLAG_MODIFIER) || info.modifier != 0) fail("a linear modifier");
    if (info.size.width != 960 || info.size.height != 540) fail("a ranged size");
    if (info.framerate.num != 240 || info.framerate.denom != 1) fail("a fixed framerate");

    object = format_begin();
    prop(SPA_FORMAT_VIDEO_MODIFIER);
    pod_long((int64_t)modifier);
    pod_end(object, SPA_TYPE_OBJECT);
    if (spa_parse_video_format(buffer, used - 16, &info) == 0) fail("a truncated pod was accepted");
    if (info.modifier != 0) fail("a modifier read past the end");
    if (spa_parse_video_format(buffer, 4, &info) == 0) fail("a header was accepted as a format");

    printf("portal-format: ok\n");
    return 0;
}
