#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include <wayland-client.h>

#include "ext-image-capture-source-v1-client-protocol.h"
#include "ext-image-copy-capture-v1-client-protocol.h"

enum { TILES_X = 16, TILES_Y = 4 };

static struct wl_display *display;
static struct wl_shm *shm;
static struct ext_output_image_capture_source_manager_v1 *sources;
static struct ext_image_copy_capture_manager_v1 *manager;
static struct ext_image_copy_capture_session_v1 *session;
static struct ext_image_copy_capture_frame_v1 *frame;
static struct wl_output *target;
static const char *target_name = "DP-4";
static uint32_t width, height, format = UINT32_MAX;
static bool constraints, stopped;
static struct wl_buffer *buffer;
static uint32_t *pixels;
static size_t bytes;
static unsigned sequence;
static FILE *out;
static const char *dump_dir;
static unsigned *dumps;
static size_t dump_count;

static void fail(const char *message) {
    fprintf(stderr, "speedlab-capture: %s\n", message);
    exit(1);
}

static uint64_t fold(uint64_t hash, uint64_t value) {
    hash ^= value;
    hash *= 0x9e3779b97f4a7c15ull;
    return hash ^ (hash >> 29);
}

static void hash_tiles(uint64_t *tiles) {
    unsigned tw = width / TILES_X, th = height / TILES_Y;
    for (unsigned i = 0; i < TILES_X * TILES_Y; i++) tiles[i] = 0xcbf29ce484222325ull;
    for (unsigned row = 0; row < height; row++) {
        unsigned ty = row / th < TILES_Y ? row / th : TILES_Y - 1;
        const uint32_t *line = pixels + (size_t)row * width;
        for (unsigned tx = 0; tx < TILES_X; tx++) {
            unsigned x0 = tx * tw, x1 = tx + 1 == TILES_X ? width : x0 + tw;
            uint64_t hash = tiles[ty * TILES_X + tx];
            unsigned x = x0;
            for (; x + 2 <= x1; x += 2) {
                uint64_t pair;
                memcpy(&pair, line + x, sizeof(pair));
                hash = fold(hash, pair & 0x00ffffff00ffffffull);
            }
            if (x < x1) hash = fold(hash, line[x] & 0x00ffffffu);
            tiles[ty * TILES_X + tx] = hash;
        }
    }
}

static void dump(unsigned index) {
    char path[4096];
    snprintf(path, sizeof(path), "%s/frame-%06u.ppm", dump_dir, index);
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "P6\n%u %u\n255\n", width / 4, height / 4);
    for (unsigned y = 0; y + 4 <= height; y += 4)
        for (unsigned x = 0; x + 4 <= width; x += 4) {
            uint32_t p = pixels[(size_t)y * width + x];
            unsigned char rgb[3] = { (unsigned char)(p >> 16), (unsigned char)(p >> 8), (unsigned char)p };
            fwrite(rgb, 1, 3, f);
        }
    fclose(f);
}

static void record(uint32_t sec_hi, uint32_t sec_lo, uint32_t nsec) {
    uint64_t tiles[TILES_X * TILES_Y], whole = 0xcbf29ce484222325ull;
    hash_tiles(tiles);
    for (unsigned i = 0; i < TILES_X * TILES_Y; i++) whole = fold(whole, tiles[i]);
    fprintf(out, "frame %u %llu.%09u %ux%u %016llx", sequence,
        (unsigned long long)(((uint64_t)sec_hi << 32) | sec_lo), nsec, width, height,
        (unsigned long long)whole);
    for (unsigned i = 0; i < TILES_X * TILES_Y; i++) fprintf(out, " %016llx", (unsigned long long)tiles[i]);
    fputc('\n', out);
    fflush(out);
    for (size_t i = 0; dump_dir && i < dump_count; i++)
        if (dumps[i] == sequence) dump(sequence);
}

static void next_frame(void);

