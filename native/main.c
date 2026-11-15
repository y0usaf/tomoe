#include "internal.h"

#include "backend.h"
#include "budget.h"
#include "control.h"
#include "desktop-session.h"
#include "ipc.h"
#include "json.h"
#include "lisp.h"
#include "policy.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/signalfd.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "lisp-sources.h"

enum { JSON_WIRE_VERSION = 3, CONTROL_WIRE_VERSION = 2 };

struct loop_runtime {
    bool quit;
    bool failed;
    char xwayland_display[8];
};

struct loop_source {
    struct wl_event_source *source;
    loop_fd_fn fd_fn;
    loop_timer_fn timer_fn;
    void *data;
};

static struct loop_runtime loop_state;

static int loop_fd_dispatch(int fd, uint32_t mask, void *data) {
    struct loop_source *source = data;
    source->fd_fn(source->data, fd, mask);
    return 0;
}

static int loop_timer_dispatch(void *data) {
    struct loop_source *source = data;
    source->timer_fn(source->data);
    return 0;
}

struct loop_source *loop_watch_fd(struct tomoe *server, int fd, uint32_t mask, loop_fd_fn fn,
        void *data) {
    if (!server || !server->display || fd < 0) return NULL;
    struct loop_source *source = calloc(1, sizeof(*source));
    if (!source) return NULL;
    source->fd_fn = fn;
    source->data = data;
    source->source = wl_event_loop_add_fd(wl_display_get_event_loop(server->display), fd,
        mask ? mask : WL_EVENT_READABLE, loop_fd_dispatch, source);
    if (!source->source) {
        free(source);
        return NULL;
    }
    return source;
}

void loop_update_fd(struct loop_source *source, uint32_t mask) {
    if (!source || !source->source) return;
    wl_event_source_fd_update(source->source, mask ? mask : WL_EVENT_READABLE);
}

void loop_set_data(struct loop_source *source, void *data) {
    if (source) source->data = data;
}

void loop_remove(struct loop_source *source) {
    if (!source) return;
    if (source->source) wl_event_source_remove(source->source);
    free(source);
}

struct loop_source *loop_timer(struct tomoe *server, loop_timer_fn fn, void *data) {
    if (!server || !server->display) return NULL;
    struct loop_source *source = calloc(1, sizeof(*source));
    if (!source) return NULL;
    source->timer_fn = fn;
    source->data = data;
    source->source = wl_event_loop_add_timer(wl_display_get_event_loop(server->display),
        loop_timer_dispatch, source);
    if (!source->source) {
        free(source);
        return NULL;
    }
    return source;
}

void loop_timer_arm(struct loop_source *source, uint64_t delay_ms) {
    if (!source || !source->source) return;
    int delay = delay_ms > (uint64_t)INT_MAX ? INT_MAX : (int)delay_ms;
    wl_event_source_timer_update(source->source, delay);
}

uint64_t loop_now_ms(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000u + (uint64_t)now.tv_nsec / 1000000u;
}

double loop_refresh_ms(struct tomoe *server) {
    int refresh = 0;
    if (server) {
        struct output *output;
        wl_list_for_each(output, &server->outputs, link) {
            if (output->screen && output->screen->refresh > refresh)
                refresh = output->screen->refresh;
        }
    }
    if (refresh <= 0) refresh = 60000;
    return 1000.0 / refresh;
}

void loop_request_quit(struct tomoe *server) {
    (void)server;
    loop_state.quit = true;
}

static lisp_value primitive_quit(int count, lisp_value *arguments) {
    (void)count;
    (void)arguments;
    loop_state.quit = true;
    return lisp_nil();
}

static lisp_value primitive_reload_requested(int count, lisp_value *arguments) {
    (void)count;
    (void)arguments;
    return lisp_nil();
}

static lisp_value primitive_xwayland_display(int count, lisp_value *arguments) {
    (void)count;
    (void)arguments;
    return lisp_cstring(loop_state.xwayland_display);
}

void loop_register_primitives(struct tomoe *server) {
    (void)server;
    lisp_define_primitive("%QUIT", primitive_quit, 0, 0);
    lisp_define_primitive("%RELOAD-REQUESTED", primitive_reload_requested, 0, 0);
    lisp_define_primitive("%XWAYLAND-DISPLAY", primitive_xwayland_display, 0, 0);
}

