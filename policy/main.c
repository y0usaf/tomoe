#include "sidecar.h"
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const char lisp_prelude[] = {
#embed "../lisp/prelude.lisp"
};
static const char lisp_api[] = {
#embed "../lisp/api.lisp"
};
static const char lisp_ui[] = {
#embed "../lisp/ui.lisp"
};
static const char lisp_cursor[] = {
#embed "../lisp/cursor.lisp"
};
static const char lisp_dialogs[] = {
#embed "../lisp/dialogs.lisp"
};
static const char lisp_rules[] = {
#embed "../lisp/rules.lisp"
};
static const char lisp_ipc[] = {
#embed "../lisp/ipc.lisp"
};
static const char lisp_plan[] = {
#embed "../lisp/plan.lisp"
};
static const char lisp_runtime[] = {
#embed "../lisp/runtime.lisp"
};
static const char lisp_livepatch[] = {
#embed "../lisp/livepatch.lisp"
};
static const char lisp_desktop[] = {
#embed "../lisp/desktop.lisp"
};
static const char lisp_default_config[] = {
#embed "../lisp/default-config.lisp"
};

static const struct lisp_builtin_source tomoe_builtin_sources[] = {
    { "prelude", lisp_prelude, sizeof(lisp_prelude) },
    { "api", lisp_api, sizeof(lisp_api) },
    { "ui", lisp_ui, sizeof(lisp_ui) },
    { "cursor", lisp_cursor, sizeof(lisp_cursor) },
    { "dialogs", lisp_dialogs, sizeof(lisp_dialogs) },
    { "rules", lisp_rules, sizeof(lisp_rules) },
    { "ipc", lisp_ipc, sizeof(lisp_ipc) },
    { "plan", lisp_plan, sizeof(lisp_plan) },
    { "runtime", lisp_runtime, sizeof(lisp_runtime) },
    { "livepatch", lisp_livepatch, sizeof(lisp_livepatch) },
    { "desktop", lisp_desktop, sizeof(lisp_desktop) },
    { "default-config", lisp_default_config, sizeof(lisp_default_config) },
};
static const size_t tomoe_builtin_source_count = sizeof(tomoe_builtin_sources) / sizeof(tomoe_builtin_sources[0]);

enum { POLICY_LISP_REGION = 256 << 20 };

static int argument_fd(int argc, char **argv, const char *name) {
    for (int i = 1; i + 1 < argc; i++)
        if (!strcmp(argv[i], name)) return atoi(argv[i + 1]);
    return -1;
}

int main(int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1], "--client")) return policy_client(argc - 2, argv + 2);
    int link = argument_fd(argc, argv, "--fd");
    int control_fd = argument_fd(argc, argv, "--control-fd");
    int json_fd = argument_fd(argc, argv, "--json-fd");
    if (link < 0) {
        fprintf(stderr, "tomoe-policy: run by tomoe, not by hand\n");
        return 2;
    }
    signal(SIGPIPE, SIG_IGN);
    if (lisp_init(POLICY_LISP_REGION)) {
        fprintf(stderr, "tomoe-policy: cannot initialise the Lisp runtime\n");
        return 1;
    }
    policy_register_proxies();
    policy_register_local();
    char error[1024];
    if (lisp_load_builtins(tomoe_builtin_sources, tomoe_builtin_source_count, error,
            sizeof(error))) {
        fprintf(stderr, "tomoe-policy: %s\n", error);
        return 1;
    }
    if (policy_link_open(link)) {
        fprintf(stderr, "tomoe-policy: cannot watch the compositor link\n");
        return 1;
    }
    struct control_server *control = NULL;
    struct ipc_server *ipc = NULL;
    bool adopted = false;
    while (!loop_quitting()) {
        bool more = policy_link_work();
        if (!adopted && policy_link_ready()) {
            control = control_fd >= 0 ? control_adopt(control_fd) : NULL;
            ipc = json_fd >= 0 ? ipc_adopt(json_fd) : NULL;
            adopted = true;
        }
        if (loop_dispatch(more ? 0 : -1) < 0) break;
        control_serve(control);
        ipc_serve(ipc);
        lisp_end_turn();
    }
    core_flush();
    uint64_t deadline = loop_now_ms() + 100;
    while (ipc_pending(ipc) && loop_now_ms() < deadline) ipc_drain(ipc, 1);
    ipc_stop(ipc);
    control_stop(control);
    lisp_shutdown();
    return 0;
}
