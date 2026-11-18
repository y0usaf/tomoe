#include "harness.h"

#include <ctype.h>
#include <dirent.h>
#include <poll.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdint.h>

#define REFRESH_HZ 240
#define SKIP_SHARE 8
#define FRAME_SHARE 0.9

static const char policy_text[] =
    "\n"
    "(in-package #:tomoe-user)\n"
    "\n"
    "(define-extension \"anim-probe\"\n"
    "    (:reads (:windows :outputs :command)\n"
    "     :state (list :moved nil :viewed nil :bar nil :hidden nil))\n"
    "    (snapshot state event)\n"
    "  (let* ((command (and (eq (getf event :type) :command) (getf event :name)))\n"
    "         (output (first (context snapshot :outputs)))\n"
    "         (windows (context snapshot :windows)))\n"
    "    (cond ((equal command \"move\") (setf (getf state :moved) (not (getf state :moved))))\n"
    "          ((equal command \"view\") (setf (getf state :viewed) (not (getf state :viewed))))\n"
    "          ((equal command \"bar\") (setf (getf state :bar) t))\n"
    "          ((equal command \"hide\") (setf (getf state :hidden) t))\n"
    "          ((equal command \"show\") (setf (getf state :hidden) nil)))\n"
    "    (let ((offset (if (getf state :moved) 600 0))\n"
    "          (left (if (getf state :bar) 600 40))\n"
    "          (view (if (getf state :viewed) 120 0)))\n"
    "      (values state\n"
    "              (append\n"
    "               (list (configure-output (getf output :name) :mode '(1280 720 240)\n"
    "                                       :scale 1 :position '(0 0))\n"
    "                     (settings\n"
    "                      :animations\n"
    "                      '(:window-move (:spring (:stiffness 16000 :epsilon 1/1000))\n"
    "                        :window-open (:ease (:duration-ms 90 :curve :ease-out-expo))\n"
    "                        :window-view (:spring (:stiffness 16000 :epsilon 1/1000))))\n"
    "                     (shell-surface :bar\n"
    "                                    (ui :rect :width 200 :height 60 :background \"#ff0000ff\")\n"
    "                                    :anchors '(:top :left)\n"
    "                                    :margin (list 560 0 0 left)\n"
    "                                    :slide t :layer :overlay)\n"
    "                     (set-view view 0)\n"
    "                     (define-command \"move\")\n"
    "                     (define-command \"view\")\n"
    "                     (define-command \"bar\")\n"
    "                     (define-command \"hide\")\n"
    "                     (define-command \"show\"))\n"
    "               (when output\n"
    "                 (loop for window in windows\n"
    "                       for index from 0\n"
    "                       append (if (getf state :hidden)\n"
    "                                  (list (hide-window (getf window :id)))\n"
    "                                  (list (show-window (getf window :id))\n"
    "                                        (place (getf window :id)\n"
    "                                               (+ (getf output :x) offset (* index 220))\n"
    "                                               (+ (getf output :y) 200)\n"
    "                                               180 180)))))\n"
    "              nil)))))\n";

struct fail_frame {
    jmp_buf buffer;
    struct fail_frame *previous;
};

static struct fail_frame *failure_frame;
static char failure_text[65536];

static char **report_lines;
static size_t report_count;
static size_t report_capacity;

struct problems {
    char **items;
    size_t count;
    size_t capacity;
};

static struct problems main_problems;
static struct problems control_problems;

static void failf(const char *format, ...) {
    va_list arguments;
    va_start(arguments, format);
    vsnprintf(failure_text, sizeof failure_text, format, arguments);
    va_end(arguments);
    longjmp(failure_frame->buffer, 1);
}

static void rethrow(void) {
    longjmp(failure_frame->buffer, 1);
}

static void *grow_array(void *data, size_t *capacity, size_t needed, size_t size) {
    if (needed <= *capacity) return data;
    size_t grown = *capacity ? *capacity : 8;
    while (grown < needed) grown *= 2;
    void *result = realloc(data, grown * size);
    if (!result) harness_fail("out of memory");
    *capacity = grown;
    return result;
}

static void say(const char *line) {
    report_lines = grow_array(report_lines, &report_capacity, report_count + 1, sizeof *report_lines);
    report_lines[report_count] = strdup(line);
    if (!report_lines[report_count]) harness_fail("out of memory");
    report_count++;
    printf("%s\n", line);
    fflush(stdout);
}

static void problem_add_to(struct problems *list, const char *format, ...) {
    char text[65536];
    va_list arguments;
    va_start(arguments, format);
    vsnprintf(text, sizeof text, format, arguments);
    va_end(arguments);
    list->items = grow_array(list->items, &list->capacity, list->count + 1, sizeof *list->items);
    list->items[list->count] = strdup(text);
    if (!list->items[list->count]) harness_fail("out of memory");
    list->count++;
}

static void problem_add(const char *format, ...) {
    char text[65536];
    va_list arguments;
    va_start(arguments, format);
    vsnprintf(text, sizeof text, format, arguments);
    va_end(arguments);
    problem_add_to(&main_problems, "%s", text);
}

static char *read_whole_file(const char *path, size_t *length) {
    FILE *file = fopen(path, "rb");
    if (!file) return NULL;
    size_t capacity = 4096;
    size_t used = 0;
    char *text = malloc(capacity);
    if (!text) harness_fail("out of memory");
    for (;;) {
        if (used + 4096 + 1 > capacity) {
            capacity *= 2;
            text = realloc(text, capacity);
            if (!text) harness_fail("out of memory");
        }
        size_t got = fread(text + used, 1, capacity - used - 1, file);
        used += got;
        if (got == 0) break;
    }
    fclose(file);
    text[used] = '\0';
    if (length) *length = used;
    return text;
}

static char *strip_copy(const char *text) {
    const char *start = text;
    while (*start && isspace((unsigned char)*start)) start++;
    const char *end = text + strlen(text);
    while (end > start && isspace((unsigned char)end[-1])) end--;
    size_t length = (size_t)(end - start);
    char *result = malloc(length + 1);
    if (!result) harness_fail("out of memory");
    memcpy(result, start, length);
    result[length] = '\0';
    return result;
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
                int written = snprintf(child, sizeof child, "%s/%s", path, entry->d_name);
                if (written > 0 && (size_t)written < sizeof child) remove_tree(child);
            }
            closedir(directory);
        }
        rmdir(path);
    } else {
        unlink(path);
    }
}