#ifndef TOMOE_PATH_PREFIX
#define TOMOE_PATH_PREFIX ""
#endif
#ifndef TOMOE_FONTCONFIG_FILE
#define TOMOE_FONTCONFIG_FILE ""
#endif
#ifndef TOMOE_VERSION
#define TOMOE_VERSION "dev"
#endif

static const char *version_string(void) {
    return TOMOE_VERSION;
}

static void usage(void) {
    fputs(
        "Usage: tomoe [--socket NAME] [--backend auto|nested|headless|drm] [--bare]\n"
        "       [--config FILE] [--watch|--no-watch] [--drm_device PATH]\n"
        "       tomoe [--socket NAME] commands\n"
        "       tomoe [--socket NAME] command NAME [ARGUMENT=VALUE ...]\n"
        "       tomoe [--socket NAME] inspect|reload|mount FILE|unmount NAME|event PLIST|hit-test X Y|frames|animation|memory|timings|quit\n"
        "       tomoe [--socket NAME] msg METHOD [JSON]\n"
        "\n"
        "Default socket: tomoe-0. Default backend: auto. winit and tty mean nested and drm.\n"
        "Auto nests in an existing Wayland display, or uses DRM when none is found.\n"
        "Nested mode discovers live wayland-N sockets when WAYLAND_DISPLAY is unset.\n"
        "Extensions are trusted Common Lisp programs. --bare omits all shipped policy.\n"
        "Loads $XDG_CONFIG_HOME/tomoe/init.lisp or ~/.config/tomoe/init.lisp when present.\n"
        "--config overrides that file; --bare skips it.\n"
        "Extension sources are watched and reloaded when edited; --no-watch disables that.\n"
        "commands lists every registered command with its arguments as JSON.\n"
        "command runs one by name, aiming it with ARGUMENT=VALUE pairs, and prints the\n"
        "values it ran with; every key binding runs the same command.\n"
        "patch applies a patch source file or forms to the running session, and\n"
        "undo-patch withdraws it, restoring the definitions it replaced and removing the\n"
        "timers, processes, surfaces and bindings it owned:\n"
        "  tomoe command patch file=/path/patch.lisp\n"
        "  tomoe command patch forms='(in-package #:tomoe-user) ...'\n"
        "  tomoe command undo-patch [name=patch-1|id=1]\n"
        "event sends one data plist to a live instance as an injected input event.\n"
        "hit-test reads one screen-space point from a live instance without changing it.\n"
        "frames reads each output's frame counter, last drawn frame, buffer age, drawn pixels and frame times.\n"
        "timings reads the latency samples each path keeps when --test-timings is set; timings reset clears them.\n"
        "animation reads the configured settle times and how animation frames were paced:\n"
        "moving frames, refreshes skipped between them, samples with no elapsed time, how\n"
        "long the first frame advanced the animation, how far ahead of the frame it\n"
        "sampled, and how the cursor trail's drawn steps were spaced.\n"
        "memory reads the Lisp heap and collector, the native heap, textures and output buffers.\n"
        "X11 clients connect through DISPLAY; Xwayland starts on the first connection and runs with Tomoe's own window manager.\n"
        "Control replies are versioned Lisp data. Mutating control commands are silent on success.\n",
        stdout);
}

static int cli_error(const char *format, ...) {
    va_list args;
    va_start(args, format);
    fprintf(stderr, "tomoe: ");
    vfprintf(stderr, format, args);
    fputc('\n', stderr);
    va_end(args);
    return 1;
}

struct cli_options {
    const char *name;
    bool explicit_name;
    const char *backend;
    bool bare;
    bool config_missing;
    const char *config;
    bool watch;
    const char *drm_device;
    const char *render_node;
};

static char *absolute_source(const char *path, char *error, size_t error_size) {
    char resolved[PATH_MAX];
    if (path[0] == '/') {
        snprintf(resolved, sizeof(resolved), "%s", path);
    } else {
        char directory[PATH_MAX];
        if (!getcwd(directory, sizeof(directory))) {
            snprintf(error, error_size, "Cannot resolve %s.", path);
            return NULL;
        }
        size_t directory_length = strlen(directory), path_length = strlen(path);
        if (directory_length + path_length + 2 > sizeof(resolved)) {
            snprintf(error, error_size, "Source path is too long.");
            return NULL;
        }
        memcpy(resolved, directory, directory_length);
        resolved[directory_length] = '/';
        memcpy(resolved + directory_length + 1, path, path_length + 1);
    }
    if (access(resolved, F_OK)) {
        snprintf(error, error_size, "No extension source at %s.", path);
        return NULL;
    }
    return strdup(resolved);
}

