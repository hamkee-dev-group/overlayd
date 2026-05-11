#include "cmd.h"
#include "store.h"
#include "util.h"

#include <stdio.h>
#include <string.h>

static void usage(void) {
    fprintf(stderr,
        "usage: overlayd init [path]\n"
        "\n"
        "  Initialize an overlayd store at the given path (default $OVERLAYD_ROOT\n"
        "  or ./.overlayd). Safe to re-run on an existing store.\n");
}

int cmd_init(int argc, char **argv, const char *root_override) {
    const char *path = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            usage();
            return 0;
        }
        if (argv[i][0] == '-') {
            usage();
            return 2;
        }
        if (!path) {
            path = argv[i];
        } else {
            usage();
            return 2;
        }
    }
    char def[4096];
    if (!path) {
        if (root_override && *root_override) {
            path = root_override;
        } else if (store_default_root(def, sizeof(def)) != 0) {
            warnx_("cannot resolve default root");
            return 1;
        } else {
            path = def;
        }
    }
    if (store_init(path) != 0) {
        warnx_("init %s: failed", path);
        return 1;
    }
    printf("%s\n", path);
    return 0;
}
