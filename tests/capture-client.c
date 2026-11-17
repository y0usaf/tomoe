#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include <wayland-client.h>

#include "ext-foreign-toplevel-list-v1-client-protocol.h"
#include "ext-image-capture-source-v1-client-protocol.h"
#include "ext-image-copy-capture-v1-client-protocol.h"
#include "linux-dmabuf-v1-client-protocol.h"

#define DRM_FORMAT_ARGB8888 0x34325241u
#define DRM_FORMAT_XRGB8888 0x34325258u
#define DRM_FORMAT_MOD_LINEAR 0ull

struct udmabuf_create {
    uint32_t memfd;
    uint32_t flags;
    uint64_t offset;
    uint64_t size;
};
#define UDMABUF_CREATE _IOW('u', 0x42, struct udmabuf_create)

enum { TARGETS_MAX = 16 };

struct target {
    struct ext_foreign_toplevel_handle_v1 *handle;
    struct ext_image_capture_source_v1 *source;
    struct ext_image_copy_capture_session_v1 *session;
    struct ext_image_copy_capture_frame_v1 *frame;
    struct wl_buffer *buffer;
    struct wl_shm_pool *pool;
    void *pixels;
    size_t bytes;
    int memfd;
    int width, height;
    uint32_t format;
    uint64_t linear_modifier;
    bool described, started, closed, stopped;
    int captured, failed;
    double issued;
    char title[256];
    char app_id[256];
};

static struct wl_display *display;
static struct wl_registry *registry;
static struct wl_shm *shm;
static struct zwp_linux_dmabuf_v1 *dmabuf_manager;
static struct ext_foreign_toplevel_list_v1 *toplevels;
static struct ext_foreign_toplevel_image_capture_source_manager_v1 *source_manager;
static struct ext_output_image_capture_source_manager_v1 *output_sources;
static struct ext_image_copy_capture_manager_v1 *capture_manager;
static struct wl_output *output;
static struct target targets[TARGETS_MAX];
static int seen;
static int wanted = 1;
static int frames_per_target = 60;
static int delay_ms;
static int udmabuf_mode;
static int output_mode;
static const char *wanted_app_id = "capture-target";
static uint32_t shm_format = DRM_FORMAT_ARGB8888;
static int stopping;

static const char *program = "tomoe-capture-client";

static void fail(const char *format, ...) __attribute__((format(printf, 1, 2), noreturn));

static void fail(const char *format, ...) {
    va_list arguments;
    va_start(arguments, format);
    fputs(program, stderr);
    fputs(": ", stderr);
    vfprintf(stderr, format, arguments);
    fputc('\n', stderr);
    va_end(arguments);
    exit(1);
}

static double seconds(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return now.tv_sec + now.tv_nsec / 1e9;
}

static uint32_t shm_token(uint32_t format) {
    return format == DRM_FORMAT_XRGB8888 ? WL_SHM_FORMAT_XRGB8888 : WL_SHM_FORMAT_ARGB8888;
}

static void allocate(struct target *target) {
    size_t stride = (size_t)target->width * 4;
    long page = sysconf(_SC_PAGESIZE);
    target->bytes = stride * (size_t)target->height;
    size_t size = target->bytes;
    if (page > 0) size = (size + (size_t)page - 1) / (size_t)page * (size_t)page;
    target->memfd = memfd_create("tomoe-capture", MFD_CLOEXEC | MFD_ALLOW_SEALING);
    if (target->memfd < 0) fail("memfd_create: %s", strerror(errno));
    if (ftruncate(target->memfd, (off_t)size) != 0)
        fail("ftruncate to %zu: %s", size, strerror(errno));
    target->pixels = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED,
        target->memfd, 0);
    if (target->pixels == MAP_FAILED)
        fail("mmap of %zu bytes: %s", size, strerror(errno));
    memset(target->pixels, 0, size);
    fcntl(target->memfd, F_ADD_SEALS, F_SEAL_SHRINK);
    if (!udmabuf_mode) {
        target->pool = wl_shm_create_pool(shm, target->memfd, (int32_t)size);
        if (!target->pool) fail("wl_shm_create_pool refused %zu bytes", size);
        target->buffer = wl_shm_pool_create_buffer(target->pool, 0, target->width,
            target->height, (int32_t)stride, shm_token(shm_format));
        if (!target->buffer) fail("wl_shm_pool_create_buffer refused the pool");
        return;
    }
    int device = open("/dev/udmabuf", O_RDWR | O_CLOEXEC);
    if (device < 0) fail("cannot open /dev/udmabuf: %s", strerror(errno));
    struct udmabuf_create create = { .memfd = (uint32_t)target->memfd, .flags = 0,
        .offset = 0, .size = size };
    int fd = ioctl(device, UDMABUF_CREATE, &create);
    close(device);
    if (fd < 0) fail("udmabuf create: %s", strerror(errno));
    struct zwp_linux_buffer_params_v1 *params =
        zwp_linux_dmabuf_v1_create_params(dmabuf_manager);
    if (!params) fail("no dma-buf params object");
    zwp_linux_buffer_params_v1_add(params, fd, 0, 0, (uint32_t)stride,
        (uint32_t)(target->linear_modifier >> 32), (uint32_t)target->linear_modifier);
    target->buffer = zwp_linux_buffer_params_v1_create_immed(params, target->width,
        target->height, target->format, 0);
    zwp_linux_buffer_params_v1_destroy(params);
    close(fd);
    if (!target->buffer) fail("cannot wrap the udmabuf as a wl_buffer");
}