static void frame_transform(void *d, struct ext_image_copy_capture_frame_v1 *f, uint32_t t) {
    (void)d, (void)f, (void)t;
}
static void frame_damage(void *d, struct ext_image_copy_capture_frame_v1 *f, int32_t x, int32_t y,
        int32_t w, int32_t h) {
    (void)d, (void)f, (void)x, (void)y, (void)w, (void)h;
}
static uint32_t when_hi, when_lo, when_ns;
static void frame_time(void *d, struct ext_image_copy_capture_frame_v1 *f, uint32_t hi, uint32_t lo, uint32_t ns) {
    (void)d, (void)f;
    when_hi = hi, when_lo = lo, when_ns = ns;
}
static void frame_ready(void *d, struct ext_image_copy_capture_frame_v1 *f) {
    (void)d;
    ext_image_copy_capture_frame_v1_destroy(f);
    frame = NULL;
    record(when_hi, when_lo, when_ns);
    sequence++;
    next_frame();
}
static void frame_failed(void *d, struct ext_image_copy_capture_frame_v1 *f, uint32_t reason) {
    (void)d;
    ext_image_copy_capture_frame_v1_destroy(f);
    frame = NULL;
    fprintf(out, "failed %u %u\n", sequence, reason);
    fflush(out);
    if (reason == EXT_IMAGE_COPY_CAPTURE_FRAME_V1_FAILURE_REASON_STOPPED) return;
    next_frame();
}

static const struct ext_image_copy_capture_frame_v1_listener frame_listener = {
    frame_transform, frame_damage, frame_time, frame_ready, frame_failed,
};

static void allocate(void) {
    if (buffer) {
        wl_buffer_destroy(buffer);
        munmap(pixels, bytes);
    }
    bytes = (size_t)width * height * 4;
    int fd = memfd_create("speedlab-capture", MFD_CLOEXEC);
    if (fd < 0 || ftruncate(fd, (off_t)bytes) < 0) fail("buffer");
    pixels = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (pixels == MAP_FAILED) fail("map");
    struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, (int32_t)bytes);
    buffer = wl_shm_pool_create_buffer(pool, 0, (int32_t)width, (int32_t)height, (int32_t)width * 4, format);
    wl_shm_pool_destroy(pool);
    close(fd);
}

static void next_frame(void) {
    if (stopped || !constraints || frame) return;
    frame = ext_image_copy_capture_session_v1_create_frame(session);
    ext_image_copy_capture_frame_v1_add_listener(frame, &frame_listener, NULL);
    ext_image_copy_capture_frame_v1_attach_buffer(frame, buffer);
    ext_image_copy_capture_frame_v1_damage_buffer(frame, 0, 0, (int32_t)width, (int32_t)height);
    ext_image_copy_capture_frame_v1_capture(frame);
}

static void s_size(void *d, struct ext_image_copy_capture_session_v1 *s, uint32_t w, uint32_t h) {
    (void)d, (void)s;
    width = w, height = h;
}
static void s_shm(void *d, struct ext_image_copy_capture_session_v1 *s, uint32_t f) {
    (void)d, (void)s;
    if (f == WL_SHM_FORMAT_XRGB8888 || (f == WL_SHM_FORMAT_ARGB8888 && format != WL_SHM_FORMAT_XRGB8888))
        format = f;
}
static void s_dmabuf_device(void *d, struct ext_image_copy_capture_session_v1 *s, struct wl_array *a) {
    (void)d, (void)s, (void)a;
}
static void s_dmabuf_format(void *d, struct ext_image_copy_capture_session_v1 *s, uint32_t f, struct wl_array *m) {
    (void)d, (void)s, (void)f, (void)m;
}
static void s_done(void *d, struct ext_image_copy_capture_session_v1 *s) {
    (void)d, (void)s;
    if (format == UINT32_MAX) fail("no shm format");
    if (frame) {
        ext_image_copy_capture_frame_v1_destroy(frame);
        frame = NULL;
    }
    allocate();
    constraints = true;
    fprintf(out, "constraints %ux%u\n", width, height);
    fflush(out);
    next_frame();
}
static void s_stopped(void *d, struct ext_image_copy_capture_session_v1 *s) {
    (void)d, (void)s;
    stopped = true;
    fprintf(out, "stopped %u\n", sequence);
    fflush(out);
}

