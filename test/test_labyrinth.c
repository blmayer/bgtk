/* test/test_labyrinth.c — headless screenshots for Labyrinth web browser.
 *
 * Build: make test_labyrinth
 * Run:   ./test_labyrinth
 */
#include <stdio.h>
#include <string.h>
#include <linux/input.h>
#include "bgtk.h"

void labyrinth_test_init(struct BGTK_Context *c, int w, int h);
void labyrinth_test_load(const char *url);
void labyrinth_test_load_html(const char *html);
const char *labyrinth_test_url(void);

static const char *SAMPLE =
	"<!DOCTYPE html><html><body>"
	"<h1>Labyrinth Test</h1>"
	"<p>Inline HTML rendered through <b>bgtk_html_parse_inline</b>.</p>"
	"<h2>List</h2>"
	"<ul><li>Alpha</li><li>Beta</li><li>Gamma</li></ul>"
	"<p><a href=\"http://example.com/\">Example link</a> (click wiring later).</p>"
	"</body></html>";

int main(void)
{
	int width = 720, height = 520;
	struct BGTK_Context *ctx;
	uint32_t *fb;
	uint32_t bg;
	int i, bright;
	struct InputEvent click = {0};

	ctx = bgtk_init_mock(width, height);
	if (!ctx) {
		fprintf(stderr, "test_labyrinth: init failed\n");
		return 1;
	}
	bgtk_log_open("test_labyrinth");
	labyrinth_test_init(ctx, width, height);

	/* 00: about:home */
	labyrinth_test_load("about:home");
	bgtk_draw_widgets(ctx);
	take_screenshot(ctx, "labyrinth_00_home.png");

	fb = (uint32_t *)ctx->shm_buffer;
	bg = ctx->theme.background | 0xFF000000u;
	bright = 0;
	for (i = 0; i < width * height; i++) {
		if (fb[i] != 0 && fb[i] != bg)
			bright++;
	}
	if (bright < 100) {
		fprintf(stderr, "test_labyrinth: home page empty (bright=%d)\n",
			bright);
		bgtk_destroy_mock(ctx);
		return 1;
	}
	if (strcmp(labyrinth_test_url(), "about:home") != 0) {
		fprintf(stderr, "test_labyrinth: url want about:home got %s\n",
			labyrinth_test_url());
		bgtk_destroy_mock(ctx);
		return 1;
	}

	/* 01: inline HTML sample */
	labyrinth_test_load_html(SAMPLE);
	take_screenshot(ctx, "labyrinth_01_inline_html.png");

	/* 02: focus URL bar (bottom) — click lower strip */
	click.type = EV_KEY;
	click.code = BTN_LEFT;
	click.value = 1;
	click.x = width / 2;
	click.y = height - 36;
	bgtk_inject_event(ctx, click);
	click.value = 0;
	bgtk_inject_event(ctx, click);
	take_screenshot(ctx, "labyrinth_02_url_focused.png");

	/* 03: about:blank */
	labyrinth_test_load("about:blank");
	bgtk_draw_widgets(ctx);
	take_screenshot(ctx, "labyrinth_03_blank.png");

	/* 04: back to home via load (history not required) */
	labyrinth_test_load("about:home");
	bgtk_draw_widgets(ctx);
	take_screenshot(ctx, "labyrinth_04_home_again.png");

	/*
	 * 05: Shift+wheel horizontal scroll on a deliberately wide page.
	 * Scrollable must report content_width > view and scroll_x change.
	 */
	{
		static const char *WIDE =
			"<!DOCTYPE html><html><body>"
			"<h1>Wide</h1>"
			"<p>"
			"XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX"
			"XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX"
			"XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX"
			"XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX"
			"XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX"
			"XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX"
			"</p>"
			"</body></html>";
		struct BGTK_Widget *root, *sc = NULL;
		struct InputEvent sh = {0}, wh = {0};
		int before_x, after_x, cw, vw;

		labyrinth_test_load_html(WIDE);
		bgtk_draw_widgets(ctx);
		/* Find any scrollable under the shell (HTML page scroll). */
		root = ctx->root_widget;
		if (root && root->type == BGTK_WIDGET_FRAME)
			root = root->data.frame.child;
		if (root && root->type == BGTK_WIDGET_LIST &&
		    root->data.list_widget.widget_count > 0)
			root = root->data.list_widget.items[0]; /* content_host */
		if (root && root->type == BGTK_WIDGET_FRAME)
			root = root->data.frame.child; /* page */
		if (root && root->type == BGTK_WIDGET_FRAME)
			root = root->data.frame.child; /* scroll */
		if (root && root->type == BGTK_WIDGET_SCROLLABLE)
			sc = root;
		if (!sc) {
			fprintf(stderr,
				"test_labyrinth: no content scrollable for "
				"h-scroll test\n");
			bgtk_destroy_mock(ctx);
			return 1;
		}
		/* Force a wide child so content_width exceeds the view (HTML
		 * word-wrap often keeps natural width ≤ viewport). */
		if (sc->data.scrollable.widget_count > 0 &&
		    sc->data.scrollable.items[0]) {
			struct BGTK_Widget *ch = sc->data.scrollable.items[0];

			if (ch->w < sc->w + 400)
				ch->w = sc->w + 400;
		}
		if (sc->data.scrollable.tmp) {
			free(sc->data.scrollable.tmp);
			sc->data.scrollable.tmp = NULL;
			sc->data.scrollable.widget_capacity = 0;
			sc->data.scrollable.tmp_items = NULL;
			sc->data.scrollable.tmp_item0 = NULL;
			sc->data.scrollable.tmp_nitems = 0;
		}
		bgtk_draw_widgets(ctx);
		cw = sc->data.scrollable.content_width;
		vw = sc->w;
		if (cw <= vw) {
			fprintf(stderr,
				"test_labyrinth: content not wider than view "
				"(content_w=%d view_w=%d)\n",
				cw, vw);
			bgtk_destroy_mock(ctx);
			return 1;
		}
		before_x = sc->data.scrollable.scroll_x;
		/* Hold Shift and wheel down (positive value = up; use -3). */
		sh.type = EV_KEY;
		sh.code = KEY_LEFTSHIFT;
		sh.value = 1;
		bgtk_inject_event(ctx, sh);
		wh.type = EV_REL;
		wh.code = REL_WHEEL;
		wh.value = -3;
		wh.x = width / 2;
		wh.y = height / 3;
		if (!bgtk_inject_event(ctx, wh)) {
			fprintf(stderr,
				"test_labyrinth: Shift+wheel not handled "
				"(content_w=%d view_w=%d)\n",
				cw, vw);
			bgtk_destroy_mock(ctx);
			return 1;
		}
		after_x = sc->data.scrollable.scroll_x;
		sh.value = 0;
		bgtk_inject_event(ctx, sh);
		if (after_x <= before_x) {
			fprintf(stderr,
				"test_labyrinth: scroll_x %d -> %d "
				"(want increase; content_w=%d view_w=%d)\n",
				before_x, after_x, cw, vw);
			bgtk_destroy_mock(ctx);
			return 1;
		}
		take_screenshot(ctx, "labyrinth_05_shift_wheel_hscroll.png");
	}

	printf("test_labyrinth complete. PNG frames written.\n");
	bgtk_destroy_mock(ctx);
	return 0;
}
