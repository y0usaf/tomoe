#ifndef LIBEVDEV_H
#define LIBEVDEV_H

#include <linux/input.h>
#include <stdarg.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LIBEVDEV_ATTRIBUTE_PRINTF(_format, _args) __attribute__ ((format (printf, _format, _args)))

struct libevdev;

enum libevdev_read_flag {
    LIBEVDEV_READ_FLAG_SYNC = 1,
    LIBEVDEV_READ_FLAG_NORMAL = 2,
    LIBEVDEV_READ_FLAG_FORCE_SYNC = 4,
    LIBEVDEV_READ_FLAG_BLOCKING = 8
};

enum libevdev_log_priority {
    LIBEVDEV_LOG_ERROR = 10,
    LIBEVDEV_LOG_INFO = 20,
    LIBEVDEV_LOG_DEBUG = 30
};

typedef void (*libevdev_log_func_t)(enum libevdev_log_priority priority,
                                    void *data,
                                    const char *file, int line,
                                    const char *func,
                                    const char *format, va_list args)
    LIBEVDEV_ATTRIBUTE_PRINTF(6, 0);

typedef void (*libevdev_device_log_func_t)(const struct libevdev *dev,
                                           enum libevdev_log_priority priority,
                                           void *data,
                                           const char *file, int line,
                                           const char *func,
                                           const char *format, va_list args)
    LIBEVDEV_ATTRIBUTE_PRINTF(7, 0);

enum libevdev_grab_mode {
    LIBEVDEV_GRAB = 3,
    LIBEVDEV_UNGRAB = 4
};

enum libevdev_read_status {
    LIBEVDEV_READ_STATUS_SUCCESS = 0,
    LIBEVDEV_READ_STATUS_SYNC = 1
};

struct libevdev* libevdev_new(void);
int libevdev_new_from_fd(int fd, struct libevdev **dev);
void libevdev_free(struct libevdev *dev);
int libevdev_set_fd(struct libevdev* dev, int fd);
int libevdev_change_fd(struct libevdev* dev, int fd);
int libevdev_get_fd(const struct libevdev* dev);
int libevdev_grab(struct libevdev *dev, enum libevdev_grab_mode grab);

void libevdev_set_log_function(libevdev_log_func_t logfunc, void *data);
void libevdev_set_log_priority(enum libevdev_log_priority priority);
enum libevdev_log_priority libevdev_get_log_priority(void);
void libevdev_set_device_log_function(struct libevdev *dev,
                                      libevdev_device_log_func_t logfunc,
                                      enum libevdev_log_priority priority,
                                      void *data);

int libevdev_next_event(struct libevdev *dev, unsigned int flags, struct input_event *ev);

int libevdev_has_event_type(const struct libevdev *dev, unsigned int type);
int libevdev_has_event_code(const struct libevdev *dev, unsigned int type, unsigned int code);
int libevdev_has_property(const struct libevdev *dev, unsigned int prop);
int libevdev_enable_property(struct libevdev *dev, unsigned int prop);
int libevdev_disable_property(struct libevdev *dev, unsigned int prop);
int libevdev_enable_event_type(struct libevdev *dev, unsigned int type);
int libevdev_disable_event_type(struct libevdev *dev, unsigned int type);
int libevdev_enable_event_code(struct libevdev *dev, unsigned int type, unsigned int code, const void *data);
int libevdev_disable_event_code(struct libevdev *dev, unsigned int type, unsigned int code);

const char* libevdev_get_name(const struct libevdev *dev);
const char* libevdev_get_phys(const struct libevdev *dev);
const char* libevdev_get_uniq(const struct libevdev *dev);
void libevdev_set_name(struct libevdev *dev, const char *name);
void libevdev_set_phys(struct libevdev *dev, const char *phys);
void libevdev_set_uniq(struct libevdev *dev, const char *uniq);
int libevdev_get_id_product(const struct libevdev *dev);
int libevdev_get_id_vendor(const struct libevdev *dev);
int libevdev_get_id_bustype(const struct libevdev *dev);
int libevdev_get_id_version(const struct libevdev *dev);
void libevdev_set_id_product(struct libevdev *dev, int product_id);
void libevdev_set_id_vendor(struct libevdev *dev, int vendor_id);
void libevdev_set_id_bustype(struct libevdev *dev, int bustype);
void libevdev_set_id_version(struct libevdev *dev, int version);
int libevdev_get_driver_version(const struct libevdev *dev);
int libevdev_get_repeat(const struct libevdev *dev, int *delay, int *period);
int libevdev_set_clock_id(struct libevdev *dev, int clockid);

int libevdev_get_event_value(const struct libevdev *dev, unsigned int type, unsigned int code);
int libevdev_set_event_value(struct libevdev *dev, unsigned int type, unsigned int code, int value);
int libevdev_fetch_event_value(const struct libevdev *dev, unsigned int type, unsigned int code, int *value);

int libevdev_get_slot_value(const struct libevdev *dev, unsigned int slot, unsigned int code);
int libevdev_set_slot_value(struct libevdev *dev, unsigned int slot, unsigned int code, int value);
int libevdev_fetch_slot_value(const struct libevdev *dev, unsigned int slot, unsigned int code, int *value);
int libevdev_get_num_slots(const struct libevdev *dev);
int libevdev_get_current_slot(const struct libevdev *dev);

const struct input_absinfo* libevdev_get_abs_info(const struct libevdev *dev, unsigned int code);
int libevdev_get_abs_maximum(const struct libevdev *dev, unsigned int code);
int libevdev_get_abs_minimum(const struct libevdev *dev, unsigned int code);
int libevdev_get_abs_fuzz(const struct libevdev *dev, unsigned int code);
int libevdev_get_abs_flat(const struct libevdev *dev, unsigned int code);
int libevdev_get_abs_resolution(const struct libevdev *dev, unsigned int code);
void libevdev_set_abs_maximum(struct libevdev *dev, unsigned int code, int val);
void libevdev_set_abs_minimum(struct libevdev *dev, unsigned int code, int val);
void libevdev_set_abs_fuzz(struct libevdev *dev, unsigned int code, int val);
void libevdev_set_abs_flat(struct libevdev *dev, unsigned int code, int val);
void libevdev_set_abs_resolution(struct libevdev *dev, unsigned int code, int val);
void libevdev_set_abs_info(struct libevdev *dev, unsigned int code, const struct input_absinfo *abs);

int libevdev_event_is_type(const struct input_event *ev, unsigned int type);
int libevdev_event_is_code(const struct input_event *ev, unsigned int type, unsigned int code);

int libevdev_event_type_get_max(unsigned int type);
const char* libevdev_event_type_get_name(unsigned int type);
const char* libevdev_event_code_get_name(unsigned int type, unsigned int code);
const char* libevdev_property_get_name(unsigned int prop);
const char* libevdev_event_value_get_name(unsigned int type, unsigned int code, int value);
int libevdev_event_type_from_name(const char *name);
int libevdev_event_type_from_name_n(const char *name, size_t len);
int libevdev_event_code_from_name(unsigned int type, const char *name);
int libevdev_event_code_from_name_n(unsigned int type, const char *name, size_t len);
int libevdev_property_from_name(const char *name);
int libevdev_property_from_name_n(const char *name, size_t len);
int libevdev_event_value_from_name(unsigned int type, unsigned int code, const char *name);
int libevdev_event_value_from_name_n(unsigned int type, unsigned int code, const char *name, size_t len);

#ifdef __cplusplus
}
#endif

#endif
