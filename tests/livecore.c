#include "harness.h"

#include <ctype.h>
#include <dirent.h>
#include <poll.h>
#include <pthread.h>
#include <regex.h>
#include <stdarg.h>
#include <strings.h>

#define SOCKET "livecore"
#define MAX_WINDOWS 16
#define MAX_FRAMES 64

struct phase {
    const char *name;
    int code;
};

static const struct phase phases[] = {
    { "pre", 183 },
    { "inject1", 189 },
    { "inject2", 190 },
    { "swapped", 184 },
    { "closed", 187 },
    { "refused", 185 },
    { "second", 186 },
    { "closed2", 188 }
};

#define PHASE_COUNT (sizeof phases / sizeof phases[0])

static const char *tomoe_binary;
static const char *core_a;
static const char *core_b;
static const char *core_bad;
static const char *client_binary;
static const char *script_binary;
static const char *pointer_binary;
static const char *policy_path;
static const char *stamp_b;

static char root[4096];
static char home[4096];
static char runtime[4096];
static char log_path[4096];
static struct harness_env environment;

struct text {
    char *body;
    size_t used;
    size_t capacity;
};

static void text_grow(struct text *text, size_t extra) {
    if (text->used + extra + 1 <= text->capacity) return;
    size_t grown = text->capacity ? text->capacity : 256;
    while (grown < text->used + extra + 1) grown *= 2;
    char *next = realloc(text->body, grown);
    if (!next) harness_fail("out of memory");
    text->body = next;
    text->capacity = grown;
}

static void text_add_bytes(struct text *text, const char *value, size_t length) {
    text_grow(text, length);
    memcpy(text->body + text->used, value, length);
    text->used += length;
    text->body[text->used] = '\0';
}

static void text_add(struct text *text, const char *value) {
    text_add_bytes(text, value, strlen(value));
}

static void text_addf(struct text *text, const char *format, ...) {
    va_list arguments;
    va_start(arguments, format);
    char small[1024];
    int length = vsnprintf(small, sizeof small, format, arguments);
    va_end(arguments);
    if (length < 0) return;
    if ((size_t)length < sizeof small) {
        text_add_bytes(text, small, (size_t)length);
        return;
    }
    text_grow(text, (size_t)length);
    va_start(arguments, format);
    vsnprintf(text->body + text->used, (size_t)length + 1, format, arguments);
    va_end(arguments);
    text->used += (size_t)length;
}

static char *text_take(struct text *text) {
    if (!text->body) {
        char *empty = strdup("");
        if (!empty) harness_fail("out of memory");
        return empty;
    }
    return text->body;
}

static char *format(const char *pattern, ...) {
    struct text text = { 0 };
    va_list arguments;
    va_start(arguments, pattern);
    char small[1024];
    int length = vsnprintf(small, sizeof small, pattern, arguments);
    va_end(arguments);
    if (length < 0) return text_take(&text);
    if ((size_t)length < sizeof small) {
        text_add_bytes(&text, small, (size_t)length);
        return text_take(&text);
    }
    text_grow(&text, (size_t)length);
    va_start(arguments, pattern);
    vsnprintf(text.body, (size_t)length + 1, pattern, arguments);
    va_end(arguments);
    text.used = (size_t)length;
    return text_take(&text);
}

static void strip(char *line) {
    size_t length = strlen(line);
    while (length && isspace((unsigned char)line[length - 1])) line[--length] = '\0';
    size_t start = 0;
    while (line[start] && isspace((unsigned char)line[start])) start++;
    if (start) memmove(line, line + start, length - start + 1);
}

static void repr_quote(struct text *out, const char *value) {
    for (const char *at = value; *at; at++) {
        if (*at == '\\' || *at == '\'') text_add(out, "\\");
        char one[2] = { *at, 0 };
        text_add(out, one);
    }
}

struct sink {
    char **lines;
    size_t count;
    size_t capacity;
    pthread_mutex_t lock;
};

static void sink_init(struct sink *sink) {
    memset(sink, 0, sizeof *sink);
    pthread_mutex_init(&sink->lock, NULL);
}

static void sink_add(struct sink *sink, const char *text) {
    char *copy = strdup(text);
    if (!copy) harness_fail("out of memory");
    pthread_mutex_lock(&sink->lock);
    if (sink->count == sink->capacity) {
        size_t grown = sink->capacity ? sink->capacity * 2 : 128;
        char **next = realloc(sink->lines, grown * sizeof *next);
        if (!next) {
            pthread_mutex_unlock(&sink->lock);
            harness_fail("out of memory");
        }
        sink->lines = next;
        sink->capacity = grown;
    }
    sink->lines[sink->count++] = copy;
    pthread_mutex_unlock(&sink->lock);
}

static long sink_count(struct sink *sink, const char *pattern) {
    regex_t expression;
    if (regcomp(&expression, pattern, REG_EXTENDED) != 0)
        harness_fail("a test pattern is not a valid expression");
    long found = 0;
    pthread_mutex_lock(&sink->lock);
    for (size_t index = 0; index < sink->count; index++)
        if (regexec(&expression, sink->lines[index], 0, NULL, 0) == 0) found++;
    pthread_mutex_unlock(&sink->lock);
    regfree(&expression);
    return found;
}

static int sink_wait(struct sink *sink, const char *pattern, long wanted, double seconds) {
    double deadline = harness_now() + seconds;
    while (harness_now() < deadline) {
        if (sink_count(sink, pattern) >= wanted) return 1;
        harness_sleep(0.05);
    }
    return 0;
}

static int sink_has(struct sink *sink, const char *word) {
    int found = 0;
    pthread_mutex_lock(&sink->lock);
    for (size_t index = 0; index < sink->count; index++)
        if (!strcmp(sink->lines[index], word)) {
            found = 1;
            break;
        }
    pthread_mutex_unlock(&sink->lock);
    return found;
}

static char *sink_select(struct sink *sink, size_t tail) {
    struct text text = { 0 };
    pthread_mutex_lock(&sink->lock);
    size_t start = sink->count > tail ? sink->count - tail : 0;
    for (size_t index = start; index < sink->count; index++) {
        if (index > start) text_add(&text, "\n");
        text_add(&text, sink->lines[index]);
    }
    pthread_mutex_unlock(&sink->lock);
    return text_take(&text);
}

