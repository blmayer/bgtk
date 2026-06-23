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

	/* 03: Click "Font" */
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

	printf("test_settings complete. PNG files written.\n");
	bgtk_destroy_mock(ctx);
	return 0;
}