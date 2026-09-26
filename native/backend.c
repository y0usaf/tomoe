#include "internal.h"
#include "ui.h"

#include <malloc.h>
#include <unistd.h>

static int backend_open(struct tomoe *s) {
    const char *kind = getenv("TOMOE_BACKEND");
    if (kind && !strcmp(kind, "headless")) {
        s->backend = SCREEN_HEADLESS;
        return -1;
    }
    if (kind && !strcmp(kind, "nested")) {
        s->backend = SCREEN_NESTED;
        return nested_create(s);
    }
    s->backend = SCREEN_DRM;
    if (!session_create(s)) return -2;
    int fd = kms_create(s);
    return fd < 0 ? -2 : fd;
}

static bool backend_start(struct tomoe *s) {
    if (s->backend == SCREEN_HEADLESS) return headless_start(s);
    if (s->backend == SCREEN_NESTED) return nested_start(s);
    kms_start(s);
    return true;
}
int tomoe_abi_version(void) { return 31; }
static bool create_scene_trees(struct tomoe *s) {
    s->layer_tree[ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND] = node_create(s->scene);
    s->layer_tree[ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM] = node_create(s->scene);
    s->window_tree = node_create(s->scene);
    s->layer_tree[ZWLR_LAYER_SHELL_V1_LAYER_TOP] = node_create(s->scene);
    s->fullscreen_tree = node_create(s->scene);
    s->layer_tree[ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY] = node_create(s->scene);
    s->drag_icon_tree = node_create(s->scene);
    for (int i = 0; i < 4; i++) {
        if (!s->layer_tree[i]) return false;
    }
    return s->window_tree && s->fullscreen_tree && s->drag_icon_tree;
}
struct tomoe *tomoe_create(const char *socket_name) {
    log_verbosity = getenv("TOMOE_DEBUG") ? LOG_DEBUG : LOG_ERROR;
    mallopt(M_MMAP_THRESHOLD, 128 * 1024);
    struct tomoe *s = calloc(1, sizeof(*s));
    if (!s) return NULL;
    wl_list_init(&s->windows); wl_list_init(&s->layers); wl_list_init(&s->outputs);
    wl_list_init(&s->keyboards); wl_list_init(&s->input_devices); wl_list_init(&s->events); wl_list_init(&s->bindings);
    wl_list_init(&s->activation_tokens);
    wl_list_init(&s->surfaces);
    wl_list_init(&s->feedbacks);
    wl_list_init(&s->decorations);
    wl_list_init(&s->constraints);
    wl_list_init(&s->relative_pointers);
    wl_list_init(&s->lock_surfaces);
    wl_list_init(&s->foreigns);
    wl_list_init(&s->foreign_managers);
    wl_list_init(&s->foreign_lists);
    wl_list_init(&s->virtual_pointers);
    wl_list_init(&s->background_effects);
    wl_list_init(&s->copy_frames);
    wl_list_init(&s->capture_sessions);
    wl_list_init(&s->idle_notifications);
    wl_list_init(&s->idle_inhibitors);
    wl_list_init(&s->gammas);
    wl_list_init(&s->powers);
    wl_list_init(&s->tearings);
    wl_list_init(&s->activation_pending);
    wl_list_init(&s->libinput_fds);
    s->next_binding_id = 1;
    s->next_device_id = 1;
    s->next_output_id = 1;
    s->next_ui_callback_id = 1;
    s->view_zoom = 1.0;
    settings_default(&s->settings);
    s->settings.nested_width = s->settings.nested_height = 0;
    s->display = wl_display_create();
    if (!s->display) goto failed;
    int drm_fd = backend_open(s);
    if (drm_fd == -2) goto failed;
    s->renderer = render_create(drm_fd);
    if (s->backend == SCREEN_NESTED && drm_fd >= 0) close(drm_fd);
    if (!s->renderer || !buffers_listen(s)) goto failed;
    if (!capture_listen(s)) goto failed;
    if (!surfaces_listen(s)) goto failed;
    s->scene = node_create(NULL);
    if (!s->scene || !screens_listen(s)) goto failed;
    if (!create_scene_trees(s)) goto failed;
    s->seat = seat_create(s);
    if (!s->seat ||
            !selection_listen(s) || !activation_listen(s) || !xdg_shell_listen(s) ||
            !layer_shell_listen(s)) goto failed;
    input_listen(s);
    if (!libinput_listen(s)) goto failed;
    if (!virtual_input_listen(s) || !protocols_listen(s) || !lock_listen(s) ||
            !background_effects_listen(s)) goto failed;
    if (wl_display_add_socket(s->display, socket_name) < 0) goto failed;
    s->running = true;
    if (!backend_start(s) || s->failed) goto failed;
    return s;
failed:
    tomoe_log(LOG_ERROR, "tomoe: backend startup failed");
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
    xdg_shell_finish(s);
    if (s->display) wl_display_destroy_clients(s->display);
    if (s->display) surfaces_finish(s);
    detach(&s->cursor_surface_destroy);
    if (s->scene) node_destroy(s->scene);
    cursor_finish(s);
    if (s->default_cursor) buffer_drop(s->default_cursor);
    libinput_finish(s);
    headless_finish(s);
    nested_finish(s);
    kms_destroy(s);
    session_finish(s);
    keyboard_logical_finish(s);
    seat_destroy(s->seat);
    settings_finish(&s->settings);
    if (s->render_timeline) timeline_unref(s->render_timeline);
    if (s->renderer) render_destroy(s->renderer);
    if (s->display) wl_display_destroy(s->display);
    buffers_finish();
    tomoe_clear_bindings(s);
    free(s->ui_hovered);
    free(s->grab_owner);
    free(s->grab_otherwise);
    while (tomoe_next_event(s)) { }
    free(s->hit_result);
    free(s->frames_result);
    free(s->output_preview);
    free(s->output_current);
    layers_preview_finish(s);
    presentation_finish(s);
    keyboard_profile_finish(s->keyboard_profile);
    free(s);
}
