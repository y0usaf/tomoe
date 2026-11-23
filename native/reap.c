#include <signal.h>
#include <sys/pidfd.h>
#include <sys/wait.h>
#include <unistd.h>
#include "internal.h"

enum { REAP_POLL_MS = 100, REAP_RECENT = 8 };

struct reap {
    struct reap *next;
    pid_t pid;
    int pidfd;
    struct wl_event_source *source;
};

static struct reap *reaps;
static struct { pid_t pid; int status; } recent[REAP_RECENT];
static unsigned recent_next;

static bool reap_try(struct reap *entry) {
    int status = 0;
    pid_t done = waitpid(entry->pid, &status, WNOHANG);
    if (done == entry->pid) {
        recent[recent_next % REAP_RECENT].pid = done;
        recent[recent_next % REAP_RECENT].status = status;
        recent_next++;
    }
    return done == entry->pid || (done < 0 && errno == ECHILD);
}

static void reap_free(struct reap *entry) {
    for (struct reap **at = &reaps; *at; at = &(*at)->next) {
        if (*at != entry) continue;
        *at = entry->next;
        break;
    }
    if (entry->source) wl_event_source_remove(entry->source);
    if (entry->pidfd >= 0) close(entry->pidfd);
    free(entry);
}

static int reap_ready(int fd, uint32_t mask, void *data) {
    struct reap *entry = data;
    if (reap_try(entry)) reap_free(entry);
    return 0;
}

static int reap_tick(void *data) {
    struct reap *entry = data;
    if (reap_try(entry)) reap_free(entry);
    else wl_event_source_timer_update(entry->source, REAP_POLL_MS);
    return 0;
}

void reap_later(struct tomoe *s, pid_t pid, int pidfd) {
    if (pid <= 0) {
        if (pidfd >= 0) close(pidfd);
        return;
    }
    kill(pid, SIGKILL);
    struct reap probe = { .pid = pid };
    if (reap_try(&probe) || !s || !s->display) {
        if (pidfd >= 0) close(pidfd);
        return;
    }
    struct reap *entry = calloc(1, sizeof(*entry));
    if (!entry) {
        if (pidfd >= 0) close(pidfd);
        return;
    }
    entry->pid = pid;
    entry->pidfd = pidfd >= 0 ? pidfd : (int)pidfd_open(pid, 0);
    struct wl_event_loop *loop = wl_display_get_event_loop(s->display);
    if (entry->pidfd >= 0)
        entry->source = wl_event_loop_add_fd(loop, entry->pidfd, WL_EVENT_READABLE, reap_ready, entry);
    if (!entry->source) {
        entry->source = wl_event_loop_add_timer(loop, reap_tick, entry);
        if (entry->source) wl_event_source_timer_update(entry->source, REAP_POLL_MS);
    }
    entry->next = reaps;
    reaps = entry;
}

bool reap_pending(pid_t pid) {
    for (struct reap *entry = reaps; entry; entry = entry->next)
        if (entry->pid == pid) return true;
    return false;
}

bool reap_status(pid_t pid, int *status) {
    for (unsigned i = 0; i < REAP_RECENT; i++) {
        if (recent[i].pid != pid) continue;
        *status = recent[i].status;
        return true;
    }
    return false;
}
