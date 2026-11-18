#include "harness.h"

#define NAMES_COUNT 36

static const char *names[NAMES_COUNT] = {
    "default", "context-menu", "help", "pointer", "progress", "wait", "cell",
    "crosshair", "text", "vertical-text", "alias", "copy", "move", "no-drop",
    "not-allowed", "grab", "grabbing", "e-resize", "n-resize", "ne-resize",
    "nw-resize", "s-resize", "se-resize", "sw-resize", "w-resize", "ew-resize",
    "ns-resize", "nesw-resize", "nwse-resize", "col-resize", "row-resize",
    "all-scroll", "zoom-in", "zoom-out", "dnd-ask", "all-resize"
};

static const char *tomoe_binary;
static const char *client_binary;
static char root[4096];
static char runtime[4096];
static char log_path[4096];
static struct harness_env base;
static struct harness_env env;
static pid_t compositor;
static int compositor_running;
static struct harness_child client;
static int client_running;

static const char *rooted(const char *suffix) {
    static char buffers[4][4096];
    static int slot;
    char *buffer = buffers[slot++ % 4];
    harness_join(buffer, 4096, root, suffix);
    return buffer;
}

static void write_cursor_file(const char *path, unsigned tag) {
    unsigned fields[20] = {
        0x72756358, 16, 0x10000, 1,
        0xfffd0002, 24, 28,
        36, 0xfffd0002, 24, 1, 2, 2, 0, 0, 0,
        tag, tag, tag, tag
    };
    unsigned char data[sizeof fields];
    for (size_t index = 0; index < sizeof fields; index += 4) {
        unsigned value = fields[index / 4];
        data[index] = (unsigned char)(value & 0xff);
        data[index + 1] = (unsigned char)((value >> 8) & 0xff);
        data[index + 2] = (unsigned char)((value >> 16) & 0xff);
        data[index + 3] = (unsigned char)((value >> 24) & 0xff);
    }
    FILE *file = fopen(path, "wb");
    if (!file) harness_fail("cannot write a cursor theme file");
    if (fwrite(data, 1, sizeof data, file) != sizeof data) harness_fail("cannot write a cursor file");
    if (fclose(file) != 0) harness_fail("cannot write a cursor file");
}

static char *cli(const char *first, const char *second) {
    char *argv[6] = { (char *)tomoe_binary, "--socket", "pointer-check", (char *)first,
        (char *)second, NULL };
    struct harness_run run = harness_run(argv, env_items(&env));
    if (run.status != 0) {
        fprintf(stderr, "tomoe %s failed with %d\n%s\n", first, run.status,
                run.output ? run.output : "");
        harness_fail("the compositor command failed");
    }
    return run.output;
}

static void command(struct harness_child *child, const char *value, const char *expected) {
    char line[8192];
    char seen[16384] = "";
    harness_write_line(child, value);
    for (;;) {
        if (!harness_read_line(child, line, sizeof line))
            harness_fail("the pointer client exited during a command");
        if (strcmp(line, "ok") == 0) break;
        if (!line[0]) harness_fail("the pointer client exited during a command");
        if (seen[0]) strncat(seen, " | ", sizeof seen - strlen(seen) - 1);
        strncat(seen, line, sizeof seen - strlen(seen) - 1);
    }
    if (strcmp(seen, expected) != 0) {
        fprintf(stderr, "command %s answered \"%s\", wanted \"%s\"\n", value, seen, expected);
        harness_fail("the pointer client answered differently");
    }
}

struct cursor {
    char *text;
    int hidden;
    long long width;
    unsigned pixel;
    int have_pixel;
};

