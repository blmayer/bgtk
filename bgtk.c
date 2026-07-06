#define STB_IMAGE_WRITE_IMPLEMENTATION

#include "bgtk.h"

#include <bgce.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <stdarg.h>
#include <stb_image_write.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "config.h"
#include "internal.h"

/* Dedicated BGTK logs live under ~/.cache/bgtk/ (or $XDG_CACHE_HOME/bgtk/),
 * separate from BGCE server/client stderr so app and compositor output do not
 * fight over one stream or file. */
static FILE *bgtk_log_fp;
static char bgtk_log_name[64] = "bgtk";

static void bgtk_log_timestamp(char *ts, size_t tslen)
{
	struct timeval tv;
	struct tm tm;
	gettimeofday(&tv, NULL);
	localtime_r(&tv.tv_sec, &tm);
	/* YYYY-mm-dd HH:MM:SS.mmm — same stamp on file and stderr. */
	int n = strftime(ts, tslen, "%Y-%m-%d %H:%M:%S", &tm);
	if (n > 0 && (size_t)n + 5 < tslen)
		snprintf(ts + n, tslen - (size_t)n, ".%03d",
			 (int)(tv.tv_usec / 1000));
}

static int bgtk_mkdir_p(const char *path)
{
	struct stat st;
	if (!path || !path[0])
		return -1;
	if (stat(path, &st) == 0)
		return S_ISDIR(st.st_mode) ? 0 : -1;
	if (mkdir(path, 0700) < 0 && errno != EEXIST)
		return -1;
	return 0;
}

static int bgtk_ensure_log_dir(char *dir, size_t dirlen)
{
	const char *xdg = getenv("XDG_CACHE_HOME");
	const char *home = getenv("HOME");
	int n;

	if (xdg && xdg[0] == '/') {
		if (bgtk_mkdir_p(xdg) < 0)
			return -1;
		n = snprintf(dir, dirlen, "%s/bgtk", xdg);
	} else if (home && home[0]) {
		char cache[512];
		n = snprintf(cache, sizeof(cache), "%s/.cache", home);
		if (n < 0 || (size_t)n >= sizeof(cache))
			return -1;
		if (bgtk_mkdir_p(cache) < 0)
			return -1;
		n = snprintf(dir, dirlen, "%s/.cache/bgtk", home);
	} else {
		return -1;
	}
	if (n < 0 || (size_t)n >= dirlen)
		return -1;
	return bgtk_mkdir_p(dir);
}

void bgtk_log_open(const char *app_name)
{
	char dir[512];
	char path[640];

	if (app_name && app_name[0]) {
		strncpy(bgtk_log_name, app_name, sizeof(bgtk_log_name) - 1);
		bgtk_log_name[sizeof(bgtk_log_name) - 1] = '\0';
	}

	if (bgtk_log_fp && bgtk_log_fp != stderr) {
		fclose(bgtk_log_fp);
		bgtk_log_fp = NULL;
	}

	if (bgtk_ensure_log_dir(dir, sizeof(dir)) < 0) {
		char ts[40];
		bgtk_log_timestamp(ts, sizeof(ts));
		bgtk_log_fp = stderr;
		fprintf(stderr, "%s [%s] log dir unavailable; using stderr only\n",
			ts, bgtk_log_name);
		return;
	}

	snprintf(path, sizeof(path), "%s/%s.log", dir, bgtk_log_name);
	bgtk_log_fp = fopen(path, "a");
	if (!bgtk_log_fp) {
		char ts[40];
		bgtk_log_timestamp(ts, sizeof(ts));
		bgtk_log_fp = stderr;
		fprintf(stderr, "%s [%s] cannot open %s: %s (stderr only)\n",
			ts, bgtk_log_name, path, strerror(errno));
		return;
	}
	setvbuf(bgtk_log_fp, NULL, _IOLBF, 0);
	/* First line after open — path is useful when diagnosing launch failures. */
	{
		char ts[40];
		bgtk_log_timestamp(ts, sizeof(ts));
		fprintf(bgtk_log_fp, "%s [%s] === log open pid=%ld path=%s ===\n",
			ts, bgtk_log_name, (long)getpid(), path);
		fflush(bgtk_log_fp);
	}
}

static void bgtk_log_ensure(void)
{
	if (!bgtk_log_fp)
		bgtk_log_open(bgtk_log_name);
}

static void bgtk_log_v(int with_errno, const char *fmt, va_list ap)
{
	int saved = errno;
	char msg[1024];
	vsnprintf(msg, sizeof(msg), fmt, ap);

	char ts[40];
	bgtk_log_timestamp(ts, sizeof(ts));

	bgtk_log_ensure();

	if (with_errno)
		fprintf(bgtk_log_fp, "%s [%s] %s: %s (errno=%d)\n",
			ts, bgtk_log_name, msg, strerror(saved), saved);
	else
		fprintf(bgtk_log_fp, "%s [%s] %s\n", ts, bgtk_log_name, msg);
	fflush(bgtk_log_fp);

	/* Mirror to stderr (with the same timestamp) so interactive runs match
	 * the file; durable record is always the dedicated log file. */
	if (bgtk_log_fp != stderr) {
		if (with_errno)
			fprintf(stderr, "%s [%s] %s: %s\n", ts, bgtk_log_name, msg,
				strerror(saved));
		else
			fprintf(stderr, "%s [%s] %s\n", ts, bgtk_log_name, msg);
	}
}

