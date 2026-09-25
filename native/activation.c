#include "internal.h"
#include <sys/random.h>
#include "xdg-activation-v1-protocol.h"

#define ACTIVATION_TOKEN_LIMIT 64u
#define ACTIVATION_TRACKED_TOKEN_LIMIT 128u
#define ACTIVATION_PENDING_LIMIT 64u
#define ACTIVATION_TIMEOUT_MSEC 10000u

struct token {
    struct wl_list link;
    struct tomoe *server;
    struct wl_resource *resource;
    char name[33];
    uint64_t deadline_msec;
    uint32_t serial;
    bool serial_bearing, committed, issued, accepted, activate;
};

struct pending_request {
    struct wl_list link;
    struct tomoe *server;
    struct wlr_surface *surface;
    bool activate;
    uint64_t deadline_msec;
    struct wl_listener surface_destroy;
    struct wl_event_source *timeout;
};

static uint64_t activation_now_msec(void) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) < 0) return 0;
    return (uint64_t)now.tv_sec * 1000u + (uint64_t)now.tv_nsec / 1000000u;
}

static uint64_t activation_deadline(void) {
    uint64_t now = activation_now_msec();
    return now ? now + ACTIVATION_TIMEOUT_MSEC : 0;
}

static bool deadline_expired(uint64_t deadline) {
    uint64_t now = activation_now_msec();
    return deadline && now >= deadline;
}

static uint32_t deadline_remaining(uint64_t deadline) {
    uint64_t now = activation_now_msec();
    if (!deadline || !now || now >= deadline) return 0;
    uint64_t remaining = deadline - now;
    return remaining > UINT32_MAX ? UINT32_MAX : (uint32_t)remaining;
}

static bool serial_at_least(uint32_t serial, uint32_t latest) {
    return (uint32_t)(serial - latest) < (UINT32_C(1) << 31);
}

static bool fresh_serial(struct tomoe *s, uint32_t serial) {
    return (s->have_keyboard_enter_serial &&
            serial_at_least(serial, s->latest_keyboard_enter_serial)) ||
        (s->have_pointer_enter_serial &&
            serial_at_least(serial, s->latest_pointer_enter_serial));
}

static void token_destroy(struct token *token) {
    struct tomoe *s = token->server;
    if (token->resource) wl_resource_set_user_data(token->resource, NULL);
    wl_list_remove(&token->link);
    if (token->issued && s->activation_token_count > 0) s->activation_token_count--;
    if (s->activation_tracked_count > 0) s->activation_tracked_count--;
    free(token);
}

static struct token *token_create(struct tomoe *s) {
    struct token *token, *next;
    wl_list_for_each_safe(token, next, &s->activation_tokens, link)
        if (token->committed && deadline_expired(token->deadline_msec)) token_destroy(token);
    if (s->activation_tracked_count >= ACTIVATION_TRACKED_TOKEN_LIMIT) {
        wlr_log(WLR_ERROR, "tomoe: activation token limit reached");
        return NULL;
    }
    unsigned char bytes[16];
    token = calloc(1, sizeof(*token));
    if (!token || getrandom(bytes, sizeof(bytes), 0) != sizeof(bytes)) {
        free(token);
        wlr_log(WLR_ERROR, "tomoe: activation token allocation failed");
        return NULL;
    }
    for (size_t i = 0; i < sizeof(bytes); i++) snprintf(token->name + 2 * i, 3, "%02x", bytes[i]);
    token->server = s;
    wl_list_insert(s->activation_tokens.prev, &token->link);
    s->activation_tracked_count++;
    return token;
}

static void token_commit(struct token *token) {
    struct tomoe *s = token->server;
    token->committed = true;
    token->activate = token->issued || token->serial_bearing;
    token->accepted = !token->serial_bearing || s->settings.honor_invalid_serial ||
        fresh_serial(s, token->serial);
    token->deadline_msec = activation_deadline();
}

