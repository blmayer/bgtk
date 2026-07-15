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
int labyrinth_test_app_key(struct InputEvent ev);
struct BGTK_Widget *labyrinth_test_addr(void);
void labyrinth_test_focus_addr(void);

static const char *SAMPLE =
	"<!DOCTYPE html><html><body>"
	"<h1>Labyrinth Test</h1>"
	"<p>Inline HTML rendered through <b>bgtk_html_parse_inline</b>.</p>"
	"<h2>List</h2>"
	"<ul><li>Alpha</li><li>Beta</li><li>Gamma</li></ul>"
	"<p><a href=\"about:blank\">Go blank</a> is a wired link.</p>"
	"<h2>Pre</h2>"
	"<pre>alpha\n"
	"  beta\n"
	"gamma</pre>"
	"</body></html>";

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
	default:
		break;
	}
	return NULL;
}

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

	/* 00: about:home — shortcuts table; address bar focused (startup) */
	labyrinth_test_load("about:home");
	labyrinth_test_focus_addr();
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
	if (!labyrinth_test_addr() ||
	    ctx->focused_widget != labyrinth_test_addr()) {
		fprintf(stderr,
			"test_labyrinth: startup should focus address bar\n");
		bgtk_destroy_mock(ctx);
		return 1;
	}

	/* 01: inline HTML sample (pre + wired link) */
	labyrinth_test_load_html(SAMPLE);
	take_screenshot(ctx, "labyrinth_01_inline_html.png");
	{
		struct BGTK_Widget *link = find_href(ctx->root_widget);
		struct BGTK_Widget *sc = NULL, *w;
		int scx, scy;

		if (!link || !link->data.text.href ||
		    strcmp(link->data.text.href, "about:blank") != 0) {
			fprintf(stderr,
				"test_labyrinth: link href not wired "
				"(got %s)\n",
				link && link->data.text.href
					? link->data.text.href
					: "(null)");
			bgtk_destroy_mock(ctx);
			return 1;
		}
		/* Walk up to content scrollable (link x/y are content-space). */
		for (w = link; w; w = w->parent) {
			if (w->type == BGTK_WIDGET_SCROLLABLE) {
				sc = w;
				break;
			}
		}
		if (!sc) {
			fprintf(stderr,
				"test_labyrinth: link not under scrollable\n");
			bgtk_destroy_mock(ctx);
			return 1;
		}
		bgtk_widget_screen_pos(sc, &scx, &scy);
		/* Screen = scroll origin + content pos − scroll offset. */
		click.type = EV_KEY;
		click.code = BTN_LEFT;
		click.value = 1;
		click.x = scx + link->x + link->w / 2 -
			  sc->data.scrollable.scroll_x;
		click.y = scy + link->y + link->h / 2 -
			  sc->data.scrollable.scroll_y;
		if (!bgtk_inject_event(ctx, click)) {
			fprintf(stderr,
				"test_labyrinth: link click not handled "
				"(screen %d,%d content link %d,%d)\n",
				click.x, click.y, link->x, link->y);
			bgtk_destroy_mock(ctx);
			return 1;
		}
		click.value = 0;
		bgtk_inject_event(ctx, click);
		bgtk_draw_widgets(ctx);
		take_screenshot(ctx, "labyrinth_01b_link_clicked.png");
		if (strcmp(labyrinth_test_url(), "about:blank") != 0) {
			fprintf(stderr,
				"test_labyrinth: after link click want "
				"about:blank got %s\n",
				labyrinth_test_url());
			bgtk_destroy_mock(ctx);
			return 1;
		}
	}

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

	/* 06: Ctrl+L focuses URL bar; Ctrl+R reloads */
	{
		struct InputEvent ke = {0};
		struct BGTK_Widget *addr;
		const char *url_before;

		labyrinth_test_load("about:home");
		bgtk_draw_widgets(ctx);
		/* Focus content so Ctrl+L has something to switch from. */
		if (ctx->root_widget)
			bgtk_set_focus(ctx, ctx->root_widget);

		ke.type = EV_KEY;
		ke.code = KEY_LEFTCTRL;
		ke.value = 1;
		if (labyrinth_test_app_key(ke) != 0) {
			/* mod-only may return 0 — ok */
		}
		ke.code = KEY_L;
		ke.value = 1;
		if (labyrinth_test_app_key(ke) != 1) {
			fprintf(stderr,
				"test_labyrinth: Ctrl+L not handled\n");
			bgtk_destroy_mock(ctx);
			return 1;
		}
		addr = labyrinth_test_addr();
		if (!addr || ctx->focused_widget != addr) {
			fprintf(stderr,
				"test_labyrinth: Ctrl+L did not focus addr bar\n");
			bgtk_destroy_mock(ctx);
			return 1;
		}
		bgtk_draw_widgets(ctx);
		take_screenshot(ctx, "labyrinth_06_ctrl_l_addr.png");

		/* Release L, still holding ctrl; R reloads */
		ke.code = KEY_L;
		ke.value = 0;
		labyrinth_test_app_key(ke);
		url_before = labyrinth_test_url();
		ke.code = KEY_R;
		ke.value = 1;
		if (labyrinth_test_app_key(ke) != 1) {
			fprintf(stderr,
				"test_labyrinth: Ctrl+R not handled\n");
			bgtk_destroy_mock(ctx);
			return 1;
		}
		if (strcmp(labyrinth_test_url(), url_before) != 0 &&
		    strcmp(labyrinth_test_url(), "about:home") != 0) {
			fprintf(stderr,
				"test_labyrinth: Ctrl+R left unexpected url %s\n",
				labyrinth_test_url());
			bgtk_destroy_mock(ctx);
			return 1;
		}
		/* stay on about:home after reload */
		if (strcmp(labyrinth_test_url(), "about:home") != 0) {
			fprintf(stderr,
				"test_labyrinth: after Ctrl+R want about:home "
				"got %s\n",
				labyrinth_test_url());
			bgtk_destroy_mock(ctx);
			return 1;
		}
		ke.code = KEY_R;
		ke.value = 0;
		labyrinth_test_app_key(ke);
		ke.code = KEY_LEFTCTRL;
		ke.value = 0;
		labyrinth_test_app_key(ke);
		bgtk_draw_widgets(ctx);
		take_screenshot(ctx, "labyrinth_06b_ctrl_r_reload.png");
	}

	printf("test_labyrinth complete. PNG frames written.\n");
	bgtk_destroy_mock(ctx);
	return 0;
}