static void sink_repr(struct text *out, struct sink *sink, size_t tail) {
    text_add(out, "[");
    pthread_mutex_lock(&sink->lock);
    size_t start = sink->count > tail ? sink->count - tail : 0;
    for (size_t index = start; index < sink->count; index++) {
        if (index > start) text_add(out, ", ");
        text_add(out, "'");
        repr_quote(out, sink->lines[index]);
        text_add(out, "'");
    }
    pthread_mutex_unlock(&sink->lock);
    text_add(out, "]");
}

static void remove_tree(const char *path) {
    struct stat status;
    if (lstat(path, &status) != 0) return;
    if (S_ISDIR(status.st_mode)) {
        DIR *directory = opendir(path);
        if (directory) {
            struct dirent *entry;
            while ((entry = readdir(directory))) {
                if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
                char child[4096];
                harness_join(child, sizeof child, path, entry->d_name);
                remove_tree(child);
            }
            closedir(directory);
        }
        rmdir(path);
        return;
    }
    unlink(path);
}

static int status_code(int status) {
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return -WTERMSIG(status);
    return 255;
}

static struct sink client_lines;

struct window {
    char name[64];
    pid_t pid;
    int fd;
    int killed;
    int ended;
    pthread_t reader;
};

static struct window windows[MAX_WINDOWS];
static int window_count;

static pid_t compositor;
static int compositor_running;
static int compositor_code;

struct driver {
    pid_t pid;
    int fd;
    int started;
    int ended;
    pthread_t reader;
    struct sink output;
};

static struct driver input_driver;

static void *read_driver(void *argument);

static void *read_window(void *argument) {
    struct window *window = argument;
    FILE *stream = fdopen(window->fd, "r");
    if (!stream) return NULL;
    char *line = NULL;
    size_t size = 0;
    while (getline(&line, &size, stream) > 0) {
        strip(line);
        size_t needed = strlen(window->name) + strlen(line) + 3;
        char *text = malloc(needed);
        if (!text) harness_fail("out of memory");
        snprintf(text, needed, "%s: %s", window->name, line);
        sink_add(&client_lines, text);
        free(text);
    }
    free(line);
    fclose(stream);
    return NULL;
}

static pid_t spawn_streaming(char *const argv[], char **env, int *input, int *output) {
    int in_pipe[2] = { -1, -1 }, out_pipe[2] = { -1, -1 };
    if (input && pipe(in_pipe) != 0) harness_fail("cannot make a pipe");
    if (output && pipe(out_pipe) != 0) harness_fail("cannot make a pipe");
    pid_t child = fork();
    if (child < 0) harness_fail("cannot fork");
    if (child == 0) {
        if (input) {
            dup2(in_pipe[0], STDIN_FILENO);
            close(in_pipe[0]);
            close(in_pipe[1]);
        }
        if (output) {
            dup2(out_pipe[1], STDOUT_FILENO);
            dup2(out_pipe[1], STDERR_FILENO);
            close(out_pipe[0]);
            close(out_pipe[1]);
        }
        signal(SIGPIPE, SIG_DFL);
        execvpe(argv[0], argv, env);
        fprintf(stderr, "harness: cannot run %s: %s\n", argv[0], strerror(errno));
        _exit(127);
    }
    if (input) {
        close(in_pipe[0]);
        *input = in_pipe[1];
    }
    if (output) {
        close(out_pipe[1]);
        *output = out_pipe[0];
    }
    return child;
}

struct reply {
    int status;
    char *out;
    char *err;
};

static void fail(const char *message);

static struct reply cli(const char *const *arguments, size_t count, int check) {
    char *argv[16];
    argv[0] = (char *)tomoe_binary;
    argv[1] = "--socket";
    argv[2] = (char *)SOCKET;
    for (size_t index = 0; index < count; index++) argv[3 + index] = (char *)arguments[index];
    argv[3 + count] = NULL;
    int out_pipe[2], err_pipe[2];
    if (pipe(out_pipe) != 0 || pipe(err_pipe) != 0) harness_fail("cannot make a pipe");
    pid_t child = fork();
    if (child < 0) harness_fail("cannot fork");
    if (child == 0) {
        dup2(out_pipe[1], STDOUT_FILENO);
        dup2(err_pipe[1], STDERR_FILENO);
        close(out_pipe[0]);
        close(out_pipe[1]);
        close(err_pipe[0]);
        close(err_pipe[1]);
        signal(SIGPIPE, SIG_DFL);
        execvpe(argv[0], argv, env_items(&environment));
        fprintf(stderr, "harness: cannot run %s: %s\n", argv[0], strerror(errno));
        _exit(127);
    }
    close(out_pipe[1]);
    close(err_pipe[1]);
    struct text out = { 0 }, err = { 0 };
    struct text *targets[2] = { &out, &err };
    struct pollfd fds[2] = {
        { out_pipe[0], POLLIN, 0 },
        { err_pipe[0], POLLIN, 0 }
    };
    int open = 2;
    while (open > 0) {
        int ready = poll(fds, 2, -1);
        if (ready < 0) {
            if (errno == EINTR) continue;
            break;
        }
        for (int slot = 0; slot < 2; slot++) {
            if (fds[slot].fd < 0 || !fds[slot].revents) continue;
            char chunk[4096];
            ssize_t got = read(fds[slot].fd, chunk, sizeof chunk);
            if (got > 0) {
                text_add_bytes(targets[slot], chunk, (size_t)got);
                continue;
            }
            if (got < 0 && errno == EINTR) continue;
            close(fds[slot].fd);
            fds[slot].fd = -1;
            open--;
        }
    }
    int status = 0;
    for (;;) {
        pid_t done = waitpid(child, &status, 0);
        if (done == child) break;
        if (done < 0 && errno == EINTR) continue;
        break;
    }
    struct reply reply = { status_code(status), text_take(&out), text_take(&err) };
    if (check && reply.status != 0) {
        struct text message = { 0 };
        text_add(&message, "tomoe");
        for (size_t index = 0; index < count; index++) {
            text_add(&message, " ");
            text_add(&message, arguments[index]);
        }
        text_addf(&message, " exited %d:\n", reply.status);
        text_add(&message, reply.out);
        text_add(&message, reply.err);
        free(reply.out);
        free(reply.err);
        fail(text_take(&message));
    }
    return reply;
}

