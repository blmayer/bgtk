#include "bgtk.h"

#include <bgce.h>
#include <errno.h>
#include <linux/input.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// The font path is hardcoded for simplicity.
#define DEFAULT_FONT_PATH                                 \
	"/usr/share/fonts/ttf-input/InputMono/InputMono/" \
	"InputMono-Regular.ttf"
#define DEFAULT_FONT_SIZE 12

// Helper to create a generic widget
static struct BGTK_Widget* bgtk_widget_new(struct BGTK_Context* ctx,
					   enum BGTK_Widget_Type type,
					   int flags) {
	struct BGTK_Widget* widget =
	    (struct BGTK_Widget*)calloc(1, sizeof(struct BGTK_Widget));
	if (!widget) {
		perror("calloc");
		return NULL;
	}
	widget->ctx = ctx;
	widget->type = type;
	widget->flags = flags;
	return widget;
}

// --- Core Functions ---

struct BGTK_Context* bgtk_init(void) {
	struct BGTK_Context* ctx =
	    (struct BGTK_Context*)calloc(1, sizeof(struct BGTK_Context));
	if (!ctx) {
		perror("calloc");
		return NULL;
	}

	ctx->conn_fd = -1;  // Initialize fd to invalid value
	ctx->font_size = DEFAULT_FONT_SIZE;

	// 1. Initialize FreeType
	if (FT_Init_FreeType(&ctx->ft_library)) {
		fprintf(stderr,
			"bgtk_init: Could not init FreeType library.\n");
		free(ctx);
		return NULL;
	}

	// 2. Load Font
	if (FT_New_Face(ctx->ft_library, DEFAULT_FONT_PATH, 0, &ctx->ft_face)) {
		fprintf(stderr,
			"bgtk_init: Could not load font %s. Falling back "
			"to simple "
			"drawing.\n",
			DEFAULT_FONT_PATH);
		// Continue with default font context, simple text
		// drawing is used later.
	} else {
		// Set font size
		FT_Set_Pixel_Sizes(ctx->ft_face, 0, ctx->font_size);
	}

	// 3. Connect to BGCE
	int conn_fd = bgce_connect();
	if (conn_fd < 0) {
		fprintf(stderr,
			"bgtk_init: Failed to connect to BGCE server.\n");
		if (ctx->ft_library) {
			FT_Done_FreeType(ctx->ft_library);
		}
		free(ctx);
		return NULL;
	}

	// 4. Get Server Info (optional, but good for context)
	struct ServerInfo s_info;
	if (bgce_get_server_info(conn_fd, &s_info) != 0) {
		fprintf(stderr, "bgtk_init: Failed to get server info.\n");
		bgce_disconnect(conn_fd);
		if (ctx->ft_library) {
			FT_Done_FreeType(ctx->ft_library);
		}
		free(ctx);
		return NULL;
	}

	//// 5. Subscribe to Input Events
	// struct BGCEMessage sub_msg = { .type = MSG_SUBSCRIBE_INPUT };
	//// This message has no payload, just the type.
	// if (bgce_send_msg(conn_fd, &sub_msg) < 0) {
	//     perror("bgtk_init: Failed to subscribe to input");
	//     // Not fatal, but input won't work
	// }

	// 6. Request a buffer with server dimensions (full screen)
	struct BufferRequest req = {.width = 600, .height = 480};

	// We expect the libbgce function to handle the shm attach
	void* buffer = bgce_get_buffer(conn_fd, req);
	if (!buffer) {
		fprintf(stderr,
			"bgtk_init: Failed to get buffer from server.\n");
		bgce_disconnect(conn_fd);
		if (ctx->ft_library) {
			FT_Done_FreeType(ctx->ft_library);
		}
		free(ctx);
		return NULL;
	}

	ctx->conn_fd = conn_fd;
	ctx->shm_buffer = buffer;
	ctx->buf_width = 600;
	ctx->buf_height = 480;

	ctx->width = 600;
	ctx->height = 480;

