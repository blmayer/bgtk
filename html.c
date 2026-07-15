/*
 * html.c - HTML-to-BGTK widget tree parser.
 *
 * Uses libxml2 to parse HTML and maps common tags to real BGTK widgets
 * so users can describe their UI in standard HTML.
 *
 * Supported tags:
 *   h1..h6, p, b, i, a, span    -> BGTK_WIDGET_TEXT (with header_level / bold)
 *   pre, code                    -> mono text; pre keeps newlines/spaces
 *   br                           -> line break (block flush)
 *   ul, ol, li                   -> BGTK_WIDGET_LIST (vertical, with bullet/number)
 *   button                       -> BGTK_WIDGET_BUTTON
 *   input[type=text]             -> BGTK_WIDGET_TEXT_INPUT
 *   input[type=checkbox]         -> BGTK_WIDGET_BUTTON (toggle)
 *   select                       -> BGTK_WIDGET_LIST (dropdown placeholder)
 *   img                          -> BGTK_WIDGET_IMAGE
 *   div, section, article, body  -> BGTK_WIDGET_LIST (vertical container)
 *   header, nav, footer          -> BGTK_WIDGET_LIST (vertical container)
 *
 * Layout is strictly top-down (block elements stack vertically).
 * Inline elements within a block are collected into a horizontal list row.
 * <p> keeps mixed inlines so <a href> stays a separate clickable text widget.
 *
 * Attributes:  width, height, padding, margin  (in pixels)
 *              href on <a>                     -> text.href (owned string)
 *              onclick / onchange             -> callbacks are left NULL
 *              (the user wires them in code via the returned widget tree)
 */

#include "html.h"
#include "css.h"
#include "internal.h"

#include <ctype.h>
#include <libxml/HTMLparser.h>
#include <libxml/tree.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Active stylesheet while converting a document (NULL if none). */
static struct BGTK_CSS *g_html_css;

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

// Read an integer attribute (width, height, padding, margin) or return def.
static int attr_int(xmlNode *node, const char *name, int def)
{
	xmlChar *val = xmlGetProp(node, (const xmlChar *)name);
	if (!val)
		return def;
	int r = atoi((const char *)val);
	xmlFree(val);
	return r;
}

// Read a string attribute; caller must xmlFree the result.
static xmlChar *attr_str(xmlNode *node, const char *name)
{
	return xmlGetProp(node, (const xmlChar *)name);
}

// Collect all text content from a node (direct text + child text nodes).
static char *collect_text(xmlNode *node)
{
	xmlChar *raw = xmlNodeGetContent(node);
	if (!raw)
		return strdup("");

	// Trim leading/trailing whitespace.
	char *s = (char *)raw;
	while (*s && isspace((unsigned char)*s))
		s++;
	int len = (int)strlen(s);
	while (len > 0 && isspace((unsigned char)s[len - 1]))
		len--;

	char *out = strndup(s, len);
	xmlFree(raw);
	return out ? out : strdup("");
}

/* Preformatted: keep spaces/newlines; expand tabs; normalize CRLF → LF.
 * Strip a single leading/trailing newline (common HTML pretty-print). */
static char *collect_text_pre(xmlNode *node)
{
	xmlChar *raw;
	const char *s;
	size_t i, n, out_n = 0, out_cap;
	char *out;

	raw = xmlNodeGetContent(node);
	if (!raw)
		return strdup("");
	s = (const char *)raw;
	n = strlen(s);
	/* Drop one outer newline often left by <pre>\n...\n</pre>. */
	if (n > 0 && s[0] == '\n') {
		s++;
		n--;
	}
	if (n > 0 && s[n - 1] == '\n')
		n--;
	out_cap = n * 2 + 1;
	out = malloc(out_cap);
	if (!out) {
		xmlFree(raw);
		return strdup("");
	}
	for (i = 0; i < n; i++) {
		char c = s[i];

		if (c == '\r') {
			if (i + 1 < n && s[i + 1] == '\n')
				continue;
			c = '\n';
		}
		if (c == '\t') {
			int spaces = 8 - (int)(out_n % 8);

			while (spaces-- > 0) {
				if (out_n + 1 >= out_cap) {
					out_cap *= 2;
					out = realloc(out, out_cap);
					if (!out) {
						xmlFree(raw);
						return strdup("");
					}
				}
				out[out_n++] = ' ';
			}
			continue;
		}
		if (out_n + 1 >= out_cap) {
			out_cap *= 2;
			out = realloc(out, out_cap);
			if (!out) {
				xmlFree(raw);
				return strdup("");
			}
		}
		out[out_n++] = c;
	}
	out[out_n] = '\0';
	xmlFree(raw);
	return out;
}

/* Measure/create text with an alternate face (mono for pre/code). */
static struct BGTK_Widget *make_text_face(struct BGTK_Context *ctx,
					  const char *txt, BGTK_Options opts,
					  int font_role)
{
	FT_Face old, face;
	struct BGTK_Widget *w;

	if (!txt)
		return NULL;
	old = ctx ? ctx->ft_face : NULL;
	face = ctx ? bgtk_font_face(ctx, font_role) : NULL;
	if (face)
		ctx->ft_face = face;
	if (face && ctx)
		FT_Set_Pixel_Sizes(face, 0,
				   ctx->font_size > 0 ? ctx->font_size : 14);
	w = bgtk_text(ctx, (char *)txt, opts);
	if (ctx)
		ctx->ft_face = old;
	if (w)
		w->data.text.font_role = font_role;
	return w;
}