static void report(struct target *target, int index, const char *result) {
    uint32_t pixel = 0;
    if (target->pixels && target->width > 0 && target->height > 0) {
        size_t stride = (size_t)target->width * 4;
        pixel = ((uint32_t *)target->pixels)[stride / 4 * (size_t)(target->height / 2) +
            (size_t)(target->width / 2)];
    }
    printf("frame %d %d %s %.3f %08x\n", index, target->captured + target->failed, result,
        (seconds() - target->issued) * 1000.0, pixel);
    fflush(stdout);
}

static void capture(struct target *target, int index);

static void frame_ready(void *data, struct ext_image_copy_capture_frame_v1 *frame) {
    int index = (int)(intptr_t)data;
    struct target *target = &targets[index];
    target->frame = NULL;
    ext_image_copy_capture_frame_v1_destroy(frame);
    target->captured++;
    report(target, index, "ready");
    if (delay_ms > 0) usleep((useconds_t)delay_ms * 1000);
    if (!stopping && target->captured < frames_per_target && !target->closed)
        capture(target, index);
}

static void frame_failed(void *data, struct ext_image_copy_capture_frame_v1 *frame,
        uint32_t reason) {
    int index = (int)(intptr_t)data;
    struct target *target = &targets[index];
    target->frame = NULL;
    ext_image_copy_capture_frame_v1_destroy(frame);
    target->failed++;
    report(target, index, reason == EXT_IMAGE_COPY_CAPTURE_FRAME_V1_FAILURE_REASON_STOPPED ?
        "stopped" : "failed");
    if (!stopping && target->failed < 50 &&
            target->captured + target->failed < frames_per_target && !target->closed)
        capture(target, index);
}

static void frame_transform(void *data, struct ext_image_copy_capture_frame_v1 *frame,
        uint32_t transform) {
    (void)data; (void)frame; (void)transform;
}

static void frame_damage(void *data, struct ext_image_copy_capture_frame_v1 *frame,
        int32_t x, int32_t y, int32_t width, int32_t height) {
    (void)data; (void)frame; (void)x; (void)y; (void)width; (void)height;
}

static void frame_presentation_time(void *data, struct ext_image_copy_capture_frame_v1 *frame,
        uint32_t sec_hi, uint32_t sec_lo, uint32_t nsec) {
    (void)data; (void)frame; (void)sec_hi; (void)sec_lo; (void)nsec;
}

static const struct ext_image_copy_capture_frame_v1_listener frame_listener = {
    .transform = frame_transform,
    .damage = frame_damage,
    .presentation_time = frame_presentation_time,
    .ready = frame_ready,
    .failed = frame_failed,
};

static void capture(struct target *target, int index) {
    target->frame = ext_image_copy_capture_session_v1_create_frame(target->session);
    ext_image_copy_capture_frame_v1_add_listener(target->frame, &frame_listener,
        (void *)(intptr_t)index);
    ext_image_copy_capture_frame_v1_attach_buffer(target->frame, target->buffer);
    target->issued = seconds();
    ext_image_copy_capture_frame_v1_capture(target->frame);
    wl_display_flush(display);
}

