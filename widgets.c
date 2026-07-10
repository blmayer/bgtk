#include <bgce.h>
#include <linux/input.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bgtk.h"
#include "internal.h"

void bgtk_widget_screen_pos(const struct BGTK_Widget *w, int *x, int *y)
{
	int sx = 0, sy = 0;

	if (w) {
		if (w->flags & BGTK_FLAG_RELATIVE) {
			sx = w->abs_x;
			sy = w->abs_y;
		} else {
			sx = w->x;
			sy = w->y;
		}
	}
	if (x)
		*x = sx;
	if (y)
		*y = sy;
}

int bgtk_widget_hit(const struct BGTK_Widget *w, int x, int y)
{
	int sx, sy;

	if (!w)
		return 0;
	bgtk_widget_screen_pos(w, &sx, &sy);
	return x >= sx && x < sx + w->w && y >= sy && y < sy + w->h;
}

void bgtk_widget_set_parent(struct BGTK_Widget *child,
			    struct BGTK_Widget *parent)
{
	if (child)
		child->parent = parent;
}

// Default event handler for widgets
static int
default_handle_event(struct BGTK_Widget *widget, struct InputEvent ev)
{
	if (bgtk_widget_hit(widget, ev.x, ev.y)) {
		if (ev.code == BTN_LEFT && ev.value == 1) {
			bgtk_set_focus(widget->ctx, widget);
			return 1;
		}
	}
	return 0;		// Event not handled by this widget
}

// Button event handler
static int button_handle_event(struct BGTK_Widget *widget, struct InputEvent ev)
{
	int inside = bgtk_widget_hit(widget, ev.x, ev.y);

	// Mouse down: set pressed state if inside.
	if (ev.code == BTN_LEFT && ev.value == 1) {
		if (!inside) {
			return 0;
		}
		bgtk_set_focus(widget->ctx, widget);

		if (!widget->data.button.pressed) {
			widget->data.button.pressed = 1;
			if (widget->ctx) {
				bgtk_draw_widgets(widget->ctx);
			}
		}
		return 1;
	}
	// Mouse up: always clear pressed state; only fire callback if released
	// inside.
	if (ev.code == BTN_LEFT && ev.value == 0) {
		int was_pressed = widget->data.button.pressed;
		if (was_pressed) {
			widget->data.button.pressed = 0;
			if (widget->ctx) {
				bgtk_draw_widgets(widget->ctx);
			}
		}

		if (was_pressed && inside) {
			if (widget->data.button.callback) {
				widget->data.button.callback(widget->data.button.cb_data);
			}
			return 1;
		}

		return was_pressed ? 1 : 0;
	}

	return 0;		// Event not handled by this specific handler
}

// Sets the focused widget for keyboard input.
void bgtk_set_focus(struct BGTK_Context *ctx, struct BGTK_Widget *widget)
{
	ctx->focused_widget = widget;
	bgtk_draw_widgets(ctx);
}

/* Pixel width of text[0..len-1] (byte length; UTF-8 aware). */
static int measure_prefix_width(FT_Face face, const char *text, int len)
{
	return measure_text_prefix(face, text, len);
}

static void
text_input_ensure_cursor_visible(struct BGTK_Context *ctx,
				 struct BGTK_Widget *widget)
{
	if (!ctx || !ctx->ft_face || !widget) {
		return;
	}
	int content_w = widget->w - 2 * (widget->margin + widget->padding);
	if (content_w < 1) {
		content_w = 1;
	}
	int cursor_px =
	    measure_prefix_width(ctx->ft_face, widget->data.text_input.text,
				 (int)widget->data.text_input.cursor_pos);

	int scroll_x = widget->data.text_input.scroll_x;
	if (scroll_x < 0) {
		scroll_x = 0;
	}
	// Keep a small right-side caret margin so the cursor isn't flush.
	int caret_margin = 2;

	if (cursor_px - scroll_x > content_w - caret_margin) {
		scroll_x = cursor_px - (content_w - caret_margin);
	}
	if (cursor_px - scroll_x < 0) {
		scroll_x = cursor_px;
	}
	if (scroll_x < 0) {
		scroll_x = 0;
	}
	widget->data.text_input.scroll_x = scroll_x;
}

