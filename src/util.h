#ifndef OVERLAYD_UTIL_H
#define OVERLAYD_UTIL_H

#include <stddef.h>
#include <sys/types.h>
#include <stdarg.h>
#include <stdio.h>

#define OVERLAYD_NAME_MAX 64

extern int g_verbose;

void die(const char *fmt, ...) __attribute__((noreturn, format(printf, 1, 2)));
void warnx_(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void info_(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

int valid_name(const char *s);
int safe_for_overlay_opt(const char *s);

char *xstrdup(const char *s);
void *xmalloc(size_t n);
void *xrealloc(void *p, size_t n);

int path_join(char *out, size_t outsz, const char *a, const char *b);
int path_join3(char *out, size_t outsz, const char *a, const char *b, const char *c);

int mkdir_p(const char *path, mode_t mode);
int rm_rf(const char *path);
int copy_tree(const char *src, const char *dst);
int is_dir(const char *path);
int path_exists(const char *path);

int write_file(const char *path, const char *buf, size_t len);
int read_file(const char *path, char **buf_out, size_t *len_out);

int meta_set(const char *path, const char *key, const char *value);
int meta_get(const char *path, const char *key, char *out, size_t outsz);
int meta_has(const char *path, const char *key);
int meta_unset(const char *path, const char *key);

int is_mountpoint(const char *path);

#endif
