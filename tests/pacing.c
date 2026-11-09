#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>
#include "fifo-v1-client-protocol.h"
#include "presentation-time-client-protocol.h"
#include "wlr-layer-shell-unstable-v1-client-protocol.h"

#include "wlr-output-power-management-unstable-v1-client-protocol.h"
static struct wl_display *display;
static struct wl_compositor *compositor;
static struct wl_subcompositor *subcompositor;
static struct wl_shm *shm;
static struct wp_fifo_manager_v1 *manager;
static struct wp_presentation *presentation;
static struct zwlr_layer_shell_v1 *shell;
static bool configured;
static unsigned presented, discarded;
static struct wl_output *output;
static struct zwlr_output_power_manager_v1 *powers;
static uint64_t last_time;

static void global(void *data, struct wl_registry *registry, uint32_t name,
        const char *interface, uint32_t version) {
    if (!strcmp(interface, "wl_compositor"))
        compositor = wl_registry_bind(registry, name, &wl_compositor_interface, 4);
    else if (!strcmp(interface, "wl_subcompositor"))
        subcompositor = wl_registry_bind(registry, name, &wl_subcompositor_interface, 1);
    else if (!strcmp(interface, "wl_shm"))
        shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
    else if (!strcmp(interface, "wp_fifo_manager_v1"))
        manager = wl_registry_bind(registry, name, &wp_fifo_manager_v1_interface, 1);
    else if (!strcmp(interface, "wp_presentation"))
        presentation = wl_registry_bind(registry, name, &wp_presentation_interface, 1);
    else if (!strcmp(interface, "wl_output") && !output)
        output = wl_registry_bind(registry, name, &wl_output_interface, 1);
    else if (!strcmp(interface, "zwlr_output_power_manager_v1"))
        powers = wl_registry_bind(registry, name, &zwlr_output_power_manager_v1_interface, 1);
    else if (!strcmp(interface, "zwlr_layer_shell_v1"))
        shell = wl_registry_bind(registry, name, &zwlr_layer_shell_v1_interface, 1);
}

static void global_remove(void *data, struct wl_registry *registry, uint32_t name) {}
static const struct wl_registry_listener registry_listener = { global, global_remove };

static void configure(void *data, struct zwlr_layer_surface_v1 *layer, uint32_t serial,
        uint32_t width, uint32_t height) {
    zwlr_layer_surface_v1_ack_configure(layer, serial);
    configured = true;
}
static void closed(void *data, struct zwlr_layer_surface_v1 *layer) { abort(); }
static const struct zwlr_layer_surface_v1_listener layer_listener = { configure, closed };

static void sync_output(void *data, struct wp_presentation_feedback *feedback,
        struct wl_output *output) {}
static void shown(void *data, struct wp_presentation_feedback *feedback, uint32_t hi,
        uint32_t lo, uint32_t nsec, uint32_t refresh, uint32_t seq_hi, uint32_t seq_lo,
        uint32_t flags) {
    uint64_t when = (((uint64_t)hi << 32) | lo) * 1000000000 + nsec;
    if (last_time) assert(when > last_time);
    last_time = when;
    presented++;
    wp_presentation_feedback_destroy(feedback);
}
static void dropped(void *data, struct wp_presentation_feedback *feedback) {
    discarded++;
    wp_presentation_feedback_destroy(feedback);
}
static const struct wp_presentation_feedback_listener feedback_listener = {
    sync_output, shown, dropped,
};

static void feedback(struct wl_surface *surface) {
    struct wp_presentation_feedback *f = wp_presentation_feedback(presentation, surface);
    wp_presentation_feedback_add_listener(f, &feedback_listener, NULL);
}

static void pump(unsigned count) {
    struct timespec start, now;
    clock_gettime(CLOCK_MONOTONIC, &start);
    while (presented + discarded < count) {
        while (wl_display_prepare_read(display) != 0)
            assert(wl_display_dispatch_pending(display) >= 0);
        assert(wl_display_flush(display) >= 0 || errno == EAGAIN);
        struct pollfd fd = { wl_display_get_fd(display), POLLIN, 0 };
        int ready = poll(&fd, 1, 100);
        if (ready > 0) assert(wl_display_read_events(display) >= 0);
        else wl_display_cancel_read(display);
        assert(wl_display_dispatch_pending(display) >= 0);
        clock_gettime(CLOCK_MONOTONIC, &now);
        assert(now.tv_sec - start.tv_sec < 5);
    }
}

