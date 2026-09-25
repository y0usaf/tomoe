#include "internal.h"
#include <fcntl.h>
#include <unistd.h>
#include <wlr/render/color.h>
#include "wlr-gamma-control-unstable-v1-protocol.h"

struct gamma {
    struct wl_resource *resource;
    struct wl_list link;
    struct output *output;
    uint16_t *table;
    size_t size;
};

static const struct zwlr_gamma_control_v1_interface gamma_impl;

static void gamma_detach(struct gamma *gamma) {
    if (!gamma) return;
    if (gamma->table) {
        gamma->output->gamma_dirty = true;
        wlr_output_schedule_frame(gamma->output->wlr);
    }
    wl_resource_set_user_data(gamma->resource, NULL);
    wl_list_remove(&gamma->link);
    free(gamma->table);
    free(gamma);
}

static void gamma_fail(struct gamma *gamma) {
    zwlr_gamma_control_v1_send_failed(gamma->resource);
    gamma_detach(gamma);
}

static void gamma_resource_destroy(struct wl_resource *resource) {
    gamma_detach(wl_resource_get_user_data(resource));
}

static void gamma_set(struct wl_client *client, struct wl_resource *resource, int fd) {
    struct gamma *gamma = wl_resource_get_user_data(resource);
    size_t bytes = gamma ? gamma->size * 3 * sizeof(uint16_t) : 0;
    uint16_t *table = gamma ? malloc(bytes) : NULL;
    int flags = fcntl(fd, F_GETFL, 0);
    ssize_t read = table && flags != -1 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) != -1 ?
        pread(fd, table, bytes, 0) : -1;
    close(fd);
    if (!gamma) {
        free(table);
        return;
    }
    if (read >= 0 && (size_t)read != bytes) {
        free(table);
        wl_resource_post_error(resource, ZWLR_GAMMA_CONTROL_V1_ERROR_INVALID_GAMMA,
            "gamma ramps have the wrong size");
        return;
    }
    if (read < 0) {
        free(table);
        gamma_fail(gamma);
        return;
    }
    free(gamma->table);
    gamma->table = table;
    gamma->output->gamma_dirty = true;
    wlr_output_schedule_frame(gamma->output->wlr);
}

static void gamma_destroy(struct wl_client *client, struct wl_resource *resource) {
    wl_resource_destroy(resource);
}

static const struct zwlr_gamma_control_v1_interface gamma_impl = {
    .set_gamma = gamma_set,
    .destroy = gamma_destroy,
};

static struct gamma *gamma_for(struct output *o) {
    struct gamma *gamma;
    wl_list_for_each(gamma, &o->server->gammas, link)
        if (gamma->output == o) return gamma;
    return NULL;
}

static void get_gamma_control(struct wl_client *client, struct wl_resource *manager,
        uint32_t id, struct wl_resource *output_resource) {
    struct tomoe *s = wl_resource_get_user_data(manager);
    struct wl_resource *resource = wl_resource_create(client, &zwlr_gamma_control_v1_interface,
        wl_resource_get_version(manager), id);
    if (!resource) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(resource, &gamma_impl, NULL, gamma_resource_destroy);
    struct wlr_output *wlr = wlr_output_from_resource(output_resource);
    struct output *o = NULL, *candidate;
    wl_list_for_each(candidate, &s->outputs, link) if (candidate->wlr == wlr) o = candidate;
    size_t size = o ? wlr_output_get_gamma_size(wlr) : 0;
    struct gamma *gamma = size && !gamma_for(o) ? calloc(1, sizeof(*gamma)) : NULL;
    if (!gamma) {
        zwlr_gamma_control_v1_send_failed(resource);
        return;
    }
    gamma->resource = resource;
    gamma->output = o;
    gamma->size = size;
    wl_list_insert(&s->gammas, &gamma->link);
    wl_resource_set_user_data(resource, gamma);
    zwlr_gamma_control_v1_send_gamma_size(resource, size);
}

static void manager_destroy(struct wl_client *client, struct wl_resource *resource) {
    wl_resource_destroy(resource);
}

static const struct zwlr_gamma_control_manager_v1_interface manager_impl = {
    .get_gamma_control = get_gamma_control,
    .destroy = manager_destroy,
};

static void bind(struct wl_client *client, void *data, uint32_t version, uint32_t id) {
    struct wl_resource *resource = wl_resource_create(client,
        &zwlr_gamma_control_manager_v1_interface, version, id);
    if (!resource) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(resource, &manager_impl, data, NULL);
}

bool gamma_listen(struct tomoe *s) {
    return wl_global_create(s->display, &zwlr_gamma_control_manager_v1_interface, 1, s, bind);
}

void gamma_apply(struct output *o, struct wlr_output_state *state) {
    o->gamma_dirty = false;
    struct gamma *gamma = gamma_for(o);
    struct wlr_color_transform *transform = gamma && gamma->table ?
        wlr_color_transform_init_lut_3x1d(gamma->size, gamma->table,
            gamma->table + gamma->size, gamma->table + 2 * gamma->size) : NULL;
    if (gamma && gamma->table && !transform) {
        gamma_fail(gamma);
        return;
    }
    wlr_output_state_set_color_transform(state, transform);
    wlr_color_transform_unref(transform);
    if (wlr_output_test_state(o->wlr, state)) return;
    wlr_output_state_set_color_transform(state, NULL);
    state->committed &= ~WLR_OUTPUT_STATE_COLOR_TRANSFORM;
    if (gamma) gamma_fail(gamma);
}

void gamma_output_gone(struct output *o) {
    struct gamma *gamma = gamma_for(o);
    if (!gamma) return;
    free(gamma->table);
    gamma->table = NULL;
    gamma_fail(gamma);
}
