#include "internal.h"
#include <fcntl.h>
#include <libseat.h>
#include <unistd.h>

struct session {
    struct libseat *seat;
    struct wl_event_source *source;
    struct tomoe *server;
    struct wl_event_source *test_timer;
    char *test_path;
    bool active;
};

static void set_active(struct tomoe *s, bool active) {
    struct session *session = s->session;
    if (!session || session->active == active) return;
    session->active = active;
    tomoe_log(LOG_WARNING, "tomoe: the seat is %s; %s", active ? "active" : "inactive",
        active ? "outputs draw" : "outputs stop drawing");
    if (active) {
        kms_resume(s);
        libinput_active(s, true);
    } else {
        kms_pause(s);
        libinput_active(s, false);
    }
}

static void enable_seat(struct libseat *seat, void *data) {
    set_active(data, true);
}

static void disable_seat(struct libseat *seat, void *data) {
    set_active(data, false);
    libseat_disable_seat(seat);
}

static int test_session_poll(void *data) {
    struct session *session = data;
    char text[16] = {0};
    wl_event_source_timer_update(session->test_timer, 50);
    int fd = open(session->test_path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return 0;
    ssize_t count = read(fd, text, sizeof(text) - 1);
    close(fd);
    if (count > 0) set_active(session->server, strncmp(text, "inactive", 8) != 0);
    return 0;
}

static struct libseat_seat_listener listener = {
    .enable_seat = enable_seat,
    .disable_seat = disable_seat,
};

static int dispatch(int fd, uint32_t mask, void *data) {
    struct tomoe *s = data;
    if (libseat_dispatch(s->session->seat, 0) < 0) fail(s, "libseat dispatch failed");
    return 0;
}

bool session_create(struct tomoe *s) {
    struct session *session = calloc(1, sizeof(*session));
    if (!session) return false;
    session->server = s;
    s->session = session;
    libseat_set_log_level(LIBSEAT_LOG_LEVEL_ERROR);
    session->seat = libseat_open_seat(&listener, s);
    if (!session->seat) {
        tomoe_log(LOG_ERROR, "tomoe: libseat could not open a seat");
        return false;
    }
    session->source = wl_event_loop_add_fd(wl_display_get_event_loop(s->display),
        libseat_get_fd(session->seat), WL_EVENT_READABLE, dispatch, s);
    for (int tries = 0; !session->active && tries < 100; tries++)
        if (libseat_dispatch(session->seat, 100) < 0) break;
    if (!session->active) tomoe_log(LOG_ERROR, "tomoe: libseat never activated the seat");
    const char *test = getenv("TOMOE_TEST_SESSION");
    if (test && *test && (session->test_path = strdup(test))) {
        session->test_timer = wl_event_loop_add_timer(wl_display_get_event_loop(s->display),
            test_session_poll, session);
        if (session->test_timer) wl_event_source_timer_update(session->test_timer, 50);
    }
    return session->source && session->active;
}

void session_finish(struct tomoe *s) {
    struct session *session = s->session;
    if (!session) return;
    if (session->source) wl_event_source_remove(session->source);
    if (session->test_timer) wl_event_source_remove(session->test_timer);
    free(session->test_path);
    if (session->seat) libseat_dispatch(session->seat, 0);
    if (session->seat) libseat_close_seat(session->seat);
    free(session);
    s->session = NULL;
}

const char *session_seat_name(struct tomoe *s) {
    return libseat_seat_name(s->session->seat);
}

bool session_active(struct tomoe *s) {
    return s->session && s->session->active;
}

int session_open(struct tomoe *s, const char *path, int *device) {
    int fd = -1;
    *device = libseat_open_device(s->session->seat, path, &fd);
    return *device < 0 ? -1 : fd;
}

void session_close(struct tomoe *s, int device, int fd) {
    libseat_close_device(s->session->seat, device);
    close(fd);
}

void session_change_vt(struct tomoe *s, int vt) {
    if (s->session) libseat_switch_session(s->session->seat, vt);
}
