#ifndef TOMOE_DESKTOP_SESSION_H
#define TOMOE_DESKTOP_SESSION_H
#include <stdbool.h>
#include <stddef.h>

struct desktop_bus;
struct tomoe;

int desktop_runtime_directory(char *out, size_t size, char *error, size_t error_size);
bool desktop_socket_name_valid(const char *name);
int desktop_control_path(const char *name, char *out, size_t size, char *error, size_t error_size);
int desktop_json_path(const char *name, char *out, size_t size, char *error, size_t error_size);
int desktop_json_client_path(const char *explicit_name, char *out, size_t size, char *error,
    size_t error_size);
bool desktop_socket_answering(const char *path);
int desktop_connect_unix(const char *path, int timeout_seconds);
int desktop_free_x_display(char *out, size_t size, char *error, size_t error_size);
int desktop_parent_wayland_display(char *out, size_t size);
int desktop_primary_drm_devices(const char *path, char *out, size_t size, char *error,
    size_t error_size);
char *desktop_default_config(void);
struct desktop_bus *desktop_bus_start(struct tomoe *server, const char *name, char *error,
    size_t error_size);
void desktop_bus_service(struct desktop_bus *bus);
void desktop_bus_stop(struct desktop_bus *bus);
void desktop_session_start(void);
void desktop_session_stop(void);

#endif