// Text input event handler
static int
text_input_handle_event(struct BGTK_Widget *widget, struct InputEvent ev)
{
	// First check if the click is within the text input's bounds.
	// For non-pointer events (keyboard), coordinates may be 0/undefined,
	// so we only hit-test when a position is actually provided.
	if (ev.code == BTN_LEFT && ev.value == 1) {
		if (bgtk_widget_hit(widget, ev.x, ev.y)) {
			bgtk_set_focus(widget->ctx, widget);
			return 1;	// Event handled
		}
		return 0;	// Event not in this widget
	}

	int focused = (widget->ctx && widget->ctx->focused_widget == widget);

	// Only handle keyboard events if this widget is focused
	if (!focused) {
		return 0;
	}
	/* Press or autorepeat (not release). */
	if (ev.type != EV_KEY || (ev.value != 1 && ev.value != 2))
		return 0;

	/* Pure modifiers: already tracked on ctx; consume. */
	if (ev.code == KEY_LEFTSHIFT || ev.code == KEY_RIGHTSHIFT ||
	    ev.code == KEY_LEFTCTRL || ev.code == KEY_RIGHTCTRL ||
	    ev.code == KEY_LEFTALT || ev.code == KEY_RIGHTALT)
		return 1;

	struct BGTK_Context *ctx = widget->ctx;
	int mods = bgtk_mods_from_ctx(ctx);
	char *text = widget->data.text_input.text;
	int cursor = (int)widget->data.text_input.cursor_pos;
	int len = text ? (int)strlen(text) : 0;
	int changed = 0;

	if (ev.code == KEY_TAB) {
		if (widget->data.text_input.on_tab)
			widget->data.text_input.on_tab();
		if (ctx)
			bgtk_draw_widgets(ctx);
		return 1;
	}
	if (ev.code == KEY_ENTER || ev.code == KEY_KPENTER) {
		if (widget->data.text_input.on_enter)
			widget->data.text_input.on_enter();
		/* Skip redraw if the app already closed its BGCE connection
		 * (e.g. launcher after spawn) — draw would paint a black
		 * frame into a dying shm and race the compositor erase. */
		if (ctx && ctx->conn_fd >= 0)
			bgtk_draw_widgets(ctx);
		return 1;
	}

	/* Ctrl chords (never insert a letter under Ctrl). */
	if (mods & BGTK_MOD_CTRL) {
		if (ev.code == KEY_A) {
			widget->data.text_input.cursor_pos = 0;
			changed = 1;
		} else if (ev.code == KEY_E) {
			widget->data.text_input.cursor_pos = (uint32_t)len;
			changed = 1;
		} else if (ev.code == KEY_U && text && cursor > 0) {
			memmove(text, text + cursor, (size_t)(len - cursor) + 1);
			widget->data.text_input.cursor_pos = 0;
			changed = 1;
		} else if (ev.code == KEY_K && text && cursor < len) {
			text[cursor] = '\0';
			changed = 1;
		} else if ((ev.code == KEY_W || ev.code == KEY_BACKSPACE) &&
			   text && cursor > 0) {
			int i = cursor - 1;
			while (i > 0 && text[i] == ' ')
				i--;
			while (i > 0 && text[i - 1] != ' ')
				i--;
			memmove(text + i, text + cursor, (size_t)(len - cursor) + 1);
			widget->data.text_input.cursor_pos = (uint32_t)i;
			changed = 1;
		}
		/* Ctrl+C/V/X and other Ctrl+letter: consume, no insert */
		if (changed) {
			text_input_ensure_cursor_visible(ctx, widget);
			if (widget->data.text_input.on_change)
				widget->data.text_input.on_change();
			if (ctx)
				bgtk_draw_widgets(ctx);
		}
		return 1;
	}

	char bytes[8];
	int n = bgtk_key_to_bytes(ev.code, mods, BGTK_KEY_TEXT, bytes, sizeof(bytes));
	if (n == 1 && bytes[0] >= 32 && bytes[0] < 127) {
		char ascii = bytes[0];
		text = realloc(widget->data.text_input.text, (size_t)len + 2);
		if (!text)
			return 0;
		cursor = (int)widget->data.text_input.cursor_pos;
		len = (int)strlen(text);
		memmove(&text[cursor + 1], &text[cursor], (size_t)(len - cursor) + 1);
		text[cursor] = ascii;
		widget->data.text_input.text = text;
		widget->data.text_input.cursor_pos = (uint32_t)(cursor + 1);
		text_input_ensure_cursor_visible(ctx, widget);
		if (widget->data.text_input.on_change)
			widget->data.text_input.on_change();
		if (ctx)
			bgtk_draw_widgets(ctx);
		return 1;
	}

	if (ev.code == KEY_BACKSPACE && text && cursor > 0) {
		memmove(&text[cursor - 1], &text[cursor], (size_t)(len - cursor) + 1);
		widget->data.text_input.cursor_pos--;
		text_input_ensure_cursor_visible(ctx, widget);
		if (widget->data.text_input.on_change)
			widget->data.text_input.on_change();
		if (ctx)
			bgtk_draw_widgets(ctx);
		return 1;
	}
	if (ev.code == KEY_DELETE && text && cursor < len) {
		memmove(&text[cursor], &text[cursor + 1], (size_t)(len - cursor));
		text_input_ensure_cursor_visible(ctx, widget);
		if (widget->data.text_input.on_change)
			widget->data.text_input.on_change();
		if (ctx)
			bgtk_draw_widgets(ctx);
		return 1;
	}
	if (ev.code == KEY_LEFT && cursor > 0) {
		widget->data.text_input.cursor_pos--;
		text_input_ensure_cursor_visible(ctx, widget);
		if (ctx)
			bgtk_draw_widgets(ctx);
		return 1;
	}
	if (ev.code == KEY_RIGHT && cursor < len) {
		widget->data.text_input.cursor_pos++;
		text_input_ensure_cursor_visible(ctx, widget);
		if (ctx)
			bgtk_draw_widgets(ctx);
		return 1;
	}
	if (ev.code == KEY_HOME) {
		widget->data.text_input.cursor_pos = 0;
		text_input_ensure_cursor_visible(ctx, widget);
		if (ctx)
			bgtk_draw_widgets(ctx);
		return 1;
	}
	if (ev.code == KEY_END) {
		widget->data.text_input.cursor_pos = (uint32_t)len;
		text_input_ensure_cursor_visible(ctx, widget);
		if (ctx)
			bgtk_draw_widgets(ctx);
		return 1;
	}

	return 0;
}