/* Theme spacing when ctx is available; fall back to hard defaults. */
static int theme_pad(struct BGTK_Context *ctx, int fallback)
{
	if (ctx && ctx->theme.padding > 0)
		return ctx->theme.padding;
	return fallback;
}

// Build an BGTK_Options from common HTML attributes on a node.
static BGTK_Options opts_from_node(xmlNode *node, int def_pad, int def_margin)
{
	return (BGTK_Options){
		.flags   = 0,
		.padding = attr_int(node, "padding", def_pad),
		.margin  = attr_int(node, "margin", def_margin),
	};
}

/* ------------------------------------------------------------------ */
/* Inline vs block classification                                      */
/* ------------------------------------------------------------------ */

static int is_inline_tag(const char *tag)
{
	if (!tag)
		return 0;
	return strcmp(tag, "b") == 0 || strcmp(tag, "i") == 0 ||
	       strcmp(tag, "a") == 0 || strcmp(tag, "span") == 0 ||
	       strcmp(tag, "strong") == 0 || strcmp(tag, "em") == 0 ||
	       strcmp(tag, "code") == 0;
}

/* ------------------------------------------------------------------ */
/* Forward declarations                                                */
/* ------------------------------------------------------------------ */

static struct BGTK_Widget *convert_node(struct BGTK_Context *ctx,
					xmlNode *node, int avail_w);
static struct BGTK_Widget *convert_container(struct BGTK_Context *ctx,
					     xmlNode *node, int avail_w);

/* Apply g_html_css + inline style to widget; return 1 if display:none. */
static int html_style_widget(xmlNode *node, struct BGTK_Widget *w)
{
	xmlChar *id = NULL, *cls = NULL, *sty = NULL;
	const char *tag = NULL;
	int hide;

	if (!node || node->type != XML_ELEMENT_NODE)
		return 0;
	tag = (const char *)node->name;
	id = xmlGetProp(node, (const xmlChar *)"id");
	cls = xmlGetProp(node, (const xmlChar *)"class");
	sty = xmlGetProp(node, (const xmlChar *)"style");
	hide = bgtk_css_apply(g_html_css, w, tag,
			      id ? (const char *)id : NULL,
			      cls ? (const char *)cls : NULL,
			      sty ? (const char *)sty : NULL);
	if (id)
		xmlFree(id);
	if (cls)
		xmlFree(cls);
	if (sty)
		xmlFree(sty);
	return hide;
}

/* Collect <style>…</style> text under root into css. */
static void html_collect_styles(xmlNode *node, struct BGTK_CSS *css)
{
	if (!node || !css)
		return;
	for (xmlNode *c = node; c; c = c->next) {
		if (c->type == XML_ELEMENT_NODE) {
			const char *tag = (const char *)c->name;
			if (strcmp(tag, "style") == 0) {
				xmlChar *txt = xmlNodeGetContent(c);
				if (txt) {
					bgtk_css_add_sheet(css, (const char *)txt);
					xmlFree(txt);
				}
				continue;
			}
			if (strcmp(tag, "script") == 0)
				continue;
			if (c->children)
				html_collect_styles(c->children, css);
		}
	}
}

/* ------------------------------------------------------------------ */
/* Inline text helpers                                                 */
/* ------------------------------------------------------------------ */

// Create a text widget for a raw text node (XML_TEXT_NODE).
// Keep spaces between inlines (e.g. </code> — text); only drop pure
// newline/indent whitespace used between block elements.
static struct BGTK_Widget *make_text_widget(struct BGTK_Context *ctx,
					    const char *raw)
{
	const char *s;
	int len, i, only_ws, has_space;
	char *txt;
	struct BGTK_Widget *w;

	if (!raw || !*raw)
		return NULL;
	only_ws = 1;
	has_space = 0;
	for (s = raw; *s; s++) {
		if (!isspace((unsigned char)*s)) {
			only_ws = 0;
			break;
		}
		if (*s == ' ' || *s == '\t')
			has_space = 1;
	}
	if (only_ws) {
		/* Inter-inline space from " </a> " etc.; ignore indent-only. */
		if (!has_space)
			return NULL;
		return bgtk_text(ctx, " ",
				 (BGTK_Options){.padding = 0, .margin = 0});
	}
	/* Keep content; strip only leading/trailing newlines (pretty-print). */
	s = raw;
	while (*s == '\n' || *s == '\r')
		s++;
	len = (int)strlen(s);
	while (len > 0 && (s[len - 1] == '\n' || s[len - 1] == '\r'))
		len--;
	/* Collapse internal newlines to spaces for inline text nodes. */
	txt = strndup(s, len);
	if (!txt)
		return NULL;
	for (i = 0; txt[i]; i++) {
		if (txt[i] == '\n' || txt[i] == '\r' || txt[i] == '\t')
			txt[i] = ' ';
	}
	w = bgtk_text(ctx, txt, (BGTK_Options){.padding = 0, .margin = 0});
	free(txt);
	return w;
}