static void make_tempdir(char *out, size_t size, const char *prefix) {
    const char *root = getenv("TMPDIR");
    if (!root || !*root) root = "/tmp";
    snprintf(out, size, "%s/%sXXXXXX", root, prefix);
    if (!mkdtemp(out)) harness_fail("cannot create a temporary directory");
}

static void run_capture(char *const argv[], char **env, int *status_out, char **out_text,
        size_t *out_length, char **err_text, size_t *err_length) {
    int out_pipe[2] = { -1, -1 };
    int err_pipe[2] = { -1, -1 };
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
        execvpe(argv[0], argv, env);
        fprintf(stderr, "harness: cannot run %s: %s\n", argv[0], strerror(errno));
        _exit(127);
    }
    close(out_pipe[1]);
    close(err_pipe[1]);
    char *out_buffer = malloc(8192);
    char *err_buffer = malloc(4096);
    if (!out_buffer || !err_buffer) harness_fail("out of memory");
    size_t out_capacity = 8192, out_used = 0;
    size_t err_capacity = 4096, err_used = 0;
    struct pollfd descriptors[2] = {
        { out_pipe[0], POLLIN, 0 },
        { err_pipe[0], POLLIN, 0 }
    };
    int alive = 2;
    char chunk[8192];
    while (alive > 0) {
        int ready = poll(descriptors, 2, -1);
        if (ready < 0) {
            if (errno == EINTR) continue;
            break;
        }
        for (int index = 0; index < 2; index++) {
            if (descriptors[index].fd < 0 || !descriptors[index].revents) continue;
            ssize_t got = read(descriptors[index].fd, chunk, sizeof chunk);
            if (got > 0) {
                if (index == 0) {
                    out_buffer = grow_array(out_buffer, &out_capacity, out_used + (size_t)got + 1, 1);
                    memcpy(out_buffer + out_used, chunk, (size_t)got);
                    out_used += (size_t)got;
                } else {
                    err_buffer = grow_array(err_buffer, &err_capacity, err_used + (size_t)got + 1, 1);
                    memcpy(err_buffer + err_used, chunk, (size_t)got);
                    err_used += (size_t)got;
                }
            } else if (got == 0 || errno != EINTR) {
                close(descriptors[index].fd);
                descriptors[index].fd = -1;
                alive--;
            }
        }
    }
    out_buffer[out_used] = '\0';
    err_buffer[err_used] = '\0';
    int status = harness_wait_status(child, 3600);
    if (status_out) *status_out = status;
    if (out_text) *out_text = out_buffer;
    else free(out_buffer);
    if (out_length) *out_length = out_used;
    if (err_text) *err_text = err_buffer;
    else free(err_buffer);
    if (err_length) *err_length = err_used;
}

static char *base_tomoe_debug;
static char *base_mesa_override;
static char *base_software;

static void remember_environment(void) {
    const char *value;
    value = getenv("TOMOE_DEBUG");
    base_tomoe_debug = value ? strdup(value) : NULL;
    value = getenv("MESA_LOADER_DRIVER_OVERRIDE");
    base_mesa_override = value ? strdup(value) : NULL;
    value = getenv("LIBGL_ALWAYS_SOFTWARE");
    base_software = value ? strdup(value) : NULL;
}

static void restore_key(const char *key, const char *value) {
    if (value) setenv(key, value, 1);
    else unsetenv(key);
}

struct sha256 {
    uint32_t state[8];
    uint64_t bits;
    unsigned char block[64];
    size_t used;
};

static const uint32_t sha256_constants[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u,
    0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
    0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
    0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
    0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
    0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u,
    0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u,
    0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u
};

static uint32_t rotate_right(uint32_t value, int bits) {
    return (value >> bits) | (value << (32 - bits));
}

static void sha256_transform(struct sha256 *context, const unsigned char *block) {
    uint32_t words[64];
    for (int index = 0; index < 16; index++)
        words[index] = (uint32_t)block[index * 4] << 24 |
            (uint32_t)block[index * 4 + 1] << 16 |
            (uint32_t)block[index * 4 + 2] << 8 |
            (uint32_t)block[index * 4 + 3];
    for (int index = 16; index < 64; index++) {
        uint32_t first = rotate_right(words[index - 15], 7) ^
            rotate_right(words[index - 15], 18) ^ (words[index - 15] >> 3);
        uint32_t second = rotate_right(words[index - 2], 17) ^
            rotate_right(words[index - 2], 19) ^ (words[index - 2] >> 10);
        words[index] = words[index - 16] + first + words[index - 7] + second;
    }
    uint32_t a = context->state[0], b = context->state[1];
    uint32_t c = context->state[2], d = context->state[3];
    uint32_t e = context->state[4], f = context->state[5];
    uint32_t g = context->state[6], h = context->state[7];
    for (int index = 0; index < 64; index++) {
        uint32_t upper = rotate_right(e, 6) ^ rotate_right(e, 11) ^ rotate_right(e, 25);
        uint32_t choose = (e & f) ^ (~e & g);
        uint32_t first = h + upper + choose + sha256_constants[index] + words[index];
        uint32_t lower = rotate_right(a, 2) ^ rotate_right(a, 13) ^ rotate_right(a, 22);
        uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
        uint32_t second = lower + majority;
        h = g;
        g = f;
        f = e;
        e = d + first;
        d = c;
        c = b;
        b = a;
        a = first + second;
    }
    context->state[0] += a;
    context->state[1] += b;
    context->state[2] += c;
    context->state[3] += d;
    context->state[4] += e;
    context->state[5] += f;
    context->state[6] += g;
    context->state[7] += h;
}

static void sha256_init(struct sha256 *context) {
    static const uint32_t initial[8] = {
        0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
        0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u
    };
    memcpy(context->state, initial, sizeof initial);
    context->bits = 0;
    context->used = 0;
}

