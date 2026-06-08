#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bgtk.h"
#include "internal.h"

// Define STB_IMAGE_IMPLEMENTATION in one source file
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

// Loads an image file into a pixel buffer (RGBA format).
// Returns 0 on success, -1 on failure.
int load_image(const char *path, uint32_t **out_pixels, int *out_w, int *out_h)
{
	int w, h, channels;
	unsigned char *pixels = stbi_load(path, &w, &h, &channels, 4);
	if (!pixels) {
		fprintf(stderr, "Failed to load image: %s\n", path);
		return -1;
	}
	// Convert to uint32_t (RGBA)
	*out_pixels = (uint32_t *) pixels;
	*out_w = w;
	*out_h = h;
	return 0;
}

void clear_buffer(struct BGTK_Context *ctx)
{
	uint32_t *pixels = (uint32_t *) ctx->shm_buffer;
	size_t size = (size_t)ctx->width * ctx->height;
	for (size_t i = 0; i < size; i++) {
		pixels[i] = ctx->theme.background;
	}
}

void draw_rect(struct BGTK_Context *ctx, uint32_t *pixels, int x, int y, int w,
	       int h, uint32_t color)
{
	// Basic clipping and drawing
	int x1 = x;
	int y1 = y;
	int x2 = x + w;
	int y2 = y + h;

	// TODO: stride can be of a tmp buffer != from ctx
	int stride = ctx->width;
	for (int j = y1; j < y2; j++) {
		for (int i = x1; i < x2; i++) {
			pixels[j * stride + i] = color;
		}
	}
}

void measure_text(FT_Face face, const char *text, int *out_width,
		  int *out_height)
{
	int width = 0;

	for (const char *p = text; *p; p++) {
		if (FT_Load_Char(face, *p, FT_LOAD_DEFAULT)) {
			continue;
		}

		width += face->glyph->advance.x;	// 26.6 units
	}

	width >>= 6;

	int ascent = face->size->metrics.ascender >> 6;
	int descent = -face->size->metrics.descender >> 6;
	int height = ascent + descent;

	*out_width = width;
	*out_height = height;
}

void calculate_widget_size(struct BGTK_Context *ctx, struct BGTK_Widget *w)
{
	if (!w) {
		return;
	}

	switch (w->type) {
	case BGTK_WIDGET_LABEL:
		if (w->data.label.text) {
			calculate_widget_size(ctx, w->data.label.text);
			w->w = w->data.label.text->w +
			    2 * (w->padding + w->margin);
			w->h = w->data.label.text->h +
			    2 * (w->padding + w->margin);
		}
		break;
	case BGTK_WIDGET_TEXT:
		if (w->data.text.text) {
			measure_text(ctx->ft_face, w->data.text.text,
				     &w->w, &w->h);
			// Add padding + margin to the text widget
			// (outer size)
			w->w += 2 * (w->padding + w->margin);
			w->h += 2 * (w->padding + w->margin);
		}
		break;
	case BGTK_WIDGET_BUTTON:
		if (w->data.button.label) {
			calculate_widget_size(ctx, w->data.button.label);
			// Account for border drawn in draw_widget().
			int border_w = ctx->theme.button_border_size;
			if (border_w < 1) {
				border_w = 1;
			}
			w->w = w->data.button.label->w +
			    2 * (w->margin + w->padding + border_w);
			w->h = w->data.button.label->h +
			    2 * (w->margin + w->padding + border_w);
		}
		break;

	case BGTK_WIDGET_SCROLLABLE:
		w->data.scrollable.content_height = 0;
		for (int i = 0; i < w->data.scrollable.widget_count; i++) {
			struct BGTK_Widget *child = w->data.scrollable.items[i];
			calculate_widget_size(ctx, child);
			w->data.scrollable.content_height +=
			    child->h + 2 * w->margin;
		}

		// Subtract the last margin (no margin after the last
		// widget)
		if (w->data.scrollable.widget_count > 0) {
			w->data.scrollable.content_height -= 2 * w->margin;
		}
		break;
	case BGTK_WIDGET_LIST:
		w->data.list_widget.content_width = 0;
		w->data.list_widget.content_height = 0;
		int max_width = 0;
		int max_height = 0;
		for (int i = 0; i < w->data.list_widget.widget_count; i++) {
			struct BGTK_Widget *child =
			    w->data.list_widget.items[i];
			calculate_widget_size(ctx, child);
			if (w->data.list_widget.orientation ==
			    BGTK_LIST_VERTICAL) {
				w->data.list_widget.content_height +=
				    child->h + 2 * w->margin;
				if (child->w > max_width) {
					max_width = child->w;
				}
			} else {	// BGTK_LIST_HORIZONTAL
				w->data.list_widget.content_width +=
				    child->w + 2 * w->margin;
				if (child->h > max_height) {
					max_height = child->h;
				}
			}
		}

		// Subtract the last margin
		if (w->data.list_widget.widget_count > 0) {
			if (w->data.list_widget.orientation ==
			    BGTK_LIST_VERTICAL) {
				w->data.list_widget.content_height -=
				    2 * w->margin;
			} else {
				w->data.list_widget.content_width -=
				    2 * w->margin;
			}
		}
		// Use max dimensions for the other axis
		if (w->data.list_widget.orientation == BGTK_LIST_VERTICAL) {
			w->data.list_widget.content_width = max_width;
		} else {
			w->data.list_widget.content_height = max_height;
		}
		break;
	case BGTK_WIDGET_IMAGE:
	case BGTK_WIDGET_TEXT_INPUT:
		// Text input is a fixed-size widget; its size is set by
		// the constructor (bgtk_text_input). Do not resize
		// based on content.
		break;
	default:
		break;
	}
}

