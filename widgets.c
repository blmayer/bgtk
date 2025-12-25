#include <bgce.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bgtk.h"
#include "internal.h"

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
	return widget;
}

void set_label(struct BGTK_Widget* widget, char* label) {
	printf("BGTK: setting label: %s\n", label);
	if (widget->data.label.text) {
		free(widget->data.label.text->data.text.text);
		free(widget->data.label.text);
	}

	// Create a new text widget for the label
	struct BGTK_Widget* text_widget = bgtk_text(widget->ctx, label, 0);
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

	widget->set_label = set_label;

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
	struct BGTK_Widget* widget = widget_new(ctx, BGTK_WIDGET_TEXT, flags);
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

	widget->data.scrollable.widgets = (struct BGTK_Widget**)calloc(
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
