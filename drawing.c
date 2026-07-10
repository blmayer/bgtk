#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_OUTLINE_H
#include FT_SYNTHESIS_H

#include "bgtk.h"
#include "internal.h"

// Define STB_IMAGE_IMPLEMENTATION in one source file
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

/* Pack 8-bit channels into framebuffer pixel format 0xAARRGGBB (BGCE/XRGB). */
static uint32_t pack_argb(uint8_t a, uint8_t r, uint8_t g, uint8_t b)
{
	return ((uint32_t)a << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) |
	       (uint32_t)b;
}

/* ------------------------------------------------------------------ */
/* UTF-8 decode (no allocation; FreeType wants Unicode codepoints)    */
/* ------------------------------------------------------------------ */

uint32_t bgtk_utf8_next_n(const char **s, size_t *nleft)
{
	const unsigned char *p;
	size_t n;
	uint32_t c;

	if (!s || !*s || !nleft || *nleft == 0)
		return 0;
	p = (const unsigned char *)*s;
	n = *nleft;
	c = p[0];

	/* ASCII */
	if (c < 0x80) {
		*s = (const char *)(p + 1);
		*nleft = n - 1;
		return c;
	}

	/* 2-byte: 110xxxxx 10xxxxxx */
	if ((c & 0xE0) == 0xC0) {
		if (n < 2 || (p[1] & 0xC0) != 0x80)
			goto bad;
		c = ((c & 0x1Fu) << 6) | (p[1] & 0x3Fu);
		if (c < 0x80)
			goto bad; /* overlong */
		*s = (const char *)(p + 2);
		*nleft = n - 2;
		return c;
	}

	/* 3-byte: 1110xxxx 10xxxxxx 10xxxxxx */
	if ((c & 0xF0) == 0xE0) {
		if (n < 3 || (p[1] & 0xC0) != 0x80 || (p[2] & 0xC0) != 0x80)
			goto bad;
		c = ((c & 0x0Fu) << 12) | ((p[1] & 0x3Fu) << 6) | (p[2] & 0x3Fu);
		if (c < 0x800 || (c >= 0xD800 && c <= 0xDFFF))
			goto bad; /* overlong / surrogate */
		*s = (const char *)(p + 3);
		*nleft = n - 3;
		return c;
	}

	/* 4-byte: 11110xxx 10xxxxxx 10xxxxxx 10xxxxxx */
	if ((c & 0xF8) == 0xF0) {
		if (n < 4 || (p[1] & 0xC0) != 0x80 || (p[2] & 0xC0) != 0x80 ||
		    (p[3] & 0xC0) != 0x80)
			goto bad;
		c = ((c & 0x07u) << 18) | ((p[1] & 0x3Fu) << 12) |
		    ((p[2] & 0x3Fu) << 6) | (p[3] & 0x3Fu);
		if (c < 0x10000 || c > 0x10FFFF)
			goto bad;
		*s = (const char *)(p + 4);
		*nleft = n - 4;
		return c;
	}

bad:
	/* Skip one byte; render replacement if caller cares. */
	*s = (const char *)(p + 1);
	*nleft = n - 1;
	return 0xFFFD;
}

uint32_t bgtk_utf8_next(const char **s)
{
	size_t n;

	if (!s || !*s || !**s)
		return 0;
	/* Bound by remaining string length (safe for untrusted input). */
	n = strlen(*s);
	return bgtk_utf8_next_n(s, &n);
}

static int load_cp(FT_Face face, uint32_t cp, int style)
{
	if (!face)
		return -1;
	if (FT_Load_Char(face, (FT_ULong)cp,
			 FT_LOAD_DEFAULT | FT_LOAD_NO_BITMAP))
		return -1;
	if ((style & BGTK_TEXT_BOLD) &&
	    face->glyph->format == FT_GLYPH_FORMAT_OUTLINE)
		FT_GlyphSlot_Embolden(face->glyph);
	return 0;
}

// Loads an image file into a pixel buffer as 0xAARRGGBB uint32 pixels.
// stbi gives RGBA bytes; casting those to uint32 is wrong (R/B swap on LE).
// Returns 0 on success, -1 on failure.
int load_image(const char *path, uint32_t **out_pixels, int *out_w, int *out_h)
{
	int w, h, channels, n, i;
	unsigned char *rgba;
	uint32_t *out;

	rgba = stbi_load(path, &w, &h, &channels, 4);
	if (!rgba) {
		fprintf(stderr, "Failed to load image: %s\n", path);
		return -1;
	}
	n = w * h;
	out = malloc((size_t)n * sizeof(uint32_t));
	if (!out) {
		stbi_image_free(rgba);
		return -1;
	}
	for (i = 0; i < n; i++) {
		unsigned char *p = rgba + (size_t)i * 4;
		/* p[0]=R p[1]=G p[2]=B p[3]=A → same packing as BGCE rgba_to_u32 */
		out[i] = pack_argb(p[3], p[0], p[1], p[2]);
	}
	stbi_image_free(rgba);
	*out_pixels = out;
	*out_w = w;
	*out_h = h;
	return 0;
}

void clear_buffer(struct BGTK_Context *ctx)
{
	uint32_t *pixels = (uint32_t *) ctx->shm_buffer;
	size_t size = (size_t)ctx->width * ctx->height;
	for (size_t i = 0; i < size; i++) {
		uint32_t bg = ctx->theme.background;
		pixels[i] = (0xFFu << 24) | (bg & 0x00FFFFFF);  // force full alpha
	}
}

void draw_rect(struct BGTK_Context *ctx, uint32_t *pixels, int x, int y, int w,
	       int h, uint32_t color)
{
	int x1 = x;
	int y1 = y;
	int x2 = x + w;
	int y2 = y + h;
	int stride;
	uint32_t c;

	if (!ctx || !pixels || w <= 0 || h <= 0)
		return;
	/* Clip to buffer — unclipped writes into BGCE mmap shm → SIGBUS. */
	if (x1 < 0)
		x1 = 0;
	if (y1 < 0)
		y1 = 0;
	if (x2 > ctx->width)
		x2 = ctx->width;
	if (y2 > ctx->height)
		y2 = ctx->height;
	if (x1 >= x2 || y1 >= y2)
		return;

	stride = ctx->width;
	c = (0xFFu << 24) | (color & 0x00FFFFFF);
	for (int j = y1; j < y2; j++) {
		for (int i = x1; i < x2; i++) {
			pixels[j * stride + i] = c;
		}
	}
}

