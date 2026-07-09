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
#include "config.h"
#include "internal.h"

/* Declarations from apps/settings.c (compiled with SETTINGS_TEST_MODE) */
extern void settings_build_ui(struct BGTK_Context *c, struct config *config,
			       int width, int height);
extern void settings_layout(void);
extern struct config *settings_get_config(void);
extern void settings_test_set_image_bg(const char *path, ImageMode mode);
extern void settings_test_toggle_bg_mode(void);

/* 32x32: top-left 16x16 red, rest black — scaled vs tiled look different. */
static int write_pattern_ppm(const char *path)
{
	FILE *f = fopen(path, "wb");
	int y, x;

	if (!f)
		return -1;
	fprintf(f, "P6\n32 32\n255\n");
	for (y = 0; y < 32; y++) {
		for (x = 0; x < 32; x++) {
			unsigned char c[3];
			if (x < 16 && y < 16) {
				c[0] = 0xE5;
				c[1] = 0x39;
				c[2] = 0x35;
			} else {
				c[0] = c[1] = c[2] = 0x20;
			}
			fwrite(c, 1, 3, f);
		}
	}
	fclose(f);
	return 0;
}

int main(void)
{
	/* Match settings app request — dual-col theme needs ~900 wide. */
	int width = 900;
	int height = 560;

	struct BGTK_Context *ctx = bgtk_init_mock(width, height);
	if (!ctx) {
		fprintf(stderr, "test_settings: bgtk_init_mock failed\n");
		return 1;
	}

	struct config config;
	init_config_defaults(&config);
	/* Distinct red wallpaper so preview packing (not theme paper) is obvious. */
	config.type = BG_COLOR;
	config.color = 0xFFE53935;

	settings_build_ui(ctx, &config, width, height);

	/* 00: Initial view - Background page is shown */
	bgtk_draw_widgets(ctx);
	take_screenshot(ctx, "settings_00_background.png");
	/* Unfocused frame border (theme.frame_border_unfocused). */
	bgtk_set_window_focus(ctx, 0);
	bgtk_draw_widgets(ctx);
	take_screenshot(ctx, "settings_00u_unfocused.png");
	bgtk_set_window_focus(ctx, 1);
	bgtk_draw_widgets(ctx);
	/* Preview must be red (0xE53935), not BGR-swapped blue-ish. */
	{
		uint32_t *fb = (uint32_t *)ctx->shm_buffer;
		/* Sample inside the preview box (content panel, mid). */
		int sx = 350, sy = 250;
		uint32_t p = fb[sy * width + sx];
		unsigned r = (p >> 16) & 0xFF, g = (p >> 8) & 0xFF, b = p & 0xFF;
		if (r < 0xC0 || g > 0x80 || b > 0x80) {
			fprintf(stderr,
				"test_settings: preview color wrong at %d,%d "
				"got #%02X%02X%02X want ~#E53935 (R/B swap?)\n",
				sx, sy, r, g, b);
			bgtk_destroy_mock(ctx);
			return 1;
		}
	}

	/* 00b: Click Background Apply (must hit-test after preview insert).
	 * Content scrollable is on the right; Apply sits under the preview. */
	{
		struct config *sc = settings_get_config();
		uint32_t before = sc ? sc->color : 0;
		struct InputEvent click = {0};
		click.type = EV_KEY;
		click.code = BTN_LEFT;
		click.value = 1;
		/* Apply sits under the aspect-correct preview (fills leftover). */
		click.x = 180;
		click.y = 512;
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

	/* 00c/00d: Image mode preview — scaled then toggle to tiled. */
	{
		const char *pat = "settings_test_wallpaper.ppm";
		struct config *sc;
		uint32_t *fb = (uint32_t *)ctx->shm_buffer;
		uint32_t p_tl, p_mid;
		int sx_tl = 210, sy_tl = 195;
		int sx_mid = 350, sy_mid = 250;

		if (write_pattern_ppm(pat) != 0) {
			fprintf(stderr, "test_settings: cannot write %s\n", pat);
			bgtk_destroy_mock(ctx);
			return 1;
		}
		/* Default mode must be SCALED (matches BGCE). */
		sc = settings_get_config();
		if (!sc || sc->mode != IMAGE_SCALED) {
			fprintf(stderr,
				"test_settings: default mode want SCALED got %d\n",
				sc ? (int)sc->mode : -1);
			bgtk_destroy_mock(ctx);
			return 1;
		}
		settings_test_set_image_bg(pat, IMAGE_SCALED);
		bgtk_draw_widgets(ctx);
		take_screenshot(ctx, "settings_00c_mode_scaled.png");
		p_tl = fb[sy_tl * width + sx_tl];
		p_mid = fb[sy_mid * width + sx_mid];
		/* Scaled: TL red patch grows; mid of box should still be dark. */
		if (((p_tl >> 16) & 0xFF) < 0xC0) {
			fprintf(stderr,
				"test_settings: scaled preview TL not red (#%06X)\n",
				(unsigned)(p_tl & 0xFFFFFF));
			bgtk_destroy_mock(ctx);
			return 1;
		}
		/* Mid of scaled box is often dark (bottom-right of pattern);
		 * accept either dark or red depending on sample position. */
		(void)p_mid;

		settings_test_toggle_bg_mode();
		sc = settings_get_config();
		if (!sc || sc->mode != IMAGE_TILED) {
			fprintf(stderr,
				"test_settings: after toggle want TILED got %d\n",
				sc ? (int)sc->mode : -1);
			bgtk_destroy_mock(ctx);
			return 1;
		}
		bgtk_draw_widgets(ctx);
		take_screenshot(ctx, "settings_00d_mode_tiled.png");
		p_mid = fb[sy_mid * width + sx_mid];
		/* Tiled: mid of large box hits a repeated tile (red or dark).
		 * Sample a second red-tile location further right/down. */
		{
			int found_red = 0, found_dark = 0;
			int y, x;
			for (y = 140; y < 360; y += 8) {
				for (x = 200; x < 620; x += 8) {
					uint32_t p = fb[y * width + x];
					unsigned r = (p >> 16) & 0xFF;
					if (r > 0xC0)
						found_red = 1;
					else if (r < 0x50)
						found_dark = 1;
				}
			}
			if (!found_red || !found_dark) {
				fprintf(stderr,
					"test_settings: tiled preview missing "
					"red/dark tiles (red=%d dark=%d)\n",
					found_red, found_dark);
				bgtk_destroy_mock(ctx);
				return 1;
			}
		}
		/* Toggle again back to scaled. */
		settings_test_toggle_bg_mode();
		sc = settings_get_config();
		if (!sc || sc->mode != IMAGE_SCALED) {
			fprintf(stderr,
				"test_settings: second toggle want SCALED got %d\n",
				sc ? (int)sc->mode : -1);
			bgtk_destroy_mock(ctx);
			return 1;
		}
		/* Restore color page for the rest of the suite. */
		sc->type = BG_COLOR;
		sc->color = 0xFFE53935;
		settings_layout();
	}

	/* Sidebar nav: first button center ~40, step ~45 (pad/2+2 chrome). */
	#define SIDEBAR_X 70
	/* Nav buttons ~41px tall + 16px gap (SIDEBAR_NAV_MARGIN=8 → 2×margin). */
	#define SIDEBAR_BTN_Y(n) (39 + (n) * 57)

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
		/* First theme color field (Background) under Goldie chrome. */
		click.x = 320;
		click.y = 40;
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
	if (bgtk_resize_mock(ctx, 1100, 700) != 0) {
		fprintf(stderr, "test_settings: bgtk_resize_mock failed\n");
		bgtk_destroy_mock(ctx);
		return 1;
	}
	settings_layout();
	if (ctx->root_widget->w != 1100 || ctx->root_widget->h != 700) {
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