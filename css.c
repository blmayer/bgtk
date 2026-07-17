/* css.c — minimal CSS parser/applier for BGTK HTML */
#include "css.h"
#include "config.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

enum {
	SEL_UNIVERSAL = 0,
	SEL_TYPE = 1,
	SEL_CLASS = 10,
	SEL_ID = 100,
};

struct BGTK_CSS_Decl {
	int color_set, bg_set;
	uint32_t color, bg;
	int font_weight; /* 0 unset, 1 normal, 2 bold */
	int font_style;  /* 0 unset, 1 normal, 2 italic */
	int text_align;  /* -1 unset, 0 left, 1 center, 2 right */
	int margin;      /* -1 unset */
	int margin_auto; /* 1 = margin:auto (or left/right auto) */
	int padding;     /* -1 unset */
	int width;       /* -1 unset, px */
	int max_width;   /* -1 unset, px */
	int border_w;    /* -1 unset */
	int border_color_set;
	uint32_t border_color;
	int table_layout_fixed; /* 1 = table-layout:fixed */
	int display_none;
};

struct BGTK_CSS_Rule {
	char sel_type[32];  /* empty = any */
	char sel_class[64]; /* empty = any */
	char sel_id[64];    /* empty = any */
	int specificity;
	struct BGTK_CSS_Decl decl;
};

struct BGTK_CSS {
	struct BGTK_CSS_Rule *rules;
	int n, cap;
};

struct BGTK_CSS *bgtk_css_create(void)
{
	return calloc(1, sizeof(struct BGTK_CSS));
}

void bgtk_css_destroy(struct BGTK_CSS *css)
{
	if (!css)
		return;
	free(css->rules);
	free(css);
}

static void skip_ws(const char **pp)
{
	const char *p = *pp;
	while (*p && isspace((unsigned char)*p))
		p++;
	*pp = p;
}

uint32_t bgtk_css_parse_color(const char *s)
{
	static const struct {
		const char *name;
		uint32_t c;
	} named[] = {
		{ "black", 0xFF000000 },   { "white", 0xFFFFFFFF },
		{ "red", 0xFFFF0000 },	   { "green", 0xFF008000 },
		{ "blue", 0xFF0000FF },	   { "yellow", 0xFFFFFF00 },
		{ "cyan", 0xFF00FFFF },	   { "magenta", 0xFFFF00FF },
		{ "gray", 0xFF808080 },	   { "grey", 0xFF808080 },
		{ "orange", 0xFFFFA500 },  { "purple", 0xFF800080 },
		{ "navy", 0xFF000080 },	   { "teal", 0xFF008080 },
		{ "silver", 0xFFC0C0C0 },  { "maroon", 0xFF800000 },
		{ "lime", 0xFF00FF00 },	   { "aqua", 0xFF00FFFF },
		{ "fuchsia", 0xFFFF00FF }, { "olive", 0xFF808000 },
		{ "transparent", 0 },
	};
	char buf[64];
	size_t i, n;

	if (!s || !*s)
		return 0;
	skip_ws(&s);
	if (*s == '#') {
		uint32_t c = parse_hex_color(s);
		/* parse_hex_color returns opaque black on bad input; accept #000 */
		if (s[0] == '#' && (strlen(s) == 4 || strlen(s) == 7 ||
				    strlen(s) == 9)) {
			if (strlen(s) == 4) {
				/* #rgb → #rrggbb */
				unsigned r, g, b;
				if (sscanf(s + 1, "%1x%1x%1x", &r, &g, &b) ==
				    3) {
					r |= r << 4;
					g |= g << 4;
					b |= b << 4;
					return 0xFF000000u | (r << 16) |
					       (g << 8) | b;
				}
			}
			return c;
		}
		return 0;
	}
	n = 0;
	while (s[n] && (isalnum((unsigned char)s[n]) || s[n] == '-') &&
	       n + 1 < sizeof(buf)) {
		buf[n] = (char)tolower((unsigned char)s[n]);
		n++;
	}
	buf[n] = '\0';
	for (i = 0; i < sizeof(named) / sizeof(named[0]); i++) {
		if (strcmp(buf, named[i].name) == 0)
			return named[i].c;
	}
	return 0;
}