	// Initial allocation for widgets
	ctx->widget_capacity = 10;
	ctx->widgets = (struct BGTK_Widget**)calloc(
	    ctx->widget_capacity, sizeof(struct BGTK_Widget*));
	if (!ctx->widgets) {
		perror("calloc");
		// Cleanup shm_buffer and FT
		bgce_disconnect(conn_fd);
		if (ctx->ft_library) {
			FT_Done_FreeType(ctx->ft_library);
		}
		free(ctx);
		return NULL;
	}

	return ctx;
}

void bgtk_destroy(struct BGTK_Context* ctx) {
	if (!ctx) {
		return;
	}

	// Free all widgets
	for (size_t i = 0; i < ctx->widget_count; i++) {
		struct BGTK_Widget* w = ctx->widgets[i];
		if (w->type == BGTK_WIDGET_LABEL) {
			if (w->data.label.text) {
				free(w->data.label.text->data.text.text);
				free(w->data.label.text);
			}
		} else if (w->type == BGTK_WIDGET_BUTTON) {
			if (w->data.button.label) {
				// The label
				// widget is
				// freed
				// separately,
				// but its text
				// child must be
				// freed here
				if (w->data.button.label->data.label.text) {
					free(w->data.button.label->data.label
						 .text->data.text.text);
					free(w->data.button.label->data.label
						 .text);
				}
				free(w->data.button.label);
			}
		} else if (w->type == BGTK_WIDGET_TEXT) {
			free(w->data.text.text);
		}
		free(w);
	}

	// Free FreeType resources
	if (ctx->ft_face) {
		FT_Done_Face(ctx->ft_face);
	}
	if (ctx->ft_library) {
		FT_Done_FreeType(ctx->ft_library);
	}

	// Disconnect from BGCE
	if (ctx->conn_fd > 0) {
		bgce_disconnect(ctx->conn_fd);
	}

	free(ctx);
}

// --- Drawing Primitives & Widgets ---

// A few basic colors (0xAARRGGBB)
#define BGTK_COLOR_BG 0xFFCCCCCC     // Light Gray
#define BGTK_COLOR_BTN 0xFF007BFF    // Blue
#define BGTK_COLOR_TEXT 0xFF000000   // Black
#define BGTK_COLOR_WHITE 0xFFFFFFFF  // White

static void bgtk_clear_buffer(struct BGTK_Context* ctx, uint32_t color) {
	uint32_t* pixels = (uint32_t*)ctx->shm_buffer;
	size_t size = (size_t)ctx->buf_width * ctx->buf_height;
	for (size_t i = 0; i < size; i++) {
		pixels[i] = color;
	}
}

static void bgtk_draw_rect(struct BGTK_Context* ctx, uint32_t* pixels, int x,
			   int y, int w, int h, uint32_t color) {
	// Basic clipping and drawing
	int x1 = x;
	int y1 = y;
	int x2 = x + w;
	int y2 = y + h;

	// Clip to buffer bounds
	if (x1 < 0) {
		x1 = 0;
	}
	if (y1 < 0) {
		y1 = 0;
	}
	if (x2 > (int)ctx->buf_width) {
		x2 = ctx->buf_width;
	}
	if (y2 > (int)ctx->buf_height) {
		y2 = ctx->buf_height;
	}

	int stride = ctx->buf_width;

	for (int j = y1; j < y2; j++) {
		for (int i = x1; i < x2; i++) {
			pixels[j * stride + i] = color;
		}
	}
}

void measure_text(FT_Face face, const char* text, int* out_width,
		  int* out_height) {
	int width = 0;

	for (const char* p = text; *p; p++) {
		if (FT_Load_Char(face, *p, FT_LOAD_DEFAULT)) {
			continue;
		}

		width += face->glyph->advance.x;  // 26.6 units
	}

	width >>= 6;

	int ascent = face->size->metrics.ascender >> 6;
	int descent = -face->size->metrics.descender >> 6;
	int height = ascent + descent;

	*out_width = width;
	*out_height = height;
}

