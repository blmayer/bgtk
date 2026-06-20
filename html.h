#ifndef BGTK_HTML_H
#define BGTK_HTML_H

#include "bgtk.h"

// Parse an HTML file and return a frame widget containing the full widget tree.
// The frame is sized to (width x height). Caller sets ctx->root_widget to the result.
struct BGTK_Widget* bgtk_html_parse(struct BGTK_Context* ctx,
				    const char* path, int width, int height);

// Parse an HTML string directly (for inline / embedded use).
struct BGTK_Widget* bgtk_html_parse_inline(struct BGTK_Context* ctx,
					   const char* html, int width, int height);

#endif