/* ------------------------------------------------------------------ */
/* Tag converters                                                      */
/* ------------------------------------------------------------------ */

// h1..h6 -> text widget with header_level set.
static struct BGTK_Widget *convert_heading(struct BGTK_Context *ctx,
					   xmlNode *node, int level)
{
	char *txt = collect_text(node);
	if (!txt || !*txt) {
		free(txt);
		return NULL;
	}
	BGTK_Options opts = opts_from_node(node, 2, 4);
	struct BGTK_Widget *w = bgtk_text(ctx, txt, opts);
	free(txt);
	if (w)
		w->data.text.header_level = level;
	return w;
}

/* <p> keeps mixed inlines so <a href> stays its own widget. */
static struct BGTK_Widget *convert_p(struct BGTK_Context *ctx, xmlNode *node,
				     int avail_w)
{
	struct BGTK_Widget *w = convert_container(ctx, node, avail_w);
	int mar;

	if (!w)
		return NULL;
	mar = attr_int(node, "margin", 4);
	if (w->margin < mar)
		w->margin = mar;
	return w;
}

// <b>/<strong> -> text with BGTK_TEXT_BOLD.
static struct BGTK_Widget *convert_bold(struct BGTK_Context *ctx, xmlNode *node)
{
	char *txt = collect_text(node);
	if (!txt || !*txt) {
		free(txt);
		return NULL;
	}
	struct BGTK_Widget *w =
		bgtk_text(ctx, txt, (BGTK_Options){.text_style = BGTK_TEXT_BOLD});
	free(txt);
	return w;
}

// <i>/<em> -> synthetic italic.
static struct BGTK_Widget *convert_italic(struct BGTK_Context *ctx,
					  xmlNode *node)
{
	char *txt = collect_text(node);
	if (!txt || !*txt) {
		free(txt);
		return NULL;
	}
	struct BGTK_Widget *w = bgtk_text(
		ctx, txt, (BGTK_Options){.text_style = BGTK_TEXT_ITALIC});
	free(txt);
	return w;
}

// <a> -> accent (theme.highlight) via header_level 10; href on text.href.
static struct BGTK_Widget *convert_a(struct BGTK_Context *ctx, xmlNode *node)
{
	char *txt = collect_text(node);
	xmlChar *href;
	struct BGTK_Widget *w;

	if (!txt || !*txt) {
		free(txt);
		return NULL;
	}
	w = bgtk_text(ctx, txt, (BGTK_Options){0});
	free(txt);
	if (!w)
		return NULL;
	w->data.text.header_level = 10;
	href = attr_str(node, "href");
	if (href && href[0]) {
		w->data.text.href = strdup((const char *)href);
		xmlFree(href);
	} else if (href) {
		xmlFree(href);
	}
	return w;
}

/* <code> -> mono, inline (spaces kept via collect_text trim only). */
static struct BGTK_Widget *convert_code(struct BGTK_Context *ctx, xmlNode *node)
{
	char *txt = collect_text(node);
	struct BGTK_Widget *w;

	if (!txt || !*txt) {
		free(txt);
		return NULL;
	}
	w = make_text_face(ctx, txt, (BGTK_Options){0}, BGTK_FONT_MONO);
	free(txt);
	return w;
}

/* <pre> -> mono multi-line block; preserves whitespace. */
static struct BGTK_Widget *convert_pre(struct BGTK_Context *ctx, xmlNode *node)
{
	char *txt = collect_text_pre(node);
	struct BGTK_Widget *w;
	BGTK_Options opts;

	if (!txt) {
		return NULL;
	}
	/* Empty pre still reserves a line so layout does not collapse. */
	if (!txt[0]) {
		free(txt);
		txt = strdup(" ");
		if (!txt)
			return NULL;
	}
	opts = opts_from_node(node, 4, 4);
	w = make_text_face(ctx, txt, opts, BGTK_FONT_MONO);
	free(txt);
	if (w && w->color_bg == 0)
		/* Subtle panel so pre blocks read as code regions. */
		w->color_bg = 0xFF1A1A1A;
	return w;
}

/* <br> -> empty-ish line fragment (block, forces inline flush). */
static struct BGTK_Widget *convert_br(struct BGTK_Context *ctx)
{
	struct BGTK_Widget *w = bgtk_text(ctx, " ", (BGTK_Options){0});

	if (w) {
		/* Half-line break so stacked <br>s still advance. */
		if (w->h > 4)
			w->h = w->h / 2 + 2;
	}
	return w;
}

// <button> -> BGTK_WIDGET_BUTTON. Callback is NULL; user wires it later.
static struct BGTK_Widget *convert_button(struct BGTK_Context *ctx,
					  xmlNode *node)
{
	char *txt = collect_text(node);
	if (!txt || !*txt) {
		free(txt);
		txt = strdup("button");
	}
	BGTK_Options opts = opts_from_node(node, 6, 4);
	struct BGTK_Widget *label = bgtk_text(ctx, txt, (BGTK_Options){0});
	free(txt);
	if (!label)
		return NULL;
	return bgtk_button(ctx, label, NULL, NULL, opts);
}

