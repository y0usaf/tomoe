#include "sidecar.h"
#include "control.h"
#include "desktop-session.h"
#include "ipc.h"
#include "json.h"
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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
    if (lisp_init(64u << 20))
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

int policy_client(int argc, char **argv) {
    struct cli_options options = { .name = "tomoe-0", .backend = "auto", .watch = true };
    int index = 0;
    while (index < argc) {
        const char *option = argv[index++];
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
            const char *path = argv[index++];
            if (access(path, F_OK)) options.config_missing = true;
            else options.config = path;
        } else if (!strcmp(option, "--drm_device")) {
            if (index >= argc) return cli_error("--drm_device requires a value.");
            options.drm_device = argv[index++];
        } else if (!strcmp(option, "--render_node")) {
            if (index >= argc) return cli_error("--render_node requires a value.");
            options.render_node = argv[index++];
        } else if (!strncmp(option, "--test-", 7)) {
            continue;
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
    return cli_error("No command given.");
}
