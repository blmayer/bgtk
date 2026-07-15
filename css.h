/* css.h — minimal CSS for BGTK HTML (Labyrinth and bgtk_html_parse*).
 *
 * Supported selectors (specificity ascending):
 *   *          type (p, h1, a, body, …)
 *   .class     #id
 *   type.class type#id
 *
 * Supported properties:
 *   color, background-color (or background: #hex)
 *   font-weight (bold/normal/700), font-style (italic/normal)
 *   text-align (left/center/right)
 *   margin, padding (px number or shorthand single value)
 *   display: none
 *
 * Cascade: stylesheet order, then inline style="…" wins.
 * Not yet: media queries, combinators, !important, units other than px.
 */
#ifndef BGTK_CSS_H
#define BGTK_CSS_H

#include "bgtk.h"
#include <stdint.h>

struct BGTK_CSS;

struct BGTK_CSS *bgtk_css_create(void);
void bgtk_css_destroy(struct BGTK_CSS *css);

/* Append rules from a stylesheet string (e.g. <style> body). */
void bgtk_css_add_sheet(struct BGTK_CSS *css, const char *source);

/*
 * Resolve styles for an element and apply to widget.
 * tag/id/class from HTML; style_attr is the raw style="" value (may be NULL).
 * Returns 1 if the element should be omitted (display:none).
 */
int bgtk_css_apply(struct BGTK_CSS *css, struct BGTK_Widget *w,
		   const char *tag, const char *id, const char *class_attr,
		   const char *style_attr);

/* Parse a single #rgb/#rrggbb/#rrggbbaa or named color → 0xAARRGGBB.
 * Returns 0 if unrecognised (caller treats as unset). */
uint32_t bgtk_css_parse_color(const char *s);

#endif
