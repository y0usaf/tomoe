#include "internal.h"
#include "ui.h"

#include <fcntl.h>
#include <malloc.h>
#include <signal.h>
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
int tomoe_abi_version(void) { return 42; }

void tomoe_trim_heap(void) {
    malloc_trim(0);
}

static int stop_pipe[2] = { -1, -1 };
static struct sigaction stop_previous[2];

static void stop_write(void) {
    char byte = 0;
    if (stop_pipe[1] >= 0) {
        ssize_t count = write(stop_pipe[1], &byte, 1);
        (void)count;
    }
}

static void stop_forward(int signal, siginfo_t *info, void *context,
        const struct sigaction *previous) {
    if (previous->sa_flags & SA_SIGINFO) previous->sa_sigaction(signal, info, context);
    else if (previous->sa_handler == SIG_DFL) raise(signal);
    else if (previous->sa_handler != SIG_IGN) previous->sa_handler(signal);
}

static void stop_term(int signal, siginfo_t *info, void *context) {
    stop_write();
    stop_forward(signal, info, context, &stop_previous[0]);
}

static void stop_int(int signal, siginfo_t *info, void *context) {
    stop_write();
    stop_forward(signal, info, context, &stop_previous[1]);
}

static int stop_drain(int fd, uint32_t mask, void *data) {
    char buffer[64];
    (void)mask; (void)data;
    while (read(fd, buffer, sizeof(buffer)) > 0) { }
    return 0;
}

