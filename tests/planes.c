#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <drm_fourcc.h>
#include <unistd.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

enum { P_CRTC_ID, P_FB_ID, P_SRC_X, P_SRC_Y, P_SRC_W, P_SRC_H, P_CRTC_X, P_CRTC_Y, P_CRTC_W,
    P_CRTC_H, P_TYPE, P_MODE_ID, P_ACTIVE, P_COUNT };

static const char *const prop_names[P_COUNT] = { "CRTC_ID", "FB_ID", "SRC_X", "SRC_Y", "SRC_W",
    "SRC_H", "CRTC_X", "CRTC_Y", "CRTC_W", "CRTC_H", "type", "MODE_ID", "ACTIVE" };

struct prop {
    uint32_t id;
    uint64_t value;
};

struct props {
    struct prop item[P_COUNT];
};

static void props_read(int fd, uint32_t object, uint32_t type, struct props *props) {
    memset(props, 0, sizeof(*props));
    drmModeObjectProperties *list = drmModeObjectGetProperties(fd, object, type);
    if (!list) return;
    for (uint32_t i = 0; i < list->count_props; i++) {
        drmModePropertyRes *prop = drmModeGetProperty(fd, list->props[i]);
        if (!prop) continue;
        for (int p = 0; p < P_COUNT; p++) {
            if (strcmp(prop->name, prop_names[p])) continue;
            props->item[p].id = prop->prop_id;
            props->item[p].value = list->prop_values[i];
        }
        drmModeFreeProperty(prop);
    }
    drmModeFreeObjectProperties(list);
}

static const char *type_name(uint64_t type) {
    return type == DRM_PLANE_TYPE_PRIMARY ? "PRIMARY" : type == DRM_PLANE_TYPE_CURSOR ?
        "CURSOR" : type == DRM_PLANE_TYPE_OVERLAY ? "OVERLAY" : "OTHER";
}

static int dump(int fd) {
    drmModePlaneRes *planes = drmModeGetPlaneResources(fd);
    if (!planes) {
        fprintf(stderr, "planes: %s\n", strerror(errno));
        return 1;
    }
    for (uint32_t i = 0; i < planes->count_planes; i++) {
        struct props props;
        props_read(fd, planes->planes[i], DRM_MODE_OBJECT_PLANE, &props);
        uint64_t src_w = props.item[P_SRC_W].value >> 16, src_h = props.item[P_SRC_H].value >> 16;
        printf("plane %u type=%s crtc=%llu fb=%llu dst=%llu,%llu+%llux%llu src=%llu,%llu+%llux%llu\n",
            planes->planes[i], type_name(props.item[P_TYPE].value),
            (unsigned long long)props.item[P_CRTC_ID].value,
            (unsigned long long)props.item[P_FB_ID].value,
            (unsigned long long)props.item[P_CRTC_X].value,
            (unsigned long long)props.item[P_CRTC_Y].value,
            (unsigned long long)props.item[P_CRTC_W].value,
            (unsigned long long)props.item[P_CRTC_H].value,
            (unsigned long long)props.item[P_SRC_X].value >> 16,
            (unsigned long long)props.item[P_SRC_Y].value >> 16,
            (unsigned long long)src_w, (unsigned long long)src_h);
        drmModeFB2 *fb = props.item[P_FB_ID].value ?
            drmModeGetFB2(fd, (uint32_t)props.item[P_FB_ID].value) : NULL;
        if (fb) {
            printf("fb %u width=%u height=%u format=0x%08x modifier=0x%016llx\n", fb->fb_id,
                fb->width, fb->height, fb->pixel_format, (unsigned long long)fb->modifier);
            drmModeFreeFB2(fb);
        }
    }
    drmModeFreePlaneResources(planes);
    drmModeRes *res = drmModeGetResources(fd);
    if (res) {
        for (int i = 0; i < res->count_crtcs; i++) printf("crtc %u\n", res->crtcs[i]);
        for (int i = 0; i < res->count_connectors; i++) {
            drmModeConnector *c = drmModeGetConnector(fd, res->connectors[i]);
            if (!c) continue;
            printf("connector %u name=%s connection=%d\n", c->connector_id,
                drmModeGetConnectorTypeName(c->connector_type),
                c->connection == DRM_MODE_CONNECTED);
            for (int m = 0; m < c->count_modes; m++)
                printf("mode %ux%u@%u\n", c->modes[m].hdisplay, c->modes[m].vdisplay,
                    c->modes[m].vrefresh);
            drmModeFreeConnector(c);
        }
        drmModeFreeResources(res);
    }
    return 0;
}

