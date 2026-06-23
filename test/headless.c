/* Minimal headless (mock) test for BGTK.
 *
 * Build with:
 *     make headless
 *     ./headless
 *
 * Produces headless_*.png files you can open to visually inspect the UI.
 *
 * Key APIs demonstrated:
 *   - bgtk_init_mock(w, h)          : create a context with an owned in-memory buffer
 *   - take_screenshot(ctx, "foo.png") or take_screenshot(ctx, NULL)
 *   - bgtk_inject_event(ctx, ev)    : simulate clicks and key presses
 *   - bgtk_destroy_mock(ctx)
 *   - BGTK_Options.text_align       : left / center / right within widget bounds
 *
 * No BGCE server, DRM, or real input devices are required.
 * This is the recommended workflow for developing and debugging widgets.
 */

#include <stdio.h>
#include <string.h>
#include <linux/input.h>

#include "bgtk.h"

static int clicks = 0;
static struct BGTK_Widget *counter_label = NULL;

static void on_click(void *userdata)
{
	(void)userdata;
	clicks++;
	if (counter_label && counter_label->data.label.set_label) {
		char buf[64];
		snprintf(buf, sizeof(buf), "clicks: %d", clicks);
		counter_label->data.label.set_label(counter_label, buf);
	}
}

/* Force outer width so text_align has horizontal room (draw still measures height). */
static struct BGTK_Widget *fix_w(struct BGTK_Widget *w, int width)
{
	if (w)
		w->w = width;
	return w;
}

/* One column: heading + text/label/button/input all at the given alignment,
 * at a shared width so left/center/right is obvious in screenshots.
 * out_input (optional) receives the column's text_input for later focus/typing. */
static struct BGTK_Widget *align_column(struct BGTK_Context *ctx,
					const char *heading,
					enum BGTK_Text_Align align,
					struct BGTK_Widget **out_input)
{
	const int row_w = 190;
	BGTK_Options ao = {.padding = 4, .margin = 2, .text_align = align};

	struct BGTK_Widget *h = fix_w(bgtk_text(ctx, (char *)heading, ao), row_w);
	struct BGTK_Widget *t = fix_w(bgtk_text(ctx, "text", ao), row_w);
	struct BGTK_Widget *l = fix_w(bgtk_label(ctx, "label", ao), row_w);
	struct BGTK_Widget *bl = bgtk_text(ctx, "btn", (BGTK_Options){.padding = 2, .text_align = align});
	struct BGTK_Widget *b = fix_w(
		bgtk_button(ctx, bl, NULL, NULL, (BGTK_Options){.padding = 6, .margin = 2, .text_align = align}),
		row_w);
	struct BGTK_Widget *ti = bgtk_text_input(ctx, "input", row_w, 0, ao);
	if (out_input)
		*out_input = ti;

	struct BGTK_Widget *items[5] = {h, t, l, b, ti};
	return bgtk_list(ctx, items, 5,
		(BGTK_Options){.orientation = BGTK_LIST_VERTICAL, .margin = 4});
}

static int run_align_scene(void)
{
	/* Wide enough for three 200px columns side by side. */
	struct BGTK_Context *ctx = bgtk_init_mock(680, 280);
	if (!ctx) {
		fprintf(stderr, "headless: align init failed\n");
		return 1;
	}

	struct BGTK_Widget *title = bgtk_text(ctx, "text_align L / C / R",
		(BGTK_Options){.padding = 4, .margin = 4});

	struct BGTK_Widget *right_input = NULL;
	struct BGTK_Widget *cols[3] = {
		align_column(ctx, "LEFT", BGTK_ALIGN_LEFT, NULL),
		align_column(ctx, "CENTER", BGTK_ALIGN_CENTER, NULL),
		align_column(ctx, "RIGHT", BGTK_ALIGN_RIGHT, &right_input),
	};
	struct BGTK_Widget *row = bgtk_list(ctx, cols, 3,
		(BGTK_Options){.orientation = BGTK_LIST_HORIZONTAL, .margin = 4});

	struct BGTK_Widget *stack[2] = {title, row};
	ctx->root_widget = bgtk_list(ctx, stack, 2,
		(BGTK_Options){.orientation = BGTK_LIST_VERTICAL, .margin = 4});

	bgtk_draw_widgets(ctx);
	take_screenshot(ctx, "headless_05_text_align.png");

	/* Focus the right-aligned input and type a short string; alignment
	 * should still apply while the text fits (no horizontal scroll). */
	bgtk_set_focus(ctx, right_input);
	struct InputEvent key = {0};
	key.type = EV_KEY;
	key.value = 1;
	key.code = KEY_H; bgtk_inject_event(ctx, key);
	key.code = KEY_I; bgtk_inject_event(ctx, key);
	bgtk_draw_widgets(ctx);
	take_screenshot(ctx, "headless_06_text_align_input.png");

	bgtk_destroy_mock(ctx);
	return 0;
}