void bgtk_log(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	bgtk_log_v(0, fmt, ap);
	va_end(ap);
}

void bgtk_log_errno(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	bgtk_log_v(1, fmt, ap);
	va_end(ap);
}

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
	ctx->buffer_mapped = 0;
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

	bgtk_log("init resources %dx%d theme_bg=0x%08X text=0x%08X font_size=%d font='%s'",
		 ctx->width, ctx->height, ctx->theme.background,
		 ctx->theme.button_text, ctx->font_size,
		 ctx->font_path[0] ? ctx->font_path : "(none)");

	// 1. Initialize FreeType
	if (FT_Init_FreeType(&ctx->ft_library)) {
		bgtk_log("FreeType library init failed");
	}
	if (!ctx->ft_library) {
		return;
	}

	// 2. The actual FT loading (New_Face, Select, sizing, validation) lives here
	//    in init_resources. The default *selection* (with #ifdefs) was done in
	//    init_config_defaults.
	if (ctx->font_path[0] != '\0') {
		if (FT_New_Face(ctx->ft_library, ctx->font_path, 0, &ctx->ft_face) != 0) {
			bgtk_log("could not load font '%s'; text will use placeholders",
				 ctx->font_path);
			ctx->font_path[0] = '\0';
		} else {
			bgtk_log("loaded font '%s' size=%d", ctx->font_path, ctx->font_size);
		}
	} else {
		bgtk_log("no font path configured and no system default found");
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
			bgtk_log("font '%s' has no usable glyphs (tw=%d th=%d); dropping face",
				 ctx->font_path, tw, th);
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
	ctx->buffer_mapped = 1; /* caller mapped via bgce_get_buffer / mmap */

	bgtk_init_resources(ctx);
	if (!ctx->ft_library) {
		// freetype failed; buffer is caller-owned for real init.
		free(ctx);
		return NULL;
	}
	return ctx;
}

static void bgtk_release_buffer(struct BGTK_Context *ctx)
{
	if (!ctx || !ctx->shm_buffer)
		return;
	size_t bytes = (size_t)ctx->width * (size_t)ctx->height * BGCE_BYTES_PER_PIXEL;
	if (ctx->buffer_mapped) {
		if (bytes > 0)
			munmap(ctx->shm_buffer, bytes);
	} else {
		free(ctx->shm_buffer);
	}
	ctx->shm_buffer = NULL;
}

int bgtk_handle_buffer_change(struct BGTK_Context *ctx,
			      const struct BufferReply *reply)
{
	if (!ctx || !reply || !reply->shm_name[0] ||
	    reply->width == 0 || reply->height == 0) {
		bgtk_log("buffer change: invalid reply");
		return -1;
	}

	size_t new_bytes = (size_t)reply->width * (size_t)reply->height *
			   BGCE_BYTES_PER_PIXEL;
	int fd = bgce_buf_open(reply->shm_name);
	if (fd < 0) {
		bgtk_log_errno("buffer change: open '%s'", reply->shm_name);
		return -1;
	}
	void *map = mmap(NULL, new_bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	close(fd);
	if (map == MAP_FAILED) {
		bgtk_log_errno("buffer change: mmap %ux%u", reply->width,
			       reply->height);
		return -1;
	}

	bgtk_log("buffer change %dx%d -> %ux%u shm='%s'",
		 ctx->width, ctx->height, reply->width, reply->height,
		 reply->shm_name);

	bgtk_release_buffer(ctx);
	ctx->shm_buffer = map;
	ctx->buffer_mapped = 1;
	ctx->width = (int)reply->width;
	ctx->height = (int)reply->height;
	if (ctx->root_widget) {
		ctx->root_widget->w = ctx->width;
		ctx->root_widget->h = ctx->height;
	}
	return 0;
}

int bgtk_resize_mock(struct BGTK_Context *ctx, int width, int height)
{
	if (!ctx || width < 1 || height < 1 || ctx->buffer_mapped)
		return -1;
	void *nb = calloc((size_t)width * (size_t)height * BGCE_BYTES_PER_PIXEL, 1);
	if (!nb)
		return -1;
	bgtk_release_buffer(ctx);
	ctx->shm_buffer = nb;
	ctx->buffer_mapped = 0;
	ctx->width = width;
	ctx->height = height;
	if (ctx->root_widget) {
		ctx->root_widget->w = width;
		ctx->root_widget->h = height;
	}
	return 0;
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
	/* Release framebuffer (mmap for real apps, malloc for mock). */
	bgtk_release_buffer(ctx);
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
	/* buffer released inside bgtk_destroy via bgtk_release_buffer. */
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
