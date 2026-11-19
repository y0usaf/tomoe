#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <xf86drm.h>

#include "drm-version-helper.c"

static int compare(const char *what, int fd) {
    drmVersionPtr want = drmGetVersion(fd);
    char *got = drm_driver_name(fd);
    const char *a = want ? want->name : "(null)", *b = got ? got : "(null)";
    int same = strcmp(a, b) == 0;
    printf("%s %s %s %s\n", same ? "match" : "DIFFER", what, a, b);
    if (want) drmFreeVersion(want);
    free(got);
    return same;
}

int main(int argc, char **argv) {
    int ok = compare("invalid", -1);
    int null = open("/dev/null", O_RDWR | O_CLOEXEC);
    ok &= compare("/dev/null", null);
    close(null);
    ok &= compare("closed", null);
    int pipes[2];
    if (pipe(pipes) == 0) {
        ok &= compare("pipe", pipes[0]);
        close(pipes[0]);
        close(pipes[1]);
    }
    int nodes = 0;
    for (int i = 1; i < argc; i++) {
        int fd = open(argv[i], O_RDWR | O_CLOEXEC);
        if (fd < 0) fd = open(argv[i], O_RDONLY | O_CLOEXEC);
        if (fd < 0) {
            printf("skip %s\n", argv[i]);
            continue;
        }
        ok &= compare(argv[i], fd);
        close(fd);
        nodes++;
    }
    printf("drm-version: %s on %d DRM nodes and 4 non-DRM descriptors\n", ok ? "identical" : "different", nodes);
    return ok ? 0 : 1;
}