static void sha256_update(struct sha256 *context, const void *data, size_t length) {
    const unsigned char *bytes = data;
    context->bits += (uint64_t)length * 8;
    while (length) {
        size_t take = 64 - context->used;
        if (take > length) take = length;
        memcpy(context->block + context->used, bytes, take);
        context->used += take;
        bytes += take;
        length -= take;
        if (context->used == 64) {
            sha256_transform(context, context->block);
            context->used = 0;
        }
    }
}

static void sha256_final(struct sha256 *context, unsigned char *digest) {
    uint64_t bits = context->bits;
    context->block[context->used++] = 0x80;
    if (context->used > 56) {
        memset(context->block + context->used, 0, 64 - context->used);
        sha256_transform(context, context->block);
        context->used = 0;
    }
    memset(context->block + context->used, 0, 56 - context->used);
    for (int index = 0; index < 8; index++)
        context->block[63 - index] = (unsigned char)(bits >> (8 * index));
    sha256_transform(context, context->block);
    for (int index = 0; index < 8; index++) {
        digest[index * 4] = (unsigned char)(context->state[index] >> 24);
        digest[index * 4 + 1] = (unsigned char)(context->state[index] >> 16);
        digest[index * 4 + 2] = (unsigned char)(context->state[index] >> 8);
        digest[index * 4 + 3] = (unsigned char)context->state[index];
    }
}

struct node {
    int is_list;
    char *text;
    struct node **items;
    size_t count;
    size_t capacity;
};

static struct node *node_new(int is_list) {
    struct node *node = malloc(sizeof *node);
    if (!node) harness_fail("out of memory");
    memset(node, 0, sizeof *node);
    node->is_list = is_list;
    return node;
}

static void node_push(struct node *list, struct node *item) {
    list->items = grow_array(list->items, &list->capacity, list->count + 1, sizeof *list->items);
    list->items[list->count++] = item;
}

static struct node *node_atom(const char *start, size_t length) {
    struct node *node = node_new(0);
    node->text = malloc(length + 1);
    if (!node->text) harness_fail("out of memory");
    memcpy(node->text, start, length);
    node->text[length] = '\0';
    return node;
}

static void node_free(struct node *node) {
    if (!node) return;
    if (node->is_list) {
        for (size_t index = 0; index < node->count; index++) node_free(node->items[index]);
        free(node->items);
    } else {
        free(node->text);
    }
    free(node);
}

static int is_delimiter(char character) {
    return character == ' ' || character == '\n' || character == '\t' ||
        character == '(' || character == ')' || character == '"';
}

static struct node *parse_node(const char **at) {
    while (**at == ' ' || **at == '\n' || **at == '\t') (*at)++;
    char character = **at;
    if (!character) return NULL;
    if (character == '(') {
        (*at)++;
        struct node *list = node_new(1);
        for (;;) {
            while (**at == ' ' || **at == '\n' || **at == '\t') (*at)++;
            if (**at == ')') {
                (*at)++;
                break;
            }
            if (!**at) break;
            struct node *item = parse_node(at);
            if (!item) break;
            node_push(list, item);
        }
        return list;
    }
    if (character == '"') {
        const char *start = *at;
        const char *cursor = *at + 1;
        while (*cursor && *cursor != '"') cursor += *cursor == '\\' ? 2 : 1;
        if (*cursor == '"') cursor++;
        struct node *atom = node_atom(start, (size_t)(cursor - start));
        *at = cursor;
        return atom;
    }
    if (character == ')') {
        (*at)++;
        return NULL;
    }
    const char *start = *at;
    while (**at && !is_delimiter(**at)) (*at)++;
    return node_atom(start, (size_t)(*at - start));
}

static struct node *parse_first(const char *text) {
    const char *at = text;
    return parse_node(&at);
}

static struct node *unwrap_node(struct node *value) {
    if (value && value->is_list && value->count >= 3 && value->items[1] &&
            !value->items[1]->is_list && !strcmp(value->items[1]->text, ":OK")) {
        value = value->items[2];
    }
    while (value && value->is_list && value->count == 1) value = value->items[0];
    return value;
}

static struct node *plist_find(struct node *value, const char *name) {
    if (!value || !value->is_list) return NULL;
    struct node *found = NULL;
    for (size_t index = 0; index + 1 < value->count; index += 2) {
        struct node *key = value->items[index];
        if (!key || key->is_list || !key->text) continue;
        const char *text = key->text;
        while (*text == ':') text++;
        size_t length = strlen(text);
        size_t wanted = strlen(name);
        if (length != wanted) continue;
        size_t position = 0;
        while (position < length &&
                tolower((unsigned char)text[position]) ==
                tolower((unsigned char)name[position])) position++;
        if (position == length) found = value->items[index + 1];
    }
    return found;
}

static int node_integer(struct node *node, long long *value) {
    if (!node || node->is_list || !node->text) return 0;
    const char *text = node->text;
    while (*text && isspace((unsigned char)*text)) text++;
    char *end = NULL;
    errno = 0;
    long long parsed = strtoll(text, &end, 10);
    if (end == text || errno != 0) return 0;
    while (*end && isspace((unsigned char)*end)) end++;
    if (*end) return 0;
    *value = parsed;
    return 1;
}

static int text_integer(const char *text, long long *value) {
    char *end = NULL;
    errno = 0;
    long long parsed = strtoll(text, &end, 10);
    if (end == text || errno != 0) return 0;
    while (*end && isspace((unsigned char)*end)) end++;
    if (*end) return 0;
    *value = parsed;
    return 1;
}

struct counters {
    int have_move;
    int have_open;
    int have_view;
    char move_text[64];
    char open_text[64];
    char view_text[64];
    long long frames;
    long long gaps;
    long long dead;
    long long stale;
    long long first;
    long long lead_min;
    long long lead_max;
    long long span;
    long long interval;
    long long steady;
    long long period;
    long long refresh;
};

static long long counter_number(struct node *plist, const char *name) {
    struct node *value = plist_find(plist, name);
    long long number = 0;
    if (!value) return 0;
    if (!node_integer(value, &number)) harness_fail("cannot read an animation counter");
    return number;
}

static int counter_string(struct node *plist, const char *name, char *out, size_t size) {
    struct node *value = plist_find(plist, name);
    if (!value || value->is_list || !value->text) return 0;
    snprintf(out, size, "%s", value->text);
    return 1;
}

