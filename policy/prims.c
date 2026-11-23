#include "sidecar.h"
#include "json.h"
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int64_t integer(lisp_value value) {
    if (lisp_intp(value)) return lisp_get_int(value);
    return lisp_numberp(value) ? (int64_t)lisp_get_double(value) : 0;
}

static uint64_t unsigned_integer(lisp_value value) {
    return (uint64_t)integer(value);
}

static const char *string(lisp_value value) {
    if (!lisp_stringp(value)) return "";
    const char *text = lisp_get_string(value, NULL);
    return text ? text : "";
}

static lisp_value boolean(bool value) {
    return lisp_bool(value);
}

static const char *window_identifier_text(uint32_t id) {
    bool found;
    lisp_value value = policy_cached_identifier(id, &found);
    if (!found) {
        lisp_value argument = lisp_uint(id);
        char error[256];
        if (core_call(TPL_PRIM_WINDOW_IDENTIFIER, 1, &argument, &value, error, sizeof(error)))
            return NULL;
    }
    return lisp_stringp(value) ? lisp_get_string(value, NULL) : NULL;
}

static lisp_value prim_xwayland_display(int argc, lisp_value *argv) {
    const char *display = getenv("DISPLAY");
    return lisp_cstring(display ? display : "");
}

static lisp_value prim_reload_requested(int argc, lisp_value *argv) {
    return lisp_nil();
}

static lisp_value prim_getenv(int argc, lisp_value *argv) {
    const char *value = getenv(string(argv[0]));
    return value ? lisp_cstring(value) : lisp_nil();
}

static lisp_value prim_read_file(int argc, lisp_value *argv) {
    const char *path = string(argv[0]);
    uint64_t limit = unsigned_integer(argv[1]);
    if (!*path) return lisp_nil();
    struct stat status;
    if (stat(path, &status) < 0 || !S_ISREG(status.st_mode)) return lisp_nil();
    if (status.st_size < 0 || (uint64_t)status.st_size > limit) return lisp_nil();
    FILE *file = fopen(path, "rb");
    if (!file) return lisp_nil();
    size_t size = (size_t)status.st_size;
    char *buffer = malloc(size + 1);
    if (!buffer) {
        fclose(file);
        return lisp_nil();
    }
    size_t got = fread(buffer, 1, size, file);
    fclose(file);
    if (got != size) {
        free(buffer);
        return lisp_nil();
    }
    buffer[size] = '\0';
    lisp_value text = lisp_string(buffer, size);
    free(buffer);
    return text;
}

struct path_list {
    char **items;
    size_t count, capacity;
    bool failed;
};

static void path_list_add(struct path_list *list, const char *path) {
    if (list->failed) return;
    if (list->count == list->capacity) {
        size_t capacity = list->capacity ? list->capacity * 2 : 64;
        char **grown = realloc(list->items, capacity * sizeof(char *));
        if (!grown) { list->failed = true; return; }
        list->items = grown;
        list->capacity = capacity;
    }
    list->items[list->count] = strdup(path);
    if (!list->items[list->count]) { list->failed = true; return; }
    list->count++;
}

static void path_list_walk(struct path_list *list, const char *path) {
    if (list->failed) return;
    struct stat info;
    if (lstat(path, &info) != 0) return;
    if (S_ISREG(info.st_mode)) {
        path_list_add(list, path);
        return;
    }
    if (!S_ISDIR(info.st_mode) || S_ISLNK(info.st_mode)) return;
    DIR *directory = opendir(path);
    if (!directory) return;
    struct dirent *entry;
    while (!list->failed && (entry = readdir(directory))) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
        char child[PATH_MAX];
        int written = snprintf(child, sizeof(child), "%s/%s", path, entry->d_name);
        if (written < 0 || (size_t)written >= sizeof(child)) continue;
        path_list_walk(list, child);
    }
    closedir(directory);
}

static int compare_paths(const void *left, const void *right) {
    return strcmp(*(char *const *)left, *(char *const *)right);
}

static lisp_value prim_list_directory(int argc, lisp_value *argv) {
    char resolved[PATH_MAX];
    if (!realpath(string(argv[0]), resolved)) return lisp_nil();
    struct path_list list = { 0 };
    path_list_walk(&list, resolved);
    if (list.failed) {
        for (size_t i = 0; i < list.count; i++) free(list.items[i]);
        free(list.items);
        lisp_error("Cannot list directory %s.", resolved);
    }
    qsort(list.items, list.count, sizeof(char *), compare_paths);
    lisp_value result = lisp_nil();
    for (size_t i = list.count; i > 0; i--) {
        result = lisp_cons(lisp_cstring(list.items[i - 1]), result);
        free(list.items[i - 1]);
    }
    free(list.items);
    return result;
}

static bool patch_name(const char *name) {
    size_t length = strlen(name);
    if (length < 1 || length > 40) return false;
    for (size_t i = 0; i < length; i++)
        if (!isalnum((unsigned char)name[i]) && name[i] != '-' && name[i] != '_')
            return false;
    return true;
}

static lisp_value prim_write_append(int argc, lisp_value *argv) {
    const char *path = string(argv[0]);
    size_t length = 0;
    const char *text = lisp_get_string(argv[1], &length);
    int fd = open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
    if (fd < 0) lisp_error("Cannot open %s.", path);
    while (length > 0) {
        ssize_t written = write(fd, text, length);
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0) { close(fd); lisp_error("Cannot write %s.", path); }
        text += written;
        length -= (size_t)written;
    }
    close(fd);
    return lisp_t();
}

