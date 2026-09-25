#include "internal.h"

struct virtual_pointer {
    struct wl_list link;
    struct tomoe *server;
    struct wlr_input_device *device;
    struct wlr_output *output;
    struct wl_listener destroy;
    struct wl_listener output_destroy;
};

static bool output_live(struct tomoe *s, struct wlr_output *output) {
    if (!output) return false;
    struct output *tracked;
    wl_list_for_each(tracked, &s->outputs, link) {
        if (tracked->wlr == output) return true;
    }
    return false;
}

static void output_destroy(struct wl_listener *listener, void *data) {
    struct virtual_pointer *pointer =
        wl_container_of(listener, pointer, output_destroy);
    pointer->output = NULL;
    wlr_cursor_map_input_to_output(pointer->server->cursor,
        pointer->device, NULL);
    detach(&pointer->output_destroy);
    wl_list_init(&pointer->output_destroy.link);
}

static void virtual_pointer_destroy(struct wl_listener *listener, void *data) {
    struct virtual_pointer *pointer =
        wl_container_of(listener, pointer, destroy);
    detach(&pointer->destroy);
    detach(&pointer->output_destroy);
    wl_list_remove(&pointer->link);
    free(pointer);
}

static void new_virtual_pointer(struct wl_listener *listener, void *data) {
    struct tomoe *s = wl_container_of(listener, s, new_virtual_pointer);
    struct wlr_virtual_pointer_v1_new_pointer_event *event = data;
    struct wlr_virtual_pointer_v1 *wlr_pointer = event->new_pointer;
    struct wlr_input_device *device = &wlr_pointer->pointer.base;
    struct virtual_pointer *pointer = calloc(1, sizeof(*pointer));
    if (!pointer) {
        fail(s, "virtual pointer allocation failed");
        return;
    }

    pointer->server = s;
    pointer->device = device;
    wl_list_insert(&s->virtual_pointers, &pointer->link);
    listen(&pointer->destroy, &device->events.destroy, virtual_pointer_destroy);

    wlr_cursor_attach_input_device(s->cursor, device);
    if (event->suggested_output && output_live(s, event->suggested_output)) {
        pointer->output = event->suggested_output;
        listen(&pointer->output_destroy, &pointer->output->events.destroy,
            output_destroy);
        wlr_cursor_map_input_to_output(s->cursor, device, pointer->output);
    }
}

bool virtual_pointers_listen(struct tomoe *s) {
    struct wlr_virtual_pointer_manager_v1 *manager =
        wlr_virtual_pointer_manager_v1_create(s->display);
    if (!manager) {
        fail(s, "virtual pointer manager creation failed");
        return false;
    }
    listen(&s->new_virtual_pointer, &manager->events.new_virtual_pointer,
        new_virtual_pointer);
    return true;
}

struct wlr_output *virtual_pointer_output(struct tomoe *s,
        struct wlr_input_device *device) {
    struct virtual_pointer *pointer;
    wl_list_for_each(pointer, &s->virtual_pointers, link) {
        if (pointer->device != device) continue;
        if (pointer->output && !output_live(s, pointer->output)) {
            output_destroy(&pointer->output_destroy, NULL);
        }
        return pointer->output;
    }
    return NULL;
}