static void bgtk_draw_text(struct BGTK_Context* ctx, uint32_t* pixels,
			   const char* text, int x, int y, uint32_t color) {
	if (!ctx->ft_face) {
		// Fallback to simple placeholder if font didn't load
		bgtk_draw_rect(ctx, pixels, x, y, 5, 5, color);
		return;
	}

	// Set font size for drawing context
	FT_Set_Pixel_Sizes(ctx->ft_face, 0, ctx->font_size);

	int pen_x = x;
	int pen_y = y + (ctx->ft_face->size->metrics.ascender >> 6);

	int stride = ctx->buf_width;

	for (const char* p = text; *p; p++) {
		FT_UInt index = FT_Get_Char_Index(ctx->ft_face, *p);

		if (FT_Load_Glyph(ctx->ft_face, index,
				  FT_LOAD_DEFAULT | FT_LOAD_TARGET_LIGHT)) {
			continue;
		}

		FT_Render_Glyph(ctx->ft_face->glyph, FT_RENDER_MODE_NORMAL);

		FT_GlyphSlot slot = ctx->ft_face->glyph;
		FT_Bitmap* bitmap = &slot->bitmap;

		int gx = pen_x + slot->bitmap_left;
		int gy = pen_y - slot->bitmap_top;

		for (unsigned int row = 0; row < bitmap->rows; row++) {
			for (unsigned int col = 0; col < bitmap->width; col++) {
				uint8_t a =
				    bitmap->buffer[row * bitmap->pitch + col];
				if (a == 0) {
					continue;
				}

				int32_t dx = gx + col;
				int32_t dy = gy + row;
				if (dx < 0 || dx >= (int)ctx->buf_width ||
				    dy < 0 || dy >= (int)ctx->buf_height) {
					continue;
				}

				// Blend
				uint32_t dst = pixels[dy * stride + dx];
				uint8_t inv = 255 - a;

				uint8_t r_dst = (dst >> 16) & 0xFF;
				uint8_t g_dst = (dst >> 8) & 0xFF;
				uint8_t b_dst = (dst) & 0xFF;

				uint8_t r_src = (color >> 16) & 0xFF;
				uint8_t g_src = (color >> 8) & 0xFF;
				uint8_t b_src = (color) & 0xFF;

				uint8_t r = (r_src * a + r_dst * inv) / 255;
				uint8_t g = (g_src * a + g_dst * inv) / 255;
				uint8_t b = (b_src * a + b_dst * inv) / 255;

				pixels[dy * stride + dx] =
				    (r << 16) | (g << 8) | b;
			}
		}

		pen_x += slot->advance.x >> 6;
	}
}

