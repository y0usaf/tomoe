#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <time.h>
#include <unistd.h>

#ifdef NATIVE
#include "libudev.h"
#else
#include <libudev.h>
#endif

static const char *const subsystems[] = {
    "input", "drm", "usb", "hid", "pci", "platform", "serio", "i2c",
};

static const char *const key_list[] = {
    "ID_SEAT", "WL_SEAT", "WL_OUTPUT", "ID_INPUT", "ID_INPUT_KEY",
    "ID_INPUT_MOUSE", "ID_INPUT_TOUCHPAD", "ID_INPUT_TOUCHSCREEN",
    "ID_INPUT_JOYSTICK", "ID_INPUT_TABLET", "ID_PATH", "ID_BUS", "ID_VENDOR",
    "ID_MODEL", "ID_SERIAL", "LIBINPUT_DEVICE_GROUP", "LIBINPUT_IGNORE_DEVICE",
    "MOUSE_DPI", "ABS_MT_TOUCH_MAJOR", "NO_SUCH_PROPERTY", "WL_OUTPUT_X",
    "ABS_MT_MAX",
};

static const char *dash(const char *s) {
    return s ? s : "-";
}

static int token_separator(char c) {
    return c == ' ' || c == ':';
}

static int token_compare(const void *a, const void *b) {
    return strcmp(*(char *const *)a, *(char *const *)b);
}

static char *normalize(const char *key, const char *value) {
    if (!value) return NULL;
    if (strcmp(key, "DEVLINKS") != 0 && strcmp(key, "TAGS") != 0 &&
        strcmp(key, "CURRENT_TAGS") != 0)
        return strdup(value);
    size_t n = 0, cap = 0;
    char **tokens = NULL;
    const char *p = value;
    while (*p) {
        while (*p && token_separator(*p)) p++;
        if (!*p) break;
        const char *start = p;
        while (*p && !token_separator(*p)) p++;
        if (n == cap) {
            cap = cap ? cap * 2 : 8;
            tokens = realloc(tokens, cap * sizeof(*tokens));
        }
        tokens[n++] = strndup(start, (size_t)(p - start));
    }
    if (n) qsort(tokens, n, sizeof(*tokens), token_compare);
    char separator = strcmp(key, "DEVLINKS") == 0 ? ' ' : ':';
    size_t length = 1;
    for (size_t i = 0; i < n; i++) length += strlen(tokens[i]) + (i ? 1 : 0);
    char *joined = malloc(length);
    size_t offset = 0;
    for (size_t i = 0; i < n; i++) {
        if (i) joined[offset++] = separator;
        size_t l = strlen(tokens[i]);
        memcpy(joined + offset, tokens[i], l);
        offset += l;
        free(tokens[i]);
    }
    free(tokens);
    joined[offset] = 0;
    return joined;
}

static void print_properties(struct udev_device *device, const char *prefix) {
    struct udev_list_entry *entry, *first =
        udev_device_get_properties_list_entry(device);
    size_t n = 0, cap = 0;
    char **keys = NULL, **values = NULL;
    udev_list_entry_foreach(entry, first) {
        if (n == cap) {
            cap = cap ? cap * 2 : 16;
            keys = realloc(keys, cap * sizeof(*keys));
            values = realloc(values, cap * sizeof(*values));
        }
        keys[n] = strdup(udev_list_entry_get_name(entry));
        values[n] = strdup(udev_list_entry_get_value(entry));
        n++;
    }
    for (size_t i = 0; i < n; i++) {
        char *norm = normalize(keys[i], values[i]);
        printf("%sp %s=%s\n", prefix, keys[i], norm ? norm : "");
        free(norm);
        const char *looked = udev_device_get_property_value(device, keys[i]);
        char *lnorm = normalize(keys[i], looked);
        printf("%spv %s=%s\n", prefix, keys[i], lnorm ? lnorm : "-");
        free(lnorm);
        free(keys[i]);
        free(values[i]);
    }
    free(keys);
    free(values);
}

static void print_links(struct udev_device *device) {
    struct udev_list_entry *entry, *first = udev_device_get_devlinks_list_entry(device);
    size_t n = 0, cap = 0;
    char **links = NULL;
    udev_list_entry_foreach(entry, first) {
        if (n == cap) {
            cap = cap ? cap * 2 : 4;
            links = realloc(links, cap * sizeof(*links));
        }
        links[n++] = strdup(udev_list_entry_get_name(entry));
    }
    if (n) qsort(links, n, sizeof(*links), token_compare);
    for (size_t i = 0; i < n; i++) {
        printf("link %s\n", links[i]);
        free(links[i]);
    }
    free(links);
}