// <input type="text"> -> BGTK_WIDGET_TEXT_INPUT.
static struct BGTK_Widget *convert_text_input(struct BGTK_Context *ctx,
					      xmlNode *node)
{
	xmlChar *val = attr_str(node, "value");
	char *initial = val ? (char *)val : "";
	int w = attr_int(node, "width", 200);
	int h = attr_int(node, "height", 0);
	BGTK_Options opts = opts_from_node(node, 4, 4);
	struct BGTK_Widget *ti = bgtk_text_input(ctx, initial, w, h, opts);
	if (val)
		xmlFree(val);
	return ti;
}

// <input type="checkbox"> -> toggle button (label = [ ] or [x]).
static struct BGTK_Widget *convert_checkbox(struct BGTK_Context *ctx,
					    xmlNode *node)
{
	xmlChar *checked = attr_str(node, "checked");
	const char *txt = checked ? "[x]" : "[ ]";
	if (checked)
		xmlFree(checked);
	BGTK_Options opts = opts_from_node(node, 4, 4);
	struct BGTK_Widget *label = bgtk_text(ctx, (char *)txt, (BGTK_Options){0});
	if (!label)
		return NULL;
	return bgtk_button(ctx, label, NULL, NULL, opts);
}

// <select> with <option> children -> vertical list of labels.
static struct BGTK_Widget *convert_select(struct BGTK_Context *ctx,
					  xmlNode *node)
{
	int cap = 8, count = 0;
	struct BGTK_Widget **items = calloc(cap, sizeof(struct BGTK_Widget *));
	if (!items)
		return NULL;

	for (xmlNode *child = node->children; child; child = child->next) {
		if (child->type != XML_ELEMENT_NODE)
			continue;
		if (strcmp((const char *)child->name, "option") != 0)
			continue;
		char *txt = collect_text(child);
		if (!txt || !*txt) {
			free(txt);
			continue;
		}
		struct BGTK_Widget *w = bgtk_text(ctx, txt, (BGTK_Options){.padding = 2, .margin = 1});
		free(txt);
		if (!w)
			continue;
		if (count >= cap) {
			cap *= 2;
			items = realloc(items, cap * sizeof(struct BGTK_Widget *));
		}
		items[count++] = w;
	}

	if (count == 0) {
		free(items);
		return NULL;
	}
	BGTK_Options opts = opts_from_node(node, 2, 4);
	opts.orientation = BGTK_LIST_VERTICAL;
	struct BGTK_Widget *list = bgtk_list(ctx, items, count, opts);
	free(items);
	return list;
}

// <img> -> BGTK_WIDGET_IMAGE.
static struct BGTK_Widget *convert_img(struct BGTK_Context *ctx, xmlNode *node)
{
	xmlChar *src = attr_str(node, "src");
	if (!src)
		return NULL;
	int w = attr_int(node, "width", 0);
	int h = attr_int(node, "height", 0);
	BGTK_Options opts = opts_from_node(node, 0, 4);
	struct BGTK_Widget *img = bgtk_image(ctx, (const char *)src, w, h, opts);
	xmlFree(src);
	return img;
}

// <input> dispatcher based on type attribute.
static struct BGTK_Widget *convert_input(struct BGTK_Context *ctx,
					 xmlNode *node)
{
	xmlChar *type = attr_str(node, "type");
	const char *t = type ? (const char *)type : "text";
	struct BGTK_Widget *w = NULL;

	if (strcmp(t, "checkbox") == 0)
		w = convert_checkbox(ctx, node);
	else
		w = convert_text_input(ctx, node);

	if (type)
		xmlFree(type);
	return w;
}

/* ------------------------------------------------------------------ */
/* Container / block-level converter                                   */
/* ------------------------------------------------------------------ */