/* Parse length: 12, 12px, 1.5em, 2rem, 50% (of ref_pct, default 0 → -1). */
static int parse_len(const char *v, int em_px, int pct_of)
{
	char *end;
	double n;

	if (!v)
		return -1;
	skip_ws(&v);
	if (*v == 'a' || *v == 'A') {
		/* auto */
		if (strncasecmp(v, "auto", 4) == 0)
			return -2; /* special: auto */
	}
	n = strtod(v, &end);
	if (end == v)
		return -1;
	while (*end && isspace((unsigned char)*end))
		end++;
	if (*end == '%') {
		if (pct_of <= 0)
			return -1;
		n = n * pct_of / 100.0;
	} else if ((*end == 'e' || *end == 'E') &&
		   (end[1] == 'm' || end[1] == 'M')) {
		n *= em_px > 0 ? em_px : 14;
	} else if ((*end == 'r' || *end == 'R') &&
		   (end[1] == 'e' || end[1] == 'E')) {
		/* rem */
		n *= em_px > 0 ? em_px : 14;
	} else {
		/* px or unitless */
		if (*end == 'p' || *end == 'P')
			end++;
		/* x optional */
	}
	if (n < 0)
		n = 0;
	if (n > 10000)
		n = 10000;
	return (int)(n + 0.5);
}

static int parse_px(const char *v)
{
	return parse_len(v, 14, 0);
}

/* Split CSS value into up to 4 whitespace-separated tokens. */
static int split_tokens(const char *v, char tok[][32], int max_tok)
{
	int n = 0;
	const char *p = v;

	while (*p && n < max_tok) {
		size_t i = 0;

		skip_ws(&p);
		if (!*p)
			break;
		while (*p && !isspace((unsigned char)*p) && i + 1 < 32)
			tok[n][i++] = *p++;
		tok[n][i] = '\0';
		if (i > 0)
			n++;
	}
	return n;
}

/* margin: N | V H | T H B | T R B L ; auto → margin_auto. */
static void parse_margin_shorthand(struct BGTK_CSS_Decl *d, const char *v)
{
	char tok[4][32];
	int n = split_tokens(v, tok, 4);
	int sides[4] = { -1, -1, -1, -1 }; /* T R B L; -2 = auto */
	int i, vert = -1, horiz = -1;

	if (n < 1)
		return;
	for (i = 0; i < n; i++) {
		if (strcasecmp(tok[i], "auto") == 0)
			sides[i] = -2;
		else
			sides[i] = parse_len(tok[i], 14, 0);
	}
	if (n == 1) {
		vert = sides[0];
		horiz = sides[0];
	} else if (n == 2) {
		vert = sides[0];
		horiz = sides[1];
	} else if (n == 3) {
		vert = sides[0] >= 0 ? sides[0] : sides[2];
		if (sides[0] >= 0 && sides[2] >= 0 && sides[2] > vert)
			vert = sides[2];
		horiz = sides[1];
	} else {
		vert = sides[0] >= 0 ? sides[0] : -1;
		if (sides[2] >= 0 && (vert < 0 || sides[2] > vert))
			vert = sides[2];
		horiz = sides[1] >= 0 ? sides[1] : sides[3];
		if (sides[1] == -2 || sides[3] == -2)
			horiz = -2;
	}
	if (horiz == -2 || vert == -2 ||
	    (n == 1 && sides[0] == -2))
		d->margin_auto = 1;
	if (vert >= 0)
		d->margin = vert;
	else if (horiz >= 0 && !d->margin_auto)
		d->margin = horiz;
}

/* padding multi-value: use horizontal (or single) for BGTK's one padding. */
static void parse_padding_shorthand(struct BGTK_CSS_Decl *d, const char *v)
{
	char tok[4][32];
	int n = split_tokens(v, tok, 4);
	int px = -1;

	if (n == 1)
		px = parse_len(tok[0], 14, 0);
	else if (n == 2)
		px = parse_len(tok[1], 14, 0); /* horizontal */
	else if (n >= 3)
		px = parse_len(tok[1], 14, 0);
	if (px >= 0)
		d->padding = px;
}