static void bgtk_draw_widget(struct BGTK_Context* ctx, struct BGTK_Widget* w,
			     uint32_t* pixels) {
	switch (w->type) {
		case BGTK_WIDGET_LABEL:
			// Draw label background
			bgtk_draw_rect(ctx, pixels, w->x, w->y, w->w, w->h,
				       BGTK_COLOR_BG);
			// Draw text widget (offset for padding)
			if (w->data.label.text) {
				w->data.label.text->x = w->x + 5;
				w->data.label.text->y = w->y + 5;
				bgtk_draw_widget(ctx, w->data.label.text,
						 pixels);
			}
			break;
		case BGTK_WIDGET_TEXT:
			bgtk_draw_text(ctx, pixels, w->data.text.text, w->x,
				       w->y, BGTK_COLOR_TEXT);
			break;
		case BGTK_WIDGET_BUTTON:
			// Draw button background
			bgtk_draw_rect(ctx, pixels, w->x, w->y, w->w, w->h,
				       BGTK_COLOR_BTN);

			// Draw button border (1px black)
			bgtk_draw_rect(ctx, pixels, w->x, w->y, w->w, 1,
				       BGTK_COLOR_TEXT);  // Top
			bgtk_draw_rect(ctx, pixels, w->x, w->y + w->h - 1, w->w,
				       1,
				       BGTK_COLOR_TEXT);  // Bottom
			bgtk_draw_rect(ctx, pixels, w->x, w->y, 1, w->h,
				       BGTK_COLOR_TEXT);  // Left
			bgtk_draw_rect(ctx, pixels, w->x + w->w - 1, w->y, 1,
				       w->h,
				       BGTK_COLOR_TEXT);  // Right

			// Draw label widget (offset for padding)
			if (w->data.button.label) {
				w->data.button.label->x = w->x + 10;
				w->data.button.label->y = w->y + 10;
				bgtk_draw_widget(ctx, w->data.button.label,
						 pixels);
			}
			break;
		case BGTK_WIDGET_SCROLLABLE:
			// Allocate or update the off-screen buffer if needed
			if (!w->data.scrollable.tmp) {
				// Allocate the off-screen buffer
				w->data.scrollable.tmp = calloc(
				    w->w * w->data.scrollable.content_height,
				    sizeof(uint32_t));
				if (!w->data.scrollable.tmp) {
					fprintf(stderr,
						"Failed to allocate off-screen "
						"buffer\n");
					break;
				}
				bgtk_draw_rect(
				    ctx, w->data.scrollable.tmp, 0, 0, w->w,
				    w->data.scrollable.content_height,
				    BGTK_COLOR_BG);

				// Draw child widgets into the off-screen buffer
				int current_y = 0;
				for (size_t i = 0;
				     i < w->data.scrollable.widget_count; i++) {
					struct BGTK_Widget* child =
					    w->data.scrollable.widgets[i];

					child->x = w->x + 5;  // 5px padding
					if (w->flags & BGTK_FLAG_CENTER) {
						child->x =
						    w->x +
						    (w->w - child->w) / 2;
					}
					child->y = current_y;
					bgtk_draw_widget(
					    ctx, child, w->data.scrollable.tmp);
					current_y +=
					    child->h + 5;  // 5px spacing
				}
			}

			// Copy the off-screen buffer to the framebuffer
			// according to scroll position
			uint32_t* buff = ctx->shm_buffer;
			uint32_t* tmp = w->data.scrollable.tmp;
			for (int row = 0; row < w->h; row++) {
				if (w->data.scrollable.scroll_y + row <
				    w->data.scrollable.content_height) {
					memcpy(
					    &buff[(w->y + row) *
						      ctx->buf_width +
						  w->x],
					    &tmp[(w->data.scrollable.scroll_y +
						  row) *
						 w->w],
					    w->w * 4);
				}
			}

			break;
	}
}

static void bgtk_draw_widgets(struct BGTK_Context* ctx) {
	// 1. Clear the screen
	bgtk_clear_buffer(ctx, BGTK_COLOR_BG);

	// 2. Draw all widgets
	for (size_t i = 0; i < ctx->widget_count; i++) {
		bgtk_draw_widget(ctx, ctx->widgets[i], ctx->shm_buffer);
	}
}

// TODO: implement descending into child widgets
static int bgtk_handle_input_event(struct BGTK_Context* ctx,
				   struct InputEvent* ev) {
	// Handle mouse wheel for scrolling (REL_WHEEL)
	if (ev->code == REL_WHEEL) {
		printf("handling mouse wheel: val=%d at (%u, %u)\n", ev->value,
		       ev->x, ev->y);
		for (size_t i = 0; i < ctx->widget_count; i++) {
			struct BGTK_Widget* w = ctx->widgets[i];
			if (w->type == BGTK_WIDGET_SCROLLABLE) {
				puts("found scroll widget");

				// Check if mouse is over the scrollable
				// widget
				if (ev->x >= w->x && ev->x < (w->x + w->w) &&
				    ev->y >= w->y && ev->y < (w->y + w->h)) {
					puts("updating scroll");
					w->data.scrollable.scroll_y -=
					    ev->value * 10;  // Scroll speed

					// Clamp scroll_y to valid range
					if (w->data.scrollable.scroll_y < 0) {
						w->data.scrollable.scroll_y = 0;
					}
					if (w->data.scrollable.scroll_y >
					    w->data.scrollable.content_height -
						w->h) {
						w->data.scrollable.scroll_y =
						    w->data.scrollable
							.content_height -
						    w->h;
					}
					if (w->data.scrollable.scroll_y < 0) {
						w->data.scrollable.scroll_y = 0;
					}
					printf(
					    "updated scroll position: "
					    "%d\n",
					    w->data.scrollable.scroll_y);
					bgtk_draw_widget(ctx, w,
							 ctx->shm_buffer);

					return 1;  // Redraw
				}
			}
		}
	}

	// Only handle mouse button presses for now
	if (ev->code != BTN_LEFT || ev->value != 1) {
		return 0;
	}
	printf("BGTK Got click: (%d, %d)\n", ev->x, ev->y);

	// Hit-testing for buttons
	for (size_t i = 0; i < ctx->widget_count; i++) {
		struct BGTK_Widget* w = ctx->widgets[i];

		if (w->type == BGTK_WIDGET_BUTTON) {
			// Check if click coordinates
			// are within button bounds
			if (ev->x >= w->x && ev->x < (w->x + w->w) &&
			    ev->y >= w->y && ev->y < (w->y + w->h)) {
				printf(
				    "BGTK Clicked in "
				    "button\n");

				// Trigger
				// callback
				if (w->data.button.callback) {
					w->data.button.callback();
					return 1;
				}
			}
		}
	}
	return 0;
}

