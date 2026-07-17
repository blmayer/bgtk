/*
 * html.c - HTML-to-BGTK widget tree parser.
 *
 * Uses libxml2 to parse HTML and maps common tags to real BGTK widgets
 * so users can describe their UI in standard HTML.
 *
 * Supported tags:
 *   h1..h6, p, b, i, a, span    -> BGTK_WIDGET_TEXT (with header_level / bold)
 *   pre, code                    -> mono; pre keeps newlines/spaces and
 *                                   nested inlines (<a>, <b>, …) as widgets
 *   br                           -> line break (block flush)
 *   ul, ol, li                   -> BGTK_WIDGET_LIST (vertical, with bullet/number)
 *   button                       -> BGTK_WIDGET_BUTTON
 *   input[type=text]             -> BGTK_WIDGET_TEXT_INPUT
 *   input[type=checkbox]         -> BGTK_WIDGET_BUTTON (toggle)
 *   select                       -> BGTK_WIDGET_LIST (dropdown placeholder)
 *   img                          -> BGTK_WIDGET_IMAGE (file / http via ctx->fetch_url;
 *                                   width/height attrs; CSS max-width / max-height)
 *   font, center, hr, kbd        -> color text / centered block / rule / mono
 *   div, section, article, body  -> BGTK_WIDGET_LIST (vertical container)
 *   unknown tags                 -> containers (htop, dd, article, …)
 * Text blocks wrap to page width and CSS width/max-width (px/em/rem/%).
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

/* Remeasure text widgets with mono (and recurse containers). */
static void widget_force_mono(struct BGTK_Context *ctx, struct BGTK_Widget *w)
{
	int i;

	if (!w)
		return;
	if (w->type == BGTK_WIDGET_TEXT && w->data.text.text) {
		FT_Face face = bgtk_font_face(ctx, BGTK_FONT_MONO);
		int tw = 0, th = 0;

		w->data.text.font_role = BGTK_FONT_MONO;
		if (face && ctx) {
			FT_Set_Pixel_Sizes(face, 0,
					   ctx->font_size > 0 ? ctx->font_size
							      : 14);
			measure_text_style(face, w->data.text.text,
					   w->data.text.style, &tw, &th);
			w->w = tw + 2 * (w->padding + w->margin);
			w->h = th + 2 * (w->padding + w->margin);
		}
		return;
	}
	if (w->type == BGTK_WIDGET_LIST) {
		for (i = 0; i < w->data.list_widget.widget_count; i++)
			widget_force_mono(ctx, w->data.list_widget.items[i]);
		return;
	}
	if (w->type == BGTK_WIDGET_FRAME)
		widget_force_mono(ctx, w->data.frame.child);
	else if (w->type == BGTK_WIDGET_BUTTON)
		widget_force_mono(ctx, w->data.button.label);
	else if (w->type == BGTK_WIDGET_LABEL)
		widget_force_mono(ctx, w->data.label.text);
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
	       strcmp(tag, "code") == 0 || strcmp(tag, "font") == 0 ||
	       strcmp(tag, "kbd") == 0 || strcmp(tag, "diff") == 0;
}

/* ------------------------------------------------------------------ */
/* Forward declarations                                                */
/* ------------------------------------------------------------------ */

static struct BGTK_Widget *convert_node(struct BGTK_Context *ctx,
					xmlNode *node, int avail_w);
static struct BGTK_Widget *convert_container(struct BGTK_Context *ctx,
					     xmlNode *node, int avail_w);

/* Apply g_html_css + inline style; optional CSS width/max-width outs (-1). */
static int html_style_widget(xmlNode *node, struct BGTK_Widget *w,
			     int *out_width, int *out_max_width)
{
	xmlChar *id = NULL, *cls = NULL, *sty = NULL;
	const char *tag = NULL;
	int hide;

	if (out_width)
		*out_width = -1;
	if (out_max_width)
		*out_max_width = -1;
	if (!node || node->type != XML_ELEMENT_NODE)
		return 0;
	tag = (const char *)node->name;
	id = xmlGetProp(node, (const xmlChar *)"id");
	cls = xmlGetProp(node, (const xmlChar *)"class");
	sty = xmlGetProp(node, (const xmlChar *)"style");
	hide = bgtk_css_apply(g_html_css, w, tag,
			      id ? (const char *)id : NULL,
			      cls ? (const char *)cls : NULL,
			      sty ? (const char *)sty : NULL, out_width,
			      out_max_width);
	if (id)
		xmlFree(id);
	if (cls)
		xmlFree(cls);
	if (sty)
		xmlFree(sty);
	return hide;
}

/*
 * Word-wrap plain text to max_w (content pixels). Returns malloc lines.
 * UTF-8 aware; breaks on spaces when possible.
 */
static char **html_wrap_lines(FT_Face face, const char *text, int max_w,
			      int *nlines)
{
	char **lines = NULL;
	int n = 0, cap = 0, len, i = 0;

	*nlines = 0;
	if (!text || !*text || max_w < 16)
		return NULL;
	len = (int)strlen(text);
	/* Face pixel size must already be set by the caller. */
	while (i < len) {
		int j = i, last_space = -1, w = 0;

		while (j < len) {
			const char *cp = text + j;
			size_t left = (size_t)(len - j);
			const char *start = cp;
			uint32_t ch = bgtk_utf8_next_n(&cp, &left);
			int nbytes = (int)(cp - start);
			int cw = 7;

			if (nbytes < 1)
				break;
			if (face &&
			    FT_Load_Char(face, (FT_ULong)ch, FT_LOAD_DEFAULT) ==
				    0)
				cw = face->glyph->advance.x >> 6;
			if (w + cw > max_w && j > i)
				break;
			w += cw;
			if (ch < 128 && isspace((unsigned char)ch))
				last_space = j;
			j += nbytes;
		}
		{
			int end = (j >= len)
					  ? len
					  : (last_space > i ? last_space : j);
			int lnlen = end - i;
			char *ln = malloc((size_t)lnlen + 1);

			if (!ln)
				break;
			memcpy(ln, text + i, (size_t)lnlen);
			ln[lnlen] = '\0';
			while (lnlen > 0 &&
			       isspace((unsigned char)ln[lnlen - 1]))
				ln[--lnlen] = '\0';
			if (n >= cap) {
				cap = cap ? cap * 2 : 8;
				lines = realloc(lines,
						(size_t)cap * sizeof(char *));
				if (!lines)
					break;
			}
			lines[n++] = ln;
			i = end;
			while (i < len && isspace((unsigned char)text[i]))
				i++;
		}
	}
	*nlines = n;
	return lines;
}

