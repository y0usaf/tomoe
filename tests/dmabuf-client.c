#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <unistd.h>

#include <drm_fourcc.h>
#include <gbm.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <wayland-client.h>

#include "linux-dmabuf-v1-client-protocol.h"
#include "viewporter-client-protocol.h"
#include "xdg-shell-client-protocol.h"

struct udmabuf_create {
    uint32_t memfd;
    uint32_t flags;
    uint64_t offset;
    uint64_t size;
};
#define UDMABUF_CREATE _IOW('u', 0x42, struct udmabuf_create)

static const char *program = "tomoe-dmabuf-client";
static const char *app_id = "dmabuf-check";
static struct wl_display *display;
static struct wl_compositor *compositor;
static struct xdg_wm_base *wm_base;
static struct zwp_linux_dmabuf_v1 *dmabuf;
static struct wp_viewporter *viewporter;
static struct wp_viewport *viewport;
static struct zwp_linux_dmabuf_feedback_v1 *feedback;
static struct wl_output *output;
static struct wl_seat *seat;
static struct wl_surface *surface;
static struct wl_buffer *buffers[2];
static struct gbm_device *device;
static uint32_t dmabuf_version;
static uint32_t colour = 0xff336699;
static uint32_t drm_format = DRM_FORMAT_ARGB8888;
static int output_width = 1280, output_height = 720;
static int pending_width, pending_height;
static int fixed_buf_w, fixed_buf_h;
static bool has_vp_src, has_vp_dst;
static double vp_src_x, vp_src_y, vp_src_w, vp_src_h;
static int vp_dst_w, vp_dst_h;
static int target_frames, sent_frames, active_slot;
static bool want_feedback;
static int committed;

static int fb_rounds, fb_tranches, fb_scanout, fb_formats, fb_reallocs;
static unsigned fb_signature;
static dev_t fb_main_device;
static int allocated_w, allocated_h;
static uint32_t cur_tranche_flags;
static int cur_tranche_formats;
struct feedback_format {
    uint32_t format, pad;
    uint64_t modifier;
};
enum { FEEDBACK_TRANCHE = 1024 };
static const struct feedback_format *fb_table;
static size_t fb_table_len;
static void *fb_table_map;
static size_t fb_table_bytes;
static uint16_t fb_indices[FEEDBACK_TRANCHE];
static size_t fb_index_count;
static uint64_t chosen_modifier = DRM_FORMAT_MOD_LINEAR;
static bool chosen_known, fb_done_seen, waiting_to_map, warned_no_format;
static int pending_cfg_w, pending_cfg_h;

static void fail(const char *format, ...) __attribute__((format(printf, 1, 2), noreturn));
static void commit_first_frame(void);

static void fail(const char *format, ...) {
    va_list arguments;
    va_start(arguments, format);
    fprintf(stderr, "%s: ", program);
    vfprintf(stderr, format, arguments);
    fputc('\n', stderr);
    va_end(arguments);
    exit(1);
}

static void step(const char *format, int width, int height) {
    printf(format, width, height);
    fputc('\n', stdout);
    fflush(stdout);
}

static uint8_t clamp_u8(int v) {
    return v < 0 ? 0 : v > 255 ? 255 : (uint8_t)v;
}