static int bgtk_handle_events(struct BGTK_Context* ctx) {
	struct BGCEMessage msg;
	ssize_t bytes;

	// bgce_recv_msg is blocking. This makes the UI reactive.
	bytes = bgce_recv_msg(ctx->conn_fd, &msg);
	if (bytes <= 0) {
		if (bytes == 0) {
			fprintf(stderr,
				"bgtk_main_loop: Server closed "
				"connection.\n");
		} else if (errno != EINTR) {
			perror("bgtk_main_loop: bgce_recv_msg");
		}
		return -1;  // Exit loop
	}

	switch (msg.type) {
		case MSG_INPUT_EVENT:
			return bgtk_handle_input_event(ctx,
						       &msg.data.input_event);
		case MSG_BUFFER_CHANGE:
			// TODO: Handle buffer resize/move
			return 1;  // Redraw
		default:
			// Ignore other messages for now
			return 0;
	}
}

void bgtk_main_loop(struct BGTK_Context* ctx) {
	bgtk_draw_widgets(ctx);
	bgce_draw(ctx->conn_fd);
	printf("BGTK Main Loop started. Press Ctrl+C to exit.\n");

	while (1) {
		int result = bgtk_handle_events(ctx);
		if (result == -1) {
			break;	// Server error/disconnection
		} else if (result == 1) {
			printf("BGTK drawing.\n");
			bgce_draw(ctx->conn_fd);
		}
	}
}

/*
 * Widget Creation Functions
 * ^^^^^^^^^^^^^^^^^^^^^^^^^
 */

void set_label(struct BGTK_Widget* widget, char* label) {
	printf("BGTK: setting label: %s\n", label);
	if (widget->data.label.text) {
		free(widget->data.label.text->data.text.text);
		free(widget->data.label.text);
	}

	// Create a new text widget for the label
	struct BGTK_Widget* text_widget = bgtk_text(widget->ctx, label, 0);
	if (!text_widget) {
		perror("BGTK Failed to create text widget for label");
		return;
	}

	widget->data.label.text = text_widget;

	// Calculate size based on text widget
	widget->w = text_widget->w + 10;  // Add padding
	widget->h = text_widget->h + 10;  // Add padding

	bgtk_draw_widget(widget->ctx, widget, widget->ctx->shm_buffer);
	printf("BGTK label set\n");
}

struct BGTK_Widget* bgtk_label(struct BGTK_Context* ctx, char* text) {
	struct BGTK_Widget* widget = bgtk_widget_new(ctx, BGTK_WIDGET_LABEL, 0);
	printf("BGTK allocated label\n");
	if (!widget) {
		perror("BGTK Failed to create new widget");
		return NULL;
	}

	widget->set_label = set_label;