static char *cli_stdout(const char *const *arguments, size_t count) {
    struct reply reply = cli(arguments, count, 1);
    free(reply.err);
    return reply.out;
}

struct window_info {
    long long id;
    char app[512];
    int focused;
    int mapped;
};

struct window_table {
    struct window_info items[MAX_WINDOWS * 2];
    int count;
};

static struct window_table windows_load(void) {
    const char *arguments[] = { "msg", "windows" };
    char *text = cli_stdout(arguments, 2);
    struct window_table table;
    memset(&table, 0, sizeof table);
    const char *at = strchr(text, '[');
    if (!at) {
        free(text);
        fail("the window list is not an array");
    }
    int depth = 0, quoted = 0;
    const char *start = NULL;
    for (at++; *at; at++) {
        if (quoted) {
            if (*at == '\\' && at[1]) {
                at++;
                continue;
            }
            if (*at == '"') quoted = 0;
            continue;
        }
        if (*at == '"') {
            quoted = 1;
            continue;
        }
        if (*at == '{') {
            if (!depth) start = at;
            depth++;
            continue;
        }
        if (*at != '}') continue;
        if (depth <= 0) continue;
        depth--;
        if (depth || !start) continue;
        size_t length = (size_t)(at - start) + 1;
        char object[8192];
        if (length >= sizeof object) {
            free(text);
            fail("a window record is too long");
        }
        memcpy(object, start, length);
        object[length] = '\0';
        if (table.count >= (int)(sizeof table.items / sizeof table.items[0])) {
            free(text);
            fail("too many windows");
        }
        struct window_info *info = &table.items[table.count];
        if (!json_integer(object, "id", &info->id)) {
            free(text);
            fail("a window has no id");
        }
        if (!json_string(object, "app_id", info->app, sizeof info->app)) {
            free(text);
            fail("a window has no app_id");
        }
        int flag = 0;
        if (json_boolean(object, "focused", &flag)) info->focused = flag;
        flag = 0;
        if (json_boolean(object, "mapped", &flag)) info->mapped = flag;
        table.count++;
    }
    free(text);
    return table;
}

static int identities_equal(const struct window_table *left, const struct window_table *right) {
    if (left->count != right->count) return 0;
    for (int index = 0; index < left->count; index++) {
        const struct window_info *match = NULL;
        for (int other = 0; other < right->count; other++)
            if (right->items[other].id == left->items[index].id)
                match = &right->items[other];
        if (!match || strcmp(match->app, left->items[index].app) != 0) return 0;
    }
    return 1;
}

static int compare_strings(const void *left, const void *right) {
    return strcmp(*(const char *const *)left, *(const char *const *)right);
}

static int identities_are(const struct window_table *table, const char *const *expected,
        int expected_count) {
    if (table->count != expected_count) return 0;
    const char *values[MAX_WINDOWS * 2];
    const char *wanted[MAX_WINDOWS * 2];
    for (int index = 0; index < table->count; index++) values[index] = table->items[index].app;
    for (int index = 0; index < expected_count; index++) wanted[index] = expected[index];
    qsort(values, (size_t)table->count, sizeof values[0], compare_strings);
    qsort(wanted, (size_t)expected_count, sizeof wanted[0], compare_strings);
    for (int index = 0; index < expected_count; index++)
        if (strcmp(values[index], wanted[index]) != 0) return 0;
    return 1;
}

static int identities_has(const struct window_table *table, const char *name) {
    for (int index = 0; index < table->count; index++)
        if (!strcmp(table->items[index].app, name)) return 1;
    return 0;
}

static int identities_mapped(const struct window_table *table) {
    for (int index = 0; index < table->count; index++)
        if (!table->items[index].mapped) return 0;
    return 1;
}

static void identities_repr(struct text *out, const struct window_table *table) {
    text_add(out, "{");
    for (int index = 0; index < table->count; index++) {
        if (index) text_add(out, ", ");
        text_addf(out, "%lld: '", table->items[index].id);
        repr_quote(out, table->items[index].app);
        text_add(out, "'");
    }
    text_add(out, "}");
}

static int focused_app(char *out, size_t size) {
    struct window_table table = windows_load();
    for (int index = 0; index < table.count; index++)
        if (table.items[index].focused) {
            snprintf(out, size, "%s", table.items[index].app);
            return 1;
        }
    return 0;
}

static size_t frames_read(long long *values, size_t capacity) {
    const char *arguments[] = { "frames" };
    char *text = cli_stdout(arguments, 1);
    size_t found = 0;
    for (const char *at = text; *at && found < capacity; at++) {
        if (strncasecmp(at, ":frame ", 7) != 0) continue;
        const char *digits = at + 7;
        if (!isdigit((unsigned char)*digits)) continue;
        values[found++] = strtoll(digits, NULL, 10);
    }
    free(text);
    if (!found) {
        values[0] = 0;
        return 1;
    }
    return found;
}

static int drawing(double seconds) {
    long long before[MAX_FRAMES], after[MAX_FRAMES];
    size_t before_count = frames_read(before, MAX_FRAMES);
    double deadline = harness_now() + seconds;
    while (harness_now() < deadline) {
        harness_sleep(0.25);
        size_t after_count = frames_read(after, MAX_FRAMES);
        if (before_count) {
            size_t pairs = before_count < after_count ? before_count : after_count;
            int larger = 1;
            for (size_t index = 0; index < pairs; index++)
                if (after[index] <= before[index]) {
                    larger = 0;
                    break;
                }
            if (larger) return 1;
        }
        memcpy(before, after, after_count * sizeof after[0]);
        before_count = after_count;
    }
    return 0;
}

