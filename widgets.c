#include <bgce.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bgtk.h"
#include "internal.h"

// Default event handler for widgets
static int default_handle_event(struct BGTK_Widget* widget, struct InputEvent ev) {
    // Check if the event coordinates are within the widget's bounds
    if (ev.x >= widget->x && ev.x < (widget->x + widget->w) &&
        ev.y >= widget->y && ev.y < (widget->y + widget->h)) {
        // Event is within this widget's bounds
        // For most widgets, we'll just return 1 to indicate the event was handled
        // at this level, but specific widgets can override this behavior
        return 1;
    }
    return 0; // Event not handled by this widget
}

// Button event handler
static int button_handle_event(struct BGTK_Widget* widget, struct InputEvent ev) {
    // First check if the event is within the button's bounds
    if (!(ev.x >= widget->x && ev.x < (widget->x + widget->w) &&
          ev.y >= widget->y && ev.y < (widget->y + widget->h))) {
        return 0; // Event not in this widget
    }
    
    // Handle mouse clicks
    if (ev.code == BTN_LEFT && ev.value == 1) {
        printf("Button clicked!\n");
        if (widget->data.button.callback) {
            widget->data.button.callback();
            return 1; // Event handled
        }
    }
    
    return 0; // Event not handled by this specific handler
}

// Text input event handler
static int text_input_handle_event(struct BGTK_Widget* widget, struct InputEvent ev) {
    // First check if the event is within the text input's bounds
    if (!(ev.x >= widget->x && ev.x < (widget->x + widget->w) &&
          ev.y >= widget->y && ev.y < (widget->y + widget->h))) {
        return 0; // Event not in this widget
    }
    
    // Handle mouse clicks (focus)
    if (ev.code == BTN_LEFT && ev.value == 1) {
        bgtk_set_focus(widget->ctx, widget);
        return 1; // Event handled
    }
    
    // Only handle keyboard events if this widget is focused
    if (widget->ctx->focused_widget != widget) {
        return 0;
    }
    
    // Handle keyboard events
    if (ev.type == EV_KEY && ev.value == 1) {
        // Handle printable characters
        if (ev.code < 256) {
            char c = (char)ev.code;
            if (c >= 32 && c <= 126) {  // Printable ASCII
                // Insert character at cursor_pos
                char* text = widget->data.text_input.text;
                int cursor = widget->data.text_input.cursor_pos;
                int len = strlen(text);
                
                // Resize text buffer
                text = realloc(text, len + 2);
                if (!text) {
                    return 0;
                }
                
                // Shift characters after cursor
                memmove(&text[cursor + 1], &text[cursor], len - cursor + 1);
                text[cursor] = c;
                widget->data.text_input.text = text;
                widget->data.text_input.cursor_pos++;
                
                if (widget->data.text_input.on_change) {
                    widget->data.text_input.on_change();
                }
                
                return 1; // Event handled
            }
        }
        
        // Handle backspace
        if (ev.code == KEY_BACKSPACE) {
            char* text = widget->data.text_input.text;
            int cursor = widget->data.text_input.cursor_pos;
            if (cursor > 0) {
                memmove(&text[cursor - 1], &text[cursor], strlen(text) - cursor + 1);
                widget->data.text_input.cursor_pos--;
                
                if (widget->data.text_input.on_change) {
                    widget->data.text_input.on_change();
                }
                
                return 1; // Event handled
            }
        }
        
        // Handle delete
        if (ev.code == KEY_DELETE) {
            char* text = widget->data.text_input.text;
            uint32_t cursor = widget->data.text_input.cursor_pos;
            if (cursor < strlen(text)) {
                memmove(&text[cursor], &text[cursor + 1], strlen(text) - cursor);
                
                if (widget->data.text_input.on_change) {
                    widget->data.text_input.on_change();
                }
                
                return 1; // Event handled
            }
        }
        
        // Handle arrow keys (cursor movement)
        if (ev.code == KEY_LEFT || ev.code == KEY_RIGHT) {
            int cursor = widget->data.text_input.cursor_pos;
            int len = strlen(widget->data.text_input.text);
            
            if (ev.code == KEY_LEFT && cursor > 0) {
                widget->data.text_input.cursor_pos--;
                return 1; // Event handled
            } else if (ev.code == KEY_RIGHT && cursor < len) {
                widget->data.text_input.cursor_pos++;
                return 1; // Event handled
            }
        }
    }
    
    return 0; // Event not handled by this specific handler
}

