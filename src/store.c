#include "store.h"
#include "util.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

int store_default_root(char *out, size_t outsz) {
    const char *env = getenv("OVERLAYD_ROOT");
    if (env && *env) {
        size_t n = strlen(env);
        if (n + 1 > outsz) return -1;
        memcpy(out, env, n + 1);
        return 0;
    }
    int r = snprintf(out, outsz, "./.overlayd");
    if (r < 0 || (size_t)r >= outsz) return -1;
    return 0;
}

static int populate_paths(store_t *s) {
    if (path_join(s->layers_dir, sizeof(s->layers_dir), s->root, "layers") != 0) return -1;
    if (path_join(s->ws_dir, sizeof(s->ws_dir), s->root, "workspaces") != 0) return -1;
    return 0;
}

int store_init(const char *root) {
    if (!root || !*root) {
        errno = EINVAL;
        return -1;
    }
    if (mkdir_p(root, 0755) != 0) return -1;
    char p[4096];
    if (snprintf(p, sizeof(p), "%s/layers", root) >= (int)sizeof(p)) return -1;
    if (mkdir_p(p, 0755) != 0) return -1;
    if (snprintf(p, sizeof(p), "%s/workspaces", root) >= (int)sizeof(p)) return -1;
    if (mkdir_p(p, 0755) != 0) return -1;
    if (snprintf(p, sizeof(p), "%s/version", root) >= (int)sizeof(p)) return -1;
    const char *v = "1\n";
    if (write_file(p, v, strlen(v)) != 0) return -1;
    return 0;
}

int store_open(store_t *s, const char *root_or_null) {
    if (root_or_null && *root_or_null) {
        size_t n = strlen(root_or_null);
        if (n + 1 > sizeof(s->root)) {
            errno = ENAMETOOLONG;
            return -1;
        }
        memcpy(s->root, root_or_null, n + 1);
    } else {
        if (store_default_root(s->root, sizeof(s->root)) != 0) return -1;
    }
    if (populate_paths(s) != 0) return -1;
    if (!is_dir(s->root) || !is_dir(s->layers_dir) || !is_dir(s->ws_dir)) {
        errno = ENOENT;
        return -1;
    }
    return 0;
}

int store_layer_dir(const store_t *s, const char *name, char *out, size_t outsz) {
    return path_join(out, outsz, s->layers_dir, name);
}

int store_layer_content(const store_t *s, const char *name, char *out, size_t outsz) {
    return path_join3(out, outsz, s->layers_dir, name, "content");
}

int store_layer_meta(const store_t *s, const char *name, char *out, size_t outsz) {
    return path_join3(out, outsz, s->layers_dir, name, "meta");
}

int store_layer_exists(const store_t *s, const char *name) {
    char p[4096];
    if (store_layer_dir(s, name, p, sizeof(p)) != 0) return 0;
    return is_dir(p);
}

int store_ws_dir(const store_t *s, const char *name, char *out, size_t outsz) {
    return path_join(out, outsz, s->ws_dir, name);
}

int store_ws_upper(const store_t *s, const char *name, char *out, size_t outsz) {
    return path_join3(out, outsz, s->ws_dir, name, "upper");
}

int store_ws_work(const store_t *s, const char *name, char *out, size_t outsz) {
    return path_join3(out, outsz, s->ws_dir, name, "work");
}

int store_ws_merged(const store_t *s, const char *name, char *out, size_t outsz) {
    return path_join3(out, outsz, s->ws_dir, name, "merged");
}

int store_ws_meta(const store_t *s, const char *name, char *out, size_t outsz) {
    return path_join3(out, outsz, s->ws_dir, name, "meta");
}

int store_ws_exists(const store_t *s, const char *name) {
    char p[4096];
    if (store_ws_dir(s, name, p, sizeof(p)) != 0) return 0;
    return is_dir(p);
}
