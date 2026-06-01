#include "cmd.h"
#include "store.h"
#include "util.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

static void usage(void) {
    fprintf(stderr,
        "usage: overlayd ws <subcommand> [args]\n"
        "\n"
        "subcommands:\n"
        "  create <name> -l L1 [-l L2 ...] [-m PATH] [--no-mount]\n"
        "      create a workspace and (by default) mount it. -l layers are\n"
        "      ordered top-to-bottom (first -l is uppermost lower).\n"
        "  list                       print TAB-separated: name<TAB>mounted|unmounted<TAB>mountpoint<TAB>lowers\n"
        "  info <name>                print workspace metadata\n"
        "  path <name>                print merged mountpoint\n"
        "  upper <name>               print upper directory\n"
        "  mount <name>               mount an existing workspace\n"
        "  unmount <name>             unmount a workspace\n"
        "  rm <name> [--force]        unmount and remove a workspace\n");
}

static int resolve_abs(const char *in, char *out, size_t outsz) {
    char *r = realpath(in, NULL);
    if (!r) return -1;
    size_t n = strlen(r);
    if (n + 1 > outsz) {
        free(r);
        errno = ENAMETOOLONG;
        return -1;
    }
    memcpy(out, r, n + 1);
    free(r);
    return 0;
}

static int cmp_names(const void *ap, const void *bp) {
    const char *const *a = ap;
    const char *const *b = bp;
    return strcmp(*a, *b);
}