static lisp_value prim_delete_file(int argc, lisp_value *argv) {
    const char *path = string(argv[0]);
    if (unlink(path) != 0 && errno != ENOENT) lisp_error("Cannot delete %s.", path);
    return lisp_t();
}

static lisp_value prim_write_patch_file(int argc, lisp_value *argv) {
    const char *name = string(argv[0]);
    if (!patch_name(name)) lisp_error("Invalid patch name %s.", name);
    const char *runtime = getenv("XDG_RUNTIME_DIR");
    if (!runtime) lisp_error("XDG_RUNTIME_DIR is not set.");
    char directory[PATH_MAX], path[PATH_MAX];
    int written = snprintf(directory, sizeof(directory), "%s/tomoe-patches", runtime);
    if (written < 0 || (size_t)written >= sizeof(directory))
        lisp_error("Patch directory path is too long.");
    if (mkdir(directory, 0700) != 0 && errno != EEXIST)
        lisp_error("Cannot create %s.", directory);
    written = snprintf(path, sizeof(path), "%s/%s.lisp", directory, name);
    if (written < 0 || (size_t)written >= sizeof(path))
        lisp_error("Patch path is too long.");
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) lisp_error("Cannot write %s.", path);
    if (fchmod(fd, 0600) != 0) { close(fd); lisp_error("Cannot write %s.", path); }
    size_t length = 0;
    const char *text = lisp_stringp(argv[1]) ? lisp_get_string(argv[1], &length) : "";
    if (!text) { close(fd); lisp_error("Cannot write %s.", path); }
    size_t offset = 0;
    while (offset < length) {
        ssize_t step = write(fd, text + offset, length - offset);
        if (step <= 0) { close(fd); lisp_error("Cannot write %s.", path); }
        offset += (size_t)step;
    }
    if (close(fd) != 0) lisp_error("Cannot write %s.", path);
    return lisp_cstring(path);
}

static lisp_value prim_json_broadcast(int argc, lisp_value *argv) {
    ipc_broadcast(string(argv[0]), argv[1]);
    return lisp_nil();
}

static lisp_value prim_screencast_answer(int argc, lisp_value *argv) {
    uint64_t token = unsigned_integer(argv[0]);
    lisp_value reply;
    if (lisp_eq_keyword(argv[1], "OUTPUT")) {
        const char *keys[3] = { "action", "type", "output" };
        lisp_value values[3] = { lisp_cstring("resolve"), lisp_cstring("output"),
            lisp_stringp(argv[2]) ? argv[2] : lisp_cstring("") };
        reply = json_build_object(3, keys, values);
    } else if (lisp_eq_keyword(argv[1], "WINDOW")) {
        const char *identifier = window_identifier_text((uint32_t)unsigned_integer(argv[2]));
        if (identifier && *identifier) {
            const char *keys[3] = { "action", "type", "identifier" };
            lisp_value values[3] = { lisp_cstring("resolve"), lisp_cstring("window"),
                lisp_cstring(identifier) };
            reply = json_build_object(3, keys, values);
        } else {
            const char *keys[1] = { "action" };
            lisp_value values[1] = { lisp_cstring("deny") };
            reply = json_build_object(1, keys, values);
        }
    } else {
        const char *keys[1] = { "action" };
        lisp_value values[1] = { lisp_cstring("deny") };
        reply = json_build_object(1, keys, values);
    }
    ipc_screencast_answer(token, reply);
    return lisp_nil();
}

static lisp_value prim_json_subscribed(int argc, lisp_value *argv) {
    return boolean(ipc_subscribed());
}

static lisp_value prim_json_validate_value(int argc, lisp_value *argv) {
    return json_validate_value(argv[0]);
}

static lisp_value prim_json_output_size(int argc, lisp_value *argv) {
    return lisp_uint(json_output_size(argv[0]));
}

static lisp_value prim_json_object(int argc, lisp_value *argv) {
    return json_object_of(argc, argv);
}

static lisp_value prim_json_array(int argc, lisp_value *argv) {
    return json_array_of(argc, argv);
}


static const struct local {
    const char *name;
    lisp_primitive fn;
    int min, max;
} locals[] = {
    { "%getenv", prim_getenv, 1, 1 },
    { "%read-file", prim_read_file, 2, 2 },
    { "%list-directory", prim_list_directory, 1, 1 },
    { "%write-patch-file", prim_write_patch_file, 2, 2 },
    { "%write-append", prim_write_append, 2, 2 },
    { "%delete-file", prim_delete_file, 1, 1 },
    { "%screencast-answer", prim_screencast_answer, 3, 3 },
    { "%json-broadcast", prim_json_broadcast, 2, 2 },
    { "%json-subscribed-p", prim_json_subscribed, 0, 0 },
    { "%json-validate-value", prim_json_validate_value, 1, 1 },
    { "%json-output-size", prim_json_output_size, 2, 2 },
    { "json-object", prim_json_object, 0, -1 },
    { "json-array", prim_json_array, 0, -1 },
    { "%XWAYLAND-DISPLAY", prim_xwayland_display, 0, 0 },
    { "%RELOAD-REQUESTED", prim_reload_requested, 0, 0 }
};

void policy_register_local(void) {
    for (size_t i = 0; i < sizeof(locals) / sizeof(locals[0]); i++)
        lisp_define_primitive(locals[i].name, locals[i].fn, locals[i].min, locals[i].max);
}