static void rgb_to_yuv(uint32_t argb, uint8_t *y, uint8_t *u, uint8_t *v) {
    int r = (int)((argb >> 16) & 0xff);
    int g = (int)((argb >> 8) & 0xff);
    int b = (int)(argb & 0xff);
    *y = clamp_u8(((66 * r + 129 * g + 25 * b + 128) >> 8) + 16);
    *u = clamp_u8(((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128);
    *v = clamp_u8(((112 * r - 94 * g - 18 * b + 128) >> 8) + 128);
}

static void fill_nv12(uint8_t *base, size_t stride, int width, int height) {
    uint8_t y, u, v;
    rgb_to_yuv(colour, &y, &u, &v);
    for (int row = 0; row < height; row++)
        memset(base + (size_t)row * stride, y, (size_t)width);
    uint8_t *uv = base + stride * (size_t)height;
    for (int row = 0; row < height / 2; row++) {
        uint8_t *line = uv + (size_t)row * stride;
        for (int col = 0; col < width; col += 2) {
            line[col] = u;
            if (col + 1 < width) line[col + 1] = v;
        }
    }
}

static void fill_p010(uint8_t *base, size_t stride, int width, int height) {
    uint8_t y8, u8, v8;
    rgb_to_yuv(colour, &y8, &u8, &v8);
    uint16_t y16 = (uint16_t)y8 << 8;
    uint16_t u16 = (uint16_t)u8 << 8;
    uint16_t v16 = (uint16_t)v8 << 8;
    for (int row = 0; row < height; row++) {
        uint16_t *line = (uint16_t *)(base + (size_t)row * stride);
        for (int col = 0; col < width; col++) line[col] = y16;
    }
    uint8_t *uv = base + stride * (size_t)height;
    for (int row = 0; row < height / 2; row++) {
        uint16_t *line = (uint16_t *)(uv + (size_t)row * stride);
        for (int col = 0; col < width; col += 2) {
            line[col] = u16;
            if (col + 1 < width) line[col + 1] = v16;
        }
    }
}

static struct wl_buffer *make_udmabuf(int width, int height) {
    int udmabuf = open("/dev/udmabuf", O_RDWR | O_CLOEXEC);
    if (udmabuf < 0) return NULL;
    bool nv12 = drm_format == DRM_FORMAT_NV12;
    bool p010 = drm_format == DRM_FORMAT_P010;
    if ((nv12 || p010) && ((width & 1) || (height & 1))) {
        close(udmabuf);
        fail("YUV format requires even dimensions (%dx%d)", width, height);
    }
    size_t stride = nv12 ? (size_t)((width + 63) & ~63) :
        p010 ? (size_t)(((width * 2) + 63) & ~63) : (size_t)width * 4;
    size_t raw = (nv12 || p010) ? stride * (size_t)(height + height / 2) : stride * (size_t)height;
    size_t page = 4096;
    size_t bytes = (raw + page - 1) / page * page;
    int memfd = memfd_create("tomoe-dmabuf-client", MFD_CLOEXEC | MFD_ALLOW_SEALING);
    if (memfd < 0 || ftruncate(memfd, (off_t)bytes) != 0) {
        if (memfd >= 0) close(memfd);
        close(udmabuf);
        return NULL;
    }
    uint8_t *map = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, memfd, 0);
    if (map == MAP_FAILED) {
        close(memfd);
        close(udmabuf);
        return NULL;
    }
    if (nv12) fill_nv12(map, stride, width, height);
    else if (p010) fill_p010(map, stride, width, height);
    else {
        uint32_t *words = (uint32_t *)map;
        for (size_t i = 0; i < stride * (size_t)height / 4; i++) words[i] = colour;
    }
    munmap(map, bytes);
    if (fcntl(memfd, F_ADD_SEALS, F_SEAL_SHRINK) != 0) {
        close(memfd);
        close(udmabuf);
        return NULL;
    }
    struct udmabuf_create create = { .memfd = (uint32_t)memfd, .size = bytes };
    int fd = ioctl(udmabuf, UDMABUF_CREATE, &create);
    close(udmabuf);
    close(memfd);
    if (fd < 0) return NULL;
    struct zwp_linux_buffer_params_v1 *params = zwp_linux_dmabuf_v1_create_params(dmabuf);
    zwp_linux_buffer_params_v1_add(params, fd, 0, 0, (uint32_t)stride, 0, 0);
    if (nv12 || p010) {
        int fd1 = dup(fd);
        zwp_linux_buffer_params_v1_add(params, fd1, 1, (uint32_t)(stride * (size_t)height),
            (uint32_t)stride, 0, 0);
        close(fd1);
    }
    struct wl_buffer *made = zwp_linux_buffer_params_v1_create_immed(params, width, height,
        drm_format, 0);
    zwp_linux_buffer_params_v1_destroy(params);
    close(fd);
    return made;
}

static struct wl_buffer *make_gbm_buffer(int width, int height, uint64_t want) {
    char path[64];
    const char *override = getenv("TOMOE_CLIENT_RENDER_NODE");
    if (override && *override && !device) {
        int fd = open(override, O_RDWR | O_CLOEXEC);
        if (fd >= 0) {
            device = gbm_create_device(fd);
            if (!device) close(fd);
        }
    }
    for (int node = 128; node < 136 && !device; node++) {
        snprintf(path, sizeof(path), "/dev/dri/renderD%d", node);
        int fd = open(path, O_RDWR | O_CLOEXEC);
        if (fd < 0) continue;
        device = gbm_create_device(fd);
        if (!device) close(fd);
    }
    if (!device) return NULL;
    uint64_t linear = DRM_FORMAT_MOD_LINEAR;
    struct gbm_bo *object = NULL;
    if (want != DRM_FORMAT_MOD_LINEAR && want != DRM_FORMAT_MOD_INVALID)
        object = gbm_bo_create_with_modifiers(device, (uint32_t)width, (uint32_t)height,
            drm_format, &want, 1);
    if (!object)
        object = gbm_bo_create_with_modifiers(device, (uint32_t)width, (uint32_t)height,
            drm_format, &linear, 1);
    if (!object) object = gbm_bo_create(device, (uint32_t)width, (uint32_t)height, drm_format,
        GBM_BO_USE_LINEAR);
    if (!object) object = gbm_bo_create(device, (uint32_t)width, (uint32_t)height, drm_format,
        GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING);
    if (!object) return NULL;
    if (drm_format == DRM_FORMAT_ARGB8888 || drm_format == DRM_FORMAT_XRGB8888) {
        uint32_t stride = 0;
        void *map = NULL;
        void *pixels = gbm_bo_map(object, 0, 0, (uint32_t)width, (uint32_t)height,
            GBM_BO_TRANSFER_WRITE, &stride, &map);
        if (pixels) {
            for (size_t i = 0; i < (size_t)stride * (size_t)height / 4; i++)
                ((uint32_t *)pixels)[i] = colour;
            gbm_bo_unmap(object, map);
        }
    } else {
        int fd = gbm_bo_get_fd(object);
        if (fd >= 0) {
            off_t end = lseek(fd, 0, SEEK_END);
            size_t stride0 = gbm_bo_get_stride_for_plane(object, 0);
            size_t stride1 = gbm_bo_get_stride_for_plane(object, 1);
            size_t off0 = gbm_bo_get_offset(object, 0);
            size_t off1 = gbm_bo_get_offset(object, 1);
            size_t need = off1 + stride1 * (size_t)(height / 2);
            size_t len = end > 0 && (size_t)end >= need ? (size_t)end : need;
            uint8_t *map = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
            if (map != MAP_FAILED) {
                uint8_t y8, u8, v8;
                rgb_to_yuv(colour, &y8, &u8, &v8);
                if (drm_format == DRM_FORMAT_NV12) {
                    for (int r = 0; r < height; r++)
                        memset(map + off0 + (size_t)r * stride0, y8, (size_t)width);
                    for (int r = 0; r < height / 2; r++) {
                        uint8_t *ln = map + off1 + (size_t)r * stride1;
                        for (int c = 0; c < width; c += 2) { ln[c] = u8; ln[c + 1] = v8; }
                    }
                } else if (drm_format == DRM_FORMAT_P010) {
                    for (int r = 0; r < height; r++) {
                        uint16_t *ln = (uint16_t *)(map + off0 + (size_t)r * stride0);
                        for (int c = 0; c < width; c++) ln[c] = (uint16_t)y8 << 8;
                    }
                    for (int r = 0; r < height / 2; r++) {
                        uint16_t *ln = (uint16_t *)(map + off1 + (size_t)r * stride1);
                        for (int c = 0; c < width; c += 2) {
                            ln[c] = (uint16_t)u8 << 8;
                            ln[c + 1] = (uint16_t)v8 << 8;
                        }
                    }
                }
                munmap(map, len);
            }
            close(fd);
        }
    }
    struct zwp_linux_buffer_params_v1 *params = zwp_linux_dmabuf_v1_create_params(dmabuf);
    uint64_t modifier = gbm_bo_get_modifier(object);
    int planes = gbm_bo_get_plane_count(object);
    for (int i = 0; i < planes; i++) {
        int fd = gbm_bo_get_fd_for_plane(object, i);
        zwp_linux_buffer_params_v1_add(params, fd, (uint32_t)i, gbm_bo_get_offset(object, i),
            gbm_bo_get_stride_for_plane(object, i),
            (uint32_t)(modifier >> 32), (uint32_t)(modifier & 0xffffffffu));
        close(fd);
    }
    struct wl_buffer *made = zwp_linux_buffer_params_v1_create_immed(params, width, height,
        drm_format, 0);
    zwp_linux_buffer_params_v1_destroy(params);
    return made;
}

static struct wl_buffer *make_dumb_dmabuf(int width, int height) {
    const char *card = getenv("TOMOE_CLIENT_DRM_NODE");
    int fd = open(card && *card ? card : "/dev/dri/card0", O_RDWR | O_CLOEXEC);
    if (fd < 0) return NULL;
    bool nv12 = drm_format == DRM_FORMAT_NV12;
    bool p010 = drm_format == DRM_FORMAT_P010;
    uint32_t bpp = nv12 ? 8 : p010 ? 16 : 32;
    uint32_t alloc_h = (nv12 || p010) ? (uint32_t)(height + height / 2) : (uint32_t)height;
    uint32_t handle = 0, pitch = 0;
    uint64_t size = 0, moff = 0;
    if (drmModeCreateDumbBuffer(fd, (uint32_t)width, alloc_h, bpp, 0, &handle, &pitch, &size) ||
            drmModeMapDumbBuffer(fd, handle, &moff)) {
        if (handle) drmModeDestroyDumbBuffer(fd, handle);
        close(fd);
        return NULL;
    }
    uint8_t *map = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, (off_t)moff);
    if (map != MAP_FAILED) {
        if (nv12) fill_nv12(map, pitch, width, height);
        else if (p010) fill_p010(map, pitch, width, height);
        else {
            uint32_t *words = (uint32_t *)map;
            for (size_t i = 0; i < size / 4; i++) words[i] = colour;
        }
        munmap(map, size);
    }
    int prime = -1;
    struct wl_buffer *made = NULL;
    if (drmPrimeHandleToFD(fd, handle, DRM_CLOEXEC, &prime) == 0 && prime >= 0) {
        struct zwp_linux_buffer_params_v1 *params = zwp_linux_dmabuf_v1_create_params(dmabuf);
        zwp_linux_buffer_params_v1_add(params, prime, 0, 0, pitch, 0, 0);
        if (nv12 || p010) {
            int p1 = dup(prime);
            zwp_linux_buffer_params_v1_add(params, p1, 1, pitch * (uint32_t)height, pitch, 0, 0);
            close(p1);
        }
        made = zwp_linux_buffer_params_v1_create_immed(params, width, height, drm_format, 0);
        zwp_linux_buffer_params_v1_destroy(params);
        close(prime);
    }
    drmModeDestroyDumbBuffer(fd, handle);
    close(fd);
    return made;
}

