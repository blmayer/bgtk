/* Headless test for the HTML widget parser.
 *
 * Build:  make test_html
 * Run:    ./test_html
 *
 * Produces test/screenshots/test_html_*.png screenshots for visual inspection.
 */

#include <stdio.h>
#include <string.h>
#include <linux/input.h>

#include "bgtk.h"
#include "html.h"

static const char *SAMPLE_HTML =
	"<html><body>"
	"<h1>Hello BGTK</h1>"
	"<h2>Subheading</h2>"
	"<p>This is a paragraph of text rendered from HTML.</p>"
	"<p><b>Bold text</b> and <i>italic text</i> and "
	"<a href=\"https://example.com/\">a link</a>.</p>"
	"<h2>Pre / code</h2>"
	"<p>Inline <code>code()</code> and a block:</p>"
	"<pre>line one\n"
	"  indented\n"
	"line three</pre>"
	"<pre>see <a href=\"https://example.com/\">example</a> inside pre</pre>"
	"<button>Click Me</button>"
	"<input type=\"text\" value=\"type here\" width=\"200\" />"
	"<input type=\"checkbox\" /> Check this"
	"<ul>"
	"  <li>First item</li>"
	"  <li>Second item</li>"
	"  <li>Third item</li>"
	"</ul>"
	"<ol>"
	"  <li>One</li>"
	"  <li>Two</li>"
	"  <li>Three</li>"
	"</ol>"
	"<select>"
	"  <option>Option A</option>"
	"  <option>Option B</option>"
	"  <option>Option C</option>"
	"</select>"
	"<table>"
	"  <thead><tr><th>Name</th><th>Age</th><th>City</th></tr></thead>"
	"  <tbody>"
	"    <tr><td>Alice</td><td>30</td><td>New York</td></tr>"
	"    <tr><td>Bob</td><td>25</td><td>London</td></tr>"
	"    <tr><td>Charlie</td><td>35</td><td>Tokyo</td></tr>"
	"  </tbody>"
	"</table>"
	"</body></html>";

/* Walk tree for first text with href; also count multi-line mono (pre). */
static struct BGTK_Widget *find_href(struct BGTK_Widget *w)
{
	int i;
	struct BGTK_Widget *f;

	if (!w)
		return NULL;
	if (w->type == BGTK_WIDGET_TEXT && w->data.text.href &&
	    w->data.text.href[0])
		return w;
	switch (w->type) {
	case BGTK_WIDGET_SCROLLABLE:
		for (i = 0; i < w->data.scrollable.widget_count; i++) {
			f = find_href(w->data.scrollable.items[i]);
			if (f)
				return f;
		}
		break;
	case BGTK_WIDGET_LIST:
		for (i = 0; i < w->data.list_widget.widget_count; i++) {
			f = find_href(w->data.list_widget.items[i]);
			if (f)
				return f;
		}
		break;
	case BGTK_WIDGET_FRAME:
		return find_href(w->data.frame.child);
	case BGTK_WIDGET_BUTTON:
		return find_href(w->data.button.label);
	default:
		break;
	}
	return NULL;
}

static int count_pre_like(struct BGTK_Widget *w)
{
	int i, n = 0;

	if (!w)
		return 0;
	if (w->type == BGTK_WIDGET_TEXT &&
	    w->data.text.font_role == BGTK_FONT_MONO && w->data.text.text &&
	    strchr(w->data.text.text, '\n'))
		n++;
	switch (w->type) {
	case BGTK_WIDGET_SCROLLABLE:
		for (i = 0; i < w->data.scrollable.widget_count; i++)
			n += count_pre_like(w->data.scrollable.items[i]);
		break;
	case BGTK_WIDGET_LIST:
		for (i = 0; i < w->data.list_widget.widget_count; i++)
			n += count_pre_like(w->data.list_widget.items[i]);
		break;
	case BGTK_WIDGET_FRAME:
		n += count_pre_like(w->data.frame.child);
		break;
	default:
		break;
	}
	return n;
}