void measure_text(FT_Face face, const char *text, int *out_width,
		  int *out_height)
{
	measure_text_style(face, text, 0, out_width, out_height);
}

void measure_text_style(FT_Face face, const char *text, int style,
			int *out_width, int *out_height)
{
	int width = 0;
	int ncp = 0;
	const char *p;

	if (!text) {
		if (out_width)
			*out_width = 0;
		if (out_height)
			*out_height = 12;
		return;
	}
	{
		size_t left = strlen(text);

		if (!face) {
			/* Crude: count codepoints for fallback width. */
			p = text;
			while (left > 0) {
				if (!bgtk_utf8_next_n(&p, &left))
					break;
				ncp++;
			}
			if (out_width)
				*out_width = ncp * 7 +
					     ((style & BGTK_TEXT_BOLD) ? ncp : 0);
			if (out_height)
				*out_height = 12;
			return;
		}

		p = text;
		while (left > 0) {
			uint32_t cp = bgtk_utf8_next_n(&p, &left);

			if (!cp)
				break;
			if (load_cp(face, cp, style) == 0)
				width += face->glyph->advance.x;
			ncp++;
		}
	}
	width >>= 6;
	/* Synthetic italic shear needs a little end padding. */
	if (style & BGTK_TEXT_ITALIC)
		width += (face->size->metrics.height >> 6) / 4;

	if (out_width)
		*out_width = width;
	if (out_height) {
		int ascent = face->size->metrics.ascender >> 6;
		int descent = -face->size->metrics.descender >> 6;
		*out_height = ascent + descent;
	}
}

int measure_text_prefix(FT_Face face, const char *text, int nbytes)
{
	int width = 0;
	const char *p;
	size_t left;

	if (!text || nbytes <= 0)
		return 0;
	if (!face)
		return nbytes * 7;

	p = text;
	left = (size_t)nbytes;
	while (left > 0 && *p) {
		uint32_t cp = bgtk_utf8_next_n(&p, &left);
		if (!cp)
			break;
		if (FT_Load_Char(face, (FT_ULong)cp,
				 FT_LOAD_DEFAULT | FT_LOAD_NO_BITMAP) == 0)
			width += face->glyph->advance.x;
	}
	return width >> 6;
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
			int nw = w->data.label.text->w +
			    2 * (w->padding + w->margin);
			int nh = w->data.label.text->h +
			    2 * (w->padding + w->margin);
			// Keep a larger pre-set width (for text_align room).
			if (w->w < nw)
				w->w = nw;
			w->h = nh;
		}
		break;
	case BGTK_WIDGET_TEXT:
		if (w->data.text.text) {
			int tw = 0, th = 0;
			int st = w->data.text.style;
			if (w->data.text.header_level > 0 &&
			    w->data.text.header_level <= 3)
				st |= BGTK_TEXT_BOLD;
			measure_text_style(ctx->ft_face, w->data.text.text, st,
					   &tw, &th);
			int nw = tw + 2 * (w->padding + w->margin);
			int nh = th + 2 * (w->padding + w->margin);
			if (w->w < nw)
				w->w = nw;
			w->h = nh;
		}
		break;
	case BGTK_WIDGET_RULE: {
		int t = w->data.rule.thickness;
		if (t < 1)
			t = 1;
		if (w->data.rule.orientation == BGTK_LIST_VERTICAL) {
			int nw = t + 2 * (w->padding + w->margin);
			if (w->w < nw)
				w->w = nw;
			/* Height usually set by parent; keep a minimal stub. */
			if (w->h < t + 2 * (w->padding + w->margin))
				w->h = t + 2 * (w->padding + w->margin);
		} else {
			int nh = t + 2 * (w->padding + w->margin);
			if (w->h < nh)
				w->h = nh;
			if (w->w < t + 2 * (w->padding + w->margin))
				w->w = t + 2 * (w->padding + w->margin);
		}
		break;
	}
	case BGTK_WIDGET_BUTTON:
		if (w->data.button.label) {
			int border_w;
			calculate_widget_size(ctx, w->data.button.label);
			/* Per-button border_w (-1 = theme); 0 for list rows. */
			border_w = w->data.button.border_w >= 0
					   ? w->data.button.border_w
					   : (int)ctx->theme.button_border_size;
			if (border_w < 0)
				border_w = 0;
			int nw = w->data.button.label->w +
			    2 * (w->margin + w->padding + border_w);
			int nh = w->data.button.label->h +
			    2 * (w->margin + w->padding + border_w);
			if (w->w < nw)
				w->w = nw;
			w->h = nh;
		}
		break;

	case BGTK_WIDGET_SCROLLABLE: {
		/* Must match draw_scrollable layout exactly:
		 *   child.y = current_y + margin + padding
		 *   current_y += h + 2*margin
		 *   content_height = cy + 2*(margin+padding)
		 * Underestimating height caused OOB writes into the
		 * offscreen buffer (heap corruption / SIGBUS on shm). */
		int cy = 0;
		w->data.scrollable.content_height = 0;
		for (int i = 0; i < w->data.scrollable.widget_count; i++) {
			struct BGTK_Widget *child = w->data.scrollable.items[i];
			if (!child)
				continue;
			calculate_widget_size(ctx, child);
			cy += child->h + 2 * w->margin;
		}
		w->data.scrollable.content_height =
			cy + 2 * (w->margin + w->padding);
		break;
	}
	case BGTK_WIDGET_LIST: {
		int max_width = 0;
		int max_height = 0;
		int n = w->data.list_widget.widget_count;
		int keep_w = w->w;
		int keep_h = w->h;

		w->data.list_widget.content_width = 0;
		w->data.list_widget.content_height = 0;
		for (int i = 0; i < n; i++) {
			struct BGTK_Widget *child =
			    w->data.list_widget.items[i];
			if (!child)
				continue;
			calculate_widget_size(ctx, child);
			if (w->data.list_widget.orientation ==
			    BGTK_LIST_VERTICAL) {
				w->data.list_widget.content_height +=
				    child->h + 2 * w->margin;
				if (child->w > max_width)
					max_width = child->w;
			} else {
				w->data.list_widget.content_width +=
				    child->w + 2 * w->margin;
				if (child->h > max_height)
					max_height = child->h;
			}
		}
		if (n > 0) {
			if (w->data.list_widget.orientation ==
			    BGTK_LIST_VERTICAL)
				w->data.list_widget.content_height -=
				    2 * w->margin;
			else
				w->data.list_widget.content_width -=
				    2 * w->margin;
		}
		/* Outer size: content + padding; margin is inter-item gap only.
		 * Keep a larger pre-set size so EXPAND children have free space. */
		if (w->data.list_widget.orientation == BGTK_LIST_VERTICAL) {
			w->data.list_widget.content_width = max_width;
			w->w = max_width + 2 * w->padding;
			w->h = w->data.list_widget.content_height +
			       2 * w->padding;
		} else {
			w->data.list_widget.content_height = max_height;
			w->h = max_height + 2 * w->padding;
			w->w = w->data.list_widget.content_width +
			       2 * w->padding;
		}
		if (keep_w > w->w)
			w->w = keep_w;
		if (keep_h > w->h)
			w->h = keep_h;
		break;
	}
	case BGTK_WIDGET_IMAGE:
	case BGTK_WIDGET_TEXT_INPUT:
	case BGTK_WIDGET_SWITCH:
	case BGTK_WIDGET_SPACER:
		// Fixed-size widgets; size set by constructor (expand may grow).
		break;
	case BGTK_WIDGET_FRAME:
		/* Must recurse: root is usually a frame; without this,
		 * nested scrollables never get content_height and cannot
		 * scroll (and their offscreen buffer is sized wrong). */
		if (w->data.frame.child)
			calculate_widget_size(ctx, w->data.frame.child);
		break;
	default:
		break;
	}
}