static struct wl_buffer *make_one_buffer(int width, int height) {
    struct wl_buffer *made = make_udmabuf(width, height);
    if (!made) made = make_gbm_buffer(width, height, DRM_FORMAT_MOD_LINEAR);
    if (!made) made = make_dumb_dmabuf(width, height);
    if (!made) fail("could not allocate %dx%d dmabuf format 0x%08x", width, height, drm_format);
    return made;
}

static struct wl_buffer *make_buffer(int width, int height) {
    if (!chosen_known || chosen_modifier == DRM_FORMAT_MOD_LINEAR ||
            chosen_modifier == DRM_FORMAT_MOD_INVALID)
        return make_one_buffer(width, height);
    struct wl_buffer *made = make_gbm_buffer(width, height, chosen_modifier);
    return made ? made : make_one_buffer(width, height);
}

static void feedback_format_table(void *data, struct zwp_linux_dmabuf_feedback_v1 *fb,
        int32_t fd, uint32_t size) {
    (void)data; (void)fb;
    if (fb_table_map) munmap(fb_table_map, fb_table_bytes);
    fb_table = NULL;
    fb_table_map = NULL;
    fb_table_len = fb_table_bytes = 0;
    if (fd >= 0 && size >= sizeof(struct feedback_format)) {
        void *map = mmap(NULL, size, PROT_READ, MAP_SHARED, fd, 0);
        if (map != MAP_FAILED) {
            fb_table_map = map;
            fb_table_bytes = size;
            fb_table_len = size / sizeof(struct feedback_format);
            fb_table = map;
        }
    }
    if (fd >= 0) close(fd);
}