/* Clamp scroll_y into [0, max]; return 1 if value changed. */
static int scrollable_nudge(struct BGTK_Widget *widget, int delta)
{
	int old = widget->data.scrollable.scroll_y;
	int max_y = widget->data.scrollable.content_height - widget->h;

	if (max_y < 0)
		max_y = 0;
	widget->data.scrollable.scroll_y = old + delta;
	if (widget->data.scrollable.scroll_y < 0)
		widget->data.scrollable.scroll_y = 0;
	if (widget->data.scrollable.scroll_y > max_y)
		widget->data.scrollable.scroll_y = max_y;
	return widget->data.scrollable.scroll_y != old;
}

// Scrollable event handler
static int scrollable_handle_event(struct BGTK_Widget *widget,
				   struct InputEvent ev)
{
	int sx, sy;
	int in_bounds = bgtk_widget_hit(widget, ev.x, ev.y);

	bgtk_widget_screen_pos(widget, &sx, &sy);

	/* Keyboard scroll when this widget (or nothing texty) has focus. */
	if (ev.type == EV_KEY && (ev.value == 1 || ev.value == 2) &&
	    ev.code < BTN_MISC) {
		int step = 24;
		int page = widget->h > 40 ? widget->h - 24 : 40;
		int max_y = widget->data.scrollable.content_height - widget->h;
		int handled = 0;

		if (max_y < 0)
			max_y = 0;
		if (ev.code == KEY_DOWN || ev.code == KEY_J)
			handled = scrollable_nudge(widget, step);
		else if (ev.code == KEY_UP || ev.code == KEY_K)
			handled = scrollable_nudge(widget, -step);
		else if (ev.code == KEY_PAGEDOWN || ev.code == KEY_SPACE)
			handled = scrollable_nudge(widget, page);
		else if (ev.code == KEY_PAGEUP)
			handled = scrollable_nudge(widget, -page);
		else if (ev.code == KEY_HOME) {
			handled = (widget->data.scrollable.scroll_y != 0);
			widget->data.scrollable.scroll_y = 0;
		} else if (ev.code == KEY_END) {
			handled = (widget->data.scrollable.scroll_y != max_y);
			widget->data.scrollable.scroll_y = max_y;
		} else {
			/* Forward other keys to children (e.g. link activation). */
			for (int i = 0; i < widget->data.scrollable.widget_count;
			     i++) {
				struct BGTK_Widget *child =
					widget->data.scrollable.items[i];
				if (child->handle_event &&
				    child->handle_event(child, ev))
					return 1;
			}
			return 0;
		}
		/* Caller (main loop / inject) redraws — no double-draw here. */
		return handled ? 1 : 0;
	}