int main(void)
{
	/* 1. Headless init: no server, we get a plain malloc framebuffer */
	struct BGTK_Context *ctx = bgtk_init_mock(420, 260);
	if (!ctx) {
		fprintf(stderr, "headless: init failed\n");
		return 1;
	}

	/* 2. Build a tiny widget tree (similar to image_viewer bits) */
	struct BGTK_Widget *title = bgtk_text(ctx, "headless test", (BGTK_Options){.padding = 4, .margin = 2});

	struct BGTK_Widget *btn_label = bgtk_text(ctx, "click me", (BGTK_Options){.padding = 2});
	struct BGTK_Widget *btn = bgtk_button(ctx, btn_label, on_click, NULL, (BGTK_Options){.padding = 8, .margin = 4});

	counter_label = bgtk_label(ctx, "clicks: 0", (BGTK_Options){.padding = 2, .margin = 2});

	struct BGTK_Widget *ti = bgtk_text_input(ctx, "type here", 200, 0, (BGTK_Options){.padding = 6, .margin = 4});

	struct BGTK_Widget *row[2] = {btn, ti};
	struct BGTK_Widget *roww = bgtk_list(ctx, row, 2, (BGTK_Options){.orientation = BGTK_LIST_HORIZONTAL, .margin = 4});

	struct BGTK_Widget *col[3] = {title, roww, counter_label};
	struct BGTK_Widget *root = bgtk_list(ctx, col, 3, (BGTK_Options){.orientation = BGTK_LIST_VERTICAL, .margin = 6});

	ctx->root_widget = root;
	bgtk_draw_widgets(ctx);

	/* 3. Observe initial render */
	take_screenshot(ctx, "headless_00_init.png");

	/* 4. Simulate a click on the button via inject (absolute coords after layout) */
	/* We know approx positions; for real tests query widget x/y after calculate. */
	struct InputEvent click = {0};
	click.type = EV_KEY;
	click.code = BTN_LEFT;
	click.value = 1;
	click.x = btn->x + 10;
	click.y = btn->y + 10;
	bgtk_inject_event(ctx, click);
	click.value = 0;
	bgtk_inject_event(ctx, click);

	take_screenshot(ctx, "headless_01_clicked.png");

	/* 5. Focus the text input and type some chars (tests the keyboard path) */
	bgtk_set_focus(ctx, ti);
	take_screenshot(ctx, "headless_02_focused.png");

	struct InputEvent key = {0};
	key.type = EV_KEY;
	key.value = 1;
	key.code = KEY_H; bgtk_inject_event(ctx, key);
	key.code = KEY_E; bgtk_inject_event(ctx, key);
	key.code = KEY_L; bgtk_inject_event(ctx, key);
	key.code = KEY_L; bgtk_inject_event(ctx, key);
	key.code = KEY_O; bgtk_inject_event(ctx, key);
	key.code = KEY_SPACE; bgtk_inject_event(ctx, key);
	key.code = KEY_W; bgtk_inject_event(ctx, key);
	key.code = KEY_O; bgtk_inject_event(ctx, key);
	key.code = KEY_R; bgtk_inject_event(ctx, key);
	key.code = KEY_L; bgtk_inject_event(ctx, key);
	key.code = KEY_D; bgtk_inject_event(ctx, key);

	take_screenshot(ctx, "headless_03_typed.png");

	/* 6. Backspace a couple chars */
	key.code = KEY_BACKSPACE; bgtk_inject_event(ctx, key);
	key.code = KEY_BACKSPACE; bgtk_inject_event(ctx, key);
	take_screenshot(ctx, "headless_04_backspaced.png");

	bgtk_destroy_mock(ctx);

	/* 7. text_align showcase (separate scene, wider canvas) */
	if (run_align_scene() != 0)
		return 1;

	printf("headless test complete. PNG frames written.\n");
	return 0;
}
