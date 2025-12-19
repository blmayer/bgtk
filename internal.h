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
void measure_text(FT_Face face, const char* text, int* out_width,
		  int* out_height);
void calculate_widget_size(struct BGTK_Context* ctx, struct BGTK_Widget* w);
void draw_text(struct BGTK_Context* ctx, uint32_t* pixels, const char* text,
	       int x, int y, uint32_t color);
void draw_widget(struct BGTK_Context* ctx, struct BGTK_Widget* w,
		 uint32_t* pixels);
int load_image(const char* path, uint32_t** out_pixels, int* out_w, int* out_h);

// from widgets.c
struct BGTK_Widget* bgtk_label(struct BGTK_Context* ctx, char* text);
struct BGTK_Widget* bgtk_text(struct BGTK_Context* ctx, char* text, int flags);
struct BGTK_Widget* bgtk_button(struct BGTK_Context* ctx,
				struct BGTK_Widget* label,
				BGTK_Callback callback, int flags);
struct BGTK_Widget* bgtk_scrollable(struct BGTK_Context* ctx,
				    struct BGTK_Widget** widgets,
				    int widget_count, int flags);
#endif
