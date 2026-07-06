# Makefile for BGTK

# Base flags. Freetype path is discovered via pkg-config when available
# (works on Linux and on macOS with Homebrew). The hardcoded fallback is
# the common Linux location.
CC ?= cc

FREETYPE_CFLAGS := $(shell pkg-config --cflags freetype2 2>/dev/null)
ifeq ($(FREETYPE_CFLAGS),)
  FREETYPE_CFLAGS := -I/include/freetype2
endif

FREETYPE_LIBS := $(shell pkg-config --libs freetype2 2>/dev/null)
FREETYPE_LIBDIRS :=
ifeq ($(FREETYPE_LIBS),)
  # Flat /lib (lin0) plus macOS Homebrew fallbacks for dev hosts
  FREETYPE_LIBS := -lfreetype
  FREETYPE_LIBDIRS := -L/lib -L/opt/homebrew/opt/freetype/lib -L/usr/local/opt/freetype/lib -L/opt/homebrew/lib -L/usr/local/lib
endif

OPENSSL_CFLAGS := $(shell pkg-config --cflags openssl 2>/dev/null)
ifeq ($(OPENSSL_CFLAGS),)
  OPENSSL_CFLAGS := -I/opt/homebrew/opt/openssl/include -I/opt/homebrew/opt/openssl@3/include -I/opt/homebrew/include -I/usr/local/opt/openssl/include
endif
OPENSSL_LIBDIRS := -L/opt/homebrew/opt/openssl/lib -L/opt/homebrew/opt/openssl@3/lib -L/opt/homebrew/lib -L/usr/local/opt/openssl/lib -L/usr/local/lib
OPENSSL_LIBS := -lssl -lcrypto

LIBXML2_CFLAGS := $(shell pkg-config --cflags libxml-2.0 2>/dev/null)
ifeq ($(LIBXML2_CFLAGS),)
  LIBXML2_CFLAGS := -I/include/libxml2
endif
LIBXML2_LIBS := $(shell pkg-config --libs libxml-2.0 2>/dev/null)
ifeq ($(LIBXML2_LIBS),)
  LIBXML2_LIBS := -lxml2
endif

COMPAT_DIR := compat
UNAME_S := $(shell uname)

# Base compile flags. Do NOT put -fPIC here: TinyCC errors with
# "section type conflict: .symtab 01 <> 02" when linking many PIC
# relocatable objects into a normal executable. PIC is only for lib objs.
CFLAGS = -Wall -Wextra -Werror -I. $(FREETYPE_CFLAGS) $(LIBXML2_CFLAGS)

# On non-Linux (no system bgce / linux/input.h), use the in-tree stubs.
ifneq ($(UNAME_S),Linux)
  CFLAGS += -I$(COMPAT_DIR)
endif

# Full LDFLAGS are only for real (Linux + bgce server) binaries.
LDFLAGS = $(FREETYPE_LIBDIRS) $(FREETYPE_LIBS) $(LIBXML2_LIBS) -lbgce -lm
# Prefer in-tree libbgtk.so when linking apps (before any system copy).
APP_LDFLAGS = -L. -lbgtk $(LDFLAGS)

# openpty() lives in libutil on Linux; on macOS it is in libSystem.
PTY_LIBS :=
ifeq ($(UNAME_S),Linux)
  PTY_LIBS := -lutil
endif

TARGET = libbgtk.so test_app image_viewer launcher terminal gemini_browser settings
# On macOS (Darwin), plain `make` will try to build libbgtk.so and real
# apps which require the bgce library (Linux-specific). Default to
# headless/test targets so `make` succeeds for development on mac.
ifeq ($(UNAME_S),Darwin)
  TARGET = headless test_terminal test_html test_settings
endif
INSTALL_LIB = /lib
INSTALL_INCLUDE = /include
INSTALL_BIN = /bin

SRC = bgtk.c drawing.c widgets.c config.c html.c
LIB_OBJS = bgtk.o drawing.o widgets.o config.o html.o
TEST_APP_OBJ = apps/test_app.o
IMAGE_VIEWER_OBJ = apps/image_viewer.o
LAUNCHER_OBJ = apps/launcher.o
TERMINAL_OBJ = apps/terminal.o
TERM_CORE_OBJ = apps/term_core.o
GEMINI_BROWSER_OBJ = apps/gemini_browser.o

