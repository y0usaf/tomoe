#include "internal.h"
#include "wlr-output-management-unstable-v1-protocol.h"

struct managed_mode {
    struct wl_list link;
    struct wl_resource *resource;
    struct managed_head *head;
    struct screen_mode *mode;
    int width, height, refresh;
    bool finished;
};
struct managed_head {
    struct wl_list link, modes;
    struct wl_resource *resource;
    uint64_t id;
    bool finished, initialized, enabled, adaptive_sync;
    int x, y, transform;
    wl_fixed_t scale;
    struct managed_mode *current;
};
struct output_manager_binding {
    struct wl_list link, heads;
    struct wl_resource *resource;
    struct output_manager *manager;
};
struct output_manager {
    struct tomoe *server;
    struct wl_list bindings;
    struct wl_listener destroy;
    uint32_t serial;
};
struct configured_head {
    struct wl_list link;
    struct wl_resource *resource;
    struct output_configuration *configuration;
    struct output_configuration_entry entry;
    uint32_t set;
};
struct output_configuration {
    struct tomoe *server;
    struct wl_resource *resource;
    struct wl_list heads;
    uint32_t serial;
    uint64_t revision;
    bool used;
};

static void destroy_request(struct wl_client *client, struct wl_resource *resource) {
    wl_resource_destroy(resource);
}
static struct output *find_output(struct tomoe *s, uint64_t id) {
    struct output *o;
    wl_list_for_each(o, &s->outputs, link) if (o->id == id) return o;
    return NULL;
}
static void mode_destroy(struct wl_resource *resource) {
    struct managed_mode *mode = wl_resource_get_user_data(resource);
    if (!mode) return;
    mode->resource = NULL;
    if (mode->finished && mode != mode->head->current) {
        wl_list_remove(&mode->link);
        free(mode);
    }
}
static const struct zwlr_output_mode_v1_interface mode_impl = { .release = destroy_request };
static void head_destroy(struct wl_resource *resource) {
    struct managed_head *head = wl_resource_get_user_data(resource);
    if (head) head->resource = NULL;
}
static const struct zwlr_output_head_v1_interface head_impl = { .release = destroy_request };
static struct managed_mode *new_mode(struct managed_head *head, struct screen_mode *mode,
        int width, int height, int refresh) {
    struct managed_mode *m = calloc(1, sizeof(*m));
    if (!m) return NULL;
    int version = wl_resource_get_version(head->resource);
    m->resource = wl_resource_create(wl_resource_get_client(head->resource),
        &zwlr_output_mode_v1_interface, version < 3 ? version : 3, 0);
    if (!m->resource) { free(m); return NULL; }
    m->head = head; m->mode = mode;
    m->width = width; m->height = height; m->refresh = refresh;
    wl_list_insert(head->modes.prev, &m->link);
    wl_resource_set_implementation(m->resource, &mode_impl, m, mode_destroy);
    zwlr_output_head_v1_send_mode(head->resource, m->resource);
    zwlr_output_mode_v1_send_size(m->resource, width, height);
    if (refresh > 0) zwlr_output_mode_v1_send_refresh(m->resource, refresh);
    if (mode && mode->preferred) zwlr_output_mode_v1_send_preferred(m->resource);
    return m;
}
static bool head_update(struct managed_head *h, struct output *o) {
    if (!h->resource) return false;
    if (!o) {
        if (h->finished) return false;
        struct managed_mode *m;
        wl_list_for_each(m, &h->modes, link) {
            if (m->resource && !m->finished) zwlr_output_mode_v1_send_finished(m->resource);
            m->finished = true;
        }
        zwlr_output_head_v1_send_finished(h->resource);
        h->finished = true;
        return true;
    }
    struct screen *s = o->screen;
    bool initial = !h->initialized;
    bool enabled = output_is_active(o);
    bool changed = initial;
    if (initial || enabled != h->enabled) {
        zwlr_output_head_v1_send_enabled(h->resource, enabled);
        changed = true;
    }
    if (enabled) {
        struct managed_mode *current = NULL, *m;
        wl_list_for_each(m, &h->modes, link) {
            if (m->finished) continue;
            if ((s->current_mode && m->mode == s->current_mode) ||
                    (!s->current_mode && m->width == s->width && m->height == s->height &&
                        m->refresh == s->refresh)) { current = m; break; }
        }
        if (!current) {
            current = new_mode(h, s->current_mode, s->width, s->height, s->refresh);
            if (!current) { wl_client_post_no_memory(wl_resource_get_client(h->resource)); return false; }
            changed = true;
        }
        if (initial || !h->enabled || current != h->current) {
            if (current->resource) zwlr_output_head_v1_send_current_mode(h->resource, current->resource);
            changed = true;
            if (h->current && current != h->current && !h->current->mode && !h->current->finished) {
                if (h->current->resource) zwlr_output_mode_v1_send_finished(h->current->resource);
                h->current->finished = true;
                if (!h->current->resource) {
                    wl_list_remove(&h->current->link);
                    free(h->current);
                }
            }
        }
        if (initial || !h->enabled || h->x != s->lx || h->y != s->ly) {
            zwlr_output_head_v1_send_position(h->resource, s->lx, s->ly);
            changed = true;
        }
        if (initial || !h->enabled || h->transform != (int)s->transform) {
            zwlr_output_head_v1_send_transform(h->resource, s->transform);
            changed = true;
        }
        wl_fixed_t scale = wl_fixed_from_double(s->scale);
        if (initial || !h->enabled || h->scale != scale) {
            zwlr_output_head_v1_send_scale(h->resource, scale);
            changed = true;
        }
        h->current = current; h->x = s->lx; h->y = s->ly;
        h->transform = s->transform; h->scale = scale;
    }
    if (wl_resource_get_version(h->resource) >= 4 &&
            (initial || h->adaptive_sync != s->adaptive_sync)) {
        zwlr_output_head_v1_send_adaptive_sync(h->resource, s->adaptive_sync);
        changed = true;
    }
    h->initialized = true; h->enabled = enabled; h->adaptive_sync = s->adaptive_sync;
    return changed;
}
static bool binding_update(struct output_manager_binding *b) {
    struct tomoe *s = b->manager->server;
    bool changed = false;
    struct managed_head *h;
    wl_list_for_each(h, &b->heads, link)
        changed |= head_update(h, find_output(s, h->id));
    struct output *o;
    wl_list_for_each(o, &s->outputs, link) {
        bool found = false;
        wl_list_for_each(h, &b->heads, link) if (h->id == o->id) { found = true; break; }
        if (found) continue;
        h = calloc(1, sizeof(*h));
        if (!h) { wl_client_post_no_memory(wl_resource_get_client(b->resource)); break; }
        h->resource = wl_resource_create(wl_resource_get_client(b->resource),
            &zwlr_output_head_v1_interface, wl_resource_get_version(b->resource), 0);
        if (!h->resource) { free(h); wl_client_post_no_memory(wl_resource_get_client(b->resource)); break; }
        h->id = o->id;
        wl_list_init(&h->modes);
        wl_list_insert(b->heads.prev, &h->link);
        wl_resource_set_implementation(h->resource, &head_impl, h, head_destroy);
        zwlr_output_manager_v1_send_head(b->resource, h->resource);
        struct screen *screen = o->screen;
        zwlr_output_head_v1_send_name(h->resource, screen->name);
        zwlr_output_head_v1_send_description(h->resource, screen->description ?: screen->name);
        if (screen->phys_width > 0 && screen->phys_height > 0)
            zwlr_output_head_v1_send_physical_size(h->resource, screen->phys_width, screen->phys_height);
        if (wl_resource_get_version(h->resource) >= 2) {
            if (screen->make) zwlr_output_head_v1_send_make(h->resource, screen->make);
            if (screen->model) zwlr_output_head_v1_send_model(h->resource, screen->model);
            if (screen->serial) zwlr_output_head_v1_send_serial_number(h->resource, screen->serial);
        }
        struct screen_mode *mode;
        wl_list_for_each(mode, &screen->modes, link) {
            if (!new_mode(h, mode, mode->width, mode->height, mode->refresh)) {
                wl_client_post_no_memory(wl_resource_get_client(b->resource)); break;
            }
        }
        head_update(h, o);
        changed = true;
    }
    return changed;
}
void output_management_refresh(struct tomoe *s) {
    struct output_manager *manager = s->output_manager;
    if (!manager) return;
    bool changed = false;
    struct output_manager_binding *b;
    wl_list_for_each(b, &manager->bindings, link) changed |= binding_update(b);
    if (!changed) return;
    manager->serial = wl_display_next_serial(s->display);
    wl_list_for_each(b, &manager->bindings, link)
        zwlr_output_manager_v1_send_done(b->resource, manager->serial);
}
static bool config_unused(struct output_configuration *c) {
    if (!c->used) return true;
    wl_resource_post_error(c->resource, ZWLR_OUTPUT_CONFIGURATION_V1_ERROR_ALREADY_USED,
        "configuration already tested or applied");
    return false;
}
static bool property(struct configured_head *h, uint32_t bit) {
    if (!config_unused(h->configuration)) return false;
    if (h->set & bit) {
        wl_resource_post_error(h->resource, ZWLR_OUTPUT_CONFIGURATION_HEAD_V1_ERROR_ALREADY_SET,
            "property already set"); return false;
    }
    h->set |= bit;
    return true;
}
static void config_mode(struct wl_client *client, struct wl_resource *resource,
        struct wl_resource *mode_resource) {
    struct configured_head *h = wl_resource_get_user_data(resource);
    struct managed_mode *m = wl_resource_get_user_data(mode_resource);
    if (!property(h, SCREEN_MODE)) return;
    if (!m || m->finished || m->head->id != h->entry.id || m->head->finished ||
            !find_output(h->configuration->server, h->entry.id)) {
        wl_resource_post_error(resource, ZWLR_OUTPUT_CONFIGURATION_HEAD_V1_ERROR_INVALID_MODE,
            "mode does not belong to head"); return;
    }
    if (m->mode) screen_state_set_mode(&h->entry.state, m->mode);
    else screen_state_set_custom_mode(&h->entry.state, m->width, m->height, m->refresh);
}
static void config_custom_mode(struct wl_client *client, struct wl_resource *resource,
        int32_t width, int32_t height, int32_t refresh) {
    struct configured_head *h = wl_resource_get_user_data(resource);
    if (!property(h, SCREEN_MODE)) return;
    if (width <= 0 || height <= 0 || refresh < 0) {
        wl_resource_post_error(resource, ZWLR_OUTPUT_CONFIGURATION_HEAD_V1_ERROR_INVALID_CUSTOM_MODE,
            "invalid mode size or refresh"); return;
    }
    screen_state_set_custom_mode(&h->entry.state, width, height, refresh);
}
static void config_position(struct wl_client *client, struct wl_resource *resource, int32_t x, int32_t y) {
    struct configured_head *h = wl_resource_get_user_data(resource);
    if (!property(h, 1u << 16)) return;
    h->entry.x = x; h->entry.y = y;
}
static void config_transform(struct wl_client *client, struct wl_resource *resource, int32_t transform) {
    struct configured_head *h = wl_resource_get_user_data(resource);
    if (!property(h, SCREEN_TRANSFORM)) return;
    if (transform < WL_OUTPUT_TRANSFORM_NORMAL || transform > WL_OUTPUT_TRANSFORM_FLIPPED_270) {
        wl_resource_post_error(resource, ZWLR_OUTPUT_CONFIGURATION_HEAD_V1_ERROR_INVALID_TRANSFORM,
            "invalid transform"); return;
    }
    screen_state_set_transform(&h->entry.state, transform);
}
static void config_scale(struct wl_client *client, struct wl_resource *resource, wl_fixed_t scale) {
    struct configured_head *h = wl_resource_get_user_data(resource);
    if (!property(h, SCREEN_SCALE)) return;
    if (scale <= 0) {
        wl_resource_post_error(resource, ZWLR_OUTPUT_CONFIGURATION_HEAD_V1_ERROR_INVALID_SCALE,
            "scale must be positive"); return;
    }
    screen_state_set_scale(&h->entry.state, snapped_scale(wl_fixed_to_double(scale)));
}
static void config_adaptive_sync(struct wl_client *client, struct wl_resource *resource, uint32_t state) {
    struct configured_head *h = wl_resource_get_user_data(resource);
    if (!property(h, SCREEN_VRR)) return;
    if (state > ZWLR_OUTPUT_HEAD_V1_ADAPTIVE_SYNC_STATE_ENABLED) {
        wl_resource_post_error(resource, ZWLR_OUTPUT_CONFIGURATION_HEAD_V1_ERROR_INVALID_ADAPTIVE_SYNC_STATE,
            "invalid adaptive sync state"); return;
    }
    screen_state_set_adaptive_sync_enabled(&h->entry.state, state != 0);
}
static const struct zwlr_output_configuration_head_v1_interface config_head_impl = {
    .set_mode = config_mode, .set_custom_mode = config_custom_mode,
    .set_position = config_position, .set_transform = config_transform,
    .set_scale = config_scale, .set_adaptive_sync = config_adaptive_sync,
};
static struct configured_head *configure_head(struct output_configuration *c,
        struct wl_resource *head_resource, bool enabled) {
    if (!config_unused(c)) return NULL;
    struct managed_head *head = wl_resource_get_user_data(head_resource);
    uint64_t id = head ? head->id : 0;
    struct configured_head *h;
    wl_list_for_each(h, &c->heads, link) {
        if (h->entry.id != id) continue;
        wl_resource_post_error(c->resource, ZWLR_OUTPUT_CONFIGURATION_V1_ERROR_ALREADY_CONFIGURED_HEAD,
            "head configured twice"); return NULL;
    }
    h = calloc(1, sizeof(*h));
    if (!h) { wl_client_post_no_memory(wl_resource_get_client(c->resource)); return NULL; }
    h->configuration = c; h->entry.id = id;
    screen_state_init(&h->entry.state);
    struct output *o = find_output(c->server, id);
    if (o) {
        struct screen *s = o->screen;
        if (s->current_mode) screen_state_set_mode(&h->entry.state, s->current_mode);
        else screen_state_set_custom_mode(&h->entry.state, s->width, s->height, s->refresh);
        screen_state_set_scale(&h->entry.state, s->scale);
        screen_state_set_transform(&h->entry.state, s->transform);
        screen_state_set_adaptive_sync_enabled(&h->entry.state, s->adaptive_sync);
        h->entry.x = s->lx; h->entry.y = s->ly;
    }
    screen_state_set_enabled(&h->entry.state, enabled);
    wl_list_insert(c->heads.prev, &h->link);
    return h;
}
static void config_head_destroy(struct wl_resource *resource) {
    struct configured_head *h = wl_resource_get_user_data(resource);
    h->resource = NULL;
}
static void config_enable(struct wl_client *client, struct wl_resource *resource,
        uint32_t id, struct wl_resource *head) {
    struct output_configuration *c = wl_resource_get_user_data(resource);
    struct configured_head *h = configure_head(c, head, true);
    if (!h) return;
    h->resource = wl_resource_create(client, &zwlr_output_configuration_head_v1_interface,
        wl_resource_get_version(resource), id);
    if (!h->resource) { wl_client_post_no_memory(client); return; }
    wl_resource_set_implementation(h->resource, &config_head_impl, h, config_head_destroy);
}
static void config_disable(struct wl_client *client, struct wl_resource *resource, struct wl_resource *head) {
    configure_head(wl_resource_get_user_data(resource), head, false);
}
static void config_run(struct wl_resource *resource, bool test) {
    struct output_configuration *c = wl_resource_get_user_data(resource);
    if (!config_unused(c)) return;
    c->used = true;
    if (c->serial != c->server->output_manager->serial ||
            c->revision != c->server->outputs_revision) {
        zwlr_output_configuration_v1_send_cancelled(resource); return;
    }
    size_t count = wl_list_length(&c->heads);
    struct output *o;
    wl_list_for_each(o, &c->server->outputs, link) {
        bool found = false;
        struct configured_head *h;
        wl_list_for_each(h, &c->heads, link) if (h->entry.id == o->id) { found = true; break; }
        if (!found) {
            wl_resource_post_error(resource, ZWLR_OUTPUT_CONFIGURATION_V1_ERROR_UNCONFIGURED_HEAD,
                "configuration omits a head"); return;
        }
    }
    struct output_configuration_entry *entries = calloc(count ? count : 1, sizeof(*entries));
    if (!entries) { wl_client_post_no_memory(wl_resource_get_client(resource)); return; }
    size_t i = 0;
    struct configured_head *h;
    wl_list_for_each(h, &c->heads, link) entries[i++] = h->entry;
    bool ok = output_management_configure(c->server, entries, count, test);
    free(entries);
    if (ok) zwlr_output_configuration_v1_send_succeeded(resource);
    else zwlr_output_configuration_v1_send_failed(resource);
}
static void config_apply(struct wl_client *client, struct wl_resource *resource) { config_run(resource, false); }
static void config_test(struct wl_client *client, struct wl_resource *resource) { config_run(resource, true); }
static void config_destroy(struct wl_resource *resource) {
    struct output_configuration *c = wl_resource_get_user_data(resource);
    struct configured_head *h, *next;
    wl_list_for_each_safe(h, next, &c->heads, link) {
        if (h->resource) wl_resource_destroy(h->resource);
        screen_state_finish(&h->entry.state);
        wl_list_remove(&h->link); free(h);
    }
    free(c);
}
static const struct zwlr_output_configuration_v1_interface config_impl = {
    .enable_head = config_enable, .disable_head = config_disable,
    .apply = config_apply, .test = config_test, .destroy = destroy_request,
};
static void manager_create_config(struct wl_client *client, struct wl_resource *resource,
        uint32_t id, uint32_t serial) {
    struct output_manager_binding *b = wl_resource_get_user_data(resource);
    struct output_configuration *c = calloc(1, sizeof(*c));
    if (!c) { wl_client_post_no_memory(client); return; }
    c->server = b->manager->server; c->serial = serial;
    c->revision = c->server->outputs_revision;
    wl_list_init(&c->heads);
    c->resource = wl_resource_create(client, &zwlr_output_configuration_v1_interface,
        wl_resource_get_version(resource), id);
    if (!c->resource) { free(c); wl_client_post_no_memory(client); return; }
    wl_resource_set_implementation(c->resource, &config_impl, c, config_destroy);
}
static void binding_destroy(struct wl_resource *resource) {
    struct output_manager_binding *b = wl_resource_get_user_data(resource);
    struct managed_head *h, *next;
    wl_list_for_each_safe(h, next, &b->heads, link) {
        struct managed_mode *m, *mn;
        wl_list_for_each_safe(m, mn, &h->modes, link) {
            if (m->resource) wl_resource_set_user_data(m->resource, NULL);
            wl_list_remove(&m->link); free(m);
        }
        if (h->resource) wl_resource_set_user_data(h->resource, NULL);
        wl_list_remove(&h->link); free(h);
    }
    wl_list_remove(&b->link); free(b);
}
static void manager_stop(struct wl_client *client, struct wl_resource *resource) {
    zwlr_output_manager_v1_send_finished(resource);
    wl_resource_destroy(resource);
}
static const struct zwlr_output_manager_v1_interface manager_impl = {
    .create_configuration = manager_create_config, .stop = manager_stop,
};
static void manager_bind(struct wl_client *client, void *data, uint32_t version, uint32_t id) {
    struct output_manager *manager = data;
    struct output_manager_binding *b = calloc(1, sizeof(*b));
    if (!b) { wl_client_post_no_memory(client); return; }
    b->resource = wl_resource_create(client, &zwlr_output_manager_v1_interface, version, id);
    if (!b->resource) { free(b); wl_client_post_no_memory(client); return; }
    b->manager = manager;
    wl_list_init(&b->heads);
    wl_list_insert(manager->bindings.prev, &b->link);
    wl_resource_set_implementation(b->resource, &manager_impl, b, binding_destroy);
    binding_update(b);
    zwlr_output_manager_v1_send_done(b->resource, manager->serial);
}
static void manager_destroy(struct wl_listener *listener, void *data) {
    struct output_manager *manager = wl_container_of(listener, manager, destroy);
    detach(&manager->destroy);
    manager->server->output_manager = NULL;
    free(manager);
}
bool output_management_listen(struct tomoe *s) {
    struct output_manager *manager = calloc(1, sizeof(*manager));
    if (!manager) return false;
    manager->server = s;
    manager->serial = wl_display_next_serial(s->display);
    wl_list_init(&manager->bindings);
    if (!wl_global_create(s->display, &zwlr_output_manager_v1_interface, 4, manager, manager_bind)) {
        free(manager); return false;
    }
    s->output_manager = manager;
    manager->destroy.notify = manager_destroy;
    wl_display_add_destroy_listener(s->display, &manager->destroy);
    return true;
}