static int process_exited(pid_t pid) {
    int status = 0;
    return waitpid(pid, &status, WNOHANG) == pid;
}

struct command_result {
    int status;
    char *text;
};

struct compositor {
    const char *tomoe;
    char socket[256];
    char runtime[4096];
    char log_path[4096];
    pid_t pid;
};

struct client {
    const char *binary;
    char name[256];
    char log_path[4096];
    pid_t pid;
};

static void apply_compositor_environment(const struct compositor *compositor) {
    setenv("XDG_RUNTIME_DIR", compositor->runtime, 1);
    setenv("WAYLAND_DISPLAY", compositor->socket, 1);
    setenv("TOMOE_DEBUG", "1", 1);
    setenv("MESA_LOADER_DRIVER_OVERRIDE", "zink", 1);
    setenv("LIBGL_ALWAYS_SOFTWARE", "1", 1);
}

static void apply_client_environment(const char *runtime, const char *socket) {
    setenv("XDG_RUNTIME_DIR", runtime, 1);
    setenv("WAYLAND_DISPLAY", socket, 1);
    restore_key("TOMOE_DEBUG", base_tomoe_debug);
    restore_key("MESA_LOADER_DRIVER_OVERRIDE", base_mesa_override);
    restore_key("LIBGL_ALWAYS_SOFTWARE", base_software);
}

static char *compositor_text(const struct compositor *compositor) {
    size_t length = 0;
    char *text = read_whole_file(compositor->log_path, &length);
    if (!text) {
        text = strdup("");
        if (!text) harness_fail("out of memory");
    }
    return text;
}

static struct command_result compositor_call(struct compositor *compositor, int count, ...) {
    char *argv[16];
    int used = 0;
    argv[used++] = (char *)compositor->tomoe;
    argv[used++] = "--socket";
    argv[used++] = compositor->socket;
    va_list arguments;
    va_start(arguments, count);
    for (int index = 0; index < count; index++) argv[used++] = (char *)va_arg(arguments, const char *);
    va_end(arguments);
    argv[used] = NULL;
    apply_compositor_environment(compositor);
    struct command_result result = { -1, NULL };
    size_t out_length = 0, err_length = 0;
    char *err = NULL;
    run_capture(argv, environ, &result.status, &result.text, &out_length, &err, &err_length);
    result.text = realloc(result.text, out_length + err_length + 1);
    if (!result.text) harness_fail("out of memory");
    memcpy(result.text + out_length, err, err_length + 1);
    free(err);
    return result;
}

static void compositor_start(struct compositor *compositor, const char *work,
        const char *tomoe, const char *socket, const char *const *options, int option_count) {
    memset(compositor, 0, sizeof *compositor);
    compositor->tomoe = tomoe;
    snprintf(compositor->socket, sizeof compositor->socket, "%s", socket);
    harness_join(compositor->runtime, sizeof compositor->runtime, work, "run");
    harness_mkparents(compositor->runtime, 0700);
    chmod(compositor->runtime, 0700);
    int written = snprintf(compositor->log_path, sizeof compositor->log_path, "%s/%s.log",
            work, socket);
    if (written < 0 || (size_t)written >= sizeof compositor->log_path)
        harness_fail("a path is too long");
    char *argv[16];
    int used = 0;
    argv[used++] = (char *)tomoe;
    argv[used++] = "--socket";
    argv[used++] = (char *)socket;
    argv[used++] = "--backend";
    argv[used++] = "headless";
    argv[used++] = "--bare";
    argv[used++] = "--no-watch";
    for (int index = 0; index < option_count; index++) argv[used++] = (char *)options[index];
    argv[used] = NULL;
    apply_compositor_environment(compositor);
    compositor->pid = harness_spawn(argv, environ, NULL, NULL, compositor->log_path, 1);
    double deadline = harness_now() + 120;
    while (harness_now() < deadline) {
        if (process_exited(compositor->pid)) {
            char *text = compositor_text(compositor);
            failf("tomoe exited while starting:\n%s", text);
        }
        struct command_result result = compositor_call(compositor, 1, "inspect");
        int status = result.status;
        free(result.text);
        if (status == 0) return;
        harness_sleep(0.1);
    }
    char *text = compositor_text(compositor);
    failf("tomoe did not answer inspect:\n%s", text);
}

static void compositor_stop(struct compositor *compositor) {
    if (compositor->pid > 0) {
        int status = 0;
        pid_t done = waitpid(compositor->pid, &status, WNOHANG);
        if (done == 0) {
            kill(compositor->pid, SIGTERM);
            if (harness_wait_status(compositor->pid, 20) < 0) {
                kill(compositor->pid, SIGKILL);
                harness_wait_status(compositor->pid, 10);
            }
        }
        compositor->pid = 0;
    }
}

static void compositor_mount(struct compositor *compositor, const char *path) {
    struct command_result result = compositor_call(compositor, 2, "mount", path);
    if (result.status != 0) {
        char *text = compositor_text(compositor);
        failf("mount failed: %s%s", result.text, text);
    }
    free(result.text);
}

static void compositor_command(struct compositor *compositor, const char *name) {
    struct command_result result = compositor_call(compositor, 2, "command", name);
    if (result.status != 0) {
        char *text = compositor_text(compositor);
        failf("command %s failed: %s%s", name, result.text, text);
    }
    free(result.text);
}

static int compositor_animation(struct compositor *compositor, struct counters *result) {
    struct command_result call = compositor_call(compositor, 1, "animation");
    if (call.status != 0) {
        free(call.text);
        return 0;
    }
    struct node *root = parse_first(call.text);
    free(call.text);
    struct node *value = unwrap_node(root);
    memset(result, 0, sizeof *result);
    result->have_move = counter_string(value, "move-settle-ms", result->move_text,
            sizeof result->move_text);
    result->have_open = counter_string(value, "open-ms", result->open_text,
            sizeof result->open_text);
    result->have_view = counter_string(value, "view-settle-ms", result->view_text,
            sizeof result->view_text);
    result->frames = counter_number(value, "frames");
    result->gaps = counter_number(value, "gaps");
    result->dead = counter_number(value, "dead");
    result->stale = counter_number(value, "stale");
    result->first = counter_number(value, "first-passed-ns");
    result->lead_min = counter_number(value, "lead-min-ns");
    result->lead_max = counter_number(value, "lead-max-ns");
    result->span = counter_number(value, "span-ns");
    result->interval = counter_number(value, "max-interval-ns");
    result->steady = counter_number(value, "steady-interval-ns");
    result->period = counter_number(value, "period-ns");
    result->refresh = counter_number(value, "refresh-mhz");
    node_free(root);
    return 1;
}