	// Create a text widget for the label
	struct BGTK_Widget* text_widget = bgtk_text(ctx, text, 0);
	if (!text_widget) {
		perror("BGTK Failed to create text widget for label");
		free(widget);
		return NULL;
	}

	widget->data.label.text = text_widget;

	// Calculate size based on text widget
	widget->w = text_widget->w + 10;  // Add padding
	widget->h = text_widget->h + 10;  // Add padding

	return widget;
}

struct BGTK_Widget* bgtk_text(struct BGTK_Context* ctx, char* text, int flags) {
	struct BGTK_Widget* widget =
	    bgtk_widget_new(ctx, BGTK_WIDGET_TEXT, flags);
	printf("BGTK allocated text widget\n");
	if (!widget) {
		perror("BGTK Failed to create new widget");
		return NULL;
	}

	char* ptr = calloc(1, strlen(text) + 1);
	sprintf(ptr, "%s", text);
	widget->data.text.text = ptr;

	// Calculate size based on text
	measure_text(widget->ctx->ft_face, widget->data.text.text, &widget->w,
		     &widget->h);

	return widget;
}

struct BGTK_Widget* bgtk_button(struct BGTK_Context* ctx,
				struct BGTK_Widget* label,
				BGTK_Callback callback, int flags) {
	struct BGTK_Widget* widget =
	    bgtk_widget_new(ctx, BGTK_WIDGET_BUTTON, flags);
	if (!widget) {
		perror("BGTK Failed to create new widget");
		return NULL;
	}

	widget->data.button.callback = callback;
	widget->data.button.label = label;

	// Calculate size based on label widget
	widget->w = label->w + 20;  // Add padding
	widget->h = label->h + 20;  // Add padding
	return widget;
}

// --- Layout/Management Functions ---

void bgtk_add_widget(struct BGTK_Context* ctx, struct BGTK_Widget* widget,
		     int x, int y, int w, int h) {
	if (!ctx || !widget) {
		return;
	}

	if (ctx->widget_count >= ctx->widget_capacity) {
		ctx->widget_capacity *= 2;
		struct BGTK_Widget** new_widgets =
		    (struct BGTK_Widget**)realloc(
			ctx->widgets,
			ctx->widget_capacity * sizeof(struct BGTK_Widget*));
		if (!new_widgets) {
			perror("realloc");
			// In a real app, this should be handled
			// better
			return;
		}
		ctx->widgets = new_widgets;
	}

	widget->x = x;
	widget->y = y;
	widget->w = w > 0 ? w : widget->w;
	widget->h = h > 0 ? h : widget->h;

	ctx->widgets[ctx->widget_count++] = widget;
}

// Helper to recalculate the content height of a scrollable widget
struct BGTK_Widget* bgtk_scrollable(struct BGTK_Context* ctx,
				    struct BGTK_Widget** widgets,
				    size_t widget_count, int flags) {
	struct BGTK_Widget* widget =
	    bgtk_widget_new(ctx, BGTK_WIDGET_SCROLLABLE, flags);
	if (!widget) {
		perror("BGTK Failed to create scrollable widget");
		return NULL;
	}

	widget->data.scrollable.widget_capacity =
	    widget_count > 0 ? widget_count : 10;
	widget->data.scrollable.widgets = (struct BGTK_Widget**)calloc(
	    widget->data.scrollable.widget_capacity,
	    sizeof(struct BGTK_Widget*));
	if (!widget->data.scrollable.widgets) {
		perror("calloc");
		free(widget);
		return NULL;
	}

	// Copy the input widgets into the scrollable container
	widget->data.scrollable.widget_count = widget_count;
	widget->data.scrollable.scroll_y = 0;
	widget->data.scrollable.content_height = 0;
	for (size_t i = 0; i < widget_count; i++) {
		widget->data.scrollable.widgets[i] = widgets[i];
		widget->data.scrollable.content_height +=
		    widgets[i]->h + 5;	// 5px spacing
	}

	// Initialize tmp buffer to NULL, it will be allocated during
	// drawing
	widget->data.scrollable.tmp = NULL;

	return widget;
}
