#include "tar.h"
#include "util.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/types.h>
#include <sys/xattr.h>
#include <unistd.h>

#define TAR_BLOCK 512
#define TAR_PAYLOAD_MAX (1u << 20)

typedef struct {
    char name[100];
    char mode[8];
    char uid[8];
    char gid[8];
    char size[12];
    char mtime[12];
    char chksum[8];
    char typeflag;
    char linkname[100];
    char magic[6];
    char version[2];
    char uname[32];
    char gname[32];
    char devmajor[8];
    char devminor[8];
    char prefix[155];
    char pad[12];
} tar_hdr_t;

_Static_assert(sizeof(tar_hdr_t) == TAR_BLOCK, "tar header must be 512 bytes");

static void put_octal(char *dst, size_t n, uint64_t v) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%0*llo", (int)(n - 1), (unsigned long long)v);
    memcpy(dst, buf, n - 1);
    dst[n - 1] = 0;
}

static uint64_t get_octal(const char *src, size_t n) {
    uint64_t v = 0;
    size_t i = 0;
    while (i < n && (src[i] == ' ' || src[i] == '0')) i++;
    for (; i < n; i++) {
        char c = src[i];
        if (c < '0' || c > '7') break;
        v = (v << 3) | (uint64_t)(c - '0');
    }
    return v;
}

static void compute_chksum(tar_hdr_t *h) {
    memset(h->chksum, ' ', 8);
    unsigned sum = 0;
    const unsigned char *p = (const unsigned char *)h;
    for (size_t i = 0; i < TAR_BLOCK; i++) sum += p[i];
    snprintf(h->chksum, 8, "%06o", sum);
    h->chksum[6] = 0;
    h->chksum[7] = ' ';
}