	/* Wheel: allow if pointer is over us (BGCE attaches cursor x,y). */
	if (ev.type == EV_REL && ev.code == REL_WHEEL) {
		if (!in_bounds)
			return 0;
		/* value > 0 = scroll up = content moves down (smaller scroll_y) */
		if (!scrollable_nudge(widget, -(ev.value * 32)))
			return 0;
		return 1;
	}

	if (!in_bounds)
		return 0;

	/* Pass pointer events to children in content coordinates. */
	{
		struct InputEvent cev = ev;
		cev.x = ev.x - sx;
		cev.y = ev.y - sy + widget->data.scrollable.scroll_y;

		for (int i = 0; i < widget->data.scrollable.widget_count; i++) {
			struct BGTK_Widget *child =
				widget->data.scrollable.items[i];

			/* Scroll content children are always content-relative. */
			if (!(cev.x >= child->x && cev.x < child->x + child->w &&
			      cev.y >= child->y && cev.y < child->y + child->h))
				continue;

			if (child->handle_event && child->handle_event(child, cev))
				return 1;
			break;
		}
	}

	if (ev.type == EV_KEY && ev.code == BTN_LEFT && ev.value == 1) {
		bgtk_set_focus(widget->ctx, widget);
		return 1;
	}

	return 0;
}

// Frame event handler
static int frame_handle_event(struct BGTK_Widget *widget, struct InputEvent ev)
{
	struct BGTK_Widget *child = widget->data.frame.child;

	// Keyboard events don't have meaningful pointer coordinates.
	// Forward them to the child without bounds checking.
	if (ev.type == EV_KEY) {
		if (child)
			return child->handle_event(child, ev);
		return 0;
	}
	/* Wheel uses absolute x,y from BGCE; forward into child tree. */
	if (ev.type == EV_REL && ev.code == REL_WHEEL && child &&
	    child->handle_event)
		return child->handle_event(child, ev);
	if (!bgtk_widget_hit(widget, ev.x, ev.y))
		return 0;
	// Pass event to child widget if it exists and the point is in the child
	if (child) {
		if (bgtk_widget_hit(child, ev.x, ev.y)) {
			if (child->handle_event(child, ev))
				return 1;
		}
		/* Click on cell padding (common in HTML tables): focus a
		 * text-input child so the user can type without a pixel-perfect
		 * hit on the field. */
		if (ev.code == BTN_LEFT && ev.value == 1 &&
		    child->type == BGTK_WIDGET_TEXT_INPUT) {
			bgtk_set_focus(widget->ctx, child);
			return 1;
		}
	}
	// If clicked inside the frame but not on the child, focus the frame.
	if (ev.code == BTN_LEFT && ev.value == 1) {
		bgtk_set_focus(widget->ctx, widget);
		return 1;
	}

	return 0;		// Event not handled
}

// Helper to create a generic widget
static struct BGTK_Widget *widget_new(struct BGTK_Context *ctx,
				      enum BGTK_Widget_Type type,
				      BGTK_Options options)
{
	struct BGTK_Widget *widget =
	    (struct BGTK_Widget *)calloc(1, sizeof(struct BGTK_Widget));
	if (!widget) {
		perror("calloc");
		return NULL;
	}
	widget->ctx = ctx;
	widget->type = type;
	widget->flags = options.flags;
	widget->padding = options.padding;
	widget->margin = options.margin;
	widget->text_align = options.text_align;
	widget->text_v_align = options.text_v_align;
	widget->baseline_offset = options.baseline_offset;
	widget->w = 0;
	widget->h = 0;

	// Set the default event handler
	widget->handle_event = default_handle_event;

	return widget;
}

void set_label(struct BGTK_Widget *widget, char *label)
{
	if (widget->data.label.text) {
		free(widget->data.label.text->data.text.text);
		free(widget->data.label.text);
	}
	// Create a new text widget for the label (inherit alignment)
	struct BGTK_Widget *text_widget =
	    bgtk_text(widget->ctx, label,
		      (BGTK_Options) {.text_align = widget->text_align });
	if (!text_widget) {
		perror("BGTK Failed to create text widget for " "label");
		return;
	}

	widget->data.label.text = text_widget;

	// Calculate size based on text widget and padding + margin (outer size)
	widget->w = text_widget->w + 2 * (widget->padding + widget->margin);
	widget->h = text_widget->h + 2 * (widget->padding + widget->margin);
	draw_widget(widget->ctx, widget, widget->ctx->shm_buffer);
}

