#include "harness.h"

#include <ctype.h>
#include <dirent.h>
#include <poll.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <strings.h>

#define SHAPE_COUNT 36
#define ARROW_WIDTH 12
#define ARROW_HEIGHT 16
#define PARK_X 400
#define PARK_Y 380

struct point {
    int x;
    int y;
};

struct compositor {
    char work[4096];
    char policy[4096];
    char log_path[4096];
    struct harness_env env;
    pid_t pid;
    int running;
    int log_open;
};

struct outcome {
    int status;
    int timed_out;
    char *out;
    char *err;
};

struct signature {
    long count;
    long long *offsets;
};

struct image {
    int width;
    int height;
    unsigned char *storage;
    unsigned char *data;
    size_t length;
};

struct shots {
    char **paths;
    size_t count;
    size_t capacity;
};

struct mover {
    pid_t pid;
    int output;
    int errors;
};

struct capture {
    struct compositor *compositor;
    struct shots *shots;
    atomic_int stop;
};

typedef void (*expect_fn)(struct compositor *, const char *, const char *, struct shots *,
        const char *);

static const char *const shapes[SHAPE_COUNT] = {
    "default", "context-menu", "help", "pointer", "progress", "wait", "cell",
    "crosshair", "text", "vertical-text", "alias", "copy", "move", "no-drop",
    "not-allowed", "grab", "grabbing", "e-resize", "n-resize", "ne-resize",
    "nw-resize", "s-resize", "se-resize", "sw-resize", "w-resize", "ew-resize",
    "ns-resize", "nesw-resize", "nwse-resize", "col-resize", "row-resize",
    "all-scroll", "zoom-in", "zoom-out", "dnd-ask", "all-resize"
};

static const char lisp_settings[] = "\n(defparameter +test-settle+ 0.08)\n";

static const char lisp_throwaway[] =
    "\n(defparameter +jelly+ (list :rotation (list :speed 0.035d0 :above 1d0 :below 1000d0)\n"
    "                            :settle +test-settle+))\n";

static const char lisp_tilt[] =
    "\n(defparameter +tilt+ (list :rotation (list :vx 0.009d0 :below 5000d0)\n"
    "                           :settle +test-settle+))\n";

static const char lisp_stretch[] =
    "\n(defparameter +stretch+ (list :stretch (list :speed (/ 1d0 4000d0) :above 200d0 "
    ":below 4000d0)\n"
    "                              :stretch-angle (list -90d0 (list :angle 1d0))\n"
    "                              :settle +test-settle+))\n";

static const char lisp_shake[] =
    "\n(defparameter +shake+ (list :scale (list :shake 0.5d0 :above 6d0 :below 8d0) "
    ":settle 0.2d0))\n"
    "\n"
    "(define-extension \"test-shake\" (:reads () :state nil) (snapshot state event)\n"
    "  (declare (ignore snapshot state event))\n"
    "  (values nil (list (cursor-effect \"shake\" +shake+ :order 20)) nil))\n";

static const char lisp_spring[] =
    "\n(defparameter +spring+ (list :rotation (list :vx 0.01d0 :below 3000d0) :settle 0.3d0))\n"
    "\n"
    "(define-extension \"test-spring\" (:reads () :state nil) (snapshot state event)\n"
    "  (declare (ignore snapshot state event))\n"
    "  (values nil (list (cursor-effect \"spring\" +spring+ :order 5)) nil))\n";

static const char lisp_trail[] =
    "\n(define-extension \"test-trail\" (:reads () :state nil) (snapshot state event)\n"
    "  (declare (ignore snapshot state event))\n"
    "  (values nil (list (settings :cursor-trail (list :enabled t :cell 9d0 :life 0.35d0\n"
    "                                                  :steps 4 :shape :cells\n"
    "                                                  :color \"#b8e0ff\"))) nil))\n";

static const char lisp_configure[] =
    "\n(define-extension \"test-output\" (:reads (:outputs) :state nil) (snapshot state event)\n"
    "  (declare (ignore state event))\n"
    "  (let ((output (first (context snapshot :outputs))))\n"
    "    (values nil\n"
    "            (when output\n"
    "              (list (configure-output (getf output :name)\n"
    "                                      :mode (list 1280 720 60) :scale 1\n"
    "                                      :position '(0 0))))\n"
    "            nil)))\n";

static const char extension_tilt[] =
    "\n(define-extension \"test-cursor\" (:reads () :state nil) (snapshot state event)\n"
    "  (declare (ignore snapshot state event))\n"
    "  (values nil (list (cursor-effect \"tilt\" +tilt+)) nil))\n";

static const char extension_stretch[] =
    "\n(define-extension \"test-cursor\" (:reads () :state nil) (snapshot state event)\n"
    "  (declare (ignore snapshot state event))\n"
    "  (values nil (list (cursor-effect \"stretch\" +stretch+)) nil))\n";

static const char extension_jelly[] =
    "\n(define-extension \"test-cursor\" (:reads () :state nil) (snapshot state event)\n"
    "  (declare (ignore snapshot state event))\n"
    "  (values nil (list (cursor-effect \"jelly\" +jelly+)) nil))\n";

static const struct point zigzag[] = {
    { PARK_X + 70, PARK_Y - 50 }, { PARK_X - 70, PARK_Y + 50 },
    { PARK_X + 70, PARK_Y - 50 }, { PARK_X - 70, PARK_Y + 50 },
    { PARK_X + 70, PARK_Y - 50 }, { PARK_X - 70, PARK_Y + 50 },
    { PARK_X + 70, PARK_Y - 50 }, { PARK_X - 70, PARK_Y + 50 },
    { PARK_X + 70, PARK_Y - 50 }, { PARK_X - 70, PARK_Y + 50 },
    { PARK_X + 70, PARK_Y - 50 }, { PARK_X - 70, PARK_Y + 50 },
    { PARK_X, PARK_Y }
};

static char root[4096];
static const char *tomoe_binary;
static const char *pointer_binary;
static struct compositor *live_compositor;
static long plain_wakeups;

static char **failures;
static size_t failure_count;
static size_t failure_capacity;

static void failf(const char *format, ...) {
    va_list arguments;
    va_start(arguments, format);
    char *text = NULL;
    if (vasprintf(&text, format, arguments) < 0) harness_fail("cursor: out of memory");
    va_end(arguments);
    char *prefixed = NULL;
    if (asprintf(&prefixed, "cursor: %s", text) < 0) harness_fail("cursor: out of memory");
    free(text);
    harness_fail(prefixed);
}

static void fail(const char *message) {
    failf("%s", message);
}

static void add_failure(const char *format, ...) {
    va_list arguments;
    va_start(arguments, format);
    char *text = NULL;
    if (vasprintf(&text, format, arguments) < 0) harness_fail("cursor: out of memory");
    va_end(arguments);
    if (failure_count + 1 >= failure_capacity) {
        failure_capacity = failure_capacity ? failure_capacity * 2 : 16;
        char **grown = realloc(failures, failure_capacity * sizeof failures[0]);
        if (!grown) harness_fail("cursor: out of memory");
        failures = grown;
    }
    failures[failure_count++] = text;
}

