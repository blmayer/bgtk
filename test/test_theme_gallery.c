/* test/test_theme_gallery.c
 *
 * Headless gallery of settings-app screenshots under candidate themes
 * (HN / default-theme brainstorm), including focus/highlight tokens.
 *
 * Build:  make test_theme_gallery
 * Run:    ./test_theme_gallery
 * Output: settings_theme_*.png
 */

#include <stdio.h>
#include <string.h>
#include <linux/input.h>

#include "bgtk.h"
#include "config.h"
#include "html.h"
#include "internal.h"

extern void settings_build_ui(struct BGTK_Context *c, struct config *config,
			      int width, int height);
extern struct config *settings_get_config(void);

struct theme_case {
	const char *id;
	const char *name;
	BGTK_Theme theme;
};

#define SIDEBAR_X 70
#define SIDEBAR_BTN_Y(n) (31 + (n) * 42)

static void click_xy(struct BGTK_Context *ctx, int x, int y)
{
	struct InputEvent ev = {0};
	ev.type = EV_KEY;
	ev.code = BTN_LEFT;
	ev.value = 1;
	ev.x = x;
	ev.y = y;
	bgtk_inject_event(ctx, ev);
	ev.value = 0;
	bgtk_inject_event(ctx, ev);
}

static int render_case(const struct theme_case *tc, int width, int height)
{
	char path_bg[128];
	char path_theme[128];
	struct BGTK_Context *ctx;
	struct config config;

	ctx = bgtk_init_mock(width, height);
	if (!ctx) {
		fprintf(stderr, "test_theme_gallery: init_mock failed (%s)\n",
			tc->id);
		return -1;
	}

	init_config_defaults(&config);
	config.theme = tc->theme;
	config.type = BG_COLOR;
	config.color = tc->theme.background;
	ctx->theme = tc->theme;

	settings_build_ui(ctx, &config, width, height);
	bgtk_draw_widgets(ctx);
	snprintf(path_bg, sizeof(path_bg), "settings_theme_%s_bg.png", tc->id);
	if (take_screenshot(ctx, path_bg) != 0) {
		fprintf(stderr, "test_theme_gallery: screenshot failed %s\n",
			path_bg);
		bgtk_destroy_mock(ctx);
		return -1;
	}

	click_xy(ctx, SIDEBAR_X, SIDEBAR_BTN_Y(4));
	bgtk_draw_widgets(ctx);
	snprintf(path_theme, sizeof(path_theme), "settings_theme_%s_theme.png",
		 tc->id);
	if (take_screenshot(ctx, path_theme) != 0) {
		fprintf(stderr, "test_theme_gallery: screenshot failed %s\n",
			path_theme);
		bgtk_destroy_mock(ctx);
		return -1;
	}

	printf("  %s → %s, %s\n", tc->name, path_bg, path_theme);
	bgtk_destroy_mock(ctx);
	return 0;
}

