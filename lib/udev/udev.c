#include "udev_priv.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

void *tudev_new0(size_t size) {
    return calloc(1, size);
}

char *tudev_strdup(const char *s) {
    return s ? strdup(s) : NULL;
}

char *tudev_strndup(const char *s, size_t n) {
    char *p = malloc(n + 1);
    if (!p) return NULL;
    memcpy(p, s, n);
    p[n] = 0;
    return p;
}

int tudev_streq(const char *a, const char *b) {
    return a && b && strcmp(a, b) == 0;
}

int tudev_streq_ptr(const char *a, const char *b) {
    if (!a || !b) return a == b;
    return strcmp(a, b) == 0;
}

char *tudev_read_file(const char *path, size_t *ret_len) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return NULL;
    size_t cap = 4096, len = 0;
    char *buf = malloc(cap + 1);
    if (!buf) {
        close(fd);
        errno = ENOMEM;
        return NULL;
    }
    for (;;) {
        if (len == cap) {
            char *next = realloc(buf, cap * 2 + 1);
            if (!next) {
                free(buf);
                close(fd);
                errno = ENOMEM;
                return NULL;
            }
            buf = next;
            cap *= 2;
        }
        ssize_t n = read(fd, buf + len, cap - len);
        if (n < 0) {
            if (errno == EINTR) continue;
            int error = errno;
            free(buf);
            close(fd);
            errno = error;
            return NULL;
        }
        if (n == 0) break;
        len += (size_t)n;
    }
    close(fd);
    buf[len] = 0;
    if (ret_len) *ret_len = len;
    return buf;
}

char *tudev_readlink_base(const char *path) {
    char buf[PATH_MAX + 1];
    ssize_t n = readlink(path, buf, sizeof(buf) - 1);
    if (n < 0) return NULL;
    buf[n] = 0;
    char *slash = strrchr(buf, '/');
    return strdup(slash ? slash + 1 : buf);
}

int tudev_utf8_valid(const char *s) {
    const unsigned char *p = (const unsigned char *)s;
    while (*p) {
        unsigned char c = *p;
        size_t extra;
        unsigned int cp;
        if (c < 0x80) { p++; continue; }
        if ((c & 0xe0) == 0xc0) { extra = 1; cp = c & 0x1f; }
        else if ((c & 0xf0) == 0xe0) { extra = 2; cp = c & 0x0f; }
        else if ((c & 0xf8) == 0xf0) { extra = 3; cp = c & 0x07; }
        else return 0;
        for (size_t i = 1; i <= extra; i++) {
            if ((p[i] & 0xc0) != 0x80) return 0;
            cp = (cp << 6) | (p[i] & 0x3f);
        }
        if (extra == 1 && cp < 0x80) return 0;
        if (extra == 2 && cp < 0x800) return 0;
        if (extra == 3 && cp < 0x10000) return 0;
        if (cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)) return 0;
        p += extra + 1;
    }
    return 1;
}

int tudev_has_cc(const char *s) {
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c < 0x20 || c == 0x7f) return 1;
    }
    return 0;
}

int tudev_strv_push(char ***v, size_t *n, size_t *cap, const char *s) {
    if (*n == *cap) {
        size_t next = *cap ? *cap * 2 : 8;
        char **grown = realloc(*v, next * sizeof(char *));
        if (!grown) return -ENOMEM;
        *v = grown;
        *cap = next;
    }
    char *copy = strdup(s);
    if (!copy) return -ENOMEM;
    (*v)[(*n)++] = copy;
    return 0;
}

int tudev_strv_has(char **v, size_t n, const char *s) {
    for (size_t i = 0; i < n; i++)
        if (strcmp(v[i], s) == 0) return 1;
    return 0;
}

static void strv_free(char **v, size_t n) {
    for (size_t i = 0; i < n; i++) free(v[i]);
    free(v);
}

char *tudev_path_join(const char *a, const char *b) {
    if (!a || !*a) return strdup(b ? b : "");
    if (!b || !*b) return strdup(a);
    size_t la = strlen(a), lb = strlen(b);
    int slash = a[la - 1] != '/' && b[0] != '/';
    char *p = malloc(la + lb + 2);
    if (!p) return NULL;
    memcpy(p, a, la);
    if (slash) p[la++] = '/';
    memcpy(p + la, b, lb + 1);
    return p;
}

void tudev_path_simplify(char *p) {
    size_t len = strlen(p), out = 0;
    char *tmp = malloc(len + 3);
    if (!tmp) return;
    int absolute = p[0] == '/';
    if (absolute) tmp[out++] = '/';
    const char *in = p;
    for (;;) {
        while (*in == '/') in++;
        if (!*in) break;
        const char *start = in;
        while (*in && *in != '/') in++;
        size_t clen = (size_t)(in - start);
        if (clen == 1 && start[0] == '.') continue;
        if (clen == 2 && start[0] == '.' && start[1] == '.') {
            if (out > (size_t)absolute) {
                out--;
                while (out > (size_t)absolute && tmp[out - 1] != '/') out--;
            }
            continue;
        }
        if (out > (size_t)absolute) tmp[out++] = '/';
        memcpy(tmp + out, start, clen);
        out += clen;
    }
    if (out == 0) tmp[out++] = '.';
    tmp[out] = 0;
    memcpy(p, tmp, out + 1);
    free(tmp);
}

static int next_component(const char **pp, const char **ret, size_t *len) {
    const char *p = *pp;
    for (;;) {
        while (*p == '/') p++;
        if (p[0] == '.' && (p[1] == 0 || p[1] == '/')) { p++; continue; }
        break;
    }
    *pp = p;
    if (!*p) {
        *ret = NULL;
        *len = 0;
        return 0;
    }
    const char *end = p;
    while (*end && *end != '/') end++;
    *ret = p;
    *len = (size_t)(end - p);
    *pp = end;
    return (int)(end - p);
}

int tudev_path_compare(const char *a, const char *b) {
    if (!a || !b) return (a != NULL) - (b != NULL);
    int abs_a = a[0] == '/', abs_b = b[0] == '/';
    if (abs_a != abs_b) return abs_a - abs_b;
    for (;;) {
        const char *ca, *cb;
        size_t la, lb;
        int ka = next_component(&a, &ca, &la);
        int kb = next_component(&b, &cb, &lb);
        if (ka == 0) return kb == 0 ? 0 : -1;
        if (kb == 0) return 1;
        size_t m = la < lb ? la : lb;
        int r = memcmp(ca, cb, m);
        if (r != 0) return r;
        if (la != lb) return la < lb ? -1 : 1;
    }
}