static void stop_compositor_process(struct compositor *compositor) {
    if (compositor->pid <= 0) return;
    kill(compositor->pid, SIGTERM);
    if (harness_wait_status(compositor->pid, 20) < 0) {
        kill(compositor->pid, SIGKILL);
        harness_wait_status(compositor->pid, 20);
    }
    compositor->running = 0;
    compositor->pid = -1;
}

static void cleanup(void) {
    if (live_compositor && live_compositor->running) stop_compositor_process(live_compositor);
    live_compositor = NULL;
}

static char *read_whole(const char *path) {
    FILE *file = fopen(path, "rb");
    if (!file) {
        char *empty = malloc(1);
        if (!empty) harness_fail("cursor: out of memory");
        empty[0] = '\0';
        return empty;
    }
    size_t capacity = 8192, used = 0;
    char *text = malloc(capacity);
    if (!text) harness_fail("cursor: out of memory");
    for (;;) {
        if (used + 65536 > capacity) {
            capacity *= 2;
            char *grown = realloc(text, capacity);
            if (!grown) harness_fail("cursor: out of memory");
            text = grown;
        }
        size_t got = fread(text + used, 1, capacity - used, file);
        used += got;
        if (got == 0) break;
    }
    text[used] = '\0';
    fclose(file);
    return text;
}

static char *read_fd_all(int fd) {
    size_t capacity = 8192, used = 0;
    char *text = malloc(capacity);
    if (!text) harness_fail("cursor: out of memory");
    for (;;) {
        if (used + 4096 > capacity) {
            capacity *= 2;
            char *grown = realloc(text, capacity);
            if (!grown) harness_fail("cursor: out of memory");
            text = grown;
        }
        ssize_t got = read(fd, text + used, capacity - used - 1);
        if (got < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (got == 0) break;
        used += (size_t)got;
    }
    text[used] = '\0';
    return text;
}

static void join_into(char *out, size_t size, const char *left, const char *right) {
    size_t first = strlen(left);
    if (first >= size) first = size - 1;
    memmove(out, left, first);
    size_t room = size - first - 1;
    size_t second = strlen(right);
    if (second > room) second = room;
    memmove(out + first, right, second);
    out[first + second] = '\0';
}

static void strip_into(const char *text, char *out, size_t size) {
    const char *start = text;
    while (*start && isspace((unsigned char)*start)) start++;
    size_t end = strlen(text);
    while (end > (size_t)(start - text) && isspace((unsigned char)text[end - 1])) end--;
    size_t length = end - (size_t)(start - text);
    if (length >= size) length = size - 1;
    memcpy(out, start, length);
    out[length] = '\0';
}

static void append_bytes(char **buffer, size_t *capacity, size_t *used, const char *data,
        size_t length) {
    if (*used + length + 1 > *capacity) {
        while (*used + length + 1 > *capacity) *capacity *= 2;
        char *grown = realloc(*buffer, *capacity);
        if (!grown) harness_fail("cursor: out of memory");
        *buffer = grown;
    }
    memcpy(*buffer + *used, data, length);
    *used += length;
    (*buffer)[*used] = '\0';
}

static void drain(int fd, char **buffer, size_t *capacity, size_t *used, int *open) {
    char chunk[65536];
    for (;;) {
        ssize_t got = read(fd, chunk, sizeof chunk);
        if (got > 0) {
            append_bytes(buffer, capacity, used, chunk, (size_t)got);
            continue;
        }
        if (got == 0) {
            close(fd);
            *open = 0;
            return;
        }
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) return;
        close(fd);
        *open = 0;
        return;
    }
}