static int control_operation(const char *name, enum control_operation *operation) {
    static const struct {
        const char *name;
        enum control_operation operation;
    } operations[] = {
        { "inspect", CONTROL_INSPECT },
        { "reload", CONTROL_RELOAD },
        { "mount", CONTROL_MOUNT },
        { "unmount", CONTROL_UNMOUNT },
        { "event", CONTROL_EVENT },
        { "hit-test", CONTROL_HIT_TEST },
        { "frames", CONTROL_FRAMES },
        { "animation", CONTROL_ANIMATION },
        { "memory", CONTROL_MEMORY },
        { "timings", CONTROL_TIMINGS },
        { "quit", CONTROL_QUIT }
    };
    for (size_t i = 0; i < sizeof(operations) / sizeof(operations[0]); i++) {
        if (!strcmp(name, operations[i].name)) {
            *operation = operations[i].operation;
            return 0;
        }
    }
    return -1;
}

static int client_values_ready(void) {
    static bool ready;
    if (ready) return 0;
    if (lisp_init(budget_lisp_region_bootstrap))
        return cli_error("Cannot initialise the Lisp runtime.");
    ready = true;
    return 0;
}

static int client_status(int code, const char *error) {
    if (!code) return 0;
    if (error && *error) return cli_error("%s", error);
    return code;
}

static int client_verb_allowed(const struct cli_options *options, const char *verb) {
    if (options->config || options->bare || strcmp(options->backend, "auto"))
        return cli_error("Server options cannot be combined with %s.", verb);
    return 0;
}

static int run_compositor(const struct cli_options *options);