static const struct ext_image_copy_capture_session_v1_listener session_listener = {
    s_size, s_shm, s_dmabuf_device, s_dmabuf_format, s_done, s_stopped,
};

static void out_geometry(void *d, struct wl_output *o, int32_t x, int32_t y, int32_t pw, int32_t ph,
        int32_t sp, const char *make, const char *model, int32_t t) {
    (void)d, (void)o, (void)x, (void)y, (void)pw, (void)ph, (void)sp, (void)make, (void)model, (void)t;
}
static void out_mode(void *d, struct wl_output *o, uint32_t f, int32_t w, int32_t h, int32_t r) {
    (void)d, (void)o, (void)f, (void)w, (void)h, (void)r;
}
static void out_done(void *d, struct wl_output *o) {
    (void)d, (void)o;
}
static void out_scale(void *d, struct wl_output *o, int32_t s) {
    (void)d, (void)o, (void)s;
}
static void out_name(void *d, struct wl_output *o, const char *name) {
    (void)d;
    if (!target && !strcmp(name, target_name)) target = o;
}
static void out_description(void *d, struct wl_output *o, const char *text) {
    (void)d, (void)o, (void)text;
}

static const struct wl_output_listener output_listener = {
    out_geometry, out_mode, out_done, out_scale, out_name, out_description,
};

static void global(void *data, struct wl_registry *registry, uint32_t name, const char *interface,
        uint32_t version) {
    (void)data;
    if (!strcmp(interface, wl_shm_interface.name)) {
        shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
    } else if (!strcmp(interface, ext_output_image_capture_source_manager_v1_interface.name)) {
        sources = wl_registry_bind(registry, name, &ext_output_image_capture_source_manager_v1_interface, 1);
    } else if (!strcmp(interface, ext_image_copy_capture_manager_v1_interface.name)) {
        manager = wl_registry_bind(registry, name, &ext_image_copy_capture_manager_v1_interface, 1);
    } else if (!strcmp(interface, wl_output_interface.name) && version >= 4) {
        struct wl_output *o = wl_registry_bind(registry, name, &wl_output_interface, 4);
        wl_output_add_listener(o, &output_listener, NULL);
    }
}

static void global_remove(void *data, struct wl_registry *registry, uint32_t name) {
    (void)data, (void)registry, (void)name;
}

static const struct wl_registry_listener registry_listener = { global, global_remove };

int main(int argc, char **argv) {
    out = stdout;
    for (int i = 1; i + 1 < argc; i += 2) {
        if (!strcmp(argv[i], "--output")) target_name = argv[i + 1];
        else if (!strcmp(argv[i], "--dump-dir")) dump_dir = argv[i + 1];
        else if (!strcmp(argv[i], "--dump")) {
            dumps = realloc(dumps, (dump_count + 1) * sizeof(*dumps));
            dumps[dump_count++] = (unsigned)atoi(argv[i + 1]);
        } else fail("usage: speedlab-capture [--output NAME] [--dump-dir DIR --dump N...]");
    }
    display = wl_display_connect(NULL);
    if (!display) fail("cannot connect");
    struct wl_registry *registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &registry_listener, NULL);
    if (wl_display_roundtrip(display) < 0 || wl_display_roundtrip(display) < 0) fail("registry");
    if (!shm || !sources || !manager) fail("the compositor lacks output capture");
    while (!target)
        if (wl_display_dispatch(display) < 0) fail("no output");
    struct ext_image_capture_source_v1 *source =
        ext_output_image_capture_source_manager_v1_create_source(sources, target);
    session = ext_image_copy_capture_manager_v1_create_session(manager, source,
        EXT_IMAGE_COPY_CAPTURE_MANAGER_V1_OPTIONS_PAINT_CURSORS);
    ext_image_copy_capture_session_v1_add_listener(session, &session_listener, NULL);
    fprintf(out, "session %s\n", target_name);
    fflush(out);
    while (wl_display_dispatch(display) >= 0) {}
    return 0;
}
