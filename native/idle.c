#include "internal.h"
#include "ext-idle-notify-v1-protocol.h"
#include "idle-inhibit-unstable-v1-protocol.h"

struct notification {
    struct wl_resource *resource;
    struct wl_list link;
    struct tomoe *server;
    struct wl_event_source *timer;
    uint32_t timeout;
    bool idle, obey_inhibitors;
};

struct inhibitor {
    struct wl_resource *resource;
    struct wl_list link;
    struct surface *surface;
    struct wl_listener surface_destroy;
};

static void set_idle(struct notification *n, bool idle) {
    if (n->idle == idle) return;
    n->idle = idle;
    if (idle) ext_idle_notification_v1_send_idled(n->resource);
    else ext_idle_notification_v1_send_resumed(n->resource);
}

static void reset(struct notification *n) {
    if (n->server->idle_inhibited && n->obey_inhibitors) {
        set_idle(n, false);
        if (n->timer) wl_event_source_timer_update(n->timer, 0);
    } else if (n->timer) {
        wl_event_source_timer_update(n->timer, n->timeout);
    } else {
        set_idle(n, true);
    }
}

static int timer_fired(void *data) {
    set_idle(data, true);
    return 0;
}

static void notification_free(struct wl_resource *resource) {
    struct notification *n = wl_resource_get_user_data(resource);
    if (!n) return;
    wl_list_remove(&n->link);
    if (n->timer) wl_event_source_remove(n->timer);
    free(n);
}

static void destroy_resource(struct wl_client *client, struct wl_resource *resource) {
    wl_resource_destroy(resource);
}

static const struct ext_idle_notification_v1_interface notification_impl = {
    .destroy = destroy_resource,
};

static void notification_create(struct wl_client *client, struct wl_resource *manager,
        uint32_t id, uint32_t timeout, bool obey_inhibitors) {
    struct tomoe *s = wl_resource_get_user_data(manager);
    struct wl_resource *resource = wl_resource_create(client, &ext_idle_notification_v1_interface,
        wl_resource_get_version(manager), id);
    struct notification *n = resource ? calloc(1, sizeof(*n)) : NULL;
    if (!n) {
        if (resource) wl_resource_destroy(resource);
        wl_client_post_no_memory(client);
        return;
    }
    n->resource = resource;
    n->server = s;
    n->timeout = timeout;
    n->obey_inhibitors = obey_inhibitors;
    if (timeout) n->timer = wl_event_loop_add_timer(wl_display_get_event_loop(s->display),
        timer_fired, n);
    wl_resource_set_implementation(resource, &notification_impl, n, notification_free);
    wl_list_insert(&s->idle_notifications, &n->link);
    reset(n);
}

static void get_idle_notification(struct wl_client *client, struct wl_resource *manager,
        uint32_t id, uint32_t timeout, struct wl_resource *seat) {
    notification_create(client, manager, id, timeout, true);
}

static void get_input_idle_notification(struct wl_client *client, struct wl_resource *manager,
        uint32_t id, uint32_t timeout, struct wl_resource *seat) {
    notification_create(client, manager, id, timeout, false);
}

static const struct ext_idle_notifier_v1_interface notifier_impl = {
    .destroy = destroy_resource,
    .get_idle_notification = get_idle_notification,
    .get_input_idle_notification = get_input_idle_notification,
};

static void inhibitor_free(struct wl_resource *resource) {
    struct inhibitor *i = wl_resource_get_user_data(resource);
    if (!i) return;
    wl_list_remove(&i->link);
    detach(&i->surface_destroy);
    free(i);
}

static void inhibitor_surface_destroyed(struct wl_listener *listener, void *data) {
    struct inhibitor *i = wl_container_of(listener, i, surface_destroy);
    wl_resource_set_user_data(i->resource, NULL);
    wl_list_remove(&i->link);
    detach(&i->surface_destroy);
    free(i);
}

static const struct zwp_idle_inhibitor_v1_interface inhibitor_impl = {
    .destroy = destroy_resource,
};

static void create_inhibitor(struct wl_client *client, struct wl_resource *manager, uint32_t id,
        struct wl_resource *surface) {
    struct tomoe *s = wl_resource_get_user_data(manager);
    struct wl_resource *resource = wl_resource_create(client, &zwp_idle_inhibitor_v1_interface,
        wl_resource_get_version(manager), id);
    struct inhibitor *i = resource ? calloc(1, sizeof(*i)) : NULL;
    if (!i) {
        if (resource) wl_resource_destroy(resource);
        wl_client_post_no_memory(client);
        return;
    }
    i->resource = resource;
    i->surface = surface_from_resource(surface);
    listen(&i->surface_destroy, &i->surface->events.destroy, inhibitor_surface_destroyed);
    wl_resource_set_implementation(resource, &inhibitor_impl, i, inhibitor_free);
    wl_list_insert(&s->idle_inhibitors, &i->link);
}

static const struct zwp_idle_inhibit_manager_v1_interface inhibit_impl = {
    .destroy = destroy_resource,
    .create_inhibitor = create_inhibitor,
};

static void bind_notifier(struct wl_client *client, void *data, uint32_t version, uint32_t id) {
    struct wl_resource *resource = wl_resource_create(client, &ext_idle_notifier_v1_interface,
        version, id);
    if (!resource) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(resource, &notifier_impl, data, NULL);
}

static void bind_inhibit(struct wl_client *client, void *data, uint32_t version, uint32_t id) {
    struct wl_resource *resource = wl_resource_create(client,
        &zwp_idle_inhibit_manager_v1_interface, version, id);
    if (!resource) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(resource, &inhibit_impl, data, NULL);
}

bool idle_listen(struct tomoe *s) {
    return wl_global_create(s->display, &ext_idle_notifier_v1_interface, 2, s, bind_notifier) &&
        wl_global_create(s->display, &zwp_idle_inhibit_manager_v1_interface, 1, s, bind_inhibit);
}

void idle_notify_activity(struct tomoe *s) {
    struct notification *n;
    wl_list_for_each(n, &s->idle_notifications, link) {
        if (s->idle_inhibited && n->obey_inhibitors) continue;
        set_idle(n, false);
        reset(n);
    }
}

void idle_refresh(struct tomoe *s) {
    bool inhibited = false;
    struct inhibitor *i;
    wl_list_for_each(i, &s->idle_inhibitors, link) inhibited |= surface_visible(s, i->surface);
    inhibited = inhibited && !lock_active(s);
    if (s->idle_inhibited == inhibited) return;
    s->idle_inhibited = inhibited;
    struct notification *n;
    wl_list_for_each(n, &s->idle_notifications, link) if (n->obey_inhibitors) reset(n);
}
