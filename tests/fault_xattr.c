#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <string.h>
#include <sys/types.h>
#include <sys/xattr.h>

static int is_opaque_xattr(const char *name) {
    return name && strcmp(name, "trusted.overlay.opaque") == 0;
}

int setxattr(const char *path, const char *name, const void *value, size_t size, int flags) {
    if (is_opaque_xattr(name)) {
        errno = EPERM;
        return -1;
    }
    int (*real)(const char *, const char *, const void *, size_t, int) =
        dlsym(RTLD_NEXT, "setxattr");
    return real(path, name, value, size, flags);
}

int lsetxattr(const char *path, const char *name, const void *value, size_t size, int flags) {
    if (is_opaque_xattr(name)) {
        errno = EPERM;
        return -1;
    }
    int (*real)(const char *, const char *, const void *, size_t, int) =
        dlsym(RTLD_NEXT, "lsetxattr");
    return real(path, name, value, size, flags);
}

int fsetxattr(int fd, const char *name, const void *value, size_t size, int flags) {
    if (is_opaque_xattr(name)) {
        errno = EPERM;
        return -1;
    }
    int (*real)(int, const char *, const void *, size_t, int) =
        dlsym(RTLD_NEXT, "fsetxattr");
    return real(fd, name, value, size, flags);
}
