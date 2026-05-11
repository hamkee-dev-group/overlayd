#include "cmd.h"
#include "store.h"
#include "util.h"

#include <stdio.h>
#include <string.h>

#define OVERLAYD_VERSION "0.1.0"

static void top_usage(void) {
    fprintf(stderr,
        "overlayd %s — overlayfs as an image and workspace layering service\n"
        "\n"
        "usage: overlayd [--root PATH] [-v] <command> [args]\n"
        "\n"
        "commands:\n"
        "  init [path]                  initialize a store\n"
        "  layer create|list|rm|path|info|commit|import|export\n"
        "                               manage immutable layers\n"
        "  ws create|list|info|path|upper|mount|unmount|rm\n"
        "                               manage workspaces (overlayfs mounts)\n"
        "  materialize <name> -l L1 [-l L2 ...] -m PATH\n"
        "                               create and mount a workspace, printing PATH\n"
        "  version                      print version\n"
        "  help [topic]                 print help\n"
        "\n"
        "global flags:\n"
        "  --root PATH                  store root (default: $OVERLAYD_ROOT or ./.overlayd)\n"
        "  -v, --verbose                verbose logging on stderr\n",
        OVERLAYD_VERSION);
}

int main(int argc, char **argv) {
    const char *root = NULL;

    int i = 1;
    while (i < argc) {
        if (!strcmp(argv[i], "--root")) {
            if (i + 1 >= argc) {
                top_usage();
                return 2;
            }
            root = argv[++i];
            i++;
        } else if (!strcmp(argv[i], "-v") || !strcmp(argv[i], "--verbose")) {
            g_verbose = 1;
            i++;
        } else if (!strcmp(argv[i], "--version")) {
            printf("overlayd %s\n", OVERLAYD_VERSION);
            return 0;
        } else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            top_usage();
            return 0;
        } else {
            break;
        }
    }

    if (i >= argc) {
        top_usage();
        return 2;
    }

    int sub_argc = argc - i;
    char **sub_argv = argv + i;
    const char *cmd = sub_argv[0];

    if (!strcmp(cmd, "version")) {
        printf("overlayd %s\n", OVERLAYD_VERSION);
        return 0;
    }
    if (!strcmp(cmd, "help")) {
        top_usage();
        return 0;
    }
    if (!strcmp(cmd, "init")) {
        return cmd_init(sub_argc, sub_argv, root);
    }

    store_t s;
    if (store_open(&s, root) != 0) {
        warnx_("no overlayd store; run `overlayd init` first");
        return 1;
    }

    if (!strcmp(cmd, "layer")) return cmd_layer(sub_argc, sub_argv, &s);
    if (!strcmp(cmd, "ws") || !strcmp(cmd, "workspace")) return cmd_ws(sub_argc, sub_argv, &s);
    if (!strcmp(cmd, "materialize")) return cmd_materialize(sub_argc, sub_argv, &s);

    warnx_("unknown command: %s", cmd);
    top_usage();
    return 2;
}
