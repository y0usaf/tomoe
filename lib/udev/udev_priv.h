#ifndef TOMOE_UDEV_PRIV_H
#define TOMOE_UDEV_PRIV_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#include "libudev.h"

struct udev {
    unsigned n_ref;
    void *userdata;
};

struct udev_prop {
    char *key;
    char *value;
};

struct udev_list_entry {
    char *name;
    char *value;
    struct udev_list_entry *next;
};

struct udev_list {
    struct udev_list_entry *first;
    struct udev_list_entry *last;
    size_t n;
    int unique;
};

struct udev_device {
    struct udev *udev;
    unsigned n_ref;
    char *syspath;
    const char *devpath;
    char *sysname;
    const char *sysnum;
    char *subsystem;
    char *driver_sub;
    char *devtype;
    char *driver;
    char *devname;
    char *action;
    dev_t devnum;
    int ifindex;
    unsigned long long seqnum;
    struct udev_prop *props;
    size_t nprops;
    size_t cprops;
    unsigned long long generation;
    char **devlinks;
    size_t ndevlinks;
    size_t cdevlinks;
    char **tags;
    size_t ntags;
    size_t ctags;
    char **current_tags;
    size_t ncurrent;
    size_t ccurrent;
    int devlinks_dirty;
    int tags_dirty;
    int subsystem_set;
    int devtype_set;
    int driver_set;
    int ifindex_set;
    int uevent_loaded;
    int db_loaded;
    int is_initialized;
    int sealed;
    int parent_set;
    struct udev_device *parent;
    struct udev_list *props_list;
    struct udev_list *devlinks_list;
    struct udev_list *tags_list;
    struct udev_list *current_tags_list;
    char **sysattr_keys;
    char **sysattr_vals;
    size_t nsysattr;
};

typedef struct udev_device udev_device;

struct udev_monitor_filter {
    char *subsystem;
    char *devtype;
};

struct udev_monitor {
    struct udev *udev;
    unsigned n_ref;
    int fd;
    unsigned group;
    unsigned trusted_pid;
    struct udev_monitor_filter *filters;
    size_t nfilters;
    size_t cfilters;
};

struct udev_enumerate {
    struct udev *udev;
    unsigned n_ref;
    char **match;
    size_t nmatch;
    size_t cmatch;
    struct udev_device **devices;
    size_t ndevices;
    size_t cdevices;
    int scanned;
    struct udev_list *list;
};

void *tudev_new0(size_t size);
char *tudev_strdup(const char *s);
char *tudev_strndup(const char *s, size_t n);
int tudev_streq(const char *a, const char *b);
int tudev_streq_ptr(const char *a, const char *b);
char *tudev_read_file(const char *path, size_t *ret_len);
char *tudev_readlink_base(const char *path);
int tudev_utf8_valid(const char *s);
int tudev_has_cc(const char *s);
int tudev_strv_push(char ***v, size_t *n, size_t *cap, const char *s);
int tudev_strv_has(char **v, size_t n, const char *s);
int tudev_path_compare(const char *a, const char *b);
char *tudev_path_join(const char *a, const char *b);
void tudev_path_simplify(char *p);
int tudev_prop_valid(const char *key, const char *value);

struct udev_list *tudev_list_new(int unique);
void tudev_list_free(struct udev_list *list);
void tudev_list_clear(struct udev_list *list);
struct udev_list_entry *tudev_list_add(struct udev_list *list, const char *name, const char *value);

struct udev_device *tudev_device_new(struct udev *udev);
void tudev_device_free(struct udev_device *device);
int tudev_set_syspath(struct udev_device *device, const char *syspath, int verify);
int tudev_add_prop(struct udev_device *device, const char *key, const char *value);
const char *tudev_prop(struct udev_device *device, const char *key);
int tudev_prepare(struct udev_device *device);
int tudev_read_db(struct udev_device *device);
int tudev_add_tag(struct udev_device *device, const char *tag, int current);
int tudev_add_devlink(struct udev_device *device, const char *devlink);
const char *tudev_device_subsystem(struct udev_device *device);
dev_t tudev_device_devnum(struct udev_device *device);
int tudev_device_from_nulstr(struct udev_device *device, char *nulstr, size_t len);
void tudev_device_seal(struct udev_device *device);
int tudev_device_has_ifindex(struct udev_device *device, int *ret);

#endif
