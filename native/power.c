#include "internal.h"
#include "wlr-output-power-management-unstable-v1-protocol.h"

struct power {
    struct wl_resource *resource;
    struct wl_list link;
    struct output *output;
};

static void power_detach(struct power *power) {
    if (!power) return;
    wl_resource_set_user_data(power->resource, NULL);
    wl_list_remove(&power->link);
    free(power);
}

static void power_resource_destroy(struct wl_resource *resource) {
    power_detach(wl_resource_get_user_data(resource));
}

static void send_mode(struct output *o) {
    struct power *power;
    wl_list_for_each(power, &o->server->powers, link)
        if (power->output == o)
            zwlr_output_power_v1_send_mode(power->resource, o->screen->power_off ?
                ZWLR_OUTPUT_POWER_V1_MODE_OFF : ZWLR_OUTPUT_POWER_V1_MODE_ON);
}

bool output_power(struct output *o, bool on) {
    struct screen *screen = o->screen;
    if (screen->power_off != on) return true;
    screen->power_off = !on;
    if (on) {
        screen->frame_pending = false;
        o->gamma_dirty = true;
        screen_schedule_frame(screen);
    } else {
        struct screen_state state;
        screen_state_init(&state);
        if (!screen_commit(screen, &state)) {
            screen->power_off = false;
            tomoe_log(LOG_ERROR, "tomoe: output %s could not power off", screen->name);
            return false;
        }
    }
    tomoe_log(LOG_DEBUG, "tomoe: output %s power %s", screen->name, on ? "on" : "off");
    send_mode(o);
    outputs_event(o->server);
    return true;
}

int tomoe_output_power(struct tomoe *s, const char *name, int mode) {
    if (!s || mode < 0 || mode > 2) return 0;
    bool any = false, lit = false;
    struct output *o;
    wl_list_for_each(o, &s->outputs, link) {
        if (name && strcmp(o->screen->name, name)) continue;
        any = true;
        lit |= !o->screen->power_off;
    }
    if (!any) {
        tomoe_log(LOG_ERROR, "tomoe: no output named %s to power", name ? name : "(any)");
        return 0;
    }
    bool on = mode == 2 ? !lit : mode == 1;
    bool ok = true;
    wl_list_for_each(o, &s->outputs, link)
        if (!name || !strcmp(o->screen->name, name)) ok &= output_power(o, on);
    return ok;
}

static void set_mode(struct wl_client *client, struct wl_resource *resource, uint32_t mode) {
    if (mode != ZWLR_OUTPUT_POWER_V1_MODE_OFF && mode != ZWLR_OUTPUT_POWER_V1_MODE_ON) {
        wl_resource_post_error(resource, ZWLR_OUTPUT_POWER_V1_ERROR_INVALID_MODE,
            "invalid power mode %u", mode);
        return;
    }
    struct power *power = wl_resource_get_user_data(resource);
    if (power && !output_power(power->output, mode == ZWLR_OUTPUT_POWER_V1_MODE_ON)) {
        zwlr_output_power_v1_send_failed(resource);
        power_detach(power);
    }
}

static void power_destroy(struct wl_client *client, struct wl_resource *resource) {
    wl_resource_destroy(resource);
}

static const struct zwlr_output_power_v1_interface power_impl = {
    .set_mode = set_mode,
    .destroy = power_destroy,
};

static void get_output_power(struct wl_client *client, struct wl_resource *manager,
        uint32_t id, struct wl_resource *output_resource) {
    struct tomoe *s = wl_resource_get_user_data(manager);
    struct wl_resource *resource = wl_resource_create(client, &zwlr_output_power_v1_interface,
        wl_resource_get_version(manager), id);
    if (!resource) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(resource, &power_impl, NULL, power_resource_destroy);
    struct screen *screen = screen_from_resource(output_resource);
    struct output *o = NULL, *candidate;
    wl_list_for_each(candidate, &s->outputs, link) if (candidate->screen == screen) o = candidate;
    struct power *power = o ? calloc(1, sizeof(*power)) : NULL;
    if (!power) {
        zwlr_output_power_v1_send_failed(resource);
        return;
    }
    power->resource = resource;
    power->output = o;
    wl_list_insert(&s->powers, &power->link);
    wl_resource_set_user_data(resource, power);
    zwlr_output_power_v1_send_mode(resource, o->screen->power_off ?
        ZWLR_OUTPUT_POWER_V1_MODE_OFF : ZWLR_OUTPUT_POWER_V1_MODE_ON);
}

static void manager_destroy(struct wl_client *client, struct wl_resource *resource) {
    wl_resource_destroy(resource);
}

static const struct zwlr_output_power_manager_v1_interface manager_impl = {
    .get_output_power = get_output_power,
    .destroy = manager_destroy,
};

static void bind(struct wl_client *client, void *data, uint32_t version, uint32_t id) {
    struct wl_resource *resource = wl_resource_create(client,
        &zwlr_output_power_manager_v1_interface, version, id);
    if (!resource) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(resource, &manager_impl, data, NULL);
}

bool power_listen(struct tomoe *s) {
    return wl_global_create(s->display, &zwlr_output_power_manager_v1_interface, 1, s, bind);
}

void power_output_gone(struct output *o) {
    struct power *power, *next;
    wl_list_for_each_safe(power, next, &o->server->powers, link) {
        if (power->output != o) continue;
        zwlr_output_power_v1_send_failed(power->resource);
        power_detach(power);
    }
}
