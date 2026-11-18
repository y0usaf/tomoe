#include "harness.h"

#include <ctype.h>
#include <dirent.h>
#include <poll.h>
#include <stdarg.h>

#define LINES 4000
#define WAIT 180.0
#define SLOWEST 0.25
#define ANSWERS 20
#define QUICK 0.2
#define FACTOR 25
#define SURFACE_SIZE 64
#define SURFACE "SHADER-WAIT"

static const char POLICY[] =
    "(in-package #:tomoe-user)\n"
    "\n"
    "(define-extension \"shader-wait\" (:reads (:outputs) :state nil) (snapshot state event)\n"
    "  (declare (ignore state event))\n"
    "  (let ((output (first (context snapshot :outputs))))\n"
    "    (values nil\n"
    "            (append\n"
    "             (when output\n"
    "               (list (configure-output (getf output :name) :mode (list 1280 720)\n"
    "                                       :scale 1 :position '(0 0))))\n"
    "             (list (shell-surface :shader-wait (ui :stack)\n"
    "                                  :anchors '(:top :left)\n"
    "                                  :layer :top\n"
    "                                  :click-through t\n"
    "                                  :width %d :height %d\n"
    "                                  :margin '(%lld 0 0 %lld)\n"
    "                                  :background (list :shader \"%s\" :fps 0))))\n"
    "            nil)))\n";

struct box {
    long long x, y, width, height;
};

struct reply {
    int status;
    char *text;
};

struct buffer {
    char *data;
    size_t size, capacity;
};

struct tool_output {
    int status;
    char *out;
    char *err;
};

struct image {
    long long width, height;
    unsigned char *raw;
    unsigned char *pixels;
    size_t length;
};

struct names {
    char **items;
    size_t count, capacity;
};

static const char *tomoe_path;
static char socket_name[] = "shader-wait";
static char work_dir[4096];
static char runtime_dir[4096];
static char home_dir[4096];
static char cache_dir[4096];
static char log_path[4096];
static pid_t process;

static void failure(const char *format, ...) __attribute__((noreturn));

static void failure(const char *format, ...) {
    char message[8192], line[8300];
    va_list arguments;
    va_start(arguments, format);
    vsnprintf(message, sizeof message, format, arguments);
    va_end(arguments);
    snprintf(line, sizeof line, "shader-wait: %s", message);
    harness_say(line);
    exit(1);
}

static void make_absolute(char *out, size_t size, const char *path) {
    if (path[0] == '/') {
        snprintf(out, size, "%s", path);
    } else {
        char directory[4096];
        if (!getcwd(directory, sizeof directory)) failure("cannot find the current directory");
        harness_join(out, size, directory, path);
    }
    size_t length = strlen(out);
    while (length > 1 && out[length - 1] == '/') out[--length] = '\0';
}

static void write_shader(const char *path, double seed) {
    FILE *file = fopen(path, "w");
    if (!file) failure("cannot write %s", path);
    fputs("void mainImage(out vec4 color, in vec2 at) {\n", file);
    fputs("    float s = 0.0;\n", file);
    for (int index = 0; index < LINES; index++)
        fprintf(file, "    s = s * 1.0001 + at.y * %.6f;\n", seed + (double)index * 0.000001);
    fputs("    color = vec4(fract(s), fract(s * 1.7), 0.35, 1.0);\n}\n", file);
    if (fclose(file) != 0) failure("cannot write %s", path);
}

static void write_policy(const char *path, const char *shader, long long left, long long top) {
    char text[8192];
    int length = snprintf(text, sizeof text, POLICY, SURFACE_SIZE, SURFACE_SIZE, top, left, shader);
    if (length < 0 || (size_t)length >= sizeof text) harness_fail("the policy is too long");
    FILE *file = fopen(path, "w");
    if (!file) failure("cannot write %s", path);
    if (fwrite(text, 1, (size_t)length, file) != (size_t)length) failure("cannot write %s", path);
    if (fclose(file) != 0) failure("cannot write %s", path);
}

static void buffer_append(struct buffer *buffer, const char *data, size_t length) {
    if (buffer->size + length + 1 > buffer->capacity) {
        size_t capacity = buffer->capacity ? buffer->capacity : 4096;
        while (capacity < buffer->size + length + 1) capacity *= 2;
        char *grown = realloc(buffer->data, capacity);
        if (!grown) harness_fail("out of memory");
        buffer->data = grown;
        buffer->capacity = capacity;
    }
    memcpy(buffer->data + buffer->size, data, length);
    buffer->size += length;
    buffer->data[buffer->size] = '\0';
}