int tudev_prop_valid(const char *key, const char *value) {
    if (!key || !*key) return 0;
    for (const char *p = key; *p; p++) {
        char c = *p;
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '.'))
            return 0;
    }
    if (!value || !*value) return 1;
    return tudev_utf8_valid(value) && !tudev_has_cc(value);
}

static int filename_valid(const char *s) {
    if (!s || !*s) return 0;
    if (strchr(s, '/')) return 0;
    if (strcmp(s, ".") == 0 || strcmp(s, "..") == 0) return 0;
    return 1;
}

struct udev_list *tudev_list_new(int unique) {
    struct udev_list *list = tudev_new0(sizeof(*list));
    if (list) list->unique = unique;
    return list;
}

void tudev_list_clear(struct udev_list *list) {
    if (!list) return;
    struct udev_list_entry *entry = list->first;
    while (entry) {
        struct udev_list_entry *next = entry->next;
        free(entry->name);
        free(entry->value);
        free(entry);
        entry = next;
    }
    list->first = NULL;
    list->last = NULL;
    list->n = 0;
}

void tudev_list_free(struct udev_list *list) {
    if (!list) return;
    tudev_list_clear(list);
    free(list);
}

struct udev_list_entry *tudev_list_add(struct udev_list *list, const char *name, const char *value) {
    if (!list || !name) return NULL;
    if (list->unique) {
        for (struct udev_list_entry *entry = list->first; entry; entry = entry->next)
            if (strcmp(entry->name, name) == 0) {
                char *copy = tudev_strdup(value);
                if (value && !copy) return NULL;
                free(entry->value);
                entry->value = copy;
                return entry;
            }
    }
    struct udev_list_entry *entry = tudev_new0(sizeof(*entry));
    if (!entry) return NULL;
    entry->name = strdup(name);
    entry->value = tudev_strdup(value);
    if (!entry->name || (value && !entry->value)) {
        free(entry->name);
        free(entry->value);
        free(entry);
        return NULL;
    }
    if (list->last) list->last->next = entry;
    else list->first = entry;
    list->last = entry;
    list->n++;
    return entry;
}

struct udev *udev_ref(struct udev *udev) {
    if (udev) udev->n_ref++;
    return udev;
}

struct udev *udev_unref(struct udev *udev) {
    if (!udev) return NULL;
    if (udev->n_ref > 1) {
        udev->n_ref--;
        return udev;
    }
    free(udev);
    return NULL;
}

struct udev *udev_new(void) {
    struct udev *udev = tudev_new0(sizeof(*udev));
    if (!udev) {
        errno = ENOMEM;
        return NULL;
    }
    udev->n_ref = 1;
    return udev;
}

struct udev_list_entry *udev_list_entry_get_next(struct udev_list_entry *entry) {
    return entry ? entry->next : NULL;
}

const char *udev_list_entry_get_name(struct udev_list_entry *entry) {
    return entry ? entry->name : NULL;
}

const char *udev_list_entry_get_value(struct udev_list_entry *entry) {
    return entry ? entry->value : NULL;
}

struct udev_device *tudev_device_new(struct udev *udev) {
    struct udev_device *device = tudev_new0(sizeof(*device));
    if (!device) return NULL;
    device->udev = udev;
    device->n_ref = 1;
    device->devnum = makedev(0, 0);
    return device;
}

void tudev_device_free(struct udev_device *device) {
    if (!device) return;
    free(device->syspath);
    free(device->sysname);
    free(device->subsystem);
    free(device->driver_sub);
    free(device->devtype);
    free(device->driver);
    free(device->devname);
    free(device->action);
    for (size_t i = 0; i < device->nprops; i++) {
        free(device->props[i].key);
        free(device->props[i].value);
    }
    free(device->props);
    tudev_list_free(device->devlinks_list);
    tudev_list_free(device->tags_list);
    tudev_list_free(device->current_tags_list);
    for (size_t i = 0; i < device->nsysattr; i++) {
        free(device->sysattr_keys[i]);
        free(device->sysattr_vals[i]);
    }
    free(device->sysattr_keys);
    free(device->sysattr_vals);
    strv_free(device->devlinks, device->ndevlinks);
    strv_free(device->tags, device->ntags);
    strv_free(device->current_tags, device->ncurrent);
    tudev_list_free(device->props_list);
    udev_device_unref(device->parent);
    free(device);
}

struct udev_device *udev_device_ref(struct udev_device *device) {
    if (device) device->n_ref++;
    return device;
}

struct udev_device *udev_device_unref(struct udev_device *device) {
    if (!device) return NULL;
    if (device->n_ref > 1) {
        device->n_ref--;
        return device;
    }
    tudev_device_free(device);
    return NULL;
}

static int path_is_absolute(const char *p) {
    return p && p[0] == '/';
}

static int path_starts(const char *p, const char *prefix) {
    size_t n = strlen(prefix);
    if (strncmp(p, prefix, n) == 0) return 1;
    if (n > 0 && prefix[n - 1] == '/' && strncmp(p, prefix, n - 1) == 0 &&
        p[n - 1] == 0)
        return 1;
    return 0;
}

