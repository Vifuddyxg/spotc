CC      ?= cc
PKGS     = ncursesw json-c libcurl openssl
CFLAGS  += -O2 -Wall -Wextra -std=c11 -D_DEFAULT_SOURCE $(shell pkg-config --cflags $(PKGS))
LDLIBS   = $(shell pkg-config --libs $(PKGS))
PREFIX  ?= $(HOME)/.local

OBJ = src/main.o src/ui.o src/api.o src/auth.o src/player.o src/config.o

all: spotc spotc-fx spotc-ipv4.so

spotc: $(OBJ)
	$(CC) -o $@ $(OBJ) $(LDLIBS)

$(OBJ): src/spotc.h

spotc-fx: src/fx.c
	$(CC) $(CFLAGS) -o $@ src/fx.c -lm

spotc-ipv4.so: src/ipv4.c
	$(CC) -O2 -shared -fPIC -o $@ src/ipv4.c -ldl

install: all
	install -Dm755 spotc $(PREFIX)/bin/spotc
	install -Dm755 spotc-fx $(PREFIX)/bin/spotc-fx
	install -Dm755 spotc-ipv4.so $(PREFIX)/bin/spotc-ipv4.so

clean:
	rm -f spotc spotc-fx spotc-ipv4.so src/*.o

.PHONY: all install clean