static void print_devnum(dev_t number) {
    if (major(number) == 0 && minor(number) == 0) printf("-");
    else printf("%u:%u", major(number), minor(number));
}

static void dump_device(struct udev *udev, const char *syspath) {
    struct udev_device *device = udev_device_new_from_syspath(udev, syspath);
    if (!device) {
        printf("dev %s missing\n", syspath);
        return;
    }
    printf("dev %s\n", udev_device_get_syspath(device));
    printf("devpath %s\n", dash(udev_device_get_devpath(device)));
    printf("sysname %s\n", dash(udev_device_get_sysname(device)));
    printf("sysnum %s\n", dash(udev_device_get_sysnum(device)));
    printf("devnode %s\n", dash(udev_device_get_devnode(device)));
    printf("devnum ");
    print_devnum(udev_device_get_devnum(device));
    printf("\n");
    printf("subsystem %s\n", dash(udev_device_get_subsystem(device)));
    printf("devtype %s\n", dash(udev_device_get_devtype(device)));
    printf("driver %s\n", dash(udev_device_get_driver(device)));
    printf("initialized %d\n", udev_device_get_is_initialized(device));
    printf("udev %s\n", udev_device_get_udev(device) ? "ok" : "missing");
    print_properties(device, "");
    print_links(device);
    for (size_t i = 0; i < sizeof(key_list) / sizeof(key_list[0]); i++) {
        const char *value = udev_device_get_property_value(device, key_list[i]);
        printf("get %s=%s\n", key_list[i], dash(value));
    }
    const char *sysattr = udev_device_get_sysattr_value(device, "modalias");
    char *copy = sysattr ? strdup(sysattr) : NULL;
    if (copy) {
        for (char *p = copy; *p; p++)
            if (*p == '\n') *p = 0;
    }
    printf("sysattr modalias=%s\n", dash(copy));
    free(copy);
    struct udev_device *current = device;
    for (;;) {
        struct udev_device *parent = udev_device_get_parent(current);
        if (!parent) break;
        printf("parent %s %s\n", dash(udev_device_get_syspath(parent)),
               dash(udev_device_get_subsystem(parent)));
        current = parent;
    }
    const char *subsystem = udev_device_get_subsystem(device);
    dev_t number = udev_device_get_devnum(device);
    if (major(number) != 0) {
        char type = subsystem && strcmp(subsystem, "block") == 0 ? 'b' : 'c';
        struct udev_device *found = udev_device_new_from_devnum(udev, type, number);
        printf("lookup devnum %s\n", found ? udev_device_get_syspath(found) : "missing");
        udev_device_unref(found);
    }
    udev_device_unref(device);
}

static void gather(char ***paths, size_t *n, size_t *cap, const char *path) {
    for (size_t i = 0; i < *n; i++)
        if (strcmp((*paths)[i], path) == 0) return;
    if (*n == *cap) {
        *cap = *cap ? *cap * 2 : 64;
        *paths = realloc(*paths, *cap * sizeof(**paths));
    }
    (*paths)[(*n)++] = strdup(path);
}

static int string_compare(const void *a, const void *b) {
    return strcmp(*(char *const *)a, *(char *const *)b);
}

static void gather_dir(char ***paths, size_t *n, size_t *cap, const char *dir) {
    DIR *handle = opendir(dir);
    if (!handle) return;
    struct dirent *entry;
    while ((entry = readdir(handle))) {
        if (entry->d_name[0] == '.') continue;
        size_t length = strlen(dir) + strlen(entry->d_name) + 2;
        char *path = malloc(length);
        if (!path) break;
        snprintf(path, length, "%s/%s", dir, entry->d_name);
        char *real = realpath(path, NULL);
        free(path);
        if (!real) continue;
        struct stat st;
        if (stat(real, &st) == 0 && S_ISDIR(st.st_mode)) gather(paths, n, cap, real);
        free(real);
    }
    closedir(handle);
}

static void gather_ancestors(char ***paths, size_t *n, size_t *cap, const char *path) {
    char *copy = strdup(path);
    if (!copy) return;
    for (;;) {
        char *slash = strrchr(copy, '/');
        if (!slash || slash == copy) break;
        *slash = 0;
        if (strcmp(copy, "/sys") == 0) break;
        gather(paths, n, cap, copy);
    }
    free(copy);
}

