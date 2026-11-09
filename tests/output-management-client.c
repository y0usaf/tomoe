#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wayland-client.h>
#include "wlr-output-management-unstable-v1-client-protocol.h"

struct head {
    struct zwlr_output_head_v1 *proxy;
    struct zwlr_output_mode_v1 *mode;
    char name[129];
    unsigned finished_modes;
    bool finished;
};
static struct wl_display *display;
static struct zwlr_output_manager_v1 *manager;
static struct head heads[16];
static size_t count;
static uint32_t serial;
static int result;
static void mode_size(void *data, struct zwlr_output_mode_v1 *mode, int32_t w, int32_t h) {}
static void mode_refresh(void *data, struct zwlr_output_mode_v1 *mode, int32_t refresh) {}
static void mode_preferred(void *data, struct zwlr_output_mode_v1 *mode) {}
static void mode_finished(void *data, struct zwlr_output_mode_v1 *mode) {
    struct head *h = data;
    h->finished_modes++;
    zwlr_output_mode_v1_release(mode);
}
static const struct zwlr_output_mode_v1_listener mode_listener = {
    .size = mode_size, .refresh = mode_refresh, .preferred = mode_preferred, .finished = mode_finished,
};
static void head_name(void *data, struct zwlr_output_head_v1 *head, const char *name) {
    struct head *h = data;
    snprintf(h->name, sizeof(h->name), "%s", name);
}
static void head_string(void *data, struct zwlr_output_head_v1 *head, const char *value) {}
static void head_pair(void *data, struct zwlr_output_head_v1 *head, int32_t x, int32_t y) {}
static void head_int(void *data, struct zwlr_output_head_v1 *head, int32_t value) {}
static void head_uint(void *data, struct zwlr_output_head_v1 *head, uint32_t value) {}
static void head_mode(void *data, struct zwlr_output_head_v1 *head, struct zwlr_output_mode_v1 *mode) {
    struct head *h = data;
    if (!h->mode) h->mode = mode;
    zwlr_output_mode_v1_add_listener(mode, &mode_listener, h);
}
static void head_current(void *data, struct zwlr_output_head_v1 *head, struct zwlr_output_mode_v1 *mode) {}
static void head_finished(void *data, struct zwlr_output_head_v1 *head) {
    struct head *h = data;
    assert(h->finished_modes > 0);
    h->finished = true;
    zwlr_output_head_v1_release(head);
}
static const struct zwlr_output_head_v1_listener head_listener = {
    .name = head_name, .description = head_string, .physical_size = head_pair, .mode = head_mode,
    .enabled = head_int, .current_mode = head_current, .position = head_pair, .transform = head_int,
    .scale = head_int, .finished = head_finished, .make = head_string, .model = head_string,
    .serial_number = head_string, .adaptive_sync = head_uint,
};
static void manager_head(void *data, struct zwlr_output_manager_v1 *manager, struct zwlr_output_head_v1 *head) {
    assert(count < 16);
    heads[count].proxy = head;
    zwlr_output_head_v1_add_listener(head, &head_listener, &heads[count++]);
}
static void manager_done(void *data, struct zwlr_output_manager_v1 *manager, uint32_t value) { serial = value; }
static void manager_finished(void *data, struct zwlr_output_manager_v1 *manager) {}
static const struct zwlr_output_manager_v1_listener manager_listener = {
    .head = manager_head, .done = manager_done, .finished = manager_finished,
};
static void global(void *data, struct wl_registry *registry, uint32_t id, const char *interface, uint32_t version) {
    if (!strcmp(interface, zwlr_output_manager_v1_interface.name)) {
        manager = wl_registry_bind(registry, id, &zwlr_output_manager_v1_interface, 4);
        zwlr_output_manager_v1_add_listener(manager, &manager_listener, NULL);
    }
}
static void global_remove(void *data, struct wl_registry *registry, uint32_t id) {}
static const struct wl_registry_listener registry_listener = { global, global_remove };
static void succeeded(void *data, struct zwlr_output_configuration_v1 *config) { result = 1; }
static void failed(void *data, struct zwlr_output_configuration_v1 *config) { result = 2; }
static void cancelled(void *data, struct zwlr_output_configuration_v1 *config) { result = 3; }
static const struct zwlr_output_configuration_v1_listener config_listener = { succeeded, failed, cancelled };
int main(int argc, char **argv) {
    assert(argc >= 2);
    display = wl_display_connect(NULL);
    assert(display);
    struct wl_registry *registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &registry_listener, NULL);
    assert(wl_display_roundtrip(display) >= 0);
    assert(wl_display_roundtrip(display) >= 0);
    assert(manager && serial && count);
    if (!strcmp(argv[1], "watch")) {
        assert(argc == 3);
        struct head *h = NULL;
        for (size_t i = 0; i < count; i++) if (!strcmp(heads[i].name, argv[2])) h = &heads[i];
        assert(h);
        puts("ready"); fflush(stdout);
        while (!h->finished) assert(wl_display_dispatch(display) >= 0);
        puts("head and modes finished");
        wl_display_disconnect(display);
        return 0;
    }
    bool stale = !strcmp(argv[1], "stale");
    bool cancel = !strcmp(argv[1], "cancel");
    bool omit = !strcmp(argv[1], "omit");
    bool duplicate = !strcmp(argv[1], "duplicate");
    bool twice = !strcmp(argv[1], "twice");
    bool property = !strcmp(argv[1], "property");
    bool invalid = !strcmp(argv[1], "invalid-scale");
    bool mode = !strcmp(argv[1], "mode");
    struct zwlr_output_configuration_v1 *config = zwlr_output_manager_v1_create_configuration(manager,
        stale ? serial - 1 : serial);
    zwlr_output_configuration_v1_add_listener(config, &config_listener, NULL);
    for (size_t i = 0; !omit && i < count; i++) {
        struct zwlr_output_configuration_head_v1 *h =
            zwlr_output_configuration_v1_enable_head(config, heads[i].proxy);
        if (mode && heads[i].mode) zwlr_output_configuration_head_v1_set_mode(h, heads[i].mode);
        if (i == 0 && duplicate) zwlr_output_configuration_v1_disable_head(config, heads[i].proxy);
        if (i == 0 && (property || invalid)) {
            zwlr_output_configuration_head_v1_set_scale(h, invalid ? 0 : wl_fixed_from_int(1));
            if (property) zwlr_output_configuration_head_v1_set_scale(h, wl_fixed_from_int(2));
        }
    }
    if (cancel) {
        uint32_t previous = serial;
        puts("ready"); fflush(stdout);
        while (serial == previous) assert(wl_display_dispatch(display) >= 0);
    }
    zwlr_output_configuration_v1_test(config);
    if (omit || duplicate || property || invalid) {
        assert(wl_display_roundtrip(display) == -1 && wl_display_get_error(display) == EPROTO);
        uint32_t code = wl_display_get_protocol_error(display, NULL, NULL);
        assert(code == (omit ? ZWLR_OUTPUT_CONFIGURATION_V1_ERROR_UNCONFIGURED_HEAD :
            duplicate ? ZWLR_OUTPUT_CONFIGURATION_V1_ERROR_ALREADY_CONFIGURED_HEAD :
            invalid ? ZWLR_OUTPUT_CONFIGURATION_HEAD_V1_ERROR_INVALID_SCALE :
            ZWLR_OUTPUT_CONFIGURATION_HEAD_V1_ERROR_ALREADY_SET));
    } else {
        while (!result) assert(wl_display_dispatch(display) >= 0);
        assert(result == (stale || cancel ? 3 : 1));
        if (twice) {
            zwlr_output_configuration_v1_apply(config);
            assert(wl_display_roundtrip(display) == -1 && wl_display_get_error(display) == EPROTO);
            assert(wl_display_get_protocol_error(display, NULL, NULL) ==
                ZWLR_OUTPUT_CONFIGURATION_V1_ERROR_ALREADY_USED);
        } else {
            zwlr_output_configuration_v1_destroy(config);
            assert(wl_display_roundtrip(display) >= 0);
        }
    }
    wl_display_disconnect(display);
    printf("%s passed\n", argv[1]);
    return 0;
}
