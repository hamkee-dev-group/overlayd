#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <string.h>
#include <sys/types.h>

static int targets_tmp_meta(const char *path) {
    if (!path) return 0;
    const char *last = strrchr(path, '/');
    if (!last || strcmp(last + 1, "meta") != 0) return 0;
    if (last == path) return 0;
    const char *p = last;
    while (p > path && *(p - 1) != '/') p--;
    return strncmp(p, ".tmp.", 5) == 0;
}

int open(const char *path, int flags, ...) {
    mode_t mode = 0;
    if (flags & O_CREAT) {
        va_list ap;
        va_start(ap, flags);
        mode = va_arg(ap, mode_t);
        va_end(ap);
    }
    if ((flags & (O_WRONLY | O_RDWR)) && targets_tmp_meta(path)) {
        errno = EIO;
        return -1;
    }
    int (*real)(const char *, int, ...) = dlsym(RTLD_NEXT, "open");
    return real(path, flags, mode);
}
