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
	bgtk_log_open("headless");

	/* 1. Headless init: no server, we get a plain malloc framebuffer */
	struct BGTK_Context *ctx = bgtk_init_mock(420, 260);
	if (!ctx) {
		bgtk_log("headless: init failed");
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

	/* 6b. Resize the mock framebuffer (simulates MSG_BUFFER_CHANGE path). */
	if (bgtk_resize_mock(ctx, 520, 320) != 0) {
		bgtk_log("headless: resize mock failed");
		return 1;
	}
	if (ctx->root_widget) {
		ctx->root_widget->w = ctx->width;
		ctx->root_widget->h = ctx->height;
	}
	bgtk_draw_widgets(ctx);
	take_screenshot(ctx, "headless_04b_resized.png");

	/* 6c. Shift + Ctrl in text input */
	{
		struct BGTK_Context *kctx = bgtk_init_mock(400, 120);
		struct BGTK_Widget *kti =
			bgtk_text_input(kctx, "", 360, 0,
					(BGTK_Options){.padding = 6, .margin = 4});
		kctx->root_widget = kti;
		bgtk_set_focus(kctx, kti);

		struct InputEvent ke = {0};
		ke.type = EV_KEY;
		ke.value = 1;
		/* type "hi" */
		ke.code = KEY_H;
		bgtk_inject_event(kctx, ke);
		ke.code = KEY_I;
		bgtk_inject_event(kctx, ke);
		/* Shift+1 → '!' */
		ke.code = KEY_LEFTSHIFT;
		bgtk_inject_event(kctx, ke);
		ke.code = KEY_1;
		bgtk_inject_event(kctx, ke);
		ke.code = KEY_LEFTSHIFT;
		ke.value = 0;
		bgtk_inject_event(kctx, ke);
		ke.value = 1;
		/* Ctrl+C must not insert 'c' */
		ke.code = KEY_LEFTCTRL;
		bgtk_inject_event(kctx, ke);
		ke.code = KEY_C;
		bgtk_inject_event(kctx, ke);
		ke.code = KEY_LEFTCTRL;
		ke.value = 0;
		bgtk_inject_event(kctx, ke);
		ke.value = 1;

		bgtk_draw_widgets(kctx);
		take_screenshot(kctx, "headless_04c_shift_ctrl.png");

		const char *got = kti->data.text_input.text;
		if (!got || strcmp(got, "hi!") != 0) {
			bgtk_log("headless: expected text 'hi!' got '%s'",
				 got ? got : "(null)");
			bgtk_destroy_mock(kctx);
			return 1;
		}
		/* Ctrl+A then Ctrl+K clears line */
		ke.code = KEY_LEFTCTRL;
		bgtk_inject_event(kctx, ke);
		ke.code = KEY_A;
		bgtk_inject_event(kctx, ke);
		ke.code = KEY_K;
		bgtk_inject_event(kctx, ke);
		ke.code = KEY_LEFTCTRL;
		ke.value = 0;
		bgtk_inject_event(kctx, ke);
		got = kti->data.text_input.text;
		if (!got || got[0] != '\0') {
			bgtk_log("headless: expected empty after Ctrl+A/K got '%s'",
				 got ? got : "(null)");
			bgtk_destroy_mock(kctx);
			return 1;
		}
		/* Ctrl+C byte for TTY */
		{
			char out[4];
			int n = bgtk_key_to_bytes(KEY_C, BGTK_MOD_CTRL, BGTK_KEY_TTY,
						 out, sizeof(out));
			if (n != 1 || out[0] != 3) {
				bgtk_log("headless: Ctrl+C TTY expected 0x03");
				bgtk_destroy_mock(kctx);
				return 1;
			}
		}
		/* L/R mod bits: press both ctrls, release one, still ctrl. */
		{
			struct BGTK_Context *mctx = bgtk_init_mock(100, 40);
			struct InputEvent me = {0};
			char out[4];
			int n;

			me.type = EV_KEY;
			me.value = 1;
			me.code = KEY_LEFTCTRL;
			bgtk_update_modifiers(mctx, me);
			me.code = KEY_RIGHTCTRL;
			bgtk_update_modifiers(mctx, me);
			me.value = 0;
			me.code = KEY_LEFTCTRL;
			bgtk_update_modifiers(mctx, me);
			if (!(bgtk_mods_from_ctx(mctx) & BGTK_MOD_CTRL)) {
				bgtk_log("headless: ctrl should stay held via right");
				bgtk_destroy_mock(mctx);
				return 1;
			}
			/* With ctrl held, j must be C0 (LF=10), not 'j' — this
			 * is what breaks vi when mod state is wrong. */
			n = bgtk_key_to_bytes(KEY_J, bgtk_mods_from_ctx(mctx),
					      BGTK_KEY_TTY, out, sizeof(out));
			if (n != 1 || out[0] != 10) {
				bgtk_log("headless: Ctrl+J TTY expected 0x0a got n=%d b=%d",
					 n, n > 0 ? (unsigned char)out[0] : -1);
				bgtk_destroy_mock(mctx);
				return 1;
			}
			bgtk_clear_modifiers(mctx);
			n = bgtk_key_to_bytes(KEY_J, bgtk_mods_from_ctx(mctx),
					      BGTK_KEY_TTY, out, sizeof(out));
			if (n != 1 || out[0] != 'j') {
				bgtk_log("headless: plain J after clear expected 'j'");
				bgtk_destroy_mock(mctx);
				return 1;
			}
			bgtk_destroy_mock(mctx);
		}

		/* Focused field receives keys even if tree walk would miss it
		 * (settings Theme page / nested HTML tables). */
		{
			struct BGTK_Context *fctx = bgtk_init_mock(300, 80);
			struct BGTK_Widget *fi =
				bgtk_text_input(fctx, "x", 120, 0,
						(BGTK_Options){.padding = 4});
			/* Nest the input under a frame so only focus routing
			 * (not a root-level walk of a free widget) is tested. */
			struct BGTK_Widget *ff =
				bgtk_frame(fctx, fi, 200, 50,
					   (BGTK_Options){.padding = 4});
			fctx->root_widget = ff;
			bgtk_set_focus(fctx, fi);
			struct InputEvent fk = {0};
			fk.type = EV_KEY;
			fk.value = 1;
			fk.code = KEY_Y;
			/* Deliberately zero coords — keyboard has no position. */
			fk.x = 0;
			fk.y = 0;
			bgtk_inject_event(fctx, fk);
			if (!fi->data.text_input.text ||
			    strcmp(fi->data.text_input.text, "xy") != 0) {
				bgtk_log("headless: focus-key routing expected 'xy' got '%s'",
					 fi->data.text_input.text
						 ? fi->data.text_input.text
						 : "(null)");
				bgtk_destroy_mock(fctx);
				return 1;
			}
			bgtk_destroy_mock(fctx);
		}

		/* Sticky mod: Ctrl down, focus leaves (release lost to other app
		 * or peer exited on Ctrl+C), focus returns — typing must work. */
		ke.value = 1;
		ke.code = KEY_LEFTCTRL;
		bgtk_inject_event(kctx, ke);
		if (!kctx->ctrl_held) {
			bgtk_log("headless: expected ctrl_held after press");
			bgtk_destroy_mock(kctx);
			return 1;
		}
		bgtk_set_window_focus(kctx, 0);
		bgtk_set_window_focus(kctx, 1);
		if (kctx->ctrl_held || kctx->shift_held || kctx->alt_held) {
			bgtk_log("headless: mods stuck after focus cycle "
				 "ctrl=%d shift=%d alt=%d",
				 kctx->ctrl_held, kctx->shift_held,
				 kctx->alt_held);
			bgtk_destroy_mock(kctx);
			return 1;
		}
		ke.code = KEY_X;
		bgtk_inject_event(kctx, ke);
		got = kti->data.text_input.text;
		if (!got || strcmp(got, "x") != 0) {
			bgtk_log("headless: expected 'x' after sticky-ctrl fix got '%s'",
				 got ? got : "(null)");
			bgtk_destroy_mock(kctx);
			return 1;
		}
		bgtk_draw_widgets(kctx);
		take_screenshot(kctx, "headless_04d_focus_clears_mods.png");
		bgtk_destroy_mock(kctx);
	}

	bgtk_destroy_mock(ctx);

	/* 7. text_align showcase (separate scene, wider canvas) */
	if (run_align_scene() != 0)
		return 1;

	/* 8. text style (bold/italic) + vertical/horizontal rules */
	{
		struct BGTK_Context *sctx = bgtk_init_mock(420, 160);
		struct BGTK_Widget *items_l[2], *items_r[2], *row_i[3], *col_i[3];
		struct BGTK_Widget *left, *right, *row, *col, *frame;
		struct BGTK_Widget *vrule, *hrule;
		BGTK_Options pad = {.padding = 4, .margin = 2};

		if (!sctx) {
			fprintf(stderr, "headless: style scene init failed\n");
			return 1;
		}
		sctx->theme.highlight = 0xFF2A6F97;

		items_l[0] = bgtk_text(sctx, "plain", pad);
		items_l[1] = bgtk_text(sctx, "bold",
			(BGTK_Options){.padding = 4, .margin = 2,
				       .text_style = BGTK_TEXT_BOLD});
		items_r[0] = bgtk_text(sctx, "italic",
			(BGTK_Options){.padding = 4, .margin = 2,
				       .text_style = BGTK_TEXT_ITALIC});
		items_r[1] = bgtk_text(sctx, "bold+italic",
			(BGTK_Options){.padding = 4, .margin = 2,
				       .text_style = BGTK_TEXT_BOLD |
						     BGTK_TEXT_ITALIC});
		left = bgtk_list(sctx, items_l, 2,
			(BGTK_Options){.orientation = BGTK_LIST_VERTICAL,
				       .margin = 2});
		right = bgtk_list(sctx, items_r, 2,
			(BGTK_Options){.orientation = BGTK_LIST_VERTICAL,
				       .margin = 2});
		vrule = bgtk_rule(sctx, BGTK_LIST_VERTICAL, 1,
				  (BGTK_Options){.margin = 6});
		vrule->h = 80;
		row_i[0] = left;
		row_i[1] = vrule;
		row_i[2] = right;
		row = bgtk_list(sctx, row_i, 3,
			(BGTK_Options){.orientation = BGTK_LIST_HORIZONTAL,
				       .margin = 4});
		hrule = bgtk_rule(sctx, BGTK_LIST_HORIZONTAL, 1,
				  (BGTK_Options){.margin = 4});
		hrule->w = 380;
		col_i[0] = row;
		col_i[1] = hrule;
		col_i[2] = bgtk_text(sctx, "rules + styles", pad);
		col = bgtk_list(sctx, col_i, 3,
			(BGTK_Options){.orientation = BGTK_LIST_VERTICAL,
				       .margin = 4});
		frame = bgtk_frame(sctx, col, 420, 160,
				   (BGTK_Options){.padding = 4});
		sctx->root_widget = frame;
		bgtk_draw_widgets(sctx);
		take_screenshot(sctx, "headless_07_styles_rules.png");
		bgtk_destroy_mock(sctx);
	}

	/* 08: Binary switch pill (left/right labels + knob). */
	{
		struct BGTK_Context *sctx = bgtk_init_mock(360, 120);
		struct BGTK_Widget *sw0, *sw1, *col, *frame;
		struct BGTK_Widget *items[3];
		struct InputEvent click = {0};

		if (!sctx) {
			fprintf(stderr, "headless: switch scene init failed\n");
			return 1;
		}
		sw0 = bgtk_switch(sctx, "Color", "Image", 0, NULL, NULL,
				  (BGTK_Options){.padding = 4, .margin = 4});
		sw1 = bgtk_switch(sctx, "Scaled", "Tiled", 1, NULL, NULL,
				  (BGTK_Options){.padding = 4, .margin = 4});
		items[0] = bgtk_text(sctx, "switches",
				     (BGTK_Options){.padding = 4, .margin = 2});
		items[1] = sw0;
		items[2] = sw1;
		col = bgtk_list(sctx, items, 3,
			(BGTK_Options){.orientation = BGTK_LIST_VERTICAL,
				       .margin = 4, .padding = 4});
		frame = bgtk_frame(sctx, col, 360, 120,
				   (BGTK_Options){.padding = 8});
		sctx->root_widget = frame;
		bgtk_draw_widgets(sctx);
		take_screenshot(sctx, "headless_08_switch_left.png");
		/* Click right half of first switch → value 1. */
		if (sw0) {
			click.type = EV_KEY;
			click.code = BTN_LEFT;
			click.value = 1;
			click.x = sw0->x + sw0->w * 3 / 4;
			click.y = sw0->y + sw0->h / 2;
			/* Positions assigned during draw; re-draw positions. */
			bgtk_draw_widgets(sctx);
			click.x = sw0->x + sw0->w * 3 / 4;
			click.y = sw0->y + sw0->h / 2;
			bgtk_inject_event(sctx, click);
			if (sw0->data.switch_w.value != 1) {
				fprintf(stderr,
					"headless: switch click expected 1 got %d\n",
					sw0->data.switch_w.value);
				bgtk_destroy_mock(sctx);
				return 1;
			}
			take_screenshot(sctx, "headless_08b_switch_right.png");
		}
		bgtk_destroy_mock(sctx);
	}

	printf("headless test complete. PNG frames written.\n");
	return 0;
}