struct BGTK_Widget *bgtk_label(struct BGTK_Context *ctx, char *text,
			       BGTK_Options options)
{
	struct BGTK_Widget *widget =
	    widget_new(ctx, BGTK_WIDGET_LABEL, options);
	if (!widget) {
		perror("BGTK Failed to create new widget");
		return NULL;
	}

	widget->data.label.set_label = set_label;

	// Create a text widget for the label (inherit alignment)
	struct BGTK_Widget *text_widget =
	    bgtk_text(ctx, text,
		      (BGTK_Options) {.text_align = options.text_align });
	if (!text_widget) {
		perror("BGTK Failed to create text widget for " "label");
		free(widget);
		return NULL;
	}

	widget->data.label.text = text_widget;

	// Calculate size based on text widget and padding + margin (outer size)
	widget->w = text_widget->w + 2 * (widget->padding + widget->margin);
	widget->h = text_widget->h + 2 * (widget->padding + widget->margin);
	return widget;
}

struct BGTK_Widget *bgtk_text(struct BGTK_Context *ctx, char *text,
			      BGTK_Options options)
{
	struct BGTK_Widget *widget = widget_new(ctx, BGTK_WIDGET_TEXT, options);
	if (!widget) {
		perror("BGTK Failed to create new widget");
		return NULL;
	}

	char *ptr = calloc(1, strlen(text) + 1);
	sprintf(ptr, "%s", text);
	widget->data.text.text = ptr;
	widget->data.text.header_level = 0;
	widget->data.text.style = options.text_style;

	// Calculate size based on text
	measure_text_style(widget->ctx->ft_face, widget->data.text.text,
			   widget->data.text.style, &widget->w, &widget->h);

	// Add padding + margin to the text widget (outer size)
	widget->w += 2 * (widget->padding + widget->margin);
	widget->h += 2 * (widget->padding + widget->margin);
	return widget;
}

static int rule_handle_event(struct BGTK_Widget *widget, struct InputEvent ev)
{
	(void)widget;
	(void)ev;
	return 0;
}

struct BGTK_Widget *bgtk_rule(struct BGTK_Context *ctx,
			      enum BGTK_List_Orientation orientation,
			      int thickness, BGTK_Options options)
{
	struct BGTK_Widget *w = widget_new(ctx, BGTK_WIDGET_RULE, options);
	if (!w)
		return NULL;
	if (thickness < 1)
		thickness = 1;
	w->data.rule.orientation = orientation;
	w->data.rule.thickness = thickness;
	w->data.rule.color = 0;
	w->handle_event = rule_handle_event;
	if (orientation == BGTK_LIST_VERTICAL) {
		w->w = thickness + 2 * (w->padding + w->margin);
		w->h = thickness + 2 * (w->padding + w->margin);
	} else {
		w->w = thickness + 2 * (w->padding + w->margin);
		w->h = thickness + 2 * (w->padding + w->margin);
	}
	return w;
}

/* Click left of center → value 0; right → 1. */
static int switch_handle_event(struct BGTK_Widget *widget, struct InputEvent ev)
{
	int inside, mid, new_v;

	inside = bgtk_widget_hit(widget, ev.x, ev.y);
	if (ev.code != BTN_LEFT || ev.value != 1 || !inside)
		return 0;

	/* Center of content box (labels + track). */
	{
		int sx, sy;
		bgtk_widget_screen_pos(widget, &sx, &sy);
		mid = sx + widget->margin + widget->padding +
		      (widget->w - 2 * (widget->margin + widget->padding)) / 2;
	}
	new_v = (ev.x >= mid) ? 1 : 0;
	if (new_v != widget->data.switch_w.value) {
		widget->data.switch_w.value = new_v;
		if (widget->data.switch_w.callback)
			widget->data.switch_w.callback(
				widget->data.switch_w.cb_data);
		if (widget->ctx)
			bgtk_draw_widgets(widget->ctx);
	}
	return 1;
}

