#include "internal.h"
#include <sys/random.h>
#include "ext-foreign-toplevel-list-v1-protocol.h"
#include "wlr-foreign-toplevel-management-unstable-v1-protocol.h"

#define FOREIGN_OUTPUTS 16

struct foreign {
    struct wl_list link;
    struct tomoe *server;
    uint32_t id;
    char identifier[33];
    struct wl_list wlr_handles, ext_handles;
};

static const struct zwlr_foreign_toplevel_handle_v1_interface wlr_handle_impl;
static const struct ext_foreign_toplevel_handle_v1_interface ext_handle_impl;

static void send_output(struct wl_resource *handle, struct screen *output, bool enter) {
    struct wl_resource *resource;
    wl_resource_for_each(resource, &output->resources) {
        if (wl_resource_get_client(resource) != wl_resource_get_client(handle)) continue;
        if (enter) zwlr_foreign_toplevel_handle_v1_send_output_enter(handle, resource);
        else zwlr_foreign_toplevel_handle_v1_send_output_leave(handle, resource);
    }
}

static void send_state(struct wl_resource *handle, uint32_t state) {
    struct wl_array array;
    wl_array_init(&array);
    for (uint32_t bit = 0; bit < 4; bit++) {
        if (!(state & (1u << bit))) continue;
        if (bit == ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_FULLSCREEN &&
                wl_resource_get_version(handle) < 2) continue;
        uint32_t *slot = wl_array_add(&array, sizeof(*slot));
        if (slot) *slot = bit;
    }
    zwlr_foreign_toplevel_handle_v1_send_state(handle, &array);
    wl_array_release(&array);
}

static void handle_resource_destroy(struct wl_resource *resource) {
    wl_list_remove(wl_resource_get_link(resource));
}

static void announce_wlr(struct foreign *f, struct wl_resource *manager, const char *title,
        const char *app_id, uint32_t state, struct screen *const *outputs, size_t output_count) {
    struct wl_resource *handle = wl_resource_create(wl_resource_get_client(manager),
        &zwlr_foreign_toplevel_handle_v1_interface, wl_resource_get_version(manager), 0);
    if (!handle) {
        wl_resource_post_no_memory(manager);
        return;
    }
    wl_resource_set_implementation(handle, &wlr_handle_impl, f, handle_resource_destroy);
    wl_list_insert(&f->wlr_handles, wl_resource_get_link(handle));
    zwlr_foreign_toplevel_manager_v1_send_toplevel(manager, handle);
    zwlr_foreign_toplevel_handle_v1_send_title(handle, title);
    zwlr_foreign_toplevel_handle_v1_send_app_id(handle, app_id);
    for (size_t i = 0; i < output_count; i++) send_output(handle, outputs[i], true);
    send_state(handle, state);
    zwlr_foreign_toplevel_handle_v1_send_done(handle);
}

static void announce_ext(struct foreign *f, struct wl_resource *list, const char *title,
        const char *app_id) {
    struct wl_resource *handle = wl_resource_create(wl_resource_get_client(list),
        &ext_foreign_toplevel_handle_v1_interface, wl_resource_get_version(list), 0);
    if (!handle) {
        wl_resource_post_no_memory(list);
        return;
    }
    wl_resource_set_implementation(handle, &ext_handle_impl, f, handle_resource_destroy);
    wl_list_insert(&f->ext_handles, wl_resource_get_link(handle));
    ext_foreign_toplevel_list_v1_send_toplevel(list, handle);
    ext_foreign_toplevel_handle_v1_send_identifier(handle, f->identifier);
    ext_foreign_toplevel_handle_v1_send_title(handle, title);
    ext_foreign_toplevel_handle_v1_send_app_id(handle, app_id);
    ext_foreign_toplevel_handle_v1_send_done(handle);
}

static struct foreign *foreign_find(struct tomoe *s, uint32_t id) {
    struct foreign *f;
    wl_list_for_each(f, &s->foreigns, link) if (f->id == id) return f;
    return NULL;
}

static struct foreign *foreign_create(struct tomoe *s, uint32_t id) {
    struct foreign *f = calloc(1, sizeof(*f));
    unsigned char bytes[16];
    if (!f || getrandom(bytes, sizeof(bytes), 0) != sizeof(bytes)) {
        free(f);
        fail(s, "foreign toplevel allocation failed");
        return NULL;
    }
    for (size_t i = 0; i < sizeof(bytes); i++) snprintf(f->identifier + 2 * i, 3, "%02x", bytes[i]);
    f->server = s;
    f->id = id;
    wl_list_init(&f->wlr_handles);
    wl_list_init(&f->ext_handles);
    wl_list_insert(s->foreigns.prev, &f->link);
    return f;
}

