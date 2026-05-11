#include "cmd.h"
#include "store.h"
#include "tar.h"
#include "util.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

static int build_tmp_path(store_t *s, const char *name, char *out, size_t outsz) {
    int r = snprintf(out, outsz, "%s/.tmp.%s.%ld",
                     s->layers_dir, name, (long)getpid());
    if (r < 0 || (size_t)r >= outsz) {
        errno = ENAMETOOLONG;
        return -1;
    }
    return 0;
}

static void usage(void) {
    fprintf(stderr,
        "usage: overlayd layer <subcommand> [args]\n"
        "\n"
        "subcommands:\n"
        "  create <name>                 create empty layer\n"
        "  list                          print TAB-separated: name<TAB>created_at\n"
        "  rm <name>                     remove layer\n"
        "  path <name>                   print layer content path\n"
        "  info <name>                   print layer metadata\n"
        "  commit <ws> <newlayer>        copy workspace upper into new layer\n"
        "  import <tar> <name>           extract tar into a new layer ('-' = stdin)\n"
        "  export <name> <tar>           write layer as tar ('-' = stdout)\n");
}

static int cmp_names(const void *ap, const void *bp) {
    const char *const *a = ap;
    const char *const *b = bp;
    return strcmp(*a, *b);
}

static int layer_names(store_t *s, char ***names_out, size_t *count_out) {
    DIR *d = opendir(s->layers_dir);
    if (!d) {
        warnx_("opendir %s: %s", s->layers_dir, strerror(errno));
        return -1;
    }

    size_t cap = 8;
    size_t n = 0;
    char **names = xmalloc(cap * sizeof(*names));
    struct dirent *de;
    while ((de = readdir(d))) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        if (!strncmp(de->d_name, ".tmp.", 5)) continue;
        if (!store_layer_exists(s, de->d_name)) continue;
        if (n == cap) {
            cap *= 2;
            names = xrealloc(names, cap * sizeof(*names));
        }
        names[n++] = xstrdup(de->d_name);
    }
    closedir(d);

    qsort(names, n, sizeof(*names), cmp_names);
    *names_out = names;
    *count_out = n;
    return 0;
}

static void free_names(char **names, size_t count) {
    for (size_t i = 0; i < count; i++) free(names[i]);
    free(names);
}

static int layer_info(store_t *s, int argc, char **argv) {
    if (argc != 1) {
        usage();
        return 2;
    }
    const char *name = argv[0];
    if (!valid_name(name)) {
        warnx_("invalid layer name: %s", name);
        return 2;
    }
    if (!store_layer_exists(s, name)) {
        warnx_("no such layer: %s", name);
        return 1;
    }
    char meta[4096];
    if (store_layer_meta(s, name, meta, sizeof(meta)) != 0) return 1;
    char *buf = NULL;
    size_t len = 0;
    if (read_file(meta, &buf, &len) != 0) {
        warnx_("read meta: %s", strerror(errno));
        return 1;
    }
    fwrite(buf, 1, len, stdout);
    free(buf);
    return 0;
}

static int layer_create(store_t *s, int argc, char **argv) {
    if (argc != 1) {
        usage();
        return 2;
    }
    const char *name = argv[0];
    if (!valid_name(name)) {
        warnx_("invalid layer name: %s", name);
        return 2;
    }
    if (store_layer_exists(s, name)) {
        warnx_("layer exists: %s", name);
        return 1;
    }
    char dir[4096], tmp[4096], tmp_content[4096], tmp_meta[4096];
    if (store_layer_dir(s, name, dir, sizeof(dir)) != 0) return 1;
    if (build_tmp_path(s, name, tmp, sizeof(tmp)) != 0) return 1;
    if (snprintf(tmp_content, sizeof(tmp_content), "%s/content", tmp) >= (int)sizeof(tmp_content) ||
        snprintf(tmp_meta, sizeof(tmp_meta), "%s/meta", tmp) >= (int)sizeof(tmp_meta)) return 1;
    if (mkdir(tmp, 0755) != 0) {
        warnx_("mkdir %s: %s", tmp, strerror(errno));
        return 1;
    }
    if (mkdir(tmp_content, 0755) != 0) {
        warnx_("mkdir %s: %s", tmp_content, strerror(errno));
        rm_rf(tmp);
        return 1;
    }
    char ts[32];
    snprintf(ts, sizeof(ts), "%lld", (long long)time(NULL));
    if (meta_set(tmp_meta, "name", name) != 0 ||
        meta_set(tmp_meta, "created_at", ts) != 0) {
        warnx_("write meta: %s", strerror(errno));
        rm_rf(tmp);
        return 1;
    }
    if (rename(tmp, dir) != 0) {
        warnx_("rename %s -> %s: %s", tmp, dir, strerror(errno));
        rm_rf(tmp);
        return 1;
    }
    printf("%s\n", name);
    return 0;
}

