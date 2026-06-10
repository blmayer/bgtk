# Makefile for BGTK

# Base flags. Freetype path is discovered via pkg-config when available
# (works on Linux and on macOS with Homebrew). The hardcoded fallback is
# the common Linux location.
FREETYPE_CFLAGS := $(shell pkg-config --cflags freetype2 2>/dev/null)
ifeq ($(FREETYPE_CFLAGS),)
  FREETYPE_CFLAGS := -I/usr/include/freetype2
endif

FREETYPE_LIBS := $(shell pkg-config --libs freetype2 2>/dev/null)
ifeq ($(FREETYPE_LIBS),)
  # Common fallbacks: Linux default + macOS Homebrew (Apple Silicon + Intel)
  FREETYPE_LIBS := -lfreetype
  FREETYPE_LIBDIRS := -L/opt/homebrew/lib -L/usr/local/lib
endif

CFLAGS = -Wall -Wextra -Werror -I. $(FREETYPE_CFLAGS) -fPIC

# Full LDFLAGS are only for real (Linux + bgce server) binaries.
LDFLAGS = -lfreetype -lbgce -lm

TARGET = libbgtk.so test_app image_viewer launcher headless
INSTALL_LIB = /usr/lib
INSTALL_INCLUDE = /usr/include

SRC = bgtk.c drawing.c widgets.c config.c
LIB_OBJS = bgtk.o drawing.o widgets.o config.o
TEST_APP_OBJ = apps/test_app.o
IMAGE_VIEWER_OBJ = apps/image_viewer.o
LAUNCHER_OBJ = apps/launcher.o

# ---------- Headless / mock testing support ----------
# "make headless" builds a standalone binary that uses bgtk_init_mock +
# bgtk_write_frame_png + bgtk_inject_event. It does not require a running
# bgce server or Linux input/DRI headers at runtime.
#
# On macOS (or any dev machine) this is the intended way to exercise the
# widget tree and visually inspect output by looking at the generated PNGs.
COMPAT_DIR := compat
HEADLESS_CFLAGS := $(CFLAGS) -I$(COMPAT_DIR)
HEADLESS_LDFLAGS := $(FREETYPE_LIBS) $(FREETYPE_LIBDIRS) -lm
HEADLESS_STUB := $(COMPAT_DIR)/bgce_stub.o
HEADLESS_OBJ := test/headless.o

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

# Headless target links the bgce stub instead of the real library and
# pulls in the compat headers via -I so that <bgce.h> and <linux/input.h>
# resolve on non-Linux platforms.
#
# We attach -Icompat to the specific object files only when they are
# prerequisites of the "headless" target. This way normal Linux server
# binaries (test_app, image_viewer, ...) continue to use the real system
# bgce headers if present.
$(HEADLESS_OBJ) $(LIB_OBJS) $(HEADLESS_STUB): CFLAGS += -I$(COMPAT_DIR)

$(HEADLESS_STUB): $(COMPAT_DIR)/bgce_stub.c
	$(CC) $(HEADLESS_CFLAGS) -c -o $@ $<

headless: $(HEADLESS_OBJ) $(LIB_OBJS) $(HEADLESS_STUB)
	$(CC) $(HEADLESS_CFLAGS) -o $@ $^ $(HEADLESS_LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(TARGET) $(LIB_OBJS) $(IMAGE_VIEWER_OBJ) $(TEST_APP_OBJ) $(LAUNCHER_OBJ) $(HEADLESS_OBJ) $(HEADLESS_STUB)


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