static void feedback_main_device(void *data, struct zwp_linux_dmabuf_feedback_v1 *fb,
        struct wl_array *dev) {
    (void)data; (void)fb;
    fb_main_device = dev->size >= sizeof(dev_t) ? *(dev_t *)dev->data : 0;
    fb_tranches = 0;
    fb_scanout = 0;
    fb_formats = 0;
    cur_tranche_flags = 0;
    cur_tranche_formats = 0;
}

static void feedback_tranche_target_device(void *data, struct zwp_linux_dmabuf_feedback_v1 *fb,
        struct wl_array *dev) {
    (void)data; (void)fb; (void)dev;
    cur_tranche_flags = 0;
    cur_tranche_formats = 0;
    fb_index_count = 0;
}

static void feedback_tranche_formats(void *data, struct zwp_linux_dmabuf_feedback_v1 *fb,
        struct wl_array *indices) {
    (void)data; (void)fb;
    cur_tranche_formats += (int)(indices->size / sizeof(uint16_t));
    const uint16_t *values = indices->data;
    size_t count = indices->size / sizeof(uint16_t);
    for (size_t i = 0; i < count && fb_index_count < FEEDBACK_TRANCHE; i++)
        fb_indices[fb_index_count++] = values[i];
}

static void choose_modifier(void) {
    uint64_t fallback = DRM_FORMAT_MOD_LINEAR;
    bool have_fallback = false;
    for (size_t i = 0; i < fb_index_count; i++) {
        size_t index = fb_indices[i];
        if (index >= fb_table_len) continue;
        const struct feedback_format *entry = &fb_table[index];
        if (entry->format != drm_format) continue;
        uint64_t modifier = entry->modifier == DRM_FORMAT_MOD_INVALID ? DRM_FORMAT_MOD_LINEAR :
            entry->modifier;
        if (modifier == DRM_FORMAT_MOD_LINEAR) {
            chosen_modifier = modifier;
            chosen_known = true;
            printf("chosen format=0x%08x modifier=linear from %zu tranche entries\n",
                entry->format, fb_index_count);
            fflush(stdout);
            return;
        }
        if (!have_fallback) {
            fallback = modifier;
            have_fallback = true;
        }
    }
    if (!have_fallback) return;
    chosen_modifier = fallback;
    chosen_known = true;
    printf("chosen format=0x%08x modifier=0x%016llx from %zu tranche entries\n", drm_format,
        (unsigned long long)chosen_modifier, fb_index_count);
    fflush(stdout);
}