/* border / border-width / border-color helpers */
static void parse_border_shorthand(struct BGTK_CSS_Decl *d, const char *v)
{
	char tok[8][32];
	int n = split_tokens(v, tok, 8);
	int i;

	for (i = 0; i < n; i++) {
		int px;
		uint32_t c;

		if (strcasecmp(tok[i], "none") == 0 ||
		    strcasecmp(tok[i], "hidden") == 0) {
			d->border_w = 0;
			continue;
		}
		if (strcasecmp(tok[i], "solid") == 0 ||
		    strcasecmp(tok[i], "dashed") == 0 ||
		    strcasecmp(tok[i], "dotted") == 0 ||
		    strcasecmp(tok[i], "double") == 0)
			continue;
		px = parse_len(tok[i], 14, 0);
		if (px >= 0 && (strchr(tok[i], 'p') || strchr(tok[i], 'e') ||
				isdigit((unsigned char)tok[i][0]))) {
			if (px > 16)
				px = 16;
			d->border_w = px;
			continue;
		}
		c = bgtk_css_parse_color(tok[i]);
		if (c) {
			d->border_color = c;
			d->border_color_set = 1;
		}
	}
	/* color without width still draws a 1px edge */
	if (d->border_color_set && d->border_w < 0)
		d->border_w = 1;
}

static void decl_clear(struct BGTK_CSS_Decl *d)
{
	memset(d, 0, sizeof(*d));
	d->text_align = -1;
	d->margin = -1;
	d->padding = -1;
	d->width = -1;
	d->max_width = -1;
	d->border_w = -1;
}

static void decl_set_prop(struct BGTK_CSS_Decl *d, const char *prop,
			  const char *val)
{
	char p[64], v[128];
	size_t i;

	if (!d || !prop || !val)
		return;
	for (i = 0; prop[i] && i + 1 < sizeof(p); i++)
		p[i] = (char)tolower((unsigned char)prop[i]);
	p[i] = '\0';
	/* trim val */
	while (*val && isspace((unsigned char)*val))
		val++;
	{
		size_t len = strlen(val);
		while (len > 0 && isspace((unsigned char)val[len - 1]))
			len--;
		if (len >= sizeof(v))
			len = sizeof(v) - 1;
		memcpy(v, val, len);
		v[len] = '\0';
	}

	if (strcmp(p, "color") == 0) {
		uint32_t c = bgtk_css_parse_color(v);
		if (c || strcasecmp(v, "transparent") == 0 ||
		    strcasecmp(v, "black") == 0) {
			d->color = c ? c : 0xFF000000u;
			if (strcasecmp(v, "transparent") == 0)
				d->color = 0;
			d->color_set = 1;
		}
	} else if (strcmp(p, "background-color") == 0 ||
		   strcmp(p, "background") == 0) {
		/* background: #hex [ignore rest] */
		char tok[64];
		const char *q = v;
		size_t n = 0;
		while (*q && !isspace((unsigned char)*q) && n + 1 < sizeof(tok))
			tok[n++] = *q++;
		tok[n] = '\0';
		{
			uint32_t c = bgtk_css_parse_color(tok);
			if (c || strcasecmp(tok, "transparent") == 0 ||
			    strcasecmp(tok, "black") == 0) {
				d->bg = c;
				if (strcasecmp(tok, "black") == 0 && !c)
					d->bg = 0xFF000000u;
				d->bg_set = 1;
			}
		}
	} else if (strcmp(p, "font-weight") == 0) {
		if (strcasecmp(v, "bold") == 0 || strcmp(v, "700") == 0 ||
		    strcmp(v, "800") == 0 || strcmp(v, "900") == 0)
			d->font_weight = 2;
		else if (strcasecmp(v, "normal") == 0 || strcmp(v, "400") == 0)
			d->font_weight = 1;
	} else if (strcmp(p, "font-style") == 0) {
		if (strcasecmp(v, "italic") == 0 ||
		    strcasecmp(v, "oblique") == 0)
			d->font_style = 2;
		else if (strcasecmp(v, "normal") == 0)
			d->font_style = 1;
	} else if (strcmp(p, "text-align") == 0) {
		if (strcasecmp(v, "left") == 0)
			d->text_align = 0;
		else if (strcasecmp(v, "center") == 0)
			d->text_align = 1;
		else if (strcasecmp(v, "right") == 0)
			d->text_align = 2;
	} else if (strcmp(p, "margin") == 0) {
		parse_margin_shorthand(d, v);
	} else if (strcmp(p, "margin-left") == 0 ||
		   strcmp(p, "margin-right") == 0) {
		if (strcasecmp(v, "auto") == 0)
			d->margin_auto = 1;
		else {
			int px = parse_px(v);

			if (px >= 0 && (d->margin < 0 || px > d->margin))
				d->margin = px;
		}
	} else if (strcmp(p, "margin-top") == 0 ||
		   strcmp(p, "margin-bottom") == 0) {
		int px = parse_px(v);

		if (px >= 0 && (d->margin < 0 || px > d->margin))
			d->margin = px;
	} else if (strcmp(p, "padding") == 0) {
		parse_padding_shorthand(d, v);
	} else if (strcmp(p, "padding-left") == 0 ||
		   strcmp(p, "padding-right") == 0 ||
		   strcmp(p, "padding-top") == 0 ||
		   strcmp(p, "padding-bottom") == 0) {
		int px = parse_px(v);

		if (px >= 0 && (d->padding < 0 || px > d->padding))
			d->padding = px;
	} else if (strcmp(p, "border") == 0) {
		parse_border_shorthand(d, v);
	} else if (strcmp(p, "border-width") == 0) {
		int px = parse_px(v);

		if (px >= 0) {
			if (px > 16)
				px = 16;
			d->border_w = px;
		}
	} else if (strcmp(p, "border-color") == 0) {
		uint32_t c = bgtk_css_parse_color(v);

		if (c) {
			d->border_color = c;
			d->border_color_set = 1;
			if (d->border_w < 0)
				d->border_w = 1;
		}
	} else if (strcmp(p, "table-layout") == 0) {
		if (strcasecmp(v, "fixed") == 0)
			d->table_layout_fixed = 1;
	} else if (strcmp(p, "width") == 0) {
		int px = parse_len(v, 14, 0);
		/* % widths handled later with parent; bare % → treat as 0 flag */
		if (strchr(v, '%')) {
			/* Store percent as 100000+pct so apply can expand */
			char *end;
			long pct = strtol(v, &end, 10);

			if (pct > 0 && pct <= 100)
				d->width = 100000 + (int)pct;
		} else if (px >= 0) {
			d->width = px;
		}
	} else if (strcmp(p, "max-width") == 0) {
		int px = parse_len(v, 14, 0);

		if (strchr(v, '%')) {
			char *end;
			long pct = strtol(v, &end, 10);

			if (pct > 0 && pct <= 100)
				d->max_width = 100000 + (int)pct;
		} else if (px >= 0) {
			d->max_width = px;
		}
	} else if (strcmp(p, "display") == 0) {
		if (strcasecmp(v, "none") == 0)
			d->display_none = 1;
	}
}

