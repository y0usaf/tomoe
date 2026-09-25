#include "internal.h"
#include "ui.h"
#include <wlr/render/drm_syncobj.h>

static void backend_destroy(struct wl_listener *listener, void *data) {
    struct tomoe *s = wl_container_of(listener, s, backend_destroy);
    detach(&s->new_input); detach(&s->new_output); detach(&s->backend_destroy);
    wl_list_init(&s->new_input.link); wl_list_init(&s->new_output.link);
    wl_list_init(&s->backend_destroy.link);
    s->backend = NULL; s->running = false;
}
int tomoe_abi_version(void) { return 28; }
static bool create_scene_trees(struct tomoe *s) {
    s->layer_tree[ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND] = wlr_scene_tree_create(&s->scene->tree);
    s->layer_tree[ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM] = wlr_scene_tree_create(&s->scene->tree);
    s->window_tree = wlr_scene_tree_create(&s->scene->tree);
    s->layer_tree[ZWLR_LAYER_SHELL_V1_LAYER_TOP] = wlr_scene_tree_create(&s->scene->tree);
    s->fullscreen_tree = wlr_scene_tree_create(&s->scene->tree);
    s->layer_tree[ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY] = wlr_scene_tree_create(&s->scene->tree);
    s->drag_icon_tree = wlr_scene_tree_create(&s->scene->tree);
    for (int i = 0; i < 4; i++) {
        if (!s->layer_tree[i]) return false;
    }
    return s->window_tree && s->fullscreen_tree && s->drag_icon_tree;
}
struct tomoe *tomoe_create(const char *socket_name) {
    wlr_log_init(WLR_ERROR, NULL);
    struct tomoe *s = calloc(1, sizeof(*s));
    if (!s) return NULL;
    wl_list_init(&s->windows); wl_list_init(&s->layers); wl_list_init(&s->outputs);
    wl_list_init(&s->keyboards); wl_list_init(&s->input_devices); wl_list_init(&s->events); wl_list_init(&s->bindings);
    wl_list_init(&s->tracked_surfaces);
    wl_list_init(&s->virtual_pointers);
    wl_list_init(&s->activation_tokens);
    wl_list_init(&s->activation_pending);
    s->next_binding_id = 1;
    s->next_device_id = 1;
    s->next_output_id = 1;
    s->next_ui_callback_id = 1;
    s->view_zoom = 1.0;
    settings_default(&s->settings);
    s->settings.nested_width = s->settings.nested_height = 0;
    s->display = wl_display_create();
    if (!s->display) goto failed;
    s->backend = wlr_backend_autocreate(wl_display_get_event_loop(s->display), &s->session);
    if (!s->backend) goto failed;
    s->renderer = render_create(s->backend);
    if (!s->renderer || !wlr_renderer_init_wl_display(s->renderer, s->display)) goto failed;
    s->allocator = render_allocator(s->renderer);
    if (!capture_listen(s)) goto failed;
    struct wlr_compositor *compositor = wlr_compositor_create(s->display, 6, s->renderer);
    if (!compositor ||
            !wlr_subcompositor_create(s->display) || !wlr_data_device_manager_create(s->display) ||
            !wlr_viewporter_create(s->display) ||
            !wlr_fractional_scale_manager_v1_create(s->display, 1)) goto failed;
    s->layout = wlr_output_layout_create(s->display);
    s->scene = wlr_scene_create();
    if (!s->layout || !s->scene || !wlr_xdg_output_manager_v1_create(s->display, s->layout)) goto failed;
    if (!create_scene_trees(s)) goto failed;
    s->cursor = wlr_cursor_create();
    const char *cursor_size = getenv("XCURSOR_SIZE");
    int size = cursor_size ? atoi(cursor_size) : 0;
    s->cursor_manager = wlr_xcursor_manager_create(getenv("XCURSOR_THEME"), size > 0 ? size : 24);
    s->seat = wlr_seat_create(s->display, "seat0");
    struct wlr_xdg_shell *shell = wlr_xdg_shell_create(s->display, 3);
    struct wlr_layer_shell_v1 *layer_shell = wlr_layer_shell_v1_create(s->display, 4);
    if (!s->cursor || !s->cursor_manager || !s->seat ||
            !activation_listen(s) || !shell || !layer_shell) goto failed;
    surfaces_listen(s, compositor);
    wlr_cursor_attach_output_layout(s->cursor, s->layout);
    outputs_listen(s);
    input_listen(s);
    if (!virtual_pointers_listen(s) || !protocols_listen(s) || !lock_listen(s) ||
            !background_effects_listen(s)) goto failed;
    listen(&s->backend_destroy, &s->backend->events.destroy, backend_destroy);
    windows_listen(s, shell);
    layers_listen(s, layer_shell);
    if (wl_display_add_socket(s->display, socket_name) < 0) goto failed;
    s->running = true;
    if (!wlr_backend_start(s->backend) || s->failed) goto failed;
    return s;
failed:
    wlr_log(WLR_ERROR, "tomoe: backend startup failed");
    tomoe_destroy(s);
    return NULL;
}
int tomoe_step(struct tomoe *s, int timeout_ms) {
    if (s->failed) return -1;
    if (!s->running) return 1;
    refresh_scene(s);
    wl_display_flush_clients(s->display);
    int status = wl_event_loop_dispatch(wl_display_get_event_loop(s->display), timeout_ms);
    if (status < 0 && errno != EINTR) return -1;
    keyboard_sync_leds(s);
    refresh_scene(s);
    idle_refresh(s);
    foreign_toplevels_refresh(s);
    lock_refresh(s);
    wl_display_flush_clients(s->display);
    return s->failed ? -1 : (s->running ? 0 : 1);
}
void tomoe_destroy(struct tomoe *s) {
    if (!s) return;
    s->stopping = true;
    finish_captures(s);
    presentation_finish(s);
    lock_finish(s);
    screenshot_finish(s);
    effects_finish(s);
    ui_input_finish(s);
    ui_finish(s);
    activation_finish(s);
    if (s->display) wl_display_destroy_clients(s->display);
    struct wl_listener *listeners[] = {
        &s->new_output, &s->new_input, &s->new_toplevel, &s->new_popup, &s->new_layer_surface,
        &s->motion, &s->absolute, &s->button, &s->axis, &s->frame,
        &s->request_cursor, &s->pointer_focus, &s->selection, &s->layout_change, &s->backend_destroy,
        &s->new_surface,
        &s->new_virtual_pointer, &s->new_virtual_keyboard, &s->cursor_surface_destroy,
        &s->request_set_primary_selection, &s->new_constraint,
        &s->constraint_commit, &s->constraint_destroy,
        &s->request_start_drag, &s->seat_start_drag,
    };
    for (size_t i = 0; i < sizeof(listeners) / sizeof(listeners[0]); i++) detach(listeners[i]);
    if (s->scene) wlr_scene_node_destroy(&s->scene->tree.node);
    if (s->cursor_manager) wlr_xcursor_manager_destroy(s->cursor_manager);
    if (s->cursor) wlr_cursor_destroy(s->cursor);
    if (s->backend) wlr_backend_destroy(s->backend);
    keyboard_logical_finish(s);
    settings_finish(&s->settings);
    if (s->render_timeline) wlr_drm_syncobj_timeline_unref(s->render_timeline);
    if (s->allocator) wlr_allocator_destroy(s->allocator);
    if (s->renderer) wlr_renderer_destroy(s->renderer);
    if (s->display) wl_display_destroy(s->display);
    tomoe_clear_bindings(s);
    free(s->ui_hovered);
    free(s->grab_owner);
    free(s->grab_otherwise);
    while (tomoe_next_event(s)) { }
    free(s->hit_result);
    free(s->output_preview);
    free(s->output_current);
    layers_preview_finish(s);
    presentation_finish(s);
    keyboard_profile_finish(s->keyboard_profile);
    free(s);
}