static void feedback_tranche_flags(void *data, struct zwp_linux_dmabuf_feedback_v1 *fb,
        uint32_t flags) {
    (void)data; (void)fb;
    cur_tranche_flags |= flags;
}

static void feedback_tranche_done(void *data, struct zwp_linux_dmabuf_feedback_v1 *fb) {
    (void)data; (void)fb;
    fb_tranches++;
    fb_formats += cur_tranche_formats;
    bool scanout = (cur_tranche_flags & ZWP_LINUX_DMABUF_FEEDBACK_V1_TRANCHE_FLAGS_SCANOUT) &&
        cur_tranche_formats > 0;
    if (scanout) fb_scanout = 1;
    if (fb_index_count && (scanout || !chosen_known)) choose_modifier();
    cur_tranche_flags = 0;
    cur_tranche_formats = 0;
    fb_index_count = 0;
}

static unsigned feedback_set_signature(void) {
    unsigned signature = 2166136261u;
    const unsigned parts[] = { (unsigned)fb_tranches, (unsigned)fb_formats,
        (unsigned)fb_scanout, (unsigned)((uint64_t)fb_main_device >> 32),
        (unsigned)((uint64_t)fb_main_device & 0xffffffffu) };
    for (size_t i = 0; i < sizeof(parts) / sizeof(parts[0]); i++)
        signature = (signature ^ parts[i]) * 16777619u;
    return signature;
}

static void feedback_done(void *data, struct zwp_linux_dmabuf_feedback_v1 *fb) {
    (void)data; (void)fb;
    fb_done_seen = true;
    if (!chosen_known && !warned_no_format) {
        warned_no_format = true;
        printf("no 0x%08x buffer in this feedback; allocating one anyway\n", drm_format);
        fflush(stdout);
    }
    fb_rounds++;
    unsigned signature = feedback_set_signature();
    bool changed = false;
    if (fb_rounds == 1) fb_signature = signature;
    else if (signature != fb_signature) {
        fb_signature = signature;
        changed = true;
    }
    printf("feedback round=%d scanout=%d tranches=%d formats=%d%s\n",
        fb_rounds, fb_scanout, fb_tranches, fb_formats, changed ? " changed" : "");
    if (changed && committed && allocated_w > 0) {
        struct wl_buffer *first = make_buffer(allocated_w, allocated_h);
        struct wl_buffer *second = target_frames > 0 ? make_buffer(allocated_w, allocated_h) :
            first;
        wl_buffer_destroy(buffers[0]);
        if (buffers[1] != buffers[0]) wl_buffer_destroy(buffers[1]);
        buffers[0] = first;
        buffers[1] = second;
        wl_surface_attach(surface, buffers[active_slot], 0, 0);
        wl_surface_damage_buffer(surface, 0, 0, INT32_MAX, INT32_MAX);
        wl_surface_commit(surface);
        fb_reallocs++;
        printf("realloc round=%d reallocs=%d\n", fb_rounds, fb_reallocs);
    }
    if (waiting_to_map && !committed) {
        waiting_to_map = false;
        commit_first_frame();
    }
    fflush(stdout);
}

static const struct zwp_linux_dmabuf_feedback_v1_listener feedback_listener = {
    .done = feedback_done,
    .format_table = feedback_format_table,
    .main_device = feedback_main_device,
    .tranche_done = feedback_tranche_done,
    .tranche_target_device = feedback_tranche_target_device,
    .tranche_formats = feedback_tranche_formats,
    .tranche_flags = feedback_tranche_flags,
};

