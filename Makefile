CC      ?= gcc
CFLAGS  ?= -O2 -g -Wall -Wextra -Wpedantic -Wshadow -Wstrict-prototypes \
           -D_GNU_SOURCE -std=c11
LDFLAGS ?=

PREFIX  ?= /usr/local
BINDIR  ?= $(PREFIX)/bin

SRC := \
	src/main.c \
	src/util.c \
	src/store.c \
	src/tar.c \
	src/cmd_init.c \
	src/cmd_layer.c \
	src/cmd_ws.c

OBJ := $(SRC:.c=.o)

all: overlayd

overlayd: $(OBJ)
	$(CC) $(CFLAGS) -o $@ $(OBJ) $(LDFLAGS)

src/%.o: src/%.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJ) overlayd

install: overlayd
	install -d $(DESTDIR)$(BINDIR)
	install -m 0755 overlayd $(DESTDIR)$(BINDIR)/overlayd

test: overlayd
	bash tests/test.sh
	bash tests/test_integration_flow.sh

.PHONY: all clean install test
