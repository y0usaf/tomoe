#include "udev_priv.h"

#include <dirent.h>
#include <errno.h>
#include <fnmatch.h>
#include <stdlib.h>
#include <string.h>

static int match_name(struct udev_enumerate *enumerate, const char *name) {
    if (enumerate->nmatch == 0) return 1;
    for (size_t i = 0; i < enumerate->nmatch; i++)
        if (fnmatch(enumerate->match[i], name, 0) == 0) return 1;
    return 0;
}

static int device_matches(struct udev_enumerate *enumerate, struct udev_device *device) {
    const char *subsystem = tudev_device_subsystem(device);
    if (!subsystem) return enumerate->nmatch == 0;
    return match_name(enumerate, subsystem);
}

static void clear_devices(struct udev_enumerate *enumerate) {
    for (size_t i = 0; i < enumerate->ndevices; i++)
        udev_device_unref(enumerate->devices[i]);
    free(enumerate->devices);
    enumerate->devices = NULL;
    enumerate->ndevices = 0;
    enumerate->cdevices = 0;
}

static int add_device(struct udev_enumerate *enumerate, struct udev_device *device) {
    const char *syspath = udev_device_get_syspath(device);
    for (size_t i = 0; i < enumerate->ndevices; i++)
        if (strcmp(udev_device_get_syspath(enumerate->devices[i]), syspath) == 0) return 0;
    if (enumerate->ndevices == enumerate->cdevices) {
        size_t next = enumerate->cdevices ? enumerate->cdevices * 2 : 32;
        struct udev_device **grown = realloc(enumerate->devices, next * sizeof(*grown));
        if (!grown) return -ENOMEM;
        enumerate->devices = grown;
        enumerate->cdevices = next;
    }
    enumerate->devices[enumerate->ndevices++] = device;
    return 1;
}

static int scan_dir(struct udev_enumerate *enumerate, const char *path) {
    DIR *dir = opendir(path);
    if (!dir) return errno == ENOENT ? 0 : -errno;
    int result = 0;
    struct dirent *entry;
    while ((entry = readdir(dir))) {
        if (entry->d_name[0] == '.') continue;
        if (entry->d_type != DT_DIR && entry->d_type != DT_LNK &&
            entry->d_type != DT_UNKNOWN)
            continue;
        char *syspath = tudev_path_join(path, entry->d_name);
        if (!syspath) {
            result = -ENOMEM;
            break;
        }
        struct udev_device *device = udev_device_new_from_syspath(enumerate->udev, syspath);
        free(syspath);
        if (!device) continue;
        if (!device_matches(enumerate, device)) {
            udev_device_unref(device);
            continue;
        }
        int owned = add_device(enumerate, device);
        if (owned < 0) {
            udev_device_unref(device);
            result = owned;
            break;
        }
        struct udev_device *current = device;
        for (;;) {
            struct udev_device *parent = udev_device_get_parent(current);
            if (!parent) break;
            if (device_matches(enumerate, parent)) {
                struct udev_device *copy =
                    udev_device_new_from_syspath(enumerate->udev,
                                                 udev_device_get_syspath(parent));
                if (!copy) break;
                int added = add_device(enumerate, copy);
                if (added <= 0) {
                    udev_device_unref(copy);
                    if (added == 0) break;
                    result = added;
                    break;
                }
            }
            current = parent;
        }
        if (owned == 0) udev_device_unref(device);
    }
    closedir(dir);
    return result;
}

static int scan_top(struct udev_enumerate *enumerate, const char *basedir,
                    const char *subdir) {
    char *root = tudev_path_join("/sys", basedir);
    if (!root) return -ENOMEM;
    DIR *dir = opendir(root);
    if (!dir) {
        int error = errno;
        free(root);
        return error == ENOENT ? 0 : -error;
    }
    int result = 0;
    struct dirent *entry;
    while ((entry = readdir(dir))) {
        if (entry->d_name[0] == '.') continue;
        if (entry->d_type != DT_DIR && entry->d_type != DT_LNK &&
            entry->d_type != DT_UNKNOWN)
            continue;
        if (!match_name(enumerate, entry->d_name)) continue;
        char *base = tudev_path_join(root, entry->d_name);
        if (!base) {
            result = -ENOMEM;
            break;
        }
        char *path = subdir ? tudev_path_join(base, subdir) : strdup(base);
        free(base);
        if (!path) {
            result = -ENOMEM;
            break;
        }
        int r = scan_dir(enumerate, path);
        free(path);
        if (r < 0) result = r;
    }
    closedir(dir);
    free(root);
    return result;
}