// Scrollable event handler
static int scrollable_handle_event(struct BGTK_Widget* widget, struct InputEvent ev) {
    // First check if the event is within the scrollable's bounds
    if (!(ev.x >= widget->x && ev.x < (widget->x + widget->w) &&
          ev.y >= widget->y && ev.y < (widget->y + widget->h))) {
        return 0; // Event not in this widget
    }
    
    // Handle mouse wheel for scrolling
    if (ev.code == REL_WHEEL) {
        widget->data.scrollable.scroll_y -= ev.value * 10;  // Scroll speed
        
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
        
        return 1; // Event handled
    }
    
    // Pass event to child widgets
    for (int i = 0; i < widget->data.scrollable.widget_count; i++) {
        struct BGTK_Widget* child = widget->data.scrollable.items[i];
        
        // Adjust event coordinates to be relative to the child widget
        struct InputEvent child_ev = ev;
        child_ev.x -= child->x;
        child_ev.y -= child->y;
        
        // Pass the event to the child
        if (child->handle_event(child, child_ev)) {
            return 1; // Event was handled by a child
        }
    }
    
    return 0; // Event not handled
}

// Frame event handler
static int frame_handle_event(struct BGTK_Widget* widget, struct InputEvent ev) {
    // First check if the event is within the frame's bounds
    if (!(ev.x >= widget->x && ev.x < (widget->x + widget->w) &&
          ev.y >= widget->y && ev.y < (widget->y + widget->h))) {
        return 0; // Event not in this widget
    }
    
    // Pass event to child widget if it exists
    if (widget->data.frame.child) {
        // Adjust event coordinates to be relative to the child widget
        struct InputEvent child_ev = ev;
        child_ev.x -= widget->data.frame.child->x;
        child_ev.y -= widget->data.frame.child->y;
        
        // Pass the event to the child
        return widget->data.frame.child->handle_event(widget->data.frame.child, child_ev);
    }
    
    return 0; // Event not handled
}

// Helper to create a generic widget
static struct BGTK_Widget* widget_new(struct BGTK_Context* ctx,
			  enum BGTK_Widget_Type type, BGTK_Options options) {
	struct BGTK_Widget* widget =
	    (struct BGTK_Widget*)calloc(1, sizeof(struct BGTK_Widget));
	if (!widget) {
		perror("calloc");
		return NULL;
	}
	widget->ctx = ctx;
	widget->type = type;
	widget->flags = options.flags;
	widget->padding = options.padding;
	widget->margin = options.margin;
	
	// Set the default event handler
	widget->handle_event = default_handle_event;
	
	return widget;
}

void set_label(struct BGTK_Widget* widget, char* label) {
	printf("BGTK: setting label: %s\n", label);
	if (widget->data.label.text) {
		free(widget->data.label.text->data.text.text);
		free(widget->data.label.text);
	}
	
	// Create a new text widget for the label
	struct BGTK_Widget* text_widget = bgtk_text(widget->ctx, label, (BGTK_Options){ .flags = 0 });
	if (!text_widget) {
		perror(
		    "BGTK Failed to create text widget for "
		    "label");
		return;
	}
	
	widget->data.label.text = text_widget;
	
	// Calculate size based on text widget and padding
	widget->w = text_widget->w + 2 * widget->padding;
	widget->h = text_widget->h + 2 * widget->padding;
	draw_widget(widget->ctx, widget, widget->ctx->shm_buffer);
	printf("BGTK label set\n");
}

struct BGTK_Widget* bgtk_label(struct BGTK_Context* ctx, char* text, BGTK_Options options) {
	struct BGTK_Widget* widget = widget_new(ctx, BGTK_WIDGET_LABEL, options);
	printf("BGTK allocated label\n");
	if (!widget) {
		perror("BGTK Failed to create new widget");
		return NULL;
	}

	widget->data.label.set_label = set_label;

	// Create a text widget for the label
	struct BGTK_Widget* text_widget = bgtk_text(ctx, text, (BGTK_Options){ .flags = 0 });
	if (!text_widget) {
		perror(
		    "BGTK Failed to create text widget for "
		    "label");
		free(widget);
		return NULL;
	}

	widget->data.label.text = text_widget;

	// Calculate size based on text widget and padding
	widget->w = text_widget->w + 2 * widget->padding;
	widget->h = text_widget->h + 2 * widget->padding;

	return widget;
}

struct BGTK_Widget* bgtk_text(struct BGTK_Context* ctx, char* text, BGTK_Options options) {
	printf("BGTK creating text widget\n");
	struct BGTK_Widget* widget = widget_new(ctx, BGTK_WIDGET_TEXT, options);
	printf("BGTK allocated text widget\n");
	if (!widget) {
		perror("BGTK Failed to create new widget");
		return NULL;
	}

	char* ptr = calloc(1, strlen(text) + 1);
	sprintf(ptr, "%s", text);
	widget->data.text.text = ptr;

	// Calculate size based on text
	measure_text(widget->ctx->ft_face, widget->data.text.text, &widget->w,
		     &widget->h);