static long long drawn_frames(struct node *entry) {
    struct node *frame = plist_find(entry, "frame");
    struct node *reused = plist_find(entry, "reused");
    long long drawn = 0, repeats = 0;
    if (!node_integer(frame, &drawn)) return 0;
    if (node_integer(reused, &repeats)) drawn -= repeats;
    return drawn > 0 ? drawn : 0;
}

static int compositor_drawn(struct compositor *compositor, long long *total) {
    struct command_result call = compositor_call(compositor, 1, "frames");
    *total = 0;
    if (call.status != 0) {
        free(call.text);
        return 1;
    }
    struct node *root = parse_first(call.text);
    struct node *value = unwrap_node(root);
    long long sum = 0;
    if (value && value->is_list && value->count > 0 && value->items[0]->is_list) {
        for (size_t index = 0; index < value->count; index++) {
            struct node *entry = value->items[index];
            if (!entry->is_list) continue;
            sum += drawn_frames(entry);
        }
    } else if (value && value->is_list) {
        sum += drawn_frames(value);
    }
    node_free(root);
    if (!sum) {
        char *stripped = strip_copy(call.text);
        failure_text[0] = '\0';
        snprintf(failure_text, sizeof failure_text,
                "cannot read the output frame counters: %s", stripped);
        free(stripped);
        free(call.text);
        return 0;
    }
    free(call.text);
    *total = sum;
    return 1;
}

struct frame {
    unsigned char *raw;
    size_t raw_length;
    unsigned char *pixels;
    size_t pixel_length;
    long long width;
    long long height;
};

static int parse_ppm(const unsigned char *data, size_t size, struct frame *result) {
    if (size < 2 || data[0] != 'P' || data[1] != '6') {
        char preview[17];
        size_t shown = size < 16 ? size : 16;
        memcpy(preview, data, shown);
        preview[shown] = '\0';
        failf("b'%s'", preview);
    }
    long long fields[3];
    int count = 0;
    size_t offset = 2;
    while (count < 3) {
        while (offset < size && isspace(data[offset])) offset++;
        size_t start = offset;
        while (offset < size && !isspace(data[offset])) offset++;
        char text[64];
        size_t length = offset - start;
        if (length >= sizeof text) length = sizeof text - 1;
        memcpy(text, data + start, length);
        text[length] = '\0';
        fields[count++] = strtoll(text, NULL, 10);
    }
    result->width = fields[0];
    result->height = fields[1];
    size_t start = offset + 1;
    if (start > size) start = size;
    size_t wanted = (size_t)result->width * (size_t)result->height * 3;
    size_t available = size - start;
    if (wanted > available) wanted = available;
    result->pixels = (unsigned char *)data + start;
    result->pixel_length = wanted;
    return 1;
}

static void frame_release(struct frame *frame) {
    free(frame->raw);
    memset(frame, 0, sizeof *frame);
}

static void frame_capture(struct compositor *compositor, const char *name, const char *save,
        struct frame *result) {
    char *last = strdup("");
    if (!last) harness_fail("out of memory");
    for (int attempt = 0; attempt < 6; attempt++) {
        char *argv[5] = { "grim", "-t", "ppm", "-", NULL };
        apply_compositor_environment(compositor);
        int status = -1;
        char *out = NULL;
        size_t out_length = 0;
        char *err = NULL;
        size_t err_length = 0;
        run_capture(argv, environ, &status, &out, &out_length, &err, &err_length);
        if (status == 0) {
            memset(result, 0, sizeof *result);
            result->raw = (unsigned char *)out;
            result->raw_length = out_length;
            parse_ppm(result->raw, result->raw_length, result);
            free(err);
            free(last);
            if (save && name) {
                char path[4096];
                harness_join(path, sizeof path, save, name);
                harness_concat(path, sizeof path, path, ".ppm");
                FILE *file = fopen(path, "wb");
                if (!file) harness_fail("cannot save a screen");
                if (fwrite(result->raw, 1, result->raw_length, file) != result->raw_length)
                    harness_fail("cannot save a screen");
                fclose(file);
            }
            return;
        }
        char *stripped = strip_copy(err);
        free(last);
        last = stripped;
        free(out);
        free(err);
        harness_sleep(0.05);
    }
    failf("grim could not copy the output: %s", last);
}

static void sha256_hex16(long long width, long long height, const unsigned char *pixels,
        size_t length, char *out) {
    char header[64];
    int header_length = snprintf(header, sizeof header, "%lldx%lld:", width, height);
    struct sha256 context;
    sha256_init(&context);
    sha256_update(&context, header, (size_t)header_length);
    sha256_update(&context, pixels, length);
    unsigned char digest[32];
    sha256_final(&context, digest);
    static const char digits[] = "0123456789abcdef";
    for (int index = 0; index < 16; index++) {
        out[index * 2] = digits[digest[index] >> 4];
        out[index * 2 + 1] = digits[digest[index] & 0x0f];
    }
    out[32] = '\0';
}

static void compositor_digest(struct compositor *compositor, const char *name, const char *save,
        char *out) {
    struct frame frame;
    frame_capture(compositor, name, save, &frame);
    char full[33];
    sha256_hex16(frame.width, frame.height, frame.pixels, frame.pixel_length, full);
    memcpy(out, full, 16);
    out[16] = '\0';
    frame_release(&frame);
}