static int stamp(char *out, size_t size) {
    const char *arguments[] = { "command", "core-report" };
    char *text = cli_stdout(arguments, 2);
    int found = 0;
    regex_t expression;
    if (regcomp(&expression, "\"stamp\"[[:space:]]*:[[:space:]]*\"([^\"]*)\"",
            REG_EXTENDED) == 0) {
        regmatch_t match[2];
        if (regexec(&expression, text, 2, match, 0) == 0 && match[1].rm_so >= 0) {
            size_t length = (size_t)(match[1].rm_eo - match[1].rm_so);
            if (length >= size) length = size - 1;
            memcpy(out, text + match[1].rm_so, length);
            out[length] = '\0';
            found = 1;
        }
        regfree(&expression);
    }
    if (!found && regcomp(&expression, ":stamp[[:space:]]+\"([^\"]*)\"",
            REG_EXTENDED | REG_ICASE) == 0) {
        regmatch_t match[2];
        if (regexec(&expression, text, 2, match, 0) == 0 && match[1].rm_so >= 0) {
            size_t length = (size_t)(match[1].rm_eo - match[1].rm_so);
            if (length >= size) length = size - 1;
            memcpy(out, text + match[1].rm_so, length);
            out[length] = '\0';
            found = 1;
        }
        regfree(&expression);
    }
    free(text);
    return found;
}

static int steps(long long *value) {
    const char *arguments[] = { "inspect" };
    char *text = cli_stdout(arguments, 1);
    int found = 0;
    regex_t expression;
    if (regcomp(&expression, ":LIVECORE-STEPS[[:space:]]+([0-9]+)", REG_EXTENDED) == 0) {
        regmatch_t match[2];
        if (regexec(&expression, text, 2, match, 0) == 0 && match[1].rm_so >= 0) {
            *value = strtoll(text + match[1].rm_so, NULL, 10);
            found = 1;
        }
        regfree(&expression);
    }
    free(text);
    return found;
}

static int swapped(const char *wanted, double seconds) {
    double deadline = harness_now() + seconds;
    while (harness_now() < deadline) {
        char current[512];
        if (stamp(current, sizeof current) && strcmp(current, wanted) == 0) return 1;
        harness_sleep(0.05);
    }
    return 0;
}

static int gone(const char *name, double seconds) {
    double deadline = harness_now() + seconds;
    while (harness_now() < deadline) {
        struct window_table table = windows_load();
        if (!identities_has(&table, name)) return 1;
        harness_sleep(0.05);
    }
    return 0;
}

static int window_poll(struct window *window) {
    if (window->ended) return 1;
    int status = 0;
    pid_t done = waitpid(window->pid, &status, WNOHANG);
    if (done != window->pid) return 0;
    window->ended = 1;
    return 1;
}

static int clients_ended(void) {
    for (int index = 0; index < window_count; index++)
        if (!windows[index].killed && window_poll(&windows[index])) return 1;
    return 0;
}

static void clients_ended_repr(struct text *out) {
    int first = 1;
    text_add(out, "[");
    for (int index = 0; index < window_count; index++) {
        struct window *window = &windows[index];
        if (window->killed || !window_poll(window)) continue;
        if (!first) text_add(out, ", ");
        text_add(out, "'");
        repr_quote(out, window->name);
        text_add(out, "'");
        first = 0;
    }
    text_add(out, "]");
}

static void clients_start(const char *name, const char *const *options, size_t count) {
    char *argv[32];
    argv[0] = (char *)client_binary;
    argv[1] = "--seconds";
    argv[2] = "3600";
    for (size_t index = 0; index < count; index++) argv[3 + index] = (char *)options[index];
    argv[3 + count] = NULL;
    int output = -1;
    pid_t pid = spawn_streaming(argv, env_items(&environment), NULL, &output);
    struct window *window = &windows[window_count++];
    memset(window, 0, sizeof *window);
    snprintf(window->name, sizeof window->name, "%s", name);
    window->pid = pid;
    window->fd = output;
    if (pthread_create(&window->reader, NULL, read_window, window) != 0)
        harness_fail("cannot start a reader");
}

static int clients_kill(const char *name) {
    for (int index = 0; index < window_count; index++) {
        struct window *window = &windows[index];
        if (strcmp(window->name, name) != 0) continue;
        if (!window->ended) {
            kill(window->pid, SIGKILL);
            if (harness_wait_status(window->pid, 20) < 0) fail("a client did not die");
            window->ended = 1;
        }
        window->killed = 1;
        return 1;
    }
    return 0;
}

static char *clients_output(void) {
    return sink_select(&client_lines, 40);
}

static int driver_poll(void) {
    if (!input_driver.started || input_driver.ended) return 1;
    int status = 0;
    pid_t done = waitpid(input_driver.pid, &status, WNOHANG);
    if (done != input_driver.pid) return 0;
    input_driver.ended = 1;
    return 1;
}

static void *read_driver(void *argument) {
    struct driver *driver = argument;
    FILE *stream = fdopen(driver->fd, "r");
    if (!stream) return NULL;
    char *line = NULL;
    size_t size = 0;
    while (getline(&line, &size, stream) > 0) {
        strip(line);
        sink_add(&driver->output, line);
    }
    free(line);
    fclose(stream);
    return NULL;
}

static void driver_start(void) {
    char *argv[256];
    int count = 0;
    argv[count++] = (char *)script_binary;
    argv[count++] = format("sync");
    argv[count++] = format("signal %s/driver-ready", root);
    for (size_t index = 0; index < PHASE_COUNT; index++) {
        argv[count++] = format("wait %s/go-%s", root, phases[index].name);
        argv[count++] = format("move 20 20");
        argv[count++] = format("sync");
        argv[count++] = format("move 40 40");
        argv[count++] = format("sync");
        argv[count++] = format("move 70 70");
        argv[count++] = format("sync");
        argv[count++] = format("move 232 182");
        argv[count++] = format("sync");
        argv[count++] = format("move 256 206");
        argv[count++] = format("sync");
        argv[count++] = format("sleep 80");
        argv[count++] = format("key 30 down");
        argv[count++] = format("sync");
        argv[count++] = format("sleep 60");
        argv[count++] = format("key 30 up");
        argv[count++] = format("sync");
        argv[count++] = format("sleep 100");
        argv[count++] = format("key %d down", phases[index].code);
        argv[count++] = format("sync");
        argv[count++] = format("sleep 60");
        argv[count++] = format("key %d up", phases[index].code);
        argv[count++] = format("sync");
        argv[count++] = format("sleep 150");
        argv[count++] = format("signal %s/done-%s", root, phases[index].name);
    }
    argv[count++] = format("wait %s/driver-stop", root);
    argv[count] = NULL;
    input_driver.started = 1;
    input_driver.pid = spawn_streaming(argv, env_items(&environment), NULL, &input_driver.fd);
    if (pthread_create(&input_driver.reader, NULL, read_driver, &input_driver) != 0)
        harness_fail("cannot start a reader");
}

