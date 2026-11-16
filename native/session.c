#include "internal.h"
#include <fcntl.h>
#include <poll.h>
#include <stddef.h>
#include <stdint.h>
#define listen socket_listen
#include <sys/socket.h>
#undef listen
#include <sys/un.h>
#include <unistd.h>

#define CLIENT_OPEN_SEAT 1
#define CLIENT_CLOSE_SEAT 2
#define CLIENT_OPEN_DEVICE 3
#define CLIENT_CLOSE_DEVICE 4
#define CLIENT_DISABLE_SEAT 5
#define CLIENT_SWITCH_SESSION 6
#define CLIENT_PING 7
#define SERVER_SEAT_OPENED ((1 << 15) + 1)
#define SERVER_SEAT_CLOSED ((1 << 15) + 2)
#define SERVER_DEVICE_OPENED ((1 << 15) + 3)
#define SERVER_DEVICE_CLOSED ((1 << 15) + 4)
#define SERVER_DISABLE_SEAT ((1 << 15) + 5)
#define SERVER_ENABLE_SEAT ((1 << 15) + 6)
#define SERVER_PONG ((1 << 15) + 7)
#define SERVER_SESSION_SWITCHED ((1 << 15) + 8)
#define SERVER_SEAT_DISABLED ((1 << 15) + 9)
#define SERVER_ERROR 0xffff

#define SEAT_PATH "/run/seatd.sock"
#define SEAT_NAME_MAX 64
#define SEAT_PATH_MAX 256
#define SEAT_BUFFER 4096
#define SEAT_EVENTS 8
#define SEAT_FDS 8

struct message_header {
    uint16_t opcode;
    uint16_t size;
};

struct message_open_device {
    uint16_t path_len;
};

struct message_device {
    int device;
};

struct message_session {
    int session;
};

struct message_seat {
    uint16_t name_len;
};

struct message_error {
    int code;
};

struct session {
    struct wl_event_source *source;
    struct tomoe *server;
    struct wl_event_source *test_timer;
    char *test_path;
    bool active;
    bool broken;
    bool pong_outstanding;
    int fd;
    char seat[SEAT_NAME_MAX];
    char input[SEAT_BUFFER];
    size_t input_len;
    int fds[SEAT_FDS];
    unsigned fd_head;
    unsigned fd_tail;
    uint16_t events[SEAT_EVENTS];
    unsigned event_head;
    unsigned event_tail;
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

static void consume(struct session *session, size_t size) {
    memmove(session->input, session->input + size, session->input_len - size);
    session->input_len -= size;
}

static bool available(struct session *session) {
    struct message_header header;
    if (session->input_len < sizeof header) return false;
    memcpy(&header, session->input, sizeof header);
    return session->input_len >= sizeof header + header.size;
}

static bool push_event(struct session *session, uint16_t opcode) {
    if (session->event_tail - session->event_head == SEAT_EVENTS) return false;
    session->events[session->event_tail++ % SEAT_EVENTS] = opcode;
    return true;
}

static uint16_t pop_event(struct session *session) {
    return session->events[session->event_head++ % SEAT_EVENTS];
}

static bool pop_fd(struct session *session, int *fd) {
    if (session->fd_head == session->fd_tail) return false;
    *fd = session->fds[session->fd_head++ % SEAT_FDS];
    return true;
}

static void break_connection(struct session *session, int error) {
    session->broken = true;
    errno = error;
}

static int fill(struct session *session) {
    if (session->input_len == sizeof session->input) {
        break_connection(session, EOVERFLOW);
        return -1;
    }
    struct iovec iov = {
        .iov_base = session->input + session->input_len,
        .iov_len = sizeof session->input - session->input_len,
    };
    char control[CMSG_SPACE(SEAT_FDS * sizeof(int))];
    struct msghdr message = {
        .msg_iov = &iov,
        .msg_iovlen = 1,
        .msg_control = control,
        .msg_controllen = sizeof control,
    };
    ssize_t count;
    do {
        count = recvmsg(session->fd, &message, MSG_CMSG_CLOEXEC | MSG_DONTWAIT);
    } while (count < 0 && errno == EINTR);
    if (count < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
        session->broken = true;
        return -1;
    }
    if (count == 0) {
        break_connection(session, EPIPE);
        return -1;
    }
    for (struct cmsghdr *cmsg = CMSG_FIRSTHDR(&message); cmsg; cmsg = CMSG_NXTHDR(&message, cmsg)) {
        if (cmsg->cmsg_level != SOL_SOCKET || cmsg->cmsg_type != SCM_RIGHTS) continue;
        size_t bytes = cmsg->cmsg_len - CMSG_LEN(0);
        for (size_t offset = 0; offset + sizeof(int) <= bytes; offset += sizeof(int)) {
            int fd;
            memcpy(&fd, CMSG_DATA(cmsg) + offset, sizeof fd);
            if (session->fd_tail - session->fd_head == SEAT_FDS) {
                close(fd);
                break_connection(session, EOVERFLOW);
                return -1;
            }
            session->fds[session->fd_tail++ % SEAT_FDS] = fd;
        }
    }
    session->input_len += (size_t)count;
    return 1;
}

static int send_bytes(struct session *session, const void *data, size_t size) {
    while (size > 0) {
        ssize_t count = send(session->fd, data, size, MSG_NOSIGNAL | MSG_DONTWAIT);
        if (count < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                struct pollfd fd = { .fd = session->fd, .events = POLLOUT };
                int ready;
                do {
                    ready = poll(&fd, 1, -1);
                } while (ready < 0 && errno == EINTR);
                if (ready > 0 && !(fd.revents & (POLLERR | POLLHUP | POLLNVAL))) continue;
                errno = EPIPE;
            }
            session->broken = true;
            return -1;
        }
        data = (const char *)data + count;
        size -= (size_t)count;
    }
    return 0;
}