static void session_buffer_size(void *data, struct ext_image_copy_capture_session_v1 *session,
        uint32_t width, uint32_t height) {
    struct target *target = data;
    (void)session;
    target->width = (int)width;
    target->height = (int)height;
}

static void session_shm_format(void *data, struct ext_image_copy_capture_session_v1 *session,
        uint32_t format) {
    (void)data; (void)session;
    if (format == WL_SHM_FORMAT_ARGB8888) shm_format = DRM_FORMAT_ARGB8888;
}

static void session_dmabuf_device(void *data, struct ext_image_copy_capture_session_v1 *session,
        struct wl_array *device) {
    (void)data; (void)session; (void)device;
}

static void session_dmabuf_format(void *data, struct ext_image_copy_capture_session_v1 *session,
        uint32_t format, struct wl_array *modifiers) {
    struct target *target = data;
    uint64_t *modifier;
    (void)session;
    if (format != DRM_FORMAT_ARGB8888) return;
    target->format = format;
    wl_array_for_each(modifier, modifiers)
        if (*modifier == DRM_FORMAT_MOD_LINEAR) target->linear_modifier = DRM_FORMAT_MOD_LINEAR;
    if (!target->linear_modifier) target->linear_modifier = DRM_FORMAT_MOD_LINEAR;
}

static void session_done(void *data, struct ext_image_copy_capture_session_v1 *session) {
    struct target *target = data;
    (void)session;
    if (target->started) return;
    if (target->width <= 0 || target->height <= 0)
        fail("the session reported no buffer size for %s", target->title);
    if (udmabuf_mode && !target->format) fail("the session advertised no ARGB8888 dma-buf");
    if (!target->format) target->format = DRM_FORMAT_ARGB8888;
    allocate(target);
    target->started = true;
    int index = (int)(target - targets);
    printf("target %d %dx%d %s %s\n", index, target->width, target->height,
        udmabuf_mode ? "dmabuf" : "shm", target->title);
    fflush(stdout);
    capture(target, index);
}

static void session_stopped(void *data, struct ext_image_copy_capture_session_v1 *session) {
    struct target *target = data;
    (void)session;
    target->closed = true;
    target->stopped = true;
    printf("stopped %d\n", (int)(target - targets));
    fflush(stdout);
}

static const struct ext_image_copy_capture_session_v1_listener session_listener = {
    .buffer_size = session_buffer_size,
    .shm_format = session_shm_format,
    .dmabuf_device = session_dmabuf_device,
    .dmabuf_format = session_dmabuf_format,
    .done = session_done,
    .stopped = session_stopped,
};

static void handle_closed(void *data, struct ext_foreign_toplevel_handle_v1 *handle) {
    struct target *target = data;
    (void)handle;
    target->closed = true;
}

static void handle_done(void *data, struct ext_foreign_toplevel_handle_v1 *handle) {
    struct target *target = data;
    (void)handle;
    target->described = true;
}

static void handle_title(void *data, struct ext_foreign_toplevel_handle_v1 *handle,
        const char *title) {
    struct target *target = data;
    (void)handle;
    snprintf(target->title, sizeof(target->title), "%s", title ? title : "");
}

static void handle_app_id(void *data, struct ext_foreign_toplevel_handle_v1 *handle,
        const char *app_id) {
    struct target *target = data;
    (void)handle;
    snprintf(target->app_id, sizeof(target->app_id), "%s", app_id ? app_id : "");
}

static void handle_identifier(void *data, struct ext_foreign_toplevel_handle_v1 *handle,
        const char *identifier) {
    (void)data; (void)handle; (void)identifier;
}

static const struct ext_foreign_toplevel_handle_v1_listener handle_listener = {
    .closed = handle_closed,
    .done = handle_done,
    .title = handle_title,
    .app_id = handle_app_id,
    .identifier = handle_identifier,
};

static void toplevel(void *data, struct ext_foreign_toplevel_list_v1 *list,
        struct ext_foreign_toplevel_handle_v1 *handle) {
    (void)data; (void)list;
    if (seen >= TARGETS_MAX) {
        ext_foreign_toplevel_handle_v1_destroy(handle);
        return;
    }
    struct target *target = &targets[seen];
    memset(target, 0, sizeof(*target));
    target->handle = handle;
    ext_foreign_toplevel_handle_v1_add_listener(handle, &handle_listener, target);
    seen++;
}

