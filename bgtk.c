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

int take_screenshot(struct BGTK_Context *ctx, const char *path)
{
	if (!ctx || !ctx->shm_buffer) {
		return -1;
	}
	char filename[256];
	const char *out_path = path;
	if (!out_path) {
		// Timestamped name for SYSRQ / convenience
		struct timeval tv;
		gettimeofday(&tv, NULL);
		snprintf(filename, sizeof(filename), "screenshot_%ld_%06d.png",
			 (long)tv.tv_sec, (int)tv.tv_usec);
		out_path = filename;
	}

	// Convert internal 0xAARRGGBB buffer (alpha may vary) to proper
	// opaque RGBA byte layout for the PNG writer. This ensures colors
	// (including fuchsia headers/links etc.) appear correctly in mock
	// screenshots instead of being lost to alpha=0 or channel order.
	unsigned char *rgba = (unsigned char *)malloc((size_t)ctx->width * ctx->height * 4);
	if (!rgba) {
		fprintf(stderr, "take_screenshot: out of memory for RGBA conversion\n");
		return -1;
	}
	uint32_t *src = (uint32_t *)ctx->shm_buffer;
	for (int i = 0; i < ctx->width * ctx->height; i++) {
		uint32_t p = src[i];
		rgba[i*4 + 0] = (p >> 16) & 0xFF; // R
		rgba[i*4 + 1] = (p >>  8) & 0xFF; // G
		rgba[i*4 + 2] = p & 0xFF;         // B
		rgba[i*4 + 3] = 0xFF;             // force opaque
	}

	int result = stbi_write_png(out_path, ctx->width, ctx->height,
				    4, rgba, ctx->width * 4);
	free(rgba);

	if (!result) {
		fprintf(stderr, "Failed to save frame to %s.\n", out_path);
		return -1;
	}
	printf("Frame written to %s.\n", out_path);
	return 0;
}

static void bgtk_init_resources(struct BGTK_Context *ctx);

// Public mock init: full implementation for headless/testing. Owns its framebuffer.
struct BGTK_Context *bgtk_init_mock(int width, int height)
{
	struct BGTK_Context *ctx =
	    (struct BGTK_Context *)calloc(1, sizeof(struct BGTK_Context));
	if (!ctx) {
		perror("calloc");
		return NULL;
	}

	ctx->conn_fd = -1;
	ctx->width = width;
	ctx->height = height;
	ctx->shm_buffer = calloc((size_t)width * height * BGCE_BYTES_PER_PIXEL, 1);
	if (!ctx->shm_buffer) {
		perror("calloc framebuffer");
		free(ctx);
		return NULL;
	}

	bgtk_init_resources(ctx);
	if (!ctx->ft_library) {
		free(ctx->shm_buffer);
		free(ctx);
		return NULL;
	}
	return ctx;
}

// Public: feed a synthetic event for testing (clicks, keys, etc).
// Redraws automatically if the handler requests it.
int bgtk_inject_event(struct BGTK_Context *ctx, struct InputEvent ev)
{
	if (!ctx) {
		return 0;
	}
	int res = bgtk_handle_input_event(ctx, ev);
	if (res) {
		bgtk_draw_widgets(ctx);
	}
	return res;
}

// Common resource setup (config, freetype, font) after buffer/conn/ dims are set.
// Used by both real bgtk_init and bgtk_init_mock.
static void bgtk_init_resources(struct BGTK_Context *ctx)
{
	ctx->root_widget = NULL;
	ctx->window_focused = 1;
	ctx->shift_held = 0;

	// Load config (parse_config now always starts from defaults via init_config_defaults).
	struct config config = {0};
	parse_config(&config);
	ctx->theme = config.theme;
	strncpy(ctx->font_path, config.font_path, MAX_PATH_LEN - 1);
	ctx->font_path[MAX_PATH_LEN - 1] = '\0';
	ctx->font_size = config.font_size;

	// 1. Initialize FreeType
	if (FT_Init_FreeType(&ctx->ft_library)) {
		fprintf(stderr,
			"bgtk_init: Could not init FreeType library.\n");
	}
	if (!ctx->ft_library) {
		return;
	}

	// 2. The actual FT loading (New_Face, Select, sizing, validation) lives here
	//    in init_resources. The default *selection* (with #ifdefs) was done in
	//    init_config_defaults.
	if (ctx->font_path[0] != '\0') {
		if (FT_New_Face(ctx->ft_library, ctx->font_path, 0, &ctx->ft_face) != 0) {
			fprintf(stderr, "bgtk_init: Could not load font from config: %s\n"
			                "             Using placeholder drawing.\n",
			        ctx->font_path);
			ctx->font_path[0] = '\0';
		} else {
			fprintf(stderr, "bgtk_init: Using font: %s\n", ctx->font_path);
		}
	}

	if (ctx->ft_face) {
		// Force a Unicode charmap if the font has one. This prevents
		// "symbol" or other cmap encodings from mapping ASCII to the
		// wrong glyphs (which often look like triangles/boxes for .notdef).
		FT_Select_Charmap(ctx->ft_face, FT_ENCODING_UNICODE);

		FT_Set_Pixel_Sizes(ctx->ft_face, 0, ctx->font_size);

		// Validate that the loaded face actually produces glyph metrics.
		// Catches .ttc wrong face, symbol fonts, or broken files that
		// "load" without error but produce no visible text.
		int tw = 0, th = 0;
		measure_text(ctx->ft_face, "Ag", &tw, &th);
		if (tw <= 0 || th <= 0) {
			fprintf(stderr, "bgtk_init: Font '%s' loaded but has no usable glyphs. "
			                "Using placeholder drawing.\n", ctx->font_path);
			FT_Done_Face(ctx->ft_face);
			ctx->ft_face = NULL;
			ctx->font_path[0] = '\0';
		}
	}
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
	if (!ctx->shm_buffer) {
		// Real server path requires caller-provided buffer.
		free(ctx);
		return NULL;
	}
	ctx->width = width;
	ctx->height = height;

	bgtk_init_resources(ctx);
	if (!ctx->ft_library) {
		// freetype failed; buffer is caller-owned for real init.
		free(ctx);
		return NULL;
	}
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
	// Free FreeType resources (buffer ownership is with caller for real init,
	// or handled in bgtk_destroy_mock for headless).
	if (ctx->ft_face) {
		FT_Done_Face(ctx->ft_face);
	}
	if (ctx->ft_library) {
		FT_Done_FreeType(ctx->ft_library);
	}

	free(ctx);
}

void bgtk_destroy_mock(struct BGTK_Context *ctx)
{
	if (ctx) {
		if (ctx->shm_buffer) {
			free(ctx->shm_buffer);
			ctx->shm_buffer = NULL;
		}
	}
	bgtk_destroy(ctx);
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
	if (ctx && ctx->conn_fd >= 0) {
		bgce_draw(ctx->conn_fd);
	}
}

// Handles a single event and returns whether a redraw is needed.
int bgtk_handle_input_event(struct BGTK_Context *ctx, struct InputEvent ev)
{
	// Handle some keys
	if (ev.code == KEY_SYSRQ) {
		take_screenshot(ctx, NULL);
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