static long long context_switches(pid_t pid) {
    char path[64];
    snprintf(path, sizeof path, "/proc/%d/status", (int)pid);
    FILE *file = fopen(path, "r");
    if (!file) harness_fail("cannot read a process status");
    char line[1024];
    long long total = 0;
    while (fgets(line, sizeof line, file)) {
        if (!strncmp(line, "voluntary_ctxt_switches", 23) ||
                !strncmp(line, "nonvoluntary_ctxt_switches", 26)) {
            const char *text = line;
            while (*text && !isspace((unsigned char)*text)) text++;
            while (*text && isspace((unsigned char)*text)) text++;
            total += strtoll(text, NULL, 10);
        }
    }
    fclose(file);
    return total;
}

static long long compositor_wakeups(struct compositor *compositor, double seconds) {
    long long before = context_switches(compositor->pid);
    harness_sleep(seconds);
    return context_switches(compositor->pid) - before;
}

static void client_start(struct client *client, const char *work, const char *binary,
        const char *name, const char *colour, const char *runtime, const char *socket) {
    memset(client, 0, sizeof *client);
    client->binary = binary;
    snprintf(client->name, sizeof client->name, "%s", name);
    int written = snprintf(client->log_path, sizeof client->log_path, "%s/%s.log", work, name);
    if (written < 0 || (size_t)written >= sizeof client->log_path)
        harness_fail("a path is too long");
    char *argv[16];
    int used = 0;
    argv[used++] = (char *)binary;
    argv[used++] = "--mode";
    argv[used++] = "xdg";
    argv[used++] = "--app-id";
    argv[used++] = (char *)name;
    argv[used++] = "--title";
    argv[used++] = (char *)name;
    argv[used++] = "--size";
    argv[used++] = "180x180";
    argv[used++] = "--color";
    argv[used++] = (char *)colour;
    argv[used++] = "--seconds";
    argv[used++] = "3600";
    argv[used] = NULL;
    apply_client_environment(runtime, socket);
    client->pid = harness_spawn(argv, environ, NULL, NULL, client->log_path, 1);
    double deadline = harness_now() + 60;
    while (harness_now() < deadline) {
        size_t length = 0;
        char *text = read_whole_file(client->log_path, &length);
        if (!text) {
            text = strdup("");
            if (!text) harness_fail("out of memory");
        }
        if (strstr(text, "mapped")) {
            free(text);
            return;
        }
        if (process_exited(client->pid)) {
            failf("%s exited while mapping:\n%s", name, text);
        }
        free(text);
        harness_sleep(0.05);
    }
    size_t length = 0;
    char *text = read_whole_file(client->log_path, &length);
    if (!text) {
        text = strdup("");
        if (!text) harness_fail("out of memory");
    }
    failf("%s never mapped:\n%s", name, text);
}

static void client_stop(struct client *client) {
    if (client->pid > 0) {
        int status = 0;
        pid_t done = waitpid(client->pid, &status, WNOHANG);
        if (done == 0) {
            kill(client->pid, SIGTERM);
            if (harness_wait_status(client->pid, 10) < 0) {
                kill(client->pid, SIGKILL);
                harness_wait_status(client->pid, 10);
            }
        }
        client->pid = 0;
    }
}

struct measurement {
    char label[128];
    long long expected_ms;
    long long settle_ms;
    char digest[17];
    long long drawn;
    long long period;
    long long refresh;
    long long frames;
    long long gaps;
    long long dead;
    long long stale;
    long long span;
    long long interval;
    long long steady;
    long long first;
    long long lead_min;
    long long lead_max;
    int counted;
};

static double measurement_refreshes(const struct measurement *measurement) {
    return measurement->period ? (double)measurement->span / (double)measurement->period : 0.0;
}

static const char *measurement_line(const struct measurement *measurement, char *buffer,
        size_t size) {
    if (!measurement->counted) {
        snprintf(buffer, size, "%s: counters unavailable, screen %s",
                measurement->label, measurement->digest);
        return buffer;
    }
    double refreshes = measurement_refreshes(measurement);
    double share = refreshes ? (double)measurement->frames / refreshes : 0.0;
    double followed = measurement->drawn ?
        (double)measurement->frames / (double)measurement->drawn : 0.0;
    snprintf(buffer, size,
            "%s: %lld animation frames over %.1f refreshes (%.1f ms, %.2f a refresh),"
            " %lld composited frames (%.2f an animation frame), worst interval %.2f ms"
            " (steady %.2f ms), period %.2f ms, first frame after %.2f ms, sampled"
            " %.2f..%.2f ms ahead, %lld dead, %lld stale, %lld skipped, screen %s",
            measurement->label, measurement->frames, refreshes,
            (double)measurement->span / 1e6, share, measurement->drawn, followed,
            (double)measurement->interval / 1e6, (double)measurement->steady / 1e6,
            (double)measurement->period / 1e6, (double)measurement->first / 1e6,
            (double)measurement->lead_min / 1e6, (double)measurement->lead_max / 1e6,
            measurement->dead, measurement->stale, measurement->gaps, measurement->digest);
    return buffer;
}

static void measurement_check(const struct measurement *measurement, struct problems *problems) {
    if (!measurement->counted) return;
    if (measurement->refresh != REFRESH_HZ * 1000) {
        problem_add_to(problems, "%s: the output runs at %.0f Hz, not %d",
                measurement->label, (double)measurement->refresh / 1000, REFRESH_HZ);
    }
    if (measurement->dead) {
        problem_add_to(problems, "%s: %lld frame(s) showed the animation's start"
                " position, so the first frame after the input did not move",
                measurement->label, measurement->dead);
    }
    if (measurement->first <= 0) {
        problem_add_to(problems, "%s: no frame advanced the animation", measurement->label);
    }
    double refreshes = measurement_refreshes(measurement);
    long long stale = measurement->frames / SKIP_SHARE;
    if (stale < 1) stale = 1;
    if (measurement->stale > stale) {
        problem_add_to(problems, "%s: %lld of %lld animation frames were sampled less than"
                " half a refresh (%.2f ms) ahead of their frame start, so the state lagged"
                " the display", measurement->label, measurement->stale, measurement->frames,
                (double)measurement->period / 2e6);
    }
    if (measurement->span < measurement->expected_ms * 800000) {
        problem_add_to(problems, "%s: the animation ran for %.1f ms of its %lld ms settle"
                " time", measurement->label, (double)measurement->span / 1e6,
                measurement->settle_ms);
    }
    if ((double)measurement->frames < refreshes) {
        problem_add_to(problems, "%s: %lld animation frames for %.1f refreshes: fewer than"
                " one a refresh", measurement->label, measurement->frames, refreshes);
    }
    if (measurement->drawn && (double)measurement->frames < (double)measurement->drawn *
            FRAME_SHARE) {
        problem_add_to(problems, "%s: %lld animation frames for the %lld frames the output"
                " composited: the animation stopped following the display",
                measurement->label, measurement->frames, measurement->drawn);
    }
    long long allowed = measurement->frames / SKIP_SHARE;
    if (allowed < 1) allowed = 1;
    if (measurement->gaps > allowed) {
        problem_add_to(problems, "%s: %lld refresh(es) went by without an animation frame"
                " while the animation drew %lld, more than the one in %d a loaded host may"
                " skip", measurement->label, measurement->gaps, measurement->frames,
                SKIP_SHARE);
    }
}

