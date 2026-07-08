# Makefile for BGTK

# Base flags. Freetype path is discovered via pkg-config when available
# (works on Linux and on macOS with Homebrew). The hardcoded fallback is
# the common Linux location.
CC ?= cc

# User-local prefix (bgce, freetype, libxml, etc. under a home install).
HOME_LOCAL ?= $(HOME)/.local
# FreeType headers live in include/freetype2, not only include/.
HOME_LOCAL_CFLAGS = -I$(HOME_LOCAL)/include -I$(HOME_LOCAL)/include/freetype2 \
	-I$(HOME_LOCAL)/include/libxml2
HOME_LOCAL_LDFLAGS = -L$(HOME_LOCAL)/lib
# So pkg-config finds .pc files from a home-prefix install.
PKG_CONFIG_ENV = PKG_CONFIG_PATH="$(HOME_LOCAL)/lib/pkgconfig:$(HOME_LOCAL)/share/pkgconfig:$${PKG_CONFIG_PATH}"

FREETYPE_CFLAGS := $(shell $(PKG_CONFIG_ENV) pkg-config --cflags freetype2 2>/dev/null)
ifeq ($(FREETYPE_CFLAGS),)
  FREETYPE_CFLAGS := -I$(HOME_LOCAL)/include/freetype2 -I$(HOME_LOCAL)/include \
	-I/include/freetype2
endif

FREETYPE_LIBS := $(shell $(PKG_CONFIG_ENV) pkg-config --libs freetype2 2>/dev/null)
# Always search home lib for -lfreetype even when pkg-config returns flags.
FREETYPE_LIBDIRS := -L$(HOME_LOCAL)/lib
ifeq ($(FREETYPE_LIBS),)
  # Flat /lib (lin0) plus macOS Homebrew fallbacks for dev hosts
  FREETYPE_LIBS := -lfreetype
  FREETYPE_LIBDIRS += -L/lib -L/opt/homebrew/opt/freetype/lib -L/usr/local/opt/freetype/lib -L/opt/homebrew/lib -L/usr/local/lib
endif

# libtls (LibreSSL / OpenBSD / libretls). Prefer pkg-config; fall back to home/local.
LIBTLS_CFLAGS := $(shell $(PKG_CONFIG_ENV) pkg-config --cflags libtls 2>/dev/null)
ifeq ($(LIBTLS_CFLAGS),)
  LIBTLS_CFLAGS := $(shell $(PKG_CONFIG_ENV) pkg-config --cflags libretls 2>/dev/null)
endif
ifeq ($(LIBTLS_CFLAGS),)
  LIBTLS_CFLAGS := -I$(HOME_LOCAL)/include -I/usr/include -I/usr/local/include \
	-I/opt/homebrew/opt/libretls/include -I/opt/homebrew/include
endif
LIBTLS_LIBS := $(shell $(PKG_CONFIG_ENV) pkg-config --libs libtls 2>/dev/null)
ifeq ($(LIBTLS_LIBS),)
  LIBTLS_LIBS := $(shell $(PKG_CONFIG_ENV) pkg-config --libs libretls 2>/dev/null)
endif
ifeq ($(LIBTLS_LIBS),)
  LIBTLS_LIBS := -L$(HOME_LOCAL)/lib -L/usr/local/lib -L/opt/homebrew/opt/libretls/lib \
	-L/opt/homebrew/lib -ltls
endif

LIBXML2_CFLAGS := $(shell $(PKG_CONFIG_ENV) pkg-config --cflags libxml-2.0 2>/dev/null)
ifeq ($(LIBXML2_CFLAGS),)
  LIBXML2_CFLAGS := -I$(HOME_LOCAL)/include/libxml2 -I$(HOME_LOCAL)/include \
	-I/include/libxml2
endif
LIBXML2_LIBS := $(shell $(PKG_CONFIG_ENV) pkg-config --libs libxml-2.0 2>/dev/null)
ifeq ($(LIBXML2_LIBS),)
  LIBXML2_LIBS := -L$(HOME_LOCAL)/lib -lxml2
endif

COMPAT_DIR := compat
UNAME_S := $(shell uname)

# Base compile flags. Do NOT put -fPIC here: TinyCC errors with
# "section type conflict: .symtab 01 <> 02" when linking many PIC
# relocatable objects into a normal executable. PIC is only for lib objs.
CFLAGS = -Wall -Wextra -Werror -I. $(HOME_LOCAL_CFLAGS) $(FREETYPE_CFLAGS) $(LIBXML2_CFLAGS)