static int run_cli(int argc, char **argv) {
    struct cli_options options = { .name = "tomoe-0", .backend = "auto", .watch = true };
    int index = 0;
    while (index < argc) {
        const char *option = argv[index++];
        if (!strcmp(option, "-h") || !strcmp(option, "--help")) {
            usage();
            return 0;
        }
        if (!strcmp(option, "-V") || !strcmp(option, "--version")) {
            printf("tomoe %s, JSON wire %d, control wire %d, native ABI %d\n", version_string(),
                JSON_WIRE_VERSION, CONTROL_WIRE_VERSION, tomoe_abi_version());
            return 0;
        }
        if (!strcmp(option, "--socket")) {
            if (index >= argc) return cli_error("--socket requires a value.");
            options.name = argv[index++];
            options.explicit_name = true;
        } else if (!strcmp(option, "--backend")) {
            if (index >= argc) return cli_error("--backend requires a value.");
            const char *value = argv[index++];
            if (!strcmp(value, "winit")) options.backend = "nested";
            else if (!strcmp(value, "tty")) options.backend = "drm";
            else options.backend = value;
        } else if (!strcmp(option, "--config")) {
            if (index >= argc) return cli_error("--config requires a value.");
            char error[512];
            char *path = absolute_source(argv[index++], error, sizeof(error));
            if (!path) {
                fprintf(stderr, "tomoe: %s\n", error);
                options.config_missing = true;
            } else {
                options.config = path;
            }
        } else if (!strcmp(option, "--drm_device")) {
            if (index >= argc) return cli_error("--drm_device requires a value.");
            options.drm_device = argv[index++];
        } else if (!strcmp(option, "--render_node")) {
            if (index >= argc) return cli_error("--render_node requires a value.");
            options.render_node = argv[index++];
        } else if (!strncmp(option, "--test-", 7)) {
            const char *spec = option + 7;
            size_t length = strcspn(spec, "=");
            char name[128];
            if (length >= sizeof(name)) return cli_error("Unknown test flag: %s", option);
            memcpy(name, spec, length);
            name[length] = '\0';
            const char *value = spec[length] == '=' ? spec + length + 1 : NULL;
            if (!tomoe_set_test_flag(name, value))
                return cli_error("Unknown test flag: %s", option);
        } else if (!strcmp(option, "--bare")) {
            options.bare = true;
        } else if (!strcmp(option, "--watch")) {
            options.watch = true;
        } else if (!strcmp(option, "--no-watch")) {
            options.watch = false;
        } else if (!strcmp(option, "msg")) {
            int status = client_verb_allowed(&options, "msg");
            if (status) return status;
            int remaining = argc - index;
            if (remaining < 1 || remaining > 2)
                return cli_error("msg requires METHOD and at most one JSON argument.");
            if (client_values_ready()) return 1;
            const char *method = argv[index++];
            lisp_value params = lisp_nil();
            if (index < argc) {
                char error[512];
                if (json_parse(argv[index], strlen(argv[index]), false, &params, error,
                        sizeof(error)))
                    return cli_error("%s", error);
                index++;
            }
            char path[PATH_MAX], error[512];
            if (desktop_json_client_path(options.explicit_name ? options.name : NULL, path,
                    sizeof(path), error, sizeof(error)))
                return cli_error("%s", error);
            return client_status(ipc_client(path, method, params, error, sizeof(error)), error);
        } else if (!strcmp(option, "commands")) {
            int status = client_verb_allowed(&options, "commands");
            if (status) return status;
            if (index < argc) return cli_error("commands takes no arguments.");
            if (client_values_ready()) return 1;
            char path[PATH_MAX], error[512];
            if (desktop_json_client_path(options.explicit_name ? options.name : NULL, path,
                    sizeof(path), error, sizeof(error)))
                return cli_error("%s", error);
            return client_status(ipc_client(path, "commands", lisp_nil(), error,
                sizeof(error)), error);
        } else if (!strcmp(option, "command")) {
            int status = client_verb_allowed(&options, "command");
            if (status) return status;
            if (index >= argc) return cli_error("command requires a NAME.");
            if (client_values_ready()) return 1;
            const char *command = argv[index++];
            int pairs = argc - index;
            const char **keys = pairs ? calloc((size_t)pairs, sizeof(*keys)) : NULL;
            lisp_value *values = pairs ? calloc((size_t)pairs, sizeof(*values)) : NULL;
            if (pairs && (!keys || !values)) {
                free(keys);
                free(values);
                return cli_error("Cannot allocate the command arguments.");
            }
            for (int i = 0; i < pairs; i++) {
                const char *argument = argv[index + i];
                const char *equals = strchr(argument, '=');
                if (!equals || equals == argument) {
                    free(keys);
                    free(values);
                    return cli_error("command arguments are NAME=VALUE, not %s.", argument);
                }
                size_t length = (size_t)(equals - argument);
                char *key = malloc(length + 1);
                if (!key) {
                    free(keys);
                    free(values);
                    return cli_error("Cannot allocate the command arguments.");
                }
                memcpy(key, argument, length);
                key[length] = '\0';
                keys[i] = key;
                values[i] = lisp_cstring(equals + 1);
            }
            lisp_value arguments = json_build_object(pairs, keys, values);
            for (int i = 0; i < pairs; i++) free((void *)keys[i]);
            free(keys);
            free(values);
            const char *request_keys[2] = { "name", "arguments" };
            lisp_value request_values[2] = { lisp_cstring(command), arguments };
            lisp_value params = json_build_object(2, request_keys, request_values);
            char path[PATH_MAX], error[512];
            if (desktop_json_client_path(options.explicit_name ? options.name : NULL, path,
                    sizeof(path), error, sizeof(error)))
                return cli_error("%s", error);
            return client_status(ipc_client(path, "command", params, error, sizeof(error)),
                error);
        } else {
            enum control_operation operation;
            if (control_operation(option, &operation)) {
                return cli_error("Unknown option or command: %s", option);
            }
            int status = client_verb_allowed(&options, "a control command");
            if (status) return status;
            if (client_values_ready()) return 1;
            char path[PATH_MAX], error[512];
            if (desktop_control_path(options.name, path, sizeof(path), error, sizeof(error)))
                return cli_error("%s", error);
            return client_status(control_client(path, operation, argc - index, argv + index,
                error, sizeof(error)), error);
        }
    }
    if (!options.bare && !options.config && !options.config_missing)
        options.config = desktop_default_config();
    return run_compositor(&options);
}

static void session_signals_ready(void *data, int fd, uint32_t mask) {
    (void)data;
    (void)mask;
    struct signalfd_siginfo info;
    while (read(fd, &info, sizeof(info)) == (ssize_t)sizeof(info)) loop_state.quit = true;
}