/* If text widget wider than max_content, replace text with wrapped lines. */
static void wrap_text_widget(struct BGTK_Context *ctx, struct BGTK_Widget *w,
			     int max_content)
{
	char **lines;
	int n = 0, i, st;
	size_t total = 0;
	char *joined;
	FT_Face face;

	if (!ctx || !w || w->type != BGTK_WIDGET_TEXT || !w->data.text.text)
		return;
	if (max_content < 16)
		return;
	/* Natural width already fits. */
	if (w->w - 2 * (w->padding + w->margin) <= max_content)
		return;

	face = bgtk_font_face(ctx, w->data.text.font_role);
	if (face)
		FT_Set_Pixel_Sizes(face, 0,
				   ctx->font_size > 0 ? ctx->font_size : 14);
	st = w->data.text.style;
	if (w->data.text.header_level > 0 && w->data.text.header_level <= 3)
		st |= BGTK_TEXT_BOLD;
	lines = html_wrap_lines(face, w->data.text.text, max_content, &n);
	if (!lines || n < 1) {
		if (lines) {
			for (i = 0; i < n; i++)
				free(lines[i]);
			free(lines);
		}
		return;
	}
	for (i = 0; i < n; i++)
		total += strlen(lines[i]) + 1;
	joined = malloc(total + 1);
	if (!joined) {
		for (i = 0; i < n; i++)
			free(lines[i]);
		free(lines);
		return;
	}
	joined[0] = '\0';
	for (i = 0; i < n; i++) {
		if (i)
			strcat(joined, "\n");
		strcat(joined, lines[i]);
		free(lines[i]);
	}
	free(lines);
	free(w->data.text.text);
	w->data.text.text = joined;
	/* Remeasure multi-line. */
	{
		int tw = 0, th = 0;

		measure_text_style(face, joined, st, &tw, &th);
		w->w = tw + 2 * (w->padding + w->margin);
		w->h = th + 2 * (w->padding + w->margin);
		if (w->w > max_content + 2 * (w->padding + w->margin))
			w->w = max_content + 2 * (w->padding + w->margin);
	}
}

/* CSS length may be encoded as 100000+pct for percentage. */
static int css_len_px(int v, int avail)
{
	if (v >= 100000)
		return (avail * (v - 100000)) / 100;
	if (v > 0)
		return v;
	return -1;
}

/* Clamp wrap width: page avail, CSS width/max-width, HTML width attr. */
static int clamp_wrap_w(int avail_w, int css_w, int css_max, int attr_w,
			int pad_mar)
{
	int m = avail_w - pad_mar;
	int cw, cm;

	if (m < 16)
		m = 16;
	cw = css_len_px(css_w, m);
	cm = css_len_px(css_max, m);
	if (cw > 0 && cw < m)
		m = cw;
	if (cm > 0 && cm < m)
		m = cm;
	if (attr_w > 0 && attr_w < m)
		m = attr_w;
	return m;
}