// Convert children of a container node into an array of widgets.
// Inline siblings are grouped into horizontal list rows.
static int convert_children(struct BGTK_Context *ctx, xmlNode *parent,
			    int avail_w, struct BGTK_Widget ***out_items,
			    int *out_count)
{
	int cap = 16, count = 0;
	struct BGTK_Widget **items = calloc(cap, sizeof(struct BGTK_Widget *));
	if (!items)
		return -1;

	// Temporary buffer for grouping inline widgets.
	int inline_cap = 16, inline_count = 0;
	struct BGTK_Widget **inline_buf = calloc(inline_cap, sizeof(struct BGTK_Widget *));

	// Flush inline_buf into a single horizontal list and append to items.
	#define FLUSH_INLINE() do { \
		if (inline_count > 0) { \
			BGTK_Options hopts = {.orientation = BGTK_LIST_HORIZONTAL, .margin = 0}; \
			struct BGTK_Widget *row = bgtk_list(ctx, inline_buf, inline_count, hopts); \
			if (row) { \
				if (count >= cap) { cap *= 2; items = realloc(items, cap * sizeof(struct BGTK_Widget *)); } \
				items[count++] = row; \
			} \
			inline_count = 0; \
		} \
	} while (0)

	for (xmlNode *child = parent->children; child; child = child->next) {
		// Pure text node -> inline text widget.
		if (child->type == XML_TEXT_NODE) {
			struct BGTK_Widget *tw = make_text_widget(ctx, (const char *)child->content);
			if (tw) {
				if (inline_count >= inline_cap) {
					inline_cap *= 2;
					inline_buf = realloc(inline_buf, inline_cap * sizeof(struct BGTK_Widget *));
				}
				inline_buf[inline_count++] = tw;
			}
			continue;
		}

		if (child->type != XML_ELEMENT_NODE)
			continue;

		const char *tag = (const char *)child->name;

		if (is_inline_tag(tag)) {
			struct BGTK_Widget *w = convert_node(ctx, child, avail_w);
			if (w) {
				if (inline_count >= inline_cap) {
					inline_cap *= 2;
					inline_buf = realloc(inline_buf, inline_cap * sizeof(struct BGTK_Widget *));
				}
				inline_buf[inline_count++] = w;
			}
			continue;
		}

		// Block element: flush any pending inlines, then convert.
		FLUSH_INLINE();
		struct BGTK_Widget *w = convert_node(ctx, child, avail_w);
		if (w) {
			if (count >= cap) {
				cap *= 2;
				items = realloc(items, cap * sizeof(struct BGTK_Widget *));
			}
			items[count++] = w;
		}
	}
	FLUSH_INLINE();
	#undef FLUSH_INLINE

	free(inline_buf);
	*out_items = items;
	*out_count = count;
	return 0;
}

// Generic block container (div, section, body, ul, ol, ...).
static struct BGTK_Widget *convert_container(struct BGTK_Context *ctx,
					     xmlNode *node, int avail_w)
{
	struct BGTK_Widget **items = NULL;
	int count = 0;
	if (convert_children(ctx, node, avail_w, &items, &count) < 0)
		return NULL;

	if (count == 0) {
		free(items);
		return NULL;
	}
	// Single child: return directly to avoid unnecessary nesting.
	if (count == 1) {
		struct BGTK_Widget *only = items[0];
		free(items);
		return only;
	}

	BGTK_Options opts = opts_from_node(node, 0, 0);
	opts.orientation = BGTK_LIST_VERTICAL;
	struct BGTK_Widget *list = bgtk_list(ctx, items, count, opts);
	free(items);
	return list;
}

// <li> -> bullet/number prefix + content in a horizontal row.
static struct BGTK_Widget *convert_li(struct BGTK_Context *ctx, xmlNode *node,
				      int avail_w, int ordered, int index)
{
	char prefix[16];
	if (ordered)
		snprintf(prefix, sizeof(prefix), "%d. ", index);
	else
		snprintf(prefix, sizeof(prefix), "- ");

	struct BGTK_Widget *bullet = bgtk_text(ctx, prefix, (BGTK_Options){.margin = 2});
	if (!bullet)
		return NULL;

	// The li content itself can be complex (nested blocks).
	struct BGTK_Widget *content = convert_container(ctx, node, avail_w);
	if (!content) {
		// Fallback: try collecting text directly.
		char *txt = collect_text(node);
		if (txt && *txt) {
			content = bgtk_text(ctx, txt, (BGTK_Options){.margin = 0});
		}
		free(txt);
		if (!content)
			return bullet; // degenerate: just the bullet
	}

	struct BGTK_Widget *row_items[2] = {bullet, content};
	return bgtk_list(ctx, row_items, 2,
			 (BGTK_Options){.orientation = BGTK_LIST_HORIZONTAL, .margin = 2});
}

// <ul> / <ol> -> vertical list of <li> items.
static struct BGTK_Widget *convert_list(struct BGTK_Context *ctx, xmlNode *node,
					int avail_w, int ordered)
{
	int cap = 16, count = 0;
	struct BGTK_Widget **items = calloc(cap, sizeof(struct BGTK_Widget *));
	if (!items)
		return NULL;

	int idx = 1;
	for (xmlNode *child = node->children; child; child = child->next) {
		if (child->type != XML_ELEMENT_NODE)
			continue;
		if (strcmp((const char *)child->name, "li") != 0)
			continue;
		struct BGTK_Widget *w = convert_li(ctx, child, avail_w, ordered, idx);
		if (!w)
			continue;
		if (count >= cap) {
			cap *= 2;
			items = realloc(items, cap * sizeof(struct BGTK_Widget *));
		}
		items[count++] = w;
		idx++;
	}

	if (count == 0) {
		free(items);
		return NULL;
	}
	BGTK_Options opts = opts_from_node(node, 0, 4);
	opts.orientation = BGTK_LIST_VERTICAL;
	struct BGTK_Widget *list = bgtk_list(ctx, items, count, opts);
	free(items);
	return list;
}

/* ------------------------------------------------------------------ */
/* Table converter                                                     */
/* ------------------------------------------------------------------ */

// True if the cell has element children (button/input/...), not pure text.
static int cell_has_element_child(xmlNode *node)
{
	for (xmlNode *c = node->children; c; c = c->next)
		if (c->type == XML_ELEMENT_NODE)
			return 1;
	return 0;
}