static int sound_device_compare(const char *a, const char *b) {
    const char *sound_a = strstr(a, "/sound/card");
    if (!sound_a) return 0;
    sound_a = strchr(a, '/');
    if (!sound_a) return 0;
    size_t prefix = (size_t)(sound_a - a);
    if (strncmp(a, b, prefix) != 0) return 0;
    const char *sound_b = b + prefix;
    int ca = strncmp(sound_a, "/controlC", 9) == 0;
    int cb = strncmp(sound_b, "/controlC", 9) == 0;
    return ca - cb;
}

static int late_block(const char *devpath) {
    return strstr(devpath, "/block/md") != NULL || strstr(devpath, "/block/dm-") != NULL;
}

static int device_compare(const void *pa, const void *pb) {
    const struct udev_device *a = *(const struct udev_device *const *)pa;
    const struct udev_device *b = *(const struct udev_device *const *)pb;
    int r = sound_device_compare(a->devpath, b->devpath);
    if (r != 0) return r;
    r = late_block(a->devpath) - late_block(b->devpath);
    if (r != 0) return r;
    return tudev_path_compare(a->devpath, b->devpath);
}

static int scan(struct udev_enumerate *enumerate) {
    clear_devices(enumerate);
    int result = 0;
    int r = scan_top(enumerate, "bus", "devices");
    if (r < 0) result = r;
    r = scan_top(enumerate, "class", NULL);
    if (r < 0) result = r;
    qsort(enumerate->devices, enumerate->ndevices, sizeof(enumerate->devices[0]),
          device_compare);
    enumerate->scanned = 1;
    return result;
}

struct udev_enumerate *udev_enumerate_new(struct udev *udev) {
    if (!udev) {
        errno = EINVAL;
        return NULL;
    }
    struct udev_enumerate *enumerate = tudev_new0(sizeof(*enumerate));
    if (!enumerate) {
        errno = ENOMEM;
        return NULL;
    }
    enumerate->udev = udev;
    enumerate->n_ref = 1;
    enumerate->list = tudev_list_new(0);
    if (!enumerate->list) {
        free(enumerate);
        errno = ENOMEM;
        return NULL;
    }
    return enumerate;
}

struct udev_enumerate *udev_enumerate_ref(struct udev_enumerate *enumerate) {
    if (enumerate) enumerate->n_ref++;
    return enumerate;
}

struct udev_enumerate *udev_enumerate_unref(struct udev_enumerate *enumerate) {
    if (!enumerate) return NULL;
    if (enumerate->n_ref > 1) {
        enumerate->n_ref--;
        return enumerate;
    }
    clear_devices(enumerate);
    for (size_t i = 0; i < enumerate->nmatch; i++) free(enumerate->match[i]);
    free(enumerate->match);
    tudev_list_free(enumerate->list);
    free(enumerate);
    return NULL;
}

struct udev *udev_enumerate_get_udev(struct udev_enumerate *enumerate) {
    if (!enumerate) {
        errno = EINVAL;
        return NULL;
    }
    return enumerate->udev;
}

int udev_enumerate_add_match_subsystem(struct udev_enumerate *enumerate,
                                       const char *subsystem) {
    if (!enumerate) return -EINVAL;
    if (!subsystem) return 0;
    if (tudev_strv_has(enumerate->match, enumerate->nmatch, subsystem)) return 0;
    if (enumerate->nmatch == enumerate->cmatch) {
        size_t next = enumerate->cmatch ? enumerate->cmatch * 2 : 4;
        char **grown = realloc(enumerate->match, next * sizeof(*grown));
        if (!grown) return -ENOMEM;
        enumerate->match = grown;
        enumerate->cmatch = next;
    }
    char *copy = strdup(subsystem);
    if (!copy) return -ENOMEM;
    enumerate->match[enumerate->nmatch++] = copy;
    enumerate->scanned = 0;
    return 0;
}

int udev_enumerate_scan_devices(struct udev_enumerate *enumerate) {
    if (!enumerate) return -EINVAL;
    return scan(enumerate);
}

struct udev_list_entry *udev_enumerate_get_list_entry(struct udev_enumerate *enumerate) {
    if (!enumerate) {
        errno = EINVAL;
        return NULL;
    }
    if (!enumerate->scanned) scan(enumerate);
    tudev_list_clear(enumerate->list);
    for (size_t i = 0; i < enumerate->ndevices; i++)
        tudev_list_add(enumerate->list, udev_device_get_syspath(enumerate->devices[i]), NULL);
    if (!enumerate->list->first) {
        errno = ENODATA;
        return NULL;
    }
    return enumerate->list->first;
}
