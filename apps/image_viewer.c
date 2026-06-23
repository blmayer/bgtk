#include <bgce.h>
#include <bgtk.h>
#include <errno.h>
#include <linux/input.h>
#include <stdio.h>
#include <stdlib.h>

static struct BGTK_Context* ctx = NULL;
static struct BGTK_Widget* text_input = NULL;
static struct BGTK_Widget* image_widget = NULL;
static struct BGTK_Widget* col = NULL;  // Parent list widget

static void load_button_clicked(void *userdata) {
	(void)userdata;
	if (!ctx || !text_input || text_input->type != BGTK_WIDGET_TEXT_INPUT) {
		return;
	}

	const char* path = text_input->data.text_input.text;
	if (!path || !path[0]) {
		fprintf(stderr, "No image path provided\n");
		return;
	}

	struct BGTK_Widget* new_image = bgtk_image(ctx,
					   path,
					   800,
					   520,
					   (BGTK_Options){
					       .flags = 0,
					       .padding = 5,
					       .margin = 2,
					   });
	if (new_image) {
		// Replace pointer in parent list before freeing old widget
		col->data.list_widget.items[1] = new_image;
		free(image_widget);
		image_widget = new_image;
	}
	bgtk_draw_widgets(ctx);
}

int main(void) {
	setvbuf(stdout, NULL, _IONBF, 0);
	setvbuf(stderr, NULL, _IONBF, 0);

	int conn_fd = bgce_connect();
	if (conn_fd < 0) {
		fprintf(stderr,
			"image_viewer: Failed to connect to BGCE server.\n");
		return -1;
	}

	int width = 800;
	int height = 600;
	struct BufferRequest req = {.width = width, .height = height};
	void* buffer = bgce_get_buffer(conn_fd, req);
	if (!buffer) {
		fprintf(stderr,
			"image_viewer: Failed to get buffer from server.\n");
		bgce_disconnect(conn_fd);
		return -3;
	}

	ctx = bgtk_init(conn_fd, buffer, width, height);
	if (!ctx) {
		fprintf(stderr, "image_viewer: Failed to initialize BGTK.\n");
		return 1;
	}

	text_input = bgtk_text_input(ctx,
				     "example.png",
				     600,
				     0,
				     (BGTK_Options){
					 .flags = 0,
					 .padding = 6,
					 .margin = 5,
				     });

	struct BGTK_Widget* button_label = bgtk_text(ctx,
						     "Load",
						     (BGTK_Options){
							 .flags = 0,
							 .padding = 2,
						     });
	struct BGTK_Widget* button = bgtk_button(ctx,
						 button_label,
						 load_button_clicked, NULL,
						 (BGTK_Options){
						     .flags = 0,
						     .padding = 5,
						     .margin = 5,
						 });

	image_widget = bgtk_image(ctx,
				  "example.png",
				  800,
				  520,
				  (BGTK_Options){
				      .flags = 0,
				      .padding = 5,
				      .margin = 2,
				  });
	if (!image_widget) {
		image_widget = bgtk_text(ctx,
					 "Failed to load example.png",
					 (BGTK_Options){
					     .flags = 0,
					     .padding = 5,
					     .margin = 2,
					 });
	}

	struct BGTK_Widget* row_widgets[2] = {text_input, button};

	struct BGTK_Widget* row =
	    bgtk_list(ctx,
		      row_widgets,
		      2,
		      (BGTK_Options){.orientation = BGTK_LIST_HORIZONTAL});

	struct BGTK_Widget* col_widgets[2] = {row, image_widget};

	col = bgtk_list(ctx,
		      col_widgets,
		      2,
		      (BGTK_Options){.orientation = BGTK_LIST_VERTICAL});

	struct BGTK_Widget* frame =
	    bgtk_frame(ctx, col, 800, 600, (BGTK_Options){});

	ctx->root_widget = frame;
	bgtk_draw_widgets(ctx);

	printf("Starting image viewer main loop (%dx%d)...\n",
	       ctx->width,
	       ctx->height);
	struct BGCEMessage msg;
	ssize_t bytes;
	while (1) {
		bytes = bgce_recv_msg(ctx->conn_fd, &msg);
		if (bytes <= 0) {
			if (bytes == 0) {
				fprintf(stderr,
					"image_viewer: Server closed "
					"connection.\n");
			} else if (errno != EINTR) {
				perror("image_viewer: bgce_recv_msg");
			}
			break;
		}

		int res = 0;
		switch (msg.type) {
			case MSG_INPUT_EVENT:
				// Ignore mouse movement
				if (msg.data.input_event.type != EV_REL &&
				    msg.data.input_event.type != EV_ABS) {
					res = bgtk_handle_input_event(
					    ctx,
					    msg.data.input_event);
				}
				break;
			case MSG_FOCUS_CHANGE:
				bgtk_set_window_focus(
				    ctx,
				    msg.data.focus_event.state);
				break;
			case MSG_BUFFER_CHANGE:
				break;
			default:
				break;
		}

		if (res) {
			bgce_draw(conn_fd);
		}
	}

	bgtk_destroy(ctx);
	return 0;
}