static char *buffer_take(struct buffer *buffer) {
    if (!buffer->data) {
        buffer->data = strdup("");
        if (!buffer->data) harness_fail("out of memory");
    }
    char *text = buffer->data;
    buffer->data = NULL;
    buffer->size = 0;
    buffer->capacity = 0;
    return text;
}

static struct tool_output run_tool(char *const argv[], double timeout) {
    struct tool_output output = { -1, NULL, NULL };
    int pipes[2][2];
    if (pipe(pipes[0]) != 0 || pipe(pipes[1]) != 0) harness_fail("cannot make a pipe");
    pid_t child = fork();
    if (child < 0) harness_fail("cannot fork");
    if (child == 0) {
        dup2(pipes[0][1], STDOUT_FILENO);
        dup2(pipes[1][1], STDERR_FILENO);
        close(pipes[0][0]);
        close(pipes[0][1]);
        close(pipes[1][0]);
        close(pipes[1][1]);
        execvp(argv[0], argv);
        _exit(127);
    }
    close(pipes[0][1]);
    close(pipes[1][1]);
    struct buffer streams[2] = { { 0 }, { 0 } };
    int open_fds[2] = { pipes[0][0], pipes[1][0] };
    int remaining = 2;
    double deadline = harness_now() + timeout;
    char chunk[4096];
    while (remaining > 0) {
        struct pollfd waiting[2];
        for (int index = 0; index < 2; index++) {
            waiting[index].fd = open_fds[index];
            waiting[index].events = POLLIN;
            waiting[index].revents = 0;
        }
        double left = deadline - harness_now();
        if (left <= 0) break;
        int ready = poll(waiting, 2, (int)(left * 1000.0) + 1);
        if (ready < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (ready == 0) continue;
        for (int index = 0; index < 2; index++) {
            if (open_fds[index] < 0 || !waiting[index].revents) continue;
            ssize_t got = read(open_fds[index], chunk, sizeof chunk);
            if (got < 0) {
                if (errno == EINTR) continue;
                close(open_fds[index]);
                open_fds[index] = -1;
                remaining--;
                continue;
            }
            if (got == 0) {
                close(open_fds[index]);
                open_fds[index] = -1;
                remaining--;
                continue;
            }
            buffer_append(&streams[index], chunk, (size_t)got);
        }
    }
    for (int index = 0; index < 2; index++)
        if (open_fds[index] >= 0) close(open_fds[index]);
    int status = 0;
    if (remaining > 0) {
        kill(child, SIGKILL);
        while (waitpid(child, &status, 0) < 0 && errno == EINTR) {}
        output.status = -1;
    } else {
        while (waitpid(child, &status, 0) < 0) {
            if (errno != EINTR) {
                status = 0;
                break;
            }
        }
        if (WIFEXITED(status))
            output.status = WEXITSTATUS(status);
        else
            output.status = 128 + (WIFSIGNALED(status) ? WTERMSIG(status) : 0);
    }
    output.out = buffer_take(&streams[0]);
    output.err = buffer_take(&streams[1]);
    return output;
}

static struct reply cli_timeout(const char *command, double timeout) {
    char *argv[] = { (char *)tomoe_path, "--socket", socket_name, (char *)command, NULL };
    struct tool_output tool = run_tool(argv, timeout);
    size_t out = strlen(tool.out), err = strlen(tool.err);
    struct reply reply;
    reply.status = tool.status;
    reply.text = malloc(out + err + 1);
    if (!reply.text) harness_fail("out of memory");
    memcpy(reply.text, tool.out, out);
    memcpy(reply.text + out, tool.err, err + 1);
    free(tool.out);
    free(tool.err);
    return reply;
}

static struct reply cli(const char *command) {
    return cli_timeout(command, WAIT);
}

static char *log_text(void) {
    FILE *file = fopen(log_path, "r");
    if (!file) return strdup("");
    size_t capacity = 65536, size = 0;
    char *text = malloc(capacity);
    if (!text) harness_fail("out of memory");
    for (;;) {
        if (size + 4096 > capacity) {
            capacity *= 2;
            char *grown = realloc(text, capacity);
            if (!grown) harness_fail("out of memory");
            text = grown;
        }
        size_t got = fread(text + size, 1, capacity - size - 1, file);
        if (got == 0) break;
        size += got;
    }
    fclose(file);
    text[size] = '\0';
    return text;
}

static void screenshot(const char *path) {
    char *argv[] = { "grim", "-t", "ppm", (char *)path, NULL };
    struct tool_output tool = run_tool(argv, 15.0);
    if (tool.status == -1) {
        free(tool.out);
        free(tool.err);
        failure("the session did not answer a screen capture: it was not drawing");
    }
    if (tool.status != 0) {
        char message[8192];
        snprintf(message, sizeof message, "cannot capture the screen: %s", tool.err);
        free(tool.out);
        free(tool.err);
        failure("%s", message);
    }
    free(tool.out);
    free(tool.err);
}

static struct image read_ppm(const char *path) {
    FILE *file = fopen(path, "rb");
    if (!file) failure("cannot read %s", path);
    size_t capacity = 1 << 20, size = 0;
    unsigned char *raw = malloc(capacity);
    if (!raw) harness_fail("out of memory");
    for (;;) {
        if (size == capacity) {
            capacity *= 2;
            unsigned char *grown = realloc(raw, capacity);
            if (!grown) harness_fail("out of memory");
            raw = grown;
        }
        size_t got = fread(raw + size, 1, capacity - size, file);
        if (got == 0) break;
        size += got;
    }
    fclose(file);
    size_t at = 0;
    const char *fields[4];
    for (int index = 0; index < 4; index++) {
        while (at < size && isspace(raw[at])) at++;
        fields[index] = (const char *)raw + at;
        while (at < size && !isspace(raw[at])) at++;
    }
    if (at >= size) failure("%s is not a PPM with a header", path);
    struct image image;
    image.width = strtoll(fields[1], NULL, 10);
    image.height = strtoll(fields[2], NULL, 10);
    image.raw = raw;
    image.pixels = raw + at + 1;
    image.length = size - at - 1;
    return image;
}

static long long surface_ink(const char *path, const struct box *box) {
    struct image image = read_ppm(path);
    long long column = box->x - 8, row = box->y - 8;
    if (column < 0) column = 0;
    if (row < 0) row = 0;
    long long background = (row * image.width + column) * 3;
    long long lit = 0;
    long long end_row = box->y + box->height, end_column = box->x + box->width;
    if (end_row > image.height) end_row = image.height;
    if (end_column > image.width) end_column = image.width;
    for (row = box->y; row < end_row; row++) {
        for (column = box->x; column < end_column; column++) {
            long long offset = (row * image.width + column) * 3;
            if (offset < 0 || offset + 3 > (long long)image.length) {
                lit++;
                continue;
            }
            if (background < 0 || background + 3 > (long long)image.length) {
                lit++;
                continue;
            }
            if (memcmp(image.pixels + offset, image.pixels + background, 3) != 0) lit++;
        }
    }
    free(image.raw);
    return lit;
}

static const char *expect_literal(const char *text, const char *literal) {
    size_t length = strlen(literal);
    if (strncmp(text, literal, length) != 0) return NULL;
    return text + length;
}

static const char *expect_integer(const char *text, int sign, long long *value) {
    const char *at = text;
    int negative = 0;
    if (sign && *at == '-') {
        negative = 1;
        at++;
    }
    if (!isdigit((unsigned char)*at)) return NULL;
    long long number = 0;
    while (isdigit((unsigned char)*at)) {
        number = number * 10 + (*at - '0');
        at++;
    }
    *value = negative ? -number : number;
    return at;
}

static int parse_surface_box(const char *text, struct box *box) {
    static const char prefix[] = ":NAME :" SURFACE " :OUTPUT \"";
    size_t length = strlen(prefix);
    const char *at = text;
    while ((at = strstr(at, prefix)) != NULL) {
        const char *quote = strchr(at + length, '"');
        if (!quote) return 0;
        const char *part = quote + 1;
        long long x = 0, y = 0, width = 0, height = 0;
        const char *next = expect_literal(part, " :X ");
        if (next) next = expect_integer(next, 1, &x);
        if (next) next = expect_literal(next, " :Y ");
        if (next) next = expect_integer(next, 1, &y);
        if (next) next = expect_literal(next, " :WIDTH ");
        if (next) next = expect_integer(next, 0, &width);
        if (next) next = expect_literal(next, " :HEIGHT ");
        if (next) next = expect_integer(next, 0, &height);
        if (next) {
            box->x = x;
            box->y = y;
            box->width = width;
            box->height = height;
            return 1;
        }
        at++;
    }
    return 0;
}

static int surface_box(struct box *box) {
    struct reply reply = cli("inspect");
    int found = parse_surface_box(reply.text, box);
    free(reply.text);
    return found;
}

static void names_push(struct names *list, const char *name) {
    if (list->count == list->capacity) {
        list->capacity = list->capacity ? list->capacity * 2 : 32;
        char **grown = realloc(list->items, list->capacity * sizeof *grown);
        if (!grown) harness_fail("out of memory");
        list->items = grown;
    }
    list->items[list->count] = strdup(name);
    if (!list->items[list->count]) harness_fail("out of memory");
    list->count++;
}

static void list_cache(const char *path, struct names *list) {
    DIR *dir = opendir(path);
    if (!dir) return;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
        names_push(list, entry->d_name);
    }
    closedir(dir);
}