static void measure(struct compositor *compositor, const char *label, const char *command,
        long long expected_ms, long long settle_ms, const char *frames_dir,
        const char *scenario, struct measurement *result) {
    struct counters before;
    int have_before = compositor_animation(compositor, &before);
    long long drawn = 0;
    if (!compositor_drawn(compositor, &drawn)) drawn = 0;
    compositor_command(compositor, command);
    double pause = settle_ms / 1000.0 * 3.5;
    harness_sleep(pause < 0.8 ? 0.8 : pause);
    struct counters after;
    int have_after = compositor_animation(compositor, &after);
    long long total = 0;
    if (!compositor_drawn(compositor, &total)) drawn = 0;
    else drawn = total - drawn;
    memset(result, 0, sizeof *result);
    snprintf(result->label, sizeof result->label, "%s", label);
    result->expected_ms = expected_ms;
    result->settle_ms = settle_ms;
    result->drawn = drawn;
    compositor_digest(compositor, scenario, frames_dir, result->digest);
    result->period = have_after ? after.period : 0;
    result->refresh = have_after ? after.refresh : 0;
    result->frames = (have_after ? after.frames : 0) - (have_before ? before.frames : 0);
    result->gaps = (have_after ? after.gaps : 0) - (have_before ? before.gaps : 0);
    result->dead = (have_after ? after.dead : 0) - (have_before ? before.dead : 0);
    result->stale = (have_after ? after.stale : 0) - (have_before ? before.stale : 0);
    result->span = have_after ? after.span : 0;
    result->interval = have_after ? after.interval : 0;
    result->steady = have_after ? after.steady : 0;
    result->first = have_after ? after.first : 0;
    result->lead_min = have_after ? after.lead_min : 0;
    result->lead_max = have_after ? after.lead_max : 0;
    result->counted = have_after && have_before;
    char line[4096];
    say(measurement_line(result, line, sizeof line));
}

static long long python_round(double value) {
    long long truncated = (long long)value;
    double fraction = value - (double)truncated;
    if (fraction > 0.5) return truncated + 1;
    if (fraction < -0.5) return truncated - 1;
    if (fraction == 0.5) return truncated % 2 == 0 ? truncated : truncated + 1;
    if (fraction == -0.5) return truncated % 2 == 0 ? truncated : truncated - 1;
    return truncated;
}

static struct compositor stalled_compositor;
static int stalled_running;

static long long stalled_control(const char *work, const char *tomoe, const char *socket,
        const char *policy, const char *frames_dir, long long period) {
    volatile long long stall_ms = period ? python_round((double)period / 1e6 * 4.0) : 9;
    if (stall_ms < 1) stall_ms = 1;
    control_problems.count = 0;
    stalled_running = 0;
    char stalled_socket[512];
    snprintf(stalled_socket, sizeof stalled_socket, "%s-stall", socket);
    char option[64];
    snprintf(option, sizeof option, "--test-stall-fence-ms=%lld", stall_ms);
    const char *options[2] = { "--test-stall-fence", option };
    struct fail_frame frame;
    frame.previous = failure_frame;
    failure_frame = &frame;
    if (setjmp(frame.buffer) == 0) {
        compositor_start(&stalled_compositor, work, tomoe, stalled_socket, options, 2);
        stalled_running = 1;
        compositor_mount(&stalled_compositor, policy);
        harness_sleep(0.6);
        struct measurement control;
        measure(&stalled_compositor, "stalled frames", "bar", 73, 73, frames_dir, "stalled",
                &control);
        struct command_result quit = compositor_call(&stalled_compositor, 1, "quit");
        free(quit.text);
        compositor_stop(&stalled_compositor);
        stalled_running = 0;
        failure_frame = frame.previous;
        measurement_check(&control, &control_problems);
        return stall_ms;
    }
    if (stalled_running) {
        struct command_result quit = compositor_call(&stalled_compositor, 1, "quit");
        free(quit.text);
        compositor_stop(&stalled_compositor);
        stalled_running = 0;
    }
    failure_frame = frame.previous;
    rethrow();
    return 0;
}

static struct compositor main_compositor;
static struct client main_client;
static int main_compositor_started;
static int main_client_started;
static char work_dir[4096];
static char frames_dir[4096];
static char policy_path[4096];
static char socket_name[256];

static void usage(const char *name) {
    fprintf(stderr, "usage: %s --tomoe TOMOE --client CLIENT [--frames DIR]"
            " [--allow-missing-counters]\n", name);
}

