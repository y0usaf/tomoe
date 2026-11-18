#include "harness.h"
#include <ctype.h>
#include <float.h>
#include <stdarg.h>
#include <sys/mman.h>
#include <zlib.h>

#define PATH_SIZE 4096
#define FAILURE_MAX 64
#define FAILURE_TEXT 1024
#define FRAME_MAX 8
#define FIELD_MAX 48
#define SERIES_MAX 512
#define ANIM_FIELD_MAX 96
#define ANIM_TIME_MAX 4096
#define SAMPLE_MAX 8192
#define POINT_MAX 1 << 20

static const char *tomoe_binary;
static const char *pointer_binary;
static const char *work_directory;
static const char *emit_directory;
static const char *report_path;
static int option_trail;
static int option_verbose;
static int option_fps;
static int option_steps;
static double option_idle;
static double option_cell;
static double option_life;

static const double TOLERANCE = 6.0;
static const double MAXIMUM = 20.0;
static const int DRAGS = 6;
static const double COST_GROWTH = 1.5;
static const double COST_SLACK = 1.0;
static const int CAP_FPS = 60;
static const double CAP_GROWTH = 1.25;
static const double STEP_GROWTH = 1.5;

static const char *MARKER_SHADER =
    "void mainImage(out vec4 color, vec2 at) {\n"
    "    color = vec4(1.0, 0.0, 1.0, 1.0);\n"
    "}\n";

static const char *POLICY_HEAD =
    "(in-package #:tomoe-user)\n"
    "\n"
    "(define-extension \"cursor-trail\" (:reads (:outputs) :state nil) (snapshot state event)\n"
    "  (declare (ignore state event))\n"
    "  (let ((output (first (context snapshot :outputs))))\n"
    "    (values nil\n"
    "            (append\n"
    "             (when output\n"
    "               (list (configure-output (getf output :name)\n"
    "                                       :mode (list 1280 720 %d)\n"
    "                                       :scale 1 :position '(0 0))))\n"
    "             (list ";

static const char *POLICY_TAIL =
    "))\n"
    "            nil)))\n";

static const char *TRAIL_TEMPLATE =
    "(shell-surface :cursor-trail (ui :stack)\n"
    "                                  :anchors '(:top :right :bottom :left)\n"
    "                                  :layer %s\n"
    "                                  :click-through t\n"
    "                                  :background '(:trail :cell %s :life %s\n"
    "                                                :steps %d :color #xb8e0ffff\n"
    "                                                :fps %d :idle %s))";

static const char *MARKER_TEMPLATE =
    "(shell-surface :marker (ui :stack)\n"
    "                             :anchors '(:left :top)\n"
    "                             :layer :overlay\n"
    "                             :click-through t\n"
    "                             :width 240 :height 160 :margin '(40 40 0 0)\n"
    "                             :background (list :shader \"%s\" :fps 0 :idle 0.5))";

struct point {
    long long x;
    long long y;
};

struct failure_list {
    int count;
    char messages[FAILURE_MAX][FAILURE_TEXT];
};

static void *xmalloc(size_t size) {
    void *block = malloc(size);
    if (!block) harness_fail("out of memory");
    return block;
}

static void *xrealloc(void *block, size_t size) {
    void *grown = realloc(block, size);
    if (!grown) harness_fail("out of memory");
    return grown;
}

static char *xstrdup(const char *text) {
    size_t length = strlen(text);
    char *copy = xmalloc(length + 1);
    memcpy(copy, text, length + 1);
    return copy;
}

struct json_node;
struct compositor;

static void compositor_stop(struct compositor *compositor);
static void attach_failures(struct json_node *report, const struct failure_list *failures);
static void report_print(struct json_node *report);
static void report_save(struct json_node *report, const char *path);

static struct json_node *active_report;
static struct failure_list *active_failures;
static struct compositor *active_compositor;
static int active_flow;

static void failf(const char *format, ...) {
    char text[16384];
    va_list arguments;
    va_start(arguments, format);
    vsnprintf(text, sizeof text, format, arguments);
    va_end(arguments);
    if (active_flow) {
        active_flow = 0;
        compositor_stop(active_compositor);
        attach_failures(active_report, active_failures);
        report_print(active_report);
        if (report_path) report_save(active_report, report_path);
    }
    harness_fail(text);
}

static void failure_add(struct failure_list *list, const char *format, ...) {
    char text[4096];
    va_list arguments;
    va_start(arguments, format);
    vsnprintf(text, sizeof text, format, arguments);
    va_end(arguments);
    if (list->count >= FAILURE_MAX) return;
    size_t length = strlen(text);
    if (length >= FAILURE_TEXT) length = FAILURE_TEXT - 1;
    memcpy(list->messages[list->count], text, length);
    list->messages[list->count][length] = '\0';
    list->count++;
}

struct text {
    char *data;
    size_t length;
    size_t capacity;
};

static void text_init(struct text *text) {
    text->capacity = 256;
    text->length = 0;
    text->data = xmalloc(text->capacity);
    text->data[0] = '\0';
}

static void text_reserve(struct text *text, size_t extra) {
    size_t needed = text->length + extra + 1;
    while (text->capacity < needed) text->capacity *= 2;
    text->data = xrealloc(text->data, text->capacity);
}

static void text_add(struct text *text, const char *format, ...) {
    va_list arguments;
    va_start(arguments, format);
    va_list copy;
    va_copy(copy, arguments);
    int needed = vsnprintf(NULL, 0, format, copy);
    va_end(copy);
    if (needed < 0) harness_fail("cannot format text");
    text_reserve(text, (size_t)needed);
    vsnprintf(text->data + text->length, text->capacity - text->length, format, arguments);
    va_end(arguments);
    text->length += (size_t)needed;
}

static void float_text(double value, char *out, size_t size) {
    if (value == 0.0) {
        snprintf(out, size, "%s", value < 0.0 ? "-0.0" : "0.0");
        return;
    }
    double magnitude = value < 0.0 ? -value : value;
    if (magnitude >= 1e-4 && magnitude < 1e15) {
        for (int digits = 1; digits <= 17; digits++) {
            snprintf(out, size, "%.*f", digits, value);
            if (strtod(out, NULL) == value) return;
        }
    }
    for (int precision = 1; precision <= 17; precision++) {
        snprintf(out, size, "%.*g", precision, value);
        if (strtod(out, NULL) == value) {
            if (!strpbrk(out, ".eEn")) {
                size_t length = strlen(out);
                if (length + 2 < size) {
                    out[length] = '.';
                    out[length + 1] = '0';
                    out[length + 2] = '\0';
                }
            }
            return;
        }
    }
    snprintf(out, size, "%.17g", value);
}

static double round_digits(double value, int digits) {
    char formatted[128];
    snprintf(formatted, sizeof formatted, "%.*f", digits, value);
    return strtod(formatted, NULL);
}

static long long round_integer(double value) {
    double base = value < 0.0 ? -(double)(long long)(-value) : (double)(long long)value;
    double fraction = value - base;
    if (fraction > 0.5) base += 1.0;
    else if (fraction < -0.5) base -= 1.0;
    else if (fraction == 0.5) {
        if (((long long)base) % 2 != 0) base += 1.0;
    } else if (fraction == -0.5) {
        if (((long long)base) % 2 != 0) base -= 1.0;
    }
    return (long long)base;
}

static const double PI_VALUE = 3.14159265358979323846;
static const double TAU_VALUE = 6.28318530717958647692;

static double sine_value(double angle) {
    double reduced = angle - TAU_VALUE * (double)(long long)(angle / TAU_VALUE);
    if (reduced > PI_VALUE) reduced -= TAU_VALUE;
    if (reduced < -PI_VALUE) reduced += TAU_VALUE;
    if (reduced > PI_VALUE / 2) reduced = PI_VALUE - reduced;
    else if (reduced < -PI_VALUE / 2) reduced = -PI_VALUE - reduced;
    double term = reduced;
    double total = reduced;
    double square = reduced * reduced;
    for (int index = 1; index < 14; index++) {
        term *= -square / (double)((2 * index) * (2 * index + 1));
        total += term;
    }
    return total;
}

static double cosine_value(double angle) {
    return sine_value(angle + PI_VALUE / 2);
}

static double square_root(double value) {
    if (!(value > 0.0)) return 0.0;
    double guess = value > 1.0 ? value : 1.0;
    for (int index = 0; index < 60; index++) guess = 0.5 * (guess + value / guess);
    return guess;
}

static double clamp_unit(double value) {
    if (value < 0.0) return 0.0;
    if (value > 1.0) return 1.0;
    return value;
}

static int compare_double(const void *left, const void *right) {
    double first = *(const double *)left;
    double second = *(const double *)right;
    if (first < second) return -1;
    if (first > second) return 1;
    return 0;
}

static double percentile(const double *values, int count, double fraction) {
    if (count <= 0) return 0.0;
    double *ordered = xmalloc((size_t)count * sizeof(double));
    memcpy(ordered, values, (size_t)count * sizeof(double));
    qsort(ordered, (size_t)count, sizeof(double), compare_double);
    double scaled = fraction * (double)count;
    double whole = (double)(long long)scaled;
    if (whole < scaled) whole += 1.0;
    long long index = (long long)whole - 1;
    if (index < 0) index = 0;
    if (index > count - 1) index = count - 1;
    double result = ordered[index];
    free(ordered);
    return result;
}

enum json_kind { JSON_OBJECT, JSON_ARRAY, JSON_INTEGER, JSON_REAL, JSON_BOOLEAN, JSON_STRING };

struct json_node {
    enum json_kind kind;
    long long integer;
    double real;
    int boolean;
    char *text;
    char *key;
    struct json_node *first;
    struct json_node *last;
    struct json_node *next;
};

static struct json_node *json_new(enum json_kind kind) {
    struct json_node *node = xmalloc(sizeof(struct json_node));
    memset(node, 0, sizeof(struct json_node));
    node->kind = kind;
    return node;
}

static struct json_node *json_object(void) {
    return json_new(JSON_OBJECT);
}

static struct json_node *json_array(void) {
    return json_new(JSON_ARRAY);
}

static struct json_node *node_int(long long value) {
    struct json_node *node = json_new(JSON_INTEGER);
    node->integer = value;
    return node;
}

static struct json_node *node_real(double value) {
    struct json_node *node = json_new(JSON_REAL);
    node->real = value;
    return node;
}

static struct json_node *node_bool(int value) {
    struct json_node *node = json_new(JSON_BOOLEAN);
    node->boolean = value ? 1 : 0;
    return node;
}

