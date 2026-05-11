#ifndef OVERLAYD_CMD_H
#define OVERLAYD_CMD_H

#include "store.h"

int cmd_init(int argc, char **argv, const char *root_override);
int cmd_layer(int argc, char **argv, store_t *s);
int cmd_ws(int argc, char **argv, store_t *s);
int cmd_materialize(int argc, char **argv, store_t *s);

#endif