int main(int argc, char **argv) {
    const char *tomoe = NULL;
    const char *client = NULL;
    const char *frames_option = NULL;
    int allow_missing = 0;
    for (int index = 1; index < argc; index++) {
        const char *argument = argv[index];
        if (!strcmp(argument, "--tomoe") && index + 1 < argc) {
            tomoe = argv[++index];
        } else if (!strncmp(argument, "--tomoe=", 8)) {
            tomoe = argument + 8;
        } else if (!strcmp(argument, "--client") && index + 1 < argc) {
            client = argv[++index];
        } else if (!strncmp(argument, "--client=", 9)) {
            client = argument + 9;
        } else if (!strcmp(argument, "--frames") && index + 1 < argc) {
            frames_option = argv[++index];
        } else if (!strncmp(argument, "--frames=", 9)) {
            frames_option = argument + 9;
        } else if (!strcmp(argument, "--allow-missing-counters")) {
            allow_missing = 1;
        } else {
            usage(argv[0]);
            return 2;
        }
    }
    if (!tomoe || !client) {
        usage(argv[0]);
        return 2;
    }
    if (frames_option && *frames_option) {
        snprintf(frames_dir, sizeof frames_dir, "%s", frames_option);
    } else {
        make_tempdir(frames_dir, sizeof frames_dir, "animations-frames-");
    }
    harness_mkparents(frames_dir, 0777);
    make_tempdir(work_dir, sizeof work_dir, "animations-");
    harness_join(policy_path, sizeof policy_path, work_dir, "probe.lisp");
    harness_write_file(policy_path, policy_text);
    snprintf(socket_name, sizeof socket_name, "animations-%d", (int)getpid());
    remember_environment();
    struct fail_frame frame;
    frame.previous = NULL;
    failure_frame = &frame;
    if (setjmp(frame.buffer) == 0) {
        compositor_start(&main_compositor, work_dir, tomoe, socket_name, NULL, 0);
        main_compositor_started = 1;
        compositor_mount(&main_compositor, policy_path);
        harness_sleep(0.6);
        struct counters counters;
        if (compositor_animation(&main_compositor, &counters)) {
            if (!counters.have_move) failf("'move-settle-ms'");
            if (!counters.have_open) failf("'open-ms'");
            if (!counters.have_view) failf("'view-settle-ms'");
            char configured[1024];
            snprintf(configured, sizeof configured,
                    "configured: move settles in %s ms, open lasts %s ms, view settles in %s ms",
                    counters.move_text, counters.open_text, counters.view_text);
            say(configured);
            long long move_settle = 0, open_ms = 0;
            if (!text_integer(counters.move_text, &move_settle))
                harness_fail("cannot read an animation counter");
            if (!text_integer(counters.open_text, &open_ms))
                harness_fail("cannot read an animation counter");
            if (move_settle > 120)
                problem_add("the move spring settles in %s ms", counters.move_text);
            if (open_ms > 120)
                problem_add("the open ease lasts %s ms", counters.open_text);
        }
        say("gates: one animation frame in 8 may be sampled behind the display or skip a"
                " refresh on a loaded host, the animation has to follow 90% of the frames the"
                " output composites, and it still has to draw one a refresh");
        struct measurement measured[4];
        size_t measured_count = 0;
        measure(&main_compositor, "bar slide", "bar", 73, 73, frames_dir, "bar",
                &measured[measured_count++]);
        client_start(&main_client, work_dir, client, "anim-window", "ffcc00",
                main_compositor.runtime, socket_name);
        main_client_started = 1;
        harness_sleep(0.6);
        compositor_command(&main_compositor, "hide");
        harness_sleep(0.4);
        measure(&main_compositor, "window open", "show", 90, 90, frames_dir, "open",
                &measured[measured_count++]);
        compositor_command(&main_compositor, "show");
        harness_sleep(0.4);
        measure(&main_compositor, "window move", "move", 73, 73, frames_dir, "move",
                &measured[measured_count++]);
        if (!strcmp(measured[measured_count - 1].digest, measured[measured_count - 2].digest))
            problem_add("the window never moved");
        struct frame resting;
        frame_capture(&main_compositor, NULL, NULL, &resting);
        compositor_command(&main_compositor, "move");
        struct frame early;
        frame_capture(&main_compositor, NULL, NULL, &early);
        if (resting.pixel_length == early.pixel_length &&
                !memcmp(resting.pixels, early.pixels, resting.pixel_length))
            problem_add("the first frame after the window move input shows no motion");
        frame_release(&resting);
        frame_release(&early);
        harness_sleep(0.5);
        measure(&main_compositor, "workspace scroll", "view", 73, 73, frames_dir, "view",
                &measured[measured_count++]);
        if (!strcmp(measured[measured_count - 1].digest, measured[measured_count - 2].digest))
            problem_add("the view never moved");
        int all_uncounted = 1;
        for (size_t index = 0; index < measured_count; index++)
            if (measured[index].counted) all_uncounted = 0;
        if (all_uncounted) {
            if (!allow_missing)
                problem_add("the compositor does not report animation counters");
        }
        for (size_t index = 0; index < measured_count; index++)
            measurement_check(&measured[index], &main_problems);
        harness_sleep(0.4);
        long long wakes = compositor_wakeups(&main_compositor, 1.0);
        char rest_line[128];
        snprintf(rest_line, sizeof rest_line, "rest: %lld wakeups/s", wakes);
        say(rest_line);
        if (wakes > 2)
            problem_add("the compositor woke %lld times in a second at rest", wakes);
        if (measured_count && measured[0].counted) {
            long long stall_ms = stalled_control(work_dir, tomoe, socket_name, policy_path,
                    frames_dir, measured[0].period);
            if (control_problems.count) {
                char control_line[2048];
                snprintf(control_line, sizeof control_line,
                        "negative control: a frame stall of %lld ms is rejected: %s",
                        stall_ms, control_problems.items[0]);
                say(control_line);
            } else {
                problem_add("a frame stall of %lld ms passed the pacing gates", stall_ms);
            }
        } else {
            say("negative control: skipped, this run reports no animation counters");
        }
        printf("frames: %s\n", frames_dir);
        fflush(stdout);
    } else {
        problem_add("%s", failure_text);
    }
    if (main_client_started) client_stop(&main_client);
    if (main_compositor_started) {
        struct command_result quit = compositor_call(&main_compositor, 1, "quit");
        free(quit.text);
        compositor_stop(&main_compositor);
    }
    remove_tree(work_dir);
    for (size_t index = 0; index < main_problems.count; index++) {
        char line[65536];
        snprintf(line, sizeof line, "animation speed: FAILED: %s", main_problems.items[index]);
        say(line);
    }
    const char *report = getenv("ANIMATIONS_REPORT");
    if (report && *report) {
        FILE *file = fopen(report, "w");
        if (!file) harness_fail("cannot write the report");
        for (size_t index = 0; index < report_count; index++)
            fprintf(file, "%s\n", report_lines[index]);
        fclose(file);
    }
    return main_problems.count ? 1 : 0;
}