/* Resolve relative URL against base (absolute http/https/file/about). */
static void html_resolve_url(const char *base, const char *rel, char *out,
			     size_t outlen)
{
	char dir[768], full[1024];
	const char *slash;

	if (!out || outlen < 2)
		return;
	out[0] = '\0';
	if (!rel || !*rel) {
		snprintf(out, outlen, "%s", base ? base : "");
		return;
	}
	/* Protocol-relative //host/path */
	if (rel[0] == '/' && rel[1] == '/') {
		if (base && !strncmp(base, "https:", 6))
			snprintf(out, outlen, "https:%s", rel);
		else
			snprintf(out, outlen, "http:%s", rel);
		return;
	}
	if (strstr(rel, "://") || !strncmp(rel, "data:", 5) ||
	    !strncmp(rel, "about:", 6) || !strncmp(rel, "file:", 5)) {
		snprintf(out, outlen, "%s", rel);
		return;
	}
	if (!base || !*base) {
		snprintf(out, outlen, "%s", rel);
		return;
	}
	if (rel[0] == '/') {
		/* Site-absolute path on same origin */
		if (!strncmp(base, "https://", 8) ||
		    !strncmp(base, "http://", 7)) {
			const char *scheme_end = strstr(base, "://");
			const char *host = scheme_end ? scheme_end + 3 : base;
			const char *path = strchr(host, '/');
			size_t origin = path ? (size_t)(path - base)
					     : strlen(base);

			if (origin + strlen(rel) + 1 < outlen) {
				memcpy(out, base, origin);
				out[origin] = '\0';
				snprintf(out + origin, outlen - origin, "%s",
					 rel);
			}
			return;
		}
		if (!strncmp(base, "file://", 7)) {
			/* file:///site/root/ + /photos/x → file:///site/root/photos/x */
			const char *root = base + 7;
			char rootdir[768];
			char *slashp;
			size_t rl;

			snprintf(rootdir, sizeof(rootdir), "%s", root);
			rl = strlen(rootdir);
			if (rl > 0 && rootdir[rl - 1] != '/') {
				slashp = strrchr(rootdir, '/');
				if (slashp)
					slashp[1] = '\0';
			}
			rl = strlen(rootdir);
			if (rl > 0 && rootdir[rl - 1] == '/')
				rootdir[rl - 1] = '\0';
			snprintf(out, outlen, "file://%s%s", rootdir, rel);
			return;
		}
		snprintf(out, outlen, "%s", rel);
		return;
	}
	/* Directory of base + rel */
	slash = strrchr(base, '/');
	if (slash && slash > base + 7) {
		size_t dlen = (size_t)(slash - base + 1);

		if (dlen >= sizeof(dir))
			dlen = sizeof(dir) - 1;
		memcpy(dir, base, dlen);
		dir[dlen] = '\0';
	} else {
		snprintf(dir, sizeof(dir), "%s", base);
	}
	snprintf(full, sizeof(full), "%s%s", dir, rel);
	snprintf(out, outlen, "%s", full);
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
					   xmlNode *node, int level,
					   int avail_w)
{
	char *txt = collect_text(node);
	if (!txt || !*txt) {
		free(txt);
		return NULL;
	}
	BGTK_Options opts = opts_from_node(node, 2, 4);
	struct BGTK_Widget *w = bgtk_text(ctx, txt, opts);
	free(txt);
	if (w) {
		int maxc;

		w->data.text.header_level = level;
		maxc = avail_w - 2 * (w->padding + w->margin);
		if (level > 0 && level <= 3 && ctx && ctx->ft_face) {
			/* Headings use larger font; measure wrap with size bump. */
			int old = ctx->font_size;
			ctx->font_size = old + (4 - level);
			FT_Set_Pixel_Sizes(ctx->ft_face, 0, ctx->font_size);
			wrap_text_widget(ctx, w, maxc > 16 ? maxc : 16);
			ctx->font_size = old;
			FT_Set_Pixel_Sizes(ctx->ft_face, 0,
					   old > 0 ? old : 14);
		} else {
			wrap_text_widget(ctx, w, maxc > 16 ? maxc : 16);
		}
	}
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
	/* Single text paragraph: wrap to content width. */
	if (w->type == BGTK_WIDGET_TEXT) {
		int maxc = avail_w - 2 * (w->padding + w->margin);

		wrap_text_widget(ctx, w, maxc > 16 ? maxc : 16);
	}
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

/* <font color="#rrggbb"> -> text with color_fg (legacy HTML). */
static struct BGTK_Widget *convert_font(struct BGTK_Context *ctx, xmlNode *node)
{
	char *txt = collect_text(node);
	xmlChar *col;
	struct BGTK_Widget *w;

	if (!txt || !*txt) {
		free(txt);
		return NULL;
	}
	w = bgtk_text(ctx, txt, (BGTK_Options){0});
	free(txt);
	if (!w)
		return NULL;
	col = attr_str(node, "color");
	if (col && col[0]) {
		uint32_t c = bgtk_css_parse_color((const char *)col);

		if (c)
			w->color_fg = c;
		xmlFree(col);
	} else if (col) {
		xmlFree(col);
	}
	return w;
}

/* <center> / unknown block-ish tags → container, optional center align. */
static struct BGTK_Widget *convert_center(struct BGTK_Context *ctx,
					  xmlNode *node, int avail_w)
{
	struct BGTK_Widget *w = convert_container(ctx, node, avail_w);

	if (w) {
		w->text_align = BGTK_ALIGN_CENTER;
		if (w->type == BGTK_WIDGET_TEXT)
			w->text_align = BGTK_ALIGN_CENTER;
		else if (w->type == BGTK_WIDGET_LIST) {
			int i;

			for (i = 0; i < w->data.list_widget.widget_count; i++) {
				struct BGTK_Widget *ch =
					w->data.list_widget.items[i];

				if (ch && ch->type == BGTK_WIDGET_TEXT)
					ch->text_align = BGTK_ALIGN_CENTER;
			}
		}
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

/* Pure-text <pre>: one multi-line mono widget (newlines kept in string). */
static struct BGTK_Widget *convert_pre_plain(struct BGTK_Context *ctx,
					     xmlNode *node)
{
	char *txt = collect_text_pre(node);
	struct BGTK_Widget *w;
	BGTK_Options opts;

	if (!txt)
		return NULL;
	if (!txt[0]) {
		free(txt);
		txt = strdup(" ");
		if (!txt)
			return NULL;
	}
	opts = opts_from_node(node, 4, 4);
	w = make_text_face(ctx, txt, opts, BGTK_FONT_MONO);
	free(txt);
	/* No default pre panel bg — match page (CSS may set background). */
	return w;
}

/*
 * Pre text run: expand tabs, keep spaces; split on \n into line rows.
 * Segments join the current horizontal line; each \n flushes a row.
 */
static int pre_append_text(struct BGTK_Context *ctx, const char *raw,
			   struct BGTK_Widget ***inline_buf, int *inline_count,
			   int *inline_cap, struct BGTK_Widget ***items,
			   int *count, int *cap)
{
	char *norm, *p, *start;
	size_t n, i, out_n = 0, out_cap;
	struct BGTK_Widget *tw;

	if (!raw)
		return 0;
	/* Expand tabs / normalize newlines into a mutable buffer. */
	n = strlen(raw);
	out_cap = n * 2 + 1;
	norm = malloc(out_cap);
	if (!norm)
		return -1;
	for (i = 0; i < n; i++) {
		char c = raw[i];

		if (c == '\r') {
			if (i + 1 < n && raw[i + 1] == '\n')
				continue;
			c = '\n';
		}
		if (c == '\t') {
			int spaces = 8 - (int)(out_n % 8);

			while (spaces-- > 0) {
				if (out_n + 1 >= out_cap) {
					out_cap *= 2;
					norm = realloc(norm, out_cap);
					if (!norm)
						return -1;
				}
				norm[out_n++] = ' ';
			}
			continue;
		}
		if (out_n + 1 >= out_cap) {
			out_cap *= 2;
			norm = realloc(norm, out_cap);
			if (!norm)
				return -1;
		}
		norm[out_n++] = c;
	}
	norm[out_n] = '\0';

	start = norm;
	for (p = norm; ; p++) {
		if (*p != '\n' && *p != '\0')
			continue;
		{
			char save = *p;
			int at_end = (save == '\0');

			*p = '\0';
			if (start[0]) {
				tw = make_text_face(
					ctx, start,
					(BGTK_Options){.padding = 0, .margin = 0},
					BGTK_FONT_MONO);
				if (tw) {
					if (*inline_count >= *inline_cap) {
						*inline_cap *= 2;
						*inline_buf = realloc(
							*inline_buf,
							(size_t)(*inline_cap) *
								sizeof(**inline_buf));
					}
					if (*inline_buf)
						(*inline_buf)[(*inline_count)++] =
							tw;
				}
			}
			if (!at_end) {
				/* End of line: flush horizontal run as a row. */
				if (*inline_count > 0) {
					BGTK_Options hopts = {
						.orientation =
							BGTK_LIST_HORIZONTAL,
						.margin = 0,
						.padding = 0};
					struct BGTK_Widget *row = bgtk_list(
						ctx, *inline_buf, *inline_count,
						hopts);
					if (row) {
						if (*count >= *cap) {
							*cap *= 2;
							*items = realloc(
								*items,
								(size_t)(*cap) *
									sizeof(**items));
						}
						if (*items)
							(*items)[(*count)++] =
								row;
					}
					*inline_count = 0;
				} else {
					/* Blank line inside pre. */
					tw = make_text_face(
						ctx, " ",
						(BGTK_Options){.padding = 0,
							       .margin = 0},
						BGTK_FONT_MONO);
					if (tw) {
						if (*count >= *cap) {
							*cap *= 2;
							*items = realloc(
								*items,
								(size_t)(*cap) *
									sizeof(**items));
						}
						if (*items)
							(*items)[(*count)++] =
								tw;
					}
				}
				start = p + 1;
			}
			if (at_end)
				break;
		}
	}
	free(norm);
	return 0;
}

/*
 * <pre> with nested markup: walk children; text keeps whitespace; inlines
 * (<a>, <b>, <code>, …) become real widgets (mono + link href preserved).
 */
static struct BGTK_Widget *convert_pre_rich(struct BGTK_Context *ctx,
					    xmlNode *node, int avail_w)
{
	int cap = 16, count = 0;
	int inline_cap = 16, inline_count = 0;
	struct BGTK_Widget **items = calloc((size_t)cap, sizeof(*items));
	struct BGTK_Widget **inline_buf =
		calloc((size_t)inline_cap, sizeof(*inline_buf));
	struct BGTK_Widget *block;
	BGTK_Options opts;
	int first_text = 1;

	if (!items || !inline_buf) {
		free(items);
		free(inline_buf);
		return NULL;
	}

	for (xmlNode *child = node->children; child; child = child->next) {
		if (child->type == XML_TEXT_NODE ||
		    child->type == XML_CDATA_SECTION_NODE) {
			const char *raw = (const char *)child->content;
			const char *use = raw;

			/* Drop one leading newline after <pre> (pretty-print). */
			if (first_text && use && use[0] == '\n')
				use++;
			first_text = 0;
			if (use && use[0])
				pre_append_text(ctx, use, &inline_buf,
						&inline_count, &inline_cap,
						&items, &count, &cap);
			continue;
		}
		if (child->type != XML_ELEMENT_NODE)
			continue;
		first_text = 0;
		{
			const char *tag = (const char *)child->name;
			struct BGTK_Widget *w;

			if (strcmp(tag, "br") == 0) {
				/* Force new line. */
				if (inline_count > 0) {
					BGTK_Options hopts = {
						.orientation =
							BGTK_LIST_HORIZONTAL,
						.margin = 0};
					struct BGTK_Widget *row = bgtk_list(
						ctx, inline_buf, inline_count,
						hopts);
					if (row) {
						if (count >= cap) {
							cap *= 2;
							items = realloc(
								items,
								(size_t)cap *
									sizeof(*items));
						}
						if (items)
							items[count++] = row;
					}
					inline_count = 0;
				}
				continue;
			}
			if (is_inline_tag(tag) || strcmp(tag, "code") == 0) {
				w = convert_node(ctx, child, avail_w);
				if (w) {
					widget_force_mono(ctx, w);
					if (inline_count >= inline_cap) {
						inline_cap *= 2;
						inline_buf = realloc(
							inline_buf,
							(size_t)inline_cap *
								sizeof(*inline_buf));
					}
					if (inline_buf)
						inline_buf[inline_count++] = w;
				}
				continue;
			}
			/* Rare block inside pre: flush then convert. */
			if (inline_count > 0) {
				BGTK_Options hopts = {
					.orientation = BGTK_LIST_HORIZONTAL,
					.margin = 0};
				struct BGTK_Widget *row = bgtk_list(
					ctx, inline_buf, inline_count, hopts);
				if (row) {
					if (count >= cap) {
						cap *= 2;
						items = realloc(
							items,
							(size_t)cap *
								sizeof(*items));
					}
					if (items)
						items[count++] = row;
				}
				inline_count = 0;
			}
			w = convert_node(ctx, child, avail_w);
			if (w) {
				widget_force_mono(ctx, w);
				if (count >= cap) {
					cap *= 2;
					items = realloc(items,
							(size_t)cap *
								sizeof(*items));
				}
				if (items)
					items[count++] = w;
			}
		}
	}
	/* Flush last partial line. */
	if (inline_count > 0) {
		BGTK_Options hopts = {.orientation = BGTK_LIST_HORIZONTAL,
				      .margin = 0};
		struct BGTK_Widget *row =
			bgtk_list(ctx, inline_buf, inline_count, hopts);
		if (row) {
			if (count >= cap) {
				cap *= 2;
				items = realloc(items,
						(size_t)cap * sizeof(*items));
			}
			if (items)
				items[count++] = row;
		}
		inline_count = 0;
	}
	free(inline_buf);

	if (count == 0) {
		free(items);
		return convert_pre_plain(ctx, node);
	}
	if (count == 1) {
		block = items[0];
		free(items);
	} else {
		opts = opts_from_node(node, 4, 4);
		opts.orientation = BGTK_LIST_VERTICAL;
		opts.margin = 0;
		block = bgtk_list(ctx, items, count, opts);
		free(items);
	}
	if (block) {
		BGTK_Options o = opts_from_node(node, 4, 4);
		struct BGTK_Widget *wrap;

		/* Outer pad only — transparent like browser <pre> default. */
		wrap = bgtk_frame(ctx, block,
				  block->w + 2 * o.padding,
				  block->h + 2 * o.padding,
				  (BGTK_Options){.padding = o.padding,
						 .margin = o.margin});
		if (wrap) {
			wrap->data.frame.border_w = 0;
			block = wrap;
		}
	}
	return block;
}

/* <pre> -> mono block; nested <a>/<b>/… kept as widgets when present. */
static struct BGTK_Widget *convert_pre(struct BGTK_Context *ctx, xmlNode *node,
				       int avail_w)
{
	int has_el = 0;

	for (xmlNode *c = node ? node->children : NULL; c; c = c->next) {
		if (c->type == XML_ELEMENT_NODE) {
			has_el = 1;
			break;
		}
	}
	if (!has_el)
		return convert_pre_plain(ctx, node);
	return convert_pre_rich(ctx, node, avail_w);
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

/* Try load pixels from resolved path/url (file, http, fetch hook). */
static int html_load_image_src(struct BGTK_Context *ctx, const char *path,
			       uint32_t **pixels, int *iw, int *ih)
{
	unsigned char *data = NULL;
	size_t dlen = 0;

	*pixels = NULL;
	*iw = *ih = 0;
	if (!path || !*path)
		return -1;

	if (!strncmp(path, "file://", 7)) {
		const char *fp = path + 7;

		if (fp[0] == '/' && fp[1] == '/')
			fp++;
		if (load_image(fp, pixels, iw, ih) == 0)
			return 0;
	} else if (!strncmp(path, "http://", 7) ||
		   !strncmp(path, "https://", 8)) {
		if (ctx && ctx->fetch_url &&
		    ctx->fetch_url(path, &data, &dlen, ctx->fetch_userdata) ==
			    0 &&
		    data && dlen > 0) {
			int ok = load_image_mem(data, (int)dlen, pixels, iw, ih);

			free(data);
			return ok;
		}
		return -1;
	} else if (path[0] == '/') {
		/* Absolute FS path */
		if (load_image(path, pixels, iw, ih) == 0)
			return 0;
	} else {
		if (load_image(path, pixels, iw, ih) == 0)
			return 0;
	}
	/* Last resort: app fetch hook with original path (maps /photos under site). */
	if (ctx && ctx->fetch_url &&
	    ctx->fetch_url(path, &data, &dlen, ctx->fetch_userdata) == 0 &&
	    data && dlen > 0) {
		int ok = load_image_mem(data, (int)dlen, pixels, iw, ih);

		free(data);
		return ok;
	}
	return -1;
}

// <img> -> BGTK_WIDGET_IMAGE (file path or http(s) via ctx->fetch_url).
static struct BGTK_Widget *convert_img(struct BGTK_Context *ctx, xmlNode *node,
				       int avail_w)
{
	xmlChar *src = attr_str(node, "src");
	xmlChar *alt;
	char resolved[1024];
	const char *path;
	int attr_w, attr_h, iw = 0, ih = 0;
	uint32_t *pixels = NULL;
	BGTK_Options opts;
	struct BGTK_Widget *img = NULL;
	int max_w, max_h = 0;
	int css_w = -1, css_max = -1;

	if (!src || !src[0]) {
		if (src)
			xmlFree(src);
		return NULL;
	}
	attr_w = attr_int(node, "width", 0);
	attr_h = attr_int(node, "height", 0);
	opts = opts_from_node(node, 0, 4);
	html_resolve_url(ctx ? ctx->base_url : NULL, (const char *)src,
			 resolved, sizeof(resolved));
	path = resolved[0] ? resolved : (const char *)src;

	(void)html_load_image_src(ctx, path, &pixels, &iw, &ih);
	/* Also try original src via fetch (site-absolute /photos/...). */
	if (!pixels && src[0] == '/' && ctx && ctx->fetch_url)
		(void)html_load_image_src(ctx, (const char *)src, &pixels, &iw,
					  &ih);
	xmlFree(src);

	/* Peek CSS width/max-width/max-height from style for sizing. */
	html_style_widget(node, NULL, &css_w, &css_max);

	if (!pixels || iw < 1 || ih < 1) {
		alt = attr_str(node, "alt");
		img = bgtk_text(ctx,
				alt && alt[0] ? (char *)alt : "[image]",
				(BGTK_Options){.padding = 4, .margin = 4});
		if (alt)
			xmlFree(alt);
		return img;
	}

	/* Fit to attrs, CSS, and page width (width:100% ≈ fill avail). */
	max_w = avail_w - 2 * (opts.padding + opts.margin);
	if (max_w < 16)
		max_w = 16;
	if (css_max > 0 && css_max < max_w)
		max_w = css_max;
	if (css_w > 0 && css_w < max_w)
		max_w = css_w;
	/* CSS max-height on images (e.g. body>p>img { max-height:540px }) */
	{
		xmlChar *sty = attr_str(node, "style");

		if (sty) {
			const char *p = strstr((const char *)sty, "max-height");

			if (p) {
				p = strchr(p, ':');
				if (p)
					max_h = atoi(p + 1);
			}
			xmlFree(sty);
		}
	}
	/* style on parent not available — default max-height for large photos */
	if (max_h <= 0 && ih > 540 && avail_w > 0)
		max_h = 540;

	{
		int dw = attr_w > 0 ? attr_w : iw;
		int dh = attr_h > 0 ? attr_h : ih;

		if (attr_w > 0 && attr_h <= 0)
			dh = (int)((long)ih * attr_w / (iw > 0 ? iw : 1));
		else if (attr_h > 0 && attr_w <= 0)
			dw = (int)((long)iw * attr_h / (ih > 0 ? ih : 1));
		/* width:100% without attrs → fill content width */
		if (attr_w <= 0 && attr_h <= 0 && dw > max_w) {
			dh = (int)((long)dh * max_w / (dw > 0 ? dw : 1));
			dw = max_w;
		} else if (dw > max_w) {
			dh = (int)((long)dh * max_w / (dw > 0 ? dw : 1));
			dw = max_w;
		}
		if (max_h > 0 && dh > max_h) {
			dw = (int)((long)dw * max_h / (dh > 0 ? dh : 1));
			dh = max_h;
		}
		if (dw < 1)
			dw = 1;
		if (dh < 1)
			dh = 1;
		if (dw != iw || dh != ih) {
			uint32_t *scaled =
				scale_image_argb(pixels, iw, ih, dw, dh);

			if (scaled) {
				pixels = scaled;
				iw = dw;
				ih = dh;
			}
		}
	}

	img = bgtk_image(ctx, NULL, iw + 2 * (opts.padding + opts.margin),
			 ih + 2 * (opts.padding + opts.margin), opts);
	if (!img) {
		free(pixels);
		return NULL;
	}
	img->data.image.pixels = pixels;
	img->data.image.img_w = iw;
	img->data.image.img_h = ih;
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
	// Wrap any text run wider than avail_w first.
	#define FLUSH_INLINE() do { \
		int _fi; \
		for (_fi = 0; _fi < inline_count; _fi++) { \
			struct BGTK_Widget *_tw = inline_buf[_fi]; \
			if (_tw && _tw->type == BGTK_WIDGET_TEXT) { \
				int _m = avail_w - 2 * (_tw->padding + _tw->margin); \
				wrap_text_widget(ctx, _tw, _m > 16 ? _m : 16); \
			} \
		} \
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

/* Convert cell content into a framed cell (border optional). */
static struct BGTK_Widget *convert_cell(struct BGTK_Context *ctx,
					xmlNode *node, int avail_w,
					int is_header, int border_w,
					uint32_t border_color)
{
	struct BGTK_Widget *content = NULL;
	int cell_pad;
	int css_pad = -1;

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
		content = convert_container(ctx, node, avail_w > 16 ? avail_w : 16);
	}
	if (!content)
		return NULL;
	if (is_header && content->type == BGTK_WIDGET_TEXT)
		content->data.text.style |= BGTK_TEXT_BOLD;
	if (content->type == BGTK_WIDGET_TEXT) {
		content->text_v_align = BGTK_VALIGN_CENTER;
		content->text_align = BGTK_ALIGN_LEFT;
		wrap_text_widget(ctx, content,
				 avail_w > 16 ? avail_w - 8 : 16);
	}
	if (content->type == BGTK_WIDGET_BUTTON ||
	    content->type == BGTK_WIDGET_TEXT_INPUT)
		content->margin = 0;

	cell_pad = theme_pad(ctx, 4) / 2 + 2;
	if (cell_pad < 2)
		cell_pad = 2;
	if (cell_pad > 10)
		cell_pad = 10;
	/* CSS padding on td/th (e.g. padding: 0 1em). */
	{
		struct BGTK_Widget probe = {0};
		int hide;

		probe.padding = cell_pad;
		hide = html_style_widget(node, &probe, NULL, NULL);
		(void)hide;
		if (probe.padding != cell_pad)
			css_pad = probe.padding;
	}
	if (css_pad >= 0)
		cell_pad = css_pad;
	if (border_w < 0)
		border_w = 0;
	{
		struct BGTK_Widget *frame = bgtk_frame(
			ctx, content,
			content->w + 2 * (cell_pad + border_w),
			content->h + 2 * (cell_pad + border_w),
			(BGTK_Options){.padding = cell_pad, .margin = 0});
		if (frame) {
			frame->data.frame.border_w = border_w;
			frame->data.frame.border_color = border_color;
			/* Apply remaining cell CSS (colors) onto frame. */
			html_style_widget(node, frame, NULL, NULL);
		}
		return frame;
	}
}

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
			if (count >= cap) {
				cap *= 2;
				rows = realloc(rows, cap * sizeof(xmlNode *));
			}
			rows[count++] = child;
		} else if (strcmp(n, "thead") == 0 || strcmp(n, "tbody") == 0 ||
			   strcmp(n, "tfoot") == 0) {
			for (xmlNode *gc = child->children; gc; gc = gc->next) {
				if (gc->type == XML_ELEMENT_NODE &&
				    strcmp((const char *)gc->name, "tr") == 0) {
					if (count >= cap) {
						cap *= 2;
						rows = realloc(
							rows,
							cap * sizeof(xmlNode *));
					}
					rows[count++] = gc;
				}
			}
		}
	}
	*out = rows;
	*out_count = count;
	return 0;
}

/* Table border from border= attr + CSS cascade (stylesheet + inline). */
static void table_border_style(xmlNode *node, int *border_w, uint32_t *border_c)
{
	int bw = attr_int(node, "border", 0);
	int css_bw = 0;
	uint32_t bc = 0xFF888888u, css_bc = 0;
	xmlChar *id = NULL, *cls = NULL, *sty = NULL;

	if (bw < 0)
		bw = 0;
	if (bw > 8)
		bw = 8;
	if (node) {
		id = xmlGetProp(node, (const xmlChar *)"id");
		cls = xmlGetProp(node, (const xmlChar *)"class");
		sty = xmlGetProp(node, (const xmlChar *)"style");
	}
	bgtk_css_border(g_html_css, "table",
			id ? (const char *)id : NULL,
			cls ? (const char *)cls : NULL,
			sty ? (const char *)sty : NULL, &css_bw, &css_bc);
	if (css_bw > bw)
		bw = css_bw;
	/* border=0 but stylesheet border-color still draws 1px */
	if (bw == 0 && css_bc)
		bw = 1;
	if (css_bc)
		bc = css_bc;
	if (id)
		xmlFree(id);
	if (cls)
		xmlFree(cls);
	if (sty)
		xmlFree(sty);
	*border_w = bw;
	*border_c = bc;
}

static int table_layout_is_fixed(xmlNode *node)
{
	xmlChar *id = NULL, *cls = NULL, *sty = NULL;
	int fixed;

	if (!node)
		return 0;
	id = xmlGetProp(node, (const xmlChar *)"id");
	cls = xmlGetProp(node, (const xmlChar *)"class");
	sty = xmlGetProp(node, (const xmlChar *)"style");
	fixed = bgtk_css_table_layout_fixed(
		g_html_css, "table", id ? (const char *)id : NULL,
		cls ? (const char *)cls : NULL,
		sty ? (const char *)sty : NULL);
	if (id)
		xmlFree(id);
	if (cls)
		xmlFree(cls);
	if (sty)
		xmlFree(sty);
	return fixed;
}

/*
 * <table> with colspan/rowspan: absolute grid of cell frames.
 * Occupancy map places cells; sizes equalize columns/rows then span.
 */
static struct BGTK_Widget *convert_table(struct BGTK_Context *ctx,
					 xmlNode *node, int avail_w)
{
	xmlNode **tr_nodes = NULL;
	int num_rows = 0, max_cols = 0, r, c, ncells = 0, i;
	int border_w = 0;
	uint32_t border_c = 0xFF888888u;
	int *col_w, *row_h, *occ;
	struct {
		xmlNode *node;
		int r, c, cs, rs;
		struct BGTK_Widget *w;
	} *placed = NULL;
	int placed_n = 0, placed_cap = 0;
	struct BGTK_Widget **grid_items = NULL;
	struct BGTK_Widget *grid;
	int total_w = 0, total_h = 0, gi = 0;

	if (collect_rows(node, &tr_nodes, &num_rows) < 0 || num_rows == 0) {
		free(tr_nodes);
		return NULL;
	}
	table_border_style(node, &border_w, &border_c);

	/* First pass: place cells with colspan/rowspan into occupancy. */
	/* Cap columns high enough for colspan growth. */
	max_cols = 32;
	occ = calloc((size_t)num_rows * (size_t)max_cols, sizeof(int));
	if (!occ) {
		free(tr_nodes);
		return NULL;
	}
	/* occ[r*max_cols+c] = 1 if taken */
	for (r = 0; r < num_rows; r++) {
		c = 0;
		for (xmlNode *ch = tr_nodes[r]->children; ch; ch = ch->next) {
			const char *tn;
			int cs, rs, cc, rr;

			if (ch->type != XML_ELEMENT_NODE)
				continue;
			tn = (const char *)ch->name;
			if (strcmp(tn, "td") != 0 && strcmp(tn, "th") != 0)
				continue;
			while (c < max_cols && occ[r * max_cols + c])
				c++;
			if (c >= max_cols)
				break;
			cs = attr_int(ch, "colspan", 1);
			rs = attr_int(ch, "rowspan", 1);
			if (cs < 1)
				cs = 1;
			if (rs < 1)
				rs = 1;
			if (c + cs > max_cols)
				cs = max_cols - c;
			if (r + rs > num_rows)
				rs = num_rows - r;
			if (placed_n >= placed_cap) {
				placed_cap = placed_cap ? placed_cap * 2 : 16;
				placed = realloc(placed,
						 (size_t)placed_cap *
							 sizeof(*placed));
			}
			placed[placed_n].node = ch;
			placed[placed_n].r = r;
			placed[placed_n].c = c;
			placed[placed_n].cs = cs;
			placed[placed_n].rs = rs;
			placed[placed_n].w = NULL;
			placed_n++;
			for (rr = r; rr < r + rs; rr++)
				for (cc = c; cc < c + cs; cc++)
					occ[rr * max_cols + cc] = 1;
			c += cs;
		}
	}
	/* Shrink max_cols to highest used column+1 */
	{
		int used = 0;

		for (r = 0; r < num_rows; r++)
			for (c = 0; c < max_cols; c++)
				if (occ[r * max_cols + c] && c + 1 > used)
					used = c + 1;
		if (used < 1)
			used = 1;
		max_cols = used;
	}

	/* Convert cell widgets with provisional width. */
	for (i = 0; i < placed_n; i++) {
		int cw = avail_w / (max_cols > 0 ? max_cols : 1);
		int is_th = strcmp((const char *)placed[i].node->name, "th") ==
			    0;

		if (cw < 40)
			cw = 40;
		cw *= placed[i].cs;
		placed[i].w = convert_cell(ctx, placed[i].node, cw, is_th,
					   border_w, border_c);
		if (placed[i].w)
			ncells++;
	}
	if (ncells < 1) {
		free(occ);
		free(placed);
		free(tr_nodes);
		return NULL;
	}

	col_w = calloc((size_t)max_cols, sizeof(int));
	row_h = calloc((size_t)num_rows, sizeof(int));
	/* Natural size → column/row bases (colspan distributes width). */
	for (i = 0; i < placed_n; i++) {
		int cs = placed[i].cs, rs = placed[i].rs;
		int nw, nh, part, rem, k;

		if (!placed[i].w)
			continue;
		nw = placed[i].w->w;
		nh = placed[i].w->h;
		part = nw / cs;
		rem = nw % cs;
		for (k = 0; k < cs; k++) {
			int need = part + (k < rem ? 1 : 0);

			if (need > col_w[placed[i].c + k])
				col_w[placed[i].c + k] = need;
		}
		part = nh / rs;
		rem = nh % rs;
		for (k = 0; k < rs; k++) {
			int need = part + (k < rem ? 1 : 0);

			if (need > row_h[placed[i].r + k])
				row_h[placed[i].r + k] = need;
		}
	}
	/* Minimum column width */
	for (c = 0; c < max_cols; c++)
		if (col_w[c] < 24)
			col_w[c] = 24;
	for (r = 0; r < num_rows; r++)
		if (row_h[r] < 16)
			row_h[r] = 16;

	total_w = 0;
	for (c = 0; c < max_cols; c++)
		total_w += col_w[c];

	/*
	 * table-layout:fixed → equal columns.
	 * Width is content-sized unless CSS width is set (px or % of avail).
	 * Do not stretch to the full page when width is auto (terminal.pink).
	 */
	if (table_layout_is_fixed(node) && max_cols > 0) {
		int target = total_w;
		int css_w = -1, css_max = -1;
		int each, rem, acc;

		html_style_widget(node, NULL, &css_w, &css_max);
		if (css_w >= 100000) {
			int pct = css_w - 100000;

			if (pct > 0 && pct <= 100 && avail_w > 0)
				target = avail_w * pct / 100;
		} else if (css_w > 0) {
			target = css_w;
		}
		/* Still respect max-width. */
		if (css_max >= 100000) {
			int pct = css_max - 100000;
			int lim = avail_w * pct / 100;

			if (lim > 0 && target > lim)
				target = lim;
		} else if (css_max > 0 && target > css_max) {
			target = css_max;
		}
		if (target < max_cols * 16)
			target = max_cols * 16;
		each = target / max_cols;
		rem = target % max_cols;
		if (each < 16)
			each = 16;
		acc = 0;
		for (c = 0; c < max_cols; c++) {
			col_w[c] = each + (c < rem ? 1 : 0);
			acc += col_w[c];
		}
		total_w = acc;
	}

	/* Shrink only if wider than the page. */
	if (total_w > avail_w && avail_w > 40 && total_w > 0) {
		int acc = 0;

		for (c = 0; c < max_cols; c++) {
			col_w[c] = col_w[c] * avail_w / total_w;
			if (col_w[c] < 16)
				col_w[c] = 16;
			acc += col_w[c];
		}
		total_w = acc;
	}
	total_h = 0;
	for (r = 0; r < num_rows; r++)
		total_h += row_h[r];

	/* Size each cell to its span and set x,y offsets. */
	grid_items = calloc((size_t)placed_n, sizeof(*grid_items));
	for (i = 0; i < placed_n; i++) {
		int x = 0, y = 0, ww = 0, hh = 0, k;

		if (!placed[i].w)
			continue;
		for (k = 0; k < placed[i].c; k++)
			x += col_w[k];
		for (k = 0; k < placed[i].r; k++)
			y += row_h[k];
		for (k = 0; k < placed[i].cs; k++)
			ww += col_w[placed[i].c + k];
		for (k = 0; k < placed[i].rs; k++)
			hh += row_h[placed[i].r + k];
		placed[i].w->w = ww;
		placed[i].w->h = hh;
		placed[i].w->x = x;
		placed[i].w->y = y;
		/* Expand framed content child to fill cell. */
		if (placed[i].w->type == BGTK_WIDGET_FRAME &&
		    placed[i].w->data.frame.child) {
			struct BGTK_Widget *ch = placed[i].w->data.frame.child;
			int bw = placed[i].w->data.frame.border_w;
			int pad = placed[i].w->padding;

			if (bw < 0)
				bw = 0;
			ch->w = ww - 2 * (pad + bw);
			ch->h = hh - 2 * (pad + bw);
			if (ch->w < 1)
				ch->w = 1;
			if (ch->h < 1)
				ch->h = 1;
		}
		grid_items[gi++] = placed[i].w;
	}

	grid = bgtk_list(ctx, grid_items, gi,
			 (BGTK_Options){.orientation = BGTK_LIST_VERTICAL,
					.margin = 0,
					.padding = 0});
	free(grid_items);
	if (grid) {
		grid->flags |= BGTK_FLAG_ABSOLUTE;
		grid->w = total_w;
		grid->h = total_h;
		grid->data.list_widget.content_width = total_w;
		grid->data.list_widget.content_height = total_h;
	}

	free(occ);
	free(placed);
	free(col_w);
	free(row_h);
	free(tr_nodes);
	return grid;
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

	/* display:none + peek width/max-width for child layout. */
	{
		int peek_w = -1, peek_max = -1;

		if (html_style_widget(node, NULL, &peek_w, &peek_max))
			return NULL;
		if (peek_w > 0 || peek_max > 0) {
			int lim = clamp_wrap_w(avail_w, peek_w, peek_max, 0, 0);

			if (lim > 0 && lim < avail_w)
				avail_w = lim;
		}
	}

	if (strcmp(tag, "h1") == 0)
		w = convert_heading(ctx, node, 1, avail_w);
	else if (strcmp(tag, "h2") == 0)
		w = convert_heading(ctx, node, 2, avail_w);
	else if (strcmp(tag, "h3") == 0)
		w = convert_heading(ctx, node, 3, avail_w);
	else if (strcmp(tag, "h4") == 0)
		w = convert_heading(ctx, node, 3, avail_w);
	else if (strcmp(tag, "h5") == 0)
		w = convert_heading(ctx, node, 3, avail_w);
	else if (strcmp(tag, "h6") == 0)
		w = convert_heading(ctx, node, 3, avail_w);
	else if (strcmp(tag, "p") == 0)
		w = convert_p(ctx, node, avail_w);
	else if (strcmp(tag, "b") == 0)
		w = convert_bold(ctx, node);
	else if (strcmp(tag, "strong") == 0)
		w = convert_bold(ctx, node);
	else if (strcmp(tag, "i") == 0)
		w = convert_italic(ctx, node);
	else if (strcmp(tag, "em") == 0)
		w = convert_italic(ctx, node);
	else if (strcmp(tag, "a") == 0)
		w = convert_a(ctx, node);
	else if (strcmp(tag, "font") == 0)
		w = convert_font(ctx, node);
	else if (strcmp(tag, "code") == 0 || strcmp(tag, "kbd") == 0)
		w = convert_code(ctx, node);
	else if (strcmp(tag, "pre") == 0)
		w = convert_pre(ctx, node, avail_w);
	else if (strcmp(tag, "br") == 0)
		w = convert_br(ctx);
	else if (strcmp(tag, "hr") == 0)
		w = bgtk_rule(ctx, BGTK_LIST_HORIZONTAL, 1,
			      (BGTK_Options){.margin = 4, .padding = 0});
	else if (strcmp(tag, "span") == 0 || strcmp(tag, "diff") == 0) {
		char *txt = collect_text(node);

		if (txt && *txt)
			w = bgtk_text(ctx, txt, (BGTK_Options){0});
		free(txt);
	} else if (strcmp(tag, "center") == 0)
		w = convert_center(ctx, node, avail_w);
	else if (strcmp(tag, "button") == 0)
		w = convert_button(ctx, node);
	else if (strcmp(tag, "input") == 0)
		w = convert_input(ctx, node);
	else if (strcmp(tag, "select") == 0)
		w = convert_select(ctx, node);
	else if (strcmp(tag, "img") == 0)
		w = convert_img(ctx, node, avail_w);
	else if (strcmp(tag, "ul") == 0)
		w = convert_list(ctx, node, avail_w, 0);
	else if (strcmp(tag, "ol") == 0)
		w = convert_list(ctx, node, avail_w, 1);
	else if (strcmp(tag, "table") == 0)
		w = convert_table(ctx, node, avail_w);
	else
		/* div, section, article, body, htop, dd, article, … */
		w = convert_container(ctx, node, avail_w);

	if (w) {
		int css_w = -1, css_max = -1;
		int attr_w = attr_int(node, "width", 0);

		if (html_style_widget(node, w, &css_w, &css_max)) {
			w = NULL;
		} else if (w->type == BGTK_WIDGET_TEXT) {
			int padm = 2 * (w->padding + w->margin);
			int lim = clamp_wrap_w(avail_w, css_w, css_max, attr_w,
					       padm);

			wrap_text_widget(ctx, w, lim);
		} else if (w->type == BGTK_WIDGET_IMAGE) {
			int lim = css_len_px(css_max, avail_w);
			int cw = css_len_px(css_w, avail_w);

			if (cw > 0 && (lim < 0 || cw < lim))
				lim = cw;
			if (lim > 0 && w->data.image.img_w > lim) {
				int nh = (int)((long)w->data.image.img_h * lim /
					       w->data.image.img_w);
				uint32_t *sc = scale_image_argb(
					w->data.image.pixels,
					w->data.image.img_w,
					w->data.image.img_h, lim,
					nh > 0 ? nh : 1);

				if (sc) {
					w->data.image.pixels = sc;
					w->data.image.img_w = lim;
					w->data.image.img_h = nh > 0 ? nh : 1;
					w->w = lim +
					       2 * (w->padding + w->margin);
					w->h = w->data.image.img_h +
					       2 * (w->padding + w->margin);
				}
			}
		}
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
