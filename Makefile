# Makefile for BGTK

CFLAGS = -Wall -Wextra -Werror -I. -I/usr/include/freetype2 -I../bgce
LDFLAGS = -lfreetype -lbgce -lm

TARGET = libbgtk.so app
SRC = app.c bgtk.c drawing.c widgets.c
LIB_OBJS = bgtk.c drawing.c widgets.c
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
test: $(TARGET)

.PHONY: test
	@echo "Starting BGCE server in background..."
	bgce &
	BGCE_PID=$$
	@echo "Running app..."
	./app || true
	@echo "Killing BGCE server (PID: $$BGCE_PID)..."
	kill $$BGCE_PID 2>/dev/null || true
	@echo "Test complete."