int tudev_set_syspath(struct udev_device *device, const char *path, int verify) {
    char *syspath = NULL;
    if (verify) {
        syspath = realpath(path, NULL);
        if (!syspath) {
            if (errno == ENOENT || errno == ENOTDIR) errno = ENODEV;
            return -errno;
        }
        if (!path_starts(syspath, "/sys/")) {
            char *real_sys = realpath("/sys", NULL);
            if (!real_sys) {
                free(syspath);
                return -ENODEV;
            }
            size_t n = strlen(real_sys);
            const char *rest = NULL;
            if (strncmp(syspath, real_sys, n) == 0 &&
                (syspath[n] == '/' || syspath[n] == 0))
                rest = syspath + n;
            if (!rest) {
                free(real_sys);
                free(syspath);
                return -ENODEV;
            }
            char *mapped = tudev_path_join("/sys", rest);
            free(real_sys);
            free(syspath);
            if (!mapped) return -ENOMEM;
            tudev_path_simplify(mapped);
            syspath = mapped;
        }
        if (path_starts(syspath, "/sys/devices/")) {
            char *uevent = tudev_path_join(syspath, "uevent");
            if (!uevent) {
                free(syspath);
                return -ENOMEM;
            }
            int ok = access(uevent, F_OK) == 0;
            int error = errno;
            free(uevent);
            if (!ok) {
                free(syspath);
                return error == ENOENT ? -ENODEV : -error;
            }
        } else {
            struct stat st;
            if (stat(syspath, &st) < 0) {
                int error = errno;
                free(syspath);
                return -error;
            }
            if (!S_ISDIR(st.st_mode)) {
                free(syspath);
                return -ENODEV;
            }
        }
    } else {
        if (!path_starts(path, "/sys/")) return -EINVAL;
        syspath = strdup(path);
        if (!syspath) return -ENOMEM;
        tudev_path_simplify(syspath);
    }
    if (strcmp(syspath, "/sys") == 0) {
        free(syspath);
        return -ENODEV;
    }
    free(device->syspath);
    free(device->sysname);
    device->sysname = NULL;
    device->sysnum = NULL;
    device->syspath = syspath;
    device->devpath = syspath + 4;
    return tudev_add_prop(device, "DEVPATH", device->devpath);
}

int tudev_add_prop(struct udev_device *device, const char *key, const char *value) {
    if (!tudev_prop_valid(key, value)) return -EINVAL;
    for (size_t i = 0; i < device->nprops; i++)
        if (strcmp(device->props[i].key, key) == 0) {
            if (!value || !*value) {
                free(device->props[i].key);
                free(device->props[i].value);
                memmove(&device->props[i], &device->props[i + 1],
                        (device->nprops - i - 1) * sizeof(device->props[0]));
                device->nprops--;
                device->generation++;
                return 0;
            }
            char *copy = strdup(value);
            if (!copy) return -ENOMEM;
            free(device->props[i].value);
            device->props[i].value = copy;
            device->generation++;
            return 0;
        }
    if (!value || !*value) {
        device->generation++;
        return 0;
    }
    if (device->nprops == device->cprops) {
        size_t next = device->cprops ? device->cprops * 2 : 16;
        struct udev_prop *grown = realloc(device->props, next * sizeof(*grown));
        if (!grown) return -ENOMEM;
        device->props = grown;
        device->cprops = next;
    }
    device->props[device->nprops].key = strdup(key);
    device->props[device->nprops].value = strdup(value);
    if (!device->props[device->nprops].key || !device->props[device->nprops].value) {
        free(device->props[device->nprops].key);
        free(device->props[device->nprops].value);
        return -ENOMEM;
    }
    device->nprops++;
    device->generation++;
    return 0;
}

const char *tudev_prop(struct udev_device *device, const char *key) {
    for (size_t i = 0; i < device->nprops; i++)
        if (strcmp(device->props[i].key, key) == 0) return device->props[i].value;
    return NULL;
}

static int set_subsystem(struct udev_device *device, const char *subsystem) {
    char *copy = tudev_strdup(subsystem);
    if (subsystem && !copy) return -ENOMEM;
    int r = tudev_add_prop(device, "SUBSYSTEM", copy);
    if (r < 0) {
        free(copy);
        return r;
    }
    device->subsystem_set = 1;
    free(device->subsystem);
    device->subsystem = copy;
    return 0;
}

static int set_devtype(struct udev_device *device, const char *devtype) {
    if (!devtype) return -EINVAL;
    char *copy = strdup(devtype);
    if (!copy) return -ENOMEM;
    int r = tudev_add_prop(device, "DEVTYPE", copy);
    if (r < 0) {
        free(copy);
        return r;
    }
    device->devtype_set = 1;
    free(device->devtype);
    device->devtype = copy;
    return 0;
}

static int set_driver(struct udev_device *device, const char *driver) {
    char *copy = tudev_strdup(driver);
    if (driver && !copy) return -ENOMEM;
    int r = tudev_add_prop(device, "DRIVER", copy);
    if (r < 0) {
        free(copy);
        return r;
    }
    device->driver_set = 1;
    free(device->driver);
    device->driver = copy;
    return 0;
}

static int mangle_devname(const char *p, char **ret) {
    if (!p || !*p) return -EINVAL;
    if (strstr(p, "/../") || strcmp(p, "..") == 0) return -EINVAL;
    char *q;
    if (path_is_absolute(p)) {
        if (!path_starts(p, "/dev/") || p[5] == 0) return -EINVAL;
        q = strdup(p);
    } else
        q = tudev_path_join("/dev", p);
    if (!q) return -ENOMEM;
    tudev_path_simplify(q);
    *ret = q;
    return 0;
}

static int set_devname(struct udev_device *device, const char *devname) {
    char *mangled = NULL;
    int r = mangle_devname(devname, &mangled);
    if (r < 0) return r;
    r = tudev_add_prop(device, "DEVNAME", mangled);
    if (r < 0) {
        free(mangled);
        return r;
    }
    free(device->devname);
    device->devname = mangled;
    return 0;
}

static int parse_uint(const char *s, unsigned *ret) {
    if (!s || !*s) return -EINVAL;
    if (*s == '+' || *s == '-') return -EINVAL;
    unsigned long long v = 0;
    for (; *s; s++) {
        if (*s < '0' || *s > '9') return -EINVAL;
        v = v * 10 + (unsigned)(*s - '0');
        if (v > UINT_MAX) return -ERANGE;
    }
    *ret = (unsigned)v;
    return 0;
}

static int parse_u64(const char *s, unsigned long long *ret) {
    if (!s || !*s) return -EINVAL;
    if (*s == '+' || *s == '-') return -EINVAL;
    unsigned long long v = 0;
    for (; *s; s++) {
        if (*s < '0' || *s > '9') return -EINVAL;
        unsigned digit = (unsigned)(*s - '0');
        if (v > (ULLONG_MAX - digit) / 10) return -ERANGE;
        v = v * 10 + digit;
    }
    *ret = v;
    return 0;
}