static int layer_list(store_t *s, int argc, char **argv) {
    (void)argv;
    if (argc != 0) {
        usage();
        return 2;
    }
    char **names = NULL;
    size_t count = 0;
    if (layer_names(s, &names, &count) != 0) {
        return 1;
    }
    for (size_t i = 0; i < count; i++) {
        char meta[4096];
        if (store_layer_meta(s, names[i], meta, sizeof(meta)) != 0) continue;
        char created[32] = "?";
        meta_get(meta, "created_at", created, sizeof(created));
        printf("%s\t%s\n", names[i], created);
    }
    free_names(names, count);
    return 0;
}

static int layer_rm(store_t *s, int argc, char **argv) {
    if (argc != 1) {
        usage();
        return 2;
    }
    const char *name = argv[0];
    if (!valid_name(name)) {
        warnx_("invalid layer name: %s", name);
        return 2;
    }
    if (!store_layer_exists(s, name)) {
        warnx_("no such layer: %s", name);
        return 1;
    }
    char dir[4096];
    if (store_layer_dir(s, name, dir, sizeof(dir)) != 0) return 1;

    DIR *d = opendir(s->ws_dir);
    if (d) {
        struct dirent *de;
        while ((de = readdir(d))) {
            if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
            char meta[4096];
            if (store_ws_meta(s, de->d_name, meta, sizeof(meta)) != 0) continue;
            char lowers[8192];
            if (meta_get(meta, "lowers", lowers, sizeof(lowers)) == 0) {
                char *p = lowers;
                while (p && *p) {
                    char *next = strchr(p, ',');
                    if (next) *next = 0;
                    if (!strcmp(p, name)) {
                        warnx_("layer %s in use by workspace %s", name, de->d_name);
                        closedir(d);
                        return 1;
                    }
                    p = next ? next + 1 : NULL;
                }
            }
        }
        closedir(d);
    }

    if (rm_rf(dir) != 0) {
        warnx_("rm -rf %s: %s", dir, strerror(errno));
        return 1;
    }
    return 0;
}

static int layer_path(store_t *s, int argc, char **argv) {
    if (argc != 1) {
        usage();
        return 2;
    }
    const char *name = argv[0];
    if (!valid_name(name)) {
        warnx_("invalid layer name: %s", name);
        return 2;
    }
    if (!store_layer_exists(s, name)) {
        warnx_("no such layer: %s", name);
        return 1;
    }
    char content[4096];
    if (store_layer_content(s, name, content, sizeof(content)) != 0) return 1;
    printf("%s\n", content);
    return 0;
}

static int layer_commit(store_t *s, int argc, char **argv) {
    if (argc != 2) {
        usage();
        return 2;
    }
    const char *ws_name = argv[0];
    const char *new_name = argv[1];
    if (!valid_name(ws_name) || !valid_name(new_name)) {
        warnx_("invalid name");
        return 2;
    }
    if (!store_ws_exists(s, ws_name)) {
        warnx_("no such workspace: %s", ws_name);
        return 1;
    }
    if (store_layer_exists(s, new_name)) {
        warnx_("layer exists: %s", new_name);
        return 1;
    }
    char ws_meta[4096];
    if (store_ws_meta(s, ws_name, ws_meta, sizeof(ws_meta)) != 0) return 1;
    char mounted[16] = "";
    if (meta_get(ws_meta, "mounted", mounted, sizeof(mounted)) == 0 && !strcmp(mounted, "1")) {
        warnx_("workspace %s is mounted; unmount before commit", ws_name);
        return 1;
    }
    char upper[4096];
    if (store_ws_upper(s, ws_name, upper, sizeof(upper)) != 0) return 1;
    char dir[4096], tmp[4096], tmp_content[4096], tmp_meta[4096];
    if (store_layer_dir(s, new_name, dir, sizeof(dir)) != 0) return 1;
    if (build_tmp_path(s, new_name, tmp, sizeof(tmp)) != 0) return 1;
    if (snprintf(tmp_content, sizeof(tmp_content), "%s/content", tmp) >= (int)sizeof(tmp_content) ||
        snprintf(tmp_meta, sizeof(tmp_meta), "%s/meta", tmp) >= (int)sizeof(tmp_meta)) return 1;
    if (mkdir(tmp, 0755) != 0) {
        warnx_("mkdir %s: %s", tmp, strerror(errno));
        return 1;
    }
    if (copy_tree(upper, tmp_content) != 0) {
        warnx_("copy_tree: %s", strerror(errno));
        rm_rf(tmp);
        return 1;
    }
    char ts[32];
    snprintf(ts, sizeof(ts), "%lld", (long long)time(NULL));
    char lowers[8192] = "";
    meta_get(ws_meta, "lowers", lowers, sizeof(lowers));
    meta_set(tmp_meta, "name", new_name);
    meta_set(tmp_meta, "created_at", ts);
    meta_set(tmp_meta, "from_ws", ws_name);
    if (lowers[0]) meta_set(tmp_meta, "parents", lowers);
    if (rename(tmp, dir) != 0) {
        warnx_("rename: %s", strerror(errno));
        rm_rf(tmp);
        return 1;
    }
    printf("%s\n", new_name);
    return 0;
}