static int ws_names(store_t *s, char ***names_out, size_t *count_out) {
    DIR *d = opendir(s->ws_dir);
    if (!d) {
        warnx_("opendir %s: %s", s->ws_dir, strerror(errno));
        return -1;
    }

    size_t cap = 8;
    size_t n = 0;
    char **names = xmalloc(cap * sizeof(*names));
    struct dirent *de;
    while ((de = readdir(d))) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        if (!strncmp(de->d_name, ".tmp.", 5)) continue;
        if (!store_ws_exists(s, de->d_name)) continue;
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

static int ws_mountpoint(store_t *s, const char *name, char *out, size_t outsz, int ensure_default) {
    char meta[4096];
    if (store_ws_meta(s, name, meta, sizeof(meta)) != 0) return -1;
    if (meta_get(meta, "mountpoint", out, outsz) == 0) return 0;

    char merged[4096];
    if (store_ws_merged(s, name, merged, sizeof(merged)) != 0) return -1;
    if (ensure_default && mkdir_p(merged, 0755) != 0) {
        warnx_("mkdir %s: %s", merged, strerror(errno));
        return -1;
    }
    if (resolve_abs(merged, out, outsz) == 0) return 0;
    if (!ensure_default && errno == ENOENT) {
        size_t n = strlen(merged);
        if (n + 1 > outsz) {
            errno = ENAMETOOLONG;
            return -1;
        }
        memcpy(out, merged, n + 1);
        return 0;
    }
    warnx_("realpath %s: %s", merged, strerror(errno));
    return -1;
}

static int ws_real_mount_state(store_t *s, const char *name, char *mountpoint, size_t outsz) {
    if (ws_mountpoint(s, name, mountpoint, outsz, 0) != 0) return -1;
    return is_mountpoint(mountpoint) ? 1 : 0;
}

static int build_lower_opt(store_t *s, char **layers, int n, char *out, size_t outsz) {
    size_t off = 0;
    for (int i = 0; i < n; i++) {
        if (!valid_name(layers[i])) {
            warnx_("invalid layer name: %s", layers[i]);
            return -1;
        }
        if (!store_layer_exists(s, layers[i])) {
            warnx_("no such layer: %s", layers[i]);
            return -1;
        }
        char content[4096];
        char abs_content[4096];
        if (store_layer_content(s, layers[i], content, sizeof(content)) != 0) return -1;
        if (resolve_abs(content, abs_content, sizeof(abs_content)) != 0) {
            warnx_("realpath %s: %s", content, strerror(errno));
            return -1;
        }
        if (!safe_for_overlay_opt(abs_content)) {
            warnx_("layer path contains unsafe characters: %s", abs_content);
            return -1;
        }
        if (i > 0) {
            if (off + 1 >= outsz) {
                errno = ENAMETOOLONG;
                return -1;
            }
            out[off++] = ':';
        }
        size_t cn = strlen(abs_content);
        if (off + cn + 1 >= outsz) {
            errno = ENAMETOOLONG;
            return -1;
        }
        memcpy(out + off, abs_content, cn);
        off += cn;
    }
    out[off] = 0;
    return 0;
}

static int do_mount(store_t *s, const char *name) {
    char meta[4096], upper[4096], work[4096];
    if (store_ws_meta(s, name, meta, sizeof(meta)) != 0 ||
        store_ws_upper(s, name, upper, sizeof(upper)) != 0 ||
        store_ws_work(s, name, work, sizeof(work)) != 0) return -1;

    char mountpoint[4096];
    if (ws_mountpoint(s, name, mountpoint, sizeof(mountpoint), 1) != 0) {
        return -1;
    }

    if (!is_dir(mountpoint)) {
        if (mkdir_p(mountpoint, 0755) != 0) {
            warnx_("mkdir %s: %s", mountpoint, strerror(errno));
            return -1;
        }
    }

    if (is_mountpoint(mountpoint)) {
        if (meta_set(meta, "mounted", "1") != 0 ||
            meta_set(meta, "mountpoint", mountpoint) != 0) {
            warnx_("write meta: %s", strerror(errno));
            return -1;
        }
        return 0;
    }

    char lowers[8192];
    if (meta_get(meta, "lowers", lowers, sizeof(lowers)) != 0) {
        warnx_("workspace %s has no lowers", name);
        return -1;
    }

    char *layer_argv[64];
    int layer_argc = 0;
    char *p = lowers;
    while (p && *p && layer_argc < (int)(sizeof(layer_argv) / sizeof(layer_argv[0]))) {
        char *next = strchr(p, ',');
        if (next) *next = 0;
        layer_argv[layer_argc++] = p;
        p = next ? next + 1 : NULL;
    }

    char lower_opt[8192];
    if (build_lower_opt(s, layer_argv, layer_argc, lower_opt, sizeof(lower_opt)) != 0)
        return -1;

    char abs_upper[4096], abs_work[4096];
    if (resolve_abs(upper, abs_upper, sizeof(abs_upper)) != 0 ||
        resolve_abs(work, abs_work, sizeof(abs_work)) != 0) return -1;
    if (!safe_for_overlay_opt(abs_upper) || !safe_for_overlay_opt(abs_work)) {
        warnx_("workspace path contains unsafe characters");
        return -1;
    }

    char data[16384];
    int r = snprintf(data, sizeof(data),
                     "lowerdir=%s,upperdir=%s,workdir=%s",
                     lower_opt, abs_upper, abs_work);
    if (r < 0 || (size_t)r >= sizeof(data)) {
        errno = ENAMETOOLONG;
        return -1;
    }

    info_("mount overlay -o %s %s", data, mountpoint);
    if (mount("overlay", mountpoint, "overlay", 0, data) != 0) {
        warnx_("mount overlay on %s: %s", mountpoint, strerror(errno));
        return -1;
    }
    if (meta_set(meta, "mounted", "1") != 0 ||
        meta_set(meta, "mountpoint", mountpoint) != 0) {
        warnx_("write meta: %s", strerror(errno));
        return -1;
    }
    return 0;
}

static int do_unmount(store_t *s, const char *name) {
    char meta[4096];
    if (store_ws_meta(s, name, meta, sizeof(meta)) != 0) return -1;
    char mountpoint[4096];
    if (ws_mountpoint(s, name, mountpoint, sizeof(mountpoint), 0) != 0) {
        return -1;
    }
    if (!is_mountpoint(mountpoint)) {
        if (meta_set(meta, "mounted", "0") != 0) {
            warnx_("write meta: %s", strerror(errno));
            return -1;
        }
        return 0;
    }
    if (umount(mountpoint) != 0) {
        if (errno == EINVAL || errno == ENOENT) {
            if (meta_set(meta, "mounted", "0") != 0) {
                warnx_("write meta: %s", strerror(errno));
                return -1;
            }
            return 0;
        }
        warnx_("umount %s: %s", mountpoint, strerror(errno));
        return -1;
    }
    if (meta_set(meta, "mounted", "0") != 0) {
        warnx_("write meta: %s", strerror(errno));
        return -1;
    }
    return 0;
}

static int ws_create_common(store_t *s, int argc, char **argv, int require_mountpoint, int allow_no_mount) {
    if (argc < 1) {
        usage();
        return 2;
    }
    const char *name = argv[0];
    if (!valid_name(name)) {
        warnx_("invalid workspace name: %s", name);
        return 2;
    }
    char *layers[64];
    int n_layers = 0;
    const char *mountpoint_arg = NULL;
    int do_mount_flag = 1;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-l") || !strcmp(argv[i], "--layer")) {
            if (i + 1 >= argc) {
                usage();
                return 2;
            }
            if (n_layers >= (int)(sizeof(layers) / sizeof(layers[0]))) {
                warnx_("too many layers");
                return 2;
            }
            layers[n_layers++] = argv[++i];
        } else if (!strcmp(argv[i], "-m") || !strcmp(argv[i], "--mount")) {
            if (i + 1 >= argc) {
                usage();
                return 2;
            }
            mountpoint_arg = argv[++i];
        } else if (!strcmp(argv[i], "--no-mount")) {
            if (!allow_no_mount) {
                warnx_("materialize always mounts the workspace");
                return 2;
            }
            do_mount_flag = 0;
        } else {
            usage();
            return 2;
        }
    }
    if (require_mountpoint && !mountpoint_arg) {
        warnx_("materialize requires -m PATH");
        return 2;
    }
    if (n_layers == 0) {
        warnx_("at least one -l layer required");
        return 2;
    }
    if (store_ws_exists(s, name)) {
        warnx_("workspace exists: %s", name);
        return 1;
    }

    char dir[4096], upper[4096], work[4096], merged[4096], meta[4096];
    if (store_ws_dir(s, name, dir, sizeof(dir)) != 0 ||
        store_ws_upper(s, name, upper, sizeof(upper)) != 0 ||
        store_ws_work(s, name, work, sizeof(work)) != 0 ||
        store_ws_merged(s, name, merged, sizeof(merged)) != 0 ||
        store_ws_meta(s, name, meta, sizeof(meta)) != 0) return 1;

    if (mkdir_p(upper, 0755) != 0 ||
        mkdir_p(work, 0755) != 0) {
        warnx_("create dirs: %s", strerror(errno));
        return 1;
    }

    char joined[8192];
    size_t off = 0;
    for (int i = 0; i < n_layers; i++) {
        if (!valid_name(layers[i])) {
            warnx_("invalid layer name: %s", layers[i]);
            rm_rf(dir);
            return 2;
        }
        if (!store_layer_exists(s, layers[i])) {
            warnx_("no such layer: %s", layers[i]);
            rm_rf(dir);
            return 1;
        }
        size_t ln = strlen(layers[i]);
        if (off + ln + 2 >= sizeof(joined)) {
            warnx_("lowers too long");
            rm_rf(dir);
            return 1;
        }
        if (i > 0) joined[off++] = ',';
        memcpy(joined + off, layers[i], ln);
        off += ln;
    }
    joined[off] = 0;

    char ts[32];
    snprintf(ts, sizeof(ts), "%lld", (long long)time(NULL));
    if (meta_set(meta, "name", name) != 0 ||
        meta_set(meta, "created_at", ts) != 0 ||
        meta_set(meta, "lowers", joined) != 0 ||
        meta_set(meta, "mounted", "0") != 0) {
        warnx_("write meta: %s", strerror(errno));
        rm_rf(dir);
        return 1;
    }

    char abs_mp[4096];
    if (mountpoint_arg) {
        if (mkdir_p(mountpoint_arg, 0755) != 0) {
            warnx_("mkdir %s: %s", mountpoint_arg, strerror(errno));
            rm_rf(dir);
            return 1;
        }
        if (resolve_abs(mountpoint_arg, abs_mp, sizeof(abs_mp)) != 0) {
            warnx_("realpath %s: %s", mountpoint_arg, strerror(errno));
            rm_rf(dir);
            return 1;
        }
        if (meta_set(meta, "mountpoint", abs_mp) != 0) {
            warnx_("write meta: %s", strerror(errno));
            rm_rf(dir);
            return 1;
        }
    } else {
        if (mkdir_p(merged, 0755) != 0) {
            warnx_("mkdir %s: %s", merged, strerror(errno));
            rm_rf(dir);
            return 1;
        }
        if (resolve_abs(merged, abs_mp, sizeof(abs_mp)) != 0) {
            warnx_("realpath %s: %s", merged, strerror(errno));
            rm_rf(dir);
            return 1;
        }
        if (meta_set(meta, "mountpoint", abs_mp) != 0) {
            warnx_("write meta: %s", strerror(errno));
            rm_rf(dir);
            return 1;
        }
    }

    if (do_mount_flag) {
        if (do_mount(s, name) != 0) {
            rm_rf(dir);
            return 1;
        }
    }
    printf("%s\n", abs_mp);
    return 0;
}