bool foreign_announce(struct tomoe *s, uint32_t id) {
    const char *title, *app_id;
    uint32_t state;
    struct screen *outputs[FOREIGN_OUTPUTS];
    size_t output_count;
    if (!window_foreign_fields(s, id, &title, &app_id, &state, outputs, FOREIGN_OUTPUTS,
            &output_count)) return false;
    struct foreign *f = foreign_find(s, id);
    if (!f && !(f = foreign_create(s, id))) return false;
    struct wl_resource *manager;
    wl_resource_for_each(manager, &s->foreign_managers)
        announce_wlr(f, manager, title, app_id, state, outputs, output_count);
    wl_resource_for_each(manager, &s->foreign_lists) announce_ext(f, manager, title, app_id);
    return true;
}

void foreign_fields(struct tomoe *s, uint32_t id, const char *title, const char *app_id,
        uint32_t state) {
    struct foreign *f = foreign_find(s, id);
    if (!f) return;
    struct wl_resource *handle;
    wl_resource_for_each(handle, &f->wlr_handles) {
        zwlr_foreign_toplevel_handle_v1_send_title(handle, title);
        zwlr_foreign_toplevel_handle_v1_send_app_id(handle, app_id);
        send_state(handle, state);
        zwlr_foreign_toplevel_handle_v1_send_done(handle);
    }
    wl_resource_for_each(handle, &f->ext_handles) {
        ext_foreign_toplevel_handle_v1_send_title(handle, title);
        ext_foreign_toplevel_handle_v1_send_app_id(handle, app_id);
        ext_foreign_toplevel_handle_v1_send_done(handle);
    }
}

void foreign_output(struct tomoe *s, uint32_t id, struct screen *output, bool enter) {
    struct foreign *f = foreign_find(s, id);
    if (!f || !output) return;
    struct wl_resource *handle;
    wl_resource_for_each(handle, &f->wlr_handles) {
        send_output(handle, output, enter);
        zwlr_foreign_toplevel_handle_v1_send_done(handle);
    }
}

void foreign_forget(struct tomoe *s, uint32_t id) {
    struct foreign *f = foreign_find(s, id);
    if (!f) return;
    struct wl_resource *handle, *next;
    wl_resource_for_each_safe(handle, next, &f->wlr_handles) {
        zwlr_foreign_toplevel_handle_v1_send_closed(handle);
        wl_resource_set_user_data(handle, NULL);
        wl_list_remove(wl_resource_get_link(handle));
        wl_list_init(wl_resource_get_link(handle));
    }
    wl_resource_for_each_safe(handle, next, &f->ext_handles) {
        ext_foreign_toplevel_handle_v1_send_closed(handle);
        wl_resource_set_user_data(handle, NULL);
        wl_list_remove(wl_resource_get_link(handle));
        wl_list_init(wl_resource_get_link(handle));
    }
    wl_list_remove(&f->link);
    free(f);
}

const char *foreign_identifier(struct tomoe *s, uint32_t id) {
    struct foreign *f = foreign_find(s, id);
    return f ? f->identifier : NULL;
}

uint32_t foreign_handle_window(struct wl_resource *handle) {
    struct foreign *f = wl_resource_get_user_data(handle);
    return f ? f->id : 0;
}

static void request(struct wl_resource *handle, const char *name, int requested,
        struct wl_resource *output) {
    struct foreign *f = wl_resource_get_user_data(handle);
    if (f) window_foreign_request(f->server, f->id, name, requested,
        output ? screen_from_resource(output) : NULL);
}

static void set_maximized(struct wl_client *client, struct wl_resource *handle) {
    request(handle, "maximize", 1, NULL);
}

static void unset_maximized(struct wl_client *client, struct wl_resource *handle) {
    request(handle, "maximize", 0, NULL);
}

static void set_minimized(struct wl_client *client, struct wl_resource *handle) {
    request(handle, "minimize", 1, NULL);
}

static void unset_minimized(struct wl_client *client, struct wl_resource *handle) {
    request(handle, "minimize", 0, NULL);
}

static void activate(struct wl_client *client, struct wl_resource *handle,
        struct wl_resource *seat) {
    request(handle, "activate", -1, NULL);
}