static void pending_destroy(struct pending_request *pending) {
    struct tomoe *s = pending->server;
    if (pending->timeout) {
        wl_event_source_remove(pending->timeout);
        pending->timeout = NULL;
    }
    detach(&pending->surface_destroy);
    wl_list_remove(&pending->link);
    wl_list_init(&pending->link);
    if (s->activation_pending_count > 0) s->activation_pending_count--;
    free(pending);
}

static int pending_timeout(void *data) {
    pending_destroy(data);
    return 0;
}

static void pending_surface_destroy(struct wl_listener *listener, void *data) {
    struct pending_request *pending =
        wl_container_of(listener, pending, surface_destroy);
    pending_destroy(pending);
}

static void emit_request(struct tomoe *s, uint32_t id, bool activate) {
    struct event *event;
    size_t size;
    FILE *out = begin_event(s, &event, &size);
    if (!out) return;
    fprintf(out, "(:type :request :id %u :request :%s)", id,
        activate ? "activate" : "urgent");
    end_event(s, event, out);
}

static void pending_map(struct tomoe *s, struct wlr_surface *surface) {
    struct pending_request *pending, *tmp;
    wl_list_for_each_safe(pending, tmp, &s->activation_pending, link) {
        if (pending->surface != surface) continue;
        uint32_t id = find_window_id_for_surface(s, surface);
        bool activate = pending->activate;
        bool expired = deadline_expired(pending->deadline_msec);
        pending_destroy(pending);
        if (id && !expired) emit_request(s, id, activate);
    }
}

static void activate(struct wl_client *client, struct wl_resource *resource,
        const char *name, struct wl_resource *surface_resource) {
    struct tomoe *s = wl_resource_get_user_data(resource);
    struct wlr_surface *surface = wlr_surface_from_resource(surface_resource);
    struct token *token, *found = NULL;
    wl_list_for_each(token, &s->activation_tokens, link)
        if (token->committed && !strcmp(token->name, name)) found = token;
    if (!found) return;
    bool accepted = found->accepted, activate = found->activate;
    uint64_t deadline = found->deadline_msec;
    token_destroy(found);
    if (!accepted || deadline_expired(deadline)) return;
    uint32_t id = find_window_id_for_surface(s, surface);
    if (id && window_surface_mapped(s, surface)) {
        emit_request(s, id, activate);
        return;
    }

    struct pending_request *pending;
    wl_list_for_each(pending, &s->activation_pending, link) {
        if (pending->surface != surface) continue;
        pending->activate = activate;
        pending->deadline_msec = deadline;
        if (pending->timeout) {
            uint32_t remaining = deadline_remaining(deadline);
            if (remaining) wl_event_source_timer_update(pending->timeout, remaining);
        }
        return;
    }
    if (s->activation_pending_count >= ACTIVATION_PENDING_LIMIT) {
        wlr_log(WLR_ERROR, "tomoe: activation pending request limit reached");
        return;
    }

    pending = calloc(1, sizeof(*pending));
    if (!pending) {
        wlr_log(WLR_ERROR, "tomoe: activation pending request allocation failed");
        return;
    }
    pending->server = s;
    pending->surface = surface;
    pending->activate = activate;
    pending->deadline_msec = deadline;
    pending->surface_destroy.notify = pending_surface_destroy;
    wl_signal_add(&surface->events.destroy, &pending->surface_destroy);
    struct wl_event_loop *loop = wl_display_get_event_loop(s->display);
    pending->timeout = wl_event_loop_add_timer(loop, pending_timeout, pending);
    if (!pending->timeout) {
        detach(&pending->surface_destroy);
        free(pending);
        wlr_log(WLR_ERROR, "tomoe: activation pending timer allocation failed");
        return;
    }
    uint32_t remaining = deadline_remaining(deadline);
    if (!remaining) remaining = ACTIVATION_TIMEOUT_MSEC;
    wl_event_source_timer_update(pending->timeout, remaining);
    wl_list_insert(s->activation_pending.prev, &pending->link);
    s->activation_pending_count++;
}

