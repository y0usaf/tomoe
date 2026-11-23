#include "desktop-session.h"

#include "policy.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <limits.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include "wayland.h"

struct execution;
struct execution *tomoe_process_start_dependent(char *const argv[], char *const envp[], int *error);
int tomoe_exec_fd(struct execution *job, int which);
int tomoe_exec_poll(struct execution *job);
int tomoe_exec_code(struct execution *job);
int tomoe_exec_stop(struct execution *job, int signal);
int tomoe_exec_release(struct execution *job);

extern char **environ;

struct desktop_bus {
    struct tomoe *server;
    struct execution *job;
    struct loop_source *watch;
    char *path;
    char *own;
    char *transient;
    bool exited;
};

static void set_error(char *error, size_t size, const char *format, ...) {
    if (!error || !size) return;
    va_list args;
    va_start(args, format);
    vsnprintf(error, size, format, args);
    va_end(args);
}

int desktop_free_x_display(char *out, size_t size, char *error, size_t error_size) {
    for (int n = 0; n < 64; n++) {
        char lock[64];
        snprintf(lock, sizeof(lock), "/tmp/.X%d-lock", n);
        FILE *file = fopen(lock, "re");
        bool busy = false;
        if (file) {
            char line[64];
            long pid = 0;
            bool parsed = false;
            if (fgets(line, sizeof(line), file)) {
                char *end = NULL;
                pid = strtol(line, &end, 10);
                parsed = end != line;
            }
            fclose(file);
            if (!parsed) busy = true;
            else if (pid > 0) {
                char proc[64];
                snprintf(proc, sizeof(proc), "/proc/%ld", pid);
                busy = access(proc, F_OK) == 0;
            }
        }
        if (busy) continue;
        int written = snprintf(out, size, ":%d", n);
        if (written < 0 || (size_t)written >= size) break;
        return 0;
    }
    set_error(error, error_size, "No free X display below :64.");
    return -1;
}

static int compare_names(const void *left, const void *right) {
    const char *a = *(const char *const *)left;
    const char *b = *(const char *const *)right;
    return strcmp(a, b);
}

int desktop_parent_wayland_display(char *out, size_t size) {
    const char *display = getenv("WAYLAND_DISPLAY");
    if (display && *display) {
        if (strlen(display) + 1 > size) return -1;
        strcpy(out, display);
        return 0;
    }
    char directory[PATH_MAX];
    char error[256];
    if (desktop_runtime_directory(directory, sizeof(directory), error, sizeof(error))) return -1;
    DIR *dir = opendir(directory);
    if (!dir) return -1;
    char **names = NULL;
    size_t count = 0;
    struct dirent *entry;
    while ((entry = readdir(dir))) {
        size_t length = strlen(entry->d_name);
        if (length <= 8 || strncmp(entry->d_name, "wayland-", 8)) continue;
        bool digits = true;
        for (size_t i = 8; i < length; i++)
            if (entry->d_name[i] < '0' || entry->d_name[i] > '9') digits = false;
        if (!digits) continue;
        char **grown = realloc(names, (count + 1) * sizeof(*names));
        if (!grown) break;
        names = grown;
        names[count] = strdup(entry->d_name);
        if (!names[count]) break;
        count++;
    }
    closedir(dir);
    if (count > 1) qsort(names, count, sizeof(*names), compare_names);
    int found = -1;
    for (size_t i = 0; i < count; i++) {
        char path[PATH_MAX];
        int written = snprintf(path, sizeof(path), "%s/%s", directory, names[i]);
        if (written > 0 && (size_t)written < sizeof(path) && desktop_socket_answering(path)) {
            if (strlen(names[i]) + 1 <= size) {
                strcpy(out, names[i]);
                found = 0;
            }
            break;
        }
    }
    for (size_t i = 0; i < count; i++) free(names[i]);
    free(names);
    return found;
}