static int driver_ready(double seconds) {
    char path[4096];
    harness_join(path, sizeof path, root, "driver-ready");
    double deadline = harness_now() + seconds;
    while (harness_now() < deadline) {
        if (harness_file_exists(path)) return 1;
        if (driver_poll()) return 0;
        harness_sleep(0.05);
    }
    return 0;
}

static int driver_run(const char *phase, double seconds) {
    char name[512], go[4096], done[4096];
    snprintf(name, sizeof name, "go-%s", phase);
    harness_join(go, sizeof go, root, name);
    harness_write_file(go, "");
    snprintf(name, sizeof name, "done-%s", phase);
    harness_join(done, sizeof done, root, name);
    double deadline = harness_now() + seconds;
    while (harness_now() < deadline) {
        if (harness_file_exists(done)) return 1;
        if (driver_poll()) return 0;
        harness_sleep(0.05);
    }
    return 0;
}

static void driver_stop(void) {
    char path[4096];
    harness_join(path, sizeof path, root, "driver-stop");
    harness_write_file(path, "");
    if (!input_driver.started || input_driver.ended) return;
    kill(input_driver.pid, SIGKILL);
    if (harness_wait_status(input_driver.pid, 20) < 0) harness_fail("the input driver did not stop");
    input_driver.ended = 1;
}

struct pointer_state {
    pid_t pid;
    int input;
    int fd;
    int ended;
    pthread_t reader;
    struct sink lines;
};

static void *read_pointer(void *argument) {
    struct pointer_state *state = argument;
    FILE *stream = fdopen(state->fd, "r");
    if (!stream) return NULL;
    char *line = NULL;
    size_t size = 0;
    while (getline(&line, &size, stream) > 0) {
        strip(line);
        sink_add(&state->lines, line);
    }
    free(line);
    fclose(stream);
    return NULL;
}

static int pointer_poll(struct pointer_state *state) {
    if (state->ended) return 1;
    int status = 0;
    pid_t done = waitpid(state->pid, &status, WNOHANG);
    if (done != state->pid) return 0;
    state->ended = 1;
    return 1;
}

static int pointer_wait(struct pointer_state *state, const char *word, double seconds) {
    double deadline = harness_now() + seconds;
    while (harness_now() < deadline) {
        if (sink_has(&state->lines, word)) return 1;
        if (pointer_poll(state)) return 0;
        harness_sleep(0.05);
    }
    return sink_has(&state->lines, word);
}

static int write_all(int fd, const char *text) {
    size_t length = strlen(text), sent = 0;
    while (sent < length) {
        ssize_t used = write(fd, text + sent, length - sent);
        if (used < 0) {
            if (errno == EINTR) continue;
            return 0;
        }
        sent += (size_t)used;
    }
    return 1;
}

static void pointer_stop(struct pointer_state *state) {
    if (state->ended) return;
    kill(state->pid, SIGKILL);
    int status = 0;
    while (waitpid(state->pid, &status, 0) < 0 && errno == EINTR) {}
    state->ended = 1;
}

static void pointer_round(void) {
    struct pointer_state *state = calloc(1, sizeof *state);
    if (!state) harness_fail("out of memory");
    sink_init(&state->lines);
    char *argv[] = { (char *)pointer_binary, NULL };
    state->pid = spawn_streaming(argv, env_items(&environment), &state->input, &state->fd);
    if (pthread_create(&state->reader, NULL, read_pointer, state) != 0)
        harness_fail("cannot start a reader");
    if (!pointer_wait(state, "ready", 30)) {
        struct text message = { 0 };
        text_add(&message, "the pointer never reached a new client: ");
        sink_repr(&message, &state->lines, 6);
        pointer_stop(state);
        fail(text_take(&message));
    }
    if (!write_all(state->input, "shape 4\n")) {
        pointer_stop(state);
        fail("the pointer client stopped reading");
    }
    if (!pointer_wait(state, "ok", 30)) {
        struct text message = { 0 };
        text_add(&message, "a new client's pointer request was not answered: ");
        sink_repr(&message, &state->lines, 6);
        pointer_stop(state);
        fail(text_take(&message));
    }
    pointer_stop(state);
}

static void input_round(const char *phase, int code, const char *const *motions,
        int motion_count) {
    char target[512];
    if (!focused_app(target, sizeof target) || !target[0]) {
        struct text message = { 0 };
        text_add(&message, "no window is focused in ");
        text_add(&message, phase);
        fail(text_take(&message));
    }
    long long before_steps = 0;
    int have_steps = steps(&before_steps);
    char pattern[1024];
    snprintf(pattern, sizeof pattern, "%s: .* key %d [01]", target, code);
    long before_keys = sink_count(&client_lines, pattern);
    char patterns[8][1024];
    long before_motion[8];
    for (int index = 0; index < motion_count; index++) {
        snprintf(patterns[index], sizeof patterns[index],
            "%s: .* pointer (motion|enter) ", motions[index]);
        before_motion[index] = sink_count(&client_lines, patterns[index]);
    }
    if (!driver_run(phase, 60)) {
        char *tail = sink_select(&input_driver.output, 6);
        struct text message = { 0 };
        text_add(&message, "the input driver for ");
        text_add(&message, phase);
        text_add(&message, " stopped: ");
        text_add(&message, tail);
        free(tail);
        fail(text_take(&message));
    }
    if (!sink_wait(&client_lines, pattern, before_keys + 1, 20)) {
        char *output = clients_output();
        struct text message = { 0 };
        text_add(&message, "the window focused in ");
        text_add(&message, phase);
        text_add(&message, " received no key event: ");
        text_add(&message, output);
        free(output);
        fail(text_take(&message));
    }
    for (int index = 0; index < motion_count; index++) {
        if (!sink_wait(&client_lines, patterns[index], before_motion[index] + 1, 20)) {
            char *output = clients_output();
            struct text message = { 0 };
            text_add(&message, motions[index]);
            text_add(&message, " received no pointer event in ");
            text_add(&message, phase);
            text_add(&message, ": ");
            text_add(&message, output);
            free(output);
            fail(text_take(&message));
        }
    }
    if (have_steps) {
        long long after = 0;
        if (!steps(&after) || after != before_steps + 1) {
            struct text message = { 0 };
            text_add(&message, "the session's own key binding did not fire in ");
            text_add(&message, phase);
            fail(text_take(&message));
        }
    }
}

