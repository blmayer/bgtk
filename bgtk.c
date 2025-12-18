#include "bgtk.h"

#include <bgce.h>
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

// A few basic colors (0xAARRGGBB)
#define BGTK_COLOR_BG 0xFFCCCCCC     // Light Gray
#define BGTK_COLOR_BTN 0xFF007BFF    // Blue
#define BGTK_COLOR_TEXT 0xFF000000   // Black
#define BGTK_COLOR_WHITE 0xFFFFFFFF  // White

// Helper to create a generic widget
static struct BGTK_Widget* widget_new(struct BGTK_Context* ctx,
				      enum BGTK_Widget_Type type, int flags) {
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

struct BGTK_Context* bgtk_init(int conn_fd, void* buffer, int width,
			       int height) {
	struct BGTK_Context* ctx =
	    (struct BGTK_Context*)calloc(1, sizeof(struct BGTK_Context));
	if (!ctx) {
		perror("calloc");
		return NULL;
	}

	ctx->conn_fd = conn_fd;
	ctx->shm_buffer = buffer;
	ctx->font_size = DEFAULT_FONT_SIZE;
	ctx->width = width;
	ctx->height = height;
	ctx->root_widget = NULL;

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
		free(ctx);
		return NULL;
	}

	// Set font size
	FT_Set_Pixel_Sizes(ctx->ft_face, 0, ctx->font_size);

	return ctx;
}

void bgtk_destroy(struct BGTK_Context* ctx) {
	if (!ctx) {
		return;
	}

	// Free the root widget and its children recursively
	if (ctx->root_widget) {
		if (ctx->root_widget->type == BGTK_WIDGET_SCROLLABLE) {
			if (ctx->root_widget->data.scrollable.widgets) {
				for (int i = 0;
				     i < ctx->root_widget->data.scrollable
					     .widget_count;
				     i++) {
					free(ctx->root_widget->data.scrollable
						 .widgets[i]);
				}
				free(ctx->root_widget->data.scrollable.widgets);
			}
			if (ctx->root_widget->data.scrollable.tmp) {
				free(ctx->root_widget->data.scrollable.tmp);
			}
		} else if (ctx->root_widget->type == BGTK_WIDGET_LABEL) {
			if (ctx->root_widget->data.label.text) {
				free(ctx->root_widget->data.label.text->data
					 .text.text);
				free(ctx->root_widget->data.label.text);
			}
		} else if (ctx->root_widget->type == BGTK_WIDGET_BUTTON) {
			if (ctx->root_widget->data.button.label) {
				if (ctx->root_widget->data.button.label->data
					.label.text) {
					free(ctx->root_widget->data.button
						 .label->data.label.text->data
						 .text.text);
					free(ctx->root_widget->data.button
						 .label->data.label.text);
				}
				free(ctx->root_widget->data.button.label);
			}
		} else if (ctx->root_widget->type == BGTK_WIDGET_TEXT) {
			free(ctx->root_widget->data.text.text);
		}
		free(ctx->root_widget);
	}

	// Free FreeType resources
	if (ctx->ft_face) {
		FT_Done_Face(ctx->ft_face);
	}
	if (ctx->ft_library) {
		FT_Done_FreeType(ctx->ft_library);
	}

	free(ctx);
}

// --- Drawing Primitives & Widgets ---

static void clear_buffer(struct BGTK_Context* ctx, uint32_t color) {
	uint32_t* pixels = (uint32_t*)ctx->shm_buffer;
	size_t size = (size_t)ctx->width * ctx->height;
	for (size_t i = 0; i < size; i++) {
		pixels[i] = color;
	}
}

