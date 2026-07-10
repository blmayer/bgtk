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
	"<p><b>Bold text</b> and <i>italic text</i> and <a href=\"#\">a link</a>.</p>"
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

int main(void)
{
	struct BGTK_Context *ctx = bgtk_init_mock(480, 600);
	if (!ctx) {
		fprintf(stderr, "test_html: init failed\n");
		return 1;
	}

	/* Parse inline HTML string */
	struct BGTK_Widget *root = bgtk_html_parse_inline(ctx, SAMPLE_HTML, 480, 600);
	if (!root) {
		fprintf(stderr, "test_html: parse failed\n");
		bgtk_destroy_mock(ctx);
		return 1;
	}

	ctx->root_widget = root;
	bgtk_draw_widgets(ctx);
	take_screenshot(ctx, "test_html_00_init.png");

	printf("test_html complete. PNG frames written.\n");
	bgtk_destroy_mock(ctx);
	return 0;
}