static void registry_global(void *data, struct wl_registry *registry, uint32_t name,
        const char *interface, uint32_t version) {
    (void)data;
    if (strcmp(interface, wl_compositor_interface.name) == 0)
        compositor = wl_registry_bind(registry, name, &wl_compositor_interface,
            version < 4 ? version : 4);
    else if (strcmp(interface, xdg_wm_base_interface.name) == 0)
        wm_base = wl_registry_bind(registry, name, &xdg_wm_base_interface, 1);
    else if (strcmp(interface, zwp_linux_dmabuf_v1_interface.name) == 0) {
        dmabuf_version = version < 4 ? version : 4;
        dmabuf = wl_registry_bind(registry, name, &zwp_linux_dmabuf_v1_interface, dmabuf_version);
    } else if (strcmp(interface, wp_viewporter_interface.name) == 0)
        viewporter = wl_registry_bind(registry, name, &wp_viewporter_interface, 1);
    else if (strcmp(interface, wl_output_interface.name) == 0 && !output)
        output = wl_registry_bind(registry, name, &wl_output_interface, 1);
    else if (strcmp(interface, wl_seat_interface.name) == 0 && !seat)
        seat = wl_registry_bind(registry, name, &wl_seat_interface, 1);
}

static void registry_remove(void *data, struct wl_registry *registry, uint32_t name) {
    (void)data; (void)registry; (void)name;
}

static const struct wl_registry_listener registry_listener = {
    .global = registry_global, .global_remove = registry_remove,
};

static void output_geometry(void *data, struct wl_output *source, int32_t x, int32_t y,
        int32_t physical_width, int32_t physical_height, int32_t subpixel,
        const char *make, const char *model, int32_t transform) {
    (void)data; (void)source; (void)x; (void)y; (void)physical_width; (void)physical_height;
    (void)subpixel; (void)make; (void)model; (void)transform;
}

static void output_mode(void *data, struct wl_output *source, uint32_t flags, int32_t width,
        int32_t height, int32_t refresh) {
    (void)data; (void)source; (void)refresh;
    if (!(flags & WL_OUTPUT_MODE_CURRENT) || width <= 0 || height <= 0) return;
    output_width = width;
    output_height = height;
}

static void output_done(void *data, struct wl_output *source) { (void)data; (void)source; }
static void output_scale(void *data, struct wl_output *source, int32_t scale) {
    (void)data; (void)source; (void)scale;
}

static const struct wl_output_listener output_listener = {
    .geometry = output_geometry, .mode = output_mode,
    .done = output_done, .scale = output_scale,
};

static void pointer_enter(void *data, struct wl_pointer *pointer, uint32_t serial,
        struct wl_surface *entered, wl_fixed_t x, wl_fixed_t y) {
    (void)data; (void)entered; (void)x; (void)y;
    wl_pointer_set_cursor(pointer, serial, NULL, 0, 0);
    puts("hidden");
    fflush(stdout);
}

static void pointer_leave(void *data, struct wl_pointer *pointer, uint32_t serial,
        struct wl_surface *left) {
    (void)data; (void)pointer; (void)serial; (void)left;
}

static void pointer_motion(void *data, struct wl_pointer *pointer, uint32_t time,
        wl_fixed_t x, wl_fixed_t y) {
    (void)data; (void)pointer; (void)time; (void)x; (void)y;
}

static void pointer_button(void *data, struct wl_pointer *pointer, uint32_t serial,
        uint32_t time, uint32_t button, uint32_t state) {
    (void)data; (void)pointer; (void)serial; (void)time; (void)button; (void)state;
}

static void pointer_axis(void *data, struct wl_pointer *pointer, uint32_t time,
        uint32_t axis, wl_fixed_t value) {
    (void)data; (void)pointer; (void)time; (void)axis; (void)value;
}

static const struct wl_pointer_listener pointer_listener = {
    .enter = pointer_enter, .leave = pointer_leave, .motion = pointer_motion,
    .button = pointer_button, .axis = pointer_axis,
};

static void seat_capabilities(void *data, struct wl_seat *source, uint32_t capabilities) {
    (void)data;
    static struct wl_pointer *pointer;
    if ((capabilities & WL_SEAT_CAPABILITY_POINTER) && !pointer) {
        pointer = wl_seat_get_pointer(source);
        wl_pointer_add_listener(pointer, &pointer_listener, NULL);
    }
}

static void seat_name(void *data, struct wl_seat *source, const char *name) {
    (void)data; (void)source; (void)name;
}

static const struct wl_seat_listener seat_listener = {
    .capabilities = seat_capabilities, .name = seat_name,
};