struct BGTK_Widget *bgtk_switch(struct BGTK_Context *ctx, const char *left,
				const char *right, int value,
				BGTK_Callback callback, void *cb_data,
				BGTK_Options options)
{
	struct BGTK_Widget *w;
	int lw = 0, lh = 0, rw = 0, rh = 0;
	int track_h, gap, kn, fs;

	w = widget_new(ctx, BGTK_WIDGET_SWITCH, options);
	if (!w)
		return NULL;
	w->data.switch_w.left = strdup(left ? left : "");
	w->data.switch_w.right = strdup(right ? right : "");
	w->data.switch_w.value = value ? 1 : 0;
	w->data.switch_w.callback = callback;
	w->data.switch_w.cb_data = cb_data;
	w->handle_event = switch_handle_event;

	if (ctx && ctx->ft_face) {
		FT_Set_Pixel_Sizes(ctx->ft_face, 0,
				   ctx->font_size > 0 ? ctx->font_size : 14);
		measure_text(ctx->ft_face, w->data.switch_w.left, &lw, &lh);
		measure_text(ctx->ft_face, w->data.switch_w.right, &rw, &rh);
	} else {
		fs = (ctx && ctx->font_size > 0) ? ctx->font_size : 14;
		lh = rh = fs;
		lw = (int)strlen(w->data.switch_w.left) * (fs / 2 + 1);
		rw = (int)strlen(w->data.switch_w.right) * (fs / 2 + 1);
	}
	if (lh < rh)
		lh = rh;
	/* left_label | gap | track | gap | right_label */
	fs = (ctx && ctx->font_size > 0) ? ctx->font_size : 14;
	kn = fs + 2;
	if (kn < 12)
		kn = 12;
	gap = 8;
	track_h = kn + 6;
	/* Track ≈ 3× knob — readable without dominating the row. */
	w->w = lw + rw + kn * 3 + gap * 2 + 2 * (w->padding + w->margin);
	w->h = (track_h > lh ? track_h : lh) + 2 * (w->padding + w->margin);
	return w;
}

struct BGTK_Widget *bgtk_spacer(struct BGTK_Context *ctx, int min_w, int min_h,
				BGTK_Options options)
{
	struct BGTK_Widget *w = widget_new(ctx, BGTK_WIDGET_SPACER, options);

	if (!w)
		return NULL;
	w->w = min_w > 0 ? min_w : 0;
	w->h = min_h > 0 ? min_h : 0;
	return w;
}

struct BGTK_Widget *bgtk_button(struct BGTK_Context *ctx,
				struct BGTK_Widget *label,
				BGTK_Callback callback, void *cb_data,
				BGTK_Options options)
{
	struct BGTK_Widget *widget =
	    widget_new(ctx, BGTK_WIDGET_BUTTON, options);
	if (!widget) {
		perror("BGTK Failed to create new widget");
		return NULL;
	}

	widget->data.button.callback = callback;
	widget->data.button.cb_data = cb_data;
	widget->data.button.label = label;
	widget->data.button.pressed = 0;
	widget->data.button.bg_override = 0;
	widget->data.button.border_w = -1; /* theme default */

	// Override the default event handler with button-specific one
	widget->handle_event = button_handle_event;

	// Calculate size based on label widget and padding + margin (outer
	// size)
	widget->w = label->w + 2 * (widget->padding + widget->margin);
	widget->h = label->h + 2 * (widget->padding + widget->margin);
	bgtk_widget_set_parent(label, widget);
	return widget;
}

struct BGTK_Widget *bgtk_scrollable(struct BGTK_Context *ctx,
				    struct BGTK_Widget **items,
				    int widget_count, BGTK_Options options)
{
	struct BGTK_Widget *widget =
	    widget_new(ctx, BGTK_WIDGET_SCROLLABLE, options);
	if (!widget) {
		perror("BGTK Failed to create scrollable widget");
		return NULL;
	}

	widget->data.scrollable.items =
	    (struct BGTK_Widget **)calloc(widget_count,
					  sizeof(struct BGTK_Widget *));
	if (!widget->data.scrollable.items) {
		perror("calloc");
		free(widget);
		return NULL;
	}
	// Copy the input widgets into the scrollable container
	widget->data.scrollable.widget_count = widget_count;
	widget->data.scrollable.scroll_y = 0;
	widget->data.scrollable.content_height = 0;
	for (int i = 0; i < widget_count; i++) {
		widget->data.scrollable.items[i] = items[i];
		bgtk_widget_set_parent(items[i], widget);
		widget->data.scrollable.content_height += items[i]->h + 5 + 2 * widget->margin;	// 5px spacing + margin
	}

	// Initialize tmp buffer to NULL, it will be allocated during drawing
	widget->data.scrollable.tmp = NULL;
	widget->data.scrollable.widget_capacity = 0;
	// Override the default event handler with scrollable-specific one
	widget->handle_event = scrollable_handle_event;

	return widget;
}