static int set_devnum(struct udev_device *device, const char *major_str, const char *minor_str) {
    unsigned major_no, minor_no = 0;
    if (parse_uint(major_str, &major_no) < 0) return -EINVAL;
    if (major_no == 0) return 0;
    if (minor_str && parse_uint(minor_str, &minor_no) < 0) return -EINVAL;
    int r = tudev_add_prop(device, "MAJOR", major_str);
    if (r < 0) return r;
    if (minor_str) {
        r = tudev_add_prop(device, "MINOR", minor_str);
        if (r < 0) return r;
    }
    device->devnum = makedev(major_no, minor_no);
    return 0;
}

static int set_ifindex(struct udev_device *device, const char *value) {
    unsigned index;
    if (parse_uint(value, &index) < 0 || index == 0 || index > INT_MAX) return -EINVAL;
    int r = tudev_add_prop(device, "IFINDEX", value);
    if (r < 0) return r;
    device->ifindex = (int)index;
    device->ifindex_set = 1;
    return 0;
}

static int set_usec_initialized(struct udev_device *device, unsigned long long when) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%llu", when);
    int r = tudev_add_prop(device, "USEC_INITIALIZED", buf);
    if (r < 0) return r;
    return 0;
}

static char *sysattr(udev_device *device, const char *name) {
    char *path = tudev_path_join(device->syspath, name);
    if (!path) return NULL;
    if (tudev_streq(name, "driver") || tudev_streq(name, "subsystem") ||
        tudev_streq(name, "module")) {
        char *value = tudev_readlink_base(path);
        if (value) {
            free(path);
            return value;
        }
        if (errno != EINVAL) {
            int error = errno;
            free(path);
            errno = error;
            return NULL;
        }
    }
    char *value = tudev_read_file(path, NULL);
    int error = errno;
    free(path);
    errno = error;
    return value;
}

static int set_drivers_subsystem(struct udev_device *device) {
    const char *devpath = device->devpath;
    const char *mark = strstr(devpath, "/drivers/");
    if (!mark) {
        size_t len = strlen(devpath);
        const char *suffix = "/drivers";
        size_t slen = strlen(suffix);
        if (len >= slen && strcmp(devpath + len - slen, suffix) == 0)
            mark = devpath + len - slen;
    }
    if (!mark) return -EINVAL;
    const char *end = mark;
    while (end > devpath && end[-1] != '/') end--;
    const char *start = end;
    while (start > devpath && start[-1] != '/') start--;
    if (start == end) return -EINVAL;
    char *sub = tudev_strndup(start, (size_t)(end - start));
    if (!sub) return -ENOMEM;
    int r = set_subsystem(device, "drivers");
    if (r < 0) {
        free(sub);
        return r;
    }
    free(device->driver_sub);
    device->driver_sub = sub;
    return 0;
}

static int uevent_line(udev_device *device, const char *key, const char *value,
                       const char **major, const char **minor) {
    if (strcmp(key, "SUBSYSTEM") == 0) return set_subsystem(device, value);
    if (strcmp(key, "DEVTYPE") == 0) return set_devtype(device, value);
    if (strcmp(key, "IFINDEX") == 0) return set_ifindex(device, value);
    if (strcmp(key, "DEVNAME") == 0) return set_devname(device, value);
    if (strcmp(key, "DEVUID") == 0) {
        unsigned ignored;
        if (parse_uint(value, &ignored) < 0) return -EINVAL;
        return tudev_add_prop(device, "DEVUID", value);
    }
    if (strcmp(key, "DEVGID") == 0) {
        unsigned ignored;
        if (parse_uint(value, &ignored) < 0) return -EINVAL;
        return tudev_add_prop(device, "DEVGID", value);
    }
    if (strcmp(key, "DEVMODE") == 0) {
        unsigned mode;
        if (parse_uint(value, &mode) < 0 || mode > 07777) return -EINVAL;
        return tudev_add_prop(device, "DEVMODE", value);
    }
    if (strcmp(key, "DISKSEQ") == 0) {
        unsigned long long seq;
        if (parse_u64(value, &seq) < 0 || seq == 0) return -EINVAL;
        return tudev_add_prop(device, "DISKSEQ", value);
    }
    if (strcmp(key, "DRIVER") == 0) return set_driver(device, value);
    if (strcmp(key, "MAJOR") == 0) {
        *major = value;
        return 0;
    }
    if (strcmp(key, "MINOR") == 0) {
        *minor = value;
        return 0;
    }
    return tudev_add_prop(device, key, value);
}

static int read_uevent(udev_device *device) {
    if (device->uevent_loaded || device->sealed) return 0;
    device->uevent_loaded = 1;
    char *text = sysattr(device, "uevent");
    if (!text) return 0;
    const char *major = NULL, *minor = NULL;
    char *line = text;
    while (line && *line) {
        char *end = strchr(line, '\n');
        if (end) *end = 0;
        char *eq = strchr(line, '=');
        if (eq) {
            *eq = 0;
            uevent_line(device, line, eq + 1, &major, &minor);
        }
        line = end ? end + 1 : NULL;
    }
    if (major) set_devnum(device, major, minor);
    if (device->subsystem && strcmp(device->subsystem, "drivers") == 0)
        set_drivers_subsystem(device);
    free(text);
    return 0;
}

static int add_tag(udev_device *device, const char *tag, int current) {
    if (!filename_valid(tag)) return -EINVAL;
    for (const char *p = tag; *p; p++) {
        char c = *p;
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
              (c >= '0' && c <= '9') || c == '-' || c == '_'))
            return -EINVAL;
    }
    int r = 0;
    if (!tudev_strv_has(device->tags, device->ntags, tag))
        r = tudev_strv_push(&device->tags, &device->ntags, &device->ctags, tag);
    if (r < 0) return r;
    if (current) {
        if (!tudev_strv_has(device->current_tags, device->ncurrent, tag)) {
            r = tudev_strv_push(&device->current_tags, &device->ncurrent, &device->ccurrent, tag);
            if (r < 0) return r;
        }
    }
    device->tags_dirty = 1;
    return 0;
}

static int add_devlink(udev_device *device, const char *devlink) {
    char *mangled = NULL;
    int r = mangle_devname(devlink, &mangled);
    if (r < 0) return r;
    if (!tudev_strv_has(device->devlinks, device->ndevlinks, mangled))
        r = tudev_strv_push(&device->devlinks, &device->ndevlinks, &device->cdevlinks, mangled);
    free(mangled);
    if (r < 0) return r;
    device->devlinks_dirty = 1;
    return 0;
}