int desktop_primary_drm_devices(const char *path, char *out, size_t size, char *error,
        size_t error_size) {
    const char *name = strrchr(path, '/');
    name = name ? name + 1 : path;
    char card[PATH_MAX];
    if (!strncmp(name, "renderD", 7)) {
        char pattern[PATH_MAX];
        snprintf(pattern, sizeof(pattern), "/sys/class/drm/%s/device/drm/card*", name);
        glob_t found;
        memset(&found, 0, sizeof(found));
        if (glob(pattern, 0, NULL, &found) || !found.gl_pathc) {
            globfree(&found);
            set_error(error, error_size, "No card node for %s.", path);
            return -1;
        }
        const char *node = strrchr(found.gl_pathv[0], '/');
        snprintf(card, sizeof(card), "/dev/dri/%s", node ? node + 1 : found.gl_pathv[0]);
        globfree(&found);
    } else if (!realpath(path, card)) {
        set_error(error, error_size, "Cannot resolve %s.", path);
        return -1;
    }
    glob_t cards;
    memset(&cards, 0, sizeof(cards));
    if (glob("/dev/dri/card*", 0, NULL, &cards)) {
        globfree(&cards);
        set_error(error, error_size, "Cannot list /dev/dri.");
        return -1;
    }
    size_t used = 0;
    int written = snprintf(out, size, "%s", card);
    if (written < 0 || (size_t)written >= size) {
        globfree(&cards);
        set_error(error, error_size, "DRM device list is too long.");
        return -1;
    }
    used = (size_t)written;
    for (size_t i = 0; i < cards.gl_pathc; i++) {
        if (!strcmp(cards.gl_pathv[i], card)) continue;
        written = snprintf(out + used, size - used, ":%s", cards.gl_pathv[i]);
        if (written < 0 || (size_t)written >= size - used) {
            globfree(&cards);
            set_error(error, error_size, "DRM device list is too long.");
            return -1;
        }
        used += (size_t)written;
    }
    globfree(&cards);
    return 0;
}

char *desktop_default_config(void) {
    const char *root = getenv("XDG_CONFIG_HOME");
    char directory[PATH_MAX];
    if (root && *root) {
        int written = snprintf(directory, sizeof(directory), "%s", root);
        if (written < 0 || (size_t)written >= sizeof(directory)) return NULL;
    } else {
        const char *home = getenv("HOME");
        if (!home || !*home) return NULL;
        int written = snprintf(directory, sizeof(directory), "%s/.config", home);
        if (written < 0 || (size_t)written >= sizeof(directory)) return NULL;
    }
    size_t length = strlen(directory);
    while (length > 1 && directory[length - 1] == '/') directory[--length] = '\0';
    char path[PATH_MAX];
    int written = snprintf(path, sizeof(path), "%s/tomoe/init.lisp", directory);
    if (written < 0 || (size_t)written >= sizeof(path)) return NULL;
    if (access(path, F_OK)) return NULL;
    return strdup(path);
}

static const char *const session_variables[] = { "WAYLAND_DISPLAY", "DISPLAY",
    "XDG_CURRENT_DESKTOP", "TOMOE_PORTAL_CHOOSER", "TOMOE_SOCKET" };
enum { SESSION_VARIABLE_COUNT = sizeof(session_variables) / sizeof(session_variables[0]) };

static size_t session_variable_list(char *out, size_t size, bool only_set) {
    size_t used = 0;
    out[0] = '\0';
    for (size_t i = 0; i < SESSION_VARIABLE_COUNT; i++) {
        if (only_set && !getenv(session_variables[i])) continue;
        int written = snprintf(out + used, size - used, "%s%s", used ? " " : "",
            session_variables[i]);
        if (written < 0 || (size_t)written >= size - used) break;
        used += (size_t)written;
    }
    return used;
}

