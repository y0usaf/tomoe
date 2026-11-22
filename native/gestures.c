#include "internal.h"
#include "pointer-gestures-unstable-v1-protocol.h"

struct gesture {
    struct wl_list link;
    struct wl_resource *resource;
    uint32_t kind;
    bool active;
};

void pointer_gesture_end(struct tomoe *s, struct input_device *device, uint32_t kind,
        uint32_t time, int cancelled) {
    if (s->gesture_kind != kind || s->gesture_device != device) return;
    uint32_t serial = wl_display_next_serial(s->display);
    struct gesture *gesture;
    wl_list_for_each(gesture, &s->gestures, link) {
        if (!gesture->active) continue;
        if (kind == GESTURE_SWIPE)
            zwp_pointer_gesture_swipe_v1_send_end(gesture->resource, serial, time, cancelled);
        else if (kind == GESTURE_PINCH)
            zwp_pointer_gesture_pinch_v1_send_end(gesture->resource, serial, time, cancelled);
        else
            zwp_pointer_gesture_hold_v1_send_end(gesture->resource, serial, time, cancelled);
        gesture->active = false;
    }
    s->gesture_kind = 0;
    s->gesture_device = NULL;
}

void pointer_gesture_cancel(struct tomoe *s) {
    if (!s->gesture_kind) return;
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    pointer_gesture_end(s, s->gesture_device, s->gesture_kind,
        (uint32_t)((uint64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000), 1);
}

void pointer_gesture_begin(struct tomoe *s, struct input_device *device, uint32_t kind,
        uint32_t time, uint32_t fingers) {
    pointer_gesture_cancel(s);
    struct seat_pointer_state *state = &s->seat->pointer_state;
    if (!state->focused_surface || !state->focused_client || s->grab_mode || s->screenshot ||
            state->grab != &state->default_grab) return;
    if (kind < GESTURE_SWIPE || kind > GESTURE_HOLD) return;
    idle_notify_activity(s);
    s->gesture_kind = kind;
    s->gesture_device = device;
    double ratio = physical_hit_ratio(s, s->pointer_x, s->pointer_y);
    struct output *output = output_at_physical(s, s->pointer_x, s->pointer_y);
    s->gesture_scale = ratio > 0 ?
        (output ? snapped_scale(output->screen->scale) : reference_scale(s)) / ratio : 1;
    uint32_t serial = wl_display_next_serial(s->display);
    struct gesture *gesture;
    wl_list_for_each(gesture, &s->gestures, link) {
        if (gesture->kind != kind || wl_resource_get_client(gesture->resource) !=
                state->focused_client->client) continue;
        gesture->active = true;
        if (kind == GESTURE_SWIPE)
            zwp_pointer_gesture_swipe_v1_send_begin(gesture->resource, serial, time,
                state->focused_surface->resource, fingers);
        else if (kind == GESTURE_PINCH)
            zwp_pointer_gesture_pinch_v1_send_begin(gesture->resource, serial, time,
                state->focused_surface->resource, fingers);
        else
            zwp_pointer_gesture_hold_v1_send_begin(gesture->resource, serial, time,
                state->focused_surface->resource, fingers);
    }
}

void pointer_gesture_update(struct tomoe *s, struct input_device *device, uint32_t kind,
        uint32_t time, double dx, double dy, double scale, double rotation) {
    if (kind != s->gesture_kind || device != s->gesture_device) return;
    idle_notify_activity(s);
    struct gesture *gesture;
    wl_list_for_each(gesture, &s->gestures, link) {
        if (!gesture->active) continue;
        if (kind == GESTURE_SWIPE)
            zwp_pointer_gesture_swipe_v1_send_update(gesture->resource, time,
                wl_fixed_from_double(dx * s->gesture_scale),
                wl_fixed_from_double(dy * s->gesture_scale));
        else if (kind == GESTURE_PINCH)
            zwp_pointer_gesture_pinch_v1_send_update(gesture->resource, time,
                wl_fixed_from_double(dx * s->gesture_scale),
                wl_fixed_from_double(dy * s->gesture_scale), wl_fixed_from_double(scale),
                wl_fixed_from_double(rotation));
    }
}

static void destroy(struct wl_client *client, struct wl_resource *resource) {
    wl_resource_destroy(resource);
}

static void resource_destroy(struct wl_resource *resource) {
    struct gesture *gesture = wl_resource_get_user_data(resource);
    wl_list_remove(&gesture->link);
    free(gesture);
}

static const struct zwp_pointer_gesture_swipe_v1_interface swipe_impl = { .destroy = destroy };
static const struct zwp_pointer_gesture_pinch_v1_interface pinch_impl = { .destroy = destroy };
static const struct zwp_pointer_gesture_hold_v1_interface hold_impl = { .destroy = destroy };

static void create(struct wl_client *client, struct wl_resource *manager, uint32_t id,
        struct wl_resource *pointer, uint32_t kind) {
    struct tomoe *s = wl_resource_get_user_data(manager);
    struct seat_client *seat_client = wl_resource_get_user_data(pointer);
    const struct wl_interface *interface = kind == GESTURE_SWIPE ?
        &zwp_pointer_gesture_swipe_v1_interface : kind == GESTURE_PINCH ?
        &zwp_pointer_gesture_pinch_v1_interface : &zwp_pointer_gesture_hold_v1_interface;
    const void *impl = kind == GESTURE_SWIPE ? (const void *)&swipe_impl :
        kind == GESTURE_PINCH ? (const void *)&pinch_impl : (const void *)&hold_impl;
    struct gesture *gesture = calloc(1, sizeof(*gesture));
    struct wl_resource *resource = gesture ? wl_resource_create(client, interface,
        wl_resource_get_version(manager), id) : NULL;
    if (!resource) {
        free(gesture);
        wl_client_post_no_memory(client);
        return;
    }
    gesture->resource = resource;
    gesture->kind = seat_client && seat_client->seat == s->seat ? kind : 0;
    wl_resource_set_implementation(resource, impl, gesture, resource_destroy);
    wl_list_insert(&s->gestures, &gesture->link);
}

static void swipe(struct wl_client *client, struct wl_resource *manager, uint32_t id,
        struct wl_resource *pointer) { create(client, manager, id, pointer, GESTURE_SWIPE); }
static void pinch(struct wl_client *client, struct wl_resource *manager, uint32_t id,
        struct wl_resource *pointer) { create(client, manager, id, pointer, GESTURE_PINCH); }
static void hold(struct wl_client *client, struct wl_resource *manager, uint32_t id,
        struct wl_resource *pointer) { create(client, manager, id, pointer, GESTURE_HOLD); }

static const struct zwp_pointer_gestures_v1_interface manager_impl = {
    .get_swipe_gesture = swipe,
    .get_pinch_gesture = pinch,
    .release = destroy,
    .get_hold_gesture = hold,
};

static void bind_manager(struct wl_client *client, void *data, uint32_t version, uint32_t id) {
    struct wl_resource *resource = wl_resource_create(client, &zwp_pointer_gestures_v1_interface,
        version, id);
    if (!resource) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(resource, &manager_impl, data, NULL);
}

bool gestures_listen(struct tomoe *s) {
    wl_list_init(&s->gestures);
    return wl_global_create(s->display, &zwp_pointer_gestures_v1_interface, 3, s,
        bind_manager) != NULL;
}
