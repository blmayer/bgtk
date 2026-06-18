#include <bgce.h>
#include <linux/input.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bgtk.h"
#include "internal.h"

// Default event handler for widgets
static int
default_handle_event(struct BGTK_Widget *widget, struct InputEvent ev)
{
	// Check if the event coordinates are within the widget's bounds
	if (ev.x >= widget->x && ev.x < (widget->x + widget->w) &&
	    ev.y >= widget->y && ev.y < (widget->y + widget->h)) {
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
	int inside = (ev.x >= widget->x && ev.x < (widget->x + widget->w) &&
		      ev.y >= widget->y && ev.y < (widget->y + widget->h));

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
				widget->data.button.callback();
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

// Returns pixel width of text[0..len-1]
static int measure_prefix_width(FT_Face face, const char *text, int len)
{
	if (!face || !text || len <= 0) {
		return len > 0 ? len * 7 : 0;  /* crude fallback */
	}
	int width = 0;
	for (int i = 0; i < len && text[i]; i++) {
		if (FT_Load_Char(face, (unsigned char)text[i], FT_LOAD_DEFAULT)) {
			continue;
		}
		width += face->glyph->advance.x;
	}
	return width >> 6;
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
		if (ev.x >= widget->x && ev.x < (widget->x + widget->w) &&
		    ev.y >= widget->y && ev.y < (widget->y + widget->h)) {
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
	// Handle keyboard events
	if (ev.type == EV_KEY && ev.value == 1) {
		// Special keys handled via callbacks. Consume so they don't insert.
		if (ev.code == KEY_TAB) {
			if (widget->data.text_input.on_tab) {
				widget->data.text_input.on_tab();
				if (widget->ctx) {
					bgtk_draw_widgets(widget->ctx);
				}
			}
			return 1;
		}
		if (ev.code == KEY_ENTER || ev.code == KEY_KPENTER) {
			if (widget->data.text_input.on_enter) {
				widget->data.text_input.on_enter();
				if (widget->ctx) {
					bgtk_draw_widgets(widget->ctx);
				}
			}
			return 1;
		}

		// Translate linux input key codes to ASCII where possible.
		// IMPORTANT: KEY_* constants are not contiguous in a way that
		// matches ASCII ordering, so do explicit mapping.
		char ascii = 0;
		switch (ev.code) {
		case KEY_A:
			ascii = 'a';
			break;
		case KEY_B:
			ascii = 'b';
			break;
		case KEY_C:
			ascii = 'c';
			break;
		case KEY_D:
			ascii = 'd';
			break;
		case KEY_E:
			ascii = 'e';
			break;
		case KEY_F:
			ascii = 'f';
			break;
		case KEY_G:
			ascii = 'g';
			break;
		case KEY_H:
			ascii = 'h';
			break;
		case KEY_I:
			ascii = 'i';
			break;
		case KEY_J:
			ascii = 'j';
			break;
		case KEY_K:
			ascii = 'k';
			break;
		case KEY_L:
			ascii = 'l';
			break;
		case KEY_M:
			ascii = 'm';
			break;
		case KEY_N:
			ascii = 'n';
			break;
		case KEY_O:
			ascii = 'o';
			break;
		case KEY_P:
			ascii = 'p';
			break;
		case KEY_Q:
			ascii = 'q';
			break;
		case KEY_R:
			ascii = 'r';
			break;
		case KEY_S:
			ascii = 's';
			break;
		case KEY_T:
			ascii = 't';
			break;
		case KEY_U:
			ascii = 'u';
			break;
		case KEY_V:
			ascii = 'v';
			break;
		case KEY_W:
			ascii = 'w';
			break;
		case KEY_X:
			ascii = 'x';
			break;
		case KEY_Y:
			ascii = 'y';
			break;
		case KEY_Z:
			ascii = 'z';
			break;
		case KEY_1:
			ascii = '1';
			break;
		case KEY_2:
			ascii = '2';
			break;
		case KEY_3:
			ascii = '3';
			break;
		case KEY_4:
			ascii = '4';
			break;
		case KEY_5:
			ascii = '5';
			break;
		case KEY_6:
			ascii = '6';
			break;
		case KEY_7:
			ascii = '7';
			break;
		case KEY_8:
			ascii = '8';
			break;
		case KEY_9:
			ascii = '9';
			break;
		case KEY_0:
			ascii = '0';
			break;
		case KEY_SPACE:
			ascii = ' ';
			break;
		case KEY_MINUS:
			ascii = '-';
			break;
		case KEY_EQUAL:
			ascii = '=';
			break;
		case KEY_LEFTBRACE:
			ascii = '[';
			break;
		case KEY_RIGHTBRACE:
			ascii = ']';
			break;
		case KEY_BACKSLASH:
			ascii = '\\';
			break;
		case KEY_SEMICOLON:
			ascii = ';';
			break;
		case KEY_APOSTROPHE:
			ascii = '\'';
			break;
		case KEY_COMMA:
			ascii = ',';
			break;
		case KEY_DOT:
			ascii = '.';
			break;
		case KEY_SLASH:
			ascii = '/';
			break;
		default:
			break;
		}
		// Apply shift for uppercase letters (lean: only letters).
		if (ascii >= 'a' && ascii <= 'z' && widget->ctx &&
		    widget->ctx->shift_held) {
			ascii -= 32;
		}
		if (ascii) {
			// Insert character at cursor_pos
			char *text = widget->data.text_input.text;
			int cursor = widget->data.text_input.cursor_pos;
			int len = strlen(text);

			text = realloc(text, len + 2);
			if (!text) {
				return 0;
			}

			memmove(&text[cursor + 1], &text[cursor],
				len - cursor + 1);
			text[cursor] = ascii;
			widget->data.text_input.text = text;
			widget->data.text_input.cursor_pos++;
			text_input_ensure_cursor_visible(widget->ctx, widget);

			if (widget->data.text_input.on_change) {
				widget->data.text_input.on_change();
			}
			if (widget->ctx) {
				bgtk_draw_widgets(widget->ctx);
			}

			return 1;	// Event handled
		}
		// Handle backspace
		if (ev.code == KEY_BACKSPACE) {
			char *text = widget->data.text_input.text;
			int cursor = widget->data.text_input.cursor_pos;
			if (cursor > 0) {
				memmove(&text[cursor - 1], &text[cursor],
					strlen(text) - cursor + 1);
				widget->data.text_input.cursor_pos--;
				text_input_ensure_cursor_visible(widget->ctx,
								 widget);

				if (widget->data.text_input.on_change) {
					widget->data.text_input.on_change();
				}
				if (widget->ctx) {
					bgtk_draw_widgets(widget->ctx);
				}

				return 1;	// Event handled
			}
		}
		// Handle delete
		if (ev.code == KEY_DELETE) {
			char *text = widget->data.text_input.text;
			uint32_t cursor = widget->data.text_input.cursor_pos;
			if (cursor < strlen(text)) {
				memmove(&text[cursor], &text[cursor + 1],
					strlen(text) - cursor);
				text_input_ensure_cursor_visible(widget->ctx,
								 widget);

				if (widget->data.text_input.on_change) {
					widget->data.text_input.on_change();
				}
				if (widget->ctx) {
					bgtk_draw_widgets(widget->ctx);
				}

				return 1;	// Event handled
			}
		}
		// Handle arrow keys (cursor movement)
		if (ev.code == KEY_LEFT || ev.code == KEY_RIGHT) {
			int cursor = widget->data.text_input.cursor_pos;
			int len = strlen(widget->data.text_input.text);

			// Handle arrow keys (cursor movement)
			if (ev.code == KEY_LEFT || ev.code == KEY_RIGHT) {
				if (ev.code == KEY_LEFT && cursor > 0) {
					widget->data.text_input.cursor_pos--;
					text_input_ensure_cursor_visible
					    (widget->ctx, widget);
					if (widget->ctx) {
						bgtk_draw_widgets(widget->ctx);
					}
					return 1;	// Event handled
				} else if (ev.code == KEY_RIGHT && cursor < len) {
					widget->data.text_input.cursor_pos++;
					text_input_ensure_cursor_visible
					    (widget->ctx, widget);
					if (widget->ctx) {
						bgtk_draw_widgets(widget->ctx);
					}
					return 1;	// Event handled
				}
			}
		}
	}

	return 0;		// Event not handled by this specific handler
}

// Scrollable event handler
static int
scrollable_handle_event(struct BGTK_Widget *widget, struct InputEvent ev)
{
	// Keyboard events don't have meaningful pointer coordinates.
	// Forward them to children without bounds checking.
	if (ev.type == EV_KEY && ev.code < BTN_MISC) {
		for (int i = 0; i < widget->data.scrollable.widget_count; i++) {
			struct BGTK_Widget *child =
			    widget->data.scrollable.items[i];
			if (child->handle_event(child, ev)) {
				return 1;	// Event was handled by a child
			}
		}
	}
	// First check if the event is within the scrollable's bounds
	if (!(ev.x >= widget->x && ev.x < (widget->x + widget->w) &&
	      ev.y >= widget->y && ev.y < (widget->y + widget->h))) {
		return 0;	// Event not in this widget
	}
	// Pass event to child widget if it exists
	// Events are absolute screen coordinates, while children in the
	// scrollable are positioned in scrollable CONTENT coordinates.
	// Transform the event into content-space before forwarding.
	struct InputEvent cev = ev;
	cev.x = ev.x - widget->x;
	cev.y = ev.y - widget->y + widget->data.scrollable.scroll_y;

	for (int i = 0; i < widget->data.scrollable.widget_count; i++) {
		struct BGTK_Widget *child = widget->data.scrollable.items[i];

		if (!(cev.x >= child->x && cev.x < child->x + child->w &&
		      cev.y >= child->y && cev.y < child->y + child->h)) {
			continue;
		}

		if (child->handle_event(child, cev)) {
			return 1;	// Event was handled by a child
		}
		// Coordinates matched this child, but it didn't handle the
		// event. The scrollable should now try to handle it.
		break;
	}

	// Handle mouse wheel for scrolling
	// (wheel events come in as EV_REL / REL_WHEEL)
	if (ev.type == EV_REL && ev.code == REL_WHEEL) {
		int old_scroll_y = widget->data.scrollable.scroll_y;

		widget->data.scrollable.scroll_y -= ev.value * 10;	// Scroll speed

		// Clamp scroll_y to valid range
		if (widget->data.scrollable.scroll_y < 0) {
			widget->data.scrollable.scroll_y = 0;
		}
		if (widget->data.scrollable.scroll_y >
		    widget->data.scrollable.content_height - widget->h) {
			widget->data.scrollable.scroll_y =
			    widget->data.scrollable.content_height - widget->h;
		}
		if (widget->data.scrollable.scroll_y < 0) {
			widget->data.scrollable.scroll_y = 0;
		}

		if (widget->data.scrollable.scroll_y == old_scroll_y) {
			return 0;	// Nothing changed; avoid needless
			// redraw/blink
		}

		if (widget->ctx) {
			bgtk_draw_widgets(widget->ctx);
		}
		return 1;	// Event handled
	}

	if (ev.code == BTN_LEFT && ev.value == 1) {
		bgtk_set_focus(widget->ctx, widget);
		return 1;
	}

	return 0;		// Event not handled
}

// Frame event handler
static int frame_handle_event(struct BGTK_Widget *widget, struct InputEvent ev)
{
	// Keyboard events don't have meaningful pointer coordinates.
	// Forward them to the child without bounds checking.
	if (ev.type == EV_KEY) {
		if (widget->data.frame.child) {
			return widget->data.frame.child->handle_event(widget->
								      data.
								      frame.
								      child,
								      ev);
		}
	}
	// First check if the event is within the frame's bounds
	if (!(ev.x >= widget->x && ev.x < (widget->x + widget->w) &&
	      ev.y >= widget->y && ev.y < (widget->y + widget->h))) {
		return 0;	// Event not in this widget
	}
	// Pass event to child widget if it exists
	if (widget->data.frame.child) {
		// Events use absolute coordinates.
		// Only forward to the child if the event is within the child's
		// absolute bounds.
		int cx0 = widget->data.frame.child->x;
		int cy0 = widget->data.frame.child->y;
		int cx1 = cx0 + widget->data.frame.child->w;
		int cy1 = cy0 + widget->data.frame.child->h;

		if (ev.x >= cx0 && ev.x < cx1 && ev.y >= cy0 && ev.y < cy1) {
			return widget->data.frame.child->handle_event(widget->
								      data.
								      frame.
								      child,
								      ev);
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
	// Create a new text widget for the label
	struct BGTK_Widget *text_widget =
	    bgtk_text(widget->ctx, label, (BGTK_Options) {.flags = 0 });
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

	// Create a text widget for the label
	struct BGTK_Widget *text_widget =
	    bgtk_text(ctx, text, (BGTK_Options) {.flags = 0 });
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

	// Calculate size based on text
	measure_text(widget->ctx->ft_face, widget->data.text.text, &widget->w,
		     &widget->h);

	// Add padding + margin to the text widget (outer size)
	widget->w += 2 * (widget->padding + widget->margin);
	widget->h += 2 * (widget->padding + widget->margin);
	return widget;
}

struct BGTK_Widget *bgtk_button(struct BGTK_Context *ctx,
				struct BGTK_Widget *label,
				BGTK_Callback callback, BGTK_Options options)
{
	struct BGTK_Widget *widget =
	    widget_new(ctx, BGTK_WIDGET_BUTTON, options);
	if (!widget) {
		perror("BGTK Failed to create new widget");
		return NULL;
	}

	widget->data.button.callback = callback;
	widget->data.button.label = label;
	widget->data.button.pressed = 0;

	// Override the default event handler with button-specific one
	widget->handle_event = button_handle_event;

	// Calculate size based on label widget and padding + margin (outer
	// size)
	widget->w = label->w + 2 * (widget->padding + widget->margin);
	widget->h = label->h + 2 * (widget->padding + widget->margin);
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
		widget->data.scrollable.content_height += items[i]->h + 5 + 2 * widget->margin;	// 5px spacing + margin
	}

	// Initialize tmp buffer to NULL, it will be allocated during drawing
	widget->data.scrollable.tmp = NULL;
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
	// First check if the event is within the list's bounds
	if (!(ev.x >= widget->x && ev.x < (widget->x + widget->w) &&
	      ev.y >= widget->y && ev.y < (widget->y + widget->h))) {
		return 0;	// Event not in this widget
	}
	// Pass event to child widget if it exists
	// Events use absolute coordinates.
	for (int i = 0; i < widget->data.list_widget.widget_count; i++) {
		struct BGTK_Widget *child = widget->data.list_widget.items[i];

		if (!(ev.x >= child->x && ev.x < child->x + child->w &&
		      ev.y >= child->y && ev.y < child->y + child->h)) {
			continue;
		}

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

	// Calculate content dimensions based on orientation
	int max_width = 0;
	int max_height = 0;
	for (int i = 0; i < widget_count; i++) {
		widget->data.list_widget.items[i] = items[i];
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

	// Remove the last margin (no margin after the last widget)
	if (widget_count > 0) {
		if (options.orientation == BGTK_LIST_VERTICAL) {
			widget->h -= 2 * widget->margin;
		} else {
			widget->w -= 2 * widget->margin;
		}
	}
	// Use max dimensions for the other axis
	if (options.orientation == BGTK_LIST_VERTICAL) {
		widget->w = max_width;
	} else {
		widget->h = max_height;
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
