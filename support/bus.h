#ifndef TOMOE_BUS_H
#define TOMOE_BUS_H

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

struct tomoe_bus;
struct tomoe_bus_message;
struct tomoe_bus_slot;

typedef struct tomoe_bus tomoe_bus;
typedef struct tomoe_bus_message tomoe_bus_message;
typedef struct tomoe_bus_slot tomoe_bus_slot;

#define TOMOE_BUS_STRUCT 'r'
#define TOMOE_BUS_DICT_ENTRY 'e'

struct tomoe_bus_method {
    const char *member;
    const char *signature;
    const char *result;
    int (*call)(struct tomoe_bus_message *message, void *userdata);
};

struct tomoe_bus_property {
    const char *name;
    const char *signature;
    int (*get)(struct tomoe_bus_message *reply, const char *property,
               void *userdata);
    size_t offset;
    int changing;
};

struct tomoe_bus_signal {
    const char *name;
    const char *signature;
};

int tomoe_bus_open_session(struct tomoe_bus **bus, uint64_t timeout_usec);
int tomoe_bus_open_system(struct tomoe_bus **bus, uint64_t timeout_usec);
int tomoe_bus_open_address(struct tomoe_bus **bus, const char *address,
                           uint64_t timeout_usec);
struct tomoe_bus *tomoe_bus_unref(struct tomoe_bus *bus);
void tomoe_bus_flush_close_unref(struct tomoe_bus *bus);
void tomoe_bus_close(struct tomoe_bus *bus);
int tomoe_bus_set_timeout(struct tomoe_bus *bus, uint64_t usec);
int tomoe_bus_process(struct tomoe_bus *bus);
int tomoe_bus_wait(struct tomoe_bus *bus, uint64_t usec);
int tomoe_bus_fd(const struct tomoe_bus *bus);
int tomoe_bus_events(const struct tomoe_bus *bus);
int tomoe_bus_flush(struct tomoe_bus *bus);
const char *tomoe_bus_unique_name(const struct tomoe_bus *bus);

int tomoe_bus_request_name(struct tomoe_bus *bus, const char *name);
int tomoe_bus_add_match(struct tomoe_bus *bus, struct tomoe_bus_slot **slot,
                        const char *rule,
                        int (*handle)(struct tomoe_bus_message *, void *),
                        void *userdata);
int tomoe_bus_add_object(struct tomoe_bus *bus, struct tomoe_bus_slot **slot,
                         const char *path, const char *interface,
                         const struct tomoe_bus_method *methods,
                         size_t method_count,
                         const struct tomoe_bus_property *properties,
                         size_t property_count,
                         const struct tomoe_bus_signal *signals,
                         size_t signal_count, void *userdata);
void tomoe_bus_slot_unref(struct tomoe_bus_slot *slot);
int tomoe_bus_emit_signal(struct tomoe_bus *bus, const char *path,
                          const char *interface, const char *member,
                          const char *signature, ...);
int tomoe_bus_emit_properties_changed(struct tomoe_bus *bus, const char *path,
                                      const char *interface, ...);
int tomoe_bus_call(struct tomoe_bus *bus, const char *destination,
                   const char *path, const char *interface,
                   const char *member, const char *signature, ...);
int tomoe_bus_call_async(struct tomoe_bus *bus, struct tomoe_bus_slot **slot,
                         const char *destination, const char *path,
                         const char *interface, const char *member,
                         int (*reply)(struct tomoe_bus_message *, void *),
                         void *userdata, const char *signature, ...);
int tomoe_bus_call_asyncv(struct tomoe_bus *bus, struct tomoe_bus_slot **slot,
                          const char *destination, const char *path,
                          const char *interface, const char *member,
                          int (*reply)(struct tomoe_bus_message *, void *),
                          void *userdata, const char *signature, va_list ap);
int tomoe_bus_send(struct tomoe_bus *bus, struct tomoe_bus_message *message);

struct tomoe_bus_message *tomoe_message_ref(struct tomoe_bus_message *message);
void tomoe_message_unref(struct tomoe_bus_message *message);
int tomoe_message_new_method_return(struct tomoe_bus_message *call,
                                    struct tomoe_bus_message **reply);
int tomoe_reply_method_return(struct tomoe_bus_message *call,
                              const char *signature, ...);
int tomoe_reply_method_error(struct tomoe_bus_message *call, const char *name,
                             const char *format, ...);
int tomoe_reply_method_errorv(struct tomoe_bus_message *call,
                              const char *name, const char *format,
                              va_list ap);
int tomoe_message_read(struct tomoe_bus_message *message,
                       const char *signature, ...);
int tomoe_message_read_array(struct tomoe_bus_message *message, char type,
                             const void **data, size_t *length);
int tomoe_message_enter_container(struct tomoe_bus_message *message, char type,
                                  const char *contents);
int tomoe_message_exit_container(struct tomoe_bus_message *message);
int tomoe_message_peek_type(struct tomoe_bus_message *message, char *type,
                            const char **contents);
int tomoe_message_skip(struct tomoe_bus_message *message,
                       const char *contents);
int tomoe_message_at_end(struct tomoe_bus_message *message, int complete);
int tomoe_message_open_container(struct tomoe_bus_message *message, char type,
                                 const char *contents);
int tomoe_message_close_container(struct tomoe_bus_message *message);
int tomoe_message_append(struct tomoe_bus_message *message,
                         const char *signature, ...);
const char *tomoe_message_sender(const struct tomoe_bus_message *message);
const char *tomoe_message_path(const struct tomoe_bus_message *message);
const char *tomoe_message_interface(const struct tomoe_bus_message *message);
const char *tomoe_message_member(const struct tomoe_bus_message *message);
const char *tomoe_message_error(const struct tomoe_bus_message *message);

int tomoe_bus_name_is_valid(const char *name);
int tomoe_bus_path_is_valid(const char *path);

#endif