void draw_text(struct BGTK_Context *ctx, uint32_t *pixels, const char *text,
	       int x, int y, uint32_t color)
{
	draw_text_style(ctx, pixels, text, x, y, color, 0);
}

void draw_text_style(struct BGTK_Context *ctx, uint32_t *pixels, const char *text,
		     int x, int y, uint32_t color, int style)
{
	draw_text_style_ex(ctx, pixels, text, x, y, color, style, 0);
}

void draw_text_style_ex(struct BGTK_Context *ctx, uint32_t *pixels,
			const char *text, int x, int y, uint32_t color,
			int style, int baseline_offset)
{
	int pen_x, pen_y, stride;
	FT_Matrix italic;
	size_t left;
	const char *p;

	if (!text || !*text)
		return;
	if (!ctx->ft_face) {
		draw_rect(ctx, pixels, x, y, 5, 5, color);
		return;
	}
	FT_Set_Pixel_Sizes(ctx->ft_face, 0,
			   ctx->font_size > 0 ? ctx->font_size : 14);

	pen_x = x;
	/* y is top of the text box; FreeType pen is baseline. */
	pen_y = y + (ctx->ft_face->size->metrics.ascender >> 6) +
		baseline_offset;
	if (ctx->theme.text_baseline_offset)
		pen_y += ctx->theme.text_baseline_offset;
	stride = ctx->width;
	/* ~12° synthetic italic shear (16.16 fixed). */
	italic = (FT_Matrix){ .xx = 0x10000, .xy = 0x5000, .yx = 0, .yy = 0x10000 };

	left = strlen(text);
	p = text;

	while (left > 0) {
		uint32_t cp = bgtk_utf8_next_n(&p, &left);
		FT_UInt index;
		FT_GlyphSlot slot;
		FT_Bitmap *bitmap;
		int gx, gy;
		unsigned int row, col;

		if (!cp)
			break;
		index = FT_Get_Char_Index(ctx->ft_face, (FT_ULong)cp);
		if (FT_Load_Glyph(ctx->ft_face, index,
				  FT_LOAD_DEFAULT | FT_LOAD_NO_BITMAP |
					  FT_LOAD_TARGET_LIGHT))
			continue;

		if ((style & BGTK_TEXT_BOLD) &&
		    ctx->ft_face->glyph->format == FT_GLYPH_FORMAT_OUTLINE)
			FT_GlyphSlot_Embolden(ctx->ft_face->glyph);
		if ((style & BGTK_TEXT_ITALIC) &&
		    ctx->ft_face->glyph->format == FT_GLYPH_FORMAT_OUTLINE)
			FT_Outline_Transform(&ctx->ft_face->glyph->outline,
					     &italic);

		if (FT_Render_Glyph(ctx->ft_face->glyph, FT_RENDER_MODE_NORMAL))
			continue;

		slot = ctx->ft_face->glyph;
		bitmap = &slot->bitmap;
		gx = pen_x + slot->bitmap_left;
		gy = pen_y - slot->bitmap_top;

		for (row = 0; row < bitmap->rows; row++) {
			for (col = 0; col < bitmap->width; col++) {
				uint8_t a =
				    bitmap->buffer[row * bitmap->pitch + col];
				int32_t dx, dy;
				uint32_t dst;
				uint8_t inv, r_dst, g_dst, b_dst;
				uint8_t r_src, g_src, b_src, r, g, b;

				if (a == 0)
					continue;
				dx = gx + (int)col;
				dy = gy + (int)row;
				if (dx < 0 || dy < 0 || dx >= ctx->width ||
				    dy >= ctx->height)
					continue;
				dst = pixels[dy * stride + dx];
				inv = 255 - a;
				r_dst = (dst >> 16) & 0xFF;
				g_dst = (dst >> 8) & 0xFF;
				b_dst = (dst) & 0xFF;
				r_src = (color >> 16) & 0xFF;
				g_src = (color >> 8) & 0xFF;
				b_src = (color) & 0xFF;
				r = (r_src * a + r_dst * inv) / 255;
				g = (g_src * a + g_dst * inv) / 255;
				b = (b_src * a + b_dst * inv) / 255;
				pixels[dy * stride + dx] =
				    (0xFFu << 24) | (r << 16) | (g << 8) | b;
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

// Compute horizontal offset for text_align within a content box of width
// content_w given measured text width text_w.
static int text_align_offset(enum BGTK_Text_Align align, int content_w,
			     int text_w)
{
	if (content_w <= text_w)
		return 0;
	if (align == BGTK_ALIGN_CENTER)
		return (content_w - text_w) / 2;
	if (align == BGTK_ALIGN_RIGHT)
		return content_w - text_w;
	return 0;  // BGTK_ALIGN_LEFT
}

static void draw_label(struct BGTK_Context *ctx, struct BGTK_Widget *w,
		       uint32_t *pixels)
{
	// Draw label background
	draw_rect(ctx, pixels, w->x + w->margin, w->y + w->margin,
		  w->w - 2 * w->margin, w->h - 2 * w->margin,
		  ctx->theme.background);
	// Draw text widget (offset for padding and margin + alignment)
	if (w->data.label.text) {
		int content_x0 = w->x + w->margin + w->padding;
		int content_w = w->w - 2 * (w->margin + w->padding);
		int ax = text_align_offset(w->text_align, content_w,
					   w->data.label.text->w);
		w->data.label.text->x = content_x0 + ax;
		w->data.label.text->y = w->y + w->margin + w->padding;
		draw_widget(ctx, w->data.label.text, pixels);
	}
}

static void draw_text_widget(struct BGTK_Context *ctx, struct BGTK_Widget *w,
			     uint32_t *pixels)
{
	int level = w->data.text.header_level;
	uint32_t color = ctx->theme.button_text;
	int old_size = ctx->font_size;
	int style = w->data.text.style;
	bool is_header = (level > 0 && level <= 3);
	bool is_accent = (level == 10);
	uint32_t accent = ctx->theme.highlight ? ctx->theme.highlight
					      : BGTK_COLOR_FUCHSIA;

	if (is_header) {
		color = accent;
		style |= BGTK_TEXT_BOLD;
		ctx->font_size = ctx->font_size + (4 - level);
	} else if (is_accent) {
		color = accent;
	}

	int content_x0 = w->x + w->margin + w->padding;
	int content_y0 = w->y + w->margin + w->padding;
	int content_w = w->w - 2 * (w->margin + w->padding);
	int content_h = w->h - 2 * (w->margin + w->padding);
	int tw = 0, th = 0;
	measure_text_style(ctx->ft_face, w->data.text.text, style, &tw, &th);
	int ax = text_align_offset(w->text_align, content_w, tw);
	int tx = content_x0 + ax;
	int ty = content_y0;
	if (content_h > th) {
		if (w->text_v_align == BGTK_VALIGN_CENTER)
			ty += (content_h - th) / 2;
		else if (w->text_v_align == BGTK_VALIGN_BOTTOM)
			ty += content_h - th;
	}

	draw_text_style_ex(ctx, pixels, w->data.text.text, tx, ty, color, style,
			   w->baseline_offset);
	if (is_header)
		ctx->font_size = old_size;
}

static void draw_button(struct BGTK_Context *ctx, struct BGTK_Widget *w,
			uint32_t *pixels)
{
	uint32_t bg = w->data.button.bg_override ? w->data.button.bg_override : ctx->theme.button;
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

	/* Border: per-button override (-1 = theme). */
	int bw = w->data.button.border_w >= 0
			 ? w->data.button.border_w
			 : (int)ctx->theme.button_border_size;
	if (bw < 0)
		bw = 0;

	if (bw > 0) {
		uint32_t border = ctx->theme.button_text;
		draw_rect(ctx, pixels, w->x + w->margin, w->y + w->margin,
			  w->w - 2 * w->margin, bw, border);
		draw_rect(ctx, pixels, w->x + w->margin,
			  w->y + w->h - bw - w->margin, w->w - 2 * w->margin, bw,
			  border);
		draw_rect(ctx, pixels, w->x + w->margin, w->y + w->margin, bw,
			  w->h - 2 * w->margin, border);
		draw_rect(ctx, pixels, w->x + w->w - bw - w->margin,
			  w->y + w->margin, bw, w->h - 2 * w->margin, border);
	}

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

		// Horizontal: respect text_align; vertical: always center
		int lx = content_x0 +
		    text_align_offset(w->text_align, content_w,
				      w->data.button.label->w) + off;
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
	int current_y = 0;
	int i;
	size_t need;
	struct BGTK_Context tmp_ctx;
	uint32_t *buff;
	uint32_t *tmp;
	int dst_x0, dst_y0, copy_w, rows;

	if (w->w < 1 || w->h < 1)
		return;
	if (content_height < w->h)
		content_height = w->h;
	/* Cap insane heights so (int)pixel count cannot wrap. */
	if (content_height > 100000)
		content_height = 100000;

	/* Reallocate offscreen buffer when page content or width changes. */
	need = (size_t)w->w * (size_t)content_height;
	if (need == 0)
		return;
	if (w->data.scrollable.tmp &&
	    (size_t)w->data.scrollable.widget_capacity != need) {
		free(w->data.scrollable.tmp);
		w->data.scrollable.tmp = NULL;
		w->data.scrollable.widget_capacity = 0;
	}
	if (!w->data.scrollable.tmp) {
		w->data.scrollable.tmp = calloc(need, sizeof(uint32_t));
		if (!w->data.scrollable.tmp) {
			fprintf(stderr,
				"Failed to allocate off-screen buffer (%dx%d)\n",
				w->w, content_height);
			return;
		}
		/* Pixel count (fits: w*h capped above). */
		w->data.scrollable.widget_capacity = (int)need;
	}

	tmp_ctx = *ctx;
	tmp_ctx.width = w->w;
	tmp_ctx.height = content_height;
	tmp_ctx.shm_buffer = w->data.scrollable.tmp;

	draw_rect(&tmp_ctx, w->data.scrollable.tmp, 0, 0, w->w, content_height,
		  ctx->theme.background);

	current_y = 0;
	for (i = 0; i < w->data.scrollable.widget_count; i++) {
		struct BGTK_Widget *child = w->data.scrollable.items[i];
		int bottom;
		int inner_w = w->w - 2 * (w->margin + w->padding);

		if (!child)
			continue;
		/* EXPAND_X: fill content width (scroll body). */
		if ((child->flags & BGTK_FLAG_EXPAND_X) && inner_w > 0)
			child->w = inner_w;
		child->x = w->margin + w->padding;
		if (w->flags & BGTK_FLAG_CENTER) {
			child->x =
			    w->margin + (w->w - 2 * w->margin - child->w) / 2;
		}
		/* Padding insets content on all sides (was X-only — Y ignored). */
		child->y = current_y + w->margin + w->padding;
		/* Content-space coords (events transform into this space). */
		child->abs_x = child->x;
		child->abs_y = child->y;
		child->parent = w;
		bottom = child->y + child->h;
		/* Skip draw if entirely past buffer (defensive). */
		if (child->y < content_height && bottom > 0)
			draw_widget(&tmp_ctx, child, w->data.scrollable.tmp);
		current_y += child->h + 2 * w->margin;
	}

	/* Blit visible rows into the window buffer — clip to shm bounds.
	 * Writing past mmap'd BGCE shm is a common SIGBUS. */
	buff = (uint32_t *)ctx->shm_buffer;
	tmp = w->data.scrollable.tmp;
	if (!buff || !tmp)
		return;
	dst_x0 = w->x;
	dst_y0 = w->y;
	copy_w = w->w;
	if (dst_x0 < 0) {
		copy_w += dst_x0;
		dst_x0 = 0;
	}
	if (dst_x0 + copy_w > ctx->width)
		copy_w = ctx->width - dst_x0;
	if (copy_w <= 0)
		return;
	rows = w->h;
	if (dst_y0 < 0) {
		rows += dst_y0;
		dst_y0 = 0;
	}
	if (dst_y0 + rows > ctx->height)
		rows = ctx->height - dst_y0;
	for (i = 0; i < rows; i++) {
		int src_row = w->data.scrollable.scroll_y + (i + (dst_y0 - w->y));
		int src_x = (dst_x0 > w->x) ? (dst_x0 - w->x) : 0;

		if (src_row < 0 || src_row >= content_height)
			continue;
		memcpy(&buff[(dst_y0 + i) * ctx->width + dst_x0],
		       &tmp[src_row * w->w + src_x],
		       (size_t)copy_w * sizeof(uint32_t));
	}
}

/* Grow EXPAND_* children into free space inside a list's content box. */
void bgtk_list_layout_expand(struct BGTK_Widget *w)
{
	int n, pad, gap, inner_w, inner_h, i, n_exp, fixed, free_sp, share, rem;
	int vert;

	if (!w || w->type != BGTK_WIDGET_LIST)
		return;
	n = w->data.list_widget.widget_count;
	pad = w->padding;
	gap = 2 * w->margin;
	inner_w = w->w - 2 * pad;
	inner_h = w->h - 2 * pad;
	n_exp = 0;
	fixed = 0;
	vert = (w->data.list_widget.orientation == BGTK_LIST_VERTICAL);

	if (inner_w < 0)
		inner_w = 0;
	if (inner_h < 0)
		inner_h = 0;

	if (vert) {
		for (i = 0; i < n; i++) {
			struct BGTK_Widget *c = w->data.list_widget.items[i];

			if (!c)
				continue;
			if (c->flags & BGTK_FLAG_EXPAND_X)
				c->w = inner_w;
			if (c->flags & BGTK_FLAG_EXPAND_Y)
				n_exp++;
			else
				fixed += c->h;
		}
		if (n > 1)
			fixed += (n - 1) * gap;
		free_sp = inner_h - fixed;
		if (n_exp > 0 && free_sp > 0) {
			share = free_sp / n_exp;
			rem = free_sp % n_exp;
			for (i = 0; i < n; i++) {
				struct BGTK_Widget *c =
					w->data.list_widget.items[i];
				if (c && (c->flags & BGTK_FLAG_EXPAND_Y)) {
					c->h = share + (rem > 0 ? 1 : 0);
					if (rem > 0)
						rem--;
				}
			}
		}
	} else {
		for (i = 0; i < n; i++) {
			struct BGTK_Widget *c = w->data.list_widget.items[i];

			if (!c)
				continue;
			if (c->flags & BGTK_FLAG_EXPAND_Y)
				c->h = inner_h;
			if (c->flags & BGTK_FLAG_EXPAND_X)
				n_exp++;
			else
				fixed += c->w;
		}
		if (n > 1)
			fixed += (n - 1) * gap;
		free_sp = inner_w - fixed;
		if (n_exp > 0 && free_sp > 0) {
			share = free_sp / n_exp;
			rem = free_sp % n_exp;
			for (i = 0; i < n; i++) {
				struct BGTK_Widget *c =
					w->data.list_widget.items[i];
				if (c && (c->flags & BGTK_FLAG_EXPAND_X)) {
					c->w = share + (rem > 0 ? 1 : 0);
					if (rem > 0)
						rem--;
				}
			}
		}
	}
}

/* Place a child: relative or absolute. ox/oy are parent content origin (draw space). */
static void place_child(struct BGTK_Widget *parent, struct BGTK_Widget *child,
			int ox, int oy, int rel_x, int rel_y)
{
	int use_rel = (parent->flags & BGTK_FLAG_RELATIVE) ||
		      (child->flags & BGTK_FLAG_RELATIVE);

	child->parent = parent;
	if (use_rel) {
		child->flags |= BGTK_FLAG_RELATIVE;
		child->x = rel_x;
		child->y = rel_y;
		child->abs_x = ox + rel_x;
		child->abs_y = oy + rel_y;
	} else {
		child->x = ox + rel_x;
		child->y = oy + rel_y;
		child->abs_x = child->x;
		child->abs_y = child->y;
	}
}

static void draw_list(struct BGTK_Context *ctx, struct BGTK_Widget *w,
		      uint32_t *pixels)
{
	int current_x = 0;
	int current_y = 0;
	int ox = w->x;
	int oy = w->y;
	int i, n = w->data.list_widget.widget_count;

	bgtk_list_layout_expand(w);

	for (i = 0; i < n; i++) {
		struct BGTK_Widget *child = w->data.list_widget.items[i];
		int rel_x, rel_y;

		if (!child)
			continue;

		if (w->data.list_widget.orientation == BGTK_LIST_VERTICAL) {
			/* Margin is row gap only — not a left inset. */
			rel_x = w->padding;
			if (w->flags & BGTK_FLAG_CENTER)
				rel_x = (w->w - child->w) / 2;
			rel_y = w->padding + current_y;
			place_child(w, child, ox, oy, rel_x, rel_y);
			current_y += child->h + 2 * w->margin;
		} else {
			rel_x = w->padding + current_x;
			rel_y = w->padding;
			if (w->flags & BGTK_FLAG_CENTER)
				rel_y = (w->h - child->h) / 2;
			place_child(w, child, ox, oy, rel_x, rel_y);
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

	/* border_w 0 = no border (used for HTML cells / borderless panels). */
	int bw = w->data.frame.border_w;
	if (bw < 0)
		bw = 0;

	if (bw > 0) {
		uint32_t border =
			w->data.frame.border_color ? w->data.frame.border_color
						  : ctx->theme.frame_border_color;
		if (!ctx->window_focused) {
			/* Dedicated unfocused chrome; 0 falls back to bg. */
			border = ctx->theme.frame_border_unfocused
					 ? ctx->theme.frame_border_unfocused
					 : ctx->theme.background;
		}
		draw_rect(ctx, pixels, w->x + w->margin, w->y + w->margin,
			  w->w - 2 * w->margin, bw, border);
		draw_rect(ctx, pixels, w->x + w->margin,
			  w->y + w->h - w->margin - bw, w->w - 2 * w->margin, bw,
			  border);
		draw_rect(ctx, pixels, w->x + w->margin, w->y + w->margin, bw,
			  w->h - 2 * w->margin, border);
		draw_rect(ctx, pixels, w->x + w->w - w->margin - bw,
			  w->y + w->margin, bw, w->h - 2 * w->margin, border);
	}

	// Draw child widget inside the frame
	if (w->data.frame.child) {
		struct BGTK_Widget *ch = w->data.frame.child;
		int rel_x = w->margin + bw + w->padding;
		int rel_y = w->margin + bw + w->padding;
		int cw = w->w - 2 * (w->margin + bw + w->padding);
		int chh = w->h - 2 * (w->margin + bw + w->padding);

		/* Default: fill frame content box. EXPAND flags same; without
		 * FILL a fixed child still gets the box (historical behavior). */
		if (cw > 0)
			ch->w = cw;
		if (chh > 0)
			ch->h = chh;
		place_child(w, ch, w->x, w->y, rel_x, rel_y);
		draw_widget(ctx, ch, pixels);
	}
}

static void draw_text_input(struct BGTK_Context *ctx, struct BGTK_Widget *w,
			    uint32_t *pixels)
{
	int focused = ctx->focused_widget == w;
	uint32_t focus = ctx->theme.focus ? ctx->theme.focus : 0xFF0066FF;
	uint32_t focus_bg =
		ctx->theme.focus_bg ? ctx->theme.focus_bg : 0xFFE8F2FF;
	uint32_t input_bg =
		ctx->theme.input_bg ? ctx->theme.input_bg : 0xFFFFFFFF;
	uint32_t text_color = ctx->theme.button_text ?
		ctx->theme.button_text : 0xFF111111;
	uint32_t field_bg;
	uint32_t border;
	int bw;

	/* Per-widget border (-1 = theme; 0 = borderless / minimal field). */
	bw = w->data.text_input.border_w >= 0
		     ? w->data.text_input.border_w
		     : (int)ctx->theme.input_border_size;
	if (bw < 0)
		bw = 0;
	/* Focused fields get a thicker ring only when a border is drawn. */
	if (focused && bw > 0 && bw < 3)
		bw = 3;
	if (bw * 2 > w->w - 2 * w->margin)
		bw = (w->w - 2 * w->margin) / 2;
	if (bw * 2 > w->h - 2 * w->margin)
		bw = (w->h - 2 * w->margin) / 2;
	if (bw < 0)
		bw = 0;

	/* Borderless: blend into window bg so only text + caret show. */
	if (bw == 0)
		field_bg = ctx->theme.background ? ctx->theme.background
						 : 0xFF0A0A0A;
	else
		field_bg = focused ? focus_bg : input_bg;
	border = focused ? focus : 0xFF888888;

	draw_rect(ctx, pixels, w->x + w->margin, w->y + w->margin,
		  w->w - 2 * w->margin, w->h - 2 * w->margin, field_bg);

	if (bw > 0) {
		draw_rect(ctx, pixels, w->x + w->margin, w->y + w->margin,
			  w->w - 2 * w->margin, bw, border);
		draw_rect(ctx, pixels, w->x + w->margin,
			  w->y + w->h - bw - w->margin, w->w - 2 * w->margin, bw,
			  border);
		draw_rect(ctx, pixels, w->x + w->margin, w->y + w->margin, bw,
			  w->h - 2 * w->margin, border);
		draw_rect(ctx, pixels, w->x + w->w - bw - w->margin,
			  w->y + w->margin, bw, w->h - 2 * w->margin, border);
	}

	int inner_x0 = w->x + w->margin + bw;
	int inner_y0 = w->y + w->margin + bw;
	int inner_w = w->w - 2 * w->margin - 2 * bw;
	int inner_h = w->h - 2 * w->margin - 2 * bw;
	if (inner_w < 1)
		inner_w = 1;
	if (inner_h < 1)
		inner_h = 1;

	int text_x = w->x + w->margin + bw + w->padding;
	int text_y = w->y + w->margin + bw + w->padding;
	int scroll_x = w->data.text_input.scroll_x;
	if (scroll_x < 0)
		scroll_x = 0;

	const char *full =
	    w->data.text_input.text ? w->data.text_input.text : "";

	int content_w = inner_w - 2 * w->padding;
	if (content_w < 1)
		content_w = 1;
	int content_h = inner_h - 2 * w->padding;
	if (content_h < 1)
		content_h = 1;
	int tw = 0, th = 0;
	measure_text(ctx->ft_face, full, &tw, &th);
	int align_off = 0;
	if (scroll_x == 0 && tw < content_w)
		align_off = text_align_offset(w->text_align, content_w, tw);
	/* Vertically center glyphs in the field (baseline was top-heavy). */
	if (th > 0 && content_h > th)
		text_y += (content_h - th) / 2;
	text_y += w->baseline_offset;

	int draw_x = text_x - scroll_x + align_off;

	if (!ctx->ft_face) {
		/* No font: still mark presence of text with a dark bar so the
		 * field is not a blank rectangle when typing without a face. */
		if (full[0])
			draw_rect(ctx, pixels, draw_x, text_y,
				  tw > 0 ? tw : (int)strlen(full) * 7, 3,
				  text_color);
	} else {
		int stride = ctx->width;
		FT_Set_Pixel_Sizes(ctx->ft_face, 0, ctx->font_size);
		int pen_x = draw_x;
		int pen_y = text_y + (ctx->ft_face->size->metrics.ascender >> 6);
		if (ctx->theme.text_baseline_offset)
			pen_y += ctx->theme.text_baseline_offset;
		uint8_t r_src = (text_color >> 16) & 0xFF;
		uint8_t g_src = (text_color >> 8) & 0xFF;
		uint8_t b_src = text_color & 0xFF;

		{
		size_t left = strlen(full);
		const char *p = full;

		while (left > 0) {
			uint32_t cp = bgtk_utf8_next_n(&p, &left);
			FT_UInt index;
			FT_GlyphSlot slot;
			FT_Bitmap *bitmap;
			int gx, gy;

			if (!cp)
				break;
			index = FT_Get_Char_Index(ctx->ft_face, (FT_ULong)cp);
			if (FT_Load_Glyph(ctx->ft_face, index,
					  FT_LOAD_DEFAULT | FT_LOAD_TARGET_LIGHT))
				continue;
			FT_Render_Glyph(ctx->ft_face->glyph, FT_RENDER_MODE_NORMAL);

			slot = ctx->ft_face->glyph;
			bitmap = &slot->bitmap;
			gx = pen_x + slot->bitmap_left;
			gy = pen_y - slot->bitmap_top;

			if (gx + (int)bitmap->width <= inner_x0) {
				pen_x += slot->advance.x >> 6;
				continue;
			}
			if (gx >= inner_x0 + inner_w)
				break;

			for (unsigned int row = 0; row < bitmap->rows; row++) {
				int32_t dy = gy + (int)row;
				if (dy < inner_y0 || dy >= inner_y0 + inner_h)
					continue;
				for (unsigned int col = 0; col < bitmap->width; col++) {
					int32_t dx = gx + (int)col;
					uint8_t a, inv, r_dst, g_dst, b_dst, r, g, b;
					uint32_t dst;

					if (dx < inner_x0 || dx >= inner_x0 + inner_w)
						continue;
					a = bitmap->buffer[row * bitmap->pitch + col];
					if (a == 0)
						continue;
					dst = pixels[dy * stride + dx];
					inv = 255 - a;
					r_dst = (dst >> 16) & 0xFF;
					g_dst = (dst >> 8) & 0xFF;
					b_dst = dst & 0xFF;
					r = (r_src * a + r_dst * inv) / 255;
					g = (g_src * a + g_dst * inv) / 255;
					b = (b_src * a + b_dst * inv) / 255;
					pixels[dy * stride + dx] =
					    (0xFFu << 24) | (r << 16) | (g << 8) | b;
				}
			}
			pen_x += slot->advance.x >> 6;
		}
		} /* UTF-8 walk */
	}

	if (focused) {
		int cursor_x = text_x - scroll_x + align_off;
		if (ctx->ft_face && w->data.text_input.text) {
			cursor_x += measure_text_prefix(
				ctx->ft_face, w->data.text_input.text,
				(int)w->data.text_input.cursor_pos);
		} else {
			cursor_x += (int)w->data.text_input.cursor_pos * 7;
		}

		if (cursor_x < inner_x0)
			cursor_x = inner_x0;
		if (cursor_x > inner_x0 + inner_w - 2)
			cursor_x = inner_x0 + inner_w - 2;

		draw_rect(ctx, pixels, cursor_x, inner_y0, 2, inner_h, focus);
	}
}

static void draw_rule(struct BGTK_Context *ctx, struct BGTK_Widget *w,
		      uint32_t *pixels)
{
	int t = w->data.rule.thickness;
	uint32_t color = w->data.rule.color;
	if (!color)
		color = ctx->theme.rule_color;
	if (!color)
		color = ctx->theme.button_text;
	if (!color)
		color = ctx->theme.frame_border_color;
	int x0 = w->x + w->margin + w->padding;
	int y0 = w->y + w->margin + w->padding;
	int iw = w->w - 2 * (w->margin + w->padding);
	int ih = w->h - 2 * (w->margin + w->padding);

	if (t < 1)
		t = 1;
	if (iw < 1 || ih < 1)
		return;
	if (w->data.rule.orientation == BGTK_LIST_VERTICAL) {
		if (t > iw)
			t = iw;
		draw_rect(ctx, pixels, x0 + (iw - t) / 2, y0, t, ih, color);
	} else {
		if (t > ih)
			t = ih;
		draw_rect(ctx, pixels, x0, y0 + (ih - t) / 2, iw, t, color);
	}
}

/*
 * Binary switch:  left_label  [●────]  right_label
 * Labels sit outside a slim track; knob slides left/right.
 */
static void draw_switch(struct BGTK_Context *ctx, struct BGTK_Widget *w,
			uint32_t *pixels)
{
	int x0 = w->x + w->margin + w->padding;
	int y0 = w->y + w->margin + w->padding;
	int iw = w->w - 2 * (w->margin + w->padding);
	int ih = w->h - 2 * (w->margin + w->padding);
	int fs = ctx->font_size > 0 ? ctx->font_size : 14;
	int kn = fs + 2;
	int val = w->data.switch_w.value ? 1 : 0;
	const char *L = w->data.switch_w.left ? w->data.switch_w.left : "";
	const char *R = w->data.switch_w.right ? w->data.switch_w.right : "";
	int lw = 0, lh = 0, rw = 0, rh = 0;
	uint32_t track = ctx->theme.button ? ctx->theme.button : 0xFF1C1814;
	uint32_t edge = ctx->theme.rule_color ? ctx->theme.rule_color
					     : (ctx->theme.button_text
							? ctx->theme.button_text
							: 0xFFF5E6D3);
	uint32_t kn_col = ctx->theme.highlight ? ctx->theme.highlight
					       : (ctx->theme.focus
							  ? ctx->theme.focus
							  : 0xFFE0A060);
	uint32_t dim = ctx->theme.button_text ? ctx->theme.button_text
					      : 0xFFF5E6D3;
	uint32_t active = kn_col;
	int gap = 8;
	int track_w, track_h, track_x, track_y, kn_x, kn_y, text_y, th;

	if (iw < 16 || ih < 8)
		return;
	if (kn > ih - 2)
		kn = ih - 2;
	if (kn < 10)
		kn = 10;

	if (ctx->ft_face) {
		FT_Set_Pixel_Sizes(ctx->ft_face, 0, fs);
		measure_text(ctx->ft_face, L, &lw, &lh);
		measure_text(ctx->ft_face, R, &rw, &rh);
	} else {
		lh = rh = fs;
		lw = (int)strlen(L) * (fs / 2 + 1);
		rw = (int)strlen(R) * (fs / 2 + 1);
	}
	th = lh > rh ? lh : rh;
	track_h = kn + 4;
	if (track_h > ih)
		track_h = ih;
	/* Pill ≈ 3× knob; matches bgtk_switch sizing. */
	track_w = kn * 3;
	if (track_w < kn + 20)
		track_w = kn + 20;
	/* Keep room for both labels. */
	if (lw + gap + track_w + gap + rw > iw)
		track_w = iw - lw - rw - gap * 2;
	if (track_w < kn + 10)
		track_w = kn + 10;

	track_x = x0 + lw + gap;
	track_y = y0 + (ih - track_h) / 2;
	text_y = y0 + (ih - th) / 2;
	if (text_y < y0)
		text_y = y0;

	/* Left / right labels */
	draw_text_style(ctx, pixels, L, x0, text_y,
			val == 0 ? active : dim,
			val == 0 ? BGTK_TEXT_BOLD : 0);
	draw_text_style(ctx, pixels, R, track_x + track_w + gap, text_y,
			val == 1 ? active : dim,
			val == 1 ? BGTK_TEXT_BOLD : 0);

	/* Track */
	draw_rect(ctx, pixels, track_x, track_y, track_w, track_h, track);
	draw_rect(ctx, pixels, track_x, track_y, track_w, 1, edge);
	draw_rect(ctx, pixels, track_x, track_y + track_h - 1, track_w, 1, edge);
	draw_rect(ctx, pixels, track_x, track_y, 1, track_h, edge);
	draw_rect(ctx, pixels, track_x + track_w - 1, track_y, 1, track_h, edge);

	/* Knob */
	if (val == 0)
		kn_x = track_x + 2;
	else
		kn_x = track_x + track_w - kn - 2;
	kn_y = track_y + (track_h - kn) / 2;
	draw_rect(ctx, pixels, kn_x, kn_y, kn, kn, kn_col);
	if (kn > 6) {
		draw_rect(ctx, pixels, kn_x + 1, kn_y + 1, kn - 2, 1, edge);
		draw_rect(ctx, pixels, kn_x + 1, kn_y + kn - 2, kn - 2, 1, edge);
		draw_rect(ctx, pixels, kn_x + 1, kn_y + 1, 1, kn - 2, edge);
		draw_rect(ctx, pixels, kn_x + kn - 2, kn_y + 1, 1, kn - 2, edge);
	}
}

void draw_widget(struct BGTK_Context *ctx, struct BGTK_Widget *w,
		 uint32_t *pixels)
{
	int saved_x = 0, saved_y = 0;
	int rel = w && (w->flags & BGTK_FLAG_RELATIVE);

	if (!w)
		return;
	/* Ensure abs is set for non-relative / root widgets. */
	if (!rel) {
		w->abs_x = w->x;
		w->abs_y = w->y;
	}
	/* Draw uses screen (or content-buffer) absolute coords. */
	if (rel) {
		saved_x = w->x;
		saved_y = w->y;
		w->x = w->abs_x;
		w->y = w->abs_y;
	}

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
	case BGTK_WIDGET_RULE:
		draw_rule(ctx, w, pixels);
		break;
	case BGTK_WIDGET_SWITCH:
		draw_switch(ctx, w, pixels);
		break;
	case BGTK_WIDGET_SPACER:
		break;
	default:
		puts("can't draw unknown widget");
		break;
	}

	if (rel) {
		w->x = saved_x;
		w->y = saved_y;
	}
}