static int session_signals_block(char *error, size_t error_size) {
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGTERM);
    sigaddset(&mask, SIGINT);
    if (sigprocmask(SIG_BLOCK, &mask, NULL) < 0) {
        snprintf(error, error_size, "Cannot block SIGTERM and SIGINT.");
        return -1;
    }
    return 0;
}

static int session_signals_start(struct tomoe *server, struct loop_source **watch, char *error,
        size_t error_size) {
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGTERM);
    sigaddset(&mask, SIGINT);
    int fd = signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC);
    if (fd < 0) {
        snprintf(error, error_size, "Cannot create the signal descriptor.");
        return -1;
    }
    *watch = loop_watch_fd(server, fd, WL_EVENT_READABLE, session_signals_ready, NULL);
    if (!*watch) {
        close(fd);
        snprintf(error, error_size, "Cannot wake the event loop for SIGTERM and SIGINT.");
        return -1;
    }
    return fd;
}

static void prepend_path(const char *extra) {
    const char *path = getenv("PATH");
    char combined[PATH_MAX];
    snprintf(combined, sizeof(combined), "%s%s%s", extra, path && *path ? ":" : "",
        path && *path ? path : "");
    setenv("PATH", combined, 1);
}

static int resolve_backend(const struct cli_options *options, char *backend, size_t size,
        char *error, size_t error_size) {
    snprintf(backend, size, "%s", options->backend);
    if (!strcmp(backend, "auto") || !strcmp(backend, "nested")) {
        char display[256];
        if (!desktop_parent_wayland_display(display, sizeof(display))) {
            setenv("WAYLAND_DISPLAY", display, 1);
            snprintf(backend, size, "nested");
        } else if (!strcmp(backend, "auto")) {
            snprintf(backend, size, "drm");
        } else {
            const char *directory = getenv("XDG_RUNTIME_DIR");
            snprintf(error, error_size,
                "No live Wayland display found in %s. Set WAYLAND_DISPLAY for a custom "
                "socket, or use --backend headless or drm.", directory ? directory : "");
            return -1;
        }
    }
    if (strcmp(backend, "nested") && strcmp(backend, "headless") && strcmp(backend, "drm")) {
        snprintf(error, error_size, "Unknown backend: %s", backend);
        return -1;
    }
    return 0;
}