// Convert a single <td>/<th> cell. Borderless by default (settings-friendly).
// Cells are left-aligned; content is vertically centered in the row.
// Pure-text cells must be BGTK_TEXT (not a nested list) so text_align works
// when draw_frame expands the child to the equalized column width.
static struct BGTK_Widget *convert_cell(struct BGTK_Context *ctx,
					xmlNode *node, int avail_w,
					int is_header, int col)
{
	struct BGTK_Widget *content = NULL;
	(void)col;

	if (!cell_has_element_child(node)) {
		char *txt = collect_text(node);
		if (!txt || !*txt) {
			free(txt);
			txt = strdup(" ");
		}
		content = bgtk_text(ctx, txt,
			(BGTK_Options){.text_v_align = BGTK_VALIGN_CENTER,
				       .text_align = BGTK_ALIGN_LEFT});
		free(txt);
	} else {
		content = convert_container(ctx, node, avail_w);
	}
	if (!content)
		return NULL;
	if (is_header && content->type == BGTK_WIDGET_TEXT)
		content->data.text.style |= BGTK_TEXT_BOLD;
	if (content->type == BGTK_WIDGET_TEXT) {
		content->text_v_align = BGTK_VALIGN_CENTER;
		content->text_align = BGTK_ALIGN_LEFT;
	}
	/* Drop widget margin inside cells so controls share the page left edge
	 * with Apply / <p> siblings (those keep their own margin). */
	if (content->type == BGTK_WIDGET_BUTTON ||
	    content->type == BGTK_WIDGET_TEXT_INPUT) {
		content->margin = 0;
	} else if (content->type == BGTK_WIDGET_LIST) {
		for (int i = 0; i < content->data.list_widget.widget_count; i++) {
			struct BGTK_Widget *ch = content->data.list_widget.items[i];
			if (ch && (ch->type == BGTK_WIDGET_BUTTON ||
				   ch->type == BGTK_WIDGET_TEXT_INPUT ||
				   ch->type == BGTK_WIDGET_TEXT))
				ch->margin = 0;
		}
	}

	/* Cell pad from theme (half of padding, min 2) so forms breathe
	 * without huge empty bands between rows. */
	/* Match typical nav-button pad (theme.pad/2+2) so form rows
	 * line up with sidebar chrome when hosts share the same inset. */
	int cell_pad = theme_pad(ctx, 4) / 2 + 2;
	if (cell_pad < 2)
		cell_pad = 2;
	if (cell_pad > 10)
		cell_pad = 10;
	struct BGTK_Widget *frame = bgtk_frame(ctx, content,
		content->w + 2 * cell_pad, content->h + 2 * cell_pad,
		(BGTK_Options){.padding = cell_pad, .margin = 0});
	if (frame)
		frame->data.frame.border_w = 0;
	return frame;
}

// Collect all <tr> rows from a <table>, including those inside <thead>/<tbody>/<tfoot>.
static int collect_rows(xmlNode *table, xmlNode ***out, int *out_count)
{
	int cap = 16, count = 0;
	xmlNode **rows = calloc(cap, sizeof(xmlNode *));
	if (!rows)
		return -1;

	for (xmlNode *child = table->children; child; child = child->next) {
		if (child->type != XML_ELEMENT_NODE)
			continue;
		const char *n = (const char *)child->name;
		if (strcmp(n, "tr") == 0) {
			if (count >= cap) { cap *= 2; rows = realloc(rows, cap * sizeof(xmlNode *)); }
			rows[count++] = child;
		} else if (strcmp(n, "thead") == 0 || strcmp(n, "tbody") == 0 || strcmp(n, "tfoot") == 0) {
			for (xmlNode *gc = child->children; gc; gc = gc->next) {
				if (gc->type == XML_ELEMENT_NODE && strcmp((const char *)gc->name, "tr") == 0) {
					if (count >= cap) { cap *= 2; rows = realloc(rows, cap * sizeof(xmlNode *)); }
					rows[count++] = gc;
				}
			}
		}
	}
	*out = rows;
	*out_count = count;
	return 0;
}