int tudev_add_tag(struct udev_device *device, const char *tag, int current) {
    return add_tag(device, tag, current);
}

int tudev_add_devlink(struct udev_device *device, const char *devlink) {
    return add_devlink(device, devlink);
}

static char *devpath_filename(const char *devpath) {
    const char *slash = strrchr(devpath, '/');
    if (!slash || slash[1] == 0) return NULL;
    return strdup(slash + 1);
}

static char *device_id(udev_device *device) {
    dev_t devnum;
    int index;
    char *id = NULL;
    if (udev_device_get_devnum(device) != makedev(0, 0)) {
        devnum = udev_device_get_devnum(device);
        const char *subsystem = tudev_device_subsystem(device);
        char type = subsystem && strcmp(subsystem, "block") == 0 ? 'b' : 'c';
        if (asprintf(&id, "%c%u:%u", type, major(devnum), minor(devnum)) < 0) return NULL;
    } else if (tudev_device_has_ifindex(device, &index) == 0 && index > 0) {
        if (asprintf(&id, "n%u", (unsigned)index) < 0) return NULL;
    } else {
        char *name = devpath_filename(device->devpath);
        if (!name) return NULL;
        const char *subsystem = tudev_device_subsystem(device);
        if (!subsystem) {
            free(name);
            return NULL;
        }
        if (strcmp(subsystem, "drivers") == 0) {
            if (asprintf(&id, "+drivers:%s:%s", device->driver_sub ? device->driver_sub : "",
                         name) < 0)
                id = NULL;
        } else if (asprintf(&id, "+%s:%s", subsystem, name) < 0)
            id = NULL;
        free(name);
    }
    if (!id) return NULL;
    if (!filename_valid(id) || strchr(id, '/')) {
        free(id);
        return NULL;
    }
    return id;
}

static int db_line(udev_device *device, char key, const char *value) {
    switch (key) {
    case 'G':
    case 'Q':
        return add_tag(device, value, key == 'Q');
    case 'S': {
        char *path = tudev_path_join("/dev", value);
        if (!path) return -ENOMEM;
        int r = add_devlink(device, path);
        free(path);
        return r;
    }
    case 'E': {
        char *copy = strdup(value);
        if (!copy) return -ENOMEM;
        char *eq = strchr(copy, '=');
        if (!eq) {
            free(copy);
            return -EINVAL;
        }
        *eq = 0;
        const char *val = eq + 1;
        if (!*val) val = NULL;
        int r = tudev_add_prop(device, copy, val);
        free(copy);
        return r;
    }
    case 'I': {
        unsigned long long when;
        if (parse_u64(value, &when) < 0) return -EINVAL;
        return set_usec_initialized(device, when);
    }
    case 'L':
        return 0;
    case 'W':
        return 0;
    case 'V':
        return 0;
    default:
        return 0;
    }
}

int tudev_read_db(udev_device *device) {
    if (device->db_loaded) return 0;
    if (device->sealed) return 0;
    char *id = device_id(device);
    if (!id) return -ENOENT;
    char *path = NULL;
    if (asprintf(&path, "/run/udev/data/%s", id) < 0) {
        free(id);
        return -ENOMEM;
    }
    free(id);
    size_t length = 0;
    char *db = tudev_read_file(path, &length);
    int error = errno;
    free(path);
    if (!db) return error == ENOENT ? 0 : -error;
    device->is_initialized = 1;
    device->db_loaded = 1;
    enum { PRE_KEY, KEY, PRE_VALUE, VALUE, INVALID_LINE } state = PRE_KEY;
    char key = 0;
    const char *value = NULL;
    for (size_t i = 0; i < length; i++) {
        char c = db[i];
        switch (state) {
        case PRE_KEY:
            if (c != '\n') {
                key = c;
                state = KEY;
            }
            break;
        case KEY:
            if (c != ':') state = INVALID_LINE;
            else {
                db[i] = 0;
                state = PRE_VALUE;
            }
            break;
        case PRE_VALUE:
            value = &db[i];
            state = VALUE;
            break;
        case INVALID_LINE:
            if (c == '\n') state = PRE_KEY;
            break;
        case VALUE:
            if (c == '\n') {
                db[i] = 0;
                db_line(device, key, value);
                state = PRE_KEY;
            }
            break;
        }
    }
    free(db);
    return 0;
}

static int join_strv(char **v, size_t n, const char *separator, int wrap, char **ret) {
    size_t sep_len = strlen(separator), len = 0;
    for (size_t i = 0; i < n; i++)
        len += strlen(v[i]) + (i ? sep_len : 0);
    if (wrap) len += sep_len;
    char *s = malloc(len + 1);
    if (!s) return -ENOMEM;
    size_t off = 0;
    for (size_t i = 0; i < n; i++) {
        if (i) {
            memcpy(s + off, separator, sep_len);
            off += sep_len;
        }
        size_t l = strlen(v[i]);
        memcpy(s + off, v[i], l);
        off += l;
    }
    if (wrap) {
        memcpy(s + off, separator, sep_len);
        off += sep_len;
    }
    s[off] = 0;
    *ret = s;
    return 0;
}

int tudev_prepare(udev_device *device) {
    int r = read_uevent(device);
    if (r < 0) return r;
    r = tudev_read_db(device);
    if (r < 0) return r;
    if (device->devlinks_dirty) {
        if (device->ndevlinks > 0) {
            char *joined = NULL;
            r = join_strv(device->devlinks, device->ndevlinks, " ", 0, &joined);
            if (r < 0) return r;
            r = tudev_add_prop(device, "DEVLINKS", joined);
            free(joined);
            if (r < 0) return r;
        }
        device->devlinks_dirty = 0;
    }
    if (device->tags_dirty) {
        if (device->ntags > 0) {
            char *joined = NULL;
            r = join_strv(device->tags, device->ntags, ":", 1, &joined);
            if (r < 0) return r;
            r = tudev_add_prop(device, "TAGS", joined);
            free(joined);
            if (r < 0) return r;
        }
        if (device->ncurrent > 0) {
            char *joined = NULL;
            r = join_strv(device->current_tags, device->ncurrent, ":", 1, &joined);
            if (r < 0) return r;
            r = tudev_add_prop(device, "CURRENT_TAGS", joined);
            free(joined);
            if (r < 0) return r;
        }
        device->tags_dirty = 0;
    }
    return 0;
}