static void parse_declarations(const char *block, struct BGTK_CSS_Decl *d)
{
	const char *p = block;

	decl_clear(d);
	while (*p) {
		char prop[64], val[128];
		size_t n;
		const char *colon, *semi, *end;

		skip_ws(&p);
		if (!*p || *p == '}')
			break;
		colon = strchr(p, ':');
		if (!colon)
			break;
		n = 0;
		while (p < colon && n + 1 < sizeof(prop)) {
			if (!isspace((unsigned char)*p))
				prop[n++] = *p;
			p++;
		}
		prop[n] = '\0';
		p = colon + 1;
		skip_ws(&p);
		semi = strchr(p, ';');
		end = semi ? semi : p + strlen(p);
		n = 0;
		while (p < end && n + 1 < sizeof(val))
			val[n++] = *p++;
		val[n] = '\0';
		/* trim trailing space */
		while (n > 0 && isspace((unsigned char)val[n - 1]))
			val[--n] = '\0';
		decl_set_prop(d, prop, val);
		if (semi)
			p = semi + 1;
		else
			break;
	}
}

static int parse_selector(const char *sel, struct BGTK_CSS_Rule *r)
{
	const char *p = sel;
	size_t n;

	r->sel_type[0] = r->sel_class[0] = r->sel_id[0] = '\0';
	r->specificity = 0;
	skip_ws(&p);
	if (!*p)
		return -1;
	if (*p == '*') {
		r->specificity = SEL_UNIVERSAL;
		return 0;
	}
	/* type */
	if (isalpha((unsigned char)*p)) {
		n = 0;
		while (*p && (isalnum((unsigned char)*p) || *p == '-') &&
		       n + 1 < sizeof(r->sel_type))
			r->sel_type[n++] = (char)tolower((unsigned char)*p++);
		r->sel_type[n] = '\0';
		r->specificity += SEL_TYPE;
	}
	while (*p == '.' || *p == '#') {
		int is_id = (*p == '#');
		char *dst = is_id ? r->sel_id : r->sel_class;
		size_t max = is_id ? sizeof(r->sel_id) : sizeof(r->sel_class);

		p++;
		n = 0;
		while (*p && (isalnum((unsigned char)*p) || *p == '-' ||
			      *p == '_') &&
		       n + 1 < max)
			dst[n++] = *p++;
		dst[n] = '\0';
		r->specificity += is_id ? SEL_ID : SEL_CLASS;
	}
	skip_ws(&p);
	/* reject complex selectors for v1 */
	if (*p)
		return -1;
	return 0;
}