static int ws_create(store_t *s, int argc, char **argv) {
    return ws_create_common(s, argc, argv, 0, 1);
}

static int ws_list(store_t *s, int argc, char **argv) {
    (void)argv;
    if (argc != 0) {
        usage();
        return 2;
    }
    char **names = NULL;
    size_t count = 0;
    if (ws_names(s, &names, &count) != 0) {
        return 1;
    }
    for (size_t i = 0; i < count; i++) {
        char meta[4096];
        if (store_ws_meta(s, names[i], meta, sizeof(meta)) != 0) continue;
        char mounted[16] = "0";
        char mp[4096] = "";
        char lowers[8192] = "";
        meta_get(meta, "mounted", mounted, sizeof(mounted));
        meta_get(meta, "mountpoint", mp, sizeof(mp));
        meta_get(meta, "lowers", lowers, sizeof(lowers));
        const char *state = !strcmp(mounted, "1") ? "mounted" : "unmounted";
        if (mp[0]) state = is_mountpoint(mp) ? "mounted" : "unmounted";
        printf("%s\t%s\t%s\t%s\n",
               names[i],
               state,
               mp,
               lowers);
    }
    free_names(names, count);
    return 0;
}

static int ws_info(store_t *s, int argc, char **argv) {
    if (argc != 1) {
        usage();
        return 2;
    }
    const char *name = argv[0];
    if (!valid_name(name)) {
        warnx_("invalid workspace name: %s", name);
        return 2;
    }
    if (!store_ws_exists(s, name)) {
        warnx_("no such workspace: %s", name);
        return 1;
    }
    char meta[4096];
    if (store_ws_meta(s, name, meta, sizeof(meta)) != 0) return 1;
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

static int ws_path(store_t *s, int argc, char **argv) {
    if (argc != 1) {
        usage();
        return 2;
    }
    const char *name = argv[0];
    if (!valid_name(name)) {
        warnx_("invalid workspace name: %s", name);
        return 2;
    }
    if (!store_ws_exists(s, name)) {
        warnx_("no such workspace: %s", name);
        return 1;
    }
    char meta[4096];
    if (store_ws_meta(s, name, meta, sizeof(meta)) != 0) return 1;
    char mp[4096];
    if (meta_get(meta, "mountpoint", mp, sizeof(mp)) != 0) {
        warnx_("workspace has no mountpoint");
        return 1;
    }
    printf("%s\n", mp);
    return 0;
}

static int ws_upper(store_t *s, int argc, char **argv) {
    if (argc != 1) {
        usage();
        return 2;
    }
    const char *name = argv[0];
    if (!valid_name(name)) {
        warnx_("invalid workspace name: %s", name);
        return 2;
    }
    if (!store_ws_exists(s, name)) {
        warnx_("no such workspace: %s", name);
        return 1;
    }
    char upper[4096];
    if (store_ws_upper(s, name, upper, sizeof(upper)) != 0) return 1;
    char abs_upper[4096];
    if (resolve_abs(upper, abs_upper, sizeof(abs_upper)) != 0) {
        warnx_("realpath: %s", strerror(errno));
        return 1;
    }
    printf("%s\n", abs_upper);
    return 0;
}

static int ws_mount(store_t *s, int argc, char **argv) {
    if (argc != 1) {
        usage();
        return 2;
    }
    const char *name = argv[0];
    if (!valid_name(name)) {
        warnx_("invalid workspace name: %s", name);
        return 2;
    }
    if (!store_ws_exists(s, name)) {
        warnx_("no such workspace: %s", name);
        return 1;
    }
    return do_mount(s, name) == 0 ? 0 : 1;
}

static int ws_unmount(store_t *s, int argc, char **argv) {
    if (argc != 1) {
        usage();
        return 2;
    }
    const char *name = argv[0];
    if (!valid_name(name)) {
        warnx_("invalid workspace name: %s", name);
        return 2;
    }
    if (!store_ws_exists(s, name)) {
        warnx_("no such workspace: %s", name);
        return 1;
    }
    return do_unmount(s, name) == 0 ? 0 : 1;
}

static int ws_rm(store_t *s, int argc, char **argv) {
    if (argc < 1 || argc > 2) {
        usage();
        return 2;
    }
    int force = 0;
    const char *name = NULL;
    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "--force") || !strcmp(argv[i], "-f")) {
            force = 1;
        } else if (!name) {
            name = argv[i];
        } else {
            usage();
            return 2;
        }
    }
    if (!name || !valid_name(name)) {
        warnx_("invalid workspace name");
        return 2;
    }
    if (!store_ws_exists(s, name)) {
        warnx_("no such workspace: %s", name);
        return 1;
    }

    char mountpoint[4096];
    int mounted = ws_real_mount_state(s, name, mountpoint, sizeof(mountpoint));
    if (mounted < 0) return 1;

    if (mounted) {
        int unmount_rc = do_unmount(s, name);
        int still_mounted = ws_real_mount_state(s, name, mountpoint, sizeof(mountpoint));
        if (still_mounted < 0) return 1;
        if (still_mounted) {
            warnx_("workspace %s is still mounted at %s", name, mountpoint);
            return 1;
        }
        if (unmount_rc != 0 && !force) return 1;
    } else {
        char meta[4096];
        if (store_ws_meta(s, name, meta, sizeof(meta)) != 0) return 1;
        char meta_mounted[16] = "0";
        meta_get(meta, "mounted", meta_mounted, sizeof(meta_mounted));
        if (!strcmp(meta_mounted, "1")) {
            if (do_unmount(s, name) != 0 && !force) return 1;
        }
    }

    char dir[4096];
    if (store_ws_dir(s, name, dir, sizeof(dir)) != 0) return 1;
    if (rm_rf(dir) != 0) {
        warnx_("rm -rf %s: %s", dir, strerror(errno));
        return 1;
    }
    return 0;
}

