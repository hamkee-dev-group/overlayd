#include "util.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/types.h>
#include <sys/xattr.h>
#include <unistd.h>

int g_verbose = 0;

void die(const char *fmt, ...) {
    va_list ap;
    fputs("overlayd: ", stderr);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    exit(1);
}

void warnx_(const char *fmt, ...) {
    va_list ap;
    fputs("overlayd: ", stderr);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

void info_(const char *fmt, ...) {
    if (!g_verbose) return;
    va_list ap;
    fputs("overlayd: ", stderr);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

int valid_name(const char *s) {
    if (!s || !*s) return 0;
    size_t n = strlen(s);
    if (n > OVERLAYD_NAME_MAX) return 0;
    if (s[0] == '.' && (n == 1 || (n == 2 && s[1] == '.'))) return 0;
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (!(isalnum((unsigned char)c) || c == '_' || c == '-' || c == '.'))
            return 0;
    }
    return 1;
}

int safe_for_overlay_opt(const char *s) {
    if (!s) return 0;
    for (; *s; s++) {
        if (*s == ':' || *s == ',' || *s == '\\' || *s == '"' || *s == '\n')
            return 0;
    }
    return 1;
}

void *xmalloc(size_t n) {
    void *p = malloc(n);
    if (!p) die("out of memory");
    return p;
}

void *xrealloc(void *p, size_t n) {
    void *q = realloc(p, n);
    if (!q) die("out of memory");
    return q;
}

char *xstrdup(const char *s) {
    char *r = strdup(s);
    if (!r) die("out of memory");
    return r;
}

int path_join(char *out, size_t outsz, const char *a, const char *b) {
    if (!a || !b) return -1;
    int r = snprintf(out, outsz, "%s/%s", a, b);
    if (r < 0 || (size_t)r >= outsz) return -1;
    return 0;
}

int path_join3(char *out, size_t outsz, const char *a, const char *b, const char *c) {
    int r = snprintf(out, outsz, "%s/%s/%s", a, b, c);
    if (r < 0 || (size_t)r >= outsz) return -1;
    return 0;
}

int path_exists(const char *path) {
    struct stat st;
    return lstat(path, &st) == 0;
}

int is_dir(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0) return 0;
    return S_ISDIR(st.st_mode);
}