static int send_request(struct session *session, uint16_t opcode, const void *payload, size_t size) {
    if (size > UINT16_MAX) {
        errno = EINVAL;
        return -1;
    }
    struct message_header header = { .opcode = opcode, .size = (uint16_t)size };
    if (send_bytes(session, &header, sizeof header) < 0) return -1;
    if (size > 0 && send_bytes(session, payload, size) < 0) return -1;
    return 0;
}

static int take_payload(struct session *session, void *target, size_t size) {
    if (session->input_len < size) {
        break_connection(session, EBADMSG);
        return -1;
    }
    if (size > 0) memcpy(target, session->input, size);
    consume(session, size);
    return 0;
}

static int wait_message(struct session *session, uint16_t opcode, void *payload, size_t size,
    bool variable, size_t *length) {
    for (;;) {
        while (available(session)) {
            struct message_header header;
            memcpy(&header, session->input, sizeof header);
            if (header.opcode == SERVER_ENABLE_SEAT || header.opcode == SERVER_DISABLE_SEAT) {
                if (header.size != 0 || !push_event(session, header.opcode)) {
                    break_connection(session, EBADMSG);
                    return -1;
                }
                consume(session, sizeof header);
                continue;
            }
            if (header.opcode == SERVER_PONG) {
                if (header.size != 0) {
                    break_connection(session, EBADMSG);
                    return -1;
                }
                session->pong_outstanding = false;
                consume(session, sizeof header);
                continue;
            }
            if (header.opcode == SERVER_ERROR) {
                struct message_error error;
                if (header.size != sizeof error) {
                    break_connection(session, EBADMSG);
                    return -1;
                }
                consume(session, sizeof header);
                if (take_payload(session, &error, sizeof error) < 0) return -1;
                errno = error.code;
                return -1;
            }
            if (header.opcode != opcode || (variable ? header.size < size : header.size != size)) {
                break_connection(session, EBADMSG);
                return -1;
            }
            consume(session, sizeof header);
            if (variable) {
                *length = header.size;
                return 0;
            }
            return take_payload(session, payload, size);
        }
        int filled = fill(session);
        if (filled < 0) return -1;
        if (filled > 0) continue;
        struct pollfd fd = { .fd = session->fd, .events = POLLIN };
        int ready;
        do {
            ready = poll(&fd, 1, -1);
        } while (ready < 0 && errno == EINTR);
        if (ready <= 0 || fd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
            break_connection(session, EPIPE);
            return -1;
        }
    }
}