static void ping(void *data, struct xdg_wm_base *base, uint32_t serial) {
    (void)data;
    xdg_wm_base_pong(base, serial);
}

static const struct xdg_wm_base_listener wm_listener = { .ping = ping };

static void apply_viewport(int cfg_w, int cfg_h) {
    if (!has_vp_src && !has_vp_dst && !fixed_buf_w) return;
    if (!viewporter) fail("the compositor does not advertise wp_viewporter");
    if (!viewport) viewport = wp_viewporter_get_viewport(viewporter, surface);
    if (has_vp_src)
        wp_viewport_set_source(viewport, wl_fixed_from_double(vp_src_x),
            wl_fixed_from_double(vp_src_y), wl_fixed_from_double(vp_src_w),
            wl_fixed_from_double(vp_src_h));
    if (has_vp_dst)
        wp_viewport_set_destination(viewport, vp_dst_w, vp_dst_h);
    else if (fixed_buf_w && cfg_w > 0 && cfg_h > 0)
        wp_viewport_set_destination(viewport, cfg_w, cfg_h);
}

static void frame_done_cb(void *data, struct wl_callback *cb, uint32_t time);

static const struct wl_callback_listener frame_listener = { .done = frame_done_cb };

static void submit_next_frame(void) {
    if (sent_frames >= target_frames) {
        printf("frames %d\n", sent_frames);
        fflush(stdout);
        target_frames = 0;
        return;
    }
    sent_frames++;
    active_slot ^= 1;
    struct wl_callback *cb = wl_surface_frame(surface);
    wl_callback_add_listener(cb, &frame_listener, NULL);
    wl_surface_attach(surface, buffers[active_slot], 0, 0);
    wl_surface_damage_buffer(surface, 0, 0, INT32_MAX, INT32_MAX);
    wl_surface_commit(surface);
}

static void frame_done_cb(void *data, struct wl_callback *cb, uint32_t time) {
    (void)data; (void)time;
    wl_callback_destroy(cb);
    if (target_frames > 0) submit_next_frame();
}

static void toplevel_configure(void *data, struct xdg_toplevel *toplevel, int32_t width,
        int32_t height, struct wl_array *states) {
    (void)data; (void)toplevel; (void)states;
    if (width > 0 && height > 0) { pending_width = width; pending_height = height; }
}

static void toplevel_close(void *data, struct xdg_toplevel *toplevel) {
    (void)data; (void)toplevel;
    exit(0);
}

static const struct xdg_toplevel_listener toplevel_listener = {
    .configure = toplevel_configure, .close = toplevel_close,
};

static void commit_first_frame(void) {
    int cfg_w = pending_cfg_w > 0 ? pending_cfg_w : output_width;
    int cfg_h = pending_cfg_h > 0 ? pending_cfg_h : output_height;
    int buf_w = fixed_buf_w > 0 ? fixed_buf_w : cfg_w;
    int buf_h = fixed_buf_h > 0 ? fixed_buf_h : cfg_h;
    allocated_w = buf_w;
    allocated_h = buf_h;
    buffers[0] = make_buffer(buf_w, buf_h);
    buffers[1] = target_frames > 0 ? make_buffer(buf_w, buf_h) : buffers[0];
    apply_viewport(cfg_w, cfg_h);
    if (target_frames > 0) {
        struct wl_callback *cb = wl_surface_frame(surface);
        wl_callback_add_listener(cb, &frame_listener, NULL);
    }
    wl_surface_attach(surface, buffers[0], 0, 0);
    wl_surface_damage_buffer(surface, 0, 0, INT32_MAX, INT32_MAX);
    wl_surface_commit(surface);
    committed = 1;
    step("mapped %dx%d", has_vp_dst ? vp_dst_w : cfg_w, has_vp_dst ? vp_dst_h : cfg_h);
}

static void surface_configure(void *data, struct xdg_surface *xdg, uint32_t serial) {
    (void)data;
    xdg_surface_ack_configure(xdg, serial);
    pending_cfg_w = pending_width > 0 ? pending_width : output_width;
    pending_cfg_h = pending_height > 0 ? pending_height : output_height;
    if (committed) {
        apply_viewport(pending_cfg_w, pending_cfg_h);
        wl_surface_attach(surface, buffers[active_slot], 0, 0);
        wl_surface_commit(surface);
        step("configure %dx%d", pending_width, pending_height);
        return;
    }
    if (want_feedback && !fb_done_seen) {
        waiting_to_map = true;
        return;
    }
    commit_first_frame();
}

static const struct xdg_surface_listener surface_listener = { .configure = surface_configure };

