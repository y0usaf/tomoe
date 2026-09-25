#include "internal.h"

static struct node *node_alloc(struct node *parent) {
    struct node *node = calloc(1, sizeof(*node));
    if (!node) return NULL;
    node->enabled = true;
    wl_signal_init(&node->events.destroy);
    wl_list_init(&node->children);
    wl_list_init(&node->link);
    node->parent = parent;
    if (parent) wl_list_insert(parent->children.prev, &node->link);
    return node;
}

struct node *node_create(struct node *parent) {
    return node_alloc(parent);
}

static void surface_destroyed(struct wl_listener *listener, void *data) {
    struct node *node = wl_container_of(listener, node, surface_destroy);
    node_destroy(node);
}

struct node *node_surface_create(struct node *parent, struct surface *surface) {
    struct node *node = node_alloc(parent);
    if (!node) return NULL;
    node->surface = surface;
    listen(&node->surface_destroy, &surface->events.destroy, surface_destroyed);
    return node;
}

void node_destroy(struct node *node) {
    if (!node) return;
    wl_signal_emit_mutable(&node->events.destroy, node);
    struct node *child, *next;
    wl_list_for_each_safe(child, next, &node->children, link) node_destroy(child);
    detach(&node->surface_destroy);
    wl_list_remove(&node->link);
    free(node);
}

void node_set_enabled(struct node *node, bool enabled) {
    node->enabled = enabled;
}

void node_set_position(struct node *node, int x, int y) {
    node->x = x;
    node->y = y;
}

void node_raise_to_top(struct node *node) {
    if (!node->parent) return;
    wl_list_remove(&node->link);
    wl_list_insert(node->parent->children.prev, &node->link);
}

void node_reparent(struct node *node, struct node *parent) {
    wl_list_remove(&node->link);
    node->parent = parent;
    wl_list_insert(parent->children.prev, &node->link);
}
