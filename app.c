#include <bgce.h>
#include <errno.h>
#include <stdio.h>

#include "bgtk.h"

static int counter = 0;
static struct BGTK_Widget* counter_label = NULL;
struct BGTK_Context* ctx = NULL;

void button_callback(void) {
	counter++;
	printf("Button clicked! Counter: %d\n", counter);

	if (counter_label && counter_label->type == BGTK_WIDGET_LABEL) {
		puts(" setting new label");
		char counter_text[32];
		sprintf(counter_text, "Counter: %d", counter);
		printf("calling set_label: %p\n", counter_label->set_label);
		counter_label->set_label(counter_label, counter_text);
	}
}

int main(void) {
	setvbuf(stdout, NULL, _IONBF, 0);  // Disable buffering for stdout
	setvbuf(stderr, NULL, _IONBF, 0);  // Disable buffering for stderr

	// 1. Connect to BGCE
	int conn_fd = bgce_connect();
	if (conn_fd < 0) {
		fprintf(stderr,
			"bgtk_init: Failed to connect to BGCE server.\n");
		return -1;
	}

	// 2. Get Server Info (optional, but good for context)
	struct ServerInfo s_info;
	if (bgce_get_server_info(conn_fd, &s_info) != 0) {
		fprintf(stderr, "bgtk_init: Failed to get server info.\n");
		bgce_disconnect(conn_fd);
		return -2;
	}

	// 3. Request a buffer with given dimensions
	struct BufferRequest req = {.width = 600, .height = 480};

	void* buffer = bgce_get_buffer(conn_fd, req);
	if (!buffer) {
		fprintf(stderr,
			"bgtk_init: Failed to get buffer from server.\n");
		bgce_disconnect(conn_fd);
		return -3;
	}

	ctx = bgtk_init(conn_fd, buffer, 600, 480);
	if (!ctx) {
		fprintf(stderr, "Failed to initialize BGTK.\n");
		return 1;
	}
	printf("BGTK init done\n");

	// 4. Create Widgets

	// char* counter_text = "Counter: 0";
	// struct BGTK_Widget* title = bgtk_label(ctx, "BGTK Demo Application");
	// struct BGTK_Widget* button_text = bgtk_text(ctx, "Click Me!", 0);
	// struct BGTK_Widget* button =
	//     bgtk_button(ctx, button_text, button_callback, 0);
	// counter_label = bgtk_label(ctx, counter_text);

	// Create a list of widgets for the scrollable container
	struct BGTK_Widget* scrollable_widgets[10];
	for (int i = 0; i < 10; i++) {
		char label_text[32];
		sprintf(label_text, "Item %d", i + 1);
		scrollable_widgets[i] = bgtk_text(ctx, label_text, 0);
	}

	// Create the scrollable widget with the list of widgets
	struct BGTK_Widget* scrollable =
	    bgtk_scrollable(ctx, scrollable_widgets, 10, BGTK_FLAG_CENTER);
	scrollable->w = 600;
	scrollable->h = 480;

	// 6. draw widgets
	ctx->root_widget = scrollable;
	bgtk_draw_widgets(ctx);

	// 5. start loop to listen for input events
	printf("Starting BGTK main loop (%dx%d)...\n", ctx->width, ctx->height);
	struct BGCEMessage msg;
	ssize_t bytes;
	while (1) {
		bytes = bgce_recv_msg(ctx->conn_fd, &msg);
		if (bytes <= 0) {
			if (bytes == 0) {
				fprintf(stderr,
					"bgtk_main_loop: Server closed "
					"connection.\n");
			} else if (errno != EINTR) {
				perror("bgtk_main_loop: bgce_recv_msg");
			}
			break;
		}

		switch (msg.type) {
			case MSG_INPUT_EVENT:
				bgtk_handle_input_event(ctx,
							msg.data.input_event);
			break;
			case MSG_BUFFER_CHANGE:
				// TODO: Handle buffer resize/move
				break;  // Redraw
			default:
				// Ignore other messages for now
				printf("Ignoring message\n");
				break;
		}
	}

	bgtk_destroy(ctx);
	return 0;
}