static void token_resource_destroy(struct wl_resource *resource) {
    struct token *token = wl_resource_get_user_data(resource);
    if (!token) return;
    token->resource = NULL;
    if (!token->committed) token_destroy(token);
}

static void token_set_serial(struct wl_client *client, struct wl_resource *resource,
        uint32_t serial, struct wl_resource *seat) {
    struct token *token = wl_resource_get_user_data(resource);
    if (!token) return;
    token->serial = serial;
    token->serial_bearing = true;
}

static void token_ignore_string(struct wl_client *client, struct wl_resource *resource,
        const char *text) {
}

static void token_ignore_surface(struct wl_client *client, struct wl_resource *resource,
        struct wl_resource *surface) {
}

static void token_commit_request(struct wl_client *client, struct wl_resource *resource) {
    struct token *token = wl_resource_get_user_data(resource);
    if (token && token->committed) {
        wl_resource_post_error(resource, XDG_ACTIVATION_TOKEN_V1_ERROR_ALREADY_USED,
            "token already committed");
        return;
    }
    if (token) token_commit(token);
    xdg_activation_token_v1_send_done(resource, token ? token->name : "");
}

static void destroy_resource(struct wl_client *client, struct wl_resource *resource) {
    wl_resource_destroy(resource);
}

static const struct xdg_activation_token_v1_interface token_impl = {
    .set_serial = token_set_serial,
    .set_app_id = token_ignore_string,
    .set_surface = token_ignore_surface,
    .commit = token_commit_request,
    .destroy = destroy_resource,
};

static void get_activation_token(struct wl_client *client, struct wl_resource *resource,
        uint32_t id) {
    struct tomoe *s = wl_resource_get_user_data(resource);
    struct wl_resource *token_resource = wl_resource_create(client,
        &xdg_activation_token_v1_interface, wl_resource_get_version(resource), id);
    if (!token_resource) {
        wl_client_post_no_memory(client);
        return;
    }
    struct token *token = token_create(s);
    if (token) token->resource = token_resource;
    wl_resource_set_implementation(token_resource, &token_impl, token, token_resource_destroy);
}

static const struct xdg_activation_v1_interface activation_impl = {
    .destroy = destroy_resource,
    .get_activation_token = get_activation_token,
    .activate = activate,
};

static void bind(struct wl_client *client, void *data, uint32_t version, uint32_t id) {
    struct wl_resource *resource = wl_resource_create(client, &xdg_activation_v1_interface,
        version, id);
    if (!resource) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(resource, &activation_impl, data, NULL);
}

bool activation_listen(struct tomoe *s) {
    return wl_global_create(s->display, &xdg_activation_v1_interface, 1, s, bind);
}

void activation_surface_mapped(struct tomoe *s, struct wlr_surface *surface) {
    if (!s || !surface) return;
    pending_map(s, surface);
}

const char *tomoe_activation_token(struct tomoe *s) {
    if (!s || s->stopping || s->failed ||
            s->activation_token_count >= ACTIVATION_TOKEN_LIMIT) return NULL;
    struct token *token = token_create(s);
    if (!token) return NULL;
    token->issued = true;
    s->activation_token_count++;
    token_commit(token);
    return token->name;
}

void tomoe_activation_revoke(struct tomoe *s, const char *name) {
    if (!s || !name) return;
    struct token *token, *next;
    wl_list_for_each_safe(token, next, &s->activation_tokens, link)
        if (token->issued && !strcmp(token->name, name)) token_destroy(token);
}

void activation_finish(struct tomoe *s) {
    if (!s) return;
    struct pending_request *pending, *pending_tmp;
    wl_list_for_each_safe(pending, pending_tmp, &s->activation_pending, link)
        pending_destroy(pending);
    struct token *token, *next;
    wl_list_for_each_safe(token, next, &s->activation_tokens, link) token_destroy(token);
}