static int names_added(const struct names *before, const struct names *now) {
    for (size_t index = 0; index < now->count; index++) {
        int seen = 0;
        for (size_t other = 0; other < before->count; other++)
            if (!strcmp(now->items[index], before->items[other])) {
                seen = 1;
                break;
            }
        if (!seen) return 1;
    }
    return 0;
}

static void names_free(struct names *list) {
    for (size_t index = 0; index < list->count; index++) free(list->items[index]);
    free(list->items);
    list->items = NULL;
    list->count = 0;
    list->capacity = 0;
}

static int compare_doubles(const void *left, const void *right) {
    double first = *(const double *)left, second = *(const double *)right;
    return (first > second) - (first < second);
}

static void cleanup(void) {
    if (process <= 0) return;
    int status = 0;
    pid_t done = waitpid(process, &status, WNOHANG);
    if (done == process || done < 0) {
        process = 0;
        return;
    }
    char *argv[] = { (char *)tomoe_path, "--socket", socket_name, "quit", NULL };
    struct tool_output tool = run_tool(argv, 10.0);
    free(tool.out);
    free(tool.err);
    if (harness_wait_status(process, 60) < 0) {
        kill(process, SIGTERM);
        if (harness_wait_status(process, 10) < 0) {
            kill(process, SIGKILL);
            harness_wait_status(process, 10);
        }
    }
    process = 0;
}