static void drain_events(struct session *session) {
    while (available(session)) {
        struct message_header header;
        memcpy(&header, session->input, sizeof header);
        if (header.opcode == SERVER_ENABLE_SEAT || header.opcode == SERVER_DISABLE_SEAT) {
            if (header.size != 0 || !push_event(session, header.opcode)) {
                break_connection(session, EBADMSG);
                return;
            }
        } else if (header.opcode == SERVER_PONG) {
            if (header.size != 0) {
                break_connection(session, EBADMSG);
                return;
            }
            session->pong_outstanding = false;
        } else {
            break_connection(session, EBADMSG);
            return;
        }
        consume(session, sizeof header);
    }
}

static void wake_after_request(struct session *session) {
    int saved = errno;
    drain_events(session);
    if (session->event_head != session->event_tail && !session->broken &&
            !session->pong_outstanding &&
            send_request(session, CLIENT_PING, NULL, 0) == 0)
        session->pong_outstanding = true;
    errno = saved;
}

static int acknowledge_disable(struct session *session) {
    if (send_request(session, CLIENT_DISABLE_SEAT, NULL, 0) < 0) {
        wake_after_request(session);
        return -1;
    }
    int result = wait_message(session, SERVER_SEAT_DISABLED, NULL, 0, false, NULL);
    wake_after_request(session);
    return result;
}

static int run_events(struct session *session) {
    int executed = 0;
    while (session->event_head != session->event_tail) {
        if (pop_event(session) == SERVER_ENABLE_SEAT) {
            set_active(session->server, true);
        } else {
            set_active(session->server, false);
            if (acknowledge_disable(session) < 0 && session->broken) return executed;
        }
        executed++;
    }
    return executed;
}

static int pump(struct session *session) {
    for (;;) {
        while (available(session)) {
            struct message_header header;
            memcpy(&header, session->input, sizeof header);
            if (header.opcode == SERVER_ENABLE_SEAT || header.opcode == SERVER_DISABLE_SEAT) {
                if (header.size != 0 || !push_event(session, header.opcode)) {
                    break_connection(session, EBADMSG);
                    return -1;
                }
            } else if (header.opcode == SERVER_PONG) {
                if (header.size != 0) {
                    break_connection(session, EBADMSG);
                    return -1;
                }
                session->pong_outstanding = false;
            } else {
                break_connection(session, EBADMSG);
                return -1;
            }
            consume(session, sizeof header);
        }
        int filled = fill(session);
        if (filled < 0) return -1;
        if (filled == 0) return 0;
    }
}

static int dispatch(int fd, uint32_t mask, void *data) {
    struct session *session = data;
    if (mask & (WL_EVENT_HANGUP | WL_EVENT_ERROR) || session->broken) {
        session->broken = true;
        fail(session->server, "seatd connection lost");
        return 0;
    }
    if (pump(session) < 0) {
        fail(session->server, "seatd dispatch failed");
        return 0;
    }
    run_events(session);
    if (session->broken) fail(session->server, "seatd connection lost");
    return 0;
}

