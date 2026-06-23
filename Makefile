# Makefile for BGTK

# Base flags. Freetype path is discovered via pkg-config when available
# (works on Linux and on macOS with Homebrew). The hardcoded fallback is
# the common Linux location.
FREETYPE_CFLAGS := $(shell pkg-config --cflags freetype2 2>/dev/null)
ifeq ($(FREETYPE_CFLAGS),)
  FREETYPE_CFLAGS := -I/usr/include/freetype2
endif

FREETYPE_LIBS := $(shell pkg-config --libs freetype2 2>/dev/null)
FREETYPE_LIBDIRS :=
ifeq ($(FREETYPE_LIBS),)
  # Common fallbacks: Linux default + macOS Homebrew (Apple Silicon + Intel)
  FREETYPE_LIBS := -lfreetype
  FREETYPE_LIBDIRS := -L/opt/homebrew/opt/freetype/lib -L/usr/local/opt/freetype/lib -L/opt/homebrew/lib -L/usr/local/lib
endif

OPENSSL_CFLAGS := $(shell pkg-config --cflags openssl 2>/dev/null)
ifeq ($(OPENSSL_CFLAGS),)
  OPENSSL_CFLAGS := -I/opt/homebrew/opt/openssl/include -I/opt/homebrew/opt/openssl@3/include -I/opt/homebrew/include -I/usr/local/opt/openssl/include
endif
OPENSSL_LIBDIRS := -L/opt/homebrew/opt/openssl/lib -L/opt/homebrew/opt/openssl@3/lib -L/opt/homebrew/lib -L/usr/local/opt/openssl/lib -L/usr/local/lib
OPENSSL_LIBS := -lssl -lcrypto

LIBXML2_CFLAGS := $(shell pkg-config --cflags libxml-2.0 2>/dev/null)
LIBXML2_LIBS := $(shell pkg-config --libs libxml-2.0 2>/dev/null)
ifeq ($(LIBXML2_LIBS),)
  LIBXML2_LIBS := -lxml2
endif

CFLAGS = -Wall -Wextra -Werror -I. $(FREETYPE_CFLAGS) $(LIBXML2_CFLAGS) -fPIC

# Full LDFLAGS are only for real (Linux + bgce server) binaries.
# Include detected FREETYPE paths so linking works on macOS (Homebrew)
# as well as Linux. FREETYPE_LIBDIRS may be empty when pkg-config succeeds
# (because pkg-config --libs usually includes -L...).
LDFLAGS = $(FREETYPE_LIBDIRS) $(FREETYPE_LIBS) $(LIBXML2_LIBS) -lbgce -lm

TARGET = libbgtk.so test_app image_viewer launcher terminal gemini_browser headless
# On macOS (Darwin), plain `make` will try to build libbgtk.so and real
# apps which require the bgce library (Linux-specific). Default to
# headless/test targets so `make` succeeds for development on mac.
# You can still do `make terminal` (or the others) explicitly if you have
# a bgce build for your platform.
ifeq ($(shell uname),Darwin)
  TARGET = headless test_terminal test_html test_settings
endif
INSTALL_LIB = /usr/lib
INSTALL_INCLUDE = /usr/include

SRC = bgtk.c drawing.c widgets.c config.c html.c
LIB_OBJS = bgtk.o drawing.o widgets.o config.o html.o
TEST_APP_OBJ = apps/test_app.o
IMAGE_VIEWER_OBJ = apps/image_viewer.o
LAUNCHER_OBJ = apps/launcher.o
TERMINAL_OBJ = apps/terminal.o
TERM_CORE_OBJ = apps/term_core.o
GEMINI_BROWSER_OBJ = apps/gemini_browser.o

# ---------- Headless / mock testing support ----------
# "make headless" builds a standalone binary that uses bgtk_init_mock +
# bgtk_write_frame_png + bgtk_inject_event. It does not require a running
# bgce server or Linux input/DRI headers at runtime.
#
# On macOS (or any dev machine) this is the intended way to exercise the
# widget tree and visually inspect output by looking at the generated PNGs.
COMPAT_DIR := compat
HEADLESS_CFLAGS := $(CFLAGS) -I$(COMPAT_DIR)
HEADLESS_LDFLAGS := $(FREETYPE_LIBS) $(FREETYPE_LIBDIRS) $(LIBXML2_LIBS) -lm
HEADLESS_STUB := $(COMPAT_DIR)/bgce_stub.o
HEADLESS_OBJ := test/headless.o
TEST_TERMINAL_OBJ := test/test_terminal.o
TEST_GEMINI_OBJ := test/test_gemini_browser.o
TEST_HTML_OBJ := test/test_html.o
SETTINGS_OBJ := apps/settings.o
TEST_SETTINGS_OBJ := test/test_settings.o

.PHONY: all clean test

all: $(TARGET)

libbgtk.so: $(LIB_OBJS)
	$(CC) -shared -o $@ $(LIB_OBJS) $(LDFLAGS)

