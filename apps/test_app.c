#include <bgce.h>
#include <errno.h>
#include <stdio.h>
#include <linux/input.h>

#include "bgtk.h"

static int counter = 0;
static struct BGTK_Widget* counter_label = NULL;
static struct BGTK_Widget* text_input = NULL;
struct BGTK_Context* ctx = NULL;

void button_callback(void *userdata) {
	(void)userdata;
	counter++;
	printf("Button clicked! Counter: %d\n", counter);

	if (counter_label && counter_label->type == BGTK_WIDGET_LABEL) {
		puts(" setting new label");
		char counter_text[32];
		sprintf(counter_text, "Counter: %d", counter);
		printf("calling set_label: %p\n",
		       counter_label->data.label.set_label);
		counter_label->data.label.set_label(counter_label,
						    counter_text);
	}
}

void text_input_changed(void) {
	if (text_input && text_input->type == BGTK_WIDGET_TEXT_INPUT) {
		printf("Text changed: %s\n", text_input->data.text_input.text);
	}
}

int main(void) {
	bgtk_log_open("test_app");
	setvbuf(stdout, NULL, _IONBF, 0);  // Disable buffering for stdout
	setvbuf(stderr, NULL, _IONBF, 0);  // Disable buffering for stderr

	// 1. Connect to BGCE
	int conn_fd = bgce_connect();
	if (conn_fd < 0) {
		bgtk_log_errno("bgce_connect");
		return -1;
	}

	// 2. Get Server Info (optional, but good for context)
	struct ServerInfo s_info;
	if (bgce_get_server_info(conn_fd, &s_info) != 0) {
		bgtk_log("bgce_get_server_info failed");
		bgce_disconnect(conn_fd);
		return -2;
	}

	// 3. Request a buffer with given dimensions
	struct BufferRequest req = {.width = 600, .height = 400};

	void* buffer = bgce_get_buffer(conn_fd, req);
	if (!buffer) {
		bgtk_log("bgce_get_buffer 600x400 failed");
		bgce_disconnect(conn_fd);
		return -3;
	}

	ctx = bgtk_init(conn_fd, buffer, 600, 400);
	if (!ctx) {
		bgtk_log("bgtk_init failed");
		return 1;
	}
	bgtk_log("BGTK init done");

	// 4. Create Widgets

	// Create a list of widgets for the scrollable container
	// Create a list of widgets for the scrollable container
	struct BGTK_Widget*
	    scrollable_widgets[10];  // Increased size for text input

	// Create a button with a label
	struct BGTK_Widget* button_label = bgtk_text(ctx, "Click me!",
						     (BGTK_Options){
							 .flags = 0,
							 .padding = 5,
							 .margin = 2,
						     });

	struct BGTK_Widget* button =
	    bgtk_button(ctx, button_label, button_callback, NULL,
			(BGTK_Options){
			    .flags = 0,
			    .padding = 10,
			    .margin = 5,
			});
	button->w = 200;
	button->h = 50;
	scrollable_widgets[0] = button;

	// Create counter label
	counter_label = bgtk_label(ctx, "Counter: 0",
				   (BGTK_Options){
				       .flags = 0,
				       .padding = 5,
				       .margin = 2,
				   });
	scrollable_widgets[1] = counter_label;

	// Add regular items
	for (int i = 0; i < 8;
	     i++) {  // Reduced count to make room for other widgets
		char label_text[32];
		sprintf(label_text, "Item %d", i + 1);
		scrollable_widgets[i + 2] = bgtk_text(ctx, label_text,
						      (BGTK_Options){
							  .flags = 0,
							  .padding = 5,
							  .margin = 2,
						      });
	}

	// Create text input widget
	text_input = bgtk_text_input(ctx, "Type here...", 300, 40,
				     (BGTK_Options){
					 .flags = 0,
					 .padding = 8,
					 .margin = 5,
				     });

	// Set the change callback
	text_input->data.text_input.on_change = text_input_changed;
	scrollable_widgets[8] = text_input;

	// Create image widget
	struct BGTK_Widget* image_widget =
	    bgtk_image(ctx, "example.png", 500, 400,
		      (BGTK_Options){
			  .flags = 0,
			  .padding = 10,
			  .margin = 5,
		      });
	if (image_widget) {
		scrollable_widgets[9] = image_widget;
	} else {
		fprintf(stderr, "Failed to load image widget\n");
		scrollable_widgets[9] = bgtk_text(ctx, "Image failed to load",
					  (BGTK_Options){
					      .flags = 0,
					      .padding = 5,
					      .margin = 2,
					  });
	}

	// Create the scrollable widget with the list of widgets
	struct BGTK_Widget* scrollable =
	    bgtk_scrollable(ctx, scrollable_widgets, 10,
			    (BGTK_Options){
				.flags = BGTK_FLAG_CENTER,
				.padding = 10,
				.margin = 5,
			    });
	scrollable->w = 600;
	scrollable->h = 400;

	ctx->root_widget = scrollable;
	bgtk_draw_widgets(ctx);

	// 6. start loop to listen for input events
	printf("Starting BGTK main loop (%dx%d)...\n", ctx->width, ctx->height);
	struct BGCEMessage msg;
	ssize_t bytes;
	while (1) {
		bytes = bgce_recv_msg(ctx->conn_fd, &msg);
		if (bytes <= 0) {
			if (bytes == 0)
				bgtk_log("server closed connection");
			else if (errno != EINTR)
				bgtk_log_errno("bgce_recv_msg");
			break;
		}

		int res = 0;
		switch (msg.type) {
		case MSG_INPUT_EVENT:
			if (msg.data.input_event.type == EV_REL ||
			    msg.data.input_event.type == EV_ABS)
				break;
			bgtk_update_modifiers(ctx, msg.data.input_event);
			if (bgtk_is_app_quit_event(ctx, msg.data.input_event))
				goto done;
			res = bgtk_handle_input_event(ctx, msg.data.input_event);
			break;
		case MSG_FOCUS_CHANGE:
			bgtk_set_window_focus(ctx, msg.data.focus_event.state);
			break;
		case MSG_BUFFER_CHANGE:
			if (bgtk_handle_buffer_change(ctx, &msg.data.buffer_reply) == 0) {
				bgtk_draw_widgets(ctx);
				res = 0;
			}
			break;
		default:
			break;
		}

		if (res)
			bgtk_draw_widgets(ctx);
	}
done:
	bgtk_destroy(ctx);
	return 0;
}