# On non-Linux (no system bgce / linux/input.h), use the in-tree stubs.
ifneq ($(UNAME_S),Linux)
  CFLAGS += -I$(COMPAT_DIR)
endif

# Full LDFLAGS are only for real (Linux + bgce server) binaries.
LDFLAGS = $(HOME_LOCAL_LDFLAGS) $(FREETYPE_LIBDIRS) $(FREETYPE_LIBS) $(LIBXML2_LIBS) -lbgce -lm
# Prefer in-tree libbgtk.so when linking apps (before any system copy).
# Runtime search is left to the dynamic linker / LD_LIBRARY_PATH (e.g. home
# PREFIX installs); we do not bake rpath into the binaries.
APP_LDFLAGS = -L. -lbgtk $(LDFLAGS)

# openpty() lives in libutil on Linux; on macOS it is in libSystem.
PTY_LIBS :=
ifeq ($(UNAME_S),Linux)
  PTY_LIBS := -lutil
endif

# Real BGCE apps (install always builds these so a lib-only install cannot
# leave stale binaries against a newer libbgtk.so / struct layout).
# gemini_browser needs libtls — built last and optional on install.
CORE_APPS = test_app image_viewer launcher terminal settings sys_status
ALL_APPS = $(CORE_APPS) gemini_browser

# Default `make` builds the library and core apps. settings is intentionally
# before gemini_browser so a missing libtls does not skip it.
# gemini_browser needs libtls; build it last (or: make gemini_browser).
TARGET = libbgtk.so $(ALL_APPS)
# On macOS (Darwin), plain `make` will try to build libbgtk.so and real
# apps which require the bgce library (Linux-specific). Default to
# headless/test targets so `make` succeeds for development on mac.
ifeq ($(UNAME_S),Darwin)
  TARGET = headless test_terminal test_html test_settings test_theme_gallery test_sys_status
endif

# Install layout. Empty PREFIX → flat root (/lib, /include, /bin) for lin0.
# Example: make install PREFIX=/usr/local
PREFIX ?=
INSTALL_LIB ?= $(PREFIX)/lib
INSTALL_INCLUDE ?= $(PREFIX)/include
INSTALL_BIN ?= $(PREFIX)/bin

SRC = bgtk.c drawing.c widgets.c config.c html.c keys.c
LIB_OBJS = bgtk.o drawing.o widgets.o config.o html.o keys.o
TEST_APP_OBJ = apps/test_app.o
IMAGE_VIEWER_OBJ = apps/image_viewer.o
LAUNCHER_OBJ = apps/launcher.o
TERMINAL_OBJ = apps/terminal.o
TERM_CORE_OBJ = apps/term_core.o
GEMINI_BROWSER_OBJ = apps/gemini_browser.o

# ---------- Headless / mock testing support ----------
# Headless links the bgce stub and does not need a running server.
HEADLESS_CFLAGS := $(CFLAGS) -I$(COMPAT_DIR)
HEADLESS_LDFLAGS := -L. $(HOME_LOCAL_LDFLAGS) $(FREETYPE_LIBS) $(FREETYPE_LIBDIRS) $(LIBXML2_LIBS) -lm
HEADLESS_STUB := $(COMPAT_DIR)/bgce_stub.o
HEADLESS_OBJ := test/headless.o
TEST_TERMINAL_OBJ := test/test_terminal.o
TEST_GEMINI_OBJ := test/test_gemini_browser.o
TEST_HTML_OBJ := test/test_html.o
SETTINGS_OBJ := apps/settings.o
TEST_SETTINGS_OBJ := test/test_settings.o
TEST_THEME_GALLERY_OBJ := test/test_theme_gallery.o
SETTINGS_TEST_OBJ := apps/settings_test.o
SYS_STATUS_OBJ := apps/sys_status.o
TEST_SYS_STATUS_OBJ := test/test_sys_status.o
SYS_STATUS_TEST_OBJ := apps/sys_status_test.o

.PHONY: all clean test install help snapshot

all: $(TARGET)