	// Add padding to the text widget
	widget->w += 2 * widget->padding;
	widget->h += 2 * widget->padding;

	return widget;
}

struct BGTK_Widget* bgtk_button(struct BGTK_Context* ctx,
				struct BGTK_Widget* label,
				BGTK_Callback callback, BGTK_Options options) {
	printf("BGTK creating button widget\n");
	struct BGTK_Widget* widget = widget_new(ctx, BGTK_WIDGET_BUTTON, options);
	if (!widget) {
		perror("BGTK Failed to create new widget");
		return NULL;
	}

	widget->data.button.callback = callback;
	widget->data.button.label = label;
	
	// Override the default event handler with button-specific one
	widget->handle_event = button_handle_event;
	
	// Calculate size based on label widget and padding
	widget->w = label->w + 2 * widget->padding;
	widget->h = label->h + 2 * widget->padding;

	return widget;
}

struct BGTK_Widget* bgtk_scrollable(struct BGTK_Context* ctx,
					  struct BGTK_Widget** items,
				  int widget_count, BGTK_Options options) {
	printf("BGTK creating scrollable widget\n");
	struct BGTK_Widget* widget =
	    widget_new(ctx, BGTK_WIDGET_SCROLLABLE, options);
	if (!widget) {
		perror("BGTK Failed to create scrollable widget");
		return NULL;
	}

	widget->data.scrollable.items = (struct BGTK_Widget**)calloc(
	    widget_count, sizeof(struct BGTK_Widget*));
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
		widget->data.scrollable.content_height += 
		    items[i]->h + 5 + 2 * widget->margin;  // 5px spacing + margin
	}

	// Initialize tmp buffer to NULL, it will be allocated
	// during drawing
	widget->data.scrollable.tmp = NULL;
	
	// Override the default event handler with scrollable-specific one
	widget->handle_event = scrollable_handle_event;
	
	printf("BGTK allocated scrollable widget\n");

	return widget;
}

struct BGTK_Widget* bgtk_image(struct BGTK_Context* ctx, const char* path,
		      BGTK_Options options) {
	printf("BGTK creating image widget\n");
	struct BGTK_Widget* widget = widget_new(ctx, BGTK_WIDGET_IMAGE, options);
	if (!widget) {
		perror("BGTK Failed to create image widget");
		return NULL;
	}

	// Load the image into a pixel buffer
	uint32_t* pixels = NULL;
	int img_w, img_h;
	if (load_image(path, &pixels, &img_w, &img_h) != 0) {
		free(widget);
		return NULL;
	}

	widget->data.image.pixels = pixels;
	widget->data.image.img_w = img_w;
	widget->data.image.img_h = img_h;

	// Add padding to the image widget
	widget->w = img_w + 2 * widget->padding;
	widget->h = img_h + 2 * widget->padding;

	return widget;
}

struct BGTK_Widget* bgtk_frame(struct BGTK_Context* ctx, struct BGTK_Widget* child, int width, int height, BGTK_Options options) {
	struct BGTK_Widget* frame = (struct BGTK_Widget*)malloc(sizeof(struct BGTK_Widget));
	if (!frame) return NULL;

	frame->ctx = ctx;
	frame->type = BGTK_WIDGET_FRAME;
	frame->x = 0; // Default position, can be adjusted
	frame->y = 0;
	frame->w = width;
	frame->h = height;
	frame->flags = options.flags;
	frame->padding = options.padding;
	frame->margin = options.margin;

	frame->data.frame.child = child;
	frame->data.frame.border_w = ctx->theme.frame_border_size;

	// Set the event handler for the frame
	frame->handle_event = frame_handle_event;

	return frame;
}

// Creates a text input widget.
struct BGTK_Widget* bgtk_text_input(
	struct BGTK_Context* ctx, char* initial_text, int width, int height, BGTK_Options options) {
	printf("BGTK creating text input widget\n");
	struct BGTK_Widget* widget = widget_new(ctx, BGTK_WIDGET_TEXT_INPUT, options);
	if (!widget) {
	    perror("BGTK Failed to create text input widget");
	    return NULL;
	}

	// Initialize text input data
	widget->data.text_input.text = strdup(initial_text ? initial_text : "");
	widget->data.text_input.cursor_pos = strlen(widget->data.text_input.text);
	widget->data.text_input.selection_start = -1;
	widget->data.text_input.selection_end = -1;
	widget->data.text_input.focused = false;
	widget->data.text_input.on_change = NULL;

	// Override the default event handler with text input-specific one
	widget->handle_event = text_input_handle_event;

	// Set widget size (width/height are outer dimensions including padding/border)
	widget->w = width;
	widget->h = height;

	return widget;
}