// List widget event handler (no scrolling - just passes events to children)
static int
list_widget_handle_event(struct BGTK_Widget *widget, struct InputEvent ev)
{
	// Keyboard events don't have meaningful pointer coordinates.
	// Forward them to children without bounds checking.
	if (ev.type == EV_KEY && ev.code < BTN_MISC) {
		for (int i = 0; i < widget->data.list_widget.widget_count; i++) {
			struct BGTK_Widget *child =
			    widget->data.list_widget.items[i];
			if (child->handle_event(child, ev)) {
				return 1;	// Event was handled by a child
			}
		}
	}
	/* Wheel: forward to children that contain the pointer. */
	if (ev.type == EV_REL && ev.code == REL_WHEEL) {
		for (int i = 0; i < widget->data.list_widget.widget_count; i++) {
			struct BGTK_Widget *child =
				widget->data.list_widget.items[i];
			if (bgtk_widget_hit(child, ev.x, ev.y) &&
			    child->handle_event &&
			    child->handle_event(child, ev))
				return 1;
		}
		return 0;
	}
	if (!bgtk_widget_hit(widget, ev.x, ev.y))
		return 0;
	/* Screen coordinates (or content-relative inside scrollables). */
	for (int i = 0; i < widget->data.list_widget.widget_count; i++) {
		struct BGTK_Widget *child = widget->data.list_widget.items[i];

		if (!bgtk_widget_hit(child, ev.x, ev.y))
			continue;

		if (child->handle_event(child, ev)) {
			return 1;	// Event was handled by a child
		}
		// Coordinates matched this child, but it didn't handle the
		// event.
		break;
	}

	if (ev.code == BTN_LEFT && ev.value == 1) {
		bgtk_set_focus(widget->ctx, widget);
		return 1;
	}

	return 0;		// Event not handled
}

struct BGTK_Widget *bgtk_list(struct BGTK_Context *ctx,
			      struct BGTK_Widget **items, int widget_count,
			      BGTK_Options options)
{
	struct BGTK_Widget *widget = widget_new(ctx, BGTK_WIDGET_LIST, options);
	if (!widget) {
		perror("BGTK Failed to create list widget");
		return NULL;
	}

	widget->data.list_widget.items =
	    (struct BGTK_Widget **)calloc(widget_count,
					  sizeof(struct BGTK_Widget *));
	if (!widget->data.list_widget.items) {
		perror("calloc");
		free(widget);
		return NULL;
	}
	// Copy the input widgets into the list container
	widget->data.list_widget.widget_count = widget_count;
	widget->data.list_widget.orientation = options.orientation;

	/* Content size matches draw_list:
	 * children at padding; gaps of 2*margin between items; outer 2*padding. */
	int max_width = 0;
	int max_height = 0;
	int pad2 = 2 * widget->padding;
	for (int i = 0; i < widget_count; i++) {
		widget->data.list_widget.items[i] = items[i];
		bgtk_widget_set_parent(items[i], widget);
		if (options.orientation == BGTK_LIST_VERTICAL) {
			widget->h += items[i]->h + 2 * widget->margin;
			if (items[i]->w > max_width) {
				max_width = items[i]->w;
			}
		} else {	// BGTK_LIST_HORIZONTAL
			widget->w += items[i]->w + 2 * widget->margin;
			if (items[i]->h > max_height) {
				max_height = items[i]->h;
			}
		}
	}

	// Remove the last inter-item margin (no gap after the last widget)
	if (widget_count > 0) {
		if (options.orientation == BGTK_LIST_VERTICAL) {
			widget->h -= 2 * widget->margin;
		} else {
			widget->w -= 2 * widget->margin;
		}
	}
	/* Outer padding only; margin is inter-item gap (not side inset). */
	if (options.orientation == BGTK_LIST_VERTICAL) {
		widget->w = max_width + pad2;
		widget->h += pad2;
	} else {
		widget->h = max_height + pad2;
		widget->w += pad2;
	}

	// Override the default event handler with list-specific one
	widget->handle_event = list_widget_handle_event;

	return widget;
}