static int wait_dispatch(struct session *session, int timeout) {
    if (session->broken) {
        errno = ENOTCONN;
        return -1;
    }
    if (pump(session) < 0) return -1;
    int executed = run_events(session);
    if (executed > 0 || timeout == 0) return executed;
    struct pollfd fd = { .fd = session->fd, .events = POLLIN };
    int ready;
    do {
        ready = poll(&fd, 1, timeout);
    } while (ready < 0 && errno == EINTR);
    if (ready < 0) return -1;
    if (ready == 0) return 0;
    if (fd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
        break_connection(session, EPIPE);
        return -1;
    }
    if (pump(session) < 0) return -1;
    return run_events(session);
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

static int connect_seat(struct session *session) {
    union {
        struct sockaddr_un unix;
        struct sockaddr generic;
    } address = {{0}};
    const char *path = getenv("SEATD_SOCK");
    if (!path) path = SEAT_PATH;
    if (strlen(path) >= sizeof address.unix.sun_path) {
        errno = ENAMETOOLONG;
        return -1;
    }
    session->fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (session->fd < 0) return -1;
    address.unix.sun_family = AF_UNIX;
    strncpy(address.unix.sun_path, path, sizeof address.unix.sun_path - 1);
    socklen_t size = offsetof(struct sockaddr_un, sun_path) + strlen(address.unix.sun_path);
    return connect(session->fd, &address.generic, size);
}

static bool open_seat(struct session *session) {
    if (send_request(session, CLIENT_OPEN_SEAT, NULL, 0) < 0) return false;
    size_t size;
    if (wait_message(session, SERVER_SEAT_OPENED, NULL, 0, true, &size) < 0) return false;
    struct message_seat message;
    if (size < sizeof message) {
        break_connection(session, EBADMSG);
        return false;
    }
    if (take_payload(session, &message, sizeof message) < 0) return false;
    if (message.name_len != size - sizeof message || message.name_len == 0 ||
        message.name_len > SEAT_NAME_MAX) {
        break_connection(session, EBADMSG);
        return false;
    }
    if (take_payload(session, session->seat, message.name_len) < 0) return false;
    if (session->seat[message.name_len - 1] != '\0') {
        break_connection(session, EBADMSG);
        return false;
    }
    return true;
}

bool session_create(struct tomoe *s) {
    struct session *session = calloc(1, sizeof(*session));
    if (!session) return false;
    session->server = s;
    session->fd = -1;
    s->session = session;
    if (connect_seat(session) < 0) {
        tomoe_log(LOG_ERROR, "tomoe: seatd could not open a seat: %s", strerror(errno));
        return false;
    }
    if (!open_seat(session)) {
        tomoe_log(LOG_ERROR, "tomoe: seatd could not open a seat: %s", strerror(errno));
        return false;
    }
    session->source = wl_event_loop_add_fd(wl_display_get_event_loop(s->display),
        session->fd, WL_EVENT_READABLE, dispatch, session);
    for (int tries = 0; !session->active && tries < 100; tries++)
        if (wait_dispatch(session, 100) < 0) break;
    if (!session->active) tomoe_log(LOG_ERROR, "tomoe: seatd never activated the seat");
    const char *test = tomoe_test_flag_value("session");
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
    if (session->fd >= 0) {
        if (!session->broken && send_request(session, CLIENT_CLOSE_SEAT, NULL, 0) == 0)
            wait_message(session, SERVER_SEAT_CLOSED, NULL, 0, false, NULL);
        close(session->fd);
    }
    int fd;
    while (pop_fd(session, &fd)) close(fd);
    free(session);
    s->session = NULL;
}

const char *session_seat_name(struct tomoe *s) {
    return s->session->seat;
}

bool session_active(struct tomoe *s) {
    return s->session && s->session->active;
}

int session_open(struct tomoe *s, const char *path, int *device) {
    struct session *session = s->session;
    *device = -1;
    size_t path_len = strlen(path) + 1;
    if (!session || session->broken || path_len > SEAT_PATH_MAX) {
        errno = EINVAL;
        return -1;
    }
    char request[sizeof(struct message_open_device) + SEAT_PATH_MAX];
    struct message_open_device message = { .path_len = (uint16_t)path_len };
    memcpy(request, &message, sizeof message);
    memcpy(request + sizeof message, path, path_len);
    if (send_request(session, CLIENT_OPEN_DEVICE, request, sizeof message + path_len) < 0) {
        wake_after_request(session);
        return -1;
    }
    struct message_device opened;
    if (wait_message(session, SERVER_DEVICE_OPENED, &opened, sizeof opened, false, NULL) < 0) {
        wake_after_request(session);
        return -1;
    }
    int fd;
    if (!pop_fd(session, &fd)) {
        break_connection(session, EBADMSG);
        wake_after_request(session);
        return -1;
    }
    *device = opened.device;
    wake_after_request(session);
    return fd;
}

void session_close(struct tomoe *s, int device, int fd) {
    struct session *session = s->session;
    if (session && !session->broken) {
        struct message_device message = { .device = device };
        if (send_request(session, CLIENT_CLOSE_DEVICE, &message, sizeof message) == 0)
            wait_message(session, SERVER_DEVICE_CLOSED, NULL, 0, false, NULL);
    }
    if (session) wake_after_request(session);
    close(fd);
}

void session_change_vt(struct tomoe *s, int vt) {
    struct session *session = s->session;
    if (!session || session->broken) return;
    struct message_session message = { .session = vt };
    if (send_request(session, CLIENT_SWITCH_SESSION, &message, sizeof message) < 0) {
        wake_after_request(session);
        return;
    }
    wait_message(session, SERVER_SESSION_SWITCHED, NULL, 0, false, NULL);
    wake_after_request(session);
}
