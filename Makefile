# Makefile for BGTK

CFLAGS = -Wall -Wextra -Werror -I. -I/usr/include/freetype2 -fPIC
LDFLAGS = -lfreetype -lbgce -lm

TARGET = libbgtk.so app
SRC = app.c bgtk.c drawing.c widgets.c config.c
LIB_OBJS = bgtk.o drawing.o widgets.o config.o
OBJ = $(SRC:.c=.o)

.PHONY: all clean test

all: $(TARGET)

libbgtk.so: $(LIB_OBJS)
	$(CC) -shared -o $@ $(LIB_OBJS) $(LDFLAGS)

app: $(OBJ)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(TARGET) $(OBJ)

.PHONY: install
install: libbgtk.so bgtk.h
	install -d $(INSTALL_LIB)
	install -m 755 libbgtk.so $(INSTALL_LIB)
	install -d $(INSTALL_INCLUDE)
	install -m 644 bgtk.h $(INSTALL_INCLUDE)
	ldconfig

.PHONY: test
test: app
	@echo "Starting BGCE server in background..."
	bgce > server.log 2>&1 &
	BGCE_PID=$$
	@echo "Running app..."
	./app > app.log 2>&1 || true
	@echo "Killing BGCE server (PID: $$BGCE_PID)..."
	kill $$BGCE_PID 2>/dev/null || true
	@echo "Test complete."

