CC ?= cc
PKG_CONFIG ?= pkg-config
CFLAGS ?= -O2 -g
CPPFLAGS += -D_GNU_SOURCE -Isrc $(shell $(PKG_CONFIG) --cflags gtk4 libpulse libpulse-mainloop-glib)
CFLAGS += -std=c11 -Wall -Wextra -Wpedantic -Wformat=2 -Wshadow
LDLIBS += $(shell $(PKG_CONFIG) --libs gtk4 libpulse libpulse-mainloop-glib) -lgtk4-layer-shell -lm

SOURCES := src/main.c src/session.c src/capture.c src/storage.c src/ui.c src/reveal.c
OBJECTS := $(SOURCES:.c=.o)

.PHONY: all clean
all: omarchy-desktop-recorder

omarchy-desktop-recorder: $(OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $(OBJECTS) $(LDLIBS)

clean:
	rm -f $(OBJECTS) omarchy-desktop-recorder