static void session_init(void) {
    harness_mkparents(work_dir, 0755);
    harness_join(runtime_dir, sizeof runtime_dir, work_dir, "run");
    harness_join(home_dir, sizeof home_dir, work_dir, "home");
    harness_mkparents(runtime_dir, 0700);
    chmod(runtime_dir, 0700);
    char cache_home[4096], cache_tomoe[4096];
    harness_join(cache_home, sizeof cache_home, home_dir, ".cache");
    harness_mkparents(cache_home, 0755);
    harness_join(cache_tomoe, sizeof cache_tomoe, cache_home, "tomoe");
    harness_join(cache_dir, sizeof cache_dir, cache_tomoe, "shaders");
    harness_join(log_path, sizeof log_path, work_dir, "tomoe.log");
    setenv("HOME", home_dir, 1);
    setenv("XDG_RUNTIME_DIR", runtime_dir, 1);
    setenv("XDG_CACHE_HOME", cache_home, 1);
    setenv("WAYLAND_DISPLAY", socket_name, 1);
    setenv("TOMOE_DEBUG", "1", 1);
    setenv("MESA_LOADER_DRIVER_OVERRIDE", "zink", 1);
    setenv("LIBGL_ALWAYS_SOFTWARE", "1", 1);
    atexit(cleanup);
}

static void start(const char *policy) {
    char *argv[] = { (char *)tomoe_path, "--socket", socket_name, "--backend", "headless",
        "--bare", "--no-watch", "--config", (char *)policy, NULL };
    process = harness_spawn(argv, environ, NULL, NULL, log_path, 0);
    if (process <= 0) failure("cannot start %s", tomoe_path);
    double deadline = harness_now() + 60.0;
    while (harness_now() < deadline) {
        int status = 0;
        if (waitpid(process, &status, WNOHANG) == process) {
            process = 0;
            failure("tomoe exited while starting:\n%s", log_text());
        }
        struct reply reply = cli("inspect");
        int code = reply.status;
        free(reply.text);
        if (code == 0) return;
    }
    failure("tomoe never answered inspect:\n%s", log_text());
}