static struct cursor image(void) {
    struct cursor state = { 0 };
    state.text = cli("msg", "cursor");
    if (!json_boolean(state.text, "hidden", &state.hidden))
        harness_fail("the cursor state has no hidden flag");
    if (!json_integer(state.text, "width", &state.width))
        harness_fail("the cursor state has no width");
    char *joined = malloc(1 << 20);
    unsigned char *pixels = malloc(1 << 20);
    if (!joined || !pixels) harness_fail("out of memory");
    if (json_strings_joined(state.text, "image", joined, 1 << 20)) {
        size_t length = base64_decode(joined, pixels, 1 << 20);
        if (length >= 4) {
            state.pixel = (unsigned)pixels[0] | (unsigned)pixels[1] << 8 |
                (unsigned)pixels[2] << 16 | (unsigned)pixels[3] << 24;
            state.have_pixel = 1;
        }
    }
    free(joined);
    free(pixels);
    return state;
}

static void inject(const char *request) {
    harness_write_file(rooted("gesture-command.tmp"), request);
    if (rename(rooted("gesture-command.tmp"), rooted("gesture-command")) != 0)
        harness_fail("cannot inject a gesture");
    free(cli("msg", "cursor"));
    free(cli("msg", "cursor"));
    if (harness_file_exists(rooted("gesture-command")))
        harness_fail("gesture injection was not consumed");
}

static void expect_pixel(unsigned wanted, const char *what) {
    struct cursor state = image();
    if (!state.have_pixel || state.pixel != wanted) {
        fprintf(stderr, "%s: pixel 0x%08x, wanted 0x%08x\n", what,
                state.have_pixel ? state.pixel : 0, wanted);
        harness_fail("the cursor image is not the expected one");
    }
    free(state.text);
}

static void ready(struct harness_child *child, const char *what) {
    char line[8192];
    for (;;) {
        if (!harness_read_line(child, line, sizeof line)) harness_fail(what);
        if (!line[0]) harness_fail(what);
        if (strcmp(line, "ready") == 0) return;
    }
}

static void cleanup(void) {
    if (client_running) {
        harness_terminate(client.pid);
        client_running = 0;
    }
    if (compositor_running && kill(compositor, 0) == 0) {
        char *argv[5] = { (char *)tomoe_binary, "--socket", "pointer-check", "quit", NULL };
        struct harness_run run = harness_run(argv, env_items(&env));
        free(run.output);
        if (harness_wait_status(compositor, 15) < 0) {
            kill(compositor, SIGTERM);
            harness_wait_status(compositor, 15);
        }
    }
    compositor_running = 0;
}