int cmd_ws(int argc, char **argv, store_t *s) {
    if (argc < 2) {
        usage();
        return 2;
    }
    const char *sub = argv[1];
    if (!strcmp(sub, "create")) return ws_create(s, argc - 2, argv + 2);
    if (!strcmp(sub, "list") || !strcmp(sub, "ls")) return ws_list(s, argc - 2, argv + 2);
    if (!strcmp(sub, "info")) return ws_info(s, argc - 2, argv + 2);
    if (!strcmp(sub, "path")) return ws_path(s, argc - 2, argv + 2);
    if (!strcmp(sub, "upper")) return ws_upper(s, argc - 2, argv + 2);
    if (!strcmp(sub, "mount")) return ws_mount(s, argc - 2, argv + 2);
    if (!strcmp(sub, "unmount") || !strcmp(sub, "umount")) return ws_unmount(s, argc - 2, argv + 2);
    if (!strcmp(sub, "rm")) return ws_rm(s, argc - 2, argv + 2);
    if (!strcmp(sub, "-h") || !strcmp(sub, "--help") || !strcmp(sub, "help")) {
        usage();
        return 0;
    }
    usage();
    return 2;
}

int cmd_materialize(int argc, char **argv, store_t *s) {
    if (argc < 2) {
        warnx_("usage: overlayd materialize <name> -l L1 [-l L2 ...] -m PATH");
        return 2;
    }
    return ws_create_common(s, argc - 1, argv + 1, 1, 0);
}
