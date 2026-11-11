# Turrón — reproductor de vídeo ligero
PREFIX ?= $(HOME)/.local
NATIVE ?= 1
BUILD  := build

PKGS := libavformat libavcodec libavutil libswresample libswscale libass libpipewire-0.3 \
        wayland-client wayland-egl egl glesv2 xkbcommon libsystemd
WP   := $(shell pkg-config --variable=pkgdatadir wayland-protocols)

CFLAGS  ?= -O2 -pipe
CFLAGS  += -std=gnu11 -Wall -Wextra -Wno-unused-parameter -pthread -I$(BUILD) $(shell pkg-config --cflags $(PKGS))
ifeq ($(NATIVE),1)
CFLAGS  += -march=native
endif
LDLIBS  := $(shell pkg-config --libs $(PKGS)) -lm -pthread

PROTOS := xdg-shell viewporter fractional-scale-v1 idle-inhibit-unstable-v1 cursor-shape-v1 tablet-v2
XML_xdg-shell := $(WP)/stable/xdg-shell/xdg-shell.xml
XML_viewporter := $(WP)/stable/viewporter/viewporter.xml
XML_fractional-scale-v1 := $(WP)/staging/fractional-scale/fractional-scale-v1.xml
XML_idle-inhibit-unstable-v1 := $(WP)/unstable/idle-inhibit/idle-inhibit-unstable-v1.xml
XML_cursor-shape-v1 := $(WP)/staging/cursor-shape/cursor-shape-v1.xml
XML_tablet-v2 := $(firstword $(wildcard $(WP)/stable/tablet/tablet-v2.xml $(WP)/unstable/tablet/tablet-unstable-v2.xml))

SRCS := main player audio gl wl osd mpris
OBJS := $(SRCS:%=$(BUILD)/%.o) $(PROTOS:%=$(BUILD)/%.o)
HDRS := $(PROTOS:%=$(BUILD)/%.h)

all: $(BUILD)/turron

$(BUILD):
	mkdir -p $(BUILD)

define PROTO_RULE
$(BUILD)/$(1).h: $$(XML_$(1)) | $(BUILD)
	wayland-scanner client-header $$< $$@
$(BUILD)/$(1).c: $$(XML_$(1)) | $(BUILD)
	wayland-scanner private-code $$< $$@
endef
$(foreach p,$(PROTOS),$(eval $(call PROTO_RULE,$(p))))

$(BUILD)/%.o: src/%.c src/turron.h $(HDRS) | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@
$(BUILD)/%.o: $(BUILD)/%.c | $(BUILD)
	$(CC) $(CFLAGS) -Wno-unused-variable -c $< -o $@

$(BUILD)/turron: $(OBJS)
	$(CC) $(CFLAGS) $(OBJS) $(LDLIBS) -o $@

install: all
	install -Dm755 $(BUILD)/turron $(DESTDIR)$(PREFIX)/bin/turron
	install -Dm644 data/turron.desktop $(DESTDIR)$(PREFIX)/share/applications/turron.desktop
	install -Dm644 data/turron.svg $(DESTDIR)$(PREFIX)/share/icons/hicolor/scalable/apps/turron.svg
	-update-desktop-database -q $(DESTDIR)$(PREFIX)/share/applications 2>/dev/null
	-gtk-update-icon-cache -q -t -f $(DESTDIR)$(PREFIX)/share/icons/hicolor 2>/dev/null

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/turron $(DESTDIR)$(PREFIX)/share/applications/turron.desktop \
	      $(DESTDIR)$(PREFIX)/share/icons/hicolor/scalable/apps/turron.svg

clean:
	rm -rf $(BUILD)

.PHONY: all install uninstall clean
