#include "bgtk.h"
#include "internal.h"
#include <stdio.h>

// A few basic colors (0xAARRGGBB)
#define BGTK_COLOR_BG 0xFFCCCCCC     // Light Gray
#define BGTK_COLOR_BTN 0xFF007BFF    // Blue
#define BGTK_COLOR_TEXT 0xFF000000   // Black
#define BGTK_COLOR_WHITE 0xFFFFFFFF  // White

void clear_buffer(struct BGTK_Context* ctx) {
	uint32_t* pixels = (uint32_t*)ctx->shm_buffer;
	size_t size = (size_t)ctx->width * ctx->height;
	for (size_t i = 0; i < size; i++) {
		pixels[i] = BGTK_COLOR_BG;
	}
}

void draw_rect(struct BGTK_Context* ctx, uint32_t* pixels, int x, int y,
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

void calculate_widget_size(struct BGTK_Context* ctx,
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

void draw_text(struct BGTK_Context* ctx, uint32_t* pixels,
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

void draw_widget(struct BGTK_Context* ctx, struct BGTK_Widget* w,
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