static void session_shell(const char *script, bool wait) {
    const char *shell = getenv("TOMOE_SHELL");
    if (!shell || !*shell) shell = "/bin/sh";
    pid_t child = fork();
    if (child < 0) return;
    if (child == 0) {
        sigset_t mask;
        struct sigaction action;
        memset(&action, 0, sizeof(action));
        action.sa_handler = SIG_DFL;
        sigemptyset(&action.sa_mask);
        for (int number = 1; number < NSIG; number++) sigaction(number, &action, NULL);
        sigemptyset(&mask);
        sigprocmask(SIG_SETMASK, &mask, NULL);
        setsid();
        int null = open("/dev/null", O_RDWR);
        if (null >= 0) {
            dup2(null, STDIN_FILENO);
            dup2(null, STDOUT_FILENO);
            dup2(null, STDERR_FILENO);
            if (null > STDERR_FILENO) close(null);
        }
        if (!wait) {
            pid_t grandchild = fork();
            if (grandchild > 0) _exit(0);
            if (grandchild < 0) _exit(127);
        }
        execl(shell, "sh", "-c", script, (char *)NULL);
        _exit(127);
    }
    while (waitpid(child, NULL, 0) < 0 && errno == EINTR) { }
}

void desktop_session_start(void) {
    char listed[512];
    session_variable_list(listed, sizeof(listed), true);
    char script[2048];
    snprintf(script, sizeof(script),
        "hash systemctl 2>/dev/null && systemctl --user import-environment %s; "
        "hash dbus-update-activation-environment 2>/dev/null && "
        "dbus-update-activation-environment %s; exit 0", listed, listed);
    session_shell(script, true);
    session_shell("hash systemctl 2>/dev/null || exit 0; "
        "systemctl --user start tomoe-session.target; "
        "timeout 10 systemctl --user start xdg-desktop-portal-gtk.service; "
        "systemctl --user try-restart xdg-desktop-portal.service", false);
}

void desktop_session_stop(void) {
    char listed[512];
    session_variable_list(listed, sizeof(listed), false);
    char script[2048];
    snprintf(script, sizeof(script),
        "hash systemctl 2>/dev/null || exit 0; "
        "systemctl --user stop tomoe-session.target; "
        "systemctl --user unset-environment %s", listed);
    session_shell(script, true);
}

static void bus_paths(const char *name, char *socket_path, size_t socket_size,
        char *transient, size_t transient_size, char *own, size_t own_size) {
    char directory[PATH_MAX];
    char error[256];
    if (desktop_runtime_directory(directory, sizeof(directory), error, sizeof(error))) {
        socket_path[0] = transient[0] = own[0] = '\0';
        return;
    }
    snprintf(socket_path, socket_size, "%s/tomoe.%s.bus", directory, name);
    snprintf(transient, transient_size, "%s/dbus-1", directory);
    snprintf(own, own_size, "unix:path=%s", socket_path);
}

static void kill_bus_daemons(const char *socket_path) {
    char pattern[PATH_MAX + 32];
    int length = snprintf(pattern, sizeof(pattern), "%c--address=unix:path=%s%c", '\0',
        socket_path, '\0');
    if (length <= 0) return;
    DIR *dir = opendir("/proc");
    if (!dir) return;
    struct dirent *entry;
    while ((entry = readdir(dir))) {
        char *end = NULL;
        long pid = strtol(entry->d_name, &end, 10);
        if (!end || *end || pid <= 0) continue;
        char path[64];
        snprintf(path, sizeof(path), "/proc/%ld/cmdline", pid);
        int fd = open(path, O_RDONLY | O_CLOEXEC);
        if (fd < 0) continue;
        char bytes[65536];
        ssize_t count = read(fd, bytes, sizeof(bytes));
        close(fd);
        if (count <= 0 || count < length) continue;
        if (memmem(bytes, (size_t)count, pattern, (size_t)length))
            kill((pid_t)pid, SIGKILL);
    }
    closedir(dir);
}

static char **environment_with(const char *name, const char *value, char **entry_out) {
    size_t count = 0;
    while (environ && environ[count]) count++;
    char **result = calloc(count + 2, sizeof(*result));
    if (!result) return NULL;
    size_t prefix = strlen(name);
    size_t index = 0;
    for (size_t i = 0; i < count; i++) {
        if (!strncmp(environ[i], name, prefix) && environ[i][prefix] == '=') continue;
        result[index++] = environ[i];
    }
    char *entry = malloc(prefix + strlen(value) + 2);
    if (!entry) {
        free(result);
        return NULL;
    }
    sprintf(entry, "%s=%s", name, value);
    result[index++] = entry;
    result[index] = NULL;
    *entry_out = entry;
    return result;
}