// <table> -> vertical list of horizontal rows with aligned column widths.
static struct BGTK_Widget *convert_table(struct BGTK_Context *ctx,
					 xmlNode *node, int avail_w)
{
	xmlNode **tr_nodes = NULL;
	int num_rows = 0;
	if (collect_rows(node, &tr_nodes, &num_rows) < 0 || num_rows == 0) {
		free(tr_nodes);
		return NULL;
	}

	// Find the max column count across all rows.
	int max_cols = 0;
	for (int r = 0; r < num_rows; r++) {
		int cols = 0;
		for (xmlNode *c = tr_nodes[r]->children; c; c = c->next)
			if (c->type == XML_ELEMENT_NODE &&
			    (strcmp((const char *)c->name, "td") == 0 ||
			     strcmp((const char *)c->name, "th") == 0))
				cols++;
		if (cols > max_cols)
			max_cols = cols;
	}
	if (max_cols == 0) {
		free(tr_nodes);
		return NULL;
	}

	// Build a 2D array of cell widgets (num_rows x max_cols).
	struct BGTK_Widget ***cells = calloc(num_rows, sizeof(struct BGTK_Widget **));
	int *row_cols = calloc(num_rows, sizeof(int));
	for (int r = 0; r < num_rows; r++) {
		cells[r] = calloc(max_cols, sizeof(struct BGTK_Widget *));
		int col = 0;
		for (xmlNode *c = tr_nodes[r]->children; c; c = c->next) {
			if (c->type != XML_ELEMENT_NODE)
				continue;
			const char *tn = (const char *)c->name;
			if (strcmp(tn, "td") != 0 && strcmp(tn, "th") != 0)
				continue;
			if (col >= max_cols)
				break;
			cells[r][col] = convert_cell(ctx, c, avail_w / max_cols,
						     strcmp(tn, "th") == 0, col);
			col++;
		}
		row_cols[r] = col;
	}

	// Compute max width per column and max height per row.
	int *col_widths = calloc(max_cols, sizeof(int));
	int *row_heights = calloc(num_rows, sizeof(int));
	for (int r = 0; r < num_rows; r++) {
		for (int c = 0; c < row_cols[r]; c++) {
			if (!cells[r][c])
				continue;
			if (cells[r][c]->w > col_widths[c])
				col_widths[c] = cells[r][c]->w;
			if (cells[r][c]->h > row_heights[r])
				row_heights[r] = cells[r][c]->h;
		}
	}

	/* Gap between columns from theme padding (min 8, max 24). */
	{
		int col_gap = theme_pad(ctx, 8);
		if (col_gap < 8)
			col_gap = 8;
		if (col_gap > 24)
			col_gap = 24;
		for (int c = 0; c < max_cols - 1; c++)
			col_widths[c] += col_gap;
	}

	// Equalize: every cell in a column gets the same width,
	// every cell in a row gets the same height.
	for (int r = 0; r < num_rows; r++) {
		for (int c = 0; c < row_cols[r]; c++) {
			if (!cells[r][c])
				continue;
			cells[r][c]->w = col_widths[c];
			cells[r][c]->h = row_heights[r];
		}
	}
	free(row_heights);

	// Build horizontal list per row, then vertical list of rows.
	struct BGTK_Widget **rows = calloc(num_rows, sizeof(struct BGTK_Widget *));
	int valid_rows = 0;
	for (int r = 0; r < num_rows; r++) {
		if (row_cols[r] == 0)
			continue;
		rows[valid_rows] = bgtk_list(ctx, cells[r], row_cols[r],
			(BGTK_Options){.orientation = BGTK_LIST_HORIZONTAL, .margin = 0});
		if (rows[valid_rows])
			valid_rows++;
	}

	struct BGTK_Widget *table = NULL;
	if (valid_rows > 0) {
		// Inner list: zero margin so rows share borders.
		BGTK_Options inner = {.orientation = BGTK_LIST_VERTICAL, .margin = 0};
		struct BGTK_Widget *grid = bgtk_list(ctx, rows, valid_rows, inner);
		if (grid) {
			/* Default margin 0 so table content lines up with sibling
			 * block widgets (paragraphs, Apply row). Override via
			 * margin="N" on <table> when inset is wanted. */
			int tm = attr_int(node, "margin", 0);
			if (tm > 0) {
				table = bgtk_frame(ctx, grid,
					grid->w + 2 * tm, grid->h + 2 * tm,
					(BGTK_Options){.padding = 0, .margin = tm});
				if (table) {
					table->data.frame.border_w = 0;
					table->data.frame.border_color = 0;
				}
			} else {
				table = grid;
			}
		}
	}

	// Cleanup temp arrays (widget pointers are owned by the tree now).
	for (int r = 0; r < num_rows; r++)
		free(cells[r]);
	free(cells);
	free(row_cols);
	free(col_widths);
	free(rows);
	free(tr_nodes);
	return table;
}

/* ------------------------------------------------------------------ */
/* Main node dispatcher                                                */
/* ------------------------------------------------------------------ */