const char *tudev_device_subsystem(udev_device *device) {
    read_uevent(device);
    if (!device->subsystem_set) {
        char *value = sysattr(device, "subsystem");
        if (value) {
            set_subsystem(device, value);
            free(value);
        } else if (path_starts(device->devpath, "/module/") && device->devpath[8])
            set_subsystem(device, "module");
        else if (strstr(device->devpath, "/drivers/") ||
                 (strlen(device->devpath) >= 8 &&
                  strcmp(device->devpath + strlen(device->devpath) - 8, "/drivers") == 0))
            set_drivers_subsystem(device);
        else if (path_starts(device->devpath, "/class/") ||
                 path_starts(device->devpath, "/bus/"))
            set_subsystem(device, "subsystem");
        else
            set_subsystem(device, NULL);
    }
    return device->subsystem;
}

dev_t tudev_device_devnum(udev_device *device) {
    read_uevent(device);
    return device->devnum;
}

int tudev_device_has_ifindex(udev_device *device, int *ret) {
    read_uevent(device);
    if (device->ifindex <= 0) return -ENOENT;
    if (ret) *ret = device->ifindex;
    return 0;
}

void tudev_device_seal(udev_device *device) {
    device->sealed = 1;
}

static int set_action(udev_device *device, const char *action) {
    static const char *const actions[] = {
        "add", "remove", "change", "move", "online", "offline", "bind", "unbind",
    };
    int known = 0;
    for (size_t i = 0; i < sizeof(actions) / sizeof(actions[0]); i++)
        if (strcmp(action, actions[i]) == 0) known = 1;
    if (!known) return -EINVAL;
    int r = tudev_add_prop(device, "ACTION", action);
    if (r < 0) return r;
    char *copy = strdup(action);
    if (!copy) return -ENOMEM;
    free(device->action);
    device->action = copy;
    return 0;
}

static int nulstr_append(udev_device *device, char *entry, const char **major,
                         const char **minor) {
    char *eq = strchr(entry, '=');
    if (!eq) return -EINVAL;
    *eq = 0;
    const char *value = eq + 1;
    if (strcmp(entry, "DEVPATH") == 0) {
        char *path = tudev_path_join("/sys", value);
        if (!path) return -ENOMEM;
        int r = tudev_set_syspath(device, path, 0);
        free(path);
        return r;
    }
    if (strcmp(entry, "MAJOR") == 0) {
        *major = value;
        return 0;
    }
    if (strcmp(entry, "MINOR") == 0) {
        *minor = value;
        return 0;
    }
    if (strcmp(entry, "SUBSYSTEM") == 0) return set_subsystem(device, value);
    if (strcmp(entry, "DEVTYPE") == 0) return set_devtype(device, value);
    if (strcmp(entry, "DEVNAME") == 0) return set_devname(device, value);
    if (strcmp(entry, "USEC_INITIALIZED") == 0) {
        unsigned long long when;
        if (parse_u64(value, &when) < 0) return -EINVAL;
        return set_usec_initialized(device, when);
    }
    if (strcmp(entry, "DRIVER") == 0) return set_driver(device, value);
    if (strcmp(entry, "IFINDEX") == 0) return set_ifindex(device, value);
    if (strcmp(entry, "DEVMODE") == 0) {
        unsigned mode;
        if (parse_uint(value, &mode) < 0 || mode > 07777) return -EINVAL;
        return tudev_add_prop(device, "DEVMODE", value);
    }
    if (strcmp(entry, "DEVUID") == 0) {
        unsigned uid;
        if (parse_uint(value, &uid) < 0) return -EINVAL;
        return tudev_add_prop(device, "DEVUID", value);
    }
    if (strcmp(entry, "DEVGID") == 0) {
        unsigned gid;
        if (parse_uint(value, &gid) < 0) return -EINVAL;
        return tudev_add_prop(device, "DEVGID", value);
    }
    if (strcmp(entry, "ACTION") == 0) return set_action(device, value);
    if (strcmp(entry, "SEQNUM") == 0) {
        unsigned long long seq;
        if (parse_u64(value, &seq) < 0 || seq == 0) return -EINVAL;
        int r = tudev_add_prop(device, "SEQNUM", value);
        if (r < 0) return r;
        device->seqnum = seq;
        return 0;
    }
    if (strcmp(entry, "DISKSEQ") == 0) {
        unsigned long long seq;
        if (parse_u64(value, &seq) < 0 || seq == 0) return -EINVAL;
        return tudev_add_prop(device, "DISKSEQ", value);
    }
    if (strcmp(entry, "DEVLINKS") == 0) {
        const char *p = value;
        while (*p) {
            while (*p == ' ') p++;
            if (!*p) break;
            const char *start = p;
            while (*p && *p != ' ') p++;
            char *word = tudev_strndup(start, (size_t)(p - start));
            if (!word) return -ENOMEM;
            add_devlink(device, word);
            free(word);
        }
        return 0;
    }
    if (strcmp(entry, "TAGS") == 0 || strcmp(entry, "CURRENT_TAGS") == 0) {
        int current = strcmp(entry, "CURRENT_TAGS") == 0;
        const char *p = value;
        while (*p) {
            const char *start = p;
            while (*p && *p != ':') p++;
            if (p > start) {
                char *word = tudev_strndup(start, (size_t)(p - start));
                if (!word) return -ENOMEM;
                add_tag(device, word, current);
                free(word);
            }
            if (*p == ':') p++;
        }
        return 0;
    }
    if (strcmp(entry, "UDEV_DATABASE_VERSION") == 0) return 0;
    return tudev_add_prop(device, entry, value);
}

int tudev_device_from_nulstr(udev_device *device, char *nulstr, size_t len) {
    const char *major = NULL, *minor = NULL;
    size_t i = 0;
    while (i < len) {
        char *entry = nulstr + i;
        char *end = memchr(entry, 0, len - i);
        if (!end) return -EINVAL;
        i += (size_t)(end - entry) + 1;
        char *nl = strchr(entry, '\n');
        if (nl) *nl = 0;
        int r = nulstr_append(device, entry, &major, &minor);
        if (r < 0) return r;
    }
    if (major) {
        int r = set_devnum(device, major, minor);
        if (r < 0) return r;
    }
    if (!device->devpath || !device->subsystem || !device->action || device->seqnum == 0)
        return -EINVAL;
    if (strcmp(device->subsystem, "drivers") == 0) {
        int r = set_drivers_subsystem(device);
        if (r < 0) return r;
    }
    device->sealed = 1;
    return 0;
}