static void bus_ready(void *data, int fd, uint32_t mask) {
    struct desktop_bus *bus = data;
    (void)fd;
    if (mask & (WL_EVENT_READABLE | WL_EVENT_HANGUP | WL_EVENT_ERROR)) bus->exited = true;
}

static void bus_watch(struct desktop_bus *bus) {
    if (!bus->server || !bus->job) return;
    if (bus->watch) loop_remove(bus->watch);
    int fd = tomoe_exec_fd(bus->job, 0);
    bus->watch = fd >= 0 ? loop_watch_fd(bus->server, fd, WL_EVENT_READABLE, bus_ready, bus)
        : NULL;
}

static void bus_reap(struct execution *job) {
    tomoe_exec_stop(job, SIGTERM);
    for (int i = 0; i < 100 && !tomoe_exec_poll(job); i++) {
        struct timespec pause = { .tv_sec = 0, .tv_nsec = 10000000 };
        nanosleep(&pause, NULL);
    }
    if (!tomoe_exec_poll(job)) {
        tomoe_exec_stop(job, SIGKILL);
        for (int i = 0; i < 100 && !tomoe_exec_poll(job); i++) {
            struct timespec pause = { .tv_sec = 0, .tv_nsec = 10000000 };
            nanosleep(&pause, NULL);
        }
    }
    if (tomoe_exec_poll(job) > 0) tomoe_exec_release(job);
}

static bool bus_await(struct execution *job, const char *path, char *error, size_t error_size) {
    for (int i = 0; i < 400; i++) {
        if (desktop_socket_answering(path)) return true;
        int status = tomoe_exec_poll(job);
        if (status > 0) {
            set_error(error, error_size, "dbus-daemon exited with code %d.",
                tomoe_exec_code(job));
            return false;
        }
        struct timespec pause = { .tv_sec = 0, .tv_nsec = 5000000 };
        nanosleep(&pause, NULL);
    }
    set_error(error, error_size, "dbus-daemon is not listening after 2 s.");
    return false;
}

static struct execution *bus_launch(const char *path, const char *own, char *error,
        size_t error_size) {
    char address_argument[PATH_MAX + 192];
    snprintf(address_argument, sizeof(address_argument), "--address=unix:path=%s", path);
    char *argv[] = { "dbus-daemon", "--session", "--nofork", address_argument, NULL };
    char *environment_entry = NULL;
    char **environment = environment_with("DBUS_SESSION_BUS_ADDRESS", own, &environment_entry);
    if (!environment) {
        set_error(error, error_size, "Cannot prepare the bus environment.");
        return NULL;
    }
    int start_error = 0;
    struct execution *job = tomoe_process_start_dependent(argv, environment, &start_error);
    free(environment_entry);
    free(environment);
    if (!job) {
        set_error(error, error_size, "Cannot start dbus-daemon: errno %d.", start_error);
        return NULL;
    }
    if (!bus_await(job, path, error, error_size)) {
        bus_reap(job);
        unlink(path);
        return NULL;
    }
    return job;
}

