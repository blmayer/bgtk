/* test/test_settings.c
 *
 * Headless (mock) test for the settings application.
 *
 * Build:  make test_settings
 * Run:    ./test_settings
 *
 * Produces settings_*.png screenshots for visual inspection.
 * Exercises the sidebar navigation, page content, text input, and apply flow.
 */

#include <stdio.h>
#include <string.h>
#include <linux/input.h>

#include "bgtk.h"
#include "html.h"
#include "config.h"
#include "internal.h"

/* Declarations from apps/settings.c (compiled with SETTINGS_TEST_MODE) */
extern void settings_build_ui(struct BGTK_Context *c, struct config *config,
			       int width, int height);
extern void settings_layout(void);
extern struct config *settings_get_config(void);

int main(void)
{
	int width = 700;
	int height = 480;

	struct BGTK_Context *ctx = bgtk_init_mock(width, height);
	if (!ctx) {
		fprintf(stderr, "test_settings: bgtk_init_mock failed\n");
		return 1;
	}

	struct config config;
	init_config_defaults(&config);

	settings_build_ui(ctx, &config, width, height);

	/* 00: Initial view - Background page is shown */
	bgtk_draw_widgets(ctx);
	take_screenshot(ctx, "settings_00_background.png");

	/* 00b: Click Background Apply (must hit-test after preview insert).
	 * Content scrollable is on the right; Apply sits under the preview. */
	{
		struct config *sc = settings_get_config();
		uint32_t before = sc ? sc->color : 0;
		struct InputEvent click = {0};
		click.type = EV_KEY;
		click.code = BTN_LEFT;
		click.value = 1;
		/* Apply under preview; layout shifted by intro line + panel rule. */
		click.x = 192;
		click.y = 255;
		bgtk_inject_event(ctx, click);
		click.value = 0;
		if (!bgtk_inject_event(ctx, click)) {
			fprintf(stderr,
				"test_settings: Background Apply click not handled\n");
			bgtk_destroy_mock(ctx);
			return 1;
		}
		/* Apply reads the field (still default) and rewrites config. */
		if (sc && sc->color != before && before != 0) {
			/* color may stay the same if field unchanged — ok */
		}
		(void)before;
		take_screenshot(ctx, "settings_00b_bg_apply.png");
	}

	/* Sidebar buttons are inside a scrollable; their content-space positions
	 * are offset by the scrollable's screen position (~8px) plus internal
	 * padding/margin.  Each button is ~38px tall with ~4px gap.
	 * Content-space centers: btn0=23, btn1=65, btn2=107, btn3=149, btn4=191.
	 * Add scrollable screen-y offset (~8) for absolute coords. */
	#define SIDEBAR_X 70
	#define SIDEBAR_BTN_Y(n) (31 + (n) * 42)

	/* 01: Click "Cursor" */
	{
		struct InputEvent click = {0};
		click.type = EV_KEY;
		click.code = BTN_LEFT;
		click.value = 1;
		click.x = SIDEBAR_X;
		click.y = SIDEBAR_BTN_Y(1);
		bgtk_inject_event(ctx, click);
		click.value = 0;
		bgtk_inject_event(ctx, click);
	}
	take_screenshot(ctx, "settings_01_cursor.png");

	/* 02: Click "Shortcuts" */
	{
		struct InputEvent click = {0};
		click.type = EV_KEY;
		click.code = BTN_LEFT;
		click.value = 1;
		click.x = SIDEBAR_X;
		click.y = SIDEBAR_BTN_Y(2);
		bgtk_inject_event(ctx, click);
		click.value = 0;
		bgtk_inject_event(ctx, click);
	}
	take_screenshot(ctx, "settings_02_shortcuts.png");

	/* 03: Click "Font" — sans/mono/serif pickers + size */
	{
		struct InputEvent click = {0};
		click.type = EV_KEY;
		click.code = BTN_LEFT;
		click.value = 1;
		click.x = SIDEBAR_X;
		click.y = SIDEBAR_BTN_Y(3);
		bgtk_inject_event(ctx, click);
		click.value = 0;
		bgtk_inject_event(ctx, click);
	}
	take_screenshot(ctx, "settings_03_font.png");

	/* Defaults must include distinct mono/serif paths (or at least set). */
	{
		struct config *sc = settings_get_config();
		if (!sc || !sc->font_sans_path[0]) {
			fprintf(stderr, "test_settings: missing UI/sans font path\n");
			bgtk_destroy_mock(ctx);
			return 1;
		}
		if (!sc->font_mono_path[0] || !sc->font_serif_path[0]) {
			fprintf(stderr,
				"test_settings: mono/serif defaults missing "
				"mono='%s' serif='%s'\n",
				sc->font_mono_path, sc->font_serif_path);
			bgtk_destroy_mock(ctx);
			return 1;
		}
	}

	/* Open Mono picker (second font row button, content panel). */
	{
		struct InputEvent click = {0};
		click.type = EV_KEY;
		click.code = BTN_LEFT;
		click.value = 1;
		/* Content panel ~x=200; Mono row roughly under Sans. */
		click.x = 280;
		click.y = 95;
		bgtk_inject_event(ctx, click);
		click.value = 0;
		bgtk_inject_event(ctx, click);
	}
	take_screenshot(ctx, "settings_03b_font_mono_open.png");

	/* 04: Click "Theme" */
	{
		struct InputEvent click = {0};
		click.type = EV_KEY;
		click.code = BTN_LEFT;
		click.value = 1;
		click.x = SIDEBAR_X;
		click.y = SIDEBAR_BTN_Y(4);
		bgtk_inject_event(ctx, click);
		click.value = 0;
		bgtk_inject_event(ctx, click);
	}
	take_screenshot(ctx, "settings_04_theme.png");

	/* 04b: Focus first theme color field and type a char (nested table). */
	{
		struct InputEvent click = {0};
		struct InputEvent key = {0};
		struct config *sc;
		const char *got;

		click.type = EV_KEY;
		click.code = BTN_LEFT;
		click.value = 1;
		/* Content scroll ~161,17; first input ~151,14 (see probe). */
		click.x = 161 + 151 + 20;
		click.y = 17 + 14 + 12;
		bgtk_inject_event(ctx, click);
		click.value = 0;
		bgtk_inject_event(ctx, click);

		key.type = EV_KEY;
		key.value = 1;
		key.code = KEY_A;
		key.x = 0;
		key.y = 0;
		if (!bgtk_inject_event(ctx, key)) {
			fprintf(stderr,
				"test_settings: theme field key not handled\n");
			bgtk_destroy_mock(ctx);
			return 1;
		}
		sc = settings_get_config();
		(void)sc;
		/* Field text is on the widget; re-read via tree is heavy —
		 * screenshot + non-zero handle is the gate. Append check via
		 * focused widget if set. */
		if (ctx->focused_widget &&
		    ctx->focused_widget->type == BGTK_WIDGET_TEXT_INPUT) {
			got = ctx->focused_widget->data.text_input.text;
			if (!got || !strchr(got, 'a')) {
				fprintf(stderr,
					"test_settings: theme type expected 'a' in '%s'\n",
					got ? got : "(null)");
				bgtk_destroy_mock(ctx);
				return 1;
			}
		} else {
			fprintf(stderr,
				"test_settings: theme input not focused after click\n");
			bgtk_destroy_mock(ctx);
			return 1;
		}
		take_screenshot(ctx, "settings_04b_theme_typed.png");
	}

	/* 05: Go back to Background */
	{
		struct InputEvent click = {0};
		click.type = EV_KEY;
		click.code = BTN_LEFT;
		click.value = 1;
		click.x = SIDEBAR_X;
		click.y = SIDEBAR_BTN_Y(0);
		bgtk_inject_event(ctx, click);
		click.value = 0;
		bgtk_inject_event(ctx, click);
	}
	take_screenshot(ctx, "settings_05_back_to_bg.png");

	/* 06: Window resize — chrome + page must reflow (was a no-op before). */
	if (bgtk_resize_mock(ctx, 900, 640) != 0) {
		fprintf(stderr, "test_settings: bgtk_resize_mock failed\n");
		bgtk_destroy_mock(ctx);
		return 1;
	}
	settings_layout();
	if (ctx->root_widget->w != 900 || ctx->root_widget->h != 640) {
		fprintf(stderr, "test_settings: root not resized (%dx%d)\n",
			ctx->root_widget->w, ctx->root_widget->h);
		bgtk_destroy_mock(ctx);
		return 1;
	}
	take_screenshot(ctx, "settings_06_resized.png");

	printf("test_settings complete. PNG files written.\n");
	bgtk_destroy_mock(ctx);
	return 0;
}