/* compat/bgce.h
 *
 * Tiny portable stand-in for ../bgce/bgce.h .
 * Only the parts that BGTK's public API and internal code actually
 * reference are defined here. This lets you build and run the
 * headless / mock tests on macOS (or any non-Linux machine) without
 * needing the full bgce tree or Linux headers at compile time.
 *
 * On real Linux builds the original <bgce.h> (from libbgce) is used.
 */

#ifndef BGTK_COMPAT_BGCE_H
#define BGTK_COMPAT_BGCE_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#define SOCKET_PATH "/tmp/bgce.sock"
#define BGCE_BYTES_PER_PIXEL 4
#define MAX_INPUT_DEVICES 4

/* Message types (only a few are referenced in apps; included for completeness) */
enum {
	MSG_GET_SERVER_INFO = 1,
	MSG_GET_BUFFER,
	MSG_DRAW,
	MSG_INPUT_EVENT,
	MSG_BUFFER_CHANGE,
	MSG_FOCUS_CHANGE,
	MSG_SUBSCRIBE_INPUT,
	MSG_MOVE
};

/* Input device description (present in InputEvent in the real header) */
struct InputDevice {
	uint16_t id;
	uint16_t type_mask;
	char name[256];
};

/* The event struct that BGTK's public API uses (bgtk_handle_input_event etc.) */
struct InputEvent {
	int32_t type;  /* EV_KEY, EV_REL, ... */
	int32_t value;
	uint32_t code;
	int32_t x;
	int32_t y;
	struct InputDevice device;  /* carried for protocol compatibility; BGTK ignores it */
};

/* Focus event */
struct FocusEvent {
	int32_t state;
};

/* Buffer / server info structures (used by real apps, not by core BGTK) */
struct ServerInfo {
	uint32_t width;
	uint32_t height;
	uint32_t color_depth;
	uint16_t input_device_count;
	struct InputDevice devices[MAX_INPUT_DEVICES];
};

struct BufferRequest {
	uint32_t width;
	uint32_t height;
};

struct BufferReply {
	int status;
	char shm_name[64];
	uint32_t width;
	uint32_t height;
};

struct MoveRequest {
	int32_t x;
	int32_t y;
};

struct ResizeRequest {
	uint32_t width;
	uint32_t height;
};

struct BGCEMessage {
	uint32_t type;
	union {
		struct ServerInfo server_info;
		struct BufferRequest buffer_request;
		struct BufferReply buffer_reply;
		struct InputEvent input_event;
		struct FocusEvent focus_event;
		struct MoveRequest move_request;
	} data;
};

/* Client-side API surface (only bgce_draw is called from the BGTK core;
 * the rest are used by the example apps. We provide prototypes so link
 * succeeds when using the stub. */
ssize_t bgce_send_msg(int conn, struct BGCEMessage *msg);
ssize_t bgce_recv_msg(int conn, struct BGCEMessage *msg);
int bgce_connect(void);
int bgce_get_server_info(int fd, struct ServerInfo *out_info);
void *bgce_get_buffer(int conn, struct BufferRequest req);
int bgce_move(int fd, int x, int y);
int bgce_draw(int fd);
void bgce_disconnect(int fd);

#endif /* BGTK_COMPAT_BGCE_H */