# ---------- Headless / mock testing support ----------
# Headless links the bgce stub and does not need a running server.
HEADLESS_CFLAGS := $(CFLAGS) -I$(COMPAT_DIR)
HEADLESS_LDFLAGS := -L. $(FREETYPE_LIBS) $(FREETYPE_LIBDIRS) $(LIBXML2_LIBS) -lm
HEADLESS_STUB := $(COMPAT_DIR)/bgce_stub.o
HEADLESS_OBJ := test/headless.o
TEST_TERMINAL_OBJ := test/test_terminal.o
TEST_GEMINI_OBJ := test/test_gemini_browser.o
TEST_HTML_OBJ := test/test_html.o
SETTINGS_OBJ := apps/settings.o
TEST_SETTINGS_OBJ := test/test_settings.o
SETTINGS_TEST_OBJ := apps/settings_test.o

.PHONY: all clean test install

all: $(TARGET)

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
	$(CC) -o $@ $(GEMINI_BROWSER_OBJ) $(APP_LDFLAGS) $(OPENSSL_LIBDIRS) $(OPENSSL_LIBS)

terminal: $(TERMINAL_OBJ) $(TERM_CORE_OBJ) libbgtk.so
	$(CC) -o $@ $(TERMINAL_OBJ) $(TERM_CORE_OBJ) $(APP_LDFLAGS) $(PTY_LIBS)

settings: $(SETTINGS_OBJ) libbgtk.so
	$(CC) -o $@ $(SETTINGS_OBJ) $(APP_LDFLAGS)

# Headless targets still link LIB_OBJS + stub directly (no libbgce).
# Rebuild lib objs with -Icompat only for those units when on Linux so
# mock/stub headers are available; on Darwin CFLAGS already has -Icompat.
$(HEADLESS_OBJ) $(HEADLESS_STUB) $(TEST_TERMINAL_OBJ) $(TEST_GEMINI_OBJ) $(TEST_HTML_OBJ) $(TEST_SETTINGS_OBJ) $(SETTINGS_TEST_OBJ): CFLAGS += -I$(COMPAT_DIR)

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

$(TEST_GEMINI_OBJ): CFLAGS += $(OPENSSL_CFLAGS)
$(TEST_GEMINI_OBJ): test/test_gemini_browser.c
	$(CC) $(CFLAGS) -c -o $@ $<

$(GEMINI_BROWSER_OBJ): CFLAGS += $(OPENSSL_CFLAGS)

test_terminal: $(TEST_TERMINAL_OBJ) $(TERM_CORE_OBJ) $(LIB_OBJS) $(HEADLESS_STUB)
	$(CC) -o $@ $(TEST_TERMINAL_OBJ) $(TERM_CORE_OBJ) $(LIB_OBJS) $(HEADLESS_STUB) $(HEADLESS_LDFLAGS) $(PTY_LIBS)

test_gemini_browser: $(TEST_GEMINI_OBJ) $(LIB_OBJS) $(HEADLESS_STUB)
	$(CC) -o $@ $(TEST_GEMINI_OBJ) $(LIB_OBJS) $(HEADLESS_STUB) $(HEADLESS_LDFLAGS) $(OPENSSL_LIBDIRS) $(OPENSSL_LIBS)

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

# Header dependencies so touching a .h triggers recompilation.
CORE_HEADERS = bgtk.h internal.h config.h
$(LIB_OBJS): $(CORE_HEADERS)
html.o: html.h
$(TEST_HTML_OBJ): html.h bgtk.h
$(HEADLESS_OBJ): bgtk.h
$(SETTINGS_OBJ): html.h bgtk.h config.h
$(SETTINGS_TEST_OBJ): html.h bgtk.h config.h
$(TEST_SETTINGS_OBJ): html.h bgtk.h config.h

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(TARGET) terminal test_terminal test_gemini_browser gemini_browser test_html test_settings settings \
		$(LIB_OBJS) $(IMAGE_VIEWER_OBJ) $(TEST_APP_OBJ) $(LAUNCHER_OBJ) $(TERMINAL_OBJ) $(TERM_CORE_OBJ) \
		$(GEMINI_BROWSER_OBJ) $(HEADLESS_OBJ) $(HEADLESS_STUB) $(TEST_TERMINAL_OBJ) $(TEST_GEMINI_OBJ) \
		$(TEST_HTML_OBJ) $(SETTINGS_OBJ) $(TEST_SETTINGS_OBJ) $(SETTINGS_TEST_OBJ) libbgtk.so

install: libbgtk.so bgtk.h
	install -d $(INSTALL_LIB)
	install -m 755 libbgtk.so $(INSTALL_LIB)
	install -d $(INSTALL_INCLUDE)
	install -m 644 bgtk.h $(INSTALL_INCLUDE)
	-install -d $(INSTALL_BIN)
	-for f in test_app image_viewer launcher terminal settings gemini_browser; do \
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