static int run_compositor(const struct cli_options *options) {
    char error[1024];
    char control_path[PATH_MAX], json_path[PATH_MAX];
    if (desktop_control_path(options->name, control_path, sizeof(control_path), error,
            sizeof(error)))
        return cli_error("%s", error);
    if (desktop_json_path(options->name, json_path, sizeof(json_path), error, sizeof(error)))
        return cli_error("%s", error);
    if (desktop_socket_answering(control_path))
        return cli_error("Another session is listening on %s.", control_path);
    if (session_signals_block(error, sizeof(error))) return cli_error("%s", error);
    if (*TOMOE_PATH_PREFIX) prepend_path(TOMOE_PATH_PREFIX);
    if (*TOMOE_FONTCONFIG_FILE) setenv("FONTCONFIG_FILE", TOMOE_FONTCONFIG_FILE, 0);
    char backend[16];
    if (resolve_backend(options, backend, sizeof(backend), error, sizeof(error)))
        return cli_error("%s", error);
    tomoe_select_backend(backend);
    if (options->render_node) tomoe_select_render_node(options->render_node);
    if (options->drm_device) {
        char devices[PATH_MAX];
        if (desktop_primary_drm_devices(options->drm_device, devices, sizeof(devices), error,
                sizeof(error)))
            return cli_error("%s", error);
        tomoe_select_drm_devices(devices);
    }
    bool drm = !strcmp(backend, "drm");
    struct tomoe *server = tomoe_create(options->name);
    if (!server) return 1;
    int status = 1;
    struct control_server *control = NULL;
    struct ipc_server *ipc = NULL;
    struct desktop_bus *bus = NULL;
    bool resources = false;
    bool session = false;
    int signal_fd = -1;
    struct loop_source *signal_watch = NULL;
    if (lisp_init(budget_lisp_region_bootstrap)) {
        tomoe_log(LOG_ERROR, "tomoe: cannot initialise the Lisp runtime");
        goto done;
    }
    policy_register_primitives(server);
    resources_register_primitives(server);
    ui_layout_register_primitives(server);
    loop_register_primitives(server);
    if (lisp_load_builtins(tomoe_builtin_sources, tomoe_builtin_source_count, error,
            sizeof(error))) {
        tomoe_log(LOG_ERROR, "tomoe: %s", error);
        goto done;
    }
    signal_fd = session_signals_start(server, &signal_watch, error, sizeof(error));
    if (signal_fd < 0) {
        tomoe_log(LOG_ERROR, "tomoe: %s", error);
        goto done;
    }
    control = control_start(server, control_path, error, sizeof(error));
    if (!control) {
        tomoe_log(LOG_ERROR, "tomoe: %s", error);
        goto done;
    }
    ipc = ipc_start(server, json_path, error, sizeof(error));
    if (!ipc) {
        tomoe_log(LOG_ERROR, "tomoe: %s", error);
        goto done;
    }
    setenv("WAYLAND_DISPLAY", options->name, 1);
    setenv("TOMOE_SOCKET", json_path, 1);
    setenv("XDG_CURRENT_DESKTOP", "tomoe", 1);
    if (desktop_free_x_display(loop_state.xwayland_display,
            sizeof(loop_state.xwayland_display), error, sizeof(error))) {
        tomoe_log(LOG_ERROR, "tomoe: %s", error);
        goto done;
    }
    setenv("DISPLAY", loop_state.xwayland_display, 1);
    if (xwayland_start(server)) {
        tomoe_log(LOG_ERROR, "tomoe: cannot start the Xwayland bridge on %s",
            loop_state.xwayland_display);
        goto done;
    }
    bus = desktop_bus_start(options->name, error, sizeof(error));
    if (!bus && error[0])
        fprintf(stderr, "tomoe: session bus unavailable: %s\n", error);
    if (drm) {
        desktop_session_start();
        session = true;
    }
    if (resources_start(server))
        tomoe_log(LOG_ERROR, "tomoe: cannot start the native resources");
    resources = true;
    const char *outputs_error = tomoe_outputs_apply(server);
    if (outputs_error)
        tomoe_log(LOG_ERROR, "tomoe: %s", outputs_error);
    lisp_value sources = lisp_nil();
    if (options->config)
        sources = lisp_cons(lisp_cstring(options->config), lisp_nil());
    lisp_value boot_args[3] = { sources, lisp_bool(options->bare), lisp_bool(options->watch) };
    lisp_value boot_result = lisp_nil();
    lisp_set_limits(60000000, 5000000000ULL);
    if (policy_call(POLICY_HOOK_BOOT, 3, boot_args, &boot_result, error, sizeof(error)))
        tomoe_log(LOG_ERROR, "tomoe: %s", error);
    else if (lisp_stringp(boot_result)) {
        size_t length = 0;
        const char *message = lisp_get_string(boot_result, &length);
        if (message) tomoe_log(LOG_ERROR, "tomoe: %.*s", (int)length, message);
    }
    while (!loop_state.quit) {
        int timeout = policy_events_pending(server) ? 0 : -1;
        int step = tomoe_step(server, timeout);
        if (step < 0) {
            tomoe_log(LOG_ERROR, "tomoe: native event loop failed");
            loop_state.failed = true;
            break;
        }
        if (step > 0) break;
        tomoe_test_gestures(server);
        if (policy_events_pending(server)) {
            lisp_set_limits(2000000, 25000000ULL);
            policy_drain(server);
        }
        control_serve(control);
        ipc_serve(ipc);
        lisp_end_turn();
    }
    uint64_t deadline = loop_now_ms() + 100;
    while (ipc_pending(ipc) && loop_now_ms() < deadline) ipc_drain(ipc, 1);
    status = loop_state.failed ? 1 : 0;
done:
    if (resources) resources_stop(server);
    if (session) desktop_session_stop();
    desktop_bus_stop(bus, options->name);
    ipc_stop(ipc);
    control_stop(control);
    if (signal_watch) loop_remove(signal_watch);
    if (signal_fd >= 0) close(signal_fd);
    tomoe_destroy(server);
    lisp_shutdown();
    return status;
}

int main(int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1], "--warm-shaders")) {
        if (argc != 4) return 1;
        return tomoe_warm_shaders(argv[2], argv[3]);
    }
    return run_cli(argc - 1, argv + 1);
}