struct BGTK_Widget *bgtk_image(struct BGTK_Context *ctx, const char *path,
			       int width, int height, BGTK_Options options)
{
	struct BGTK_Widget *widget =
	    widget_new(ctx, BGTK_WIDGET_IMAGE, options);
	if (!widget) {
		perror("BGTK Failed to create image widget");
		return NULL;
	}
	// Set requested widget size (outer size). If a dimension is 0, we will
	// fill it from the image's intrinsic size once loaded.
	widget->w = width;
	widget->h = height;

	if (!path) {
		return widget;
	}
	// Load the image into a pixel buffer
	load_image(path, &widget->data.image.pixels, &widget->data.image.img_w,
		   &widget->data.image.img_h);

	// Default size to intrinsic image size (plus padding+margin) when not
	// explicitly provided.
	if (widget->w == 0) {
		widget->w = widget->data.image.img_w +
		    2 * (widget->padding + widget->margin);
	}
	if (widget->h == 0) {
		widget->h = widget->data.image.img_h +
		    2 * (widget->padding + widget->margin);
	}

	return widget;
}

struct BGTK_Widget *bgtk_frame(struct BGTK_Context *ctx,
			       struct BGTK_Widget *child, int width, int height,
			       BGTK_Options options)
{
	struct BGTK_Widget *frame = widget_new(ctx, BGTK_WIDGET_FRAME, options);
	if (!frame) {
		perror("BGTK Failed to create frame widget");
		return NULL;
	}

	frame->w = width;
	frame->h = height;

	frame->data.frame.child = child;
	frame->data.frame.border_w = ctx->theme.frame_border_size;
	bgtk_widget_set_parent(child, frame);

	// Set the event handler for the frame
	frame->handle_event = frame_handle_event;

	return frame;
}

// Creates a text input widget.
struct BGTK_Widget *bgtk_text_input(struct BGTK_Context *ctx,
				    char *initial_text, int width, int height,
				    BGTK_Options options)
{
	struct BGTK_Widget *widget =
	    widget_new(ctx, BGTK_WIDGET_TEXT_INPUT, options);
	if (!widget) {
		perror("BGTK Failed to create text input widget");
		return NULL;
	}
	// Initialize text input data
	widget->data.text_input.text = strdup(initial_text ? initial_text : "");
	widget->data.text_input.cursor_pos =
	    strlen(widget->data.text_input.text);
	widget->data.text_input.selection_start = -1;
	widget->data.text_input.selection_end = -1;
	widget->data.text_input.on_change = NULL;
	widget->data.text_input.on_tab = NULL;
	widget->data.text_input.on_enter = NULL;
	widget->data.text_input.border_w = -1; /* theme default */

	// Override the default event handler with text input-specific one
	widget->handle_event = text_input_handle_event;

	// Set widget size.
	// width/height are the INNER dimensions (content box).
	// If width and/or height is 0, that dimension is auto-sized based on
	// the current font metrics / initial content.
	// The widget outer dimensions include padding + margin.
	int inner_w = width;
	int inner_h = height;

	if (inner_w == 0 || inner_h == 0) {
		int text_w = 0;
		int text_h = 0;
		const char *sample = widget->data.text_input.text;
		// For empty text, measure a single space so height is non-zero.
		if (!sample || sample[0] == '\0') {
			sample = " ";
		}

		if (ctx && ctx->ft_face) {
			// Ensure the face metrics match the current font size.
			FT_Set_Pixel_Sizes(ctx->ft_face, 0, ctx->font_size);
			measure_text(ctx->ft_face, sample, &text_w, &text_h);
		} else {
			// Fallback: approximate.
			text_h = (ctx
				  && ctx->font_size > 0) ? ctx->font_size : 16;
			text_w = (int)strlen(sample) * (text_h / 2);
			if (text_w < 1) {
				text_w = 1;
			}
		}

		if (inner_w == 0) {
			// +2 accounts for the 1px border on each side used in
			// draw_widget().
			inner_w = text_w + 2;
		}
		if (inner_h == 0) {
			// +2 accounts for the 1px border on each side used in
			// draw_widget().
			inner_h = text_h + 2;
		}
	}

	if (inner_w < 1) {
		inner_w = 1;
	}
	if (inner_h < 1) {
		inner_h = 1;
	}

	widget->w = inner_w + 2 * (widget->padding + widget->margin);
	widget->h = inner_h + 2 * (widget->padding + widget->margin);
	return widget;
}