static struct BGTK_Widget *convert_node(struct BGTK_Context *ctx,
					xmlNode *node, int avail_w)
{
	struct BGTK_Widget *w = NULL;

	if (!node)
		return NULL;

	// Pure text -> inline text widget.
	if (node->type == XML_TEXT_NODE)
		return make_text_widget(ctx, (const char *)node->content);

	if (node->type != XML_ELEMENT_NODE)
		return NULL;

	const char *tag = (const char *)node->name;

	/* Non-rendered / non-widget tags */
	if (strcmp(tag, "style") == 0 || strcmp(tag, "script") == 0 ||
	    strcmp(tag, "head") == 0 || strcmp(tag, "meta") == 0 ||
	    strcmp(tag, "link") == 0 || strcmp(tag, "title") == 0)
		return NULL;

	/* display:none before building (cheap check via empty widget). */
	if (html_style_widget(node, NULL))
		return NULL;

	if (strcmp(tag, "h1") == 0) w = convert_heading(ctx, node, 1);
	else if (strcmp(tag, "h2") == 0) w = convert_heading(ctx, node, 2);
	else if (strcmp(tag, "h3") == 0) w = convert_heading(ctx, node, 3);
	else if (strcmp(tag, "h4") == 0) w = convert_heading(ctx, node, 3);
	else if (strcmp(tag, "h5") == 0) w = convert_heading(ctx, node, 3);
	else if (strcmp(tag, "h6") == 0) w = convert_heading(ctx, node, 3);
	else if (strcmp(tag, "p") == 0)      w = convert_p(ctx, node, avail_w);
	else if (strcmp(tag, "b") == 0)      w = convert_bold(ctx, node);
	else if (strcmp(tag, "strong") == 0) w = convert_bold(ctx, node);
	else if (strcmp(tag, "i") == 0)      w = convert_italic(ctx, node);
	else if (strcmp(tag, "em") == 0)     w = convert_italic(ctx, node);
	else if (strcmp(tag, "a") == 0)      w = convert_a(ctx, node);
	else if (strcmp(tag, "code") == 0)   w = convert_code(ctx, node);
	else if (strcmp(tag, "pre") == 0)    w = convert_pre(ctx, node);
	else if (strcmp(tag, "br") == 0)     w = convert_br(ctx);
	else if (strcmp(tag, "span") == 0) {
		/* Plain text span (not italic). */
		char *txt = collect_text(node);
		if (txt && *txt)
			w = bgtk_text(ctx, txt, (BGTK_Options){0});
		free(txt);
	} else if (strcmp(tag, "button") == 0) w = convert_button(ctx, node);
	else if (strcmp(tag, "input") == 0)  w = convert_input(ctx, node);
	else if (strcmp(tag, "select") == 0) w = convert_select(ctx, node);
	else if (strcmp(tag, "img") == 0)    w = convert_img(ctx, node);
	else if (strcmp(tag, "ul") == 0)     w = convert_list(ctx, node, avail_w, 0);
	else if (strcmp(tag, "ol") == 0)     w = convert_list(ctx, node, avail_w, 1);
	else if (strcmp(tag, "table") == 0)  w = convert_table(ctx, node, avail_w);
	else
		/* Generic containers: div, section, article, body, … */
		w = convert_container(ctx, node, avail_w);

	if (w && html_style_widget(node, w)) {
		/* display:none after build (should be rare — checked above). */
		w = NULL;
	}
	return w;
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

static struct BGTK_Widget *parse_doc(struct BGTK_Context *ctx, htmlDocPtr doc,
				     int width, int height)
{
	struct BGTK_CSS *css;
	struct BGTK_Widget *content;
	xmlNode *root, *body = NULL;

	if (!doc)
		return NULL;

	root = xmlDocGetRootElement(doc);
	if (!root) {
		xmlFreeDoc(doc);
		return NULL;
	}

	css = bgtk_css_create();
	if (css)
		html_collect_styles(root, css);
	g_html_css = css;

	// Find <body> if present, otherwise use the root element.
	for (xmlNode *c = root->children; c; c = c->next) {
		if (c->type == XML_ELEMENT_NODE &&
		    strcmp((const char *)c->name, "body") == 0) {
			body = c;
			break;
		}
	}
	if (!body)
		body = root;

	/* convert_node applies body CSS (background, etc.). */
	content = convert_node(ctx, body, width);
	if (!content)
		content = convert_container(ctx, body, width);

	g_html_css = NULL;
	bgtk_css_destroy(css);

	if (!content) {
		xmlFreeDoc(doc);
		return NULL;
	}

	// Scrollable page; outer frame is borderless (host UI supplies chrome).
	{
		int pp = theme_pad(ctx, 4);
		if (pp > 8)
			pp = 8; /* page inset only — host frame has its own pad */
		struct BGTK_Widget **items = malloc(sizeof(struct BGTK_Widget *));
		items[0] = content;
		struct BGTK_Widget *scroll = bgtk_scrollable(ctx, items, 1,
			(BGTK_Options){.padding = pp, .margin = 0});
		free(items);
		if (!scroll) {
			xmlFreeDoc(doc);
			return content; // fallback
		}
		scroll->w = width;
		scroll->h = height;
		/* Propagate body background to the page surface. */
		if (content->color_bg) {
			scroll->color_bg = content->color_bg;
		}

		struct BGTK_Widget *frame = bgtk_frame(ctx, scroll, width, height,
			(BGTK_Options){.padding = 0, .margin = 0});
		if (frame) {
			frame->data.frame.border_w = 0;
			if (content->color_bg)
				frame->color_bg = content->color_bg;
		}
		xmlFreeDoc(doc);
		return frame ? frame : scroll;
	}
}

struct BGTK_Widget *bgtk_html_parse(struct BGTK_Context *ctx,
				    const char *path, int width, int height)
{
	if (!ctx || !path)
		return NULL;

	htmlDocPtr doc = htmlReadFile(path, NULL,
		HTML_PARSE_NOERROR | HTML_PARSE_NOWARNING | HTML_PARSE_RECOVER);
	return parse_doc(ctx, doc, width, height);
}

struct BGTK_Widget *bgtk_html_parse_inline(struct BGTK_Context *ctx,
					   const char *html, int width,
					   int height)
{
	if (!ctx || !html)
		return NULL;

	htmlDocPtr doc = htmlReadMemory(html, (int)strlen(html), "inline.html",
		NULL, HTML_PARSE_NOERROR | HTML_PARSE_NOWARNING | HTML_PARSE_RECOVER);
	return parse_doc(ctx, doc, width, height);
}