static void toplevel_finished(void *data, struct ext_foreign_toplevel_list_v1 *list) {
    (void)data; (void)list;
}

static const struct ext_foreign_toplevel_list_v1_listener toplevel_listener = {
    .toplevel = toplevel,
    .finished = toplevel_finished,
};

static void dmabuf_format(void *data, struct zwp_linux_dmabuf_v1 *manager, uint32_t format) {
    (void)data; (void)manager; (void)format;
}

static void dmabuf_modifier(void *data, struct zwp_linux_dmabuf_v1 *manager, uint32_t format,
        uint32_t hi, uint32_t lo) {
    (void)data; (void)manager; (void)format; (void)hi; (void)lo;
}

static const struct zwp_linux_dmabuf_v1_listener dmabuf_listener = {
    .format = dmabuf_format,
    .modifier = dmabuf_modifier,
};

static void output_geometry(void *data, struct wl_output *handle, int32_t x, int32_t y,
        int32_t physical_width, int32_t physical_height, int32_t subpixel, const char *make,
        const char *model, int32_t transform) {
    (void)data; (void)handle; (void)x; (void)y; (void)physical_width; (void)physical_height;
    (void)subpixel; (void)make; (void)model; (void)transform;
}

static void output_mode_event(void *data, struct wl_output *handle, uint32_t flags,
        int32_t width, int32_t height, int32_t refresh) {
    (void)data; (void)handle; (void)flags; (void)width; (void)height; (void)refresh;
}

static const struct wl_output_listener output_listener = {
    .geometry = output_geometry,
    .mode = output_mode_event,
};

static void global(void *data, struct wl_registry *handle, uint32_t name,
        const char *interface, uint32_t version) {
    (void)data; (void)version;
    if (!strcmp(interface, "wl_shm"))
        shm = wl_registry_bind(handle, name, &wl_shm_interface, 1);
    else if (!strcmp(interface, "zwp_linux_dmabuf_v1"))
        dmabuf_manager = wl_registry_bind(handle, name, &zwp_linux_dmabuf_v1_interface, 3);
    else if (!strcmp(interface, "ext_foreign_toplevel_list_v1"))
        toplevels = wl_registry_bind(handle, name, &ext_foreign_toplevel_list_v1_interface, 1);
    else if (!strcmp(interface, "ext_foreign_toplevel_image_capture_source_manager_v1"))
        source_manager = wl_registry_bind(handle, name,
            &ext_foreign_toplevel_image_capture_source_manager_v1_interface, 1);
    else if (!strcmp(interface, "ext_output_image_capture_source_manager_v1"))
        output_sources = wl_registry_bind(handle, name,
            &ext_output_image_capture_source_manager_v1_interface, 1);
    else if (!strcmp(interface, "wl_output") && !output) {
        output = wl_registry_bind(handle, name, &wl_output_interface, 1);
        wl_output_add_listener(output, &output_listener, NULL);
    } else if (!strcmp(interface, "ext_image_copy_capture_manager_v1"))
        capture_manager = wl_registry_bind(handle, name,
            &ext_image_copy_capture_manager_v1_interface, 1);
}

static void global_remove(void *data, struct wl_registry *handle, uint32_t name) {
    (void)data; (void)handle; (void)name;
}

static const struct wl_registry_listener registry_listener = {
    .global = global,
    .global_remove = global_remove,
};

static void terminate(int number) {
    (void)number;
    stopping = 1;
}

static int live_targets(void) {
    int live = 0;
    for (int i = 0; i < seen; i++)
        if (targets[i].described && !targets[i].closed &&
                strcmp(targets[i].app_id, wanted_app_id) == 0) live++;
    return live;
}

static int select_targets(int schedule[]) {
    int chosen = 0;
    for (int i = 0; i < seen && chosen < wanted; i++) {
        struct target *target = &targets[i];
        if (!target->described || target->closed ||
                strcmp(target->app_id, wanted_app_id) != 0) continue;
        schedule[chosen++] = i;
    }
    return chosen;
}

static bool finished(int schedule[], int count) {
    for (int i = 0; i < count; i++) {
        struct target *target = &targets[schedule[i]];
        if (target->captured + target->failed < frames_per_target) return false;
    }
    return true;
}

