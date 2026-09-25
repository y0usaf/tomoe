#include "internal.h"
#include "ext-background-effect-v1-protocol.h"

struct background_effect {
    struct wl_list link;
    struct wl_resource *resource;
    struct surface *surface;
    pixman_region32_t pending, current;
    bool pending_active, current_active, dirty;
    struct wl_listener commit, surface_destroy;
};

static struct background_effect *effect_from_resource(struct wl_resource *resource) {
    return wl_resource_get_user_data(resource);
}

static void effect_detach(struct background_effect *effect) {
    detach(&effect->commit);
    detach(&effect->surface_destroy);
    effect->surface = NULL;
}

static void effect_commit(struct wl_listener *listener, void *data) {
    struct background_effect *effect = wl_container_of(listener, effect, commit);
    if (!effect->dirty) return;
    effect->dirty = false;
    effect->current_active = effect->pending_active;
    pixman_region32_copy(&effect->current, &effect->pending);
}

static void effect_surface_destroy(struct wl_listener *listener, void *data) {
    struct background_effect *effect = wl_container_of(listener, effect, surface_destroy);
    effect_detach(effect);
}

static void effect_resource_destroy(struct wl_resource *resource) {
    struct background_effect *effect = effect_from_resource(resource);
    effect_detach(effect);
    wl_list_remove(&effect->link);
    pixman_region32_fini(&effect->pending);
    pixman_region32_fini(&effect->current);
    free(effect);
}

static void effect_destroy(struct wl_client *client, struct wl_resource *resource) {
    wl_resource_destroy(resource);
}

static void effect_set_blur_region(struct wl_client *client, struct wl_resource *resource,
        struct wl_resource *region) {
    struct background_effect *effect = effect_from_resource(resource);
    if (!effect->surface) {
        wl_resource_post_error(resource, EXT_BACKGROUND_EFFECT_SURFACE_V1_ERROR_SURFACE_DESTROYED,
            "the surface has been destroyed");
        return;
    }
    effect->pending_active = region != NULL;
    if (region) pixman_region32_copy(&effect->pending, region_from_resource(region));
    else pixman_region32_clear(&effect->pending);
    effect->dirty = true;
}

static const struct ext_background_effect_surface_v1_interface effect_impl = {
    .destroy = effect_destroy,
    .set_blur_region = effect_set_blur_region,
};

static void manager_destroy(struct wl_client *client, struct wl_resource *resource) {
    wl_resource_destroy(resource);
}

static void manager_get(struct wl_client *client, struct wl_resource *resource,
        uint32_t id, struct wl_resource *surface_resource) {
    struct tomoe *s = wl_resource_get_user_data(resource);
    struct surface *surface = surface_from_resource(surface_resource);
    struct background_effect *effect;
    wl_list_for_each(effect, &s->background_effects, link) {
        if (effect->surface != surface) continue;
        wl_resource_post_error(resource, EXT_BACKGROUND_EFFECT_MANAGER_V1_ERROR_BACKGROUND_EFFECT_EXISTS,
            "the surface already has a background effect");
        return;
    }
    effect = calloc(1, sizeof(*effect));
    if (!effect) { wl_client_post_no_memory(client); return; }
    effect->resource = wl_resource_create(client, &ext_background_effect_surface_v1_interface,
        wl_resource_get_version(resource), id);
    if (!effect->resource) { free(effect); wl_client_post_no_memory(client); return; }
    effect->surface = surface;
    pixman_region32_init(&effect->pending);
    pixman_region32_init(&effect->current);
    wl_resource_set_implementation(effect->resource, &effect_impl, effect, effect_resource_destroy);
    listen(&effect->commit, &surface->events.commit, effect_commit);
    listen(&effect->surface_destroy, &surface->events.destroy, effect_surface_destroy);
    wl_list_insert(&s->background_effects, &effect->link);
}

static const struct ext_background_effect_manager_v1_interface manager_impl = {
    .destroy = manager_destroy,
    .get_background_effect = manager_get,
};

static void manager_bind(struct wl_client *client, void *data, uint32_t version, uint32_t id) {
    struct wl_resource *resource = wl_resource_create(client,
        &ext_background_effect_manager_v1_interface, version, id);
    if (!resource) { wl_client_post_no_memory(client); return; }
    wl_resource_set_implementation(resource, &manager_impl, data, NULL);
    ext_background_effect_manager_v1_send_capabilities(resource,
        EXT_BACKGROUND_EFFECT_MANAGER_V1_CAPABILITY_BLUR);
}

bool background_effects_listen(struct tomoe *s) {
    return wl_global_create(s->display, &ext_background_effect_manager_v1_interface, 1, s,
        manager_bind) != NULL;
}

const pixman_region32_t *background_blur_region(struct tomoe *s, struct surface *surface) {
    struct background_effect *effect;
    wl_list_for_each(effect, &s->background_effects, link)
        if (effect->surface == surface && effect->current_active) return &effect->current;
    return NULL;
}