static struct outcome run_capture(char *const argv[], char **env, double timeout) {
    struct outcome result;
    memset(&result, 0, sizeof result);
    result.status = -1;
    int out_pipe[2], err_pipe[2];
    if (pipe(out_pipe) != 0 || pipe(err_pipe) != 0) harness_fail("cursor: cannot make a pipe");
    pid_t child = fork();
    if (child < 0) harness_fail("cursor: cannot fork");
    if (child == 0) {
        dup2(out_pipe[1], STDOUT_FILENO);
        dup2(err_pipe[1], STDERR_FILENO);
        close(out_pipe[0]);
        close(out_pipe[1]);
        close(err_pipe[0]);
        close(err_pipe[1]);
        execvpe(argv[0], argv, env);
        fprintf(stderr, "%s: %s\n", argv[0], strerror(errno));
        _exit(127);
    }
    close(out_pipe[1]);
    close(err_pipe[1]);
    fcntl(out_pipe[0], F_SETFL, O_NONBLOCK);
    fcntl(err_pipe[0], F_SETFL, O_NONBLOCK);
    size_t out_capacity = 4096, out_used = 0, err_capacity = 4096, err_used = 0;
    char *out = malloc(out_capacity), *err = malloc(err_capacity);
    if (!out || !err) harness_fail("cursor: out of memory");
    out[0] = '\0';
    err[0] = '\0';
    int open_out = 1, open_err = 1;
    double deadline = timeout > 0 ? harness_now() + timeout : 0;
    while (open_out || open_err) {
        if (timeout > 0 && harness_now() > deadline) {
            result.timed_out = 1;
            kill(child, SIGKILL);
            break;
        }
        struct pollfd fds[2];
        int count = 0, at_out = -1, at_err = -1;
        if (open_out) {
            fds[count].fd = out_pipe[0];
            fds[count].events = POLLIN;
            at_out = count++;
        }
        if (open_err) {
            fds[count].fd = err_pipe[0];
            fds[count].events = POLLIN;
            at_err = count++;
        }
        int ready = poll(fds, (nfds_t)count, 50);
        if (ready < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (ready == 0) continue;
        if (at_out >= 0 && (fds[at_out].revents & (POLLIN | POLLHUP | POLLERR)))
            drain(out_pipe[0], &out, &out_capacity, &out_used, &open_out);
        if (at_err >= 0 && (fds[at_err].revents & (POLLIN | POLLHUP | POLLERR)))
            drain(err_pipe[0], &err, &err_capacity, &err_used, &open_err);
    }
    if (open_out) close(out_pipe[0]);
    if (open_err) close(err_pipe[0]);
    int status = 0;
    while (waitpid(child, &status, 0) < 0 && errno == EINTR) { }
    if (result.timed_out) result.status = -1;
    else if (WIFEXITED(status)) result.status = WEXITSTATUS(status);
    else result.status = 128 + (WIFSIGNALED(status) ? WTERMSIG(status) : 0);
    result.out = out;
    result.err = err;
    return result;
}

static void outcome_free(struct outcome *result) {
    free(result->out);
    free(result->err);
    result->out = NULL;
    result->err = NULL;
}

static char *outcome_text(struct outcome *result) {
    size_t first = strlen(result->out), second = strlen(result->err);
    char *text = malloc(first + second + 1);
    if (!text) harness_fail("cursor: out of memory");
    memcpy(text, result->out, first);
    memcpy(text + first, result->err, second + 1);
    return text;
}

static void path_parent(const char *path, char *out, size_t size) {
    snprintf(out, size, "%s", path);
    char *slash = strrchr(out, '/');
    if (!slash) {
        snprintf(out, size, ".");
        return;
    }
    if (slash == out) {
        out[1] = '\0';
        return;
    }
    *slash = '\0';
}

static void theme_cursor_write(const char *path, unsigned color) {
    unsigned header[16] = {
        0x72756358u, 16u, 0x10000u, 1u,
        0xfffd0002u, 24u, 28u,
        36u, 0xfffd0002u, 24u, 1u, ARROW_WIDTH, ARROW_HEIGHT, 0u, 0u, 0u
    };
    size_t size = sizeof header + (size_t)ARROW_WIDTH * ARROW_HEIGHT * 4;
    unsigned char *data = calloc(1, size);
    if (!data) harness_fail("cursor: out of memory");
    for (size_t index = 0; index < 16; index++) {
        unsigned value = header[index];
        data[index * 4] = (unsigned char)(value & 0xff);
        data[index * 4 + 1] = (unsigned char)((value >> 8) & 0xff);
        data[index * 4 + 2] = (unsigned char)((value >> 16) & 0xff);
        data[index * 4 + 3] = (unsigned char)((value >> 24) & 0xff);
    }
    for (int y = 0; y < ARROW_HEIGHT; y++) {
        for (int x = 0; x < ARROW_WIDTH; x++) {
            int lit = x <= (y < 8 ? y : 8) || (y >= 12 && x >= ARROW_WIDTH - 5);
            unsigned value = lit ? color : 0u;
            size_t at = sizeof header + ((size_t)y * ARROW_WIDTH + (size_t)x) * 4;
            data[at] = (unsigned char)(value & 0xff);
            data[at + 1] = (unsigned char)((value >> 8) & 0xff);
            data[at + 2] = (unsigned char)((value >> 16) & 0xff);
            data[at + 3] = (unsigned char)((value >> 24) & 0xff);
        }
    }
    FILE *file = fopen(path, "wb");
    if (!file) failf("cannot write the cursor theme file %s", path);
    if (fwrite(data, 1, size, file) != size) fail("cannot write a cursor theme file");
    if (fclose(file) != 0) fail("cannot write a cursor theme file");
    free(data);
}

static void relink_theme(const char *icons, const char *variant) {
    char link[4096], staged[4096];
    harness_join(link, sizeof link, icons, "check");
    harness_join(staged, sizeof staged, icons, ".check-relink");
    struct stat status;
    if (lstat(staged, &status) == 0 && S_ISLNK(status.st_mode)) unlink(staged);
    if (symlink(variant, staged) != 0) fail("cannot create a cursor theme link");
    if (rename(staged, link) != 0) fail("cannot replace the cursor theme link");
}

static void write_theme(const char *root_path) {
    static const char *const variants[2] = { "check-blue", "check-red" };
    static const unsigned bases[2] = { 0xff0000ffu, 0xffff0000u };
    char icons[4096];
    harness_join(icons, sizeof icons, root_path, "icons");
    for (int variant = 0; variant < 2; variant++) {
        char directory[4096], cursors[4096];
        harness_join(directory, sizeof directory, icons, variants[variant]);
        harness_join(cursors, sizeof cursors, directory, "cursors");
        harness_mkparents(cursors, 0777);
        for (int index = 1; index <= SHAPE_COUNT; index++) {
            char path[4096];
            harness_join(path, sizeof path, cursors, shapes[index - 1]);
            theme_cursor_write(path, bases[variant] | ((unsigned)index << 8));
        }
    }
    relink_theme(icons, "check-blue");
}

static void compositor_write_policy(struct compositor *compositor, const char *body) {
    size_t size = strlen("(in-package #:tomoe-user)\n") + strlen(lisp_settings) +
        strlen(body) + 1;
    char *text = malloc(size);
    if (!text) harness_fail("cursor: out of memory");
    snprintf(text, size, "(in-package #:tomoe-user)\n%s%s", lisp_settings, body);
    harness_write_file(compositor->policy, text);
    free(text);
}

static char *compositor_text(struct compositor *compositor) {
    if (!compositor->log_open) return read_whole("/dev/null");
    return read_whole(compositor->log_path);
}

static struct outcome compositor_call(struct compositor *compositor, const char *first) {
    char *arguments[6];
    arguments[0] = (char *)tomoe_binary;
    arguments[1] = (char *)"--socket";
    arguments[2] = (char *)"cursor";
    arguments[3] = (char *)first;
    arguments[4] = NULL;
    return run_capture(arguments, env_items(&compositor->env), 0);
}

static void compositor_start(struct compositor *compositor, const char *work, const char *body,
        int bare) {
    memset(compositor, 0, sizeof *compositor);
    compositor->pid = -1;
    compositor->log_open = 1;
    snprintf(compositor->work, sizeof compositor->work, "%s", work);
    harness_join(compositor->policy, sizeof compositor->policy, work, "policy.lisp");
    harness_join(compositor->log_path, sizeof compositor->log_path, work, "tomoe.log");
    char runtime[4096];
    harness_join(runtime, sizeof runtime, work, "run");
    harness_mkparents(runtime, 0700);
    chmod(runtime, 0700);
    compositor_write_policy(compositor, body);
    env_reset(&compositor->env);
    env_keep(&compositor->env, "PATH");
    env_keep(&compositor->env, "HOME");
    env_keep(&compositor->env, "MESA_LOADER_DRIVER_OVERRIDE");
    env_keep(&compositor->env, "LIBGL_DRIVERS_PATH");
    env_keep(&compositor->env, "__EGL_VENDOR_LIBRARY_FILENAMES");
    env_keep(&compositor->env, "LD_LIBRARY_PATH");
    env_keep(&compositor->env, "LIBGL_ALWAYS_SOFTWARE");
    char parent[4096], icons[4096], late[4096], icon_path[8192];
    path_parent(work, parent, sizeof parent);
    harness_join(icons, sizeof icons, parent, "icons");
    harness_join(late, sizeof late, parent, "late");
    harness_concat(late, sizeof late, late, "/icons");
    snprintf(icon_path, sizeof icon_path, "%s:%s", late, icons);
    env_put(&compositor->env, "XDG_RUNTIME_DIR", runtime);
    env_put(&compositor->env, "WAYLAND_DISPLAY", "cursor");
    env_put(&compositor->env, "XCURSOR_PATH", icon_path);
    env_put(&compositor->env, "XCURSOR_THEME", "check");
    env_put(&compositor->env, "TOMOE_DEBUG", "1");
    char *arguments[10];
    int used = 0;
    arguments[used++] = (char *)tomoe_binary;
    arguments[used++] = (char *)"--socket";
    arguments[used++] = (char *)"cursor";
    arguments[used++] = (char *)"--backend";
    arguments[used++] = (char *)"headless";
    arguments[used++] = (char *)"--no-watch";
    if (bare) arguments[used++] = (char *)"--bare";
    arguments[used++] = (char *)"--config";
    arguments[used++] = compositor->policy;
    arguments[used] = NULL;
    compositor->pid = harness_spawn(arguments, env_items(&compositor->env), NULL, NULL,
            compositor->log_path, 0);
    compositor->running = 1;
    live_compositor = compositor;
    double deadline = harness_now() + 60;
    while (harness_now() < deadline) {
        int status = harness_wait_status(compositor->pid, 0);
        if (status >= 0) {
            compositor->running = 0;
            compositor->pid = -1;
            char *log = compositor_text(compositor);
            failf("tomoe exited while starting:\n%s", log);
        }
        struct outcome run = compositor_call(compositor, "inspect");
        int code = run.status;
        outcome_free(&run);
        if (code == 0) return;
        harness_sleep(0.1);
    }
    char *log = compositor_text(compositor);
    failf("tomoe did not answer inspect:\n%s", log);
}

static void compositor_stop(struct compositor *compositor) {
    if (compositor->running) {
        if (harness_wait_status(compositor->pid, 0) < 0) stop_compositor_process(compositor);
    }
    compositor->running = 0;
    compositor->log_open = 0;
    if (live_compositor == compositor) live_compositor = NULL;
}

static void scan_fields(const char *start, const char *end, long *total) {
    const char *at = start;
    while (at < end) {
        if (*at != ':') {
            at++;
            continue;
        }
        const char *key = at + 1;
        const char *cursor = key;
        while (cursor < end && (isalpha((unsigned char)*cursor) || *cursor == '-')) cursor++;
        if (cursor == key || cursor >= end || !isspace((unsigned char)*cursor)) {
            at++;
            continue;
        }
        const char *value = cursor;
        while (value < end && isspace((unsigned char)*value)) value++;
        if (value >= end) break;
        const char *stop = value;
        while (stop < end && (isalnum((unsigned char)*stop) || *stop == '-' || *stop == '_'))
            stop++;
        if (stop == value) {
            at++;
            continue;
        }
        if ((size_t)(cursor - key) == 5 && strncasecmp(key, "frame", 5) == 0) {
            char number[64];
            size_t length = (size_t)(stop - value);
            if (length >= sizeof number) length = sizeof number - 1;
            memcpy(number, value, length);
            number[length] = '\0';
            *total += strtol(number, NULL, 10);
        }
        at = stop;
    }
}

static int contains_ci(const char *text, const char *word) {
    size_t length = strlen(word);
    for (const char *at = text; *at; at++)
        if (strncasecmp(at, word, length) == 0) return 1;
    return 0;
}

static long compositor_frames(struct compositor *compositor) {
    struct outcome run = compositor_call(compositor, "frames");
    char *reply = outcome_text(&run);
    outcome_free(&run);
    long total = 0;
    const char *at = reply;
    while (*at) {
        if (*at == ':' && strncasecmp(at + 1, "NAME", 4) == 0 && at[5] == ' ' && at[6] == '"') {
            const char *name = at + 7;
            const char *close = *name == '"' ? NULL : strchr(name, '"');
            if (close) {
                const char *line_end = strchr(close + 1, '\n');
                if (!line_end) line_end = close + 1 + strlen(close + 1);
                scan_fields(close + 1, line_end, &total);
                at = line_end;
                continue;
            }
        }
        at++;
    }
    if (total == 0 && !contains_ci(reply, "frame")) failf("unreadable frames reply: %.300s", reply);
    free(reply);
    return total;
}

static long compositor_wakeups(struct compositor *compositor) {
    char path[64];
    snprintf(path, sizeof path, "/proc/%d/status", (int)compositor->pid);
    FILE *file = fopen(path, "r");
    if (!file) fail("cannot read the compositor status");
    char line[1024];
    long total = 0;
    while (fgets(line, sizeof line, file)) {
        char *found = strstr(line, "ctxt_switches:");
        if (!found) continue;
        found += strlen("ctxt_switches:");
        total += strtol(found, NULL, 10);
    }
    fclose(file);
    return total;
}

static void encode_path(char *out, size_t size, const struct point *points, size_t count) {
    size_t used = 0;
    out[0] = '\0';
    for (size_t index = 0; index < count; index++) {
        char piece[64];
        snprintf(piece, sizeof piece, "%s%d,%d", index ? ";" : "", points[index].x,
                points[index].y);
        int wrote = snprintf(out + used, size - used, "%s", piece);
        if (wrote < 0 || (size_t)wrote >= size - used) fail("a pointer path is too long");
        used += (size_t)wrote;
    }
}

static int stroke_run(struct compositor *compositor, const struct point *points, size_t count,
        int steps, int interval_ms, int hold_ms, char *error, size_t error_size) {
    char encoded[8192], step_text[16], interval_text[16], hold_text[16];
    encode_path(encoded, sizeof encoded, points, count);
    snprintf(step_text, sizeof step_text, "%d", steps);
    snprintf(interval_text, sizeof interval_text, "%d", interval_ms);
    snprintf(hold_text, sizeof hold_text, "%d", hold_ms);
    char *arguments[14];
    int used = 0;
    arguments[used++] = (char *)pointer_binary;
    arguments[used++] = (char *)"--path";
    arguments[used++] = encoded;
    arguments[used++] = (char *)"--steps";
    arguments[used++] = step_text;
    arguments[used++] = (char *)"--interval-ms";
    arguments[used++] = interval_text;
    arguments[used++] = (char *)"--hold-ms";
    arguments[used++] = hold_text;
    arguments[used++] = (char *)"--delay-ms";
    arguments[used++] = (char *)"50";
    arguments[used++] = (char *)"--extent";
    arguments[used++] = (char *)"1280x720";
    arguments[used] = NULL;
    struct outcome run = run_capture(arguments, env_items(&compositor->env), 0);
    int failed = run.status != 0;
    if (failed) {
        char trimmed[8192];
        strip_into(run.err, trimmed, sizeof trimmed);
        join_into(error, error_size, "pointer-path failed: ", trimmed);
    }
    outcome_free(&run);
    return failed;
}

static void stroke(struct compositor *compositor, const struct point *points, size_t count,
        int steps, int interval_ms, int hold_ms) {
    char error[8192];
    if (stroke_run(compositor, points, count, steps, interval_ms, hold_ms, error,
                sizeof error)) fail(error);
}

static int screenshot_try(struct compositor *compositor, const char *name, char *path,
        size_t path_size, char *error, size_t error_size) {
    harness_join(path, path_size, compositor->work, name);
    for (int attempt = 0; attempt < 4; attempt++) {
        char *arguments[6];
        arguments[0] = (char *)"grim";
        arguments[1] = (char *)"-c";
        arguments[2] = (char *)"-t";
        arguments[3] = (char *)"ppm";
        arguments[4] = path;
        arguments[5] = NULL;
        struct outcome run = run_capture(arguments, env_items(&compositor->env), 10);
        if (run.timed_out) {
            outcome_free(&run);
            struct point probe[2] = {
                { PARK_X, PARK_Y + attempt }, { PARK_X + 1, PARK_Y + attempt }
            };
            if (stroke_run(compositor, probe, 2, 1, 8, 20, error, error_size)) return 0;
            harness_sleep(0.2);
            continue;
        }
        if (run.status != 0) {
            char trimmed[8192];
            strip_into(run.err, trimmed, sizeof trimmed);
            join_into(error, error_size, "grim failed: ", trimmed);
            outcome_free(&run);
            return 0;
        }
        outcome_free(&run);
        return 1;
    }
    snprintf(error, error_size, "grim never received a frame");
    return 0;
}

static void screenshot(struct compositor *compositor, const char *name, char *path,
        size_t path_size) {
    char error[8192];
    if (!screenshot_try(compositor, name, path, path_size, error, sizeof error)) fail(error);
}

static struct mover mover_start(struct compositor *compositor, const struct point *points,
        size_t count, int steps, int interval_ms, int hold_ms) {
    struct mover mover;
    memset(&mover, 0, sizeof mover);
    mover.pid = -1;
    mover.output = -1;
    mover.errors = -1;
    char encoded[8192], step_text[16], interval_text[16], hold_text[16];
    encode_path(encoded, sizeof encoded, points, count);
    snprintf(step_text, sizeof step_text, "%d", steps);
    snprintf(interval_text, sizeof interval_text, "%d", interval_ms);
    snprintf(hold_text, sizeof hold_text, "%d", hold_ms);
    char *arguments[14];
    int used = 0;
    arguments[used++] = (char *)pointer_binary;
    arguments[used++] = (char *)"--path";
    arguments[used++] = encoded;
    arguments[used++] = (char *)"--steps";
    arguments[used++] = step_text;
    arguments[used++] = (char *)"--interval-ms";
    arguments[used++] = interval_text;
    arguments[used++] = (char *)"--hold-ms";
    arguments[used++] = hold_text;
    arguments[used++] = (char *)"--delay-ms";
    arguments[used++] = (char *)"50";
    arguments[used++] = (char *)"--extent";
    arguments[used++] = (char *)"1280x720";
    arguments[used] = NULL;
    int out_pipe[2], err_pipe[2];
    if (pipe(out_pipe) != 0 || pipe(err_pipe) != 0) harness_fail("cursor: cannot make a pipe");
    pid_t child = fork();
    if (child < 0) harness_fail("cursor: cannot fork");
    if (child == 0) {
        dup2(out_pipe[1], STDOUT_FILENO);
        dup2(err_pipe[1], STDERR_FILENO);
        close(out_pipe[0]);
        close(out_pipe[1]);
        close(err_pipe[0]);
        close(err_pipe[1]);
        execvpe(arguments[0], arguments, env_items(&compositor->env));
        fprintf(stderr, "%s: %s\n", arguments[0], strerror(errno));
        _exit(127);
    }
    close(out_pipe[1]);
    close(err_pipe[1]);
    fcntl(out_pipe[0], F_SETFD, FD_CLOEXEC);
    fcntl(err_pipe[0], F_SETFD, FD_CLOEXEC);
    mover.pid = child;
    mover.output = out_pipe[0];
    mover.errors = err_pipe[0];
    return mover;
}

static int mover_running(struct mover *mover) {
    if (mover->pid <= 0) return 0;
    int status = 0;
    pid_t done = waitpid(mover->pid, &status, WNOHANG);
    if (done != mover->pid) return 1;
    mover->pid = -1;
    return 0;
}

static int mover_wait(struct mover *mover) {
    if (mover->pid <= 0) return 0;
    int status = 0;
    while (waitpid(mover->pid, &status, 0) < 0 && errno == EINTR) { }
    mover->pid = -1;
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    return 128 + (WIFSIGNALED(status) ? WTERMSIG(status) : 0);
}

static int mover_read_line(struct mover *mover, char *line, size_t size) {
    size_t used = 0;
    int saw = 0;
    for (;;) {
        char byte;
        ssize_t got = read(mover->output, &byte, 1);
        if (got <= 0) break;
        saw = 1;
        if (byte == '\n') break;
        if (used + 1 < size) line[used++] = byte;
    }
    line[used] = '\0';
    return saw;
}

static struct mover stroke_until(struct compositor *compositor, const struct point *points,
        size_t count, const char *word, int steps, int interval_ms) {
    struct mover mover = mover_start(compositor, points, count, steps, interval_ms, 0);
    char line[8192];
    while (mover_read_line(&mover, line, sizeof line)) {
        char trimmed[8192];
        strip_into(line, trimmed, sizeof trimmed);
        if (strncmp(trimmed, word, strlen(word)) == 0) return mover;
    }
    mover_wait(&mover);
    char *errors = read_fd_all(mover.errors);
    char trimmed[8192];
    strip_into(errors, trimmed, sizeof trimmed);
    free(errors);
    failf("pointer-path ended before it %s: %s", word, trimmed);
    return mover;
}

static void park(struct compositor *compositor) {
    struct point path[2] = { { PARK_X - 24, PARK_Y }, { PARK_X, PARK_Y } };
    stroke(compositor, path, 2, 6, 8, 120);
    harness_sleep(0.7);
}

static void idle_measure(struct compositor *compositor, double seconds, long *drawn,
        long *wakeups) {
    long before = compositor_frames(compositor);
    long woke = compositor_wakeups(compositor);
    harness_sleep(seconds);
    *drawn = compositor_frames(compositor) - before;
    *wakeups = compositor_wakeups(compositor) - woke;
}

static void image_load(const char *path, struct image *image) {
    memset(image, 0, sizeof *image);
    FILE *file = fopen(path, "rb");
    if (!file) failf("cannot read %s", path);
    size_t capacity = 1 << 20, used = 0;
    unsigned char *data = malloc(capacity);
    if (!data) harness_fail("cursor: out of memory");
    for (;;) {
        if (used + 65536 > capacity) {
            capacity *= 2;
            unsigned char *grown = realloc(data, capacity);
            if (!grown) harness_fail("cursor: out of memory");
            data = grown;
        }
        size_t got = fread(data + used, 1, capacity - used, file);
        used += got;
        if (got == 0) break;
    }
    fclose(file);
    size_t positions[3] = { 0, 0, 0 };
    int found = 0;
    for (size_t at = 0; at < used && found < 3; at++)
        if (data[at] == '\n') positions[found++] = at;
    if (found < 3) failf("%s is not a binary PPM", path);
    char dimension[256];
    size_t length = positions[1] - positions[0] - 1;
    if (length >= sizeof dimension) failf("%s is not a binary PPM", path);
    memcpy(dimension, data + positions[0] + 1, length);
    dimension[length] = '\0';
    int width = 0, height = 0;
    if (sscanf(dimension, "%d %d", &width, &height) != 2)
        failf("%s is not a binary PPM", path);
    image->width = width;
    image->height = height;
    image->storage = data;
    image->data = data + positions[2] + 1;
    image->length = used - positions[2] - 1;
}

static void image_free(struct image *image) {
    free(image->storage);
    image->storage = NULL;
    image->data = NULL;
    image->length = 0;
}

static int pixel_lit(const struct image *image, int x, int y) {
    size_t at = ((size_t)y * (size_t)image->width + (size_t)x) * 3;
    if (at + 2 >= image->length) return 0;
    return image->data[at] > 25 || image->data[at + 1] > 25 || image->data[at + 2] > 25;
}

static int compare_offsets(const void *left, const void *right) {
    long long first = *(const long long *)left;
    long long second = *(const long long *)right;
    return first < second ? -1 : first > second;
}

static struct signature signature_of(const char *path) {
    struct signature result;
    memset(&result, 0, sizeof result);
    struct image image;
    image_load(path, &image);
    long count = 0;
    int left = 0, top = 0;
    for (int y = 0; y < image.height; y++)
        for (int x = 0; x < image.width; x++)
            if (pixel_lit(&image, x, y)) {
                if (!count) {
                    left = x;
                    top = y;
                }
                if (x < left) left = x;
                if (y < top) top = y;
                count++;
            }
    result.count = count;
    if (count) {
        result.offsets = malloc(sizeof(long long) * (size_t)count);
        if (!result.offsets) harness_fail("cursor: out of memory");
        long at = 0;
        for (int y = 0; y < image.height; y++)
            for (int x = 0; x < image.width; x++)
                if (pixel_lit(&image, x, y))
                    result.offsets[at++] = ((long long)(x - left) << 32) |
                        (unsigned)(y - top);
        qsort(result.offsets, (size_t)count, sizeof result.offsets[0], compare_offsets);
    }
    image_free(&image);
    return result;
}

static int signature_equal(const struct signature *first, const struct signature *second) {
    if (first->count != second->count) return 0;
    for (long index = 0; index < first->count; index++)
        if (first->offsets[index] != second->offsets[index]) return 0;
    return 1;
}

static int same_image(const char *first, const char *second) {
    struct signature left = signature_of(first);
    struct signature right = signature_of(second);
    int equal = signature_equal(&left, &right);
    free(left.offsets);
    free(right.offsets);
    return equal;
}

static size_t lit_len(const char *path) {
    struct image image;
    image_load(path, &image);
    size_t count = 0;
    for (int y = 0; y < image.height; y++)
        for (int x = 0; x < image.width; x++)
            if (pixel_lit(&image, x, y)) count++;
    image_free(&image);
    return count;
}

static int within(int value, int centre, int radius) {
    return value - centre <= radius && centre - value <= radius;
}

static size_t near_lit_len(const char *path, int x, int y, int radius) {
    struct image image;
    image_load(path, &image);
    size_t count = 0;
    for (int py = 0; py < image.height; py++)
        for (int px = 0; px < image.width; px++)
            if (within(px, x, radius) && within(py, y, radius) &&
                    pixel_lit(&image, px, py))
                count++;
    image_free(&image);
    return count;
}

static void tint_of(const char *path, int x, int y, int radius, long *red, long *blue) {
    struct image image;
    image_load(path, &image);
    long red_total = 0, blue_total = 0;
    for (int py = 0; py < image.height; py++)
        for (int px = 0; px < image.width; px++)
            if (within(px, x, radius) && within(py, y, radius) &&
                    pixel_lit(&image, px, py)) {
                size_t at = ((size_t)py * (size_t)image.width + (size_t)px) * 3;
                red_total += image.data[at];
                blue_total += image.data[at + 2];
            }
    image_free(&image);
    *red = red_total;
    *blue = blue_total;
}

static void shots_add(struct shots *list, const char *path) {
    if (list->count + 1 >= list->capacity) {
        list->capacity = list->capacity ? list->capacity * 2 : 64;
        char **grown = realloc(list->paths, list->capacity * sizeof list->paths[0]);
        if (!grown) harness_fail("cursor: out of memory");
        list->paths = grown;
    }
    list->paths[list->count] = strdup(path);
    if (!list->paths[list->count]) harness_fail("cursor: out of memory");
    list->count++;
}

static void shots_free(struct shots *list) {
    for (size_t index = 0; index < list->count; index++) free(list->paths[index]);
    free(list->paths);
    list->paths = NULL;
    list->count = 0;
    list->capacity = 0;
}

static void *capture_frames(void *data) {
    struct capture *capture = data;
    for (;;) {
        if (atomic_load_explicit(&capture->stop, memory_order_relaxed)) return NULL;
        char name[64], path[4096], error[8192];
        snprintf(name, sizeof name, "frame-%03d.ppm", (int)capture->shots->count);
        if (!screenshot_try(capture->compositor, name, path, sizeof path, error,
                    sizeof error))
            return NULL;
        shots_add(capture->shots, path);
        harness_sleep(0.012);
    }
}

static void case_run(struct compositor *compositor, const struct point *points, size_t count,
        int steps, int interval_ms, char *rest, size_t rest_size, struct shots *shots,
        char *settled, size_t settled_size) {
    park(compositor);
    screenshot(compositor, "rest.ppm", rest, rest_size);
    struct capture capture;
    capture.compositor = compositor;
    capture.shots = shots;
    atomic_init(&capture.stop, 0);
    pthread_t thread;
    if (pthread_create(&thread, NULL, capture_frames, &capture) != 0)
        fail("cannot start the capture thread");
    harness_sleep(0.1);
    stroke(compositor, points, count, steps, interval_ms, 0);
    harness_sleep(0.05);
    atomic_store_explicit(&capture.stop, 1, memory_order_relaxed);
    pthread_join(thread, NULL);
    harness_sleep(0.9);
    screenshot(compositor, "settled.ppm", settled, settled_size);
}

static void remove_matching(const char *directory, const char *prefix, const char *suffix) {
    DIR *dir = opendir(directory);
    if (!dir) return;
    size_t prefix_length = strlen(prefix), suffix_length = strlen(suffix);
    struct dirent *entry;
    while ((entry = readdir(dir))) {
        size_t length = strlen(entry->d_name);
        if (length < prefix_length + suffix_length) continue;
        if (strncmp(entry->d_name, prefix, prefix_length) != 0) continue;
        if (strcmp(entry->d_name + length - suffix_length, suffix) != 0) continue;
        char path[4096];
        harness_join(path, sizeof path, directory, entry->d_name);
        unlink(path);
    }
    closedir(dir);
}

static void magnified(struct compositor *compositor, const char *name, const char *reference,
        struct shots *shots, const char *settled) {
    (void)shots;
    (void)settled;
    park(compositor);
    char before[4096], shot[4096], after[4096];
    screenshot(compositor, "shake-before.ppm", before, sizeof before);
    struct mover mover = stroke_until(compositor, zigzag,
            sizeof zigzag / sizeof zigzag[0], "dragged", 3, 6);
    screenshot(compositor, "shake-magnified.ppm", shot, sizeof shot);
    mover_wait(&mover);
    size_t rest_lit = near_lit_len(before, PARK_X, PARK_Y, 120);
    size_t shaken_lit = near_lit_len(shot, PARK_X, PARK_Y, 120);
    printf("%s: %zu lit pixels at rest, %zu magnified\n", name, rest_lit, shaken_lit);
    if (shaken_lit < rest_lit * 3)
        add_failure("%s: a shake did not magnify the cursor (%zu against %zu)", name,
                shaken_lit, rest_lit);
    harness_sleep(1.0);
    screenshot(compositor, "shake-after.ppm", after, sizeof after);
    if (!same_image(after, reference))
        add_failure("%s: the magnification did not time out to the plain image", name);
}

static void settled_late(struct compositor *compositor, const char *name, const char *reference,
        struct shots *shots, const char *settled) {
    (void)settled;
    park(compositor);
    char before[4096], early[4096], middle[4096], after[4096];
    screenshot(compositor, "spring-before.ppm", before, sizeof before);
    struct point path[2] = { { PARK_X, PARK_Y }, { 900, PARK_Y } };
    struct mover mover = stroke_until(compositor, path, 2, "released", 8, 10);
    screenshot(compositor, "spring-early.ppm", early, sizeof early);
    harness_sleep(0.12);
    screenshot(compositor, "spring-middle.ppm", middle, sizeof middle);
    harness_sleep(1.2);
    screenshot(compositor, "spring-after.ppm", after, sizeof after);
    mover_wait(&mover);
    if (same_image(early, reference))
        add_failure("%s: the cursor was already at rest when the motion stopped", name);
    if (same_image(early, middle))
        add_failure("%s: the cursor froze instead of settling", name);
    if (!same_image(after, reference))
        add_failure("%s: the cursor never settled to the plain image", name);
    printf("%s: settled through %zu frames\n", name, shots->count);
}

static void trailed(struct compositor *compositor, const char *name, const char *reference,
        struct shots *shots, const char *settled) {
    (void)compositor;
    (void)reference;
    (void)settled;
    size_t largest = 0;
    printf("%s: lit pixels per captured frame: [", name);
    for (size_t index = 0; index < shots->count; index++) {
        size_t lit = lit_len(shots->paths[index]);
        if (lit > largest) largest = lit;
        printf(index ? ", %zu" : "%zu", lit);
    }
    printf("]\n");
    if (!shots->count || largest < 200) add_failure("%s: the trail never drew", name);
}

static void moved(struct compositor *compositor, const char *name, const char *reference,
        struct shots *shots, const char *settled) {
    (void)compositor;
    (void)name;
    (void)reference;
    (void)shots;
    (void)settled;
}

static void build_body(char *out, size_t size, const char *extra) {
    snprintf(out, size, "%s%s", lisp_configure, extra);
}

static void check_run(const char *name, const char *extra, const char *reference,
        expect_fn expect, const struct point *path, size_t path_count, int steps,
        int interval_ms) {
    char work[4096], body[8192];
    harness_join(work, sizeof work, root, name);
    harness_mkparents(work, 0777);
    build_body(body, sizeof body, extra);
    struct compositor compositor;
    compositor_start(&compositor, work, body, 1);
    harness_sleep(1.0);
    char rest[4096], settled[4096];
    struct shots shots;
    memset(&shots, 0, sizeof shots);
    case_run(&compositor, path, path_count, steps, interval_ms, rest, sizeof rest, &shots,
            settled, sizeof settled);
    if (!same_image(rest, reference))
        add_failure("%s: the resting cursor is not the plain image", name);
    int deformed = 0;
    for (size_t index = 0; index < shots.count && !deformed; index++)
        if (!same_image(shots.paths[index], reference)) deformed = 1;
    if (!deformed) add_failure("%s: the cursor never deformed while moving", name);
    if (!same_image(settled, reference))
        add_failure("%s: the cursor did not come back to the plain image", name);
    long drawn = 0, wakeups = 0;
    idle_measure(&compositor, 2, &drawn, &wakeups);
    printf("%s: %zu captured frames, %ld idle frames, %ld idle wakeups\n", name, shots.count,
            drawn, wakeups);
    if (drawn) add_failure("%s: drew %ld frames while idle", name, drawn);
    if (wakeups > plain_wakeups + 2)
        add_failure("%s: %ld idle wakeups against %ld plain", name, wakeups, plain_wakeups);
    expect(&compositor, name, reference, &shots, settled);
    shots_free(&shots);
    compositor_stop(&compositor);
    remove_matching(work, "frame-", ".ppm");
}

static int find_on_path(const char *name) {
    const char *path = getenv("PATH");
    if (!path) return 0;
    const char *at = path;
    for (;;) {
        const char *end = strchr(at, ':');
        size_t length = end ? (size_t)(end - at) : strlen(at);
        char candidate[4096];
        char directory[4096];
        if (length >= sizeof directory) return 0;
        memcpy(directory, at, length);
        directory[length] = '\0';
        if (length == 0) snprintf(directory, sizeof directory, ".");
        join_into(candidate, sizeof candidate, directory, "/");
        join_into(candidate, sizeof candidate, candidate, name);
        struct stat status;
        if (stat(candidate, &status) == 0 && S_ISREG(status.st_mode) &&
                access(candidate, X_OK) == 0)
            return 1;
        if (!end) return 0;
        at = end + 1;
    }
}

static int usage(const char *program) {
    fprintf(stderr, "usage: %s --tomoe TOMOE --pointer POINTER [--work WORK]\n", program);
    return 2;
}

static void absolute_path(const char *path, char *out, size_t size) {
    if (path[0] == '/') snprintf(out, size, "%s", path);
    else {
        char directory[4096], joined[4096];
        if (!getcwd(directory, sizeof directory)) snprintf(directory, sizeof directory, ".");
        join_into(joined, sizeof joined, directory, "/");
        join_into(out, size, joined, path);
    }
}

int main(int argc, char **argv) {
    const char *tomoe_argument = NULL, *pointer_argument = NULL, *work_argument = NULL;
    for (int index = 1; index < argc; index++) {
        const char *option = argv[index];
        const char *value = NULL;
        char inline_option[64];
        const char *equals = strchr(option, '=');
        if (equals && strncmp(option, "--", 2) == 0) {
            size_t length = (size_t)(equals - option);
            if (length >= sizeof inline_option) return usage(argv[0]);
            memcpy(inline_option, option, length);
            inline_option[length] = '\0';
            option = inline_option;
            value = equals + 1;
        }
        if (strcmp(option, "--help") == 0 || strcmp(option, "-h") == 0) {
            printf("usage: %s --tomoe TOMOE --pointer POINTER [--work WORK]\n", argv[0]);
            return 0;
        }
        if (strcmp(option, "--tomoe") != 0 && strcmp(option, "--pointer") != 0 &&
                strcmp(option, "--work") != 0)
            return usage(argv[0]);
        if (!value) {
            if (index + 1 >= argc) return usage(argv[0]);
            value = argv[++index];
        }
        if (strcmp(option, "--tomoe") == 0) tomoe_argument = value;
        else if (strcmp(option, "--pointer") == 0) pointer_argument = value;
        else work_argument = value;
    }
    if (!tomoe_argument || !pointer_argument) return usage(argv[0]);
    if (!find_on_path("grim")) fail("grim must be on PATH");
    if (work_argument) snprintf(root, sizeof root, "%s", work_argument);
    else {
        const char *temporary = getenv("TMPDIR");
        if (!temporary || !*temporary) temporary = "/tmp";
        snprintf(root, sizeof root, "%s/tomoe-cursor-XXXXXX", temporary);
        if (!mkdtemp(root)) fail("cannot create a temporary directory");
    }
    harness_mkparents(root, 0777);
    write_theme(root);
    char tomoe_path[4096], pointer_path[4096];
    absolute_path(tomoe_argument, tomoe_path, sizeof tomoe_path);
    absolute_path(pointer_argument, pointer_path, sizeof pointer_path);
    tomoe_binary = tomoe_path;
    pointer_binary = pointer_path;
    atexit(cleanup);

    char plain_work[4096], body[8192], reference[4096];
    harness_join(plain_work, sizeof plain_work, root, "plain");
    harness_mkparents(plain_work, 0777);
    build_body(body, sizeof body, "");
    struct compositor plain;
    compositor_start(&plain, plain_work, body, 1);
    harness_sleep(1.0);
    park(&plain);
    screenshot(&plain, "plain-rest.ppm", reference, sizeof reference);
    struct signature plain_shape = signature_of(reference);
    struct point walk[2] = { { PARK_X, PARK_Y }, { 900, PARK_Y } };
    struct mover moving = mover_start(&plain, walk, 2, 8, 10, 0);
    char **frames = NULL;
    size_t frame_count = 0, frame_capacity = 0;
    while (mover_running(&moving)) {
        char name[64], path[4096];
        snprintf(name, sizeof name, "walk-%03d.ppm", (int)frame_count);
        screenshot(&plain, name, path, sizeof path);
        if (frame_count + 1 >= frame_capacity) {
            frame_capacity = frame_capacity ? frame_capacity * 2 : 64;
            char **grown = realloc(frames, frame_capacity * sizeof frames[0]);
            if (!grown) harness_fail("cursor: out of memory");
            frames = grown;
        }
        frames[frame_count] = strdup(path);
        if (!frames[frame_count]) harness_fail("cursor: out of memory");
        frame_count++;
        harness_sleep(0.02);
    }
    harness_sleep(0.6);
    int changed = 0, deformed = 0;
    for (size_t index = 0; index < frame_count; index++) {
        struct signature shape = signature_of(frames[index]);
        if (shape.count != plain_shape.count) changed = 1;
        if (!signature_equal(&shape, &plain_shape)) deformed = 1;
        free(shape.offsets);
    }
    if (changed) add_failure("plain: the cursor changed size with no cursor behaviour");
    if (deformed) add_failure("plain: the cursor was deformed with no cursor behaviour");
    struct point nudge[2] = { { PARK_X, PARK_Y }, { PARK_X + 1, PARK_Y } };
    stroke(&plain, nudge, 2, 1, 8, 80);
    harness_sleep(0.6);
    long drawn = 0, wakeups = 0;
    idle_measure(&plain, 2, &drawn, &wakeups);
    printf("plain: %ld frames and %ld wakeups while idle\n", drawn, wakeups);
    if (drawn) add_failure("plain drew %ld frames while idle", drawn);
    plain_wakeups = wakeups;
    static const char *const variants[2] = { "check-red", "check-blue" };
    static const char *const wanted[2] = { "red", "blue" };
    char icons[4096];
    harness_join(icons, sizeof icons, root, "icons");
    for (int index = 0; index < 2; index++) {
        relink_theme(icons, variants[index]);
        harness_sleep(0.4);
        char name[64], path[4096];
        snprintf(name, sizeof name, "theme-%s.ppm", wanted[index]);
        screenshot(&plain, name, path, sizeof path);
        long red = 0, blue = 0;
        tint_of(path, PARK_X, PARK_Y, 24, &red, &blue);
        printf("theme %s: red %ld blue %ld at the still pointer\n", variants[index], red, blue);
        if ((red > blue) != (strcmp(wanted[index], "red") == 0))
            add_failure("plain: relinking the theme to %s left the still cursor %s",
                    variants[index], red > blue ? "red" : "blue");
    }
    char late[4096], late_link[4096], red_theme[4096];
    harness_join(late, sizeof late, root, "late");
    harness_concat(late, sizeof late, late, "/icons");
    harness_mkparents(late, 0777);
    harness_join(late_link, sizeof late_link, late, "check");
    harness_join(red_theme, sizeof red_theme, icons, "check-red");
    if (symlink(red_theme, late_link) != 0) fail("cannot create a late theme link");
    harness_sleep(0.4);
    char late_path[4096];
    screenshot(&plain, "theme-late.ppm", late_path, sizeof late_path);
    long late_red = 0, late_blue = 0;
    tint_of(late_path, PARK_X, PARK_Y, 24, &late_red, &late_blue);
    printf("theme in a directory made after start: red %ld blue %ld\n", late_red, late_blue);
    if (late_red <= late_blue)
        add_failure("plain: a theme in a search directory created after start was never drawn");
    for (size_t index = 0; index < frame_count; index++) free(frames[index]);
    free(frames);
    compositor_stop(&plain);
    remove_matching(plain_work, "walk-", ".ppm");

    char tilt_body[8192], stretch_body[8192];
    snprintf(tilt_body, sizeof tilt_body, "%s%s", lisp_tilt, extension_tilt);
    snprintf(stretch_body, sizeof stretch_body, "%s%s", lisp_stretch, extension_stretch);
    check_run("tilt", tilt_body, reference, moved, walk, 2, 16, 20);
    check_run("stretch", stretch_body, reference, moved, walk, 2, 16, 20);
    check_run("spring", lisp_spring, reference, settled_late, walk, 2, 16, 20);
    check_run("trail", lisp_trail, reference, trailed, walk, 2, 16, 20);
    check_run("shake", lisp_shake, reference, magnified, zigzag,
            sizeof zigzag / sizeof zigzag[0], 3, 6);

    char unload_work[4096], unload_body[8192];
    harness_join(unload_work, sizeof unload_work, root, "unload");
    harness_mkparents(unload_work, 0777);
    snprintf(unload_body, sizeof unload_body, "%s%s%s", lisp_configure, lisp_throwaway,
            extension_jelly);
    struct compositor unload;
    compositor_start(&unload, unload_work, unload_body, 1);
    harness_sleep(1.0);
    char rest[4096], settled[4096], unmounted[4096];
    struct shots shots;
    memset(&shots, 0, sizeof shots);
    case_run(&unload, walk, 2, 16, 20, rest, sizeof rest, &shots, settled, sizeof settled);
    if (!same_image(rest, reference))
        add_failure("unload: the resting cursor is not the plain image");
    int unload_deformed = 0;
    for (size_t index = 0; index < shots.count && !unload_deformed; index++)
        if (!same_image(shots.paths[index], reference)) unload_deformed = 1;
    if (!unload_deformed)
        add_failure("unload: the user's effect never deformed the cursor");
    if (!same_image(settled, reference))
        add_failure("unload: the user's effect did not settle to the plain image");
    compositor_write_policy(&unload, "");
    struct outcome reload = compositor_call(&unload, "reload");
    if (reload.status != 0) {
        char *text = outcome_text(&reload);
        failf("reload failed: %s", text);
    }
    outcome_free(&reload);
    harness_sleep(0.6);
    park(&unload);
    stroke(&unload, walk, 2, 8, 10, 0);
    harness_sleep(0.9);
    park(&unload);
    screenshot(&unload, "unmounted.ppm", unmounted, sizeof unmounted);
    if (!same_image(unmounted, reference))
        add_failure("unload: removing the effect did not restore the plain image");
    idle_measure(&unload, 2, &drawn, &wakeups);
    printf("unload: %ld idle frames, %ld idle wakeups after the effect was removed\n", drawn,
            wakeups);
    if (drawn) add_failure("unload: drew %ld frames while idle after unmounting", drawn);
    shots_free(&shots);
    compositor_stop(&unload);

    if (failure_count) {
        fprintf(stderr, "cursor: FAILED\n  %s", failures[0]);
        for (size_t index = 1; index < failure_count; index++)
            fprintf(stderr, "\n  %s", failures[index]);
        fprintf(stderr, "\ncursor: work directory %s\n", root);
        return 1;
    }
    free(plain_shape.offsets);
    printf("cursor: every mode deformed the cursor and returned to the plain image\n");
    return 0;
}