static struct wl_buffer *make_buffer(void) {
    int fd = memfd_create("fifo-client", MFD_CLOEXEC);
    assert(fd >= 0 && ftruncate(fd, 64 * 64 * 4) == 0);
    uint32_t *pixels = mmap(NULL, 64 * 64 * 4, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    assert(pixels != MAP_FAILED);
    for (unsigned i = 0; i < 64 * 64; i++) pixels[i] = 0xff4080c0;
    struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, 64 * 64 * 4);
    struct wl_buffer *buffer = wl_shm_pool_create_buffer(pool, 0, 64, 64, 64 * 4,
        WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    munmap(pixels, 64 * 64 * 4);
    close(fd);
    return buffer;
}

static void commit(struct wl_surface *surface, struct wl_buffer *buffer) {
    wl_surface_attach(surface, buffer, 0, 0);
    wl_surface_damage_buffer(surface, 0, 0, 64, 64);
    feedback(surface);
    wl_surface_commit(surface);
}

static void protocol_error(const struct wl_interface *expected, uint32_t code) {
    assert(wl_display_roundtrip(display) < 0);
    assert(wl_display_get_error(display) == EPROTO);
    const struct wl_interface *actual;
    uint32_t id;
    assert(wl_display_get_protocol_error(display, &actual, &id) == code);
    assert(!strcmp(actual->name, expected->name));
}

int main(int argc, char **argv) {
    assert(argc == 2);
    display = wl_display_connect(NULL);
    assert(display);
    struct wl_registry *registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &registry_listener, NULL);
    assert(wl_display_roundtrip(display) >= 0);
    assert(compositor && subcompositor && shm && manager && presentation && shell);
    struct wl_surface *surface = wl_compositor_create_surface(compositor);
    struct wp_fifo_v1 *fifo = wp_fifo_manager_v1_get_fifo(manager, surface);
    if (!strcmp(argv[1], "duplicate")) {
        wp_fifo_manager_v1_get_fifo(manager, surface);
        protocol_error(&wp_fifo_manager_v1_interface, WP_FIFO_MANAGER_V1_ERROR_ALREADY_EXISTS);
    } else if (!strcmp(argv[1], "dead-set") || !strcmp(argv[1], "dead-wait")) {
        wl_surface_destroy(surface);
        if (!strcmp(argv[1], "dead-set")) wp_fifo_v1_set_barrier(fifo);
        else wp_fifo_v1_wait_barrier(fifo);
        protocol_error(&wp_fifo_v1_interface, WP_FIFO_V1_ERROR_SURFACE_DESTROYED);
    } else {
        struct zwlr_layer_surface_v1 *layer = zwlr_layer_shell_v1_get_layer_surface(shell,
            surface, NULL, ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY, "fifo-exercise");
        zwlr_layer_surface_v1_add_listener(layer, &layer_listener, NULL);
        zwlr_layer_surface_v1_set_size(layer, 64, 64);
        wl_surface_commit(surface);
        while (!configured) assert(wl_display_dispatch(display) >= 0);
        struct wl_buffer *buffer = make_buffer();
        commit(surface, buffer);
        pump(1);
        assert(presented == 1 && discarded == 0);
        presented = discarded = 0;
        last_time = 0;
        if (!strcmp(argv[1], "sync")) {
            struct wl_surface *child = wl_compositor_create_surface(compositor);
            struct wl_subsurface *sub = wl_subcompositor_get_subsurface(subcompositor, child, surface);
            struct wp_fifo_v1 *child_fifo = wp_fifo_manager_v1_get_fifo(manager, child);
            wp_fifo_v1_set_barrier(child_fifo);
            commit(child, buffer);
            wp_fifo_v1_wait_barrier(child_fifo);
            commit(child, buffer);
            wl_surface_commit(surface);
            pump(2);
            assert(presented == 1 && discarded == 1);
            wl_subsurface_destroy(sub);
            wp_fifo_v1_destroy(child_fifo);
            wl_surface_destroy(child);
        } else {
            bool paced = strcmp(argv[1], "unpaced") != 0;
            bool destroy = !strcmp(argv[1], "destroy");
            bool mixed = !strcmp(argv[1], "mixed");
            bool power = !strcmp(argv[1], "power");
            if (!destroy) {
                wp_fifo_manager_v1_destroy(manager);
                manager = NULL;
            }
            for (unsigned i = 0; i < 4; i++) {
                if (paced && (!mixed || i < 2)) {
                    wp_fifo_v1_wait_barrier(fifo);
                    wp_fifo_v1_set_barrier(fifo);
                }
                if (destroy && i == 3) {
                    wp_fifo_v1_destroy(fifo);
                    fifo = wp_fifo_manager_v1_get_fifo(manager, surface);
                }
                commit(surface, buffer);
            }
            if (power) {
                assert(output && powers);
                struct zwlr_output_power_v1 *p =
                    zwlr_output_power_manager_v1_get_output_power(powers, output);
                zwlr_output_power_v1_set_mode(p, ZWLR_OUTPUT_POWER_V1_MODE_OFF);
                assert(wl_display_roundtrip(display) >= 0);
                zwlr_output_power_v1_set_mode(p, ZWLR_OUTPUT_POWER_V1_MODE_ON);
                zwlr_output_power_v1_destroy(p);
            }
            pump(4);
            assert(presented == (power ? 1u : mixed ? 2u : paced ? 4u : 1u));
            assert(discarded == (power ? 3u : mixed ? 2u : paced ? 0u : 3u));
        }
        printf("%s: presented=%u discarded=%u\n", argv[1], presented, discarded);
        wp_fifo_v1_destroy(fifo);
        zwlr_layer_surface_v1_destroy(layer);
        wl_surface_destroy(surface);
        wl_buffer_destroy(buffer);
        assert(wl_display_roundtrip(display) >= 0);
    }
    printf("%s: pass\n", argv[1]);
    wl_display_disconnect(display);
    return 0;
}