static bool pump(void) {
    while (wl_display_prepare_read(display) != 0)
        if (wl_display_dispatch_pending(display) < 0) return false;
    if (wl_display_flush(display) < 0 && errno != EAGAIN) {
        wl_display_cancel_read(display);
        return false;
    }
    struct pollfd fd = { .fd = wl_display_get_fd(display), .events = POLLIN };
    if (poll(&fd, 1, 100) > 0) {
        if (wl_display_read_events(display) < 0) return false;
    } else {
        wl_display_cancel_read(display);
    }
    return wl_display_dispatch_pending(display) >= 0;
}

int main(int arguments, char **argv) {
    int seconds_budget = 30;
    for (int i = 1; i < arguments; i++) {
        if (!strcmp(argv[i], "--windows") && i + 1 < arguments) wanted = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--frames") && i + 1 < arguments)
            frames_per_target = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--delay-ms") && i + 1 < arguments) delay_ms = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--seconds") && i + 1 < arguments)
            seconds_budget = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--app-id") && i + 1 < arguments) wanted_app_id = argv[++i];
        else if (!strcmp(argv[i], "--dmabuf")) udmabuf_mode = 1;
        else if (!strcmp(argv[i], "--output")) output_mode = 1;
        else fail("unknown argument %s", argv[i]);
    }
    if (wanted < 1) wanted = 1;
    if (wanted > TARGETS_MAX) wanted = TARGETS_MAX;
    signal(SIGALRM, terminate);
    signal(SIGTERM, terminate);
    display = wl_display_connect(NULL);
    if (!display) fail("cannot connect to the compositor");
    registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &registry_listener, NULL);
    if (wl_display_roundtrip(display) < 0) fail("the registry roundtrip failed");
    if (!capture_manager) fail("the compositor does not advertise ext_image_copy_capture_v1");
    if (!shm) fail("the compositor does not advertise wl_shm");
    if (output_mode) {
        if (!output_sources)
            fail("the compositor does not advertise an output capture source");
        if (!output) fail("the compositor announced no output");
    } else {
        if (!source_manager)
            fail("the compositor does not advertise a foreign toplevel capture source");
        if (!toplevels) fail("the compositor does not advertise ext_foreign_toplevel_list_v1");
    }
    if (dmabuf_manager) zwp_linux_dmabuf_v1_add_listener(dmabuf_manager, &dmabuf_listener, NULL);
    if (udmabuf_mode && !dmabuf_manager) fail("dma-buf capture needs zwp_linux_dmabuf_v1");
    if (toplevels) ext_foreign_toplevel_list_v1_add_listener(toplevels, &toplevel_listener, NULL);
    int schedule[TARGETS_MAX];
    int count;
    if (output_mode) {
        struct target *target = &targets[0];
        target->described = true;
        snprintf(target->title, sizeof(target->title), "output");
        target->source = ext_output_image_capture_source_manager_v1_create_source(
            output_sources, output);
        if (!target->source) fail("the compositor refused an output capture source");
        target->session = ext_image_copy_capture_manager_v1_create_session(capture_manager,
            target->source, 0);
        if (!target->session) fail("the compositor refused a capture session");
        ext_image_copy_capture_session_v1_add_listener(target->session, &session_listener,
            target);
        seen = 1;
        schedule[0] = 0;
        count = 1;
    } else {
        for (int round = 0; round < 200 && live_targets() < wanted; round++) {
            if (wl_display_roundtrip(display) < 0) break;
            if (round > 4) usleep(20000);
        }
        count = select_targets(schedule);
        if (count < wanted) fail("only %d of %d capture targets were announced", count, wanted);
        for (int i = 0; i < count; i++) {
            struct target *target = &targets[schedule[i]];
            target->source = ext_foreign_toplevel_image_capture_source_manager_v1_create_source(
                source_manager, target->handle);
            if (!target->source) fail("the compositor refused a capture source");
            target->session = ext_image_copy_capture_manager_v1_create_session(capture_manager,
                target->source, 0);
            if (!target->session) fail("the compositor refused a capture session");
            ext_image_copy_capture_session_v1_add_listener(target->session, &session_listener,
                target);
        }
    }
    alarm((unsigned)seconds_budget);
    while (!stopping && !finished(schedule, count))
        if (!pump()) break;
    int captured = 0, failures = 0;
    for (int i = 0; i < count; i++) {
        captured += targets[schedule[i]].captured;
        failures += targets[schedule[i]].failed;
    }
    printf("end %d %d %d\n", count, captured, failures);
    fflush(stdout);
    return 0;
}
