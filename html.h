#ifndef BGTK_HTML_H
#define BGTK_HTML_H

#include "bgtk.h"

/* Parse HTML → widget tree (libxml2). Applies CSS from <style> and style="".
 * See css.h for supported selectors/properties. Frame is sized width×height;
 * borderless; host UI supplies chrome. */
struct BGTK_Widget* bgtk_html_parse(struct BGTK_Context* ctx,
				    const char* path, int width, int height);

struct BGTK_Widget* bgtk_html_parse_inline(struct BGTK_Context* ctx,
					   const char* html, int width, int height);

#endif