struct udev_device *udev_device_new_from_syspath(struct udev *udev, const char *syspath) {
    if (!udev || !syspath) {
        errno = EINVAL;
        return NULL;
    }
    if (!path_starts(syspath, "/sys/")) {
        errno = EINVAL;
        return NULL;
    }
    struct udev_device *device = tudev_device_new(udev);
    if (!device) {
        errno = ENOMEM;
        return NULL;
    }
    int r = tudev_set_syspath(device, syspath, 1);
    if (r < 0) {
        tudev_device_free(device);
        errno = -r;
        return NULL;
    }
    return device;
}

struct udev_device *udev_device_new_from_devnum(struct udev *udev, char type, dev_t devnum) {
    if (!udev || (type != 'b' && type != 'c')) {
        errno = EINVAL;
        return NULL;
    }
    if (major(devnum) == 0) {
        errno = ENODEV;
        return NULL;
    }
    char path[64];
    snprintf(path, sizeof(path), "/sys/dev/%s/%u:%u", type == 'b' ? "block" : "char",
             major(devnum), minor(devnum));
    struct udev_device *device = udev_device_new_from_syspath(udev, path);
    if (!device) return NULL;
    dev_t actual = udev_device_get_devnum(device);
    if (actual != devnum) {
        udev_device_unref(device);
        errno = ENXIO;
        return NULL;
    }
    const char *subsystem = tudev_device_subsystem(device);
    int is_block = subsystem && strcmp(subsystem, "block") == 0;
    if ((type == 'b') != is_block) {
        udev_device_unref(device);
        errno = ENXIO;
        return NULL;
    }
    return device;
}

struct udev *udev_device_get_udev(struct udev_device *device) {
    if (!device) {
        errno = EINVAL;
        return NULL;
    }
    return device->udev;
}

static char *parent_path(const char *path) {
    size_t len = strlen(path);
    while (len > 1 && path[len - 1] == '/') len--;
    while (len > 0 && path[len - 1] != '/') len--;
    if (len == 0) return NULL;
    while (len > 1 && path[len - 1] == '/') len--;
    return tudev_strndup(path, len);
}

struct udev_device *udev_device_get_parent(struct udev_device *device) {
    if (!device) {
        errno = EINVAL;
        return NULL;
    }
    if (!device->parent_set) {
        device->parent_set = 1;
        char *path = strdup(device->syspath);
        while (path) {
            char *parent = parent_path(path);
            free(path);
            path = NULL;
            if (!parent) break;
            if (strcmp(parent, "/sys") == 0 || strcmp(parent, "/") == 0) {
                free(parent);
                break;
            }
            struct udev_device *candidate = tudev_device_new(device->udev);
            if (candidate && tudev_set_syspath(candidate, parent, 1) == 0) {
                device->parent = candidate;
                free(parent);
                break;
            }
            udev_device_unref(candidate);
            path = parent;
        }
        free(path);
    }
    if (!device->parent) {
        errno = ENOENT;
        return NULL;
    }
    return device->parent;
}

struct udev_device *udev_device_get_parent_with_subsystem_devtype(struct udev_device *device,
        const char *subsystem, const char *devtype) {
    if (!device || !subsystem) {
        errno = EINVAL;
        return NULL;
    }
    for (;;) {
        device = udev_device_get_parent(device);
        if (!device) return NULL;
        const char *got = tudev_device_subsystem(device);
        if (!got || strcmp(got, subsystem) != 0) continue;
        if (devtype) {
            read_uevent(device);
            if (!device->devtype || strcmp(device->devtype, devtype) != 0) continue;
        }
        return device;
    }
}

const char *udev_device_get_devpath(struct udev_device *device) {
    if (!device) {
        errno = EINVAL;
        return NULL;
    }
    return device->devpath;
}

const char *udev_device_get_syspath(struct udev_device *device) {
    if (!device) {
        errno = EINVAL;
        return NULL;
    }
    return device->syspath;
}

const char *udev_device_get_subsystem(struct udev_device *device) {
    if (!device) {
        errno = EINVAL;
        return NULL;
    }
    const char *subsystem = tudev_device_subsystem(device);
    if (!subsystem) {
        errno = ENOENT;
        return NULL;
    }
    return subsystem;
}

const char *udev_device_get_devtype(struct udev_device *device) {
    if (!device) {
        errno = EINVAL;
        return NULL;
    }
    read_uevent(device);
    if (!device->devtype) {
        errno = ENOENT;
        return NULL;
    }
    return device->devtype;
}

static int ensure_sysname(udev_device *device) {
    if (device->sysname) return 0;
    char *name = devpath_filename(device->devpath);
    if (!name) return -EINVAL;
    for (char *p = name; *p; p++)
        if (*p == '!') *p = '/';
    size_t len = strlen(name), digits = 0;
    while (digits < len && name[len - 1 - digits] >= '0' && name[len - 1 - digits] <= '9')
        digits++;
    if (digits == len) digits = 0;
    device->sysname = name;
    device->sysnum = digits ? name + len - digits : NULL;
    return 0;
}

const char *udev_device_get_sysname(struct udev_device *device) {
    if (!device) {
        errno = EINVAL;
        return NULL;
    }
    if (ensure_sysname(device) < 0) {
        errno = EINVAL;
        return NULL;
    }
    return device->sysname;
}

const char *udev_device_get_sysnum(struct udev_device *device) {
    if (!device) {
        errno = EINVAL;
        return NULL;
    }
    if (ensure_sysname(device) < 0) {
        errno = EINVAL;
        return NULL;
    }
    if (!device->sysnum) {
        errno = ENOENT;
        return NULL;
    }
    return device->sysnum;
}

const char *udev_device_get_devnode(struct udev_device *device) {
    if (!device) {
        errno = EINVAL;
        return NULL;
    }
    read_uevent(device);
    if (!device->devname) {
        errno = ENOENT;
        return NULL;
    }
    return device->devname;
}