static void css_add_rule(struct BGTK_CSS *css, const struct BGTK_CSS_Rule *r)
{
	if (!css)
		return;
	if (css->n >= css->cap) {
		int ncap = css->cap ? css->cap * 2 : 16;
		struct BGTK_CSS_Rule *nr =
			realloc(css->rules, (size_t)ncap * sizeof(*nr));
		if (!nr)
			return;
		css->rules = nr;
		css->cap = ncap;
	}
	css->rules[css->n++] = *r;
}

void bgtk_css_add_sheet(struct BGTK_CSS *css, const char *source)
{
	const char *p;

	if (!css || !source)
		return;
	p = source;
	while (*p) {
		char selbuf[128], declbuf[512];
		const char *brace, *end;
		size_t n;
		struct BGTK_CSS_Rule rule;

		skip_ws(&p);
		if (!*p)
			break;
		/* skip comments */
		if (p[0] == '/' && p[1] == '*') {
			p += 2;
			while (*p && !(p[0] == '*' && p[1] == '/'))
				p++;
			if (*p)
				p += 2;
			continue;
		}
		/* @-rules: skip until next {…} or ; */
		if (*p == '@') {
			brace = strchr(p, '{');
			if (brace) {
				int depth = 1;
				p = brace + 1;
				while (*p && depth) {
					if (*p == '{')
						depth++;
					else if (*p == '}')
						depth--;
					p++;
				}
			} else {
				while (*p && *p != ';')
					p++;
				if (*p == ';')
					p++;
			}
			continue;
		}
		brace = strchr(p, '{');
		if (!brace)
			break;
		n = (size_t)(brace - p);
		while (n > 0 && isspace((unsigned char)p[n - 1]))
			n--;
		if (n >= sizeof(selbuf))
			n = sizeof(selbuf) - 1;
		memcpy(selbuf, p, n);
		selbuf[n] = '\0';
		p = brace + 1;
		end = strchr(p, '}');
		if (!end)
			break;
		n = (size_t)(end - p);
		if (n >= sizeof(declbuf))
			n = sizeof(declbuf) - 1;
		memcpy(declbuf, p, n);
		declbuf[n] = '\0';
		p = end + 1;

		/* split selectors on comma */
		{
			char *save = NULL;
			char *tok = strtok_r(selbuf, ",", &save);
			while (tok) {
				memset(&rule, 0, sizeof(rule));
				decl_clear(&rule.decl);
				if (parse_selector(tok, &rule) == 0) {
					parse_declarations(declbuf, &rule.decl);
					css_add_rule(css, &rule);
				}
				tok = strtok_r(NULL, ",", &save);
			}
		}
	}
}

static int class_list_has(const char *class_attr, const char *want)
{
	const char *p;
	size_t wlen;

	if (!class_attr || !want || !*want)
		return 0;
	wlen = strlen(want);
	p = class_attr;
	while (*p) {
		while (*p && isspace((unsigned char)*p))
			p++;
		if (!strncmp(p, want, wlen) &&
		    (p[wlen] == '\0' || isspace((unsigned char)p[wlen])))
			return 1;
		while (*p && !isspace((unsigned char)*p))
			p++;
	}
	return 0;
}

