# Makefile for BGTK

CFLAGS = -Wall -Wextra -Werror -I. -I/usr/include/freetype2 -fPIC
LDFLAGS = -lfreetype -lbgce -lm

TARGET = libbgtk.so test_app image_viewer
INSTALL_LIB = /usr/lib
INSTALL_INCLUDE = /usr/include

SRC = bgtk.c drawing.c widgets.c config.c
LIB_OBJS = bgtk.o drawing.o widgets.o config.o
TEST_APP_OBJ = apps/test_app.o
IMAGE_VIEWER_OBJ = apps/image_viewer.o


.PHONY: all clean test

all: $(TARGET)

libbgtk.so: $(LIB_OBJS)
	$(CC) -shared -o $@ $(LIB_OBJS) $(LDFLAGS)

test_app: $(TEST_APP_OBJ) $(LIB_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

image_viewer: $(IMAGE_VIEWER_OBJ) $(LIB_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)


%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(TARGET) $(LIB_OBJS) $(IMAGE_VIEWER_OBJ) $(TEST_APP_OBJ)


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

