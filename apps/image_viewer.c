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

static int theme_pad(void)
{
	return (ctx && ctx->theme.padding > 0) ? ctx->theme.padding : 12;
}

static int theme_mar(void)
{
	return (ctx && ctx->theme.margin > 0) ? ctx->theme.margin : 8;
}

static void load_button_clicked(void *userdata) {
	int pad = theme_pad();
	int mar = theme_mar();
	int half = mar > 1 ? mar / 2 : 1;

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
					       .padding = pad > 2 ? pad / 2 : 2,
					       .margin = half,
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
	bgtk_log_open("image_viewer");

	int conn_fd = bgce_connect();
	if (conn_fd < 0) {
		bgtk_log_errno("bgce_connect (is bgce running?)");
		return -1;
	}
	bgtk_log("bgce_connect ok fd=%d", conn_fd);

	int width = 800;
	int height = 600;
	struct BufferRequest req = {.width = width, .height = height};
	void* buffer = bgce_get_buffer(conn_fd, req);
	if (!buffer) {
		bgtk_log("bgce_get_buffer %dx%d failed", width, height);
		bgce_disconnect(conn_fd);
		return -3;
	}
	bgtk_log("bgce_get_buffer ok %p", buffer);

	ctx = bgtk_init(conn_fd, buffer, width, height);
	if (!ctx) {
		bgtk_log("bgtk_init failed — check fonts / FreeType");
		return 1;
	}

	{
		int pad = theme_pad();
		int mar = theme_mar();
		int half = mar > 1 ? mar / 2 : 1;
		int mid = pad > 2 ? pad / 2 : 2;

		text_input = bgtk_text_input(ctx,
					     "example.png",
					     600,
					     0,
					     (BGTK_Options){
						 .flags = 0,
						 .padding = mid,
						 .margin = mar,
					     });

		struct BGTK_Widget* button_label = bgtk_text(ctx,
							     "Load",
							     (BGTK_Options){
								 .flags = 0,
								 .padding = half,
							     });
		struct BGTK_Widget* button = bgtk_button(ctx,
							 button_label,
							 load_button_clicked, NULL,
							 (BGTK_Options){
							     .flags = 0,
							     .padding = mid,
							     .margin = mar,
							 });

		image_widget = bgtk_image(ctx,
					  "example.png",
					  800,
					  520,
					  (BGTK_Options){
					      .flags = 0,
					      .padding = mid,
					      .margin = half,
					  });
		if (!image_widget) {
			image_widget = bgtk_text(ctx,
						 "Failed to load example.png",
						 (BGTK_Options){
						     .flags = 0,
						     .padding = mid,
						     .margin = half,
						 });
		}

		struct BGTK_Widget* row_widgets[2] = {text_input, button};

		struct BGTK_Widget* row =
		    bgtk_list(ctx,
			      row_widgets,
			      2,
			      (BGTK_Options){.orientation = BGTK_LIST_HORIZONTAL,
					     .margin = half,
					     .padding = 0});

		struct BGTK_Widget* col_widgets[2] = {row, image_widget};

		col = bgtk_list(ctx,
			      col_widgets,
			      2,
			      (BGTK_Options){.orientation = BGTK_LIST_VERTICAL,
					     .margin = mar,
					     .padding = mid});

		struct BGTK_Widget* frame =
		    bgtk_frame(ctx, col, 800, 600,
			       (BGTK_Options){.padding = pad, .margin = 0});

		ctx->root_widget = frame;
	}

	bgtk_draw_widgets(ctx);

	printf("Starting image viewer main loop (%dx%d)...\n",
	       ctx->width,
	       ctx->height);
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
				if (ctx->root_widget) {
					ctx->root_widget->w = ctx->width;
					ctx->root_widget->h = ctx->height;
				}
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