static int rule_matches(const struct BGTK_CSS_Rule *r, const char *tag,
			const char *id, const char *class_attr)
{
	if (r->sel_type[0] &&
	    (!tag || strcasecmp(r->sel_type, tag) != 0))
		return 0;
	if (r->sel_id[0] && (!id || strcmp(r->sel_id, id) != 0))
		return 0;
	if (r->sel_class[0] && !class_list_has(class_attr, r->sel_class))
		return 0;
	/* universal / empty selector matches everything */
	if (!r->sel_type[0] && !r->sel_id[0] && !r->sel_class[0] &&
	    r->specificity == 0)
		return 1;
	return 1;
}

static void decl_merge(struct BGTK_CSS_Decl *dst, const struct BGTK_CSS_Decl *src)
{
	if (src->color_set) {
		dst->color = src->color;
		dst->color_set = 1;
	}
	if (src->bg_set) {
		dst->bg = src->bg;
		dst->bg_set = 1;
	}
	if (src->font_weight)
		dst->font_weight = src->font_weight;
	if (src->font_style)
		dst->font_style = src->font_style;
	if (src->text_align >= 0)
		dst->text_align = src->text_align;
	if (src->margin >= 0)
		dst->margin = src->margin;
	if (src->margin_auto)
		dst->margin_auto = 1;
	if (src->padding >= 0)
		dst->padding = src->padding;
	if (src->width >= 0)
		dst->width = src->width;
	if (src->max_width >= 0)
		dst->max_width = src->max_width;
	if (src->border_w >= 0)
		dst->border_w = src->border_w;
	if (src->border_color_set) {
		dst->border_color = src->border_color;
		dst->border_color_set = 1;
	}
	if (src->table_layout_fixed)
		dst->table_layout_fixed = 1;
	if (src->display_none)
		dst->display_none = 1;
}

static void apply_decl_to_widget(struct BGTK_Widget *w,
				 const struct BGTK_CSS_Decl *d)
{
	if (!w || !d)
		return;
	if (d->color_set)
		w->color_fg = d->color ? d->color : 0xFF000000u;
	if (d->bg_set)
		w->color_bg = d->bg;
	if (d->margin >= 0)
		w->margin = d->margin;
	if (d->margin_auto)
		w->flags |= BGTK_FLAG_MARGIN_AUTO;
	if (d->padding >= 0)
		w->padding = d->padding;
	if (d->text_align == 0)
		w->text_align = BGTK_ALIGN_LEFT;
	else if (d->text_align == 1)
		w->text_align = BGTK_ALIGN_CENTER;
	else if (d->text_align == 2)
		w->text_align = BGTK_ALIGN_RIGHT;
	if (w->type == BGTK_WIDGET_TEXT) {
		if (d->font_weight == 2)
			w->data.text.style |= BGTK_TEXT_BOLD;
		else if (d->font_weight == 1)
			w->data.text.style &= ~BGTK_TEXT_BOLD;
		if (d->font_style == 2)
			w->data.text.style |= BGTK_TEXT_ITALIC;
		else if (d->font_style == 1)
			w->data.text.style &= ~BGTK_TEXT_ITALIC;
	}
	if (w->type == BGTK_WIDGET_BUTTON && d->bg_set && d->bg)
		w->data.button.bg_override = d->bg;
	if (w->type == BGTK_WIDGET_FRAME) {
		if (d->border_w >= 0)
			w->data.frame.border_w = d->border_w;
		if (d->border_color_set)
			w->data.frame.border_color = d->border_color;
	}
}

