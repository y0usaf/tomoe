#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void style(Display *display, Window window, const char *name, const char *instance,
        const char *klass, unsigned long background) {
    XSetWindowAttributes attributes = { .background_pixel = background };
    XChangeWindowAttributes(display, window, CWBackPixel, &attributes);
    XStoreName(display, window, name);
    XClassHint hint = { .res_name = (char *)instance, .res_class = (char *)klass };
    XSetClassHint(display, window, &hint);
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: x11-client normal|override|transient\n");
        return 2;
    }
    Display *display = XOpenDisplay(NULL);
    if (!display) {
        fprintf(stderr, "cannot open display\n");
        return 1;
    }
    Window root = DefaultRootWindow(display);
    Window parent = XCreateSimpleWindow(display, root, 50, 50, 320, 200, 0, 0, 0xffffff);
    style(display, parent, "X11 Parent", "x11-parent", "X11Test", 0xffffff);
    XMapWindow(display, parent);
    if (strcmp(argv[1], "override") == 0) {
        XSetWindowAttributes attributes = { .override_redirect = True,
            .background_pixel = 0x00ff00 };
        Window menu = XCreateWindow(display, root, 300, 200, 120, 80, 0,
            CopyFromParent, InputOutput, CopyFromParent, CWOverrideRedirect | CWBackPixel,
            &attributes);
        style(display, menu, "X11 Override", "x11-override", "X11Test", 0x00ff00);
        XMapWindow(display, menu);
    } else if (strcmp(argv[1], "transient") == 0) {
        Window dialog = XCreateSimpleWindow(display, root, 400, 150, 180, 100, 0, 0, 0xff0000);
        style(display, dialog, "X11 Transient", "x11-transient", "X11Test", 0xff0000);
        XSetTransientForHint(display, dialog, parent);
        XMapWindow(display, dialog);
    } else if (strcmp(argv[1], "normal") != 0 && strcmp(argv[1], "closeable") != 0) {
        fprintf(stderr, "unknown mode %s\n", argv[1]);
        return 2;
    }
    Atom delete_window = XInternAtom(display, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(display, parent, &delete_window, 1);
    XFlush(display);
    for (;;) {
        XEvent event;
        XNextEvent(display, &event);
        if (event.type == ClientMessage && (Atom)event.xclient.data.l[0] == delete_window) {
            const char *mark = getenv("X11_CLIENT_MARK");
            if (mark) {
                FILE *file = fopen(mark, "w");
                if (file) fclose(file);
            }
            XCloseDisplay(display);
            return 0;
        }
        if (event.type == SelectionRequest) {
            XSelectionRequestEvent *request = &event.xselectionrequest;
            XSelectionEvent notify = { .type = SelectionNotify, .display = display,
                .requestor = request->requestor, .selection = request->selection,
                .target = request->target, .property = None, .time = request->time };
            XSendEvent(display, request->requestor, False, 0, (XEvent *)&notify);
            XFlush(display);
        }
    }
}