static double settle(void) {
    double deadline = harness_now() + 30.0;
    double *times = NULL;
    size_t count = 0, capacity = 0;
    int quick = 0;
    while (harness_now() < deadline) {
        double at = harness_now();
        struct reply reply = cli("inspect");
        double took = harness_now() - at;
        quick = (reply.status == 0 && took < QUICK) ? quick + 1 : 0;
        free(reply.text);
        if (count == capacity) {
            capacity = capacity ? capacity * 2 : 64;
            double *grown = realloc(times, capacity * sizeof *grown);
            if (!grown) harness_fail("out of memory");
            times = grown;
        }
        times[count++] = took;
        if (quick >= 3) {
            qsort(times, count, sizeof *times, compare_doubles);
            double median = times[count / 2];
            free(times);
            return median;
        }
        harness_sleep(0.05);
    }
    failure("the session never answered %d times inside %.0f ms:\n%s", 3, QUICK * 1000.0,
            log_text());
}

static double probe(const char *label, double settled) {
    double limit = SLOWEST > settled * FACTOR ? SLOWEST : settled * FACTOR;
    struct names before = { 0 };
    list_cache(cache_dir, &before);
    double *answered = NULL;
    size_t count = 0, capacity = 0;
    double slowest = 0.0, when = 0.0;
    double start_at = harness_now();
    for (;;) {
        double at = harness_now();
        struct reply reply = cli("inspect");
        double took = harness_now() - at;
        if (reply.status != 0) failure("%s: inspect failed while a shader compiled: %.400s",
                                       label, reply.text);
        free(reply.text);
        if (count == capacity) {
            capacity = capacity ? capacity * 2 : 64;
            double *grown = realloc(answered, capacity * sizeof *grown);
            if (!grown) harness_fail("out of memory");
            answered = grown;
        }
        answered[count++] = took;
        if (took > slowest) {
            slowest = took;
            when = harness_now() - start_at;
        }
        struct names now = { 0 };
        list_cache(cache_dir, &now);
        int fresh = names_added(&before, &now);
        names_free(&now);
        if (fresh) break;
        if (harness_now() - start_at > WAIT) {
            failure("%s: the copy of Tomoe never wrote a program within %.0f s:\n%s", label,
                    WAIT, log_text());
        }
        harness_sleep(0.01);
    }
    double took = harness_now() - start_at;
    qsort(answered, count, sizeof *answered, compare_doubles);
    double median = answered[count / 2];
    char line[4096];
    snprintf(line, sizeof line, "%s: %zu answers while the copy compiled for %.2f s, the median "
             "%.0f ms and the slowest %.0f ms at %.2f s, settled %.0f ms, limit %.0f ms", label,
             count, took, median * 1000.0, slowest * 1000.0, when, settled * 1000.0,
             limit * 1000.0);
    harness_say(line);
    if (count < ANSWERS) failure("%s: only %zu answers in %.2f s, so the session was not running "
                                 "while the driver compiled", label, count, took);
    if (median > limit) failure("%s: the session took %.0f ms at the median to answer while a "
                                "shader compiled, past %.0f ms: it waited for the copy", label,
                                median * 1000.0, limit * 1000.0);
    free(answered);
    names_free(&before);
    return took;
}

static void fallback(const char *label) {
    char path[4096];
    harness_join(path, sizeof path, work_dir, label);
    harness_concat(path, sizeof path, path, "-fallback.ppm");
    for (int attempt = 0; attempt < 200; attempt++) {
        struct box box;
        if (surface_box(&box)) {
            screenshot(path);
            long long lit = surface_ink(path, &box);
            char line[4096];
            snprintf(line, sizeof line, "%s: %lld of %lld pixels of its surface at (%lld, %lld, "
                     "%lld, %lld) drew before the program landed", label, lit,
                     box.width * box.height, box.x, box.y, box.width, box.height);
            harness_say(line);
            return;
        }
        harness_sleep(0.05);
    }
    failure("%s: the session never staged the surface:\n%s", label, log_text());
}