/* Anchor that lives under a mono pre line (nested markup). */
static int count_pre_links(struct BGTK_Widget *w)
{
	int i, n = 0;

	if (!w)
		return 0;
	if (w->type == BGTK_WIDGET_TEXT && w->data.text.href &&
	    w->data.text.href[0] &&
	    w->data.text.font_role == BGTK_FONT_MONO)
		n++;
	switch (w->type) {
	case BGTK_WIDGET_SCROLLABLE:
		for (i = 0; i < w->data.scrollable.widget_count; i++)
			n += count_pre_links(w->data.scrollable.items[i]);
		break;
	case BGTK_WIDGET_LIST:
		for (i = 0; i < w->data.list_widget.widget_count; i++)
			n += count_pre_links(w->data.list_widget.items[i]);
		break;
	case BGTK_WIDGET_FRAME:
		n += count_pre_links(w->data.frame.child);
		break;
	default:
		break;
	}
	return n;
}

/* CSS v1: style element + class/id + inline */
static const char *CSS_SAMPLE =
	"<html><head><style>"
	"body { background-color: #1a1a2e; color: #eaeaea; }"
	"h1 { color: #e94560; text-align: center; }"
	".note { color: #0f0; font-weight: bold; }"
	"#hideme { display: none; }"
	"p.lead { color: #4fc3f7; }"
	"</style></head><body>"
	"<h1>CSS styled page</h1>"
	"<p class=\"lead\">This paragraph uses class lead (sky blue).</p>"
	"<p class=\"note\">Bold green note.</p>"
	"<p id=\"hideme\">You should not see this (display:none).</p>"
	"<p style=\"color:#ffcc00;text-align:right\">Inline gold, right-aligned.</p>"
	"</body></html>";

/* Long line must wrap within a narrow max-width. */
static const char *WRAP_SAMPLE =
	"<html><body>"
	"<p style=\"max-width:140px\">"
	"AAAAAAAAAA BBBBBBBBBB CCCCCCCCCC DDDDDDDDDD EEEEEEEEEE "
	"FFFFFFFFFF GGGGGGGGGG"
	"</p>"
	"</body></html>";