static int add_prop(drmModeAtomicReq *req, int fd, uint32_t object, uint32_t type,
        const char *name, uint64_t value) {
    struct props props;
    props_read(fd, object, type, &props);
    for (int p = 0; p < P_COUNT; p++) {
        if (strcmp(prop_names[p], name)) continue;
        if (!props.item[p].id) return -1;
        return drmModeAtomicAddProperty(req, object, props.item[p].id, value);
    }
    return -1;
}

static uint32_t find_plane(int fd, uint32_t type, uint32_t crtc_index) {
    drmModePlaneRes *planes = drmModeGetPlaneResources(fd);
    uint32_t found = 0;
    for (uint32_t i = 0; planes && i < planes->count_planes; i++) {
        struct props props;
        props_read(fd, planes->planes[i], DRM_MODE_OBJECT_PLANE, &props);
        if (props.item[P_TYPE].value != type) continue;
        drmModePlane *info = drmModeGetPlane(fd, planes->planes[i]);
        bool possible = info && (info->possible_crtcs & (1u << crtc_index));
        if (info) drmModeFreePlane(info);
        if (!possible) continue;
        found = planes->planes[i];
        break;
    }
    drmModeFreePlaneResources(planes);
    return found;
}

static uint32_t dumb(int fd, int width, int height, uint32_t color, uint32_t *handle_out) {
    struct drm_mode_create_dumb create = { .width = (uint32_t)width, .height = (uint32_t)height,
        .bpp = 32 };
    if (drmIoctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &create)) return 0;
    uint32_t fb = 0, handle = create.handle;
    if (drmModeAddFB(fd, (uint32_t)width, (uint32_t)height, 24, 32, create.pitch, handle, &fb))
        return 0;
    uint64_t offset = 0;
    if (drmModeMapDumbBuffer(fd, handle, &offset)) return 0;
    void *map = mmap(NULL, create.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, (off_t)offset);
    if (map != MAP_FAILED) {
        uint32_t *words = map;
        for (size_t i = 0; i < create.size / sizeof(*words); i++) words[i] = color;
        munmap(map, create.size);
    }
    if (handle_out) *handle_out = handle;
    return fb;
}

static const char *result(int ret) {
    static char text[64];
    if (ret == 0) return "ok";
    snprintf(text, sizeof(text), "%s", strerror(-ret));
    return text;
}

static void plane_props(drmModeAtomicReq *req, int fd, uint32_t plane, uint32_t crtc,
        uint32_t fb, int x, int y, int width, int height) {
    add_prop(req, fd, plane, DRM_MODE_OBJECT_PLANE, "FB_ID", fb);
    add_prop(req, fd, plane, DRM_MODE_OBJECT_PLANE, "CRTC_ID", fb ? crtc : 0);
    add_prop(req, fd, plane, DRM_MODE_OBJECT_PLANE, "SRC_X", 0);
    add_prop(req, fd, plane, DRM_MODE_OBJECT_PLANE, "SRC_Y", 0);
    add_prop(req, fd, plane, DRM_MODE_OBJECT_PLANE, "SRC_W", (uint64_t)width << 16);
    add_prop(req, fd, plane, DRM_MODE_OBJECT_PLANE, "SRC_H", (uint64_t)height << 16);
    add_prop(req, fd, plane, DRM_MODE_OBJECT_PLANE, "CRTC_X", (uint64_t)(int64_t)x);
    add_prop(req, fd, plane, DRM_MODE_OBJECT_PLANE, "CRTC_Y", (uint64_t)(int64_t)y);
    add_prop(req, fd, plane, DRM_MODE_OBJECT_PLANE, "CRTC_W", (uint64_t)width);
    add_prop(req, fd, plane, DRM_MODE_OBJECT_PLANE, "CRTC_H", (uint64_t)height);
}