# Print targets and overrideable variables. Default goal remains `all`;
# run explicitly: make help
help:
	@echo "BGTK — Brian's Graphical Toolkit"
	@echo ""
	@echo "Usage:  make [target] [VAR=value ...]"
	@echo ""
	@echo "Default on this host ($(UNAME_S)):"
	@echo "  all → $(TARGET)"
	@echo ""
	@echo "Targets"
	@echo "  help                 Show this help"
	@echo "  all                  Default build for this platform (see above)"
	@echo ""
	@echo "  Library / real apps (need BGCE on Linux)"
	@echo "  libbgtk.so           Shared library"
	@echo "  test_app             Demo widget app"
	@echo "  image_viewer         Image viewer"
	@echo "  launcher             Application launcher (exits after spawn)"
	@echo "  terminal             Terminal emulator (PTY)"
	@echo "  settings             Theme/font/background settings UI"
	@echo "  sys_status           System status (CPU/mem/disk/net/weather)"
	@echo "  gemini_browser       Gemini browser (needs libtls)"
	@echo ""
	@echo "  Headless / mock tests (no BGCE server; produce PNGs)"
	@echo "  headless             Basic widgets + input screenshots"
	@echo "  test_terminal        Terminal/ANSI (+ optional real PTY) screenshots"
	@echo "  test_html            HTML → widget tree screenshots"
	@echo "  test_settings        Settings UI screenshots"
	@echo "  test_theme_gallery   Settings under candidate themes (settings_theme_*.png)"
	@echo "  test_gemini_browser  Gemini browser flow screenshots (libtls for live fetch)"
	@echo ""
	@echo "  Maintenance"
	@echo "  install              Install lib, header, and built apps"
	@echo "  clean                Remove binaries and objects"
	@echo "  test                 Smoke-run test_app + image_viewer under bgce"
	@echo "  snapshot             Regenerate www/bgtk.tar.gz from HEAD"
	@echo ""
	@echo "Variables (override on the command line)"
	@echo "  CC=$(CC)"
	@echo "  CFLAGS=$(CFLAGS)"
	@echo "  HOME_LOCAL=$(HOME_LOCAL)"
	@echo "    compile: -I\$$HOME_LOCAL/include{,/freetype2,/libxml2}"
	@echo "    link:    -L\$$HOME_LOCAL/lib  (and pkg-config under lib/pkgconfig)"
	@echo "  PREFIX=$(if $(PREFIX),$(PREFIX),(empty → flat /lib /include /bin))"
	@echo "  INSTALL_LIB=$(INSTALL_LIB)"
	@echo "  INSTALL_INCLUDE=$(INSTALL_INCLUDE)"
	@echo "  INSTALL_BIN=$(INSTALL_BIN)"
	@echo ""
	@echo "Examples"
	@echo "  make"
	@echo "  make CC=cc"
	@echo "  make terminal settings"
	@echo "  make headless && ./headless"
	@echo "  make install"
	@echo "  make install PREFIX=/usr/local"
	@echo "  make install PREFIX=\$$HOME/.local"
	@echo "  make HOME_LOCAL=\$$HOME/.local"
	@echo "  make install INSTALL_LIB=/opt/bgtk/lib INSTALL_INCLUDE=/opt/bgtk/include"
	@echo "  make snapshot"

# Shared library objects need -fPIC; app objects must not (TinyCC).
$(LIB_OBJS): CFLAGS += -fPIC

libbgtk.so: $(LIB_OBJS)
	$(CC) -shared -o $@ $(LIB_OBJS) $(LDFLAGS)

# Real apps: link against libbgtk.so instead of re-merging every library
# .o into the binary. Avoids TinyCC .symtab section-type conflicts and
# matches how users will link after `make install`.
test_app: $(TEST_APP_OBJ) libbgtk.so
	$(CC) -o $@ $(TEST_APP_OBJ) $(APP_LDFLAGS)

image_viewer: $(IMAGE_VIEWER_OBJ) libbgtk.so
	$(CC) -o $@ $(IMAGE_VIEWER_OBJ) $(APP_LDFLAGS)

launcher: $(LAUNCHER_OBJ) libbgtk.so
	$(CC) -o $@ $(LAUNCHER_OBJ) $(APP_LDFLAGS)

gemini_browser: $(GEMINI_BROWSER_OBJ) libbgtk.so
	$(CC) -o $@ $(GEMINI_BROWSER_OBJ) $(APP_LDFLAGS) $(LIBTLS_LIBS)

terminal: $(TERMINAL_OBJ) $(TERM_CORE_OBJ) libbgtk.so
	$(CC) -o $@ $(TERMINAL_OBJ) $(TERM_CORE_OBJ) $(APP_LDFLAGS) $(PTY_LIBS)

settings: $(SETTINGS_OBJ) libbgtk.so
	$(CC) -o $@ $(SETTINGS_OBJ) $(APP_LDFLAGS)