static void note(const char *message) {
    char text[8192];
    snprintf(text, sizeof text, "livecore: %s", message);
    harness_say(text);
}

static void dump_compositor_log(void) {
    printf("livecore: compositor log:\n");
    FILE *file = fopen(log_path, "r");
    if (file) {
        char chunk[4096];
        size_t got;
        while ((got = fread(chunk, 1, sizeof chunk, file)) > 0) fwrite(chunk, 1, got, stdout);
        fclose(file);
    }
    printf("\n");
    fflush(stdout);
}

static void dump_session_log(void) {
    char session[4096];
    harness_join(session, sizeof session, home, ".local/state/tomoe");
    harness_join(session, sizeof session, session, "livecore.log");
    struct stat status;
    if (stat(session, &status) != 0) return;
    printf("livecore: session log:\n");
    FILE *file = fopen(session, "r");
    if (file) {
        if (status.st_size > 4000) fseek(file, status.st_size - 4000, SEEK_SET);
        char chunk[4096];
        size_t got;
        while ((got = fread(chunk, 1, sizeof chunk, file)) > 0) fwrite(chunk, 1, got, stdout);
        fclose(file);
    }
    printf("\n");
    fflush(stdout);
}

static void dump_clients(void) {
    char *output = clients_output();
    printf("livecore: client output:\n%s\n", output);
    free(output);
    fflush(stdout);
}

static void dump_driver(void) {
    char *output = sink_select(&input_driver.output, 10);
    printf("livecore: driver output:\n%s\n", output);
    free(output);
    fflush(stdout);
}

static int compositor_poll(void) {
    if (!compositor_running) return 1;
    int status = 0;
    pid_t done = waitpid(compositor, &status, WNOHANG);
    if (done != compositor) return 0;
    compositor_running = 0;
    compositor_code = status_code(status);
    return 1;
}

static int compositor_reap(double seconds) {
    double deadline = harness_now() + seconds;
    for (;;) {
        if (compositor_poll()) return 1;
        if (harness_now() > deadline) return 0;
        harness_sleep(0.01);
    }
}

static void cleanup(void) {
    static int done;
    if (done) return;
    done = 1;
    if (input_driver.started) driver_stop();
    for (int index = 0; index < window_count; index++)
        if (windows[index].pid > 0 && !windows[index].ended)
            kill(windows[index].pid, SIGKILL);
    if (compositor_running) {
        kill(compositor, SIGTERM);
        if (!compositor_reap(20)) {
            kill(compositor, SIGKILL);
            compositor_reap(20);
        }
    }
}

static void fail(const char *message) {
    dump_compositor_log();
    dump_session_log();
    dump_clients();
    if (input_driver.started) dump_driver();
    cleanup();
    if (compositor_code != 0) {
        dump_compositor_log();
        dump_session_log();
        remove_tree(root);
        fprintf(stderr, "%s\n", message);
        struct text final = { 0 };
        text_addf(&final, "the compositor exited %d", compositor_code);
        harness_fail(text_take(&final));
    }
    remove_tree(root);
    harness_fail(message);
}

static void make_root(void) {
    const char *tmp = getenv("TMPDIR");
    if (!tmp || !*tmp) tmp = "/tmp";
    snprintf(root, sizeof root, "%s/tomoe-livecore-XXXXXX", tmp);
    if (!mkdtemp(root)) harness_fail("cannot create a temporary directory");
}