static int pending(int fd, uint32_t crtc, uint32_t primary, uint32_t primary_fb, uint32_t overlay,
        int x, int y, int width, int height, uint32_t overlay_fb, uint32_t flags) {
    drmModeAtomicReq *req = drmModeAtomicAlloc();
    if (!req) return -ENOMEM;
    plane_props(req, fd, primary, crtc, primary_fb, 0, 0, width, height);
    if (overlay) plane_props(req, fd, overlay, crtc, overlay_fb, x, y, 64, 64);
    int ret = drmModeAtomicCommit(fd, req, flags, NULL);
    if (ret) ret = -errno;
    drmModeAtomicFree(req);
    return ret;
}


static void blend_pixel(uint8_t *out, const uint8_t *in, int alpha, bool swap) {
    uint8_t red = in[swap ? 2 : 0], green = in[1], blue = in[swap ? 0 : 2];
    uint8_t rgb[3] = { red, green, blue };
    for (int i = 0; i < 3; i++)
        out[i] = alpha == 255 ? rgb[i] : (uint8_t)((rgb[i] * alpha + out[i] * (255 - alpha)) / 255);
}

static int compose(int fd, const char *out) {
    drmModeRes *res = drmModeGetResources(fd);
    drmModePlaneRes *planes = drmModeGetPlaneResources(fd);
    if (!res || !planes) {
        fprintf(stderr, "planes: no resources\n");
        return 1;
    }
    uint32_t crtc = 0;
    int width = 0, height = 0;
    for (int i = 0; i < res->count_crtcs && !crtc; i++) {
        struct props props;
        props_read(fd, res->crtcs[i], DRM_MODE_OBJECT_CRTC, &props);
        if (!props.item[P_ACTIVE].value || !props.item[P_MODE_ID].value) continue;
        drmModePropertyBlobRes *mode = drmModeGetPropertyBlob(fd,
            (uint32_t)props.item[P_MODE_ID].value);
        if (!mode || mode->length < sizeof(drmModeModeInfo)) {
            drmModeFreePropertyBlob(mode);
            continue;
        }
        const drmModeModeInfo *info = mode->data;
        width = info->hdisplay;
        height = info->vdisplay;
        crtc = res->crtcs[i];
        drmModeFreePropertyBlob(mode);
    }
    if (!crtc || width <= 0 || height <= 0) {
        fprintf(stderr, "planes: no active crtc\n");
        return 1;
    }
    uint8_t *image = calloc((size_t)width * (size_t)height, 4);
    if (!image) return 1;
    const uint32_t order[] = { DRM_PLANE_TYPE_PRIMARY, DRM_PLANE_TYPE_OVERLAY,
        DRM_PLANE_TYPE_CURSOR };
    for (size_t pass = 0; pass < sizeof(order) / sizeof(*order); pass++) {
        uint32_t wanted = order[pass];
        for (uint32_t i = 0; i < planes->count_planes; i++) {
            struct props props;
            props_read(fd, planes->planes[i], DRM_MODE_OBJECT_PLANE, &props);
            if (props.item[P_TYPE].value != wanted) continue;
            if (props.item[P_CRTC_ID].value != crtc || !props.item[P_FB_ID].value) {
                printf("compose skip plane %u type=%u crtc=%llu fb=%llu\n", planes->planes[i],
                    wanted, (unsigned long long)props.item[P_CRTC_ID].value,
                    (unsigned long long)props.item[P_FB_ID].value);
                continue;
            }
            drmModeFB2 *fb = drmModeGetFB2(fd, (uint32_t)props.item[P_FB_ID].value);
            if (!fb || !fb->handles[0]) {
                drmModeFreeFB2(fb);
                fprintf(stderr, "planes: plane %u has no readable buffer\n",
                    planes->planes[i]);
                return 1;
            }
            if (fb->modifier != DRM_FORMAT_MOD_LINEAR && fb->modifier != DRM_FORMAT_MOD_INVALID) {
                fprintf(stderr, "planes: plane %u buffer is not linear (0x%llx)\n",
                    planes->planes[i], (unsigned long long)fb->modifier);
                return 1;
            }
            int prime = -1;
            if (drmPrimeHandleToFD(fd, fb->handles[0], DRM_CLOEXEC, &prime) || prime < 0) {
                fprintf(stderr, "planes: plane %u buffer cannot be exported\n",
                    planes->planes[i]);
                return 1;
            }
            size_t stride = fb->pitches[0], length = stride * fb->height;
            uint8_t *map = mmap(NULL, length, PROT_READ, MAP_SHARED, prime, 0);
            close(prime);
            if (map == MAP_FAILED) {
                fprintf(stderr, "planes: plane %u buffer cannot be mapped\n",
                    planes->planes[i]);
                return 1;
            }
            int sx = (int)(props.item[P_SRC_X].value >> 16);
            int sy = (int)(props.item[P_SRC_Y].value >> 16);
            int sw = (int)(props.item[P_SRC_W].value >> 16);
            int sh = (int)(props.item[P_SRC_H].value >> 16);
            int dx = (int)(int64_t)props.item[P_CRTC_X].value;
            int dy = (int)(int64_t)props.item[P_CRTC_Y].value;
            int dw = (int)props.item[P_CRTC_W].value;
            int dh = (int)props.item[P_CRTC_H].value;
            uint32_t format = fb->pixel_format;
            bool swap = format == DRM_FORMAT_XRGB8888 || format == DRM_FORMAT_ARGB8888;
            bool alpha = format == DRM_FORMAT_ARGB8888 || format == DRM_FORMAT_ABGR8888;
            for (int y = 0; y < dh; y++) {
                int py = dy + y;
                if (py < 0 || py >= height || y >= sh) continue;
                int row = sy + y * sh / dh;
                if (row < 0 || row >= (int)fb->height) continue;
                for (int x = 0; x < dw; x++) {
                    int px = dx + x;
                    if (px < 0 || px >= width || x >= sw) continue;
                    int column = sx + x * sw / dw;
                    if (column < 0 || column >= (int)fb->width) continue;
                    const uint8_t *source = map + (size_t)row * stride + (size_t)column * 4;
                    uint8_t *target = image + ((size_t)py * (size_t)width + (size_t)px) * 4;
                    blend_pixel(target, source, alpha ? source[3] : 255, swap);
                }
            }
            munmap(map, length);
            printf("compose plane %u type=%u fb=%llu dst=%d,%d+%dx%d src=%d,%d+%dx%d "
                "format=0x%08x\n", planes->planes[i], wanted,
                (unsigned long long)props.item[P_FB_ID].value, dx, dy, dw, dh, sx, sy, sw, sh,
                format);
            drmModeFreeFB2(fb);
        }
    }
    printf("compose %dx%d\n", width, height);
    FILE *file = fopen(out, "wb");
    if (!file) return 1;
    fprintf(file, "P6\n%d %d\n255\n", width, height);
    for (size_t i = 0; i < (size_t)width * (size_t)height; i++) fwrite(image + i * 4, 1, 3, file);
    fclose(file);
    free(image);
    return 0;
}