sys_status: $(SYS_STATUS_OBJ) libbgtk.so
	$(CC) -o $@ $(SYS_STATUS_OBJ) $(APP_LDFLAGS)

# Headless targets still link LIB_OBJS + stub directly (no libbgce).
# Rebuild lib objs with -Icompat only for those units when on Linux so
# mock/stub headers are available; on Darwin CFLAGS already has -Icompat.
$(HEADLESS_OBJ) $(HEADLESS_STUB) $(TEST_TERMINAL_OBJ) $(TEST_GEMINI_OBJ) $(TEST_HTML_OBJ) $(TEST_SETTINGS_OBJ) $(TEST_THEME_GALLERY_OBJ) $(SETTINGS_TEST_OBJ) $(TEST_SYS_STATUS_OBJ) $(SYS_STATUS_TEST_OBJ): CFLAGS += -I$(COMPAT_DIR)

$(HEADLESS_STUB): $(COMPAT_DIR)/bgce_stub.c
	$(CC) $(HEADLESS_CFLAGS) -c -o $@ $<

# Headless lib objects: same sources, but ensure compat is visible on Linux.
# We reuse $(LIB_OBJS); on Linux add -Icompat only when building headless
# tests by rebuilding through a separate stamp is heavy — instead headless
# on Linux uses system bgce headers (fine: only types) + stub for link.
headless: $(HEADLESS_OBJ) $(LIB_OBJS) $(HEADLESS_STUB)
	$(CC) -o $@ $(HEADLESS_OBJ) $(LIB_OBJS) $(HEADLESS_STUB) $(HEADLESS_LDFLAGS)

# Terminal objects need -Iapps for terminal.h
$(TERMINAL_OBJ): apps/terminal.c apps/terminal.h
	$(CC) $(CFLAGS) -Iapps -c -o $@ $<

$(TERM_CORE_OBJ): apps/term_core.c apps/terminal.h
	$(CC) $(CFLAGS) -Iapps -c -o $@ $<

$(TEST_TERMINAL_OBJ): test/test_terminal.c apps/terminal.h
	$(CC) $(CFLAGS) -Iapps -c -o $@ $<

$(TEST_GEMINI_OBJ): CFLAGS += $(LIBTLS_CFLAGS)
$(TEST_GEMINI_OBJ): test/test_gemini_browser.c
	$(CC) $(CFLAGS) -c -o $@ $<

$(GEMINI_BROWSER_OBJ): CFLAGS += $(LIBTLS_CFLAGS)

test_terminal: $(TEST_TERMINAL_OBJ) $(TERM_CORE_OBJ) $(LIB_OBJS) $(HEADLESS_STUB)
	$(CC) -o $@ $(TEST_TERMINAL_OBJ) $(TERM_CORE_OBJ) $(LIB_OBJS) $(HEADLESS_STUB) $(HEADLESS_LDFLAGS) $(PTY_LIBS)

test_gemini_browser: $(TEST_GEMINI_OBJ) $(LIB_OBJS) $(HEADLESS_STUB)
	$(CC) -o $@ $(TEST_GEMINI_OBJ) $(LIB_OBJS) $(HEADLESS_STUB) $(HEADLESS_LDFLAGS) $(LIBTLS_LIBS)

test_html: $(TEST_HTML_OBJ) $(LIB_OBJS) $(HEADLESS_STUB)
	$(CC) -o $@ $(TEST_HTML_OBJ) $(LIB_OBJS) $(HEADLESS_STUB) $(HEADLESS_LDFLAGS)

# Settings headless test: compile apps/settings.c with SETTINGS_TEST_MODE
# so its main() is excluded, then link test_settings.c as the driver.
$(SETTINGS_TEST_OBJ): apps/settings.c
	$(CC) $(CFLAGS) -DSETTINGS_TEST_MODE -c -o $@ $<

$(TEST_SETTINGS_OBJ): test/test_settings.c
	$(CC) $(CFLAGS) -c -o $@ $<

test_settings: $(TEST_SETTINGS_OBJ) $(SETTINGS_TEST_OBJ) $(LIB_OBJS) $(HEADLESS_STUB)
	$(CC) -o $@ $(TEST_SETTINGS_OBJ) $(SETTINGS_TEST_OBJ) $(LIB_OBJS) $(HEADLESS_STUB) $(HEADLESS_LDFLAGS)