int main(void)
{
	/* Full BGTK_Theme including focus / focus_bg / highlight. */
	static const struct theme_case cases[] = {
		{
			.id = "00_goldie",
			.name = "Goldie (default)",
			.theme = {
				.background = 0xFF0A0A0A,
				.button = 0xFF1C1814,
				.button_text = 0xFFF5E6D3,
				.frame_border_color = 0xFFE0A060,
				.frame_border_size = 6,
				.button_border_size = 3,
				.input_border_size = 3,
				.focus = 0xFFE0A060,
				.focus_bg = 0xFF2A2018,
				.input_bg = 0xFF1C1814,
				.highlight = 0xFFD4B8A0,
				.margin = 6,
				.padding = 8,
			},
		},
		{
			.id = "00b_paper",
			.name = "Paper",
			.theme = {
				.background = 0xFFF4F1EA,
				.button = 0xFFE8E2D6,
				.button_text = 0xFF1C1917,
				.frame_border_color = 0xFFC4B8A8,
				.frame_border_size = 2,
				.button_border_size = 1,
				.input_border_size = 1,
				.focus = 0xFFB45309,
				.focus_bg = 0xFFFFF7ED,
				.highlight = 0xFF78716C,
				.margin = 4,
				.padding = 4,
			},
		},
		{
			.id = "01_ink",
			.name = "Ink",
			.theme = {
				.background = 0xFFFAFAF8,
				.button = 0xFFF0F0EC,
				.button_text = 0xFF111111,
				.frame_border_color = 0xFF111111,
				.frame_border_size = 1,
				.button_border_size = 1,
				.input_border_size = 1,
				.focus = 0xFF111111,
				.focus_bg = 0xFFEEEEEE,
				.highlight = 0xFF333333,
			},
		},
		{
			.id = "02_slate",
			.name = "Slate",
			.theme = {
				.background = 0xFFEEF1F4,
				.button = 0xFFE2E7ED,
				.button_text = 0xFF1A2332,
				.frame_border_color = 0xFF9AA7B5,
				.frame_border_size = 1,
				.button_border_size = 1,
				.input_border_size = 2,
				.focus = 0xFF2563EB,
				.focus_bg = 0xFFDBEAFE,
				.highlight = 0xFF475569,
			},
		},
		{
			.id = "03_charcoal",
			.name = "Charcoal",
			.theme = {
				.background = 0xFF1A1D23,
				.button = 0xFF2A2F38,
				.button_text = 0xFFE8EAED,
				.frame_border_color = 0xFF3D4450,
				.frame_border_size = 1,
				.button_border_size = 1,
				.input_border_size = 1,
				.focus = 0xFF60A5FA,
				.focus_bg = 0xFF1E293B,
				.highlight = 0xFF3B4252,
			},
		},
		{
			.id = "04_frost",
			.name = "Frost (Nord-ish)",
			.theme = {
				.background = 0xFFECEFF4,
				.button = 0xFFE5E9F0,
				.button_text = 0xFF2E3440,
				.frame_border_color = 0xFFD8DEE9,
				.frame_border_size = 1,
				.button_border_size = 1,
				.input_border_size = 1,
				.focus = 0xFF5E81AC,
				.focus_bg = 0xFFE5E9F0,
				.highlight = 0xFF4C566A,
			},
		},
		{
			.id = "05_matcha",
			.name = "Matcha",
			.theme = {
				.background = 0xFFF7F6F2,
				.button = 0xFFDDE8DF,
				.button_text = 0xFF1B2A1F,
				.frame_border_color = 0xFFC5D0C7,
				.frame_border_size = 1,
				.button_border_size = 1,
				.input_border_size = 1,
				.focus = 0xFF3F6F4E,
				.focus_bg = 0xFFE8F5E9,
				.highlight = 0xFF5B7C65,
			},
		},
		{
			.id = "06_a11y",
			.name = "High-contrast",
			.theme = {
				.background = 0xFFFFFFFF,
				.button = 0xFFFFFFFF,
				.button_text = 0xFF000000,
				.frame_border_color = 0xFF000000,
				.frame_border_size = 2,
				.button_border_size = 2,
				.input_border_size = 2,
				.focus = 0xFF0000FF,
				.focus_bg = 0xFFFFFF00,
				.highlight = 0xFF000000,
			},
		},
		{
			.id = "07_default",
			.name = "Default (Paper)",
			.theme = {
				.background = 0xFFF4F1EA,
				.button = 0xFFE8E2D6,
				.button_text = 0xFF1C1917,
				.frame_border_color = 0xFFC4B8A8,
				.frame_border_size = 2,
				.button_border_size = 1,
				.input_border_size = 1,
				.focus = 0xFFB45309,
				.focus_bg = 0xFFFFF7ED,
				.highlight = 0xFF78716C,
			},
		},
	};
	const int n = (int)(sizeof(cases) / sizeof(cases[0]));
	int i;
	int width = 700;
	int height = 520;

	printf("test_theme_gallery: %d themes @ %dx%d\n", n, width, height);
	for (i = 0; i < n; i++) {
		if (render_case(&cases[i], width, height) != 0)
			return 1;
	}
	printf("test_theme_gallery complete. PNG files written.\n");
	return 0;
}