static struct json_node *node_string(const char *value) {
    struct json_node *node = json_new(JSON_STRING);
    node->text = xstrdup(value);
    return node;
}

static void json_add(struct json_node *object, const char *key, struct json_node *value) {
    value->key = xstrdup(key);
    if (object->last) object->last->next = value;
    else object->first = value;
    object->last = value;
}

static void json_append(struct json_node *array, struct json_node *value) {
    if (array->last) array->last->next = value;
    else array->first = value;
    array->last = value;
}

static void json_indent(FILE *out, int depth) {
    for (int index = 0; index < depth * 2; index++) fputc(' ', out);
}

static void json_quote(const char *text, FILE *out) {
    fputc('"', out);
    for (const unsigned char *at = (const unsigned char *)text; *at; at++) {
        if (*at == '"' || *at == '\\') {
            fputc('\\', out);
            fputc(*at, out);
        } else if (*at == '\n') {
            fputs("\\n", out);
        } else if (*at == '\t') {
            fputs("\\t", out);
        } else if (*at == '\r') {
            fputs("\\r", out);
        } else if (*at < 0x20) {
            fprintf(out, "\\u%04x", *at);
        } else {
            fputc(*at, out);
        }
    }
    fputc('"', out);
}

static void json_write(struct json_node *node, FILE *out, int depth) {
    if (node->kind == JSON_OBJECT) {
        if (!node->first) {
            fputs("{}", out);
            return;
        }
        fputs("{\n", out);
        for (struct json_node *member = node->first; member; member = member->next) {
            json_indent(out, depth + 1);
            json_quote(member->key, out);
            fputs(": ", out);
            json_write(member, out, depth + 1);
            if (member->next) fputc(',', out);
            fputc('\n', out);
        }
        json_indent(out, depth);
        fputc('}', out);
        return;
    }
    if (node->kind == JSON_ARRAY) {
        if (!node->first) {
            fputs("[]", out);
            return;
        }
        fputs("[\n", out);
        for (struct json_node *item = node->first; item; item = item->next) {
            json_indent(out, depth + 1);
            json_write(item, out, depth + 1);
            if (item->next) fputc(',', out);
            fputc('\n', out);
        }
        json_indent(out, depth);
        fputc(']', out);
        return;
    }
    if (node->kind == JSON_INTEGER) {
        fprintf(out, "%lld", node->integer);
        return;
    }
    if (node->kind == JSON_REAL) {
        char formatted[64];
        float_text(node->real, formatted, sizeof formatted);
        fputs(formatted, out);
        return;
    }
    if (node->kind == JSON_BOOLEAN) {
        fputs(node->boolean ? "true" : "false", out);
        return;
    }
    json_quote(node->text, out);
}

static void report_print(struct json_node *report) {
    json_write(report, stdout, 0);
    fputc('\n', stdout);
    fflush(stdout);
}

static void report_save(struct json_node *report, const char *path) {
    FILE *file = fopen(path, "w");
    if (!file) failf("cannot write %s", path);
    json_write(report, file, 0);
    if (fclose(file) != 0) failf("cannot write %s", path);
}

static unsigned int rotate_right(unsigned int value, int bits) {
    return (value >> bits) | (value << (32 - bits));
}

static const unsigned int SHA256_CONSTANTS[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u,
    0x923f82a4u, 0xab1c5ed5u, 0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
    0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u, 0xe49b69c1u, 0xefbe4786u,
    0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u,
    0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
    0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u, 0xa2bfe8a1u, 0xa81a664bu,
    0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au,
    0x5b9cca4fu, 0x682e6ff3u, 0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u
};

struct sha256 {
    unsigned int state[8];
    unsigned long long length;
    unsigned char block[64];
    size_t used;
};

static void sha256_init(struct sha256 *hash) {
    hash->state[0] = 0x6a09e667u;
    hash->state[1] = 0xbb67ae85u;
    hash->state[2] = 0x3c6ef372u;
    hash->state[3] = 0xa54ff53au;
    hash->state[4] = 0x510e527fu;
    hash->state[5] = 0x9b05688cu;
    hash->state[6] = 0x1f83d9abu;
    hash->state[7] = 0x5be0cd19u;
    hash->length = 0;
    hash->used = 0;
}

static void sha256_compress(struct sha256 *hash, const unsigned char *block) {
    unsigned int words[64];
    for (int index = 0; index < 16; index++) {
        words[index] = (unsigned int)block[index * 4] << 24 |
            (unsigned int)block[index * 4 + 1] << 16 |
            (unsigned int)block[index * 4 + 2] << 8 |
            (unsigned int)block[index * 4 + 3];
    }
    for (int index = 16; index < 64; index++) {
        unsigned int first = words[index - 15];
        unsigned int second = words[index - 2];
        unsigned int sigma0 = rotate_right(first, 7) ^ rotate_right(first, 18) ^ (first >> 3);
        unsigned int sigma1 = rotate_right(second, 17) ^ rotate_right(second, 19) ^ (second >> 10);
        words[index] = words[index - 16] + sigma0 + words[index - 7] + sigma1;
    }
    unsigned int a = hash->state[0], b = hash->state[1], c = hash->state[2], d = hash->state[3];
    unsigned int e = hash->state[4], f = hash->state[5], g = hash->state[6], h = hash->state[7];
    for (int index = 0; index < 64; index++) {
        unsigned int sum1 = rotate_right(e, 6) ^ rotate_right(e, 11) ^ rotate_right(e, 25);
        unsigned int choice = (e & f) ^ (~e & g);
        unsigned int first = h + sum1 + choice + SHA256_CONSTANTS[index] + words[index];
        unsigned int sum0 = rotate_right(a, 2) ^ rotate_right(a, 13) ^ rotate_right(a, 22);
        unsigned int majority = (a & b) ^ (a & c) ^ (b & c);
        unsigned int second = sum0 + majority;
        h = g;
        g = f;
        f = e;
        e = d + first;
        d = c;
        c = b;
        b = a;
        a = first + second;
    }
    hash->state[0] += a;
    hash->state[1] += b;
    hash->state[2] += c;
    hash->state[3] += d;
    hash->state[4] += e;
    hash->state[5] += f;
    hash->state[6] += g;
    hash->state[7] += h;
}

static void sha256_update(struct sha256 *hash, const unsigned char *data, size_t size) {
    hash->length += size;
    while (size) {
        size_t room = 64 - hash->used;
        size_t take = size < room ? size : room;
        memcpy(hash->block + hash->used, data, take);
        hash->used += take;
        data += take;
        size -= take;
        if (hash->used == 64) {
            sha256_compress(hash, hash->block);
            hash->used = 0;
        }
    }
}

static void sha256_final(struct sha256 *hash, unsigned char out[32]) {
    unsigned long long bits = hash->length * 8;
    unsigned char pad = 0x80;
    sha256_update(hash, &pad, 1);
    unsigned char zero = 0;
    while (hash->used != 56) sha256_update(hash, &zero, 1);
    unsigned char tail[8];
    for (int index = 0; index < 8; index++) tail[index] = (unsigned char)(bits >> (56 - index * 8));
    sha256_update(hash, tail, 8);
    for (int index = 0; index < 8; index++) {
        out[index * 4] = (unsigned char)(hash->state[index] >> 24);
        out[index * 4 + 1] = (unsigned char)(hash->state[index] >> 16);
        out[index * 4 + 2] = (unsigned char)(hash->state[index] >> 8);
        out[index * 4 + 3] = (unsigned char)hash->state[index];
    }
}

static unsigned char *read_all(const char *path, size_t *size) {
    FILE *file = fopen(path, "rb");
    if (!file) return NULL;
    size_t capacity = 65536;
    size_t used = 0;
    unsigned char *data = xmalloc(capacity);
    for (;;) {
        if (used + 65536 > capacity) {
            capacity *= 2;
            data = xrealloc(data, capacity);
        }
        size_t got = fread(data + used, 1, capacity - used, file);
        used += got;
        if (got == 0) break;
    }
    if (ferror(file)) {
        fclose(file);
        free(data);
        return NULL;
    }
    fclose(file);
    if (size) *size = used;
    return data;
}

static void file_digest(const char *path, char out[65]) {
    size_t size = 0;
    unsigned char *data = read_all(path, &size);
    if (!data) failf("cannot read %s", path);
    struct sha256 hash;
    sha256_init(&hash);
    sha256_update(&hash, data, size);
    unsigned char digest[32];
    sha256_final(&hash, digest);
    free(data);
    for (int index = 0; index < 32; index++) snprintf(out + index * 2, 3, "%02x", digest[index]);
    out[64] = '\0';
}

static int files_equal(const char *first, const char *second) {
    size_t first_size = 0, second_size = 0;
    unsigned char *first_data = read_all(first, &first_size);
    unsigned char *second_data = read_all(second, &second_size);
    int equal = first_data && second_data && first_size == second_size &&
        memcmp(first_data, second_data, first_size) == 0;
    free(first_data);
    free(second_data);
    return equal;
}

struct child_env {
    char **items;
    int count;
    int capacity;
};

static void child_env_push(struct child_env *env, const char *text) {
    if (env->count + 2 > env->capacity) {
        env->capacity *= 2;
        env->items = xrealloc(env->items, (size_t)env->capacity * sizeof(char *));
    }
    env->items[env->count++] = xstrdup(text);
    env->items[env->count] = NULL;
}

static void child_env_init(struct child_env *env) {
    env->count = 0;
    env->capacity = 64;
    env->items = xmalloc((size_t)env->capacity * sizeof(char *));
    env->items[0] = NULL;
    for (char **at = environ; at && *at; at++) child_env_push(env, *at);
}

static void child_env_set(struct child_env *env, const char *key, const char *value) {
    size_t length = strlen(key);
    char *text = xmalloc(length + strlen(value) + 2);
    sprintf(text, "%s=%s", key, value);
    for (int index = 0; index < env->count; index++) {
        if (strncmp(env->items[index], key, length) == 0 && env->items[index][length] == '=') {
            free(env->items[index]);
            env->items[index] = text;
            return;
        }
    }
    if (env->count + 2 > env->capacity) {
        env->capacity *= 2;
        env->items = xrealloc(env->items, (size_t)env->capacity * sizeof(char *));
    }
    env->items[env->count++] = text;
    env->items[env->count] = NULL;
}

static void child_env_free(struct child_env *env) {
    for (int index = 0; index < env->count; index++) free(env->items[index]);
    free(env->items);
    env->items = NULL;
    env->count = 0;
    env->capacity = 0;
}

struct capture {
    int status;
    char *out;
    char *err;
};

