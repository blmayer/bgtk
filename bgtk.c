#define STB_IMAGE_WRITE_IMPLEMENTATION

#include "bgtk.h"
#include "config.h"

#include <bgce.h>
#include <linux/input.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/time.h>
#include <stb_image_write.h>

#include "internal.h"

// The font path is hardcoded for simplicity.
#define DEFAULT_FONT_PATH                                 \
	"/usr/share/fonts/ttf-input/InputMono/InputMono/" \
	"InputMono-Regular.ttf"
#define DEFAULT_FONT_SIZE 12

int take_screenshot(struct BGTK_Context ctx) {
	if (!ctx.shm_buffer) {
		fprintf(stderr, "No framebuffer available for screenshot.\n");
		return -1;
	}

	// Write the framebuffer to a PNG file
	struct timeval tv;
	gettimeofday(&tv, NULL);
	char filename[256];
	snprintf(filename, sizeof(filename), "screenshot_%ld_%06ld.png", tv.tv_sec, tv.tv_usec);
	int result = stbi_write_png(
		filename,
		ctx.width,
		ctx.height,
		BGCE_BYTES_PER_PIXEL,
		ctx.shm_buffer,
		ctx.width * BGCE_BYTES_PER_PIXEL
	);

	if (!result) {
		fprintf(stderr, "Failed to save screenshot to %s.\n", filename);
		return -1;
	}

	printf("Screenshot saved to %s.\n", filename);
	return 0;
}

struct BGTK_Context* bgtk_init(int conn_fd, void* buffer, int width,
			       int height) {
	struct BGTK_Context* ctx =
	    (struct BGTK_Context*)calloc(1, sizeof(struct BGTK_Context));
	if (!ctx) {
		perror("calloc");
		return NULL;
	}

	ctx->conn_fd = conn_fd;
	ctx->shm_buffer = buffer;
	ctx->font_size = DEFAULT_FONT_SIZE;
	ctx->width = width;
	ctx->height = height;
	ctx->root_widget = NULL;

	// Load config file
	struct config config;
	if (parse_config(&config) == 0) {
		// Store theme data
		ctx->theme = config.theme;
		strncpy(ctx->font_path, config.font_path, MAX_PATH_LEN - 1);
		ctx->font_path[MAX_PATH_LEN - 1] = '\0';
		ctx->font_size = config.font_size;
	} else {
		// Use defaults if config file is missing or invalid
		ctx->theme.background = 0xAAAAAAAA; // Default gray
		ctx->theme.button = 0x88888888;     // Default button color
		ctx->theme.button_text = 0xFFFFFFFF; // Default white text
		strncpy(ctx->font_path, DEFAULT_FONT_PATH, MAX_PATH_LEN - 1);
		ctx->font_path[MAX_PATH_LEN - 1] = '\0';
	}

	// 1. Initialize FreeType
	if (FT_Init_FreeType(&ctx->ft_library)) {
		fprintf(stderr,
			"bgtk_init: Could not init FreeType library.\n");
		free(ctx);
		return NULL;
	}

	// 2. Load Font
	if (FT_New_Face(ctx->ft_library, ctx->font_path, 0, &ctx->ft_face)) {
		fprintf(stderr,
			"bgtk_init: Could not load font %s. Falling back "
			"to simple "
			"drawing.\n",
			ctx->font_path);
		free(ctx);
		return NULL;
	}

	// Set font size
	FT_Set_Pixel_Sizes(ctx->ft_face, 0, ctx->font_size);

	return ctx;
}

void bgtk_destroy(struct BGTK_Context* ctx) {
	if (!ctx) {
		return;
	}

	// Free the root widget and its children recursively
	if (ctx->root_widget) {
		if (ctx->root_widget->type == BGTK_WIDGET_SCROLLABLE) {
			if (ctx->root_widget->data.scrollable.items) {
				for (int i = 0;
				     i < ctx->root_widget->data.scrollable
					     .widget_count;
				     i++) {
					free(ctx->root_widget->data.scrollable
						 .items[i]);
				}
				free(ctx->root_widget->data.scrollable.items);
			}
			if (ctx->root_widget->data.scrollable.tmp) {
				free(ctx->root_widget->data.scrollable.tmp);
			}
		} else if (ctx->root_widget->type == BGTK_WIDGET_LABEL) {
			if (ctx->root_widget->data.label.text) {
				free(ctx->root_widget->data.label.text->data
					 .text.text);
				free(ctx->root_widget->data.label.text);
			}
		} else if (ctx->root_widget->type == BGTK_WIDGET_BUTTON) {
			if (ctx->root_widget->data.button.label) {
				if (ctx->root_widget->data.button.label->data
					.label.text) {
					free(ctx->root_widget->data.button
						 .label->data.label.text->data
						 .text.text);
					free(ctx->root_widget->data.button
						 .label->data.label.text);
				}
				free(ctx->root_widget->data.button.label);
			}
		} else if (ctx->root_widget->type == BGTK_WIDGET_TEXT) {
			free(ctx->root_widget->data.text.text);
		}
		free(ctx->root_widget);
	}

	// Free FreeType resources
	if (ctx->ft_face) {
		FT_Done_Face(ctx->ft_face);
	}
	if (ctx->ft_library) {
		FT_Done_FreeType(ctx->ft_library);
	}

	free(ctx);
}

