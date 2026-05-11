#ifndef OVERLAYD_STORE_H
#define OVERLAYD_STORE_H

#include <stddef.h>

typedef struct {
    char root[4096];
    char layers_dir[4096];
    char ws_dir[4096];
} store_t;

int store_default_root(char *out, size_t outsz);
int store_open(store_t *s, const char *root_or_null);
int store_init(const char *root);

int store_layer_dir(const store_t *s, const char *name, char *out, size_t outsz);
int store_layer_content(const store_t *s, const char *name, char *out, size_t outsz);
int store_layer_meta(const store_t *s, const char *name, char *out, size_t outsz);
int store_layer_exists(const store_t *s, const char *name);

int store_ws_dir(const store_t *s, const char *name, char *out, size_t outsz);
int store_ws_upper(const store_t *s, const char *name, char *out, size_t outsz);
int store_ws_work(const store_t *s, const char *name, char *out, size_t outsz);
int store_ws_merged(const store_t *s, const char *name, char *out, size_t outsz);
int store_ws_meta(const store_t *s, const char *name, char *out, size_t outsz);
int store_ws_exists(const store_t *s, const char *name);

#endif