struct desktop_bus *desktop_bus_start(struct tomoe *server, const char *name, char *error,
        size_t error_size) {
    if (error && error_size) error[0] = '\0';
    char socket_path[PATH_MAX + 128], transient[PATH_MAX + 128], own[PATH_MAX + 192];
    bus_paths(name, socket_path, sizeof(socket_path), transient, sizeof(transient),
        own, sizeof(own));
    if (!socket_path[0]) {
        set_error(error, error_size, "XDG_RUNTIME_DIR is unavailable.");
        return NULL;
    }
    const char *address = getenv("DBUS_SESSION_BUS_ADDRESS");
    if (address && *address && strncmp(address, "disabled:", 9) && strcmp(address, own)) {
        setenv("DBUS_SESSION_BUS_ADDRESS", address, 1);
        return NULL;
    }
    char user[PATH_MAX + 8];
    char directory[PATH_MAX], directory_error[256];
    if (!desktop_runtime_directory(directory, sizeof(directory), directory_error,
            sizeof(directory_error))) {
        snprintf(user, sizeof(user), "%s/bus", directory);
        if (desktop_socket_answering(user)) {
            char value[PATH_MAX + 32];
            snprintf(value, sizeof(value), "unix:path=%s", user);
            setenv("DBUS_SESSION_BUS_ADDRESS", value, 1);
            return NULL;
        }
    }
    bool fresh = access(transient, F_OK) != 0;
    bool orphan = false;
    if (desktop_socket_answering(socket_path)) {
        kill_bus_daemons(socket_path);
        for (int i = 0; i < 200 && desktop_socket_answering(socket_path); i++) {
            struct timespec pause = { .tv_sec = 0, .tv_nsec = 5000000 };
            nanosleep(&pause, NULL);
        }
        orphan = true;
    }
    unlink(socket_path);
    struct execution *job = bus_launch(socket_path, own, error, error_size);
    if (!job) {
        unsetenv("DBUS_SESSION_BUS_ADDRESS");
        return NULL;
    }
    setenv("DBUS_SESSION_BUS_ADDRESS", own, 1);
    struct desktop_bus *bus = calloc(1, sizeof(*bus));
    if (!bus) {
        bus_reap(job);
        unlink(socket_path);
        unsetenv("DBUS_SESSION_BUS_ADDRESS");
        set_error(error, error_size, "Cannot allocate the session bus.");
        return NULL;
    }
    bus->server = server;
    bus->job = job;
    bus->path = strdup(socket_path);
    bus->own = strdup(own);
    bus->transient = (fresh || orphan) ? strdup(transient) : NULL;
    if (!bus->path || !bus->own) {
        bus_reap(job);
        unlink(socket_path);
        unsetenv("DBUS_SESSION_BUS_ADDRESS");
        set_error(error, error_size, "Cannot allocate the session bus.");
        free(bus->path);
        free(bus->own);
        free(bus->transient);
        free(bus);
        return NULL;
    }
    bus_watch(bus);
    return bus;
}

void desktop_bus_service(struct desktop_bus *bus) {
    if (!bus || !bus->job || !bus->exited) return;
    bus->exited = false;
    int status = tomoe_exec_poll(bus->job);
    if (status <= 0) return;
    int code = tomoe_exec_code(bus->job);
    tomoe_exec_release(bus->job);
    bus->job = NULL;
    if (bus->watch) {
        loop_remove(bus->watch);
        bus->watch = NULL;
    }
    unlink(bus->path);
    if (status == 2)
        fprintf(stderr,
            "tomoe: session bus daemon was killed by signal %d; starting a replacement\n",
            code);
    else
        fprintf(stderr,
            "tomoe: session bus daemon exited with status %d; starting a replacement\n",
            code);
    char error[1024];
    struct execution *job = bus_launch(bus->path, bus->own, error, sizeof(error));
    if (!job) {
        fprintf(stderr, "tomoe: session bus unavailable: %s\n", error);
        return;
    }
    bus->job = job;
    bus_watch(bus);
}

void desktop_bus_stop(struct desktop_bus *bus) {
    if (!bus) return;
    if (bus->watch) {
        loop_remove(bus->watch);
        bus->watch = NULL;
    }
    if (bus->job) {
        bus_reap(bus->job);
        bus->job = NULL;
        if (bus->path) unlink(bus->path);
        if (bus->transient) {
            char services[PATH_MAX + 16];
            int written = snprintf(services, sizeof(services), "%s/services", bus->transient);
            if (written > 0 && (size_t)written < sizeof(services)) rmdir(services);
            rmdir(bus->transient);
        }
    }
    free(bus->path);
    free(bus->own);
    free(bus->transient);
    free(bus);
}
