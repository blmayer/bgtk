#ifndef INTERNAL_H
#define INTERNAL_H

#include <ft2build.h>
#include <stddef.h>
#include <stdint.h>
#include FT_FREETYPE_H
#include <bgce.h>

// from drawing.c
void clear_buffer(struct BGTK_Context* ctx);
void draw_rect(struct BGTK_Context* ctx, uint32_t* pixels, int x, int y, int w,
	       int h, uint32_t color);

/* UTF-8: decode one codepoint from *s (NUL-terminated or bounded).
 * Advances *s past the sequence. Invalid bytes → U+FFFD and skip 1 byte.
 * Returns 0 only when *s is empty / n==0. */
uint32_t bgtk_utf8_next(const char **s);
/* Same for a byte-limited prefix (e.g. text input cursor). */
uint32_t bgtk_utf8_next_n(const char **s, size_t *nleft);

void measure_text(FT_Face face, const char* text, int* out_width,
		  int* out_height);
/* style: BGTK_TEXT_BOLD | BGTK_TEXT_ITALIC */
void measure_text_style(FT_Face face, const char *text, int style,
			int *out_width, int *out_height);
/* Pixel width of the first nbytes of text (UTF-8 aware; does not split chars). */
int measure_text_prefix(FT_Face face, const char *text, int nbytes);
void calculate_widget_size(struct BGTK_Context* ctx, struct BGTK_Widget* w);
void draw_text(struct BGTK_Context* ctx, uint32_t* pixels, const char* text,
	       int x, int y, uint32_t color);
void draw_text_style(struct BGTK_Context *ctx, uint32_t *pixels, const char *text,
		     int x, int y, uint32_t color, int style);
/* baseline_offset: extra pixels added to FreeType baseline (widget + theme). */
void draw_text_style_ex(struct BGTK_Context *ctx, uint32_t *pixels,
			const char *text, int x, int y, uint32_t color,
			int style, int baseline_offset);
void draw_widget(struct BGTK_Context* ctx, struct BGTK_Widget* w,
		 uint32_t* pixels);
int load_image(const char* path, uint32_t** out_pixels, int* out_w, int* out_h);
/* Decode image from memory (PNG/JPEG/…). Same pixel format as load_image. */
int load_image_mem(const unsigned char *data, int len, uint32_t **out_pixels,
		   int *out_w, int *out_h);
/* Nearest-neighbour scale ARGB buffer; frees src on success. */
uint32_t *scale_image_argb(uint32_t *src, int sw, int sh, int dw, int dh);

#endif