int main(int argc, char **argv) {
    if (argc < 10) {
        fprintf(stderr, "usage: %s TOMOE CORE_A CORE_B CORE_BAD CLIENT SCRIPT POINTER "
            "POLICY STAMP_B\n", argv[0]);
        return 1;
    }
    tomoe_binary = argv[1];
    core_a = argv[2];
    core_b = argv[3];
    core_bad = argv[4];
    client_binary = argv[5];
    script_binary = argv[6];
    pointer_binary = argv[7];
    policy_path = argv[8];
    stamp_b = argv[9];
    signal(SIGPIPE, SIG_IGN);
    sink_init(&client_lines);
    sink_init(&input_driver.output);
    make_root();
    harness_join(home, sizeof home, root, "home");
    harness_join(runtime, sizeof runtime, root, "runtime");
    harness_join(log_path, sizeof log_path, root, "compositor.log");
    if (mkdir(home, 0777) != 0 && errno != EEXIST) harness_fail("cannot make the home directory");
    if (mkdir(runtime, 0700) != 0 && errno != EEXIST) harness_fail("cannot make the runtime directory");
    chmod(runtime, 0700);
    env_reset(&environment);
    env_keep(&environment, "PATH");
    env_keep(&environment, "MESA_LOADER_DRIVER_OVERRIDE");
    env_keep(&environment, "LIBGL_DRIVERS_PATH");
    env_keep(&environment, "__EGL_VENDOR_LIBRARY_FILENAMES");
    env_keep(&environment, "LD_LIBRARY_PATH");
    env_put(&environment, "HOME", home);
    env_put(&environment, "XDG_RUNTIME_DIR", runtime);
    env_put(&environment, "WAYLAND_DISPLAY", SOCKET);
    env_put(&environment, "LIBGL_ALWAYS_SOFTWARE", "1");
    env_put(&environment, "TMPDIR", root);
    char swap_fail[4096];
    harness_join(swap_fail, sizeof swap_fail, root, "swap-fail");
    char *swap_flag = format("--test-swap-fail=%s", swap_fail);
    char *compositor_argv[] = { (char *)tomoe_binary, "--backend", "headless",
        "--socket", (char *)SOCKET, "--config", (char *)policy_path, "--no-watch",
        swap_flag, NULL };
    compositor = harness_spawn(compositor_argv, env_items(&environment), NULL, NULL, log_path, 0);
    compositor_running = 1;
    char socket_path[4096];
    harness_join(socket_path, sizeof socket_path, runtime, SOCKET);
    int answered = 0;
    for (int attempt = 0; attempt < 600; attempt++) {
        if (compositor_poll()) {
            struct text message = { 0 };
            text_add(&message, "the compositor stopped while starting:\n");
            FILE *file = fopen(log_path, "r");
            if (file) {
                char chunk[4096];
                size_t got;
                while ((got = fread(chunk, 1, sizeof chunk, file)) > 0)
                    text_add_bytes(&message, chunk, got);
                fclose(file);
            }
            fail(text_take(&message));
        }
        if (harness_file_exists(socket_path)) {
            const char *arguments[] = { "msg", "version" };
            struct reply reply = cli(arguments, 2, 0);
            free(reply.out);
            free(reply.err);
            if (reply.status == 0) {
                answered = 1;
                break;
            }
        }
        harness_sleep(0.1);
    }
    if (!answered) fail("the compositor did not answer");
    driver_start();
    if (!driver_ready(30)) {
        char *output = sink_select(&input_driver.output, (size_t)-1);
        struct text message = { 0 };
        text_add(&message, "the input driver never took the seat: ");
        text_add(&message, output);
        free(output);
        fail(text_take(&message));
    }
    const char *motions_all[] = { "core-1", "core-2", "core-3" };
    for (int index = 0; index < 3; index++) {
        char name[64], pattern[256];
        snprintf(name, sizeof name, "core-%d", index + 1);
        const char *options[] = { "--mode", "xdg", "--commit-rate", "60", "--repaint",
            "--keys", "--pointer", "--app-id", name, "--title", name, "--size", "320x200" };
        clients_start(name, options, sizeof options / sizeof options[0]);
        snprintf(pattern, sizeof pattern, "%s: mapped", name);
        if (!sink_wait(&client_lines, pattern, 1, 60)) {
            struct text message = { 0 };
            text_add(&message, "client ");
            text_add(&message, name);
            text_add(&message, " did not map");
            fail(text_take(&message));
        }
    }
    char stamp_a[512];
    if (!stamp(stamp_a, sizeof stamp_a) || !stamp_a[0])
        fail("the running core does not report its build");
    struct window_table mapped = windows_load();
    {
        const char *expected[] = { "core-1", "core-2", "core-3" };
        if (!identities_are(&mapped, expected, 3)) {
            struct text message = { 0 };
            identities_repr(&message, &mapped);
            fail(text_take(&message));
        }
    }
    if (!drawing(5)) fail("the windows were not drawing before the swap");
    input_round("pre", 183, motions_all, 3);
    if (!drawing(5)) fail("the windows stopped drawing after the first input round");
    pointer_round();
    const char *kinds[] = { "protect", "alloc" };
    const char *injected[] = { "inject1", "inject2" };
    const int inject_codes[] = { 189, 190 };
    const char *messages[] = { "cannot make the code writable", "cannot remember the patch" };
    char *file_b = format("file=%s", core_b);
    for (int index = 0; index < 2; index++) {
        struct text content = { 0 };
        text_add(&content, kinds[index]);
        text_add(&content, "\n");
        char *written = text_take(&content);
        harness_write_file(swap_fail, written);
        free(written);
        const char *arguments[] = { "command", "swap-core", file_b };
        struct reply refused = cli(arguments, 3, 0);
        if (refused.status == 0) {
            struct text message = { 0 };
            text_add(&message, "an injected ");
            text_add(&message, kinds[index]);
            text_add(&message, " failure was not reported");
            free(refused.out);
            free(refused.err);
            fail(text_take(&message));
        }
        struct text combined = { 0 };
        text_add(&combined, refused.out);
        text_add(&combined, refused.err);
        free(refused.out);
        free(refused.err);
        if (!strstr(combined.body, messages[index])) {
            struct text message = { 0 };
            text_add(&message, kinds[index]);
            text_add(&message, ": ");
            text_add(&message, text_take(&combined));
            fail(text_take(&message));
        }
        free(combined.body);
        char current[512];
        if (!stamp(current, sizeof current) || strcmp(current, stamp_a) != 0) {
            struct text message = { 0 };
            text_add(&message, "an injected ");
            text_add(&message, kinds[index]);
            text_add(&message, " failure changed the running core");
            fail(text_take(&message));
        }
        struct window_table after = windows_load();
        if (!identities_equal(&after, &mapped)) {
            struct text message = { 0 };
            text_add(&message, "an injected ");
            text_add(&message, kinds[index]);
            text_add(&message, " failure lost the windows");
            fail(text_take(&message));
        }
        if (!drawing(5)) {
            struct text message = { 0 };
            text_add(&message, "the session stopped drawing after an injected ");
            text_add(&message, kinds[index]);
            text_add(&message, " failure");
            fail(text_take(&message));
        }
        input_round(injected[index], inject_codes[index], motions_all, 3);
        if (clients_ended()) {
            struct text message = { 0 };
            text_add(&message, "a client exited during an injected ");
            text_add(&message, kinds[index]);
            text_add(&message, " failure");
            fail(text_take(&message));
        }
    }
    note("a swap that cannot protect the code or hold its records is refused, and the old "
        "core keeps drawing and taking input");
    {
        struct text line = { 0 };
        text_add(&line, "three windows mapped, drawing, and taking a bound key, a key event "
            "and the pointer on ");
        text_add(&line, stamp_a);
        note(text_take(&line));
    }
    {
        const char *arguments[] = { "command", "swap-core", file_b };
        struct reply reply = cli(arguments, 3, 1);
        free(reply.out);
        free(reply.err);
    }
    if (!swapped(stamp_b, 15)) {
        char current[512];
        struct text message = { 0 };
        text_add(&message, "the swapped core reports ");
        text_add(&message, stamp(current, sizeof current) ? current : "None");
        fail(text_take(&message));
    }
    if (clients_ended()) {
        struct text message = { 0 };
        text_add(&message, "a client exited during the swap: ");
        clients_ended_repr(&message);
        fail(text_take(&message));
    }
    {
        struct window_table after = windows_load();
        if (!identities_equal(&after, &mapped)) {
            struct text message = { 0 };
            identities_repr(&message, &after);
            fail(text_take(&message));
        }
    }
    {
        struct window_table after = windows_load();
        if (!identities_mapped(&after)) {
            struct text message = { 0 };
            identities_repr(&message, &after);
            fail(text_take(&message));
        }
    }
    if (!drawing(5)) fail("the windows stopped drawing after the swap");
    input_round("swapped", 184, motions_all, 3);
    if (!drawing(5)) fail("the windows stopped drawing after input on the swapped core");
    if (clients_ended()) fail("a client exited while the swapped core took input");
    note("the same three windows stay mapped, keep drawing, and take a bound key, a key "
        "event and the pointer after the swap");
    if (!clients_kill("core-1")) fail("the first client could not be killed");
    if (!gone("core-1", 15))
        fail("a window whose client died right after the swap is still listed");
    mapped = windows_load();
    {
        const char *expected[] = { "core-2", "core-3" };
        if (!identities_are(&mapped, expected, 2)) {
            struct text message = { 0 };
            identities_repr(&message, &mapped);
            fail(text_take(&message));
        }
    }
    {
        struct window_table after = windows_load();
        if (!identities_mapped(&after)) {
            struct text message = { 0 };
            identities_repr(&message, &after);
            fail(text_take(&message));
        }
    }
    if (!drawing(5)) fail("the session stopped drawing after a client died on the swapped core");
    {
        const char *arguments[] = { "msg", "version" };
        char *text = cli_stdout(arguments, 2);
        int empty = !text[0];
        free(text);
        if (empty) fail("the session stopped answering");
    }
    {
        const char *motions[] = { "core-2", "core-3" };
        input_round("closed", 187, motions, 2);
    }
    if (!drawing(5)) fail("the session stopped drawing after input following a close");
    if (clients_ended()) fail("a client exited after the close on the swapped core");
    note("a client that dies while the swapped core runs leaves the other windows mapped, "
        "drawing and taking input");
    {
        const char *options[] = { "--mode", "xdg", "--commit-rate", "60", "--repaint",
            "--keys", "--pointer", "--app-id", "core-4", "--title", "core-4",
            "--size", "320x200" };
        clients_start("core-4", options, sizeof options / sizeof options[0]);
    }
    if (!sink_wait(&client_lines, "core-4: mapped", 1, 60))
        fail("a client could not connect to the swapped core");
    {
        struct window_table fresh = windows_load();
        if (!identities_has(&fresh, "core-4"))
            fail("a client that connected after the swap is missing");
    }
    mapped = windows_load();
    pointer_round();
    note("a client that connects after the swap maps and takes the pointer");
    char *file_bad = format("file=%s", core_bad);
    {
        const char *arguments[] = { "command", "swap-core", file_bad };
        struct reply bad = cli(arguments, 3, 0);
        if (bad.status == 0) {
            struct text message = { 0 };
            text_add(&message, "an incompatible core was accepted:\n");
            text_add(&message, bad.out);
            free(bad.out);
            free(bad.err);
            fail(text_take(&message));
        }
        struct text combined = { 0 };
        text_add(&combined, bad.out);
        text_add(&combined, bad.err);
        free(bad.out);
        free(bad.err);
        if (!strstr(combined.body, "layout")) {
            char *text = text_take(&combined);
            fail(text);
        }
        free(combined.body);
    }
    {
        char current[512];
        if (!stamp(current, sizeof current) || strcmp(current, stamp_b) != 0)
            fail("a refused core changed the running core");
    }
    {
        struct window_table after = windows_load();
        if (!identities_equal(&after, &mapped)) fail("a refused core lost the windows");
    }
    if (!drawing(5)) fail("the windows stopped drawing after a refused swap");
    {
        const char *motions[] = { "core-2", "core-3" };
        input_round("refused", 185, motions, 2);
    }
    note("an incompatible core is refused and the session keeps drawing and taking input");
    {
        char *file_a = format("file=%s", core_a);
        const char *arguments[] = { "command", "swap-core", file_a };
        struct reply reply = cli(arguments, 3, 1);
        free(reply.out);
        free(reply.err);
        free(file_a);
    }
    if (!swapped(stamp_a, 15)) {
        char current[512];
        struct text message = { 0 };
        text_add(&message, "the second swap reports ");
        text_add(&message, stamp(current, sizeof current) ? current : "None");
        fail(text_take(&message));
    }
    {
        struct window_table after = windows_load();
        if (!identities_equal(&after, &mapped)) fail("the second swap lost the windows");
    }
    if (!drawing(5)) fail("the windows stopped drawing after the second swap");
    {
        const char *motions[] = { "core-2", "core-3" };
        input_round("second", 186, motions, 2);
    }
    if (!clients_kill("core-2")) fail("the second client could not be killed");
    if (!gone("core-2", 15))
        fail("a window whose client died after two swaps is still listed");
    mapped = windows_load();
    if (!identities_has(&mapped, "core-3"))
        fail("the last pre-swap client left with the second close");
    if (!drawing(5)) fail("the session stopped drawing after a client died after two swaps");
    {
        const char *motions[] = { "core-3" };
        input_round("closed2", 188, motions, 1);
    }
    if (clients_ended()) fail("a client exited during a swap");
    note("a second swap returns to the first core, and the pre-swap windows that stay "
        "open keep drawing and taking input after each close");
    cleanup();
    if (compositor_code != 0) {
        dump_compositor_log();
        dump_session_log();
        remove_tree(root);
        struct text message = { 0 };
        text_addf(&message, "the compositor exited %d", compositor_code);
        harness_fail(text_take(&message));
    }
    remove_tree(root);
    free(file_b);
    free(file_bad);
    free(swap_flag);
    return 0;
}