# Theme gallery: same settings object, different driver
$(TEST_THEME_GALLERY_OBJ): test/test_theme_gallery.c
	$(CC) $(CFLAGS) -c -o $@ $<

test_theme_gallery: $(TEST_THEME_GALLERY_OBJ) $(SETTINGS_TEST_OBJ) $(LIB_OBJS) $(HEADLESS_STUB)
	$(CC) -o $@ $(TEST_THEME_GALLERY_OBJ) $(SETTINGS_TEST_OBJ) $(LIB_OBJS) $(HEADLESS_STUB) $(HEADLESS_LDFLAGS)

# System status headless test
$(SYS_STATUS_TEST_OBJ): apps/sys_status.c
	$(CC) $(CFLAGS) -DSYS_STATUS_TEST_MODE -c -o $@ $<

$(TEST_SYS_STATUS_OBJ): test/test_sys_status.c
	$(CC) $(CFLAGS) -c -o $@ $<

test_sys_status: $(TEST_SYS_STATUS_OBJ) $(SYS_STATUS_TEST_OBJ) $(LIB_OBJS) $(HEADLESS_STUB)
	$(CC) -o $@ $(TEST_SYS_STATUS_OBJ) $(SYS_STATUS_TEST_OBJ) $(LIB_OBJS) $(HEADLESS_STUB) $(HEADLESS_LDFLAGS)


# Header dependencies so touching a .h triggers recompilation.
CORE_HEADERS = bgtk.h internal.h config.h
$(LIB_OBJS): $(CORE_HEADERS)
keys.o: bgtk.h
html.o: html.h
$(TEST_HTML_OBJ): html.h bgtk.h
$(HEADLESS_OBJ): bgtk.h
$(SETTINGS_OBJ): html.h bgtk.h config.h
$(SETTINGS_TEST_OBJ): html.h bgtk.h config.h
$(TEST_SETTINGS_OBJ): html.h bgtk.h config.h
$(TEST_THEME_GALLERY_OBJ): html.h bgtk.h config.h

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(TARGET) $(ALL_APPS) terminal test_terminal test_gemini_browser \
		test_html test_settings test_theme_gallery test_sys_status headless \
		$(LIB_OBJS) $(IMAGE_VIEWER_OBJ) $(TEST_APP_OBJ) $(LAUNCHER_OBJ) $(TERMINAL_OBJ) $(TERM_CORE_OBJ) \
		$(GEMINI_BROWSER_OBJ) $(HEADLESS_OBJ) $(HEADLESS_STUB) $(TEST_TERMINAL_OBJ) $(TEST_GEMINI_OBJ) \
		$(TEST_HTML_OBJ) $(SETTINGS_OBJ) $(TEST_SETTINGS_OBJ) $(TEST_THEME_GALLERY_OBJ) $(SETTINGS_TEST_OBJ) \
		$(SYS_STATUS_OBJ) $(TEST_SYS_STATUS_OBJ) $(SYS_STATUS_TEST_OBJ) libbgtk.so

# Rebuild core apps before install. After BGTK_Context layout changes, a
# lib-only install leaves blank windows (apps write root_widget at wrong offset).
# gemini_browser is best-effort (needs libtls).
install: libbgtk.so bgtk.h $(CORE_APPS)
	-$(MAKE) gemini_browser
	install -d $(INSTALL_LIB)
	install -m 755 libbgtk.so $(INSTALL_LIB)
	install -d $(INSTALL_INCLUDE)
	install -m 644 bgtk.h $(INSTALL_INCLUDE)
	-install -d $(INSTALL_BIN)
	-for f in $(ALL_APPS); do \
		if [ -f $$f ]; then install -m 755 $$f $(INSTALL_BIN); fi; \
	done
	-ldconfig 2>/dev/null || true

test: test_app image_viewer
	@echo "Starting BGCE server in background..."
	bgce > server.log 2>&1 &
	BGCE_PID=$$!
	@echo "Running app..."
	./test_app > app.log 2>&1 &
	sleep 3
	./image_viewer > image_viewer.log 2>&1 || true
	@echo "Killing BGCE server (PID: $$BGCE_PID)..."
	kill $$BGCE_PID 2>/dev/null || true
	@echo "Test complete."

# Source tarball for the project site (excludes itself via .gitattributes).
snapshot:
	git archive --worktree-attributes --format=tar.gz --prefix=bgtk/ \
		-o www/bgtk.tar.gz HEAD
	@ls -lh www/bgtk.tar.gz
