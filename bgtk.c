#define STB_IMAGE_WRITE_IMPLEMENTATION

#include "bgtk.h"

#include <bgce.h>
#include <linux/input.h>
#include <stb_image_write.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/time.h>
#include <unistd.h>

#include "config.h"
#include "internal.h"

// The font path is hardcoded for simplicity.
#define DEFAULT_FONT_PATH                                 \
	"/usr/share/fonts/ttf-input/InputMono/InputMono/" \
	"InputMono-Regular.ttf"
#define DEFAULT_FONT_SIZE 12

int take_screenshot(struct BGTK_Context ctx)
{
	if (!ctx.shm_buffer) {
		fprintf(stderr, "No framebuffer available for screenshot.\n");
		return -1;
	}
	// Write the framebuffer to a PNG file
	struct timeval tv;
	gettimeofday(&tv, NULL);
	char filename[256];
	snprintf(filename, sizeof(filename), "screenshot_%ld_%06ld.png",
		 tv.tv_sec, tv.tv_usec);
	int result = stbi_write_png(filename, ctx.width, ctx.height,
				    BGCE_BYTES_PER_PIXEL, ctx.shm_buffer,
				    ctx.width * BGCE_BYTES_PER_PIXEL);

	if (!result) {
		fprintf(stderr, "Failed to save screenshot to %s.\n", filename);
		return -1;
	}

	printf("Screenshot saved to %s.\n", filename);
	return 0;
}

struct BGTK_Context *bgtk_init(int conn_fd, void *buffer, int width, int height)
{
	struct BGTK_Context *ctx =
	    (struct BGTK_Context *)calloc(1, sizeof(struct BGTK_Context));
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
	ctx->window_focused = 1;
	ctx->shift_held = 0;

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
		ctx->theme.background = 0xAAAAAAAA;	// Default gray
		ctx->theme.button = 0x88888888;	// Default button color
		ctx->theme.button_text = 0xFFFFFFFF;	// Default white text
		ctx->theme.frame_border_size = 4;	// Default frame border size
		ctx->theme.frame_border_color = 0xFFFFFFFF;
		ctx->theme.button_border_size = 1;
		ctx->theme.input_border_size = 1;

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
			"to simple " "drawing.\n", ctx->font_path);
		free(ctx);
		return NULL;
	}
	// Set font size
	FT_Set_Pixel_Sizes(ctx->ft_face, 0, ctx->font_size);

	return ctx;
}

static void destroy_widget(struct BGTK_Widget *w);

void bgtk_destroy(struct BGTK_Context *ctx)
{
	if (!ctx) {
		return;
	}
	if (ctx->root_widget) {
		destroy_widget(ctx->root_widget);
		ctx->root_widget = NULL;
		ctx->focused_widget = NULL;
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

static void destroy_widget(struct BGTK_Widget *w)
{
	if (!w) {
		return;
	}

	switch (w->type) {
	case BGTK_WIDGET_SCROLLABLE:
		if (w->data.scrollable.items) {
			for (int i = 0; i < w->data.scrollable.widget_count; i++) {
				destroy_widget(w->data.scrollable.items[i]);
			}
			free(w->data.scrollable.items);
		}
		if (w->data.scrollable.tmp) {
			free(w->data.scrollable.tmp);
		}
		break;
	case BGTK_WIDGET_LIST:
		if (w->data.list_widget.items) {
			for (int i = 0; i < w->data.list_widget.widget_count; i++) {
				destroy_widget(w->data.list_widget.items[i]);
			}
			free(w->data.list_widget.items);
		}
		break;
	case BGTK_WIDGET_FRAME:
		if (w->data.frame.child) {
			destroy_widget(w->data.frame.child);
		}
		break;
	case BGTK_WIDGET_BUTTON:
		if (w->data.button.label) {
			destroy_widget(w->data.button.label);
		}
		break;
	case BGTK_WIDGET_LABEL:
		if (w->data.label.text) {
			destroy_widget(w->data.label.text);
		}
		break;
	case BGTK_WIDGET_TEXT:
		free(w->data.text.text);
		break;
	case BGTK_WIDGET_TEXT_INPUT:
		free(w->data.text_input.text);
		break;
	case BGTK_WIDGET_IMAGE:
		if (w->data.image.pixels) {
			free(w->data.image.pixels);
		}
		break;
	default:
		break;
	}

	free(w);
}

void bgtk_set_window_focus(struct BGTK_Context *ctx, int focused)
{
	if (!ctx) {
		return;
	}
	ctx->window_focused = focused;
	bgtk_draw_widgets(ctx);
}

// --- Drawing Primitives & Widgets ---

void bgtk_draw_widgets(struct BGTK_Context *ctx)
{
	clear_buffer(ctx);
	calculate_widget_size(ctx, ctx->root_widget);
	draw_widget(ctx, ctx->root_widget, ctx->shm_buffer);
	bgce_draw(ctx->conn_fd);
}

// Handles a single event and returns whether a redraw is needed.
int bgtk_handle_input_event(struct BGTK_Context *ctx, struct InputEvent ev)
{
	// Handle some keys
	if (ev.code == KEY_SYSRQ) {
		take_screenshot(*ctx);
		return 1;
	}
	// Track shift for text input uppercase support. Do not consume the event.
	if (ev.type == EV_KEY) {
		if (ev.code == KEY_LEFTSHIFT || ev.code == KEY_RIGHTSHIFT) {
			ctx->shift_held = (ev.value != 0);
		}
	}
	// Start event handling from the root widget
	if (!ctx->root_widget) {
		return 0;
	}
	// Make a copy of the event to avoid modifying the original
	struct InputEvent widget_ev = ev;

	// Pass the event to the root widget
	return ctx->root_widget->handle_event(ctx->root_widget, widget_ev);
}