static int write_all(int fd, const void *buf, size_t n) {
    const char *p = buf;
    while (n) {
        ssize_t w = write(fd, p, n);
        if (w < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        p += w;
        n -= (size_t)w;
    }
    return 0;
}

static int read_all(int fd, void *buf, size_t n) {
    char *p = buf;
    while (n) {
        ssize_t r = read(fd, p, n);
        if (r == 0) return 1;
        if (r < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        p += r;
        n -= (size_t)r;
    }
    return 0;
}

static int write_zero_block(int fd) {
    static char zeros[TAR_BLOCK];
    return write_all(fd, zeros, TAR_BLOCK);
}

static int set_name(tar_hdr_t *h, const char *name, int fd) {
    size_t n = strlen(name);
    if (n <= 100) {
        memcpy(h->name, name, n);
        return 0;
    }
    if (n <= 255) {
        for (size_t split = n - 1; split > 0; split--) {
            if (name[split] == '/') {
                size_t prefix_len = split;
                size_t name_len = n - split - 1;
                if (prefix_len <= 155 && name_len <= 100 && name_len > 0) {
                    memcpy(h->prefix, name, prefix_len);
                    memcpy(h->name, name + split + 1, name_len);
                    return 0;
                }
            }
        }
    }
    tar_hdr_t lh;
    memset(&lh, 0, sizeof(lh));
    memcpy(lh.name, "././@LongLink", 13);
    put_octal(lh.mode, 8, 0644);
    put_octal(lh.uid, 8, 0);
    put_octal(lh.gid, 8, 0);
    put_octal(lh.size, 12, n + 1);
    put_octal(lh.mtime, 12, 0);
    lh.typeflag = 'L';
    memcpy(lh.magic, "ustar ", 6);
    memcpy(lh.version, " ", 2);
    compute_chksum(&lh);
    if (write_all(fd, &lh, TAR_BLOCK) != 0) return -1;
    char block[TAR_BLOCK];
    memset(block, 0, sizeof(block));
    size_t off = 0;
    while (off < n + 1) {
        size_t take = (n + 1 - off > TAR_BLOCK) ? TAR_BLOCK : (n + 1 - off);
        memset(block, 0, sizeof(block));
        if (off < n) memcpy(block, name + off, take > n - off ? n - off : take);
        if (write_all(fd, block, TAR_BLOCK) != 0) return -1;
        off += take;
    }
    size_t store = (n > 100) ? 100 : n;
    memcpy(h->name, name, store);
    return 0;
}

static int set_linkname(tar_hdr_t *h, const char *target, int fd) {
    size_t n = strlen(target);
    if (n <= 100) {
        memcpy(h->linkname, target, n);
        return 0;
    }
    tar_hdr_t lh;
    memset(&lh, 0, sizeof(lh));
    memcpy(lh.name, "././@LongLink", 13);
    put_octal(lh.mode, 8, 0644);
    put_octal(lh.uid, 8, 0);
    put_octal(lh.gid, 8, 0);
    put_octal(lh.size, 12, n + 1);
    put_octal(lh.mtime, 12, 0);
    lh.typeflag = 'K';
    memcpy(lh.magic, "ustar ", 6);
    memcpy(lh.version, " ", 2);
    compute_chksum(&lh);
    if (write_all(fd, &lh, TAR_BLOCK) != 0) return -1;
    char block[TAR_BLOCK];
    size_t off = 0;
    while (off < n + 1) {
        size_t take = (n + 1 - off > TAR_BLOCK) ? TAR_BLOCK : (n + 1 - off);
        memset(block, 0, sizeof(block));
        if (off < n) memcpy(block, target + off, take > n - off ? n - off : take);
        if (write_all(fd, block, TAR_BLOCK) != 0) return -1;
        off += take;
    }
    size_t store = (n > 100) ? 100 : n;
    memcpy(h->linkname, target, store);
    return 0;
}

static int emit_entry(int fd, const char *root, const char *rel, const struct stat *st) {
    tar_hdr_t h;
    memset(&h, 0, sizeof(h));
    char name[4096];
    int whiteout = S_ISCHR(st->st_mode) && major(st->st_rdev) == 0 && minor(st->st_rdev) == 0;
    if (whiteout) {
        const char *slash = strrchr(rel, '/');
        int wr;
        if (slash)
            wr = snprintf(name, sizeof(name), "%.*s/.wh.%s",
                          (int)(slash - rel), rel, slash + 1);
        else
            wr = snprintf(name, sizeof(name), ".wh.%s", rel);
        if (wr < 0 || (size_t)wr >= sizeof(name)) {
            errno = ENAMETOOLONG;
            return -1;
        }
    } else if (S_ISDIR(st->st_mode)) {
        if (snprintf(name, sizeof(name), "%s/", rel) >= (int)sizeof(name)) {
            errno = ENAMETOOLONG;
            return -1;
        }
    } else {
        if (snprintf(name, sizeof(name), "%s", rel) >= (int)sizeof(name)) {
            errno = ENAMETOOLONG;
            return -1;
        }
    }
    if (set_name(&h, name, fd) != 0) return -1;
    put_octal(h.mode, 8, st->st_mode & 07777);
    put_octal(h.uid, 8, (uint64_t)st->st_uid);
    put_octal(h.gid, 8, (uint64_t)st->st_gid);
    put_octal(h.mtime, 12, (uint64_t)st->st_mtime);
    memcpy(h.magic, "ustar", 6);
    memcpy(h.version, "00", 2);

    char src[4096];
    if (snprintf(src, sizeof(src), "%s/%s", root, rel) >= (int)sizeof(src)) {
        errno = ENAMETOOLONG;
        return -1;
    }

    if (whiteout) {
        h.typeflag = '0';
        put_octal(h.size, 12, 0);
        compute_chksum(&h);
        if (write_all(fd, &h, TAR_BLOCK) != 0) return -1;
    } else if (S_ISREG(st->st_mode)) {
        h.typeflag = '0';
        put_octal(h.size, 12, (uint64_t)st->st_size);
        compute_chksum(&h);
        if (write_all(fd, &h, TAR_BLOCK) != 0) return -1;
        int sfd = open(src, O_RDONLY | O_NOFOLLOW);
        if (sfd < 0) return -1;
        char buf[TAR_BLOCK];
        uint64_t remaining = (uint64_t)st->st_size;
        while (remaining > 0) {
            size_t take = remaining > TAR_BLOCK ? TAR_BLOCK : (size_t)remaining;
            memset(buf, 0, TAR_BLOCK);
            ssize_t r = read(sfd, buf, take);
            if (r <= 0) {
                close(sfd);
                if (r < 0) return -1;
                errno = EIO;
                return -1;
            }
            if (write_all(fd, buf, TAR_BLOCK) != 0) {
                close(sfd);
                return -1;
            }
            remaining -= (uint64_t)r;
        }
        close(sfd);
    } else if (S_ISDIR(st->st_mode)) {
        h.typeflag = '5';
        put_octal(h.size, 12, 0);
        compute_chksum(&h);
        if (write_all(fd, &h, TAR_BLOCK) != 0) return -1;
    } else if (S_ISLNK(st->st_mode)) {
        char target[4096];
        ssize_t r = readlink(src, target, sizeof(target) - 1);
        if (r < 0) return -1;
        target[r] = 0;
        h.typeflag = '2';
        put_octal(h.size, 12, 0);
        if (set_linkname(&h, target, fd) != 0) return -1;
        compute_chksum(&h);
        if (write_all(fd, &h, TAR_BLOCK) != 0) return -1;
    } else if (S_ISCHR(st->st_mode)) {
        h.typeflag = '3';
        put_octal(h.size, 12, 0);
        put_octal(h.devmajor, 8, major(st->st_rdev));
        put_octal(h.devminor, 8, minor(st->st_rdev));
        compute_chksum(&h);
        if (write_all(fd, &h, TAR_BLOCK) != 0) return -1;
    } else if (S_ISBLK(st->st_mode)) {
        h.typeflag = '4';
        put_octal(h.size, 12, 0);
        put_octal(h.devmajor, 8, major(st->st_rdev));
        put_octal(h.devminor, 8, minor(st->st_rdev));
        compute_chksum(&h);
        if (write_all(fd, &h, TAR_BLOCK) != 0) return -1;
    } else if (S_ISFIFO(st->st_mode)) {
        h.typeflag = '6';
        put_octal(h.size, 12, 0);
        compute_chksum(&h);
        if (write_all(fd, &h, TAR_BLOCK) != 0) return -1;
    } else {
        warnx_("skipping unsupported file type: %s", src);
        return 0;
    }
    return 0;
}

static int walk_dir(int fd, const char *root, const char *rel) {
    char full[8192];
    int wr;
    if (rel[0])
        wr = snprintf(full, sizeof(full), "%s/%s", root, rel);
    else
        wr = snprintf(full, sizeof(full), "%s", root);
    if (wr < 0 || (size_t)wr >= sizeof(full)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    DIR *d = opendir(full);
    if (!d) return -1;
    struct dirent *de;
    int rc = 0;
    while ((de = readdir(d))) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        char child_rel[4096];
        int cr;
        if (rel[0])
            cr = snprintf(child_rel, sizeof(child_rel), "%s/%s", rel, de->d_name);
        else
            cr = snprintf(child_rel, sizeof(child_rel), "%s", de->d_name);
        if (cr < 0 || (size_t)cr >= sizeof(child_rel)) {
            rc = -1;
            continue;
        }
        char child_full[8192];
        int cf = snprintf(child_full, sizeof(child_full), "%s/%s", root, child_rel);
        if (cf < 0 || (size_t)cf >= sizeof(child_full)) {
            rc = -1;
            continue;
        }
        struct stat st;
        if (lstat(child_full, &st) != 0) {
            rc = -1;
            continue;
        }
        if (emit_entry(fd, root, child_rel, &st) != 0) {
            rc = -1;
            continue;
        }
        if (S_ISDIR(st.st_mode)) {
            if (walk_dir(fd, root, child_rel) != 0) rc = -1;
        }
    }
    closedir(d);
    return rc;
}

int tar_create(const char *src_dir, const char *out_path) {
    int fd;
    if (!strcmp(out_path, "-")) {
        fd = STDOUT_FILENO;
    } else {
        fd = open(out_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd < 0) return -1;
    }
    int rc = walk_dir(fd, src_dir, "");
    if (rc == 0) {
        if (write_zero_block(fd) != 0) rc = -1;
        if (write_zero_block(fd) != 0) rc = -1;
    }
    if (fd != STDOUT_FILENO) close(fd);
    return rc;
}

static int tar_read_payload(int fd, uint64_t sz, char **buf_out) {
    if (sz > TAR_PAYLOAD_MAX) {
        warnx_("tar extended header payload too large");
        return -1;
    }
    char *buf = xmalloc((size_t)sz + 1);
    uint64_t blocks = (sz + TAR_BLOCK - 1) / TAR_BLOCK;
    uint64_t off = 0;
    for (uint64_t b = 0; b < blocks; b++) {
        char blk[TAR_BLOCK];
        if (read_all(fd, blk, TAR_BLOCK) != 0) {
            free(buf);
            return -1;
        }
        size_t take = (sz - off > TAR_BLOCK) ? TAR_BLOCK : (size_t)(sz - off);
        memcpy(buf + off, blk, take);
        off += take;
    }
    buf[sz] = 0;
    *buf_out = buf;
    return 0;
}

static int tar_skip_payload(int fd, uint64_t sz) {
    uint64_t blocks = (sz + TAR_BLOCK - 1) / TAR_BLOCK;
    for (uint64_t b = 0; b < blocks; b++) {
        char blk[TAR_BLOCK];
        if (read_all(fd, blk, TAR_BLOCK) != 0) return -1;
    }
    return 0;
}

static int tar_safe_member_path(const char *name) {
    if (!name || !*name || name[0] == '/') return -1;
    const char *p = name;
    while (*p) {
        const char *slash = strchr(p, '/');
        size_t len = slash ? (size_t)(slash - p) : strlen(p);
        if (len == 0) return -1;
        if ((len == 1 && p[0] == '.') ||
            (len == 2 && p[0] == '.' && p[1] == '.')) {
            return -1;
        }
        if (!slash) break;
        p = slash + 1;
    }
    return 0;
}

static int tar_normalize_member_name(const char *in, char *out, size_t outsz) {
    size_t len = strlen(in);
    while (len > 0 && in[len - 1] == '/') len--;
    if (len == 0) {
        errno = EINVAL;
        return -1;
    }
    if (len + 1 > outsz) {
        errno = ENAMETOOLONG;
        return -1;
    }
    memcpy(out, in, len);
    out[len] = 0;
    return tar_safe_member_path(out);
}

static int pax_parse(const char *buf, size_t len, char **path_out, char **link_out) {
    size_t off = 0;
    while (off < len) {
        if (buf[off] == 0) break;

        size_t rec_start = off;
        size_t rec_len = 0;
        while (off < len && buf[off] != ' ') {
            if (!isdigit((unsigned char)buf[off])) return -1;
            rec_len = rec_len * 10 + (size_t)(buf[off] - '0');
            off++;
        }
        if (off >= len || buf[off] != ' ' || rec_len == 0) return -1;
        if (rec_start + rec_len > len) return -1;

        size_t payload_off = off + 1;
        size_t payload_len = rec_start + rec_len - payload_off;
        if (payload_len == 0 || buf[rec_start + rec_len - 1] != '\n') return -1;
        payload_len--;

        const char *payload = buf + payload_off;
        const char *eq = memchr(payload, '=', payload_len);
        if (!eq) return -1;

        size_t key_len = (size_t)(eq - payload);
        size_t value_len = payload_len - key_len - 1;
        if (key_len == 4 && !memcmp(payload, "path", 4)) {
            free(*path_out);
            *path_out = xmalloc(value_len + 1);
            memcpy(*path_out, eq + 1, value_len);
            (*path_out)[value_len] = 0;
        } else if (key_len == 8 && !memcmp(payload, "linkpath", 8)) {
            free(*link_out);
            *link_out = xmalloc(value_len + 1);
            memcpy(*link_out, eq + 1, value_len);
            (*link_out)[value_len] = 0;
        }

        off = rec_start + rec_len;
    }
    return 0;
}

static int open_parent_dir_at(int rootfd, const char *name, char *leaf, size_t leafsz) {
    char buf[4096];
    size_t n = strlen(name);
    if (n + 1 > sizeof(buf)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    memcpy(buf, name, n + 1);

    int dirfd = dup(rootfd);
    if (dirfd < 0) return -1;

    char *save = NULL;
    char *part = strtok_r(buf, "/", &save);
    while (part) {
        char *next = strtok_r(NULL, "/", &save);
        if (!next) {
            size_t leaf_len = strlen(part);
            if (leaf_len + 1 > leafsz) {
                close(dirfd);
                errno = ENAMETOOLONG;
                return -1;
            }
            memcpy(leaf, part, leaf_len + 1);
            return dirfd;
        }
        if (mkdirat(dirfd, part, 0755) != 0 && errno != EEXIST) {
            close(dirfd);
            return -1;
        }
        int nextfd = openat(dirfd, part, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
        if (nextfd < 0) {
            close(dirfd);
            return -1;
        }
        close(dirfd);
        dirfd = nextfd;
        part = next;
    }

    close(dirfd);
    errno = EINVAL;
    return -1;
}

static int ensure_dir_at(int rootfd, const char *name, mode_t mode) {
    char leaf[256];
    int dirfd = open_parent_dir_at(rootfd, name, leaf, sizeof(leaf));
    if (dirfd < 0) return -1;
    if (mkdirat(dirfd, leaf, mode & 07777) != 0 && errno != EEXIST) {
        close(dirfd);
        return -1;
    }
    int subfd = openat(dirfd, leaf, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
    if (subfd < 0) {
        close(dirfd);
        return -1;
    }
    close(subfd);
    close(dirfd);
    return 0;
}

static int unlink_nondir_at(int dirfd, const char *leaf) {
    struct stat st;
    if (fstatat(dirfd, leaf, &st, AT_SYMLINK_NOFOLLOW) != 0) {
        if (errno == ENOENT) return 0;
        return -1;
    }
    if (S_ISDIR(st.st_mode)) {
        errno = EISDIR;
        return -1;
    }
    return unlinkat(dirfd, leaf, 0);
}

int tar_extract(const char *tar_path, const char *dst_dir) {
    int fd;
    if (!strcmp(tar_path, "-")) {
        fd = STDIN_FILENO;
    } else {
        fd = open(tar_path, O_RDONLY);
        if (fd < 0) return -1;
    }
    if (mkdir_p(dst_dir, 0755) != 0) {
        if (fd != STDIN_FILENO) close(fd);
        return -1;
    }
    int rootfd = open(dst_dir, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
    if (rootfd < 0) {
        if (fd != STDIN_FILENO) close(fd);
        return -1;
    }

    char *long_name = NULL;
    char *long_link = NULL;
    char *pax_path = NULL;
    char *pax_link = NULL;
    int rc = 0;

    for (;;) {
        tar_hdr_t h;
        int r = read_all(fd, &h, TAR_BLOCK);
        if (r == 1) break;
        if (r < 0) {
            rc = -1;
            break;
        }

        int empty = 1;
        const unsigned char *p = (const unsigned char *)&h;
        for (size_t i = 0; i < TAR_BLOCK; i++) {
            if (p[i] != 0) {
                empty = 0;
                break;
            }
        }
        if (empty) {
            int r2 = read_all(fd, &h, TAR_BLOCK);
            (void)r2;
            break;
        }

        unsigned sum = 0;
        for (size_t i = 0; i < TAR_BLOCK; i++) {
            sum += (i >= 148 && i < 156) ? (unsigned)' ' : p[i];
        }
        uint64_t want = get_octal(h.chksum, 8);
        if ((uint64_t)sum != want) {
            warnx_("invalid tar header checksum");
            rc = -1;
            goto done;
        }
        if (memcmp(h.magic, "ustar", 5) != 0) {
            warnx_("invalid tar header magic");
            rc = -1;
            goto done;
        }

        uint64_t sz = get_octal(h.size, 12);
        uint64_t mode = get_octal(h.mode, 8);
        char tf = h.typeflag ? h.typeflag : '0';

        if (tf == 'L' || tf == 'K' || tf == 'x' || tf == 'g') {
            char *buf = NULL;
            if (tar_read_payload(fd, sz, &buf) != 0) {
                rc = -1;
                goto done;
            }

            if (tf == 'L') {
                free(long_name);
                long_name = buf;
            } else if (tf == 'K') {
                free(long_link);
                long_link = buf;
            } else if (tf == 'x') {
                if (pax_parse(buf, (size_t)sz, &pax_path, &pax_link) != 0) {
                    free(buf);
                    warnx_("invalid pax header");
                    rc = -1;
                    goto done;
                }
                free(buf);
                if (pax_path && tar_safe_member_path(pax_path) != 0) {
                    warnx_("unsafe pax path: %s", pax_path);
                    rc = -1;
                    goto done;
                }
                if (pax_link && tar_safe_member_path(pax_link) != 0) {
                    warnx_("unsafe pax linkpath: %s", pax_link);
                    rc = -1;
                    goto done;
                }
            } else {
                free(buf);
                warnx_("unsupported global pax header");
                rc = -1;
                goto done;
            }
            continue;
        }

        char raw_name[4096];
        if (pax_path) {
            size_t n = strlen(pax_path);
            if (n + 1 > sizeof(raw_name)) {
                warnx_("tar member path too long");
                rc = -1;
                goto done;
            }
            memcpy(raw_name, pax_path, n + 1);
        } else if (long_name) {
            size_t n = strlen(long_name);
            if (n + 1 > sizeof(raw_name)) {
                warnx_("tar member path too long");
                rc = -1;
                goto done;
            }
            memcpy(raw_name, long_name, n + 1);
        } else if (h.prefix[0]) {
            char prefix[156];
            char short_name[101];
            memcpy(prefix, h.prefix, 155);
            prefix[155] = 0;
            memcpy(short_name, h.name, 100);
            short_name[100] = 0;
            if (snprintf(raw_name, sizeof(raw_name), "%s/%s", prefix, short_name) >=
                (int)sizeof(raw_name)) {
                warnx_("tar member path too long");
                rc = -1;
                goto done;
            }
        } else {
            char short_name[101];
            memcpy(short_name, h.name, 100);
            short_name[100] = 0;
            if (snprintf(raw_name, sizeof(raw_name), "%s", short_name) >= (int)sizeof(raw_name)) {
                warnx_("tar member path too long");
                rc = -1;
                goto done;
            }
        }

        char linkname[4096];
        if (pax_link) {
            size_t n = strlen(pax_link);
            if (n + 1 > sizeof(linkname)) {
                warnx_("tar link target too long");
                rc = -1;
                goto done;
            }
            memcpy(linkname, pax_link, n + 1);
        } else if (long_link) {
            size_t n = strlen(long_link);
            if (n + 1 > sizeof(linkname)) {
                warnx_("tar link target too long");
                rc = -1;
                goto done;
            }
            memcpy(linkname, long_link, n + 1);
        } else {
            char ln[101];
            memcpy(ln, h.linkname, 100);
            ln[100] = 0;
            if (snprintf(linkname, sizeof(linkname), "%s", ln) >= (int)sizeof(linkname)) {
                warnx_("tar link target too long");
                rc = -1;
                goto done;
            }
        }

        char name[4096];
        if (tar_normalize_member_name(raw_name, name, sizeof(name)) != 0) {
            warnx_("unsafe tar member path: %s", raw_name);
            rc = -1;
            goto done;
        }

        free(long_name);
        long_name = NULL;
        free(long_link);
        long_link = NULL;
        free(pax_path);
        pax_path = NULL;
        free(pax_link);
        pax_link = NULL;

        if (tf == '5') {
            if (ensure_dir_at(rootfd, name, (mode_t)mode) != 0) {
                warnx_("mkdir %s: %s", name, strerror(errno));
                rc = -1;
                goto done;
            }
        } else if (tf == '0' || tf == '\0') {
            char leaf[256];
            int dirfd = open_parent_dir_at(rootfd, name, leaf, sizeof(leaf));
            if (dirfd < 0) {
                warnx_("open parent %s: %s", name, strerror(errno));
                rc = -1;
                goto done;
            }
            if (!strcmp(leaf, ".wh..wh..opq")) {
                if (fsetxattr(dirfd, "trusted.overlay.opaque", "y", 1, 0) != 0) {
                    warnx_("set opaque xattr %s: %s", name, strerror(errno));
                    close(dirfd);
                    rc = -1;
                    goto done;
                }
                close(dirfd);
                if (tar_skip_payload(fd, sz) != 0) {
                    rc = -1;
                    goto done;
                }
                continue;
            }
            if (!strncmp(leaf, ".wh.", 4) && strncmp(leaf + 4, ".wh.", 4) != 0) {
                memmove(leaf, leaf + 4, strlen(leaf + 4) + 1);
                if (unlink_nondir_at(dirfd, leaf) != 0 && errno != ENOENT) {
                    warnx_("unlink %s: %s", leaf, strerror(errno));
                    close(dirfd);
                    rc = -1;
                    goto done;
                }
                if (mknodat(dirfd, leaf, S_IFCHR, makedev(0, 0)) != 0) {
                    warnx_("mknod whiteout %s: %s", leaf, strerror(errno));
                    close(dirfd);
                    rc = -1;
                    goto done;
                }
                close(dirfd);
                if (tar_skip_payload(fd, sz) != 0) {
                    rc = -1;
                    goto done;
                }
                continue;
            }
            int wfd = openat(dirfd, leaf, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW,
                             (mode_t)(mode & 07777));
            close(dirfd);
            if (wfd < 0) {
                warnx_("open %s: %s", name, strerror(errno));
                rc = -1;
                goto done;
            }

            uint64_t remaining = sz;
            while (remaining > 0) {
                char blk[TAR_BLOCK];
                if (read_all(fd, blk, TAR_BLOCK) != 0) {
                    close(wfd);
                    rc = -1;
                    goto done;
                }
                size_t take = remaining > TAR_BLOCK ? TAR_BLOCK : (size_t)remaining;
                if (write_all(wfd, blk, take) != 0) {
                    close(wfd);
                    rc = -1;
                    goto done;
                }
                remaining -= take;
            }
            close(wfd);
        } else if (tf == '2') {
            char leaf[256];
            int dirfd = open_parent_dir_at(rootfd, name, leaf, sizeof(leaf));
            if (dirfd < 0) {
                warnx_("open parent %s: %s", name, strerror(errno));
                rc = -1;
                goto done;
            }
            if (unlink_nondir_at(dirfd, leaf) != 0 && errno != ENOENT) {
                warnx_("unlink %s: %s", name, strerror(errno));
                close(dirfd);
                rc = -1;
                goto done;
            }
            if (symlinkat(linkname, dirfd, leaf) != 0) {
                warnx_("symlink %s -> %s: %s", name, linkname, strerror(errno));
                close(dirfd);
                rc = -1;
                goto done;
            }
            close(dirfd);
        } else if (tf == '3' || tf == '4' || tf == '6') {
            uint64_t maj = get_octal(h.devmajor, 8);
            uint64_t min = get_octal(h.devminor, 8);
            mode_t m = mode & 07777;
            if (tf == '3') m |= S_IFCHR;
            else if (tf == '4') m |= S_IFBLK;
            else m |= S_IFIFO;

            char leaf[256];
            int dirfd = open_parent_dir_at(rootfd, name, leaf, sizeof(leaf));
            if (dirfd < 0) {
                warnx_("open parent %s: %s", name, strerror(errno));
                rc = -1;
                goto done;
            }
            if (unlink_nondir_at(dirfd, leaf) != 0 && errno != ENOENT) {
                warnx_("unlink %s: %s", name, strerror(errno));
                close(dirfd);
                rc = -1;
                goto done;
            }
            if (mknodat(dirfd, leaf, m, makedev((unsigned)maj, (unsigned)min)) != 0) {
                warnx_("mknod %s: %s", name, strerror(errno));
                close(dirfd);
                rc = -1;
                goto done;
            }
            close(dirfd);
        } else {
            uint64_t blocks = (sz + TAR_BLOCK - 1) / TAR_BLOCK;
            for (uint64_t b = 0; b < blocks; b++) {
                char blk[TAR_BLOCK];
                if (read_all(fd, blk, TAR_BLOCK) != 0) {
                    rc = -1;
                    goto done;
                }
            }
        }
    }

done:
    free(long_name);
    free(long_link);
    free(pax_path);
    free(pax_link);
    close(rootfd);
    if (fd != STDIN_FILENO) close(fd);
    return rc;
}
