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
	struct BufferRequest req = {.width = 600, .height = 400};

	void* buffer = bgce_get_buffer(conn_fd, req);
	if (!buffer) {
		fprintf(stderr,
			"bgtk_init: Failed to get buffer from server.\n");
		bgce_disconnect(conn_fd);
		return -3;
	}

	ctx = bgtk_init(conn_fd, buffer, 600, 400);
	if (!ctx) {
		fprintf(stderr, "Failed to initialize BGTK.\n");
		return 1;
	}
	printf("BGTK init done\n");

	// 4. Create Widgets

	// Create a list of widgets for the scrollable container
	struct BGTK_Widget* scrollable_widgets[21];
	for (int i = 0; i < 20; i++) {
		char label_text[32];
		sprintf(label_text, "Item %d", i + 1);
		scrollable_widgets[i] = bgtk_text(ctx, label_text, (BGTK_Options){
			.flags = 0,
			.padding = 5,
			.margin = 2,
		});
	}

	struct BGTK_Widget* image_widget = bgtk_image(ctx, "example.png", (BGTK_Options){
		.flags = 0,
		.padding = 10,
		.margin = 5,
	});
	if (image_widget) {
		image_widget->w = 500;
		image_widget->h = 400;
		scrollable_widgets[20] = image_widget;
	} else {
		fprintf(stderr, "Failed to load image widget\n");
	}

	// Create the scrollable widget with the list of widgets
	struct BGTK_Widget* scrollable =
	    bgtk_scrollable(ctx, scrollable_widgets, 21, (BGTK_Options){
			.flags = BGTK_FLAG_CENTER,
			.padding = 10,
			.margin = 5,
		});
	scrollable->w = 600;
	scrollable->h = 400;
	
	// 5. draw widgets
	ctx->root_widget = scrollable;
	bgtk_draw_widgets(ctx);

	// 6. start loop to listen for input events
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

		int res = 0;
		switch (msg.type) {
			case MSG_INPUT_EVENT:
				res = bgtk_handle_input_event(
				    ctx, msg.data.input_event);
				break;
			case MSG_BUFFER_CHANGE:
				// TODO: Handle buffer
				// resize/move
				break;	// Redraw
			default:
				// Ignore other messages for now
				printf("Ignoring message\n");
				break;
		}
		if (res) {
			bgce_draw(conn_fd);
		}
	}

	bgtk_destroy(ctx);
	return 0;
}