static const char *const roots[] = {
    "/sys/class/input",
    "/sys/class/drm",
    "/sys/bus/usb/devices",
    "/sys/bus/hid/devices",
    "/sys/bus/pci/devices",
    "/sys/bus/platform/devices",
    "/sys/bus/serio/devices",
    "/sys/bus/i2c/devices",
};

static int dump_mode(void) {
    struct udev *udev = udev_new();
    if (!udev) return 1;
    char **paths = NULL;
    size_t n = 0, cap = 0;
    for (size_t i = 0; i < sizeof(roots) / sizeof(roots[0]); i++)
        gather_dir(&paths, &n, &cap, roots[i]);
    for (size_t i = 0; i < sizeof(subsystems) / sizeof(subsystems[0]); i++) {
        struct udev_enumerate *enumerate = udev_enumerate_new(udev);
        udev_enumerate_add_match_subsystem(enumerate, subsystems[i]);
        udev_enumerate_scan_devices(enumerate);
        struct udev_list_entry *entry, *first = udev_enumerate_get_list_entry(enumerate);
        size_t count = 0;
        udev_list_entry_foreach(entry, first) {
            const char *path = udev_list_entry_get_name(entry);
            printf("enum %s %s\n", subsystems[i], path);
            gather(&paths, &n, &cap, path);
            count++;
        }
        printf("enumcount %s %zu\n", subsystems[i], count);
        udev_enumerate_unref(enumerate);
    }
    size_t found = n;
    for (size_t i = 0; i < found; i++) gather_ancestors(&paths, &n, &cap, paths[i]);
    qsort(paths, n, sizeof(*paths), string_compare);
    for (size_t i = 0; i < n; i++) dump_device(udev, paths[i]);
    printf("devices %zu\n", n);
    for (size_t i = 0; i < n; i++) free(paths[i]);
    free(paths);
    udev_unref(udev);
    return 0;
}

static void print_event(struct udev_device *device) {
    printf("ev action=%s\n", dash(udev_device_get_action(device)));
    printf("ev syspath=%s\n", dash(udev_device_get_syspath(device)));
    printf("ev sysname=%s\n", dash(udev_device_get_sysname(device)));
    printf("ev devnode=%s\n", dash(udev_device_get_devnode(device)));
    printf("ev devnum ");
    print_devnum(udev_device_get_devnum(device));
    printf("\n");
    printf("ev subsystem=%s\n", dash(udev_device_get_subsystem(device)));
    printf("ev devtype=%s\n", dash(udev_device_get_devtype(device)));
    printf("ev initialized=%d\n", udev_device_get_is_initialized(device));
    print_properties(device, "ev ");
}

static int monitor_mode(int seconds) {
    struct udev *udev = udev_new();
    if (!udev) return 1;
    struct udev_monitor *monitor = udev_monitor_new_from_netlink(udev, "udev");
    if (!monitor) {
        fprintf(stderr, "udev-trace: monitor creation failed: %s\n", strerror(errno));
        return 2;
    }
    if (udev_monitor_filter_add_match_subsystem_devtype(monitor, "input", NULL) < 0) {
        fprintf(stderr, "udev-trace: filter failed\n");
        return 2;
    }
    if (udev_monitor_enable_receiving(monitor) < 0) {
        fprintf(stderr, "udev-trace: enable failed: %s\n", strerror(errno));
        return 2;
    }
    int fd = udev_monitor_get_fd(monitor);
    printf("monitor start %d\n", fd >= 0);
    fflush(stdout);
    time_t end = time(NULL) + seconds;
    while (time(NULL) < end) {
        struct pollfd descriptor = { .fd = fd, .events = POLLIN };
        int ready = poll(&descriptor, 1, 200);
        if (ready < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (ready == 0) continue;
        for (;;) {
            errno = 0;
            struct udev_device *device = udev_monitor_receive_device(monitor);
            if (!device) {
                if (errno != 0 && errno != EAGAIN)
                    fprintf(stderr, "udev-trace: receive failed: %s\n", strerror(errno));
                break;
            }
            print_event(device);
            fflush(stdout);
            udev_device_unref(device);
        }
    }
    printf("monitor end\n");
    udev_monitor_unref(monitor);
    udev_unref(udev);
    return 0;
}

int main(int argc, char **argv) {
    if (argc > 1 && strcmp(argv[1], "monitor") == 0)
        return monitor_mode(argc > 2 ? atoi(argv[2]) : 20);
    return dump_mode();
}