static void draw_rect(struct BGTK_Context* ctx, uint32_t* pixels, int x, int y,
		      int w, int h, uint32_t color) {
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
	if (x2 > (int)ctx->width) {
		x2 = ctx->width;
	}
	if (y2 > (int)ctx->height) {
		y2 = ctx->height;
	}

	int stride = ctx->width;

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

static void calculate_widget_size(struct BGTK_Context* ctx,
				  struct BGTK_Widget* w) {
	if (!w) {
		return;
	}

	switch (w->type) {
		case BGTK_WIDGET_LABEL:
			if (w->data.label.text) {
				calculate_widget_size(ctx, w->data.label.text);
				w->w =
				    w->data.label.text->w + 10;	 // Add padding
				w->h =
				    w->data.label.text->h + 10;	 // Add padding
			}
			break;
		case BGTK_WIDGET_TEXT:
			if (w->data.text.text) {
				measure_text(ctx->ft_face, w->data.text.text,
					     &w->w, &w->h);
			}
			printf("calcutated text size: %ux%u\n", w->w, w->h);
			break;
		case BGTK_WIDGET_BUTTON:
			if (w->data.button.label) {
				calculate_widget_size(ctx,
						      w->data.button.label);
				w->w = w->data.button.label->w +
				       20;  // Add padding
				w->h = w->data.button.label->h +
				       20;  // Add padding
			}
			printf("calcutated button size: %ux%u\n", w->w, w->h);
			break;
		case BGTK_WIDGET_SCROLLABLE:
			w->data.scrollable.content_height = 0;
			for (int i = 0; i < w->data.scrollable.widget_count;
			     i++) {
				struct BGTK_Widget* child =
				    w->data.scrollable.widgets[i];
				calculate_widget_size(ctx, child);
				w->data.scrollable.content_height +=
				    child->h + 5;  // 5px spacing
			}
			printf("calcutated scrollable size: %ux%u\n", w->w,
			       w->data.scrollable.content_height);
			break;
		default:
			break;
	}
}

static void draw_text(struct BGTK_Context* ctx, uint32_t* pixels,
		      const char* text, int x, int y, uint32_t color) {
	if (!ctx->ft_face) {
		// Fallback to simple placeholder if font didn't load
		draw_rect(ctx, pixels, x, y, 5, 5, color);
		return;
	}

	// Set font size for drawing context
	FT_Set_Pixel_Sizes(ctx->ft_face, 0, ctx->font_size);

	int pen_x = x;
	int pen_y = y + (ctx->ft_face->size->metrics.ascender >> 6);

	int stride = ctx->width;

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
				if (dx < 0 || dx >= (int)ctx->width || dy < 0 ||
				    dy >= (int)ctx->height) {
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

static void draw_widget(struct BGTK_Context* ctx, struct BGTK_Widget* w,
			uint32_t* pixels) {
	switch (w->type) {
		case BGTK_WIDGET_LABEL:

			// Draw label background
			draw_rect(ctx, pixels, w->x, w->y, w->w, w->h,
				  BGTK_COLOR_BG);
			// Draw text widget (offset for padding)
			if (w->data.label.text) {
				w->data.label.text->x = w->x + 5;
				w->data.label.text->y = w->y + 5;
				draw_widget(ctx, w->data.label.text, pixels);
			}
			break;
		case BGTK_WIDGET_TEXT:
			puts("drawing text widget");
			draw_text(ctx, pixels, w->data.text.text, w->x, w->y,
				  BGTK_COLOR_TEXT);
			break;
		case BGTK_WIDGET_BUTTON:
			puts("drawing button widget");
			// Draw button background
			draw_rect(ctx, pixels, w->x, w->y, w->w, w->h,
				  BGTK_COLOR_BTN);

			// Draw button border (1px black)
			draw_rect(ctx, pixels, w->x, w->y, w->w, 1,
				  BGTK_COLOR_TEXT);  // Top
			draw_rect(ctx, pixels, w->x, w->y + w->h - 1, w->w, 1,
				  BGTK_COLOR_TEXT);  // Bottom
			draw_rect(ctx, pixels, w->x, w->y, 1, w->h,
				  BGTK_COLOR_TEXT);  // Left
			draw_rect(ctx, pixels, w->x + w->w - 1, w->y, 1, w->h,
				  BGTK_COLOR_TEXT);  // Right

			// Draw label widget (offset for padding)
			if (w->data.button.label) {
				w->data.button.label->x = w->x + 10;
				w->data.button.label->y = w->y + 10;
				draw_widget(ctx, w->data.button.label, pixels);
			}
			break;
		case BGTK_WIDGET_SCROLLABLE:
			puts("drawing scrollable widget");
			// Allocate or update the off-screen buffer if
			// needed
			if (!w->data.scrollable.tmp) {
				int content_height =
				    w->data.scrollable.content_height;
				if (w->h > content_height) {
					content_height = w->h;
				}

				// Allocate the off-screen buffer
				w->data.scrollable.tmp = calloc(
				    w->w * content_height, sizeof(uint32_t));
				if (!w->data.scrollable.tmp) {
					fprintf(stderr,
						"Failed to allocate "
						"off-screen "
						"buffer\n");
					break;
				}
				draw_rect(ctx, w->data.scrollable.tmp, 0, 0,
					  w->w,
					  w->data.scrollable.content_height,
					  BGTK_COLOR_BG);

				// Draw child widgets into the
				// off-screen buffer
				int current_y = 0;
				for (int i = 0;
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
					draw_widget(ctx, child,
						    w->data.scrollable.tmp);
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
					    &buff[(w->y + row) * ctx->width +
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

void bgtk_draw_widgets(struct BGTK_Context* ctx) {
	puts("got draw widgets request");
	clear_buffer(ctx, BGTK_COLOR_BG);
	calculate_widget_size(ctx, ctx->root_widget);
	draw_widget(ctx, ctx->root_widget, ctx->shm_buffer);
	bgce_draw(ctx->conn_fd);
}

// TODO: implement descending into child widgets
int bgtk_handle_input_event(struct BGTK_Context* ctx, struct InputEvent ev) {
	// Handle mouse wheel for scrolling (REL_WHEEL)
	if (ev.code == REL_WHEEL) {
		printf("handling mouse wheel: val=%d at (%u, %u)\n", ev.value,
		       ev.x, ev.y);
		struct BGTK_Widget* w = ctx->root_widget;

		// TODO: descend to the last scrollable widget
		// while (1) {
		if (w->type == BGTK_WIDGET_SCROLLABLE) {
			puts("found scroll widget");

			// Check if mouse is over the scrollable
			// widget
			if (ev.x >= w->x && ev.x < (w->x + w->w) &&
			    ev.y >= w->y && ev.y < (w->y + w->h)) {
				puts("updating scroll");
				w->data.scrollable.scroll_y -=
				    ev.value * 10;  // Scroll speed

				// Clamp scroll_y to valid range
				if (w->data.scrollable.scroll_y < 0) {
					w->data.scrollable.scroll_y = 0;
				}
				if (w->data.scrollable.scroll_y >
				    w->data.scrollable.content_height - w->h) {
					w->data.scrollable.scroll_y =
					    w->data.scrollable.content_height -
					    w->h;
				}
				if (w->data.scrollable.scroll_y < 0) {
					w->data.scrollable.scroll_y = 0;
				}
				printf(
				    "updated scroll position: "
				    "%d\n",
				    w->data.scrollable.scroll_y);
				draw_widget(ctx, w, ctx->shm_buffer);

				return 1;  // Redraw
			}
		}
		//}
	}

	// Only handle mouse button presses for now
	if (ev.code != BTN_LEFT || ev.value != 1) {
		return 0;
	}
	printf("BGTK Got click: (%d, %d)\n", ev.x, ev.y);

	// Hit-testing for buttons
	struct BGTK_Widget* w = ctx->root_widget;
	while (w) {
		switch (w->type) {
			case BGTK_WIDGET_BUTTON:
				// Check if click coordinates
				// are within button bounds
				if (ev.x >= w->x && ev.x < (w->x + w->w) &&
				    ev.y >= w->y && ev.y < (w->y + w->h)) {
					printf("BGTK Clicked in button\n");

					// Trigger callback
					if (w->data.button.callback) {
						w->data.button.callback();
						return 1;
					}
				}
				return 0;
			case BGTK_WIDGET_SCROLLABLE: {
				printf("clicked in a scrollable widget\n");
				w = NULL;
				struct BGTK_Widget* item = NULL;
				for (int i = 0;
				     i < w->data.scrollable.widget_count; i++) {
					item = w->data.scrollable.widgets[i];
					if (ev.x >= item->x &&
					    ev.x < (item->x + item->w) &&
					    ev.y >= item->y &&
					    ev.y < (item->y + item->h)) {
						printf(
						    "clicked in the %d "
						    "widget\n",
						    i);
						w = item;
						break;
					}
				}
				break;
			}
			default:
				printf("clicked on a widget without action\n");
				return 0;
		}
	}
	return 0;
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
		perror(
		    "BGTK Failed to create text widget for "
		    "label");
		return;
	}

	widget->data.label.text = text_widget;

	// Calculate size based on text widget
	widget->w = text_widget->w + 10;  // Add padding
	widget->h = text_widget->h + 10;  // Add padding

	draw_widget(widget->ctx, widget, widget->ctx->shm_buffer);
	printf("BGTK label set\n");
}

struct BGTK_Widget* bgtk_label(struct BGTK_Context* ctx, char* text) {
	struct BGTK_Widget* widget = widget_new(ctx, BGTK_WIDGET_LABEL, 0);
	printf("BGTK allocated label\n");
	if (!widget) {
		perror("BGTK Failed to create new widget");
		return NULL;
	}

	widget->set_label = set_label;

	// Create a text widget for the label
	struct BGTK_Widget* text_widget = bgtk_text(ctx, text, 0);
	if (!text_widget) {
		perror(
		    "BGTK Failed to create text widget for "
		    "label");
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
	printf("BGTK creating text widget\n");
	struct BGTK_Widget* widget = widget_new(ctx, BGTK_WIDGET_TEXT, flags);
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
	printf("BGTK creating button widget\n");
	struct BGTK_Widget* widget = widget_new(ctx, BGTK_WIDGET_BUTTON, flags);
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

// Helper to recalculate the content height of a scrollable
// widget
struct BGTK_Widget* bgtk_scrollable(struct BGTK_Context* ctx,
				    struct BGTK_Widget** widgets,
				    int widget_count, int flags) {
	printf("BGTK creating scrollable widget\n");
	struct BGTK_Widget* widget =
	    widget_new(ctx, BGTK_WIDGET_SCROLLABLE, flags);
	if (!widget) {
		perror("BGTK Failed to create scrollable widget");
		return NULL;
	}

	widget->data.scrollable.widgets = (struct BGTK_Widget**)calloc(
	    widget_count, sizeof(struct BGTK_Widget*));
	if (!widget->data.scrollable.widgets) {
		perror("calloc");
		free(widget);
		return NULL;
	}

	// Copy the input widgets into the scrollable container
	widget->data.scrollable.widget_count = widget_count;
	widget->data.scrollable.scroll_y = 0;
	widget->data.scrollable.content_height = 0;
	for (int i = 0; i < widget_count; i++) {
		widget->data.scrollable.widgets[i] = widgets[i];
		widget->data.scrollable.content_height +=
		    widgets[i]->h + 5;	// 5px spacing
	}

	// Initialize tmp buffer to NULL, it will be allocated
	// during drawing
	widget->data.scrollable.tmp = NULL;
	printf("BGTK allocated scrollable widget\n");

	return widget;
}