int main(void)
{
	struct BGTK_Context *ctx = bgtk_init_mock(480, 600);
	struct BGTK_Widget *root;
	uint32_t *fb;
	int i, bright;

	if (!ctx) {
		fprintf(stderr, "test_html: init failed\n");
		return 1;
	}

	/* Parse inline HTML string */
	root = bgtk_html_parse_inline(ctx, SAMPLE_HTML, 480, 600);
	if (!root) {
		fprintf(stderr, "test_html: parse failed\n");
		bgtk_destroy_mock(ctx);
		return 1;
	}

	ctx->root_widget = root;
	bgtk_draw_widgets(ctx);
	take_screenshot(ctx, "test_html_00_init.png");

	{
		struct BGTK_Widget *link = find_href(root);

		if (!link || !link->data.text.href ||
		    strcmp(link->data.text.href, "https://example.com/") != 0) {
			fprintf(stderr,
				"test_html: <a href> not stored on text widget\n");
			bgtk_destroy_mock(ctx);
			return 1;
		}
		if (count_pre_like(root) < 1) {
			fprintf(stderr,
				"test_html: expected multi-line mono <pre>\n");
			bgtk_destroy_mock(ctx);
			return 1;
		}
		if (count_pre_links(root) < 1) {
			fprintf(stderr,
				"test_html: expected <a> inside <pre> "
				"(mono + href)\n");
			bgtk_destroy_mock(ctx);
			return 1;
		}
	}

	/* CSS sample */
	root = bgtk_html_parse_inline(ctx, CSS_SAMPLE, 480, 400);
	if (!root) {
		fprintf(stderr, "test_html: CSS parse failed\n");
		bgtk_destroy_mock(ctx);
		return 1;
	}
	/* Destroy previous tree */
	bgtk_widget_destroy(ctx->root_widget);
	ctx->root_widget = root;
	bgtk_draw_widgets(ctx);
	take_screenshot(ctx, "test_html_01_css.png");

	fb = (uint32_t *)ctx->shm_buffer;
	bright = 0;
	for (i = 0; i < 480 * 400; i++) {
		unsigned r = (fb[i] >> 16) & 0xFF;
		unsigned g = (fb[i] >> 8) & 0xFF;
		unsigned b = fb[i] & 0xFF;
		/* red-ish header, green note, or gold inline */
		if (r > 180 && g < 120)
			bright++;
		else if (g > 180 && r < 80)
			bright++;
		else if (r > 180 && g > 150 && b < 80)
			bright++;
	}
	if (bright < 20) {
		fprintf(stderr,
			"test_html: CSS colors not visible (hits=%d)\n", bright);
		bgtk_destroy_mock(ctx);
		return 1;
	}

	/* Image load from local path */
	{
		static const char *IMG_HTML =
			"<html><body><p><img src=\"test/fixtures_img.jpg\" "
			"width=\"120\" height=\"80\" alt=\"fixture\"></p>"
			"</body></html>";
		struct BGTK_Widget *walk;
		int found = 0;

		root = bgtk_html_parse_inline(ctx, IMG_HTML, 480, 200);
		if (!root) {
			fprintf(stderr, "test_html: img parse failed\n");
			bgtk_destroy_mock(ctx);
			return 1;
		}
		bgtk_widget_destroy(ctx->root_widget);
		ctx->root_widget = root;
		bgtk_draw_widgets(ctx);
		take_screenshot(ctx, "test_html_03_img.png");
		walk = root;
		if (walk->type == BGTK_WIDGET_FRAME)
			walk = walk->data.frame.child;
		if (walk && walk->type == BGTK_WIDGET_SCROLLABLE &&
		    walk->data.scrollable.widget_count > 0)
			walk = walk->data.scrollable.items[0];
		/* walk for IMAGE */
		{
			struct BGTK_Widget *stack[32];
			int sp = 0;

			stack[sp++] = walk;
			while (sp > 0) {
				struct BGTK_Widget *n = stack[--sp];
				int i;

				if (!n)
					continue;
				if (n->type == BGTK_WIDGET_IMAGE &&
				    n->data.image.pixels)
					found = 1;
				if (n->type == BGTK_WIDGET_LIST)
					for (i = 0; i < n->data.list_widget
							     .widget_count;
					     i++)
						if (sp < 32)
							stack[sp++] =
								n->data.list_widget
									.items[i];
				if (n->type == BGTK_WIDGET_FRAME && sp < 32)
					stack[sp++] = n->data.frame.child;
				if (n->type == BGTK_WIDGET_SCROLLABLE)
					for (i = 0; i < n->data.scrollable
							     .widget_count;
					     i++)
						if (sp < 32)
							stack[sp++] =
								n->data.scrollable
									.items[i];
			}
		}
		if (!found) {
			fprintf(stderr,
				"test_html: expected loaded <img> pixels\n");
			bgtk_destroy_mock(ctx);
			return 1;
		}
	}

	/* Wrap: multi-line text under max-width */
	{
		struct BGTK_Widget *tw = NULL;
		struct BGTK_Widget *walk;

		root = bgtk_html_parse_inline(ctx, WRAP_SAMPLE, 480, 200);
		if (!root) {
			fprintf(stderr, "test_html: wrap parse failed\n");
			bgtk_destroy_mock(ctx);
			return 1;
		}
		bgtk_widget_destroy(ctx->root_widget);
		ctx->root_widget = root;
		bgtk_draw_widgets(ctx);
		take_screenshot(ctx, "test_html_02_wrap.png");
		/* Find first multi-line text in tree (frame→scroll→list→text). */
		walk = root;
		if (walk->type == BGTK_WIDGET_FRAME)
			walk = walk->data.frame.child;
		if (walk && walk->type == BGTK_WIDGET_SCROLLABLE &&
		    walk->data.scrollable.widget_count > 0)
			walk = walk->data.scrollable.items[0];
		if (walk && walk->type == BGTK_WIDGET_LIST &&
		    walk->data.list_widget.widget_count > 0)
			walk = walk->data.list_widget.items[0];
		if (walk && walk->type == BGTK_WIDGET_TEXT)
			tw = walk;
		if (!tw || !tw->data.text.text ||
		    !strchr(tw->data.text.text, '\n')) {
			fprintf(stderr,
				"test_html: expected wrapped multi-line "
				"paragraph\n");
			bgtk_destroy_mock(ctx);
			return 1;
		}
	}

	/* Table: colspan/rowspan + border + margin:auto + border-color */
	{
		static const char *TABLE_HTML =
			"<html><head><style>"
			"table{margin:auto;table-layout:fixed;"
			"border-collapse:collapse;border-color:#ff00ff}"
			"td{padding:0 1em}"
			"</style></head><body>"
			"<table border=1>"
			"<tr><td colspan=4>wide top</td></tr>"
			"<tr><td colspan=2 rowspan=2>span TL</td>"
			"<td>a</td><td rowspan=2>span R</td></tr>"
			"<tr><td>b</td></tr>"
			"<tr><td colspan=3>bottom</td><td>c</td></tr>"
			"</table>"
			"</body></html>";
		struct BGTK_Widget *stack[64];
		int sp = 0, abs_lists = 0, margin_auto = 0, magenta = 0;
		int x, y;

		root = bgtk_html_parse_inline(ctx, TABLE_HTML, 480, 400);
		if (!root) {
			fprintf(stderr, "test_html: table parse failed\n");
			bgtk_destroy_mock(ctx);
			return 1;
		}
		bgtk_widget_destroy(ctx->root_widget);
		ctx->root_widget = root;
		bgtk_draw_widgets(ctx);
		take_screenshot(ctx, "test_html_04_table.png");

		stack[sp++] = root;
		while (sp > 0) {
			struct BGTK_Widget *n = stack[--sp];
			int i;

			if (!n)
				continue;
			if (n->flags & BGTK_FLAG_MARGIN_AUTO)
				margin_auto = 1;
			if (n->type == BGTK_WIDGET_LIST &&
			    (n->flags & BGTK_FLAG_ABSOLUTE))
				abs_lists++;
			if (n->type == BGTK_WIDGET_FRAME &&
			    n->data.frame.border_w > 0 &&
			    n->data.frame.border_color == 0xFFFF00FFu)
				magenta = 1;
			if (n->type == BGTK_WIDGET_LIST)
				for (i = 0; i < n->data.list_widget.widget_count;
				     i++)
					if (sp < 64)
						stack[sp++] =
							n->data.list_widget
								.items[i];
			if (n->type == BGTK_WIDGET_FRAME && n->data.frame.child &&
			    sp < 64)
				stack[sp++] = n->data.frame.child;
			if (n->type == BGTK_WIDGET_SCROLLABLE)
				for (i = 0; i < n->data.scrollable.widget_count;
				     i++)
					if (sp < 64)
						stack[sp++] =
							n->data.scrollable
								.items[i];
		}
		if (abs_lists < 1) {
			fprintf(stderr,
				"test_html: expected absolute table grid\n");
			bgtk_destroy_mock(ctx);
			return 1;
		}
		if (!margin_auto) {
			fprintf(stderr,
				"test_html: expected margin:auto on table\n");
			bgtk_destroy_mock(ctx);
			return 1;
		}
		if (!magenta) {
			fprintf(stderr,
				"test_html: expected magenta table border\n");
			bgtk_destroy_mock(ctx);
			return 1;
		}
		/* Magenta pixels should appear (border-color). */
		{
			int mag_px = 0;

			fb = (uint32_t *)ctx->shm_buffer;
			for (y = 0; y < 400 && !mag_px; y++) {
				for (x = 0; x < 480; x++) {
					uint32_t p = fb[y * 480 + x];
					unsigned r = (p >> 16) & 0xFF;
					unsigned g = (p >> 8) & 0xFF;
					unsigned b = p & 0xFF;

					if (r > 200 && b > 200 && g < 80) {
						mag_px = 1;
						break;
					}
				}
			}
			if (!mag_px) {
				fprintf(stderr,
					"test_html: magenta border not in "
					"framebuffer\n");
				bgtk_destroy_mock(ctx);
				return 1;
			}
		}
	}

	printf("test_html complete. PNG frames written.\n");
	bgtk_destroy_mock(ctx);
	return 0;
}