void draw_text(struct BGTK_Context *ctx, uint32_t *pixels, const char *text,
	       int x, int y, uint32_t color)
{
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
	for (const char *p = text; *p; p++) {
		FT_UInt index = FT_Get_Char_Index(ctx->ft_face, *p);

		if (FT_Load_Glyph(ctx->ft_face, index,
				  FT_LOAD_DEFAULT | FT_LOAD_TARGET_LIGHT)) {
			continue;
		}

		FT_Render_Glyph(ctx->ft_face->glyph, FT_RENDER_MODE_NORMAL);

		FT_GlyphSlot slot = ctx->ft_face->glyph;
		FT_Bitmap *bitmap = &slot->bitmap;

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

static void draw_image(struct BGTK_Context *ctx, struct BGTK_Widget w,
		       uint32_t *pixels)
{
	if (!ctx || w.w <= 0 || w.h <= 0) {
		return;
	}

	int stride = ctx->width;

	// Clip to destination buffer to avoid OOB writes.
	int x0 = w.x;
	int y0 = w.y;
	int x1 = w.x + w.w;
	int y1 = w.y + w.h;

	if (x0 < 0) {
		x0 = 0;
	}
	if (y0 < 0) {
		y0 = 0;
	}
	if (x1 > ctx->width) {
		x1 = ctx->width;
	}
	if (y1 > ctx->height) {
		y1 = ctx->height;
	}

	if (x1 <= x0 || y1 <= y0) {
		return;
	}
	// Always fill the entire widget area with background first.
	// This ensures any region not covered by the image is deterministic.
	for (int dy = y0; dy < y1; dy++) {
		uint32_t *row = &pixels[dy * stride + x0];
		for (int dx = x0; dx < x1; dx++) {
			*row++ = ctx->theme.background;
		}
	}

	// Nothing more to do if there's no image.
	if (!w.data.image.pixels || w.data.image.img_w <= 0 ||
	    w.data.image.img_h <= 0) {
		return;
	}
	// Blit only the intersection of widget rect and image rect.
	int blit_w = w.w;
	int blit_h = w.h;
	if (blit_w > w.data.image.img_w) {
		blit_w = w.data.image.img_w;
	}
	if (blit_h > w.data.image.img_h) {
		blit_h = w.data.image.img_h;
	}

	int bx0 = w.x;
	int by0 = w.y;
	int bx1 = w.x + blit_w;
	int by1 = w.y + blit_h;

	if (bx0 < 0) {
		bx0 = 0;
	}
	if (by0 < 0) {
		by0 = 0;
	}
	if (bx1 > ctx->width) {
		bx1 = ctx->width;
	}
	if (by1 > ctx->height) {
		by1 = ctx->height;
	}

	if (bx1 <= bx0 || by1 <= by0) {
		return;
	}

	int src_x0 = bx0 - w.x;
	int src_y0 = by0 - w.y;
	int copy_w = bx1 - bx0;

	for (int dy = by0; dy < by1; dy++) {
		int sy = src_y0 + (dy - by0);
		memcpy(&pixels[dy * stride + bx0],
		       &w.data.image.pixels[sy * w.data.image.img_w + src_x0],
		       (size_t)copy_w * sizeof(uint32_t));
	}
}

static void draw_label(struct BGTK_Context *ctx, struct BGTK_Widget *w,
		       uint32_t *pixels)
{
	// Draw label background
	draw_rect(ctx, pixels, w->x + w->margin, w->y + w->margin,
		  w->w - 2 * w->margin, w->h - 2 * w->margin,
		  ctx->theme.background);
	// Draw text widget (offset for padding and margin)
	if (w->data.label.text) {
		w->data.label.text->x = w->x + w->margin + w->padding;
		w->data.label.text->y = w->y + w->margin + w->padding;
		draw_widget(ctx, w->data.label.text, pixels);
	}
}

static void draw_text_widget(struct BGTK_Context *ctx, struct BGTK_Widget *w,
			     uint32_t *pixels)
{
	draw_text(ctx, pixels, w->data.text.text, w->x + w->margin + w->padding,
		  w->y + w->margin + w->padding, ctx->theme.button_text);
}

static void draw_button(struct BGTK_Context *ctx, struct BGTK_Widget *w,
			uint32_t *pixels)
{
	uint32_t bg = ctx->theme.button;
	if (w->data.button.pressed) {
		// TODO: create a config them for this color
		uint8_t a = (bg >> 24) & 0xFF;
		uint8_t r = (bg >> 16) & 0xFF;
		uint8_t g = (bg >> 8) & 0xFF;
		uint8_t b = (bg) & 0xFF;
		r = (uint8_t) ((r * 205) / 255);
		g = (uint8_t) ((g * 205) / 255);
		b = (uint8_t) ((b * 205) / 255);
		bg = (a << 24) | (r << 16) | (g << 8) | b;
	}
	// Background (inside margin)
	draw_rect(ctx, pixels, w->x + w->margin, w->y + w->margin,
		  w->w - 2 * w->margin, w->h - 2 * w->margin, bg);

	// Border
	int bw = ctx->theme.button_border_size;

	uint32_t border = ctx->theme.button_text;
	draw_rect(ctx, pixels, w->x + w->margin, w->y + w->margin, w->w - 2 * w->margin, bw, border);	// Top
	draw_rect(ctx, pixels, w->x + w->margin, w->y + w->h - bw - w->margin, w->w - 2 * w->margin, bw, border);	// Bottom
	draw_rect(ctx, pixels, w->x + w->margin, w->y + w->margin, bw, w->h - 2 * w->margin, border);	// Left
	draw_rect(ctx, pixels, w->x + w->w - bw - w->margin, w->y + w->margin, bw, w->h - 2 * w->margin, border);	// Right

	if (w->data.button.label) {
		int off = w->data.button.pressed ? 1 : 0;

		int inner_x0 = w->x + w->margin + bw;
		int inner_y0 = w->y + w->margin + bw;
		int inner_w = w->w - 2 * w->margin - 2 * bw;
		int inner_h = w->h - 2 * w->margin - 2 * bw;
		if (inner_w < 0) {
			inner_w = 0;
		}
		if (inner_h < 0) {
			inner_h = 0;
		}

		int content_x0 = inner_x0 + w->padding;
		int content_y0 = inner_y0 + w->padding;
		int content_w = inner_w - 2 * w->padding;
		int content_h = inner_h - 2 * w->padding;
		if (content_w < 0) {
			content_w = 0;
		}
		if (content_h < 0) {
			content_h = 0;
		}

		int lx = content_x0 +
		    (content_w - w->data.button.label->w) / 2 + off;
		int ly = content_y0 +
		    (content_h - w->data.button.label->h) / 2 + off;

		w->data.button.label->x = lx;
		w->data.button.label->y = ly;
		draw_widget(ctx, w->data.button.label, pixels);
	}
}

static void draw_scrollable(struct BGTK_Context *ctx, struct BGTK_Widget *w,
			    uint32_t *pixels)
{
	(void)pixels;
	int content_height = w->data.scrollable.content_height;
	if (w->h > content_height) {
		content_height = w->h;
	}

	if (!w->data.scrollable.tmp) {
		w->data.scrollable.tmp =
		    calloc(w->w * content_height, sizeof(uint32_t));
		if (!w->data.scrollable.tmp) {
			fprintf(stderr,
				"Failed to allocate off-screen buffer\n");
			return;
		}
	}
	// Draw into temp buffer with correct stride.
	struct BGTK_Context tmp_ctx = *ctx;
	tmp_ctx.width = w->w;
	tmp_ctx.height = content_height;

	draw_rect(&tmp_ctx, w->data.scrollable.tmp, 0, 0, w->w, content_height,
		  ctx->theme.background);

	int current_y = 0;
	for (int i = 0; i < w->data.scrollable.widget_count; i++) {
		struct BGTK_Widget *child = w->data.scrollable.items[i];

		child->x = w->margin + w->padding;
		if (w->flags & BGTK_FLAG_CENTER) {
			child->x =
			    w->margin + (w->w - 2 * w->margin - child->w) / 2;
		}
		child->y = current_y + w->margin;

		draw_widget(&tmp_ctx, child, w->data.scrollable.tmp);
		current_y += child->h + 2 * w->margin;
	}

	uint32_t *buff = ctx->shm_buffer;
	uint32_t *tmp = w->data.scrollable.tmp;
	for (int row = 0; row < w->h; row++) {
		int src_row = w->data.scrollable.scroll_y + row;
		if (src_row < content_height) {
			memcpy(&buff[(w->y + row) * ctx->width + w->x],
			       &tmp[src_row * w->w], w->w * 4);
		}
	}
}

static void draw_list(struct BGTK_Context *ctx, struct BGTK_Widget *w,
		      uint32_t *pixels)
{
	// Draw list widget children directly
	// (no scrolling)
	int current_x = 0;
	int current_y = 0;

	for (int i = 0; i < w->data.list_widget.widget_count; i++) {
		struct BGTK_Widget *child = w->data.list_widget.items[i];

		if (w->data.list_widget.orientation == BGTK_LIST_VERTICAL) {
			child->x = w->x + w->margin + w->padding;
			if (w->flags & BGTK_FLAG_CENTER) {
				child->x =
				    w->x + w->margin +
				    (w->w - 2 * w->margin - child->w) / 2;
			}
			child->y = w->y + w->margin + w->padding + current_y;
			current_y += child->h + 2 * w->margin;
		} else {	// BGTK_LIST_HORIZONTAL
			child->x = w->x + w->margin + w->padding + current_x;
			child->y = w->y + w->margin + w->padding;
			if (w->flags & BGTK_FLAG_CENTER) {
				child->y =
				    w->y + w->margin +
				    (w->h - 2 * w->margin - child->h) / 2;
			}
			current_x += child->w + 2 * w->margin;
		}

		draw_widget(ctx, child, pixels);
	}
}

static void draw_image_widget(struct BGTK_Context *ctx, struct BGTK_Widget *w,
			      uint32_t *pixels)
{
	struct BGTK_Widget adjusted_widget = *w;
	adjusted_widget.x += w->margin + w->padding;
	adjusted_widget.y += w->margin + w->padding;
	adjusted_widget.w -= 2 * (w->margin + w->padding);
	adjusted_widget.h -= 2 * (w->margin + w->padding);
	if (adjusted_widget.w < 0) {
		adjusted_widget.w = 0;
	}
	if (adjusted_widget.h < 0) {
		adjusted_widget.h = 0;
	}
	draw_image(ctx, adjusted_widget, pixels);
}

static void draw_frame(struct BGTK_Context *ctx, struct BGTK_Widget *w,
		       uint32_t *pixels)
{
	// Draw frame background
	draw_rect(ctx, pixels, w->x + w->margin, w->y + w->margin,
		  w->w - 2 * w->margin, w->h - 2 * w->margin,
		  ctx->theme.background);

	uint32_t border = ctx->theme.frame_border_color;

	// If the whole window is unfocused, dim
	// heavily.
	if (!ctx->window_focused) {
		border = ctx->theme.background;
	}
	// Draw frame border
	int bw = w->data.frame.border_w;
	if (bw < 1) {
		bw = 1;
	}
	draw_rect(ctx, pixels, w->x + w->margin, w->y + w->margin, w->w - 2 * w->margin, bw, border);	// Top
	draw_rect(ctx, pixels, w->x + w->margin, w->y + w->h - w->margin - bw, w->w - 2 * w->margin, bw, border);	// Bottom
	draw_rect(ctx, pixels, w->x + w->margin, w->y + w->margin, bw, w->h - 2 * w->margin, border);	// Left
	draw_rect(ctx, pixels, w->x + w->w - w->margin - bw, w->y + w->margin, bw, w->h - 2 * w->margin, border);	// Right

	// Draw child widget inside the frame
	if (w->data.frame.child) {
		w->data.frame.child->x = w->x + w->margin + bw + w->padding;
		w->data.frame.child->y = w->y + w->margin + bw + w->padding;
		w->data.frame.child->w =
		    w->w - 2 * (w->margin + bw + w->padding);
		w->data.frame.child->h =
		    w->h - 2 * (w->margin + bw + w->padding);
		draw_widget(ctx, w->data.frame.child, pixels);
	}
}

static void draw_text_input(struct BGTK_Context *ctx, struct BGTK_Widget *w,
			    uint32_t *pixels)
{
	// Draw background (white)
	draw_rect(ctx, pixels, w->x + w->margin, w->y + w->margin,
		  w->w - 2 * w->margin, w->h - 2 * w->margin, 0xFFFFFFFF);

	int focused = ctx->focused_widget == w;
	uint32_t border = focused ? 0xFF0066FF : 0xFF000000;

	int bw = ctx->theme.input_border_size;
	if (bw < 1) {
		bw = 1;
	}
	if (bw * 2 > w->w - 2 * w->margin) {
		bw = (w->w - 2 * w->margin) / 2;
	}
	if (bw * 2 > w->h - 2 * w->margin) {
		bw = (w->h - 2 * w->margin) / 2;
	}
	if (bw < 1) {
		bw = 1;
	}

	draw_rect(ctx, pixels, w->x + w->margin, w->y + w->margin, w->w - 2 * w->margin, bw, border);	// Top
	draw_rect(ctx, pixels, w->x + w->margin, w->y + w->h - bw - w->margin, w->w - 2 * w->margin, bw, border);	// Bottom
	draw_rect(ctx, pixels, w->x + w->margin, w->y + w->margin, bw, w->h - 2 * w->margin, border);	// Left
	draw_rect(ctx, pixels, w->x + w->w - bw - w->margin, w->y + w->margin, bw, w->h - 2 * w->margin, border);	// Right

	int inner_x0 = w->x + w->margin + bw;
	int inner_y0 = w->y + w->margin + bw;
	int inner_w = w->w - 2 * w->margin - 2 * bw;
	int inner_h = w->h - 2 * w->margin - 2 * bw;
	if (inner_w < 1) {
		inner_w = 1;
	}
	if (inner_h < 1) {
		inner_h = 1;
	}

	int text_x = w->x + w->margin + bw + w->padding;
	int text_y = w->y + w->margin + bw + w->padding;
	int scroll_x = w->data.text_input.scroll_x;
	if (scroll_x < 0) {
		scroll_x = 0;
	}

	int draw_x = text_x - scroll_x;
	const char *full =
	    w->data.text_input.text ? w->data.text_input.text : "";

	if (!ctx->ft_face) {
		return;
	}

	int stride = ctx->width;
	FT_Set_Pixel_Sizes(ctx->ft_face, 0, ctx->font_size);
	int pen_x = draw_x;
	int pen_y = text_y + (ctx->ft_face->size->metrics.ascender >> 6);

	for (const char *p = full; *p; p++) {
		FT_UInt index = FT_Get_Char_Index(ctx->ft_face, *p);
		if (FT_Load_Glyph(ctx->ft_face, index,
				  FT_LOAD_DEFAULT | FT_LOAD_TARGET_LIGHT)) {
			continue;
		}
		FT_Render_Glyph(ctx->ft_face->glyph, FT_RENDER_MODE_NORMAL);

		FT_GlyphSlot slot = ctx->ft_face->glyph;
		FT_Bitmap *bitmap = &slot->bitmap;
		int gx = pen_x + slot->bitmap_left;
		int gy = pen_y - slot->bitmap_top;

		int glyph_x0 = gx;
		int glyph_x1 = gx + (int)bitmap->width;
		if (glyph_x1 <= inner_x0) {
			pen_x += slot->advance.x >> 6;
			continue;
		}
		if (glyph_x0 >= inner_x0 + inner_w) {
			break;
		}

		for (unsigned int row = 0; row < bitmap->rows; row++) {
			int32_t dy = gy + (int)row;
			if (dy < inner_y0 || dy >= inner_y0 + inner_h) {
				continue;
			}
			for (unsigned int col = 0; col < bitmap->width; col++) {
				int32_t dx = gx + (int)col;
				if (dx < inner_x0 || dx >= inner_x0 + inner_w) {
					continue;
				}

				uint8_t a =
				    bitmap->buffer[row * bitmap->pitch + col];
				if (a == 0) {
					continue;
				}

				uint32_t dst = pixels[dy * stride + dx];
				uint8_t inv = 255 - a;

				uint8_t r_dst = (dst >> 16) & 0xFF;
				uint8_t g_dst = (dst >> 8) & 0xFF;
				uint8_t b_dst = (dst) & 0xFF;

				uint8_t r_src = (0xFF000000 >> 16) & 0xFF;
				uint8_t g_src = (0xFF000000 >> 8) & 0xFF;
				uint8_t b_src = (0xFF000000) & 0xFF;

				uint8_t r = (r_src * a + r_dst * inv) / 255;
				uint8_t g = (g_src * a + g_dst * inv) / 255;
				uint8_t b = (b_src * a + b_dst * inv) / 255;

				pixels[dy * stride + dx] =
				    (r << 16) | (g << 8) | b;
			}
		}

		pen_x += slot->advance.x >> 6;
	}

	if (focused) {
		int cursor_x = text_x - scroll_x;
		for (uint32_t i = 0; i < w->data.text_input.cursor_pos; i++) {
			FT_UInt index =
			    FT_Get_Char_Index(ctx->ft_face,
					      w->data.text_input.text[i]);
			if (FT_Load_Glyph(ctx->ft_face, index, FT_LOAD_DEFAULT)) {
				continue;
			}
			cursor_x += ctx->ft_face->glyph->advance.x >> 6;
		}

		if (cursor_x < inner_x0) {
			cursor_x = inner_x0;
		}
		if (cursor_x > inner_x0 + inner_w - 1) {
			cursor_x = inner_x0 + inner_w - 1;
		}

		draw_rect(ctx, pixels, cursor_x, inner_y0, 1, inner_h,
			  0xFF000000);
	}
}

void draw_widget(struct BGTK_Context *ctx, struct BGTK_Widget *w,
		 uint32_t *pixels)
{
	switch (w->type) {
	case BGTK_WIDGET_LABEL:
		draw_label(ctx, w, pixels);
		break;
	case BGTK_WIDGET_TEXT:
		draw_text_widget(ctx, w, pixels);
		break;
	case BGTK_WIDGET_BUTTON:
		draw_button(ctx, w, pixels);
		break;
	case BGTK_WIDGET_SCROLLABLE:
		draw_scrollable(ctx, w, pixels);
		break;
	case BGTK_WIDGET_LIST:
		draw_list(ctx, w, pixels);
		break;
	case BGTK_WIDGET_IMAGE:
		draw_image_widget(ctx, w, pixels);
		break;
	case BGTK_WIDGET_FRAME:
		draw_frame(ctx, w, pixels);
		break;
	case BGTK_WIDGET_TEXT_INPUT:
		draw_text_input(ctx, w, pixels);
		break;
	default:
		puts("can't draw unknown widget");
		break;
	}
}