int tomoe_watch_signals(struct tomoe *s) {
    if (!s || !s->display) return -1;
    if (stop_pipe[0] >= 0) return 0;
    if (pipe2(stop_pipe, O_CLOEXEC | O_NONBLOCK) < 0) return -errno;
    if (!wl_event_loop_add_fd(wl_display_get_event_loop(s->display), stop_pipe[0],
            WL_EVENT_READABLE, stop_drain, NULL)) return -ENOMEM;
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    sigfillset(&action.sa_mask);
    action.sa_flags = SA_SIGINFO;
    action.sa_sigaction = stop_term;
    if (sigaction(SIGTERM, &action, &stop_previous[0]) < 0) {
        stop_pipe[1] = stop_pipe[0] = -1;
        return -errno;
    }
    action.sa_sigaction = stop_int;
    if (sigaction(SIGINT, &action, &stop_previous[1]) < 0) return -errno;
    return 0;
}
struct fd_watch {
    struct tomoe *server;
    struct wl_event_source *source;
    int fd;
};
static int watch_died(int fd, uint32_t mask, void *data) {
    struct fd_watch *watch = data;
    (void)fd;
    if (!(mask & (WL_EVENT_HANGUP | WL_EVENT_ERROR))) return 0;
    if (watch->source) {
        wl_event_source_remove(watch->source);
        watch->source = NULL;
    }
    if (watch->server && watch->server->dead_watch_count <
            sizeof(watch->server->dead_watches) / sizeof(watch->server->dead_watches[0]))
        watch->server->dead_watches[watch->server->dead_watch_count++] = (uintptr_t)watch;
    return 0;
}
uintptr_t tomoe_watch_fd(struct tomoe *s, int fd, uint32_t mask) {
    if (!s || !s->display || fd < 0) return 0;
    struct fd_watch *watch = calloc(1, sizeof(*watch));
    if (!watch) return 0;
    watch->server = s;
    watch->fd = fd;
    watch->source = wl_event_loop_add_fd(wl_display_get_event_loop(s->display), fd,
        mask ? mask : WL_EVENT_READABLE, watch_died, watch);
    if (!watch->source) {
        free(watch);
        return 0;
    }
    return (uintptr_t)watch;
}
uintptr_t tomoe_dead_watch(struct tomoe *s) {
    if (!s || !s->dead_watch_count) return 0;
    uintptr_t watch = s->dead_watches[0];
    memmove(s->dead_watches, s->dead_watches + 1,
        (s->dead_watch_count - 1) * sizeof(s->dead_watches[0]));
    s->dead_watch_count--;
    return watch;
}
void tomoe_update_fd(struct tomoe *s, uintptr_t handle, uint32_t mask) {
    struct fd_watch *watch = (struct fd_watch *)handle;
    if (!s || !watch || !watch->source) return;
    wl_event_source_fd_update(watch->source, mask ? mask : WL_EVENT_READABLE);
}
void tomoe_unwatch_fd(struct tomoe *s, uintptr_t handle) {
    struct fd_watch *watch = (struct fd_watch *)handle;
    if (!s || !watch) return;
    if (watch->source) wl_event_source_remove(watch->source);
    free(watch);
}
int tomoe_frames_waiting(struct tomoe *s) {
    return s && s->renderer && render_frames_held(s->renderer);
}
static void wayland_log(const char *fmt, va_list args) {
    char line[512];
    vsnprintf(line, sizeof(line), fmt, args);
    line[strcspn(line, "\n")] = '\0';
    tomoe_log(LOG_ERROR, "wayland: %s", line);
}
static bool create_scene_trees(struct tomoe *s) {
    s->layer_tree[ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND] = node_create(s->scene);
    s->layer_tree[ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM] = node_create(s->scene);
    s->window_tree = node_create(s->scene);
    s->layer_tree[ZWLR_LAYER_SHELL_V1_LAYER_TOP] = node_create(s->scene);
    s->fullscreen_tree = node_create(s->scene);
    s->popup_tree = node_create(s->scene);
    s->layer_tree[ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY] = node_create(s->scene);
    s->drag_icon_tree = node_create(s->scene);
    for (int i = 0; i < 4; i++) {
        if (!s->layer_tree[i]) return false;
    }
    return s->window_tree && s->fullscreen_tree && s->popup_tree && s->drag_icon_tree;
}
struct tomoe *tomoe_create(const char *socket_name) {
    log_verbosity = getenv("TOMOE_DEBUG") ? LOG_DEBUG : LOG_ERROR;
    wl_log_set_handler_server(wayland_log);
    mallopt(M_MMAP_THRESHOLD, 128 * 1024);
    signal(SIGPIPE, SIG_IGN);
    struct tomoe *s = calloc(1, sizeof(*s));
    if (!s) return NULL;
    wl_list_init(&s->windows); wl_list_init(&s->ghosts); wl_list_init(&s->layers); wl_list_init(&s->outputs);
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
    wl_list_init(&s->copy_damage);
    wl_list_init(&s->capture_sessions);
    wl_list_init(&s->cursor_sessions);
    wl_list_init(&s->idle_notifications);
    wl_list_init(&s->idle_inhibitors);
    wl_list_init(&s->gammas);
    timing_init();
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
            !background_effects_listen(s) || !xwayland_listen(s)) goto failed;
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
    if (s->renderer) render_retire_fences(s->renderer);
    if (s->outputs_deferred && !render_frames_held(s->renderer)) {
        s->outputs_deferred = false;
        const char *error = tomoe_outputs_apply(s);
        if (error) tomoe_log(LOG_ERROR, "tomoe: %s", error);
    }
    refresh_scene(s);
    screens_frame_after_events(s);
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
    xwayland_finish(s);
    sound_finish(s);
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
    window_shaders_finish(s);
    window_meshes_finish(s);
    if (s->renderer) render_destroy(s->renderer);
    if (s->grab_release) wl_event_source_remove(s->grab_release);
    if (s->display) wl_display_destroy(s->display);
    buffers_finish();
    tomoe_clear_bindings(s);
    free(s->ui_hovered);
    free(s->grab_owner);
    free(s->grab_otherwise);
    while (tomoe_next_event(s)) { }
    free(s->hit_result);
    free(s->frames_result);
    free(s->memory_result);
    free(s->animation_result);
    free(s->cursor_result);
    free(s->output_preview);
    free(s->output_current);
    layers_preview_finish(s);
    presentation_finish(s);
    keyboard_profile_finish(s->keyboard_profile);
    free(s);
}
