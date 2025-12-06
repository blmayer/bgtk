#include "bgtk.h"
#include <stdio.h>
#include <string.h>

static int counter = 0;
static BGTK_Widget* counter_label = NULL;
BGTK_Context* ctx = NULL;

void button_callback(void) {
	counter++;
	printf("Button clicked! Counter: %d\n", counter);

	if (counter_label && counter_label->type == BGTK_WIDGET_LABEL) {
		puts(" setting new label");
		char counter_text[32];
		sprintf(counter_text, "Counter: %d", counter);
		printf("calling set_label: %p\n", counter_label->set_label);
		counter_label->set_label(counter_label, counter_text);
		// Note: Widget size should ideally be recalculated here for layout purposes,
		// but for absolute layout, we omit it for now.
	}
}

int main(void) {
	setvbuf(stdout, NULL, _IONBF, 0); // Disable buffering for stdout
	setvbuf(stderr, NULL, _IONBF, 0); // Disable buffering for stderr

	ctx = bgtk_init();
	if (!ctx) {
		fprintf(stderr, "Failed to initialize BGTK.\n");
		return 1;
	}
	printf("BGTK init done\n");

	// 1. Create Widgets
	char* counter_text = "Counter: 0";

	BGTK_Widget* title = bgtk_label_new(ctx, "BGTK Demo Application");
	BGTK_Widget* button = bgtk_button_new(ctx, "Click Me!", button_callback);
	counter_label = bgtk_label_new(ctx, counter_text);
	printf("BGTK added widgets\n");

	// 2. Add Widgets (Absolute Positioning)
	// Title at the top center
	bgtk_add_widget(ctx, title, (ctx->width / 2) - (title->w / 2), 50, 0, 0);

	// Button below the title
	bgtk_add_widget(ctx, button, (ctx->width / 2) - (button->w / 2), 100, 0, 0);

	// Label below the button
	bgtk_add_widget(ctx, counter_label, (ctx->width / 2) - (counter_label->w / 2), 150, 0, 0);

	printf("Starting BGTK main loop (%dx%d)...\n", ctx->width, ctx->height);
	bgtk_main_loop(ctx);

	bgtk_destroy(ctx);
	return 0;
}