int mkdir_p(const char *path, mode_t mode) {
    if (!path || !*path) return -1;
    char buf[4096];
    size_t n = strlen(path);
    if (n >= sizeof(buf)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    memcpy(buf, path, n + 1);
    for (size_t i = 1; i < n; i++) {
        if (buf[i] == '/') {
            buf[i] = 0;
            if (mkdir(buf, mode) != 0 && errno != EEXIST) return -1;
            buf[i] = '/';
        }
    }
    if (mkdir(buf, mode) != 0 && errno != EEXIST) return -1;
    return 0;
}

static int rm_rf_at(int dirfd) {
    DIR *d = fdopendir(dirfd);
    if (!d) {
        close(dirfd);
        return -1;
    }
    int rc = 0;
    struct dirent *de;
    while ((de = readdir(d))) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        struct stat st;
        if (fstatat(dirfd, de->d_name, &st, AT_SYMLINK_NOFOLLOW) != 0) {
            rc = -1;
            continue;
        }
        if (S_ISDIR(st.st_mode)) {
            if ((st.st_mode & 0700) != 0700) {
                fchmodat(dirfd, de->d_name, (st.st_mode & 07777) | 0700, 0);
            }
            int sub = openat(dirfd, de->d_name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
            if (sub < 0) {
                rc = -1;
                continue;
            }
            if (rm_rf_at(sub) != 0) rc = -1;
            if (unlinkat(dirfd, de->d_name, AT_REMOVEDIR) != 0) rc = -1;
        } else {
            if (unlinkat(dirfd, de->d_name, 0) != 0) rc = -1;
        }
    }
    closedir(d);
    return rc;
}

int rm_rf(const char *path) {
    struct stat st;
    if (lstat(path, &st) != 0) {
        if (errno == ENOENT) return 0;
        return -1;
    }
    if (!S_ISDIR(st.st_mode)) {
        return unlink(path);
    }
    int fd = open(path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
    if (fd < 0) return -1;
    int rc = rm_rf_at(fd);
    if (rmdir(path) != 0) rc = -1;
    return rc;
}

static int copy_xattrs(const char *src, const char *dst) {
    ssize_t sz = llistxattr(src, NULL, 0);
    if (sz < 0) {
        if (errno == ENOTSUP || errno == EOPNOTSUPP || errno == ENODATA) return 0;
        return -1;
    }
    if (sz == 0) return 0;
    char *list = xmalloc((size_t)sz);
    sz = llistxattr(src, list, (size_t)sz);
    if (sz < 0) {
        free(list);
        if (errno == ENOTSUP || errno == EOPNOTSUPP) return 0;
        return -1;
    }
    int rc = 0;
    for (ssize_t i = 0; i < sz;) {
        const char *name = list + i;
        size_t nlen = strlen(name);
        ssize_t vsz = lgetxattr(src, name, NULL, 0);
        if (vsz < 0) {
            if (errno == ENODATA) { i += nlen + 1; continue; }
            rc = -1;
            i += nlen + 1;
            continue;
        }
        char *val = xmalloc(vsz > 0 ? (size_t)vsz : 1);
        if (vsz > 0) {
            vsz = lgetxattr(src, name, val, (size_t)vsz);
            if (vsz < 0) {
                free(val);
                rc = -1;
                i += nlen + 1;
                continue;
            }
        }
        if (lsetxattr(dst, name, val, (size_t)vsz, 0) != 0) {
            if (errno != ENOTSUP && errno != EOPNOTSUPP && errno != EPERM)
                rc = -1;
        }
        free(val);
        i += nlen + 1;
    }
    free(list);
    return rc;
}

static int copy_file(const char *src, const char *dst, mode_t mode) {
    int sfd = open(src, O_RDONLY | O_NOFOLLOW);
    if (sfd < 0) return -1;
    int dfd = open(dst, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, mode & 07777);
    if (dfd < 0) {
        close(sfd);
        return -1;
    }
    char buf[65536];
    for (;;) {
        ssize_t r = read(sfd, buf, sizeof(buf));
        if (r == 0) break;
        if (r < 0) {
            if (errno == EINTR) continue;
            close(sfd);
            close(dfd);
            return -1;
        }
        ssize_t off = 0;
        while (off < r) {
            ssize_t w = write(dfd, buf + off, (size_t)(r - off));
            if (w < 0) {
                if (errno == EINTR) continue;
                close(sfd);
                close(dfd);
                return -1;
            }
            off += w;
        }
    }
    close(sfd);
    close(dfd);
    return 0;
}

int copy_tree(const char *src, const char *dst) {
    struct stat st;
    if (lstat(src, &st) != 0) return -1;
    if (S_ISDIR(st.st_mode)) {
        if (mkdir(dst, st.st_mode & 07777) != 0 && errno != EEXIST) return -1;
        DIR *d = opendir(src);
        if (!d) return -1;
        struct dirent *de;
        int rc = 0;
        while ((de = readdir(d))) {
            if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
            char sp[4096], dp[4096];
            if (path_join(sp, sizeof(sp), src, de->d_name) != 0 ||
                path_join(dp, sizeof(dp), dst, de->d_name) != 0) {
                rc = -1;
                continue;
            }
            if (copy_tree(sp, dp) != 0) rc = -1;
        }
        closedir(d);
        if (copy_xattrs(src, dst) != 0) rc = -1;
        return rc;
    } else if (S_ISREG(st.st_mode)) {
        if (copy_file(src, dst, st.st_mode) != 0) return -1;
        return copy_xattrs(src, dst);
    } else if (S_ISLNK(st.st_mode)) {
        char target[4096];
        ssize_t r = readlink(src, target, sizeof(target) - 1);
        if (r < 0) return -1;
        target[r] = 0;
        if (symlink(target, dst) != 0) return -1;
        return copy_xattrs(src, dst);
    } else if (S_ISCHR(st.st_mode) || S_ISBLK(st.st_mode) ||
               S_ISFIFO(st.st_mode) || S_ISSOCK(st.st_mode)) {
        if (mknod(dst, st.st_mode, st.st_rdev) != 0) return -1;
        return copy_xattrs(src, dst);
    }
    errno = EINVAL;
    return -1;
}

int write_file(const char *path, const char *buf, size_t len) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return -1;
    while (len > 0) {
        ssize_t w = write(fd, buf, len);
        if (w < 0) {
            if (errno == EINTR) continue;
            close(fd);
            return -1;
        }
        buf += w;
        len -= (size_t)w;
    }
    return close(fd);
}

int read_file(const char *path, char **buf_out, size_t *len_out) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    struct stat st;
    if (fstat(fd, &st) != 0) {
        close(fd);
        return -1;
    }
    size_t cap = (size_t)st.st_size + 1;
    char *buf = xmalloc(cap);
    size_t off = 0;
    for (;;) {
        if (off + 1 >= cap) {
            cap *= 2;
            buf = xrealloc(buf, cap);
        }
        ssize_t r = read(fd, buf + off, cap - off - 1);
        if (r == 0) break;
        if (r < 0) {
            if (errno == EINTR) continue;
            free(buf);
            close(fd);
            return -1;
        }
        off += (size_t)r;
    }
    buf[off] = 0;
    close(fd);
    *buf_out = buf;
    if (len_out) *len_out = off;
    return 0;
}

