#include "bgtk.h"
#include <bgce.h>

#include <errno.h>
#include <linux/input.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// The font path is hardcoded for simplicity.
#define DEFAULT_FONT_PATH "/usr/share/fonts/ttf-input/InputMono/InputMono/InputMono-Regular.ttf"
#define DEFAULT_FONT_SIZE 12

// Helper to create a generic widget
static BGTK_Widget* bgtk_widget_new(BGTK_Context* ctx, BGTK_Widget_Type type) {
	BGTK_Widget* widget = (BGTK_Widget*)calloc(1, sizeof(BGTK_Widget));
	if (!widget) {
		perror("calloc");
		return NULL;
	}
	widget->ctx = ctx;
	widget->type = type;
	return widget;
}

// --- Core Functions ---

BGTK_Context* bgtk_init(void) {
	BGTK_Context* ctx = (BGTK_Context*)calloc(1, sizeof(BGTK_Context));
	if (!ctx) {
		perror("calloc");
		return NULL;
	}

	ctx->conn_fd = -1; // Initialize fd to invalid value
	ctx->font_size = DEFAULT_FONT_SIZE;

	// 1. Initialize FreeType
	if (FT_Init_FreeType(&ctx->ft_library)) {
		fprintf(stderr, "bgtk_init: Could not init FreeType library.\n");
		free(ctx);
		return NULL;
	}

	// 2. Load Font
	if (FT_New_Face(ctx->ft_library, DEFAULT_FONT_PATH, 0, &ctx->ft_face)) {
		fprintf(stderr, "bgtk_init: Could not load font %s. Falling back to simple drawing.\n", DEFAULT_FONT_PATH);
		// Continue with default font context, simple text drawing is used later.
	} else {
		// Set font size
		FT_Set_Pixel_Sizes(ctx->ft_face, 0, ctx->font_size);
	}

	// 3. Connect to BGCE
	int conn_fd = bgce_connect();
	if (conn_fd < 0) {
		fprintf(stderr, "bgtk_init: Failed to connect to BGCE server.\n");
		if (ctx->ft_library)
			FT_Done_FreeType(ctx->ft_library);
		free(ctx);
		return NULL;
	}

	// 4. Get Server Info (optional, but good for context)
	struct ServerInfo s_info;
	if (bgce_get_server_info(conn_fd, &s_info) != 0) {
		fprintf(stderr, "bgtk_init: Failed to get server info.\n");
		bgce_disconnect(conn_fd);
		if (ctx->ft_library)
			FT_Done_FreeType(ctx->ft_library);
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
	struct BufferRequest req = {
	        .width = 600,
	        .height = 480};

	// We expect the libbgce function to handle the shm attach
	void* buffer = bgce_get_buffer(conn_fd, req);
	if (!buffer) {
		fprintf(stderr, "bgtk_init: Failed to get buffer from server.\n");
		bgce_disconnect(conn_fd);
		if (ctx->ft_library)
			FT_Done_FreeType(ctx->ft_library);
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
	ctx->widgets = (BGTK_Widget**)calloc(ctx->widget_capacity, sizeof(BGTK_Widget*));
	if (!ctx->widgets) {
		perror("calloc");
		// Cleanup shm_buffer and FT
		bgce_disconnect(conn_fd);
		if (ctx->ft_library)
			FT_Done_FreeType(ctx->ft_library);
		free(ctx);
		return NULL;
	}

	return ctx;
}

void bgtk_destroy(BGTK_Context* ctx) {
	if (!ctx)
		return;

	// Free all widgets
	for (size_t i = 0; i < ctx->widget_count; i++) {
		BGTK_Widget* w = ctx->widgets[i];
		if (w->type == BGTK_WIDGET_LABEL) {
			free(w->data.label.text);
		} else if (w->type == BGTK_WIDGET_BUTTON) {
			free(w->data.button.text);
		}
		free(w);
	}
	free(ctx->widgets);

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
#define BGTK_COLOR_BG 0xFFCCCCCC    // Light Gray
#define BGTK_COLOR_BTN 0xFF007BFF   // Blue
#define BGTK_COLOR_TEXT 0xFF000000  // Black
#define BGTK_COLOR_WHITE 0xFFFFFFFF // White

static void bgtk_clear_buffer(BGTK_Context* ctx, uint32_t color) {
	uint32_t* pixels = (uint32_t*)ctx->shm_buffer;
	size_t size = (size_t)ctx->buf_width * ctx->buf_height;
	for (size_t i = 0; i < size; i++) {
		pixels[i] = color;
	}
}

static void bgtk_draw_rect(BGTK_Context* ctx, int x, int y, int w, int h, uint32_t color) {
	// Basic clipping and drawing
	int x1 = x;
	int y1 = y;
	int x2 = x + w;
	int y2 = y + h;

	// Clip to buffer bounds
	if (x1 < 0)
		x1 = 0;
	if (y1 < 0)
		y1 = 0;
	if (x2 > (int)ctx->buf_width)
		x2 = ctx->buf_width;
	if (y2 > (int)ctx->buf_height)
		y2 = ctx->buf_height;

	uint32_t* pixels = (uint32_t*)ctx->shm_buffer;
	int stride = ctx->buf_width;

	for (int j = y1; j < y2; j++) {
		for (int i = x1; i < x2; i++) {
			pixels[j * stride + i] = color;
		}
	}
}

void measure_text(FT_Face face, const char* text,
                  int* out_width, int* out_height) {
	int width = 0;

	for (const char* p = text; *p; p++) {
		if (FT_Load_Char(face, *p, FT_LOAD_DEFAULT))
			continue;

		width += face->glyph->advance.x; // 26.6 units
	}

	width >>= 6;

	int ascent = face->size->metrics.ascender >> 6;
	int descent = -face->size->metrics.descender >> 6;
	int height = ascent + descent;

	*out_width = width;
	*out_height = height;
}

static void bgtk_draw_text(BGTK_Context* ctx, const char* text, int x, int y, uint32_t color) {
	if (!ctx->ft_face) {
		// Fallback to simple placeholder if font didn't load
		bgtk_draw_rect(ctx, x, y, 5, 5, color);
		return;
	}

	// Set font size for drawing context
	FT_Set_Pixel_Sizes(ctx->ft_face, 0, ctx->font_size);

	int pen_x = x;
	int pen_y = y + (ctx->ft_face->size->metrics.ascender >> 6);

	uint32_t* pixels = ctx->shm_buffer;
	int stride = ctx->buf_width;

	for (const char* p = text; *p; p++) {
		FT_UInt index = FT_Get_Char_Index(ctx->ft_face, *p);

		if (FT_Load_Glyph(ctx->ft_face, index, FT_LOAD_DEFAULT | FT_LOAD_TARGET_LIGHT))
			continue;

		FT_Render_Glyph(ctx->ft_face->glyph, FT_RENDER_MODE_NORMAL);

		FT_GlyphSlot slot = ctx->ft_face->glyph;
		FT_Bitmap* bitmap = &slot->bitmap;

		int gx = pen_x + slot->bitmap_left;
		int gy = pen_y - slot->bitmap_top;

                for (unsigned int row = 0; row < bitmap->rows; row++) {
                        for (unsigned int col = 0; col < bitmap->width; col++) {
				uint8_t a = bitmap->buffer[row * bitmap->pitch + col];
				if (a == 0)
					continue;

                                int32_t dx = gx + col;
                                int32_t dy = gy + row;
                                if (dx < 0 || dx >= (int)ctx->buf_width ||
                                    dy < 0 || dy >= (int)ctx->buf_height)
					continue;

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

				pixels[dy * stride + dx] = (r << 16) | (g << 8) | b;
			}
		}

		pen_x += slot->advance.x >> 6;
	}
}

static void bgtk_draw_widget(BGTK_Context* ctx, BGTK_Widget* w) {
	switch (w->type) {
	case BGTK_WIDGET_LABEL:
		// Draw label background
		bgtk_draw_rect(ctx, w->x, w->y, w->w, w->h, BGTK_COLOR_BG);
		// Draw text (offset for simple padding)
		bgtk_draw_text(ctx, w->data.label.text, w->x + 5, w->y + 5, BGTK_COLOR_TEXT);
		break;
	case BGTK_WIDGET_BUTTON:
		// Draw button background
		bgtk_draw_rect(ctx, w->x, w->y, w->w, w->h, BGTK_COLOR_BTN);
		// Draw button border (1px black)
		bgtk_draw_rect(ctx, w->x, w->y, w->w, 1, BGTK_COLOR_TEXT);            // Top
		bgtk_draw_rect(ctx, w->x, w->y + w->h - 1, w->w, 1, BGTK_COLOR_TEXT); // Bottom
		bgtk_draw_rect(ctx, w->x, w->y, 1, w->h, BGTK_COLOR_TEXT);            // Left
		bgtk_draw_rect(ctx, w->x + w->w - 1, w->y, 1, w->h, BGTK_COLOR_TEXT); // Right

		// Calculate text offset for rough centering
		// NOTE: A proper implementation would use FT_Get_Text_Width()
		int text_width;
		int text_height;
		measure_text(ctx->ft_face, w->data.button.text, &text_width, &text_height);

		// Draw text
		int offset_x = (w->w - text_width) / 2;
		int offset_y = (w->h - text_height) / 2;
		bgtk_draw_text(ctx, w->data.button.text, w->x + offset_x, w->y + offset_y, BGTK_COLOR_WHITE);
		break;
	}
}

static void bgtk_draw_widgets(BGTK_Context* ctx) {
	// 1. Clear the screen
	bgtk_clear_buffer(ctx, BGTK_COLOR_BG);

	// 2. Draw all widgets
	for (size_t i = 0; i < ctx->widget_count; i++) {
		bgtk_draw_widget(ctx, ctx->widgets[i]);
	}
}

static int bgtk_handle_input_event(BGTK_Context* ctx, struct InputEvent* ev) {
	// Only handle mouse button presses for now
	if (ev->code != BTN_LEFT || ev->value != 1) {
		return 0;
	}
	printf("BGTK Got click: (%d, %d)\n", ev->x, ev->y);

	// Hit-testing for buttons
	for (size_t i = 0; i < ctx->widget_count; i++) {
		BGTK_Widget* w = ctx->widgets[i];

		if (w->type == BGTK_WIDGET_BUTTON) {
			// Check if click coordinates are within button bounds
			if (ev->x >= w->x && ev->x < (w->x + w->w) &&
			    ev->y >= w->y && ev->y < (w->y + w->h)) {
				printf("BGTK Clicked in button\n");

				// Trigger callback
				if (w->data.button.callback) {
					w->data.button.callback();
					return 1;
				}
			}
		}
	}
	return 0;
}

static int bgtk_handle_events(BGTK_Context* ctx) {
	struct BGCEMessage msg;
	ssize_t bytes;

	// bgce_recv_msg is blocking. This makes the UI reactive.
	bytes = bgce_recv_msg(ctx->conn_fd, &msg);
	if (bytes <= 0) {
		if (bytes == 0) {
			fprintf(stderr, "bgtk_main_loop: Server closed connection.\n");
		} else if (errno != EINTR) {
			perror("bgtk_main_loop: bgce_recv_msg");
		}
		return -1; // Exit loop
	}

	switch (msg.type) {
	case MSG_INPUT_EVENT:
		return bgtk_handle_input_event(ctx, &msg.data.input_event);
	case MSG_BUFFER_CHANGE:
		// TODO: Handle buffer resize/move
		return 1; // Redraw
	default:
		// Ignore other messages for now
		return 0;
	}
}

void bgtk_main_loop(BGTK_Context* ctx) {
	bgtk_draw_widgets(ctx);
	bgce_draw(ctx->conn_fd);
	printf("BGTK Main Loop started. Press Ctrl+C to exit.\n");

	while (1) {
		int result = bgtk_handle_events(ctx);
		if (result == -1) {
			break; // Server error/disconnection
		} else if (result == 1) {
			printf("BGTK drawing.\n");
			bgce_draw(ctx->conn_fd);
		}
	}
}

// --- Widget Creation Functions ---

void set_label(BGTK_Widget* widget, char* label) {
	printf("BGTK: setting label: %s\n", label);
	if (widget->data.label.text) {
		free(widget->data.label.text);
	}

	char* ptr = calloc(1, strlen(label) + 1);
	sprintf(ptr, "%s", label);

	widget->data.label.text = ptr;

	measure_text(widget->ctx->ft_face, widget->data.button.text, &widget->w, &widget->h);

	// add padding
	widget->w += 5;
	widget->h += 5;

	bgtk_draw_widget(widget->ctx, widget);
	printf("BGTK label set\n");
}

BGTK_Widget* bgtk_label_new(BGTK_Context* ctx, char* text) {
	BGTK_Widget* widget = bgtk_widget_new(ctx, BGTK_WIDGET_LABEL);
	printf("BGTK allocated label\n");
	if (!widget) {
		perror("BGTK Failed to create new widget");
		return NULL;
	}

	widget->set_label = set_label;

	set_label(widget, text);

	return widget;
}

BGTK_Widget* bgtk_button_new(BGTK_Context* ctx, char* text, BGTK_Callback callback) {
	BGTK_Widget* widget = bgtk_widget_new(ctx, BGTK_WIDGET_BUTTON);
	if (!widget) {
		perror("BGTK Failed to create new widget");
		return NULL;
	}

	widget->data.button.callback = callback;

	char* ptr = calloc(1, strlen(text) + 1);
	sprintf(ptr, "%s", text);
	widget->data.button.text = ptr;

	measure_text(widget->ctx->ft_face, widget->data.button.text, &widget->w, &widget->h);

	// add padding
	widget->w += 20;
	widget->h += 20;
	return widget;
}

// --- Layout/Management Functions ---

void bgtk_add_widget(BGTK_Context* ctx, BGTK_Widget* widget, int x, int y, int w, int h) {
	if (!ctx || !widget)
		return;

	if (ctx->widget_count >= ctx->widget_capacity) {
		ctx->widget_capacity *= 2;
		BGTK_Widget** new_widgets = (BGTK_Widget**)realloc(ctx->widgets, ctx->widget_capacity * sizeof(BGTK_Widget*));
		if (!new_widgets) {
			perror("realloc");
			// In a real app, this should be handled better
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