int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr, "usage: %s TOMOE CLIENT\n", argv[0]);
        return 2;
    }
    tomoe_binary = argv[1];
    client_binary = argv[2];
    harness_tmpdir(root, sizeof root);
    harness_join(runtime, sizeof runtime, root, "runtime");
    harness_join(log_path, sizeof log_path, root, "compositor.log");
    harness_mkparents(runtime, 0700);
    chmod(runtime, 0700);
    char cursors[4096];
    snprintf(cursors, sizeof cursors, "%s", root);
    harness_concat(cursors, sizeof cursors, cursors, "/icons/check/cursors");
    harness_mkparents(cursors, 0755);
    for (int index = 1; index <= NAMES_COUNT; index++) {
        char path[4096];
        harness_join(path, sizeof path, cursors, names[index - 1]);
        write_cursor_file(path, 0xff000000u | (unsigned)index);
    }
    env_reset(&base);
    env_keep(&base, "PATH");
    env_keep(&base, "MESA_LOADER_DRIVER_OVERRIDE");
    env_keep(&base, "LIBGL_DRIVERS_PATH");
    env_keep(&base, "__EGL_VENDOR_LIBRARY_FILENAMES");
    env_keep(&base, "LD_LIBRARY_PATH");
    env_put(&base, "HOME", root);
    env_put(&base, "XDG_RUNTIME_DIR", runtime);
    char icon_path[4096];
    harness_join(icon_path, sizeof icon_path, root, "icons");
    env_put(&base, "XCURSOR_PATH", icon_path);
    env_put(&base, "XCURSOR_THEME", "check");
    env_put(&base, "LIBGL_ALWAYS_SOFTWARE", "1");
    memset(&env, 0, sizeof env);
    for (int index = 0; index < base.count; index++) env.items[index] = strdup(base.items[index]);
    env.count = base.count;
    env.items[env.count] = NULL;
    atexit(cleanup);
    char gesture[8192], gesture_path[4096];
    harness_join(gesture_path, sizeof gesture_path, root, "gesture-command");
    harness_concat(gesture, sizeof gesture, "--test-gesture=", gesture_path);
    char *compositor_argv[] = { (char *)tomoe_binary, "--backend", "headless", "--bare",
        "--socket", "pointer-check", "--no-watch", gesture, NULL };
    compositor = harness_spawn(compositor_argv, env_items(&base), NULL, NULL, log_path, 0);
    compositor_running = 1;
    char socket_path[4096];
    harness_join(socket_path, sizeof socket_path, runtime, "pointer-check");
    int started = 0;
    for (int attempt = 0; attempt < 600; attempt++) {
        int status = harness_wait_status(compositor, 0);
        if (status >= 0) {
            compositor_running = 0;
            fprintf(stderr, "the compositor exited with %d\n%s\n", status, "");
            harness_fail("the private compositor did not start");
        }
        if (harness_file_exists(socket_path)) {
            started = 1;
            break;
        }
        harness_sleep(0.1);
    }
    if (!started) harness_fail("private compositor did not start");
    env_put(&env, "WAYLAND_DISPLAY", "pointer-check");
    {
        char *info_argv[] = { "wayland-info", NULL };
        struct harness_run info = harness_run(info_argv, env_items(&env));
        if (info.status != 0) harness_fail("wayland-info failed");
        if (!strstr(info.output, "wp_cursor_shape_manager_v1") ||
                !strstr(info.output, "zwp_pointer_gestures_v1")) {
            free(info.output);
            harness_fail("the private compositor lacks the cursor shape or gesture protocols");
        }
        free(info.output);
    }
    char *client_argv[] = { (char *)client_binary, NULL };
    client.pid = harness_spawn(client_argv, env_items(&env), &client.input, &client.output, NULL, 0);
    client_running = 1;
    ready(&client, "pointer client exited before enter");
    for (int index = 1; index <= NAMES_COUNT; index++) {
        char value[64];
        snprintf(value, sizeof value, "shape %d", index);
        command(&client, value, "");
        struct cursor state = image();
        if (!state.have_pixel || state.pixel != (0xff000000u | (unsigned)index)) {
            fprintf(stderr, "shape %d: pixel 0x%08x\n", index,
                    state.have_pixel ? state.pixel : 0);
            harness_fail("a themed shape was not applied");
        }
        if (state.hidden || state.width != 2) harness_fail("a themed shape hid the cursor");
        free(state.text);
    }
    command(&client, "stale 9", "");
    expect_pixel(0xff000024, "stale serial");
    command(&client, "destroy", "");
    expect_pixel(0xff000024, "destroyed shape device");
    command(&client, "hide", "");
    {
        struct cursor state = image();
        if (!state.hidden) harness_fail("an empty cursor did not hide the pointer");
        free(state.text);
    }
    command(&client, "shape 9", "");
    {
        struct cursor state = image();
        if (!state.have_pixel || state.pixel != 0xff000009 || state.hidden)
            harness_fail("a replacement shape device did not set the shape");
        free(state.text);
    }
    command(&client, "manager", "");
    command(&client, "shape 4", "");
    expect_pixel(0xff000004, "manager lifetime");
    {
        static const int kinds[3] = { 1, 2, 3 };
        static const char *const names[3] = { "swipe", "pinch", "hold" };
        static const int fingers[3] = { 3, 2, 1 };
        for (int index = 0; index < 3; index++) {
            char request[256], expected[256];
            snprintf(request, sizeof request, "(:begin %d 10 %d)", kinds[index],
                     fingers[index]);
            inject(request);
            snprintf(expected, sizeof expected, "%s begin 10 %d", names[index], fingers[index]);
            command(&client, "sync", expected);
            if (kinds[index] != 3) {
                snprintf(request, sizeof request, "(:update %d 11 2.5d0 -1.25d0 1.5d0 15d0)",
                         kinds[index]);
                inject(request);
                if (kinds[index] == 2)
                    snprintf(expected, sizeof expected, "%s update 11 2.500 -1.250 1.500 15.000",
                             names[index]);
                else
                    snprintf(expected, sizeof expected, "%s update 11 2.500 -1.250",
                             names[index]);
                command(&client, "sync", expected);
            }
            snprintf(request, sizeof request, "(:end %d 12 0)", kinds[index]);
            inject(request);
            snprintf(expected, sizeof expected, "%s end 0", names[index]);
            command(&client, "sync", expected);
        }
    }
    inject("(:update 1 20 1d0 2d0 1d0 0d0)");
    command(&client, "sync", "");
    inject("(:begin 1 21 3)");
    command(&client, "sync", "swipe begin 21 3");
    inject("(:other-update 1 22 1d0 2d0 1d0 0d0)");
    command(&client, "sync", "");
    inject("(:begin 2 23 2)");
    command(&client, "sync", "swipe end 1 | pinch begin 23 2");
    inject("(:end 2 24 1)");
    command(&client, "sync", "pinch end 1");
    inject("(:begin 1 30 3)");
    command(&client, "sync", "swipe begin 30 3");
    command(&client, "resubscribe", "");
    inject("(:update 1 31 1d0 2d0 1d0 0d0)");
    inject("(:end 1 32 0)");
    command(&client, "sync", "");
    inject("(:begin 3 40 2)");
    command(&client, "sync", "hold begin 40 2");
    inject("(:remove)");
    command(&client, "sync", "hold end 1");
    command(&client, "release-gestures", "");
    inject("(:begin 1 50 3)");
    command(&client, "sync", "swipe begin 50 3");
    struct harness_child second = { 0 };
    second.pid = harness_spawn(client_argv, env_items(&env), &second.input, &second.output,
                               NULL, 0);
    int second_running = 1;
    ready(&second, "second client failed to enter");
    command(&client, "sync", "swipe end 1");
    inject("(:update 1 51 1d0 2d0 1d0 0d0)");
    command(&second, "sync", "");
    inject("(:begin 2 52 2)");
    command(&second, "sync", "pinch begin 52 2");
    command(&client, "sync", "");
    command(&second, "unmap", "pinch end 1");
    inject("(:update 2 53 1d0 2d0 1d0 0d0)");
    command(&second, "sync", "");
    harness_write_line(&second, "quit");
    if (harness_wait_status(second.pid, 10) < 0) harness_fail("the second client did not quit");
    second_running = 0;
    (void)second_running;
    command(&client, "remap", "");
    inject("(:begin 3 60 1)");
    command(&client, "sync", "hold begin 60 1");
    command(&client, "unmap", "hold end 1");
    inject("(:stop)");
    harness_say("gestures: swipe/pinch/hold phases and values, cancelled overlap/device removal, "
                "late subscriptions, manager lifetime, focus isolation and loss passed");
    command(&client, "unmap", "");
    {
        struct cursor before = image();
        command(&client, "shape 9", "");
        struct cursor after = image();
        if (strcmp(before.text, after.text) != 0 || before.have_pixel != after.have_pixel ||
                (before.have_pixel && before.pixel != after.pixel))
            harness_fail("a shape on an unfocused client changed the cursor");
        free(before.text);
        free(after.text);
    }
    harness_write_line(&client, "shape 0");
    if (harness_wait_status(client.pid, 10) != 1)
        harness_fail("the pointer client did not reject an invalid shape");
    client_running = 0;
    if (harness_wait_status(compositor, 0) >= 0)
        harness_fail("the compositor exited after an invalid shape");
    harness_say("cursor: 36 themed shapes, serial rejection, surface/shape replacement, "
                "destruction, focus rejection, invalid-shape error passed");
    return 0;
}
