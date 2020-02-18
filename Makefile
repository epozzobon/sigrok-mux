CC=gcc
INCLUDE_FLAGS=-I/usr/include/glib-2.0 -I/usr/lib/x86_64-linux-gnu/glib-2.0/include/
PKG_CONFIG_LIBS=glib-2.0 libsigrok
PKG_CONFIG_CFLAGS=
PKG_CONFIG=$(shell pkg-config --cflags $(PKG_CONFIG_CFLAGS) --libs $(PKG_CONFIG_LIBS))
CFLAGS=-g -Wall -Wextra $(PKG_CONFIG) $(INCLUDE_FLAGS) -lpthread

all: build/sigrok-mux

build/sigrok-mux: build main.c capture.c capture.h
	$(CC) $(CFLAGS) main.c capture.c -o build/sigrok-mux

build:
	mkdir -p build/

clean:
	rm -rf build/