const char *udev_device_get_driver(struct udev_device *device) {
    if (!device) {
        errno = EINVAL;
        return NULL;
    }
    read_uevent(device);
    if (!device->driver_set) {
        char *value = sysattr(device, "driver");
        set_driver(device, value);
        free(value);
    }
    if (!device->driver) {
        errno = ENOENT;
        return NULL;
    }
    return device->driver;
}

dev_t udev_device_get_devnum(struct udev_device *device) {
    if (!device) {
        errno = EINVAL;
        return makedev(0, 0);
    }
    read_uevent(device);
    if (major(device->devnum) <= 0) {
        errno = ENOENT;
        return makedev(0, 0);
    }
    return device->devnum;
}

const char *udev_device_get_action(struct udev_device *device) {
    if (!device) {
        errno = EINVAL;
        return NULL;
    }
    if (!device->action) {
        errno = ENOENT;
        return NULL;
    }
    return device->action;
}

unsigned long long udev_device_get_seqnum(struct udev_device *device) {
    if (!device) {
        errno = EINVAL;
        return 0;
    }
    if (device->seqnum == 0) {
        errno = ENOENT;
        return 0;
    }
    return device->seqnum;
}

int udev_device_get_is_initialized(struct udev_device *device) {
    if (!device) {
        errno = EINVAL;
        return -EINVAL;
    }
    int r = tudev_read_db(device);
    if (r == -ENOENT) return 0;
    if (r < 0) {
        errno = -r;
        return -r;
    }
    return device->is_initialized;
}

const char *udev_device_get_property_value(struct udev_device *device, const char *key) {
    if (!device || !key) {
        errno = EINVAL;
        return NULL;
    }
    if (tudev_prepare(device) < 0) return NULL;
    const char *value = tudev_prop(device, key);
    if (!value) {
        errno = ENOENT;
        return NULL;
    }
    return value;
}

const char *udev_device_get_sysattr_value(struct udev_device *device, const char *sysattr_name) {
    if (!device || !sysattr_name) {
        errno = EINVAL;
        return NULL;
    }
    char *value = sysattr(device, sysattr_name);
    if (!value) return NULL;
    char **grown = realloc(device->sysattr_keys, (device->nsysattr + 1) * sizeof(char *));
    if (!grown) {
        free(value);
        errno = ENOMEM;
        return NULL;
    }
    device->sysattr_keys = grown;
    grown = realloc(device->sysattr_vals, (device->nsysattr + 1) * sizeof(char *));
    if (!grown) {
        free(value);
        errno = ENOMEM;
        return NULL;
    }
    device->sysattr_vals = grown;
    device->sysattr_keys[device->nsysattr] = strdup(sysattr_name);
    device->sysattr_vals[device->nsysattr] = value;
    device->nsysattr++;
    return value;
}

static int compare_string_ptr(const void *a, const void *b) {
    return strcmp(*(char *const *)a, *(char *const *)b);
}

static int prop_order_compare(const void *a, const void *b) {
    const struct udev_prop *pa = *(const struct udev_prop *const *)a;
    const struct udev_prop *pb = *(const struct udev_prop *const *)b;
    return strcmp(pa->key, pb->key);
}

struct udev_list_entry *udev_device_get_properties_list_entry(struct udev_device *device) {
    if (!device) {
        errno = EINVAL;
        return NULL;
    }
    if (tudev_prepare(device) < 0) return NULL;
    tudev_list_free(device->props_list);
    device->props_list = tudev_list_new(1);
    if (!device->props_list) return NULL;
    struct udev_prop **order = malloc((device->nprops + 1) * sizeof(*order));
    if (!order) return NULL;
    for (size_t i = 0; i < device->nprops; i++) order[i] = &device->props[i];
    if (device->nprops > 1)
        qsort(order, device->nprops, sizeof(*order), prop_order_compare);
    for (size_t i = 0; i < device->nprops; i++)
        tudev_list_add(device->props_list, order[i]->key, order[i]->value);
    free(order);
    return device->props_list->first;
}

struct udev_list_entry *udev_device_get_devlinks_list_entry(struct udev_device *device) {
    if (!device) {
        errno = EINVAL;
        return NULL;
    }
    tudev_prepare(device);
    tudev_list_free(device->devlinks_list);
    device->devlinks_list = tudev_list_new(1);
    if (!device->devlinks_list) return NULL;
    char **order = malloc((device->ndevlinks + 1) * sizeof(*order));
    if (!order) return NULL;
    for (size_t i = 0; i < device->ndevlinks; i++) order[i] = device->devlinks[i];
    if (device->ndevlinks > 1) qsort(order, device->ndevlinks, sizeof(*order),
                                     compare_string_ptr);
    for (size_t i = 0; i < device->ndevlinks; i++)
        tudev_list_add(device->devlinks_list, order[i], NULL);
    free(order);
    return device->devlinks_list->first;
}

static struct udev_list_entry *tag_list(udev_device *device, struct udev_list **list,
                                        char **tags, size_t n) {
    tudev_prepare(device);
    tudev_list_free(*list);
    *list = tudev_list_new(1);
    if (!*list) return NULL;
    char **order = malloc((n + 1) * sizeof(*order));
    if (!order) return NULL;
    for (size_t i = 0; i < n; i++) order[i] = tags[i];
    if (n > 1) qsort(order, n, sizeof(*order), compare_string_ptr);
    for (size_t i = 0; i < n; i++) tudev_list_add(*list, order[i], NULL);
    free(order);
    return (*list)->first;
}

struct udev_list_entry *udev_device_get_tags_list_entry(struct udev_device *device) {
    if (!device) {
        errno = EINVAL;
        return NULL;
    }
    return tag_list(device, &device->tags_list, device->tags, device->ntags);
}

struct udev_list_entry *udev_device_get_current_tags_list_entry(struct udev_device *device) {
    if (!device) {
        errno = EINVAL;
        return NULL;
    }
    return tag_list(device, &device->current_tags_list, device->current_tags, device->ncurrent);
}

int udev_device_has_tag(struct udev_device *device, const char *tag) {
    if (!device) return 0;
    tudev_prepare(device);
    return tudev_strv_has(device->tags, device->ntags, tag);
}

int udev_device_has_current_tag(struct udev_device *device, const char *tag) {
    if (!device) return 0;
    tudev_prepare(device);
    return tudev_strv_has(device->current_tags, device->ncurrent, tag);
}