// --- Drawing Primitives & Widgets ---

void bgtk_draw_widgets(struct BGTK_Context* ctx) {
	puts("got draw widgets request");
	clear_buffer(ctx);
	calculate_widget_size(ctx, ctx->root_widget);
	draw_widget(ctx, ctx->root_widget, ctx->shm_buffer);
	bgce_draw(ctx->conn_fd);
}

// TODO: implement descending into child widgets
int bgtk_handle_input_event(struct BGTK_Context* ctx, struct InputEvent ev) {
	// Hanlde some keys
	if (ev.code == KEY_SYSRQ) {
		printf("[BGTK] Print Screen key pressed, taking screenshot.\n");
		take_screenshot(*ctx);
		return 1;
	}

	// Handle mouse wheel for scrolling (REL_WHEEL)
	if (ev.code == REL_WHEEL) {
		printf("handling mouse wheel: val=%d at (%u, %u)\n", ev.value,
		       ev.x, ev.y);
		struct BGTK_Widget* w = ctx->root_widget;

		// TODO: descend to the last scrollable widget
		// while (1) {
		if (w->type == BGTK_WIDGET_SCROLLABLE) {
			puts("found scroll widget");

			// Check if mouse is over the scrollable
			// widget
			if (ev.x >= w->x && ev.x < (w->x + w->w) &&
			    ev.y >= w->y && ev.y < (w->y + w->h)) {
				puts("updating scroll");
				w->data.scrollable.scroll_y -=
				    ev.value * 10;  // Scroll speed

				// Clamp scroll_y to valid range
				if (w->data.scrollable.scroll_y < 0) {
					w->data.scrollable.scroll_y = 0;
				}
				if (w->data.scrollable.scroll_y >
				    w->data.scrollable.content_height - w->h) {
					w->data.scrollable.scroll_y =
					    w->data.scrollable.content_height -
					    w->h;
				}
				if (w->data.scrollable.scroll_y < 0) {
					w->data.scrollable.scroll_y = 0;
				}
				printf(
				    "updated scroll position: "
				    "%d\n",
				    w->data.scrollable.scroll_y);
				draw_widget(ctx, w, ctx->shm_buffer);

				return 1;  // Redraw
			}
		}
		//}
	}

	// Only handle mouse button presses for now
	if (ev.code != BTN_LEFT || ev.value != 1) {
		return 0;
	}
	printf("BGTK Got click: (%d, %d)\n", ev.x, ev.y);

	// Hit-testing for buttons
	struct BGTK_Widget* w = ctx->root_widget;
	while (w) {
		switch (w->type) {
			case BGTK_WIDGET_BUTTON:
				// Check if click coordinates
				// are within button bounds
				if (ev.x >= w->x && ev.x < (w->x + w->w) &&
				    ev.y >= w->y && ev.y < (w->y + w->h)) {
					printf("BGTK Clicked in button\n");

					// Trigger callback
					if (w->data.button.callback) {
						w->data.button.callback();
						return 1;
					}
				}
				return 0;
			case BGTK_WIDGET_SCROLLABLE: {
				printf("clicked in a scrollable widget\n");
				int found = 0;
				for (int i = 0;
				     i < w->data.scrollable.widget_count; i++) {
					struct BGTK_Widget* item =
					    w->data.scrollable.items[i];
					if (ev.x >= item->x &&
					    ev.x < (item->x + item->w) &&
					    ev.y >= item->y &&
					    ev.y < (item->y + item->h)) {
						printf(
						    "clicked in the %d item\n",
						    i);
						w = item;
						found = 1;
						break;
					}
				}
				if (!found) {
					return 0;
				}
				break;
			}
			default:
				printf("clicked on a widget without action\n");
				return 0;
		}
	}
	return 0;
}