static int experiment(const char *path) {
    int fd = open(path, O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        fprintf(stderr, "planes: %s: %s\n", path, strerror(errno));
        return 1;
    }
    if (drmSetClientCap(fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1) ||
            drmSetClientCap(fd, DRM_CLIENT_CAP_ATOMIC, 1)) {
        fprintf(stderr, "planes: no atomic\n");
        return 1;
    }
    drmModeRes *res = drmModeGetResources(fd);
    if (!res) return 1;
    int index = -1;
    uint32_t connector_id = 0;
    drmModeModeInfo mode = {0};
    for (int i = 0; i < res->count_connectors && !connector_id; i++) {
        drmModeConnector *c = drmModeGetConnector(fd, res->connectors[i]);
        if (!c) continue;
        if (c->connection == DRM_MODE_CONNECTED && c->count_modes) {
            connector_id = c->connector_id;
            mode = c->modes[0];
        }
        drmModeFreeConnector(c);
    }
    if (!connector_id) {
        fprintf(stderr, "planes: no connected connector\n");
        return 1;
    }
    for (int i = 0; i < res->count_crtcs; i++) {
        if (index >= 0) break;
        index = i;
    }
    uint32_t crtc = res->crtcs[index];
    int width = mode.hdisplay, height = mode.vdisplay;
    uint32_t primary = find_plane(fd, DRM_PLANE_TYPE_PRIMARY, (uint32_t)index);
    uint32_t overlay = find_plane(fd, DRM_PLANE_TYPE_OVERLAY, (uint32_t)index);
    uint32_t cursor = find_plane(fd, DRM_PLANE_TYPE_CURSOR, (uint32_t)index);
    printf("experiment planes primary=%u overlay=%u cursor=%u crtc=%u mode=%dx%d\n",
        primary, overlay, cursor, crtc, width, height);
    uint32_t dummy = 0, overlay_handle = 0, cursor_handle = 0;
    uint32_t primary_fb = dumb(fd, width, height, 0x00112233, &dummy);
    uint32_t overlay_fb = dumb(fd, 64, 64, 0x00ff8800, &overlay_handle);
    uint32_t cursor_fb = dumb(fd, 16, 16, 0x00ffffff, &cursor_handle);
    if (!primary_fb || !overlay_fb || !cursor_fb) {
        fprintf(stderr, "planes: no dumb buffers\n");
        return 1;
    }

    drmModeAtomicReq *req = drmModeAtomicAlloc();
    uint32_t blob = 0;
    drmModeCreatePropertyBlob(fd, &mode, sizeof(mode), &blob);
    add_prop(req, fd, crtc, DRM_MODE_OBJECT_CRTC, "MODE_ID", blob);
    add_prop(req, fd, crtc, DRM_MODE_OBJECT_CRTC, "ACTIVE", 1);
    add_prop(req, fd, connector_id, DRM_MODE_OBJECT_CONNECTOR, "CRTC_ID", crtc);
    plane_props(req, fd, primary, crtc, primary_fb, 0, 0, width, height);
    if (overlay) plane_props(req, fd, overlay, crtc, overlay_fb, 0, 0, 64, 64);
    int ret = drmModeAtomicCommit(fd, req, DRM_MODE_ATOMIC_ALLOW_MODESET, NULL);
    printf("experiment modeset ret=%s\n", result(ret ? -errno : 0));
    drmModeAtomicFree(req);
    if (ret) return 1;
    if (overlay) {
        req = drmModeAtomicAlloc();
        plane_props(req, fd, overlay, crtc, overlay_fb, 4, 4, 64, 64);
        ret = drmModeAtomicCommit(fd, req, DRM_MODE_ATOMIC_NONBLOCK, NULL);
        printf("experiment overlay-enable ret=%s\n", result(ret ? -errno : 0));
        drmModeAtomicFree(req);
    }
    if (cursor) {
        struct drm_mode_cursor2 cur = { .flags = DRM_MODE_CURSOR_BO, .crtc_id = crtc,
            .x = 100, .y = 100, .width = 16, .height = 16, .handle = cursor_handle,
            .hot_x = 0, .hot_y = 0 };
        ret = drmIoctl(fd, DRM_IOCTL_MODE_CURSOR2, &cur);
        printf("experiment cursor-set ret=%s\n", result(ret ? -errno : 0));
    }

    ret = pending(fd, crtc, primary, primary_fb, overlay, 10, 10, width, height, overlay_fb,
        DRM_MODE_ATOMIC_NONBLOCK | DRM_MODE_PAGE_FLIP_EVENT);
    printf("experiment flip-submitted ret=%s\n", result(ret));
    ret = pending(fd, crtc, primary, primary_fb, overlay, 20, 20, width, height, overlay_fb,
        DRM_MODE_ATOMIC_TEST_ONLY);
    printf("experiment test-only-during-flip ret=%s\n", result(ret));
    ret = pending(fd, crtc, primary, primary_fb, overlay, 30, 30, width, height, overlay_fb,
        DRM_MODE_ATOMIC_NONBLOCK | DRM_MODE_PAGE_FLIP_EVENT);
    printf("experiment commit-during-flip ret=%s\n", result(ret));
    ret = pending(fd, crtc, primary, primary_fb, overlay, 40, 40, width, height, overlay_fb,
        DRM_MODE_ATOMIC_NONBLOCK | DRM_MODE_PAGE_FLIP_EVENT | DRM_MODE_PAGE_FLIP_ASYNC);
    printf("experiment async-during-flip ret=%s\n", result(ret));
    ret = pending(fd, crtc, primary, primary_fb, overlay, 50, 50, width, height, overlay_fb, 0);
    printf("experiment blocking-during-flip ret=%s\n", result(ret));
    if (cursor) {
        struct drm_mode_cursor2 cur = { .flags = DRM_MODE_CURSOR_MOVE, .crtc_id = crtc,
            .x = 200, .y = 200 };
        ret = drmIoctl(fd, DRM_IOCTL_MODE_CURSOR2, &cur);
        printf("experiment cursor-move-during-flip ret=%s\n", result(ret ? -errno : 0));
    }
    ret = pending(fd, crtc, primary, primary_fb, overlay, 60, 60, width, height, overlay_fb,
        DRM_MODE_ATOMIC_NONBLOCK | DRM_MODE_PAGE_FLIP_EVENT);
    printf("experiment commit-before-event ret=%s\n", result(ret));

    struct pollfd pfd = { .fd = fd, .events = POLLIN };
    int ready = poll(&pfd, 1, 2000);
    if (ready > 0) {
        drmEventContext context = { .version = 3 };
        drmHandleEvent(fd, &context);
    }
    printf("experiment flip-event ret=%s\n", result(ready > 0 ? 0 : -ETIMEDOUT));
    ret = pending(fd, crtc, primary, primary_fb, overlay, 70, 70, width, height, overlay_fb,
        DRM_MODE_ATOMIC_NONBLOCK | DRM_MODE_PAGE_FLIP_EVENT);
    printf("experiment commit-after-event ret=%s\n", result(ret));
    poll(&pfd, 1, 2000);
    drmEventContext context = { .version = 3 };
    drmHandleEvent(fd, &context);
    ret = pending(fd, crtc, primary, primary_fb, overlay, 80, 80, width, height, overlay_fb,
        DRM_MODE_ATOMIC_NONBLOCK | DRM_MODE_PAGE_FLIP_EVENT);
    printf("experiment commit-after-drain ret=%s\n", result(ret));
    poll(&pfd, 1, 2000);
    drmHandleEvent(fd, &context);
    drmModeFreeResources(res);
    close(fd);
    return 0;
}