static int layer_import(store_t *s, int argc, char **argv) {
    if (argc != 2) {
        usage();
        return 2;
    }
    const char *tar_path = argv[0];
    const char *name = argv[1];
    if (!valid_name(name)) {
        warnx_("invalid layer name: %s", name);
        return 2;
    }
    if (store_layer_exists(s, name)) {
        warnx_("layer exists: %s", name);
        return 1;
    }
    char dir[4096], tmp[4096], tmp_content[4096], tmp_meta[4096];
    if (store_layer_dir(s, name, dir, sizeof(dir)) != 0) return 1;
    if (build_tmp_path(s, name, tmp, sizeof(tmp)) != 0) return 1;
    if (snprintf(tmp_content, sizeof(tmp_content), "%s/content", tmp) >= (int)sizeof(tmp_content) ||
        snprintf(tmp_meta, sizeof(tmp_meta), "%s/meta", tmp) >= (int)sizeof(tmp_meta)) return 1;
    if (mkdir(tmp, 0755) != 0) {
        warnx_("mkdir %s: %s", tmp, strerror(errno));
        return 1;
    }
    if (mkdir(tmp_content, 0755) != 0) {
        warnx_("mkdir %s: %s", tmp_content, strerror(errno));
        rm_rf(tmp);
        return 1;
    }
    if (tar_extract(tar_path, tmp_content) != 0) {
        warnx_("tar extract failed");
        rm_rf(tmp);
        return 1;
    }
    char ts[32];
    snprintf(ts, sizeof(ts), "%lld", (long long)time(NULL));
    meta_set(tmp_meta, "name", name);
    meta_set(tmp_meta, "created_at", ts);
    meta_set(tmp_meta, "imported_from", tar_path);
    if (rename(tmp, dir) != 0) {
        warnx_("rename: %s", strerror(errno));
        rm_rf(tmp);
        return 1;
    }
    printf("%s\n", name);
    return 0;
}

static int layer_export(store_t *s, int argc, char **argv) {
    if (argc != 2) {
        usage();
        return 2;
    }
    const char *name = argv[0];
    const char *tar_path = argv[1];
    if (!valid_name(name)) {
        warnx_("invalid layer name: %s", name);
        return 2;
    }
    if (!store_layer_exists(s, name)) {
        warnx_("no such layer: %s", name);
        return 1;
    }
    char content[4096];
    if (store_layer_content(s, name, content, sizeof(content)) != 0) return 1;
    if (tar_create(content, tar_path) != 0) {
        warnx_("tar create failed");
        return 1;
    }
    return 0;
}

int cmd_layer(int argc, char **argv, store_t *s) {
    if (argc < 2) {
        usage();
        return 2;
    }
    const char *sub = argv[1];
    if (!strcmp(sub, "create")) return layer_create(s, argc - 2, argv + 2);
    if (!strcmp(sub, "list") || !strcmp(sub, "ls")) return layer_list(s, argc - 2, argv + 2);
    if (!strcmp(sub, "rm")) return layer_rm(s, argc - 2, argv + 2);
    if (!strcmp(sub, "path")) return layer_path(s, argc - 2, argv + 2);
    if (!strcmp(sub, "info")) return layer_info(s, argc - 2, argv + 2);
    if (!strcmp(sub, "commit")) return layer_commit(s, argc - 2, argv + 2);
    if (!strcmp(sub, "import")) return layer_import(s, argc - 2, argv + 2);
    if (!strcmp(sub, "export")) return layer_export(s, argc - 2, argv + 2);
    if (!strcmp(sub, "-h") || !strcmp(sub, "--help") || !strcmp(sub, "help")) {
        usage();
        return 0;
    }
    usage();
    return 2;
}