static void close_toplevel(struct wl_client *client, struct wl_resource *handle) {
    request(handle, "close", -1, NULL);
}

static void set_rectangle(struct wl_client *client, struct wl_resource *handle,
        struct wl_resource *surface, int32_t x, int32_t y, int32_t width, int32_t height) {
    if (width < 0 || height < 0)
        wl_resource_post_error(handle, ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_ERROR_INVALID_RECTANGLE,
            "invalid rectangle");
}

static void set_fullscreen(struct wl_client *client, struct wl_resource *handle,
        struct wl_resource *output) {
    request(handle, "fullscreen", 1, output);
}

static void unset_fullscreen(struct wl_client *client, struct wl_resource *handle) {
    request(handle, "fullscreen", 0, NULL);
}

static void destroy_resource(struct wl_client *client, struct wl_resource *resource) {
    wl_resource_destroy(resource);
}

static const struct zwlr_foreign_toplevel_handle_v1_interface wlr_handle_impl = {
    .set_maximized = set_maximized,
    .unset_maximized = unset_maximized,
    .set_minimized = set_minimized,
    .unset_minimized = unset_minimized,
    .activate = activate,
    .close = close_toplevel,
    .set_rectangle = set_rectangle,
    .destroy = destroy_resource,
    .set_fullscreen = set_fullscreen,
    .unset_fullscreen = unset_fullscreen,
};

static const struct ext_foreign_toplevel_handle_v1_interface ext_handle_impl = {
    .destroy = destroy_resource,
};

static void manager_stop(struct wl_client *client, struct wl_resource *manager) {
    wl_list_remove(wl_resource_get_link(manager));
    wl_list_init(wl_resource_get_link(manager));
    zwlr_foreign_toplevel_manager_v1_send_finished(manager);
    wl_resource_destroy(manager);
}

static const struct zwlr_foreign_toplevel_manager_v1_interface manager_impl = {
    .stop = manager_stop,
};

static void list_stop(struct wl_client *client, struct wl_resource *list) {
    wl_list_remove(wl_resource_get_link(list));
    wl_list_init(wl_resource_get_link(list));
    ext_foreign_toplevel_list_v1_send_finished(list);
}

static const struct ext_foreign_toplevel_list_v1_interface list_impl = {
    .stop = list_stop,
    .destroy = destroy_resource,
};

static void bind_manager(struct wl_client *client, void *data, uint32_t version, uint32_t id) {
    struct tomoe *s = data;
    struct wl_resource *manager = wl_resource_create(client,
        &zwlr_foreign_toplevel_manager_v1_interface, version, id);
    if (!manager) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(manager, &manager_impl, s, handle_resource_destroy);
    wl_list_insert(&s->foreign_managers, wl_resource_get_link(manager));
    struct foreign *f;
    wl_list_for_each(f, &s->foreigns, link) {
        const char *title, *app_id;
        uint32_t state;
        struct screen *outputs[FOREIGN_OUTPUTS];
        size_t output_count;
        if (!window_foreign_fields(s, f->id, &title, &app_id, &state, outputs, FOREIGN_OUTPUTS,
                &output_count)) continue;
        announce_wlr(f, manager, title, app_id, state, outputs, output_count);
    }
}

static void bind_list(struct wl_client *client, void *data, uint32_t version, uint32_t id) {
    struct tomoe *s = data;
    struct wl_resource *list = wl_resource_create(client, &ext_foreign_toplevel_list_v1_interface,
        version, id);
    if (!list) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(list, &list_impl, s, handle_resource_destroy);
    wl_list_insert(&s->foreign_lists, wl_resource_get_link(list));
    struct foreign *f;
    wl_list_for_each(f, &s->foreigns, link) {
        const char *title, *app_id;
        uint32_t state;
        struct screen *outputs[FOREIGN_OUTPUTS];
        size_t output_count;
        if (!window_foreign_fields(s, f->id, &title, &app_id, &state, outputs, FOREIGN_OUTPUTS,
                &output_count)) continue;
        announce_ext(f, list, title, app_id);
    }
}

bool foreign_listen(struct tomoe *s) {
    return wl_global_create(s->display, &zwlr_foreign_toplevel_manager_v1_interface, 3, s,
            bind_manager) &&
        wl_global_create(s->display, &ext_foreign_toplevel_list_v1_interface, 1, s, bind_list);
}
