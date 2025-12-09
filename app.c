#include <stdio.h>

#include "bgtk.h"

static int counter = 0;
static struct BGTK_Widget* counter_label = NULL;
struct BGTK_Context* ctx = NULL;

void button_callback(void) {
	counter++;
	printf("Button clicked! Counter: %d\n", counter);

	if (counter_label && counter_label->type == BGTK_WIDGET_LABEL) {
		puts(" setting new label");
		char counter_text[32];
		sprintf(counter_text, "Counter: %d", counter);
		printf("calling set_label: %p\n", counter_label->set_label);
		counter_label->set_label(counter_label, counter_text);
	}
}

int main(void) {
	setvbuf(stdout, NULL, _IONBF, 0);  // Disable buffering for stdout
	setvbuf(stderr, NULL, _IONBF, 0);  // Disable buffering for stderr

	ctx = bgtk_init();
	if (!ctx) {
		fprintf(stderr, "Failed to initialize BGTK.\n");
		return 1;
	}
	printf("BGTK init done\n");

	// 1. Create Widgets
	char* counter_text = "Counter: 0";

	struct BGTK_Widget* title = bgtk_label(ctx, "BGTK Demo Application");
	struct BGTK_Widget* button_text = bgtk_text(ctx, "Click Me!", 0);
	struct BGTK_Widget* button =
	    bgtk_button(ctx, button_text, button_callback, 0);
	counter_label = bgtk_label(ctx, counter_text);

	// Create a list of widgets for the scrollable container
	struct BGTK_Widget* scrollable_widgets[10];
	for (int i = 0; i < 10; i++) {
		char label_text[32];
		sprintf(label_text, "Item %d", i + 1);
		scrollable_widgets[i] = bgtk_label(ctx, label_text);
	}

	// Create the scrollable widget with the list of widgets
	struct BGTK_Widget* scrollable =
	    bgtk_scrollable(ctx, scrollable_widgets, 10, BGTK_FLAG_CENTER);

	// 2. Add Widgets (Absolute Positioning)
	// Title at the top center
	bgtk_add_widget(ctx, title, (ctx->width / 2) - (title->w / 2), 20, 0,
			0);

	// Button below the title
	bgtk_add_widget(ctx, button, (ctx->width / 2) - (button->w / 2), 70, 0,
			0);

	// Label below the button
	bgtk_add_widget(ctx, counter_label,
			(ctx->width / 2) - (counter_label->w / 2), 120, 0, 0);

	// Scrollable widget below the counter labelnd how  
	bgtk_add_widget(ctx, scrollable, 0, 170, 600, 200);

	printf("Starting BGTK main loop (%dx%d)...\n", ctx->width, ctx->height);
	bgtk_main_loop(ctx);

	bgtk_destroy(ctx);
	return 0;
}
