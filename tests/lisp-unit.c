#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lisp.h"

static int failures;
static int checks;
static long malloc_calls;
static int counting;

void *__real_malloc(size_t size);
void *__real_calloc(size_t count, size_t size);
void *__real_realloc(void *ptr, size_t size);

void *__wrap_malloc(size_t size)
{
    if (counting)
        malloc_calls++;
    return __real_malloc(size);
}

void *__wrap_calloc(size_t count, size_t size)
{
    if (counting)
        malloc_calls++;
    return __real_calloc(count, size);
}

void *__wrap_realloc(void *ptr, size_t size)
{
    if (counting)
        malloc_calls++;
    return __real_realloc(ptr, size);
}

static void print_to(char *buffer, size_t size, lisp_value value, bool readably)
{
    lisp_print(value, readably, buffer, size);
}

static lisp_value unit_check(int argc, lisp_value *argv)
{
    if (argc < 3)
        lisp_error("UNIT-CHECK needs a label, expected and actual");
    checks++;
    char expected[2048];
    char actual[2048];
    print_to(expected, sizeof(expected), argv[1], true);
    print_to(actual, sizeof(actual), argv[2], true);
    if (strcmp(expected, actual) != 0) {
        failures++;
        char label[256];
        print_to(label, sizeof(label), argv[0], false);
        printf("FAIL %s: expected %s got %s\n", label, expected, actual);
    }
    return lisp_nil();
}

static lisp_value unit_check_true(int argc, lisp_value *argv)
{
    if (argc < 2)
        lisp_error("UNIT-CHECK-TRUE needs a label and a value");
    checks++;
    if (lisp_nilp(argv[1])) {
        failures++;
        char label[256];
        print_to(label, sizeof(label), argv[0], false);
        printf("FAIL %s: value is NIL\n", label);
    }
    return lisp_nil();
}

static lisp_value unit_check_error(int argc, lisp_value *argv)
{
    if (argc < 2)
        lisp_error("UNIT-CHECK-ERROR needs a label and a condition");
    checks++;
    if (lisp_nilp(argv[1])) {
        failures++;
        char label[256];
        print_to(label, sizeof(label), argv[0], false);
        printf("FAIL %s: expected an error\n", label);
    }
    return lisp_nil();
}

static char *read_file(const char *path)
{
    FILE *file = fopen(path, "rb");
    if (!file)
        return NULL;
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return NULL;
    }
    long size = ftell(file);
    if (size < 0) {
        fclose(file);
        return NULL;
    }
    rewind(file);
    char *buffer = malloc((size_t)size + 1);
    if (!buffer) {
        fclose(file);
        return NULL;
    }
    size_t got = fread(buffer, 1, (size_t)size, file);
    fclose(file);
    buffer[got] = '\0';
    return buffer;
}

static const char *base_name(const char *path)
{
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

int main(int argc, char **argv)
{
    if (lisp_init((size_t)64 * 1024 * 1024) != 0) {
        fprintf(stderr, "cannot initialise lisp\n");
        return 1;
    }
    lisp_define_primitive("UNIT-CHECK", unit_check, 3, 3);
    lisp_define_primitive("UNIT-CHECK-TRUE", unit_check_true, 2, 2);
    lisp_define_primitive("UNIT-CHECK-ERROR", unit_check_error, 2, 2);

    for (int i = 1; i < argc; i++) {
        const char *spec = argv[i];
        const char *colon = strchr(spec, ':');
        if (!colon) {
            fprintf(stderr, "bad source spec %s\n", spec);
            return 1;
        }
        size_t plen = (size_t)(colon - spec);
        char package[16];
        if (plen >= sizeof(package)) {
            fprintf(stderr, "bad package in %s\n", spec);
            return 1;
        }
        memcpy(package, spec, plen);
        package[plen] = '\0';
        const char *path = colon + 1;
        char *text = read_file(path);
        if (!text) {
            fprintf(stderr, "cannot read %s\n", path);
            return 1;
        }
        const char *pkg = strcmp(package, "tomoe") == 0 ? "TOMOE" : "TOMOE-USER";
        lisp_value args[3];
        args[0] = lisp_cstring(base_name(path));
        args[1] = lisp_string(text, strlen(text));
        args[2] = lisp_cstring(pkg);
        char error[1024] = { 0 };
        int status = lisp_call("TOMOE::%LOAD-SOURCE", 3, args, NULL, error, sizeof(error));
        free(text);
        if (status != 0) {
            fprintf(stderr, "%s: %s\n", path, error);
            return 1;
        }
        lisp_end_turn();
    }

    lisp_value ignored = lisp_nil();
    char error[1024] = { 0 };
    if (lisp_call("TOMOE-USER::UNIT-WARMUP", 0, NULL, &ignored, error, sizeof(error)) != 0) {
        fprintf(stderr, "warmup: %s\n", error);
        return 1;
    }
    lisp_end_turn();
    counting = 1;
    malloc_calls = 0;
    for (int i = 0; i < 200; i++) {
        if (lisp_call("TOMOE-USER::UNIT-WARMUP", 0, NULL, &ignored, error, sizeof(error)) != 0) {
            fprintf(stderr, "warm call: %s\n", error);
            return 1;
        }
        lisp_end_turn();
    }
    counting = 0;
    lisp_value rounds = 0;
    (void)rounds;
    if (malloc_calls != 0) {
        printf("FAIL malloc-free-calls: %ld allocations\n", malloc_calls);
        failures++;
    }
    checks++;
    printf("%d checks, %d failures\n", checks, failures);
    lisp_shutdown();
    return failures == 0 ? 0 : 1;
}
