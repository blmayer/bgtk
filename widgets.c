#include <bgce.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bgtk.h"
#include "internal.h"

// Helper to create a generic widget
static struct BGTK_Widget* widget_new(struct BGTK_Context* ctx,
				      enum BGTK_Widget_Type type, int flags) {
	struct BGTK_Widget* widget =
	    (struct BGTK_Widget*)calloc(1, sizeof(struct BGTK_Widget));
	if (!widget) {
		perror("calloc");
		return NULL;
	}
	widget->ctx = ctx;
	widget->type = type;
	widget->flags = flags;
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

	// Calculate size based on text widget
	widget->w = text_widget->w + 10;  // Add padding
	widget->h = text_widget->h + 10;  // Add padding

	draw_widget(widget->ctx, widget, widget->ctx->shm_buffer);
	printf("BGTK label set\n");
}

struct BGTK_Widget* bgtk_label(struct BGTK_Context* ctx, char* text) {
	struct BGTK_Widget* widget = widget_new(ctx, BGTK_WIDGET_LABEL, 0);
	printf("BGTK allocated label\n");
	if (!widget) {
		perror("BGTK Failed to create new widget");
		return NULL;
	}

	widget->set_label = set_label;

	// Create a text widget for the label
	struct BGTK_Widget* text_widget = bgtk_text(ctx, text, 0);
	if (!text_widget) {
		perror(
		    "BGTK Failed to create text widget for "
		    "label");
		free(widget);
		return NULL;
	}

	widget->data.label.text = text_widget;

	// Calculate size based on text widget
	widget->w = text_widget->w + 10;  // Add padding
	widget->h = text_widget->h + 10;  // Add padding

	return widget;
}

struct BGTK_Widget* bgtk_text(struct BGTK_Context* ctx, char* text, int flags) {
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

	return widget;
}

struct BGTK_Widget* bgtk_button(struct BGTK_Context* ctx,
				struct BGTK_Widget* label,
				BGTK_Callback callback, int flags) {
	printf("BGTK creating button widget\n");
	struct BGTK_Widget* widget = widget_new(ctx, BGTK_WIDGET_BUTTON, flags);
	if (!widget) {
		perror("BGTK Failed to create new widget");
		return NULL;
	}

	widget->data.button.callback = callback;
	widget->data.button.label = label;

	// Calculate size based on label widget
	widget->w = label->w + 20;  // Add padding
	widget->h = label->h + 20;  // Add padding
	return widget;
}

struct BGTK_Widget* bgtk_scrollable(struct BGTK_Context* ctx,
				    struct BGTK_Widget** widgets,
				    int widget_count, int flags) {
	printf("BGTK creating scrollable widget\n");
	struct BGTK_Widget* widget =
	    widget_new(ctx, BGTK_WIDGET_SCROLLABLE, flags);
	if (!widget) {
		perror("BGTK Failed to create scrollable widget");
		return NULL;
	}

	widget->data.scrollable.widgets = (struct BGTK_Widget**)calloc(
	    widget_count, sizeof(struct BGTK_Widget*));
	if (!widget->data.scrollable.widgets) {
		perror("calloc");
		free(widget);
		return NULL;
	}

	// Copy the input widgets into the scrollable container
	widget->data.scrollable.widget_count = widget_count;
	widget->data.scrollable.scroll_y = 0;
	widget->data.scrollable.content_height = 0;
	for (int i = 0; i < widget_count; i++) {
		widget->data.scrollable.widgets[i] = widgets[i];
		widget->data.scrollable.content_height +=
		    widgets[i]->h + 5;	// 5px spacing
	}

	// Initialize tmp buffer to NULL, it will be allocated
	// during drawing
	widget->data.scrollable.tmp = NULL;
	printf("BGTK allocated scrollable widget\n");

	return widget;
}

struct BGTK_Widget* bgtk_image(struct BGTK_Context* ctx, const char* path,
			       int flags) {
	printf("BGTK creating image widget\n");
	struct BGTK_Widget* widget = widget_new(ctx, BGTK_WIDGET_IMAGE, flags);
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

	return widget;
}