static void wait_drawn(const char *label, const char *shader) {
    char path[4096];
    harness_join(path, sizeof path, work_dir, label);
    harness_concat(path, sizeof path, path, ".ppm");
    double deadline = harness_now() + WAIT;
    while (harness_now() < deadline) {
        struct box box;
        if (!surface_box(&box)) {
            harness_sleep(0.05);
            continue;
        }
        screenshot(path);
        long long lit = surface_ink(path, &box);
        if (lit >= box.width * box.height * 9 / 10) {
            struct stat info;
            long long kib = stat(shader, &info) == 0 ? (long long)info.st_size / 1024 : 0;
            char line[4096];
            snprintf(line, sizeof line, "%s: the %lld KiB shader drew %lld of %lld pixels of its "
                     "surface at (%lld, %lld, %lld, %lld)", label, kib, lit,
                     box.width * box.height, box.x, box.y, box.width, box.height);
            harness_say(line);
            return;
        }
        harness_sleep(0.05);
    }
    failure("%s: the shader's surface never drew once its program landed\n%s", label, log_text());
}

int main(int argc, char **argv) {
    const char *tomoe_argument = NULL, *work_argument = NULL;
    for (int index = 1; index < argc; index++) {
        const char *argument = argv[index];
        if (!strcmp(argument, "--tomoe") && index + 1 < argc) {
            tomoe_argument = argv[++index];
        } else if (!strncmp(argument, "--tomoe=", 8)) {
            tomoe_argument = argument + 8;
        } else if (!strcmp(argument, "--work") && index + 1 < argc) {
            work_argument = argv[++index];
        } else if (!strncmp(argument, "--work=", 7)) {
            work_argument = argument + 7;
        } else if (!strcmp(argument, "-h") || !strcmp(argument, "--help")) {
            printf("usage: %s --tomoe TOMOE --work WORK\n", argv[0]);
            return 0;
        } else {
            fprintf(stderr, "usage: %s --tomoe TOMOE --work WORK\n", argv[0]);
            return 2;
        }
    }
    if (!tomoe_argument || !work_argument) {
        fprintf(stderr, "usage: %s --tomoe TOMOE --work WORK\n", argv[0]);
        return 2;
    }
    static char tomoe_absolute[4096];
    make_absolute(tomoe_absolute, sizeof tomoe_absolute, tomoe_argument);
    tomoe_path = tomoe_absolute;
    make_absolute(work_dir, sizeof work_dir, work_argument);
    harness_mkparents(work_dir, 0755);
    char first[4096], second[4096], policy[4096];
    harness_join(first, sizeof first, work_dir, "first.glsl");
    harness_join(second, sizeof second, work_dir, "second.glsl");
    write_shader(first, 0.25);
    write_shader(second, 0.75);
    session_init();
    harness_join(policy, sizeof policy, work_dir, "policy.lisp");
    write_policy(policy, first, 480, 360);
    start(policy);
    double settled = settle();
    fallback("first");
    double started = harness_now();
    probe("first", settled);
    wait_drawn("first", first);
    char line[4096];
    snprintf(line, sizeof line, "first: drawn %.2f s after it was asked for", harness_now() - started);
    harness_say(line);
    write_policy(policy, second, 480, 520);
    struct reply reload = cli("reload");
    if (reload.status != 0)
        failure("the config with a second shader did not load: %.400s", reload.text);
    free(reload.text);
    fallback("second");
    started = harness_now();
    probe("second", settled);
    wait_drawn("second", second);
    snprintf(line, sizeof line, "second: drawn %.2f s after it was asked for",
             harness_now() - started);
    harness_say(line);
    char *text = log_text();
    const char *could = strstr(text, "shader copy could not");
    const char *finish = strstr(text, "shader copy did not finish");
    const char *fatal = !could ? finish : !finish ? could : could < finish ? could : finish;
    if (fatal) {
        char match[64];
        size_t length = fatal == could ? strlen("shader copy could not")
                                       : strlen("shader copy did not finish");
        if (length >= sizeof match) length = sizeof match - 1;
        memcpy(match, fatal, length);
        match[length] = '\0';
        failure("the copy of Tomoe did not compile a shader: %s", match);
    }
    free(text);
    harness_say("shader-wait: a shader the driver had not built kept every answer inside the "
                "session's own range, and each surface drew once its program landed");
    return 0;
}