int main(int argc, char **argv) {
    const char *path = argc > 2 ? argv[2] : "/dev/dri/card0";
    if (argc > 1 && !strcmp(argv[1], "dump")) {
        int fd = open(path, O_RDWR | O_CLOEXEC);
        if (fd < 0) {
            fprintf(stderr, "planes: %s: %s\n", path, strerror(errno));
            return 1;
        }
        drmSetClientCap(fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1);
        drmSetClientCap(fd, DRM_CLIENT_CAP_ATOMIC, 1);
        int ret = dump(fd);
        close(fd);
        return ret;
    }
    if (argc > 1 && !strcmp(argv[1], "experiment")) return experiment(path);
    if (argc > 2 && !strcmp(argv[1], "compose")) {
        int fd = open(path, O_RDWR | O_CLOEXEC);
        if (fd < 0) {
            fprintf(stderr, "planes: %s: %s\n", path, strerror(errno));
            return 1;
        }
        drmSetClientCap(fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1);
        drmSetClientCap(fd, DRM_CLIENT_CAP_ATOMIC, 1);
        int ret = compose(fd, argv[3]);
        close(fd);
        return ret;
    }
    fprintf(stderr, "usage: planes dump|experiment [device] | compose <device> <out.ppm>\n");
    return 2;
}
