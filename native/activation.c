#include "internal.h"

#define ACTIVATION_TOKEN_LIMIT 64u
#define ACTIVATION_TRACKED_TOKEN_LIMIT 128u
#define ACTIVATION_PENDING_LIMIT 64u
#define ACTIVATION_TIMEOUT_MSEC 10000u

struct tracked_token {
    struct wl_list link;
    struct tomoe *server;
    struct wlr_xdg_activation_token_v1 *token;
    struct wl_listener destroy;
    uint64_t deadline_msec;
    uint32_t serial;
    bool issued, accepted, serial_bearing, activate;
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

static struct tracked_token *tracked_token_for(struct tomoe *s,
        struct wlr_xdg_activation_token_v1 *token) {
    struct tracked_token *tracked;
    wl_list_for_each(tracked, &s->activation_tokens, link) {
        if (tracked->token == token) return tracked;
    }
    return NULL;
}

static void tracked_token_destroy(struct wl_listener *listener, void *data) {
    struct tracked_token *tracked = wl_container_of(listener, tracked, destroy);
    struct tomoe *s = tracked->server;
    detach(&tracked->destroy);
    wl_list_remove(&tracked->link);
    wl_list_init(&tracked->link);
    if (tracked->issued && s->activation_token_count > 0)
        s->activation_token_count--;
    if (s->activation_tracked_count > 0) s->activation_tracked_count--;
    if (tracked->token) tracked->token->data = NULL;
    free(tracked);
}

static bool track_token(struct tomoe *s,
        struct wlr_xdg_activation_token_v1 *token, bool issued) {
    if (s->activation_tracked_count >= ACTIVATION_TRACKED_TOKEN_LIMIT) {
        wlr_log(WLR_ERROR, "tomoe: activation token tracking limit reached");
        return false;
    }
    struct tracked_token *tracked = calloc(1, sizeof(*tracked));
    if (!tracked) {
        wlr_log(WLR_ERROR, "tomoe: activation token tracking allocation failed");
        return false;
    }
    tracked->server = s;
    tracked->token = token;
    tracked->issued = issued;
    tracked->serial = token->serial;
    tracked->serial_bearing = token->seat != NULL;
    tracked->activate = issued || tracked->serial_bearing;
    tracked->accepted = !tracked->serial_bearing || s->settings.honor_invalid_serial ||
        fresh_serial(s, tracked->serial);
    tracked->deadline_msec = activation_deadline();
    tracked->destroy.notify = tracked_token_destroy;
    token->data = tracked;
    wl_signal_add(&token->events.destroy, &tracked->destroy);
    wl_list_insert(s->activation_tokens.prev, &tracked->link);
    s->activation_tracked_count++;
    if (issued) s->activation_token_count++;
    return true;
}

static void activation_new_token(struct wl_listener *listener, void *data) {
    struct tomoe *s = wl_container_of(listener, s, activation_new_token);
    struct wlr_xdg_activation_token_v1 *token = data;
    if (token) (void)track_token(s, token, false);
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

static void activation_request(struct wl_listener *listener, void *data) {
    struct tomoe *s = wl_container_of(listener, s, activation_request);
    struct wlr_xdg_activation_v1_request_activate_event *request = data;
    if (!request || !request->surface) return;

    struct tracked_token *tracked = tracked_token_for(s, request->token);
    if (!tracked) {
        wlr_log(WLR_ERROR, "tomoe: untracked activation token request dropped");
        return;
    }
    if (!tracked->accepted) return;
    bool activate = tracked->activate;
    struct wlr_surface *surface = request->surface;
    uint64_t deadline = tracked->deadline_msec;
    if (deadline_expired(deadline)) return;
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

static void activation_destroy(struct wl_listener *listener, void *data) {
    struct tomoe *s = wl_container_of(listener, s, activation_destroy);
    s->activation = NULL;
    detach(&s->activation_destroy);
}

void activation_listen(struct tomoe *s) {
    if (!s->activation) return;
    s->activation->token_timeout_msec = ACTIVATION_TIMEOUT_MSEC;
    listen(&s->activation_new_token, &s->activation->events.new_token,
        activation_new_token);
    listen(&s->activation_request, &s->activation->events.request_activate,
        activation_request);
    listen(&s->activation_destroy, &s->activation->events.destroy,
        activation_destroy);
}

void activation_surface_mapped(struct tomoe *s, struct wlr_surface *surface) {
    if (!s || !surface) return;
    pending_map(s, surface);
}

const char *tomoe_activation_token(struct tomoe *s) {
    if (!s || s->stopping || s->failed || !s->activation ||
            s->activation_token_count >= ACTIVATION_TOKEN_LIMIT) return NULL;
    struct wlr_xdg_activation_token_v1 *token =
        wlr_xdg_activation_token_v1_create(s->activation);
    if (!token) return NULL;
    if (!track_token(s, token, true)) {
        wlr_xdg_activation_token_v1_destroy(token);
        return NULL;
    }
    return wlr_xdg_activation_token_v1_get_name(token);
}

void tomoe_activation_revoke(struct tomoe *s, const char *name) {
    if (!s || !name) return;
    struct tracked_token *tracked, *tmp;
    wl_list_for_each_safe(tracked, tmp, &s->activation_tokens, link) {
        if (!tracked->issued ||
                strcmp(name, wlr_xdg_activation_token_v1_get_name(tracked->token)) != 0)
            continue;
        wlr_xdg_activation_token_v1_destroy(tracked->token);
        return;
    }
}

void activation_finish(struct tomoe *s) {
    if (!s) return;
    detach(&s->activation_request);
    detach(&s->activation_new_token);
    struct pending_request *pending, *pending_tmp;
    wl_list_for_each_safe(pending, pending_tmp, &s->activation_pending, link)
        pending_destroy(pending);
    struct tracked_token *tracked, *tracked_tmp;
    wl_list_for_each_safe(tracked, tracked_tmp, &s->activation_tokens, link) {
        if (tracked->issued) wlr_xdg_activation_token_v1_destroy(tracked->token);
    }
}