static int meta_load(const char *path, char ***keys_out, char ***vals_out, size_t *n_out) {
    char *buf = NULL;
    size_t len = 0;
    if (read_file(path, &buf, &len) != 0) {
        if (errno == ENOENT) {
            *keys_out = NULL;
            *vals_out = NULL;
            *n_out = 0;
            return 0;
        }
        return -1;
    }
    size_t cap = 8;
    char **keys = xmalloc(cap * sizeof(*keys));
    char **vals = xmalloc(cap * sizeof(*vals));
    size_t n = 0;
    char *p = buf;
    while (p && *p) {
        char *eol = strchr(p, '\n');
        if (eol) *eol = 0;
        if (*p) {
            char *eq = strchr(p, '=');
            if (eq) {
                *eq = 0;
                if (n == cap) {
                    cap *= 2;
                    keys = xrealloc(keys, cap * sizeof(*keys));
                    vals = xrealloc(vals, cap * sizeof(*vals));
                }
                keys[n] = xstrdup(p);
                vals[n] = xstrdup(eq + 1);
                n++;
            }
        }
        if (!eol) break;
        p = eol + 1;
    }
    free(buf);
    *keys_out = keys;
    *vals_out = vals;
    *n_out = n;
    return 0;
}

static void meta_free(char **keys, char **vals, size_t n) {
    for (size_t i = 0; i < n; i++) {
        free(keys[i]);
        free(vals[i]);
    }
    free(keys);
    free(vals);
}

static int meta_save(const char *path, char **keys, char **vals, size_t n) {
    size_t cap = 256;
    char *buf = xmalloc(cap);
    size_t off = 0;
    for (size_t i = 0; i < n; i++) {
        size_t need = strlen(keys[i]) + strlen(vals[i]) + 2;
        while (off + need >= cap) {
            cap *= 2;
            buf = xrealloc(buf, cap);
        }
        off += (size_t)snprintf(buf + off, cap - off, "%s=%s\n", keys[i], vals[i]);
    }
    int rc = write_file(path, buf, off);
    free(buf);
    return rc;
}

int meta_set(const char *path, const char *key, const char *value) {
    char **keys = NULL, **vals = NULL;
    size_t n = 0;
    if (meta_load(path, &keys, &vals, &n) != 0) return -1;
    int found = 0;
    for (size_t i = 0; i < n; i++) {
        if (!strcmp(keys[i], key)) {
            free(vals[i]);
            vals[i] = xstrdup(value);
            found = 1;
            break;
        }
    }
    if (!found) {
        keys = xrealloc(keys, (n + 1) * sizeof(*keys));
        vals = xrealloc(vals, (n + 1) * sizeof(*vals));
        keys[n] = xstrdup(key);
        vals[n] = xstrdup(value);
        n++;
    }
    int rc = meta_save(path, keys, vals, n);
    meta_free(keys, vals, n);
    return rc;
}

int meta_get(const char *path, const char *key, char *out, size_t outsz) {
    char **keys = NULL, **vals = NULL;
    size_t n = 0;
    if (meta_load(path, &keys, &vals, &n) != 0) return -1;
    int rc = -1;
    for (size_t i = 0; i < n; i++) {
        if (!strcmp(keys[i], key)) {
            size_t vlen = strlen(vals[i]);
            if (vlen + 1 > outsz) {
                errno = ERANGE;
                break;
            }
            memcpy(out, vals[i], vlen + 1);
            rc = 0;
            break;
        }
    }
    meta_free(keys, vals, n);
    if (rc != 0 && errno != ERANGE) errno = ENOENT;
    return rc;
}

int meta_has(const char *path, const char *key) {
    char tmp[64];
    return meta_get(path, key, tmp, sizeof(tmp)) == 0 || errno == ERANGE;
}

int meta_unset(const char *path, const char *key) {
    char **keys = NULL, **vals = NULL;
    size_t n = 0;
    if (meta_load(path, &keys, &vals, &n) != 0) return -1;
    size_t out = 0;
    for (size_t i = 0; i < n; i++) {
        if (!strcmp(keys[i], key)) {
            free(keys[i]);
            free(vals[i]);
        } else {
            keys[out] = keys[i];
            vals[out] = vals[i];
            out++;
        }
    }
    int rc = meta_save(path, keys, vals, out);
    for (size_t i = 0; i < out; i++) {
        free(keys[i]);
        free(vals[i]);
    }
    free(keys);
    free(vals);
    return rc;
}

int is_mountpoint(const char *path) {
    struct stat st, parent;
    if (lstat(path, &st) != 0) return 0;
    char buf[4096];
    int r = snprintf(buf, sizeof(buf), "%s/..", path);
    if (r < 0 || (size_t)r >= sizeof(buf)) return 0;
    if (lstat(buf, &parent) != 0) return 0;
    return st.st_dev != parent.st_dev;
}
