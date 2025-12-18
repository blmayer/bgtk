# Makefile for BGTK

CFLAGS = -Wall -Wextra -Werror -I. -I/usr/include/freetype2 -I/usr/local/include/bgce
LDFLAGS = -lfreetype -lbgce

TARGET = app
SRC = app.c bgtk.c drawing.c widgets.c
OBJ = $(SRC:.c=.o)

.PHONY: all clean test

all: $(TARGET)

$(TARGET): $(OBJ)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(TARGET) $(OBJ)

test: $(TARGET)

.PHONY: test
	@echo "Starting BGCE server in background..."
	bgce &
	BGCE_PID=$$
	@echo "Running $(TARGET)..."
	./$(TARGET) || true
	@echo "Killing BGCE server (PID: $$BGCE_PID)..."
	kill $$BGCE_PID 2>/dev/null || true
	@echo "Test complete."