static char *read_descriptor(int fd) {
    size_t capacity = 65536;
    size_t used = 0;
    char *text = xmalloc(capacity);
    for (;;) {
        if (used + 4096 > capacity) {
            capacity *= 2;
            text = xrealloc(text, capacity);
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

static struct capture capture_run(char *const argv[], char **env) {
    struct capture result = { -1, NULL, NULL };
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
        execvpe(argv[0], argv, env);
        fprintf(stderr, "harness: cannot run %s: %s\n", argv[0], strerror(errno));
        _exit(127);
    }
    close(out_pipe[1]);
    close(err_pipe[1]);
    result.out = read_descriptor(out_pipe[0]);
    result.err = read_descriptor(err_pipe[0]);
    close(out_pipe[0]);
    close(err_pipe[0]);
    result.status = harness_wait_status(child, 120);
    return result;
}

static void trim_text(char *text) {
    size_t length = strlen(text);
    while (length && isspace((unsigned char)text[length - 1])) text[--length] = '\0';
    size_t start = 0;
    while (text[start] && isspace((unsigned char)text[start])) start++;
    if (start) memmove(text, text + start, length - start + 1);
}

struct image {
    int width;
    int height;
    unsigned char *data;
    unsigned char *pixels;
    size_t pixel_bytes;
};

static void image_load(const char *path, struct image *image) {
    size_t size = 0;
    image->data = read_all(path, &size);
    if (!image->data) failf("cannot read %s", path);
    size_t first = 0;
    while (first < size && image->data[first] != '\n') first++;
    size_t second = first + 1;
    while (second < size && image->data[second] != '\n') second++;
    size_t third = second + 1;
    while (third < size && image->data[third] != '\n') third++;
    if (first >= size || second >= size || third >= size)
        failf("cannot read %s", path);
    char line[256];
    size_t length = second - first - 1;
    if (length >= sizeof line) length = sizeof line - 1;
    memcpy(line, image->data + first + 1, length);
    line[length] = '\0';
    char *stop = NULL;
    long width = strtol(line, &stop, 10);
    long height = stop ? strtol(stop, NULL, 10) : 0;
    if (width <= 0 || height <= 0) failf("cannot read %s", path);
    image->width = (int)width;
    image->height = (int)height;
    image->pixels = image->data + third + 1;
    image->pixel_bytes = size - (third + 1);
}

static void image_free(struct image *image) {
    free(image->data);
    image->data = NULL;
    image->pixels = NULL;
}

static void image_lit(const char *path, int step, struct point **points_out,
        long long *count_out) {
    struct image image;
    image_load(path, &image);
    size_t span = (size_t)image.width * 3;
    struct point *points = NULL;
    long long count = 0;
    size_t capacity = 0;
    for (int y = 0; y < image.height; y += step) {
        size_t row_start = (size_t)y * span;
        if (row_start + span > image.pixel_bytes) break;
        unsigned char maximum = 0;
        for (size_t at = row_start; at < row_start + span; at++)
            if (image.pixels[at] > maximum) maximum = image.pixels[at];
        if (maximum <= 20) continue;
        for (int x = 0; x < image.width; x += step) {
            size_t at = row_start + (size_t)x * 3;
            if (image.pixels[at] > 20 || image.pixels[at + 1] > 20 || image.pixels[at + 2] > 20) {
                if (points_out) {
                    if ((size_t)count == capacity) {
                        capacity = capacity ? capacity * 2 : 1024;
                        points = xrealloc(points, capacity * sizeof(struct point));
                    }
                    points[count].x = x;
                    points[count].y = y;
                }
                count++;
            }
        }
    }
    image_free(&image);
    *count_out = count;
    if (points_out) *points_out = points;
}

static long long image_lit_count(const char *path) {
    long long count = 0;
    image_lit(path, 2, NULL, &count);
    return count;
}

static long long marker_pixels(const char *path) {
    struct image image;
    image_load(path, &image);
    size_t span = (size_t)image.width * 3;
    long long count = 0;
    for (int y = 0; y < image.height; y++) {
        size_t row_start = (size_t)y * span;
        if (row_start + span > image.pixel_bytes) break;
        for (int x = 0; x < image.width; x++) {
            size_t at = row_start + (size_t)x * 3;
            if (image.pixels[at] == 0xff && image.pixels[at + 1] == 0x00 &&
                    image.pixels[at + 2] == 0xff) count++;
        }
    }
    image_free(&image);
    return count;
}

static void png_chunk(FILE *file, const char *kind, const unsigned char *data, size_t length) {
    unsigned char header[4] = {
        (unsigned char)(length >> 24), (unsigned char)(length >> 16),
        (unsigned char)(length >> 8), (unsigned char)length
    };
    fwrite(header, 1, 4, file);
    fwrite(kind, 1, 4, file);
    if (length) fwrite(data, 1, length, file);
    uLong crc = crc32(0L, Z_NULL, 0);
    crc = crc32(crc, (const Bytef *)kind, 4);
    if (length) crc = crc32(crc, data, (uInt)length);
    unsigned char footer[4] = {
        (unsigned char)(crc >> 24), (unsigned char)(crc >> 16),
        (unsigned char)(crc >> 8), (unsigned char)crc
    };
    fwrite(footer, 1, 4, file);
}

static void write_png(const char *source, const char *target) {
    struct image image;
    image_load(source, &image);
    size_t stride = (size_t)image.width * 3;
    size_t raw_size = (size_t)image.height * (stride + 1);
    unsigned char *raw = xmalloc(raw_size);
    for (int y = 0; y < image.height; y++) {
        unsigned char *row = raw + (size_t)y * (stride + 1);
        row[0] = 0;
        memcpy(row + 1, image.pixels + (size_t)y * stride, stride);
    }
    uLongf packed_size = compressBound((uLong)raw_size);
    unsigned char *packed = xmalloc(packed_size);
    if (compress2(packed, &packed_size, raw, (uLong)raw_size, 9) != Z_OK)
        failf("cannot compress %s", source);
    FILE *file = fopen(target, "wb");
    if (!file) failf("cannot write %s", target);
    static const unsigned char signature[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n' };
    fwrite(signature, 1, 8, file);
    unsigned char header[13] = {
        (unsigned char)((unsigned)image.width >> 24),
        (unsigned char)((unsigned)image.width >> 16),
        (unsigned char)((unsigned)image.width >> 8),
        (unsigned char)image.width,
        (unsigned char)((unsigned)image.height >> 24),
        (unsigned char)((unsigned)image.height >> 16),
        (unsigned char)((unsigned)image.height >> 8),
        (unsigned char)image.height,
        8, 2, 0, 0, 0
    };
    png_chunk(file, "IHDR", header, sizeof header);
    png_chunk(file, "IDAT", packed, (size_t)packed_size);
    png_chunk(file, "IEND", NULL, 0);
    if (fclose(file) != 0) failf("cannot write %s", target);
    free(packed);
    free(raw);
    image_free(&image);
}

struct frame_field {
    char key[FIELD_MAX];
    char value[128];
};

struct frame_entry {
    char name[128];
    int field_count;
    struct frame_field fields[32];
    int frame_time_count;
    long long frame_times[SERIES_MAX];
    int frame_us_count;
    long long frame_us[SERIES_MAX];
};

struct frame_table {
    int count;
    struct frame_entry entries[FRAME_MAX];
};

static struct frame_table *frame_table_new(void) {
    struct frame_table *table = xmalloc(sizeof(struct frame_table));
    memset(table, 0, sizeof(struct frame_table));
    return table;
}

static void frame_table_free(struct frame_table *table) {
    free(table);
}

static int word_matches(const char *at, const char *word) {
    for (size_t index = 0; word[index]; index++)
        if (tolower((unsigned char)at[index]) != word[index]) return 0;
    return 1;
}

static struct frame_entry *frame_entry_for(struct frame_table *table, const char *name,
        size_t length) {
    for (int index = 0; index < table->count; index++)
        if (strlen(table->entries[index].name) == length &&
                strncmp(table->entries[index].name, name, length) == 0)
            return &table->entries[index];
    if (table->count >= FRAME_MAX) return NULL;
    struct frame_entry *entry = &table->entries[table->count++];
    memset(entry, 0, sizeof(struct frame_entry));
    memcpy(entry->name, name, length);
    entry->name[length] = '\0';
    return entry;
}

static void frame_set_field(struct frame_entry *entry, const char *key, size_t key_length,
        const char *value, size_t value_length) {
    char lowered[FIELD_MAX];
    size_t length = key_length < FIELD_MAX - 1 ? key_length : FIELD_MAX - 1;
    for (size_t index = 0; index < length; index++)
        lowered[index] = (char)tolower((unsigned char)key[index]);
    lowered[length] = '\0';
    for (int index = 0; index < entry->field_count; index++) {
        if (strcmp(entry->fields[index].key, lowered) == 0) {
            size_t copy = value_length < sizeof entry->fields[index].value - 1 ?
                value_length : sizeof entry->fields[index].value - 1;
            memcpy(entry->fields[index].value, value, copy);
            entry->fields[index].value[copy] = '\0';
            return;
        }
    }
    if (entry->field_count >= 32) return;
    struct frame_field *field = &entry->fields[entry->field_count++];
    snprintf(field->key, sizeof field->key, "%s", lowered);
    size_t copy = value_length < sizeof field->value - 1 ? value_length : sizeof field->value - 1;
    memcpy(field->value, value, copy);
    field->value[copy] = '\0';
}

static int frame_series_parse(const char *text, size_t begin, size_t end, long long *values,
        int *count, int maximum) {
    size_t at = begin;
    if (at >= end || text[at] != '(') return 0;
    at++;
    while (at < end && text[at] != ')') {
        while (at < end && isspace((unsigned char)text[at])) at++;
        if (at >= end || text[at] == ')') break;
        char *stop = NULL;
        long long value = strtoll(text + at, &stop, 10);
        if (stop == text + at) break;
        if (*count < maximum) values[(*count)++] = value;
        at = (size_t)(stop - text);
    }
    return 1;
}

static void frame_parse_fields(const char *text, size_t begin, size_t end,
        struct frame_entry *entry) {
    size_t at = begin;
    while (at < end) {
        if (text[at] != ':') {
            at++;
            continue;
        }
        size_t key_start = at + 1;
        size_t key_end = key_start;
        while (key_end < end && (isalpha((unsigned char)text[key_end]) || text[key_end] == '-'))
            key_end++;
        if (key_end == key_start) {
            at++;
            continue;
        }
        size_t value_at = key_end;
        while (value_at < end && isspace((unsigned char)text[value_at])) value_at++;
        if (value_at < end && text[value_at] == '(') {
            char key[FIELD_MAX];
            size_t key_length = key_end - key_start;
            size_t copy = key_length < sizeof key - 1 ? key_length : sizeof key - 1;
            for (size_t index = 0; index < copy; index++)
                key[index] = (char)tolower((unsigned char)text[key_start + index]);
            key[copy] = '\0';
            if (strcmp(key, "frame-times") == 0)
                frame_series_parse(text, value_at, end, entry->frame_times,
                        &entry->frame_time_count, SERIES_MAX);
            else if (strcmp(key, "frame-us") == 0)
                frame_series_parse(text, value_at, end, entry->frame_us,
                        &entry->frame_us_count, SERIES_MAX);
            at = value_at + 1;
            continue;
        }
        if (value_at < end && (isalnum((unsigned char)text[value_at]) ||
                    text[value_at] == '_' || text[value_at] == '-')) {
            size_t value_end = value_at;
            while (value_end < end && (isalnum((unsigned char)text[value_end]) ||
                        text[value_end] == '_' || text[value_end] == '-')) value_end++;
            frame_set_field(entry, text + key_start, key_end - key_start,
                    text + value_at, value_end - value_at);
            at = value_end;
            continue;
        }
        at = key_end;
    }
}

static void frames_parse(const char *reply, struct frame_table *table) {
    const char *at = reply;
    while ((at = strchr(at, ':')) != NULL) {
        if (!word_matches(at + 1, "name")) {
            at++;
            continue;
        }
        const char *scan = at + 5;
        while (*scan == ' ' || *scan == '\t') scan++;
        if (*scan != '"') {
            at++;
            continue;
        }
        const char *name = ++scan;
        while (*scan && *scan != '"') scan++;
        if (*scan != '"') break;
        size_t name_length = (size_t)(scan - name);
        const char *fields = scan + 1;
        const char *line_end = fields;
        while (*line_end && *line_end != '\n') line_end++;
        struct frame_entry *entry = name_length ?
            frame_entry_for(table, name, name_length) : NULL;
        if (entry) {
            entry->field_count = 0;
            entry->frame_time_count = 0;
            entry->frame_us_count = 0;
            frame_parse_fields(fields, 0, (size_t)(line_end - fields), entry);
        }
        at = line_end;
    }
}

static const char *frame_field(const struct frame_entry *entry, const char *key) {
    for (int index = 0; index < entry->field_count; index++)
        if (strcmp(entry->fields[index].key, key) == 0) return entry->fields[index].value;
    return NULL;
}

static long long frame_number(const struct frame_entry *entry, const char *key) {
    const char *value = frame_field(entry, key);
    if (!value) return 0;
    return strtoll(value, NULL, 10);
}

static const struct frame_entry *frame_table_find(const struct frame_table *table,
        const char *name) {
    for (int index = 0; index < table->count; index++)
        if (strcmp(table->entries[index].name, name) == 0) return &table->entries[index];
    return NULL;
}

static long long frame_delta(const struct frame_table *before,
        const struct frame_entry *after) {
    const struct frame_entry *old = frame_table_find(before, after->name);
    if (!old) return 0;
    return frame_number(after, "frame") - frame_number(old, "frame");
}

static long long tables_drawn(const struct frame_table *before,
        const struct frame_table *after) {
    long long drawn = 0;
    for (int index = 0; index < after->count; index++)
        drawn += frame_delta(before, &after->entries[index]);
    return drawn;
}

struct animation_field {
    char key[FIELD_MAX];
    long long value;
};

struct animation {
    int field_count;
    struct animation_field fields[ANIM_FIELD_MAX];
    int trail_count;
    long long trail_times[ANIM_TIME_MAX];
};

static void animation_set(struct animation *animation, const char *key, size_t key_length,
        long long value) {
    char lowered[FIELD_MAX];
    size_t length = key_length < FIELD_MAX - 1 ? key_length : FIELD_MAX - 1;
    for (size_t index = 0; index < length; index++)
        lowered[index] = (char)tolower((unsigned char)key[index]);
    lowered[length] = '\0';
    for (int index = 0; index < animation->field_count; index++)
        if (strcmp(animation->fields[index].key, lowered) == 0) {
            animation->fields[index].value = value;
            return;
        }
    if (animation->field_count >= ANIM_FIELD_MAX) return;
    snprintf(animation->fields[animation->field_count].key, FIELD_MAX, "%s", lowered);
    animation->fields[animation->field_count].value = value;
    animation->field_count++;
}

static int animation_has(const struct animation *animation, const char *key) {
    for (int index = 0; index < animation->field_count; index++)
        if (strcmp(animation->fields[index].key, key) == 0) return 1;
    return 0;
}

static long long animation_number(const struct animation *animation, const char *key) {
    for (int index = 0; index < animation->field_count; index++)
        if (strcmp(animation->fields[index].key, key) == 0)
            return animation->fields[index].value;
    return 0;
}

static void animation_parse(const char *reply, struct animation *animation) {
    memset(animation, 0, sizeof(struct animation));
    for (size_t at = 0; reply[at];) {
        if (reply[at] != ':') {
            at++;
            continue;
        }
        size_t key_start = at + 1;
        size_t key_end = key_start;
        while (isalpha((unsigned char)reply[key_end]) || reply[key_end] == '-') key_end++;
        if (key_end == key_start) {
            at++;
            continue;
        }
        size_t value_at = key_end;
        while (isspace((unsigned char)reply[value_at])) value_at++;
        size_t digits = value_at;
        if (reply[digits] == '-') digits++;
        size_t digits_end = digits;
        while (isdigit((unsigned char)reply[digits_end])) digits_end++;
        if (digits_end > digits) {
            animation_set(animation, reply + key_start, key_end - key_start,
                    strtoll(reply + value_at, NULL, 10));
            at = digits_end;
            continue;
        }
        char key[FIELD_MAX];
        size_t key_length = key_end - key_start;
        size_t copy = key_length < sizeof key - 1 ? key_length : sizeof key - 1;
        for (size_t index = 0; index < copy; index++)
            key[index] = (char)tolower((unsigned char)reply[key_start + index]);
        key[copy] = '\0';
        if (strcmp(key, "trail-times") == 0 && reply[value_at] == '(') {
            int count = 0;
            frame_series_parse(reply, value_at, strlen(reply), animation->trail_times,
                    &count, ANIM_TIME_MAX);
            animation->trail_count = count;
        }
        at = key_end;
    }
}

struct steps {
    long long steps;
    long long span_ns;
    double per_refresh;
    double hz;
    long long median_gap_ns;
    long long max_gap_ns;
};

static struct steps steps_of(const long long *times, int count, long long period) {
    struct steps result;
    memset(&result, 0, sizeof result);
    result.steps = count;
    long long span = count > 1 ? times[count - 1] - times[0] : 0;
    result.span_ns = span;
    double refreshes = period ? (double)span / (double)period : 0.0;
    result.per_refresh = refreshes ? round_digits((double)count / refreshes, 3) : 0.0;
    result.hz = span ? round_digits((double)count / ((double)span / 1e9), 1) : 0.0;
    if (count > 1) {
        long long *gaps = xmalloc((size_t)(count - 1) * sizeof(long long));
        for (int index = 0; index < count - 1; index++)
            gaps[index] = times[index + 1] - times[index];
        for (int index = 1; index < count - 1; index++) {
            long long value = gaps[index];
            int place = index;
            while (place > 0 && gaps[place - 1] > value) {
                gaps[place] = gaps[place - 1];
                place--;
            }
            gaps[place] = value;
        }
        result.median_gap_ns = gaps[(count - 1) / 2];
        result.max_gap_ns = gaps[count - 2];
        free(gaps);
    }
    return result;
}

struct compositor {
    char work[PATH_SIZE];
    char runtime[PATH_SIZE];
    char log_path[PATH_SIZE];
    pid_t pid;
    int status;
    int started;
    struct child_env env;
};

static void compositor_policy(struct compositor *compositor, int trail, int marker, int fps,
        double idle, double cell, double life, int steps, int hz) {
    char policy_path[PATH_SIZE];
    harness_join(policy_path, sizeof policy_path, compositor->work, "policy.lisp");
    char shader_path[PATH_SIZE];
    harness_join(shader_path, sizeof shader_path, compositor->work, "marker.glsl");
    harness_write_file(shader_path, MARKER_SHADER);
    if (!trail && !marker) {
        harness_write_file(policy_path, "(in-package #:tomoe-user)\n");
        return;
    }
    char cell_text[64], life_text[64], idle_text[64];
    float_text(cell, cell_text, sizeof cell_text);
    float_text(life, life_text, sizeof life_text);
    float_text(idle, idle_text, sizeof idle_text);
    struct text policy;
    text_init(&policy);
    text_add(&policy, POLICY_HEAD, hz);
    if (trail) {
        char trail_text[PATH_SIZE * 2];
        snprintf(trail_text, sizeof trail_text, TRAIL_TEMPLATE,
                marker ? ":top" : ":overlay", cell_text, life_text, steps, fps, idle_text);
        text_add(&policy, "%s", trail_text);
    }
    text_add(&policy, "\n                   ");
    if (marker) {
        char marker_text[PATH_SIZE * 2];
        snprintf(marker_text, sizeof marker_text, MARKER_TEMPLATE, shader_path);
        text_add(&policy, "%s", marker_text);
    }
    text_add(&policy, "%s", POLICY_TAIL);
    harness_write_file(policy_path, policy.data);
    free(policy.data);
}

static int compositor_poll(struct compositor *compositor) {
    if (!compositor->started) return -1;
    if (compositor->status >= 0) return compositor->status;
    int status = 0;
    pid_t done = waitpid(compositor->pid, &status, WNOHANG);
    if (done != compositor->pid) return -1;
    if (WIFEXITED(status)) compositor->status = WEXITSTATUS(status);
    else compositor->status = 128 + (WIFSIGNALED(status) ? WTERMSIG(status) : 0);
    return compositor->status;
}

static char *compositor_text(struct compositor *compositor) {
    size_t size = 0;
    unsigned char *data = read_all(compositor->log_path, &size);
    if (!data) return xstrdup("");
    char *text = (char *)data;
    text = xrealloc(text, size + 1);
    text[size] = '\0';
    return text;
}

static char *compositor_call(struct compositor *compositor, const char *command, int *status) {
    char *arguments[6] = { (char *)tomoe_binary, "--socket", "trail", (char *)command, NULL };
    struct capture run = capture_run(arguments, compositor->env.items);
    size_t length = strlen(run.out) + strlen(run.err);
    char *combined = xmalloc(length + 1);
    sprintf(combined, "%s%s", run.out, run.err);
    free(run.out);
    free(run.err);
    *status = run.status;
    return combined;
}

static void compositor_start(struct compositor *compositor, const char *work, int trail,
        int fps, double idle, double cell, double life, int steps, int hz, int marker) {
    memset(compositor, 0, sizeof(struct compositor));
    compositor->status = -1;
    snprintf(compositor->work, sizeof compositor->work, "%s", work);
    harness_join(compositor->runtime, sizeof compositor->runtime, work, "run");
    harness_join(compositor->log_path, sizeof compositor->log_path, work, "tomoe.log");
    harness_mkparents(compositor->runtime, 0700);
    chmod(compositor->runtime, 0700);
    compositor_policy(compositor, trail, marker, fps, idle, cell, life, steps, hz);
    child_env_init(&compositor->env);
    child_env_set(&compositor->env, "XDG_RUNTIME_DIR", compositor->runtime);
    child_env_set(&compositor->env, "WAYLAND_DISPLAY", "trail");
    child_env_set(&compositor->env, "TOMOE_DEBUG", "1");
    char policy_path[PATH_SIZE];
    harness_join(policy_path, sizeof policy_path, work, "policy.lisp");
    char *arguments[12] = {
        (char *)tomoe_binary, "--socket", "trail", "--backend", "headless", "--bare",
        "--no-watch", "--test-timings", "--config", policy_path, NULL
    };
    compositor->pid = harness_spawn(arguments, compositor->env.items, NULL, NULL,
            compositor->log_path, 0);
    compositor->started = 1;
    double deadline = harness_now() + 60.0;
    while (harness_now() < deadline) {
        if (compositor_poll(compositor) >= 0) {
            char *text = compositor_text(compositor);
            char *message = xmalloc(strlen(text) + 64);
            sprintf(message, "tomoe exited while starting:\n%s", text);
            free(text);
            failf("%s", message);
        }
        int status = 0;
        char *reply = compositor_call(compositor, "inspect", &status);
        free(reply);
        if (status == 0) return;
        harness_sleep(0.1);
    }
    char *text = compositor_text(compositor);
    char *message = xmalloc(strlen(text) + 64);
    sprintf(message, "tomoe did not answer inspect:\n%s", text);
    free(text);
    failf("%s", message);
}

static void compositor_stop(struct compositor *compositor) {
    if (compositor_poll(compositor) < 0) {
        kill(compositor->pid, SIGTERM);
        if (harness_wait_status(compositor->pid, 20) < 0) {
            kill(compositor->pid, SIGKILL);
            harness_wait_status(compositor->pid, 10);
        }
        compositor->status = 0;
    }
    if (compositor->started) child_env_free(&compositor->env);
    compositor->started = 0;
}

static void compositor_frames(struct compositor *compositor, struct frame_table *table) {
    int status = 0;
    char *reply = compositor_call(compositor, "frames", &status);
    memset(table, 0, sizeof(struct frame_table));
    frames_parse(reply, table);
    if (table->count == 0) failf("unreadable frames reply: %.400s", reply);
    free(reply);
}

static void compositor_animation(struct compositor *compositor, struct animation *animation) {
    int status = 0;
    char *reply = compositor_call(compositor, "animation", &status);
    animation_parse(reply, animation);
    if (!animation_has(animation, "frames") || !animation_has(animation, "period-ns"))
        failf("unreadable animation reply: %.400s", reply);
    free(reply);
}

static long long compositor_wakeups(struct compositor *compositor) {
    char path[256];
    snprintf(path, sizeof path, "/proc/%d/status", (int)compositor->pid);
    size_t size = 0;
    unsigned char *data = read_all(path, &size);
    if (!data) return 0;
    char *text = (char *)data;
    long long total = 0;
    char *at = text;
    while ((at = strstr(at, "ctxt_switches:")) != NULL) {
        at += strlen("ctxt_switches:");
        while (*at == ' ' || *at == '\t') at++;
        total += strtoll(at, NULL, 10);
    }
    free(data);
    return total;
}

static int screenshot_raw(struct compositor *compositor, const char *path, char **error) {
    char *arguments[5] = { "grim", "-t", "ppm", (char *)path, NULL };
    struct capture run = capture_run(arguments, compositor->env.items);
    free(run.out);
    if (error) *error = run.err;
    else free(run.err);
    return run.status;
}

static void compositor_screenshot(struct compositor *compositor, const char *path) {
    char *error = NULL;
    int status = screenshot_raw(compositor, path, &error);
    if (status != 0) {
        if (error) trim_text(error);
        failf("grim failed: %s", error ? error : "");
    }
    free(error);
}

static void encode_path(char *out, size_t size, const struct point *path, int count) {
    size_t used = 0;
    out[0] = '\0';
    for (int index = 0; index < count; index++) {
        int written = snprintf(out + used, size - used, "%s%lld,%lld",
                index ? ";" : "", path[index].x, path[index].y);
        if (written < 0 || (size_t)written >= size - used) failf("a path is too long");
        used += (size_t)written;
    }
}

static void pointer_arguments(char **arguments, const char *encoded, int steps, int interval,
        int hold, int burst) {
    static char steps_text[32], interval_text[32], hold_text[32];
    snprintf(steps_text, sizeof steps_text, "%d", steps);
    snprintf(interval_text, sizeof interval_text, "%d", interval);
    snprintf(hold_text, sizeof hold_text, "%d", hold);
    int at = 0;
    arguments[at++] = (char *)pointer_binary;
    arguments[at++] = "--path";
    arguments[at++] = (char *)encoded;
    arguments[at++] = "--steps";
    arguments[at++] = steps_text;
    arguments[at++] = "--interval-ms";
    arguments[at++] = interval_text;
    arguments[at++] = "--hold-ms";
    arguments[at++] = hold_text;
    arguments[at++] = "--delay-ms";
    arguments[at++] = "50";
    arguments[at++] = "--extent";
    arguments[at++] = "1280x720";
    if (burst) arguments[at++] = "--burst";
    arguments[at] = NULL;
}

static void drag(struct compositor *compositor, const struct point *path, int count, int steps,
        int interval, int burst, int hold) {
    char encoded[32768];
    encode_path(encoded, sizeof encoded, path, count);
    char *arguments[16];
    pointer_arguments(arguments, encoded, steps, interval, hold, burst);
    struct capture run = capture_run(arguments, compositor->env.items);
    if (run.status != 0) {
        trim_text(run.err);
        failf("pointer-path failed: %s", run.err);
    }
    free(run.out);
    free(run.err);
}

struct async_run {
    pid_t pid;
    int out_fd;
    int err_fd;
    int started;
};

static int line_read(int fd, char *line, size_t size) {
    size_t used = 0;
    int saw = 0;
    for (;;) {
        char byte;
        ssize_t got = read(fd, &byte, 1);
        if (got <= 0) break;
        saw = 1;
        if (byte == '\n') break;
        if (used + 1 < size) line[used++] = byte;
    }
    line[used] = '\0';
    trim_text(line);
    return saw;
}

static struct async_run drag_async(struct compositor *compositor, const struct point *path,
        int count, int steps, int interval, int burst) {
    struct async_run result = { -1, -1, -1, 0 };
    char encoded[32768];
    encode_path(encoded, sizeof encoded, path, count);
    char *arguments[16];
    pointer_arguments(arguments, encoded, steps, interval, 0, burst);
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
        execvpe(arguments[0], arguments, compositor->env.items);
        fprintf(stderr, "harness: cannot run %s: %s\n", arguments[0], strerror(errno));
        _exit(127);
    }
    close(out_pipe[1]);
    close(err_pipe[1]);
    result.pid = child;
    result.out_fd = out_pipe[0];
    result.err_fd = err_pipe[0];
    result.started = 1;
    int released = 0;
    char line[4096];
    while (line_read(result.out_fd, line, sizeof line))
        if (strcmp(line, "released") == 0) {
            released = 1;
            break;
        }
    if (!released) {
        harness_wait_status(result.pid, 30);
        char *error = read_descriptor(result.err_fd);
        trim_text(error);
        char *message = xmalloc(strlen(error) + 64);
        sprintf(message, "pointer-path failed: %s", *error ? error : "no release");
        free(error);
        harness_fail(message);
    }
    return result;
}

static void async_wait(struct async_run *run) {
    if (!run->started) return;
    if (harness_wait_status(run->pid, 30) < 0) {
        kill(run->pid, SIGKILL);
        harness_wait_status(run->pid, 10);
        failf("pointer-path failed: no exit");
    }
    close(run->out_fd);
    close(run->err_fd);
    run->started = 0;
}

static void park(struct compositor *compositor, const struct point *path) {
    struct point stroke[2] = { path[0], { path[0].x + 1, path[0].y } };
    drag(compositor, stroke, 2, 1, 16, 0, 100);
    harness_sleep(1.2);
}

#define RECORDER_MAX 8192

struct recorder_shared {
    volatile sig_atomic_t stop;
    int count;
    int best;
    int best_lit;
    int lit[RECORDER_MAX];
};

struct recorder {
    struct compositor *compositor;
    const char *tag;
    int keep;
    struct recorder_shared *shared;
    pid_t pid;
    int started;
};

static void frame_path(char *out, size_t size, const char *work, const char *tag, int index) {
    char suffix[64];
    int written = snprintf(suffix, sizeof suffix, "-%03d.ppm", index);
    size_t needed = strlen(work) + 1 + strlen(tag) + (size_t)written + 1;
    if (needed > size) harness_fail("a path is too long");
    sprintf(out, "%s/%s%s", work, tag, suffix);
}

static void recorder_child(struct recorder *recorder) {
    int index = 0;
    char frame[PATH_SIZE];
    char best_path[PATH_SIZE];
    best_path[0] = '\0';
    while (!recorder->shared->stop) {
        frame_path(frame, sizeof frame, recorder->compositor->work, recorder->tag, index);
        if (screenshot_raw(recorder->compositor, frame, NULL) != 0) break;
        long long count = image_lit_count(frame);
        if (recorder->shared->count < RECORDER_MAX)
            recorder->shared->lit[recorder->shared->count++] = (int)count;
        if (recorder->keep) {
            if (recorder->shared->best < 0 || count > recorder->shared->best_lit) {
                if (recorder->shared->best >= 0) unlink(best_path);
                recorder->shared->best = index;
                recorder->shared->best_lit = (int)count;
                snprintf(best_path, sizeof best_path, "%s", frame);
            } else {
                unlink(frame);
            }
        } else {
            unlink(frame);
        }
        index++;
        harness_sleep(0.01);
    }
}

static void recorder_start(struct recorder *recorder, struct compositor *compositor,
        const char *tag, int keep) {
    recorder->compositor = compositor;
    recorder->tag = tag;
    recorder->keep = keep;
    recorder->shared = mmap(NULL, sizeof(struct recorder_shared), PROT_READ | PROT_WRITE,
            MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (recorder->shared == MAP_FAILED) harness_fail("cannot share recorder state");
    memset((void *)recorder->shared, 0, sizeof(struct recorder_shared));
    recorder->shared->best = -1;
    pid_t child = fork();
    if (child < 0) harness_fail("cannot fork");
    if (child == 0) {
        active_flow = 0;
        recorder_child(recorder);
        _exit(0);
    }
    recorder->pid = child;
    recorder->started = 1;
}

static void recorder_stop(struct recorder *recorder, int **lit, int *lit_count,
        char *best_frame, size_t best_size, struct point **points, long long *point_count) {
    if (!recorder->started) return;
    recorder->shared->stop = 1;
    if (harness_wait_status(recorder->pid, 30) < 0) {
        kill(recorder->pid, SIGKILL);
        harness_wait_status(recorder->pid, 10);
    }
    int count = recorder->shared->count;
    if (lit) {
        int *values = xmalloc((size_t)(count ? count : 1) * sizeof(int));
        for (int index = 0; index < count; index++) values[index] = recorder->shared->lit[index];
        *lit = values;
    }
    if (lit_count) *lit_count = count;
    if (best_frame) best_frame[0] = '\0';
    if (points) *points = NULL;
    if (point_count) *point_count = 0;
    if (best_frame && recorder->keep && recorder->shared->best >= 0) {
        frame_path(best_frame, best_size, recorder->compositor->work, recorder->tag,
                recorder->shared->best);
        if (points) image_lit(best_frame, 2, points, point_count);
    }
    munmap((void *)recorder->shared, sizeof(struct recorder_shared));
    recorder->started = 0;
}

static double segment_distance(double px, double py, double ax, double ay, double bx, double by) {
    double dx = bx - ax, dy = by - ay;
    double length = dx * dx + dy * dy;
    double along = 0.0;
    if (length != 0.0) along = clamp_unit(((px - ax) * dx + (py - ay) * dy) / length);
    double qx = px - (ax + dx * along);
    double qy = py - (ay + dy * along);
    return square_root(qx * qx + qy * qy);
}

static double path_distance(const struct point *point, const struct point *path, int count) {
    double best = DBL_MAX;
    for (int index = 0; index + 1 < count; index++) {
        double distance = segment_distance((double)point->x, (double)point->y,
                (double)path[index].x, (double)path[index].y,
                (double)path[index + 1].x, (double)path[index + 1].y);
        if (distance < best) best = distance;
    }
    return best;
}

struct stats {
    long long lit;
    double path_mean;
    double path_max;
    double chord_mean;
    long long off_chord;
};

static struct stats measure(const struct point *points, long long count,
        const struct point *path, int path_count) {
    struct stats result;
    memset(&result, 0, sizeof result);
    result.lit = count;
    if (count <= 0) failf("division by zero");
    double path_total = 0.0, chord_total = 0.0, path_maximum = 0.0;
    long long off = 0;
    for (long long index = 0; index < count; index++) {
        double on = path_distance(&points[index], path, path_count);
        double off_chord = segment_distance((double)points[index].x, (double)points[index].y,
                (double)path[0].x, (double)path[0].y,
                (double)path[path_count - 1].x, (double)path[path_count - 1].y);
        path_total += on;
        if (on > path_maximum) path_maximum = on;
        chord_total += off_chord;
        if (off_chord > 20.0) off++;
    }
    result.path_mean = round_digits(path_total / (double)count, 2);
    result.path_max = round_digits(path_maximum, 2);
    result.chord_mean = round_digits(chord_total / (double)count, 2);
    result.off_chord = off;
    return result;
}

struct trace_result {
    int have_frame;
    char frame[PATH_SIZE];
    struct stats stats;
    long long shots;
};

static void trace(struct compositor *compositor, const struct point *path, int path_count,
        int steps, int interval, int burst, const char *tag, struct trace_result *result) {
    memset(result, 0, sizeof(struct trace_result));
    struct recorder recorder;
    recorder_start(&recorder, compositor, tag, 1);
    harness_sleep(0.3);
    struct frame_table *before = frame_table_new();
    compositor_frames(compositor, before);
    frame_table_free(before);
    drag(compositor, path, path_count, steps, interval, burst, 400);
    struct frame_table *after = frame_table_new();
    compositor_frames(compositor, after);
    frame_table_free(after);
    harness_sleep(0.2);
    struct point *points = NULL;
    long long point_count = 0;
    int lit_count = 0;
    recorder_stop(&recorder, NULL, &lit_count, result->frame, sizeof result->frame,
            &points, &point_count);
    result->shots = lit_count;
    if (result->frame[0]) {
        result->have_frame = 1;
        result->stats = measure(points, point_count, path, path_count);
    }
    free(points);
}

struct paced_entry {
    int hz;
    int fps;
    long long refresh_mhz;
    long long period_ns;
    struct steps moving;
    struct steps resting;
    double resting_seconds;
    double resting_hz;
    long long lead_min_ns;
};

static void paced(const char *work, const struct point *path, int path_count, int hz, int fps,
        struct paced_entry *entry) {
    char tag[64];
    if (fps) snprintf(tag, sizeof tag, "refresh-%d-cap-%d", hz, fps);
    else snprintf(tag, sizeof tag, "refresh-%d", hz);
    char directory[PATH_SIZE];
    harness_join(directory, sizeof directory, work, tag);
    harness_mkparents(directory, 0777);
    struct compositor compositor;
    compositor_start(&compositor, directory, 1, fps, 0.5, 9.0, 0.35, 4, hz, 0);
    struct animation animation;
    harness_sleep(1.0);
    compositor_animation(&compositor, &animation);
    struct async_run run = drag_async(&compositor, path, path_count, 2, 6, 0);
    struct animation moving;
    compositor_animation(&compositor, &moving);
    double started = harness_now();
    harness_sleep(0.3);
    struct animation resting;
    compositor_animation(&compositor, &resting);
    double elapsed = harness_now() - started;
    async_wait(&run);
    compositor_stop(&compositor);
    long long period = animation_number(&resting, "period-ns");
    memset(entry, 0, sizeof(struct paced_entry));
    entry->hz = hz;
    entry->fps = fps;
    entry->refresh_mhz = animation_number(&resting, "refresh-mhz");
    entry->period_ns = period;
    entry->moving = steps_of(moving.trail_times, moving.trail_count, period);
    entry->resting = steps_of(resting.trail_times, resting.trail_count, period);
    entry->resting_seconds = round_digits(elapsed, 3);
    entry->resting_hz = elapsed ? round_digits((double)resting.trail_count / elapsed, 1) : 0.0;
    entry->lead_min_ns = animation_number(&resting, "lead-min-ns");
}

static struct json_node *steps_json(const struct steps *steps) {
    struct json_node *object = json_object();
    json_add(object, "steps", node_int(steps->steps));
    json_add(object, "span-ns", node_int(steps->span_ns));
    json_add(object, "steps-per-refresh", node_real(steps->per_refresh));
    json_add(object, "step-hz", node_real(steps->hz));
    json_add(object, "median-gap-ns", node_int(steps->median_gap_ns));
    json_add(object, "max-gap-ns", node_int(steps->max_gap_ns));
    return object;
}

static struct json_node *paced_json(const struct paced_entry *entry) {
    struct json_node *object = json_object();
    json_add(object, "hz", node_int(entry->hz));
    json_add(object, "fps", node_int(entry->fps));
    json_add(object, "refresh-mhz", node_int(entry->refresh_mhz));
    json_add(object, "period-ns", node_int(entry->period_ns));
    json_add(object, "moving", steps_json(&entry->moving));
    json_add(object, "resting", steps_json(&entry->resting));
    json_add(object, "resting-seconds", node_real(entry->resting_seconds));
    json_add(object, "resting-hz", node_real(entry->resting_hz));
    json_add(object, "lead-min-ns", node_int(entry->lead_min_ns));
    return object;
}

static void judge_pace(const struct paced_entry *entry, struct failure_list *failures) {
    long long period = entry->period_ns;
    int hz = entry->hz;
    int fps = entry->fps;
    char valuable[64];
    if (entry->refresh_mhz != (long long)hz * 1000)
        failure_add(failures, "refresh %d: the output runs at %.0f Hz",
                hz, (double)entry->refresh_mhz / 1000.0);
    if (!period) {
        failure_add(failures, "refresh %d: the compositor reported no refresh period", hz);
        return;
    }
    const struct steps *moving = &entry->moving;
    const struct steps *resting = &entry->resting;
    if (!fps && !moving->steps) {
        failure_add(failures, "refresh %d: the trail drew no steps while the pointer moved", hz);
    } else if (!fps && moving->per_refresh < 0.9) {
        failure_add(failures, "refresh %d: the trail took %lld steps over %.1f refreshes while "
                "the pointer moved, so some refresh showed a stale trail",
                hz, moving->steps, (double)moving->span_ns / (double)period);
    }
    if (!resting->steps) {
        failure_add(failures, "refresh %d: the trail stopped stepping while the pointer rested "
                "inside its :idle window", hz);
        return;
    }
    if (fps) {
        if (entry->resting_hz > (double)CAP_FPS * CAP_GROWTH) {
            float_text(entry->resting_hz, valuable, sizeof valuable);
            failure_add(failures, "cap: a :fps %d trail stepped %s times a second on a %d Hz "
                    "output instead of capping at %d", fps, valuable, hz, CAP_FPS);
        }
        if (resting->per_refresh >= 0.5) {
            float_text(resting->per_refresh, valuable, sizeof valuable);
            failure_add(failures, "cap: a :fps %d trail still advanced on %s of every refresh",
                    fps, valuable);
        }
        return;
    }
    if (resting->per_refresh < 0.9)
        failure_add(failures, "refresh %d: resting inside its :idle window, the trail took %lld "
                "steps over %.1f refreshes, so some refresh showed a stale trail",
                hz, resting->steps, (double)resting->span_ns / (double)period);
    if ((double)resting->median_gap_ns > (double)period * STEP_GROWTH)
        failure_add(failures, "refresh %d: the trail stepped every %.2f ms at the median, more "
                "than one refresh (%.2f ms)", hz,
                (double)resting->median_gap_ns / 1e6, (double)period / 1e6);
    if (entry->lead_min_ns <= period / 2)
        failure_add(failures, "refresh %d: the trail was timed only %.2f ms ahead of the frame "
                "start, not at the moment the frame is shown",
                hz, (double)entry->lead_min_ns / 1e6);
    if (hz > 60 && entry->resting_hz <= 60.0 * STEP_GROWTH) {
        float_text(entry->resting_hz, valuable, sizeof valuable);
        failure_add(failures, "refresh %d: the trail stepped %s times a second while the pointer "
                "rested, still gated at 60 fps instead of following the display",
                hz, valuable);
    }
}

static void drag_frames(struct compositor *compositor, const struct point *path, int path_count,
        int steps, int interval, int burst, double *times, int *time_count, long long *area) {
    struct frame_table *before = frame_table_new();
    compositor_frames(compositor, before);
    drag(compositor, path, path_count, steps, interval, burst, 0);
    struct frame_table *after = frame_table_new();
    compositor_frames(compositor, after);
    *time_count = 0;
    for (int index = 0; index < after->count; index++) {
        const struct frame_entry *entry = &after->entries[index];
        for (int at = 0; at < entry->frame_us_count && *time_count < SAMPLE_MAX; at++)
            times[(*time_count)++] = (double)entry->frame_us[at] / 1000.0;
    }
    long long drawn = tables_drawn(before, after);
    long long pixels = 0;
    for (int index = 0; index < after->count; index++) {
        long long value = frame_number(&after->entries[index], "drawn-pixels");
        if (value > pixels) pixels = value;
    }
    *area = drawn ? pixels : 0;
    frame_table_free(before);
    frame_table_free(after);
}

static void drag_cost(const char *work, const struct point *stroke, int stroke_count,
        int trail, int verbose, double *frames, int *frame_count, long long *areas,
        int *area_count) {
    const char *tag = trail ? "cost-trail" : "cost-plain";
    char directory[PATH_SIZE];
    harness_join(directory, sizeof directory, work, tag);
    harness_mkparents(directory, 0777);
    struct compositor compositor;
    compositor_start(&compositor, directory, trail, 0, 0.5, 9.0, 0.35, 4, 60, 0);
    *frame_count = 0;
    *area_count = 0;
    harness_sleep(1.0);
    park(&compositor, stroke);
    for (int round = 0; round < DRAGS; round++) {
        double times[SAMPLE_MAX];
        int count = 0;
        long long area = 0;
        drag_frames(&compositor, stroke, stroke_count, 6, 5, 1, times, &count, &area);
        for (int index = 0; index < count && *frame_count < SAMPLE_MAX; index++)
            frames[(*frame_count)++] = times[index];
        if (area) areas[(*area_count)++] = area;
        harness_sleep(0.8);
    }
    compositor_stop(&compositor);
    if (verbose) {
        char *text = compositor_text(&compositor);
        size_t length = strlen(text);
        const char *tail = length > 6000 ? text + length - 6000 : text;
        fprintf(stderr, "%s:\n%s\n", directory, tail);
        fflush(stderr);
        free(text);
    }
}

static struct json_node *summary_json(const double *values, int count, const long long *areas,
        int area_count) {
    struct json_node *object = json_object();
    json_add(object, "frames", node_int(count));
    json_add(object, "median", node_real(round_digits(percentile(values, count, 0.5), 3)));
    json_add(object, "p99", node_real(round_digits(percentile(values, count, 0.99), 3)));
    double worst = 0.0;
    for (int index = 0; index < count; index++)
        if (values[index] > worst) worst = values[index];
    json_add(object, "worst", node_real(count ? round_digits(worst, 3) : 0.0));
    long long pixels = 0;
    if (area_count) {
        double *values = xmalloc((size_t)area_count * sizeof(double));
        for (int index = 0; index < area_count; index++) values[index] = (double)areas[index];
        pixels = round_integer(percentile(values, area_count, 0.5));
        free(values);
    }
    json_add(object, "pixels", node_int(pixels));
    return object;
}

static void marker_after_trail(const char *work, struct failure_list *failures,
        struct json_node *report) {
    static const struct point zigzag[5] = {
        { 280, 520 }, { 440, 200 }, { 600, 520 }, { 760, 200 }, { 920, 520 }
    };
    long long painted[2] = { 0, 0 };
    for (int trail = 0; trail < 2; trail++) {
        const char *tag = trail ? "with-trail" : "without-trail";
        char directory[PATH_SIZE];
        harness_join(directory, sizeof directory, work, "marker-");
        harness_concat(directory, sizeof directory, directory, tag);
        harness_mkparents(directory, 0777);
        struct compositor compositor;
        compositor_start(&compositor, directory, trail, 0, 0.5, 9.0, 0.35, 4, 60, 1);
        harness_sleep(1.0);
        char encoded[1024];
        encode_path(encoded, sizeof encoded, zigzag, 5);
        char *arguments[16];
        pointer_arguments(arguments, encoded, 48, 8, 0, 0);
        pid_t pointer = harness_spawn(arguments, compositor.env.items, NULL, NULL,
                "/dev/null", 0);
        harness_sleep(0.35);
        char frame[PATH_SIZE];
        harness_join(frame, sizeof frame, directory, "marker.ppm");
        compositor_screenshot(&compositor, frame);
        if (harness_wait_status(pointer, 30) < 0) {
            kill(pointer, SIGKILL);
            harness_wait_status(pointer, 10);
            failf("the pointer path client timed out");
        }
        painted[trail] = marker_pixels(frame);
        compositor_stop(&compositor);
    }
    struct json_node *marker = json_object();
    json_add(marker, "without-trail", node_int(painted[0]));
    json_add(marker, "with-trail", node_int(painted[1]));
    json_add(report, "marker", marker);
    if (painted[0] < 1000) {
        failure_add(failures, "marker: the surface drawn after the trail painted %lld pixels on "
                "its own, not a solid block", painted[0]);
    } else if (painted[1] != painted[0]) {
        failure_add(failures, "marker: the trail moved the surface drawn after it (%lld of %lld "
                "pixels)", painted[1], painted[0]);
    }
}

static void string_list(char *out, size_t size, const int *values, int count) {
    size_t used = 0;
    out[0] = '\0';
    used += (size_t)snprintf(out, size, "[");
    for (int index = 0; index < count && index < 4; index++)
        used += (size_t)snprintf(out + used, size - used, "%s%d", index ? ", " : "", values[index]);
    snprintf(out + used, size - used, "]");
}

static void attach_failures(struct json_node *report, const struct failure_list *failures) {
    struct json_node *array = json_array();
    for (int index = 0; index < failures->count; index++)
        json_append(array, node_string(failures->messages[index]));
    json_add(report, "failures", array);
}

static void usage(const char *name) {
    fprintf(stderr, "usage: %s --tomoe PATH --pointer PATH [--trail] [--work DIR] [--emit DIR] "
            "[--report PATH] [--fps N] [--idle SECONDS] [--cell SIZE] [--life SECONDS] "
            "[--steps N] [--verbose]\n", name);
}

static const char *argument_value(int argc, char **argv, int *index, const char *name) {
    if (*index + 1 >= argc) {
        fprintf(stderr, "%s: argument %s: expected one argument\n", argv[0], name);
        exit(2);
    }
    (*index)++;
    return argv[*index];
}

static int argument_integer(int argc, char **argv, int *index, const char *name) {
    const char *value = argument_value(argc, argv, index, name);
    char *stop = NULL;
    long parsed = strtol(value, &stop, 10);
    if (stop == value || *stop) {
        fprintf(stderr, "%s: argument %s: invalid int value: '%s'\n", argv[0], name, value);
        exit(2);
    }
    return (int)parsed;
}

static double argument_double(int argc, char **argv, int *index, const char *name) {
    const char *value = argument_value(argc, argv, index, name);
    char *stop = NULL;
    double parsed = strtod(value, &stop);
    if (stop == value || *stop) {
        fprintf(stderr, "%s: argument %s: invalid float value: '%s'\n", argv[0], name, value);
        exit(2);
    }
    return parsed;
}

static void arc_points(struct point *points, double cx, double cy, double radius, double start,
        double end, int count) {
    for (int index = 0; index <= count; index++) {
        double angle = start + (end - start) * (double)index / (double)count;
        points[index].x = round_integer(cx + radius * cosine_value(angle));
        points[index].y = round_integer(cy + radius * sine_value(angle));
    }
}

int main(int argc, char **argv) {
    option_idle = 0.5;
    option_cell = 9.0;
    option_life = 0.35;
    option_steps = 4;
    for (int index = 1; index < argc; index++) {
        const char *argument = argv[index];
        if (strcmp(argument, "--tomoe") == 0) tomoe_binary = argument_value(argc, argv, &index, argument);
        else if (strcmp(argument, "--pointer") == 0) pointer_binary = argument_value(argc, argv, &index, argument);
        else if (strcmp(argument, "--trail") == 0) option_trail = 1;
        else if (strcmp(argument, "--work") == 0) work_directory = argument_value(argc, argv, &index, argument);
        else if (strcmp(argument, "--emit") == 0) emit_directory = argument_value(argc, argv, &index, argument);
        else if (strcmp(argument, "--report") == 0) report_path = argument_value(argc, argv, &index, argument);
        else if (strcmp(argument, "--fps") == 0) option_fps = argument_integer(argc, argv, &index, argument);
        else if (strcmp(argument, "--idle") == 0) option_idle = argument_double(argc, argv, &index, argument);
        else if (strcmp(argument, "--cell") == 0) option_cell = argument_double(argc, argv, &index, argument);
        else if (strcmp(argument, "--life") == 0) option_life = argument_double(argc, argv, &index, argument);
        else if (strcmp(argument, "--steps") == 0) option_steps = argument_integer(argc, argv, &index, argument);
        else if (strcmp(argument, "--verbose") == 0) option_verbose = 1;
        else if (strcmp(argument, "-h") == 0 || strcmp(argument, "--help") == 0) {
            usage(argv[0]);
            return 0;
        } else {
            fprintf(stderr, "%s: unrecognized arguments: %s\n", argv[0], argument);
            return 2;
        }
    }
    if (!tomoe_binary || !pointer_binary) {
        fprintf(stderr, "%s: the following arguments are required: --tomoe, --pointer\n", argv[0]);
        usage(argv[0]);
        return 2;
    }
    char work[PATH_SIZE];
    if (work_directory) {
        snprintf(work, sizeof work, "%s", work_directory);
        harness_mkparents(work, 0777);
    } else {
        const char *root = getenv("TMPDIR");
        if (!root || !*root) root = "/tmp";
        snprintf(work, sizeof work, "%s/trail-XXXXXX", root);
        if (!mkdtemp(work)) harness_fail("cannot create a temporary directory");
    }
    struct failure_list failures;
    memset(&failures, 0, sizeof failures);
    struct json_node *report = json_object();
    struct compositor compositor;
    compositor_start(&compositor, work, option_trail, option_fps, option_idle, option_cell,
            option_life, option_steps, 60, 0);
    active_report = report;
    active_failures = &failures;
    active_compositor = &compositor;
    active_flow = 1;
    struct point arc[49];
    static const struct point zigzag[5] = {
        { 280, 520 }, { 440, 200 }, { 600, 520 }, { 760, 200 }, { 920, 520 }
    };
    arc_points(arc, 640, 440, 300, 3.2, 6.0, 48);
    harness_sleep(1.0);
    char baseline[PATH_SIZE];
    harness_join(baseline, sizeof baseline, work, "idle.ppm");
    compositor_screenshot(&compositor, baseline);
    json_add(report, "idle_lit", node_int(image_lit_count(baseline)));
    if (!option_trail) {
        park(&compositor, arc);
        struct trace_result trace_result;
        trace(&compositor, arc, 49, 2, 6, 0, "plain", &trace_result);
        char settled[PATH_SIZE];
        harness_join(settled, sizeof settled, work, "plain-settled.ppm");
        compositor_screenshot(&compositor, settled);
        char digest[65];
        file_digest(settled, digest);
        struct json_node *plain = json_object();
        json_add(plain, "digest", node_string(digest));
        json_add(plain, "identical_to_idle", node_bool(files_equal(settled, baseline)));
        json_add(report, "plain", plain);
        if (emit_directory && trace_result.have_frame) {
            harness_mkparents(emit_directory, 0777);
            char target[PATH_SIZE];
            harness_join(target, sizeof target, emit_directory, "plain.png");
            write_png(trace_result.frame, target);
        }
        report_print(report);
        compositor_stop(&compositor);
        attach_failures(report, &failures);
        report_print(report);
        if (report_path) report_save(report, report_path);
        return failures.count ? 1 : 0;
    }
    struct frame_table *before = frame_table_new();
    compositor_frames(&compositor, before);
    long long wakes = compositor_wakeups(&compositor);
    harness_sleep(2.0);
    struct frame_table *after = frame_table_new();
    compositor_frames(&compositor, after);
    long long wake_delta = compositor_wakeups(&compositor) - wakes;
    long long drawn = tables_drawn(before, after);
    frame_table_free(before);
    frame_table_free(after);
    struct json_node *idle = json_object();
    json_add(idle, "seconds", node_real(2.0));
    json_add(idle, "drawn", node_int(drawn));
    json_add(idle, "wakes", node_int(wake_delta));
    json_add(report, "idle", idle);
    if (drawn)
        failure_add(&failures, "idle: the overlay drew %lld frames while the pointer was still",
                drawn);
    marker_after_trail(work, &failures, report);
    struct motion {
        const char *tag;
        const struct point *path;
        int count;
        int steps;
        int interval;
        int burst;
    } motions[3] = {
        { "arc", arc, 49, 2, 6, 0 },
        { "burst-arc", arc, 49, 1, 5, 1 },
        { "burst-zigzag", zigzag, 5, 6, 5, 1 }
    };
    for (int index = 0; index < 3; index++) {
        struct motion *motion = &motions[index];
        park(&compositor, motion->path);
        struct trace_result trace_result;
        trace(&compositor, motion->path, motion->count, motion->steps, motion->interval,
                motion->burst, motion->tag, &trace_result);
        if (!trace_result.have_frame) {
            struct json_node *entry = json_object();
            json_add(entry, "shots", node_int(trace_result.shots));
            json_add(report, motion->tag, entry);
            failure_add(&failures, "%s: the motion drew no trail", motion->tag);
            continue;
        }
        struct json_node *entry = json_object();
        json_add(entry, "shots", node_int(trace_result.shots));
        struct json_node *stats = json_object();
        json_add(stats, "lit", node_int(trace_result.stats.lit));
        json_add(stats, "path_mean", node_real(trace_result.stats.path_mean));
        json_add(stats, "path_max", node_real(trace_result.stats.path_max));
        json_add(stats, "chord_mean", node_real(trace_result.stats.chord_mean));
        json_add(stats, "off_chord", node_int(trace_result.stats.off_chord));
        json_add(entry, "stats", stats);
        json_add(report, motion->tag, entry);
        if (trace_result.stats.path_mean > TOLERANCE ||
                trace_result.stats.path_max > MAXIMUM) {
            char mean_text[64], max_text[64];
            float_text(trace_result.stats.path_mean, mean_text, sizeof mean_text);
            float_text(trace_result.stats.path_max, max_text, sizeof max_text);
            failure_add(&failures, "%s: the trail is %spx from the path it was dragged along "
                    "(worst %spx), not following it", motion->tag, mean_text, max_text);
        }
        if (emit_directory) {
            harness_mkparents(emit_directory, 0777);
            char target[PATH_SIZE];
            char name[128];
            snprintf(name, sizeof name, "%s.png", motion->tag);
            harness_join(target, sizeof target, emit_directory, name);
            write_png(trace_result.frame, target);
        }
    }
    park(&compositor, zigzag);
    int *fades = NULL;
    int fade_count = 0;
    struct recorder recorder;
    recorder_start(&recorder, &compositor, "fade", 0);
    harness_sleep(0.2);
    drag(&compositor, zigzag, 5, 6, 8, 0, 0);
    recorder_stop(&recorder, &fades, &fade_count, NULL, 0, NULL, NULL);
    struct json_node *fade_json = json_array();
    for (int index = 0; index < fade_count; index++)
        json_append(fade_json, node_int(fades[index]));
    json_add(report, "fade", fade_json);
    int peak = 0;
    for (int index = 1; index < fade_count; index++)
        if (fades[index] > fades[peak]) peak = index;
    if (!fade_count || fades[peak] < 200) {
        failure_add(&failures, "fade: no trail was drawn to fade (%d frames)", fade_count);
    } else {
        if (fades[fade_count - 1]) {
            failure_add(&failures, "fade: the trail was still lit (%d px) after the pointer "
                    "stopped", fades[fade_count - 1]);
        }
        int *grown = xmalloc((size_t)fade_count * sizeof(int));
        int grown_count = 0;
        for (int index = peak; index + 1 < fade_count; index++)
            if (fades[index + 1] > fades[index] + 50)
                grown[grown_count++] = index - peak;
        if (grown_count) {
            char listed[128];
            string_list(listed, sizeof listed, grown, grown_count);
            failure_add(&failures, "fade: the trail grew after the pointer stopped (%s)", listed);
        }
        free(grown);
    }
    char settled[PATH_SIZE];
    harness_join(settled, sizeof settled, work, "settled.ppm");
    compositor_screenshot(&compositor, settled);
    long long settled_lit = image_lit_count(settled);
    struct json_node *settled_json = json_object();
    json_add(settled_json, "lit", node_int(settled_lit));
    json_add(settled_json, "identical_to_idle", node_bool(files_equal(settled, baseline)));
    json_add(report, "settled", settled_json);
    if (settled_lit)
        failure_add(&failures, "settled: the trail left pixels behind after fading");
    before = frame_table_new();
    compositor_frames(&compositor, before);
    wakes = compositor_wakeups(&compositor);
    harness_sleep(1.5);
    after = frame_table_new();
    compositor_frames(&compositor, after);
    wake_delta = compositor_wakeups(&compositor) - wakes;
    drawn = tables_drawn(before, after);
    frame_table_free(before);
    frame_table_free(after);
    struct json_node *rest = json_object();
    json_add(rest, "seconds", node_real(1.5));
    json_add(rest, "drawn", node_int(drawn));
    json_add(rest, "wakes", node_int(wake_delta));
    json_add(report, "rest", rest);
    if (drawn)
        failure_add(&failures, "rest: the overlay drew %lld frames with the pointer still", drawn);
    compositor_stop(&compositor);
    struct paced_entry refresh60, refresh240, cap;
    paced(work, arc, 49, 60, 0, &refresh60);
    json_add(report, "refresh-60", paced_json(&refresh60));
    judge_pace(&refresh60, &failures);
    paced(work, arc, 49, 240, 0, &refresh240);
    json_add(report, "refresh-240", paced_json(&refresh240));
    judge_pace(&refresh240, &failures);
    paced(work, arc, 49, 240, 60, &cap);
    json_add(report, "cap", paced_json(&cap));
    judge_pace(&cap, &failures);
    double uncapped = refresh240.resting_hz;
    if (uncapped <= 2.0 * CAP_GROWTH * (double)CAP_FPS)
        failure_add(&failures, "refresh pacing: the uncapped trail did not step faster than the "
                ":fps %d cap, so the cap was not what limited it", CAP_FPS);
    double *on_frames = xmalloc(SAMPLE_MAX * sizeof(double));
    long long *on_areas = xmalloc(SAMPLE_MAX * sizeof(long long));
    int on_count = 0, on_area_count = 0;
    drag_cost(work, zigzag, 5, 1, option_verbose, on_frames, &on_count, on_areas,
            &on_area_count);
    double *off_frames = xmalloc(SAMPLE_MAX * sizeof(double));
    long long *off_areas = xmalloc(SAMPLE_MAX * sizeof(long long));
    int off_count = 0, off_area_count = 0;
    drag_cost(work, zigzag, 5, 0, 0, off_frames, &off_count, off_areas, &off_area_count);
    struct json_node *cost = json_object();
    json_add(cost, "on", summary_json(on_frames, on_count, on_areas, on_area_count));
    json_add(cost, "off", summary_json(off_frames, off_count, off_areas, off_area_count));
    json_add(report, "cost", cost);
    if (on_count < DRAGS || off_count < DRAGS)
        failure_add(&failures, "cost: too few frames to compare (%d with the trail, %d without)",
                on_count, off_count);
    double carried = round_digits(percentile(on_frames, on_count, 0.5), 3);
    double plain = round_digits(percentile(off_frames, off_count, 0.5), 3);
    if (carried > plain * COST_GROWTH + COST_SLACK) {
        char carried_text[64], plain_text[64];
        float_text(carried, carried_text, sizeof carried_text);
        float_text(plain, plain_text, sizeof plain_text);
        failure_add(&failures, "cost: with the trail a drag frame took %s ms at the median, "
                "against %s ms without it", carried_text, plain_text);
    }
    compositor_stop(&compositor);
    attach_failures(report, &failures);
    report_print(report);
    if (report_path) report_save(report, report_path);
    free(fades);
    free(on_frames);
    free(on_areas);
    free(off_frames);
    free(off_areas);
    return failures.count ? 1 : 0;
}