int bgtk_css_apply(struct BGTK_CSS *css, struct BGTK_Widget *w, const char *tag,
		   const char *id, const char *class_attr,
		   const char *style_attr, int *out_width, int *out_max_width)
{
	struct BGTK_CSS_Decl acc;
	int i, changed;

	if (out_width)
		*out_width = -1;
	if (out_max_width)
		*out_max_width = -1;
	decl_clear(&acc);
	if (css && css->n > 0) {
		/* Sort by specificity (stable for equals → later source wins
		 * if we use a stable bubble that only swaps on < ). */
		int *idx = malloc((size_t)css->n * sizeof(int));
		int a, b;

		if (idx) {
			for (i = 0; i < css->n; i++)
				idx[i] = i;
			for (a = 0; a < css->n; a++) {
				for (b = a + 1; b < css->n; b++) {
					int sa = css->rules[idx[a]].specificity;
					int sb = css->rules[idx[b]].specificity;
					/* Lower specificity first; equal keeps
					 * source order (idx[a] < idx[b]). */
					if (sb < sa ||
					    (sb == sa && idx[b] < idx[a])) {
						int t = idx[a];
						idx[a] = idx[b];
						idx[b] = t;
					}
				}
			}
			for (i = 0; i < css->n; i++) {
				const struct BGTK_CSS_Rule *r =
					&css->rules[idx[i]];
				if (rule_matches(r, tag, id, class_attr))
					decl_merge(&acc, &r->decl);
			}
			free(idx);
		}
	}
	if (style_attr && *style_attr) {
		struct BGTK_CSS_Decl inl;
		parse_declarations(style_attr, &inl);
		decl_merge(&acc, &inl);
	}
	if (acc.display_none)
		return 1;
	if (out_width)
		*out_width = acc.width;
	if (out_max_width)
		*out_max_width = acc.max_width;
	if (!w)
		return 0;
	changed = acc.color_set || acc.bg_set || acc.font_weight ||
		  acc.font_style || acc.text_align >= 0 || acc.margin >= 0 ||
		  acc.margin_auto || acc.padding >= 0 || acc.border_w >= 0 ||
		  acc.border_color_set;
	if (changed)
		apply_decl_to_widget(w, &acc);
	/* Image/box width from CSS (content width before margin). */
	if (acc.width >= 0 &&
	    (w->type == BGTK_WIDGET_IMAGE || w->type == BGTK_WIDGET_FRAME)) {
		int outer = acc.width + 2 * (w->padding + w->margin);

		if (outer > 0)
			w->w = outer;
	}
	return 0;
}

/* Cascade into acc for tag/id/class/inline (shared by apply + border helpers). */
static void css_resolve(struct BGTK_CSS *css, const char *tag, const char *id,
			const char *class_attr, const char *style_attr,
			struct BGTK_CSS_Decl *acc)
{
	int i;

	decl_clear(acc);
	if (css && css->n > 0) {
		int *idx = malloc((size_t)css->n * sizeof(int));
		int a, b;

		if (idx) {
			for (i = 0; i < css->n; i++)
				idx[i] = i;
			for (a = 0; a < css->n; a++) {
				for (b = a + 1; b < css->n; b++) {
					int sa = css->rules[idx[a]].specificity;
					int sb = css->rules[idx[b]].specificity;

					if (sb < sa ||
					    (sb == sa && idx[b] < idx[a])) {
						int t = idx[a];
						idx[a] = idx[b];
						idx[b] = t;
					}
				}
			}
			for (i = 0; i < css->n; i++) {
				const struct BGTK_CSS_Rule *r =
					&css->rules[idx[i]];
				if (rule_matches(r, tag, id, class_attr))
					decl_merge(acc, &r->decl);
			}
			free(idx);
		}
	}
	if (style_attr && *style_attr) {
		struct BGTK_CSS_Decl inl;

		parse_declarations(style_attr, &inl);
		decl_merge(acc, &inl);
	}
}

void bgtk_css_border(struct BGTK_CSS *css, const char *tag, const char *id,
		     const char *class_attr, const char *style_attr, int *bw,
		     uint32_t *bc)
{
	struct BGTK_CSS_Decl acc;

	if (bw)
		*bw = 0;
	if (bc)
		*bc = 0xFF888888u;
	css_resolve(css, tag, id, class_attr, style_attr, &acc);
	if (bw && acc.border_w >= 0)
		*bw = acc.border_w;
	if (bc && acc.border_color_set)
		*bc = acc.border_color;
	/* border-color alone implies 1px when attr may also set width */
	if (bw && acc.border_color_set && acc.border_w < 0 && *bw == 0)
		*bw = 1;
}

int bgtk_css_table_layout_fixed(struct BGTK_CSS *css, const char *tag,
				const char *id, const char *class_attr,
				const char *style_attr)
{
	struct BGTK_CSS_Decl acc;

	css_resolve(css, tag, id, class_attr, style_attr, &acc);
	return acc.table_layout_fixed;
}