static void parse_wh(const char *text, int *w, int *h) {
    if (sscanf(text, "%dx%d", w, h) != 2 || *w <= 0 || *h <= 0)
        fail("expected WxH, got \"%s\"", text);
}

int main(int count, char **arguments) {
    for (int i = 1; i < count; i++) {
        if (strcmp(arguments[i], "--colour") == 0 && i + 1 < count) {
            const char *text = arguments[++i];
            char *end = NULL;
            unsigned long value = strtoul(text, &end, 16);
            if (!end || *end || strlen(text) != 6) fail("--colour needs six hex digits");
            colour = 0xff000000u | (uint32_t)value;
        } else if (strcmp(arguments[i], "--format") == 0 && i + 1 < count) {
            const char *fmt = arguments[++i];
            if (!strcmp(fmt, "nv12")) drm_format = DRM_FORMAT_NV12;
            else if (!strcmp(fmt, "p010")) drm_format = DRM_FORMAT_P010;
            else if (!strcmp(fmt, "xrgb8888")) drm_format = DRM_FORMAT_XRGB8888;
            else if (!strcmp(fmt, "argb8888")) drm_format = DRM_FORMAT_ARGB8888;
            else fail("unknown --format \"%s\"", fmt);
        } else if (strcmp(arguments[i], "--app-id") == 0 && i + 1 < count) {
            app_id = arguments[++i];
        } else if (strcmp(arguments[i], "--buffer-size") == 0 && i + 1 < count) {
            parse_wh(arguments[++i], &fixed_buf_w, &fixed_buf_h);
        } else if (strcmp(arguments[i], "--viewport-src") == 0 && i + 1 < count) {
            const char *s = arguments[++i];
            if (sscanf(s, "%lf,%lf,%lfx%lf", &vp_src_x, &vp_src_y, &vp_src_w, &vp_src_h) != 4 ||
                    vp_src_x < 0 || vp_src_y < 0 || vp_src_w <= 0 || vp_src_h <= 0)
                fail("expected X,Y,WxH for --viewport-src, got \"%s\"", s);
            has_vp_src = true;
        } else if (strcmp(arguments[i], "--viewport-dst") == 0 && i + 1 < count) {
            parse_wh(arguments[++i], &vp_dst_w, &vp_dst_h);
            has_vp_dst = true;
        } else if (strcmp(arguments[i], "--frames") == 0 && i + 1 < count) {
            target_frames = atoi(arguments[++i]);
        } else if (strcmp(arguments[i], "--feedback") == 0) {
            want_feedback = true;
        } else if (strcmp(arguments[i], "--seconds") == 0 && i + 1 < count) {
            alarm((unsigned)atoi(arguments[++i]));
        } else {
            fail("unknown argument \"%s\"", arguments[i]);
        }
    }
    display = wl_display_connect(NULL);
    if (!display) fail("cannot connect to the display named by $WAYLAND_DISPLAY");
    struct wl_registry *registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &registry_listener, NULL);
    if (wl_display_roundtrip(display) < 0) fail("the registry roundtrip failed");
    if (!compositor) fail("the compositor does not advertise wl_compositor");
    if (!wm_base) fail("the compositor does not advertise xdg_wm_base");
    if (!dmabuf) fail("the compositor does not advertise zwp_linux_dmabuf_v1");
    if (output) wl_output_add_listener(output, &output_listener, NULL);
    if (seat) wl_seat_add_listener(seat, &seat_listener, NULL);
    if (wl_display_roundtrip(display) < 0) fail("the output roundtrip failed");
    xdg_wm_base_add_listener(wm_base, &wm_listener, NULL);
    surface = wl_compositor_create_surface(compositor);
    if (want_feedback) {
        if (dmabuf_version < 4) fail("zwp_linux_dmabuf_v1 version %u < 4", dmabuf_version);
        feedback = zwp_linux_dmabuf_v1_get_surface_feedback(dmabuf, surface);
        zwp_linux_dmabuf_feedback_v1_add_listener(feedback, &feedback_listener, NULL);
    }
    struct xdg_surface *xdg = xdg_wm_base_get_xdg_surface(wm_base, surface);
    xdg_surface_add_listener(xdg, &surface_listener, NULL);
    struct xdg_toplevel *toplevel = xdg_surface_get_toplevel(xdg);
    xdg_toplevel_add_listener(toplevel, &toplevel_listener, NULL);
    xdg_toplevel_set_title(toplevel, app_id);
    xdg_toplevel_set_app_id(toplevel, app_id);
    wl_surface_commit(surface);
    while (wl_display_dispatch(display) >= 0) { }
    return 0;
}