test_app: $(TEST_APP_OBJ) $(LIB_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

image_viewer: $(IMAGE_VIEWER_OBJ) $(LIB_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

launcher: $(LAUNCHER_OBJ) $(LIB_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

gemini_browser: $(GEMINI_BROWSER_OBJ) $(LIB_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS) $(OPENSSL_LIBDIRS) $(OPENSSL_LIBS)

terminal: $(TERMINAL_OBJ) $(TERM_CORE_OBJ) $(LIB_OBJS)
	$(CC) $(CFLAGS) -Iapps -o $@ $^ $(LDFLAGS)

# Headless target links the bgce stub instead of the real library and
# pulls in the compat headers via -I so that <bgce.h> and <linux/input.h>
# resolve on non-Linux platforms.
#
# We attach -Icompat to the specific object files only when they are
# prerequisites of the "headless" target. This way normal Linux server
# binaries (test_app, image_viewer, ...) continue to use the real system
# bgce headers if present.
$(HEADLESS_OBJ) $(LIB_OBJS) $(HEADLESS_STUB) $(TEST_TERMINAL_OBJ) $(TERMINAL_OBJ) $(TERM_CORE_OBJ) $(TEST_GEMINI_OBJ) $(TEST_HTML_OBJ) $(SETTINGS_OBJ) $(TEST_SETTINGS_OBJ) apps/settings_test.o: CFLAGS += -I$(COMPAT_DIR)

$(HEADLESS_STUB): $(COMPAT_DIR)/bgce_stub.c
	$(CC) $(HEADLESS_CFLAGS) -c -o $@ $<

headless: $(HEADLESS_OBJ) $(LIB_OBJS) $(HEADLESS_STUB)
	$(CC) $(HEADLESS_CFLAGS) -o $@ $^ $(HEADLESS_LDFLAGS)

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

$(GEMINI_BROWSER_OBJ): CFLAGS += -I$(COMPAT_DIR) $(OPENSSL_CFLAGS)

test_terminal: $(TEST_TERMINAL_OBJ) $(TERM_CORE_OBJ) $(LIB_OBJS) $(HEADLESS_STUB)
	$(CC) $(HEADLESS_CFLAGS) -Iapps -o $@ $^ $(HEADLESS_LDFLAGS)

test_gemini_browser: $(TEST_GEMINI_OBJ) $(LIB_OBJS) $(HEADLESS_STUB)
	$(CC) $(HEADLESS_CFLAGS) $(OPENSSL_CFLAGS) -o $@ $^ $(HEADLESS_LDFLAGS) $(OPENSSL_LIBDIRS) $(OPENSSL_LIBS)

test_html: $(TEST_HTML_OBJ) $(LIB_OBJS) $(HEADLESS_STUB)
	$(CC) $(HEADLESS_CFLAGS) -o $@ $^ $(HEADLESS_LDFLAGS)

# Settings app (real BGCE)
settings: $(SETTINGS_OBJ) $(LIB_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

# Settings headless test: compile apps/settings.c with SETTINGS_TEST_MODE
# so its main() is excluded, then link test_settings.c as the driver.
SETTINGS_TEST_OBJ := apps/settings_test.o

$(SETTINGS_TEST_OBJ): apps/settings.c
	$(CC) $(CFLAGS) -DSETTINGS_TEST_MODE -c -o $@ $<

$(TEST_SETTINGS_OBJ): test/test_settings.c
	$(CC) $(CFLAGS) -c -o $@ $<

test_settings: $(TEST_SETTINGS_OBJ) $(SETTINGS_TEST_OBJ) $(LIB_OBJS) $(HEADLESS_STUB)
	$(CC) $(HEADLESS_CFLAGS) -o $@ $^ $(HEADLESS_LDFLAGS)

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
	rm -f $(TARGET) terminal test_terminal test_gemini_browser gemini_browser test_html test_settings settings $(LIB_OBJS) $(IMAGE_VIEWER_OBJ) $(TEST_APP_OBJ) $(LAUNCHER_OBJ) $(TERMINAL_OBJ) $(TERM_CORE_OBJ) $(GEMINI_BROWSER_OBJ) $(HEADLESS_OBJ) $(HEADLESS_STUB) $(TEST_TERMINAL_OBJ) $(TEST_GEMINI_OBJ) $(TEST_HTML_OBJ) $(SETTINGS_OBJ) $(TEST_SETTINGS_OBJ) $(SETTINGS_TEST_OBJ)


.PHONY: install
install: libbgtk.so bgtk.h
	install -d $(INSTALL_LIB)
	install -m 755 libbgtk.so $(INSTALL_LIB)
	install -d $(INSTALL_INCLUDE)
	install -m 644 bgtk.h $(INSTALL_INCLUDE)
	ldconfig

.PHONY: test
test: test_app image_viewer
	@echo "Starting BGCE server in background..."
	bgce > server.log 2>&1 &
	BGCE_PID=$$
	@echo "Running app..."
	./test_app > app.log 2>&1 &
	sleep 3
	./image_viewer > image_viewer.log 2>&1 || true
	@echo "Killing BGCE server (PID: $$BGCE_PID)..."
	kill $$BGCE_PID 2>/dev/null || true
	@echo "Test complete."

