#define STB_IMAGE_WRITE_IMPLEMENTATION

#include "bgtk.h"

#include <bgce.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <signal.h>
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

/* backtrace(3) is glibc/BSD; musl and many embedded toolchains lack it. */
#if defined(__has_include)
#if __has_include(<execinfo.h>)
#include <execinfo.h>
#define BGTK_HAVE_BACKTRACE 1
#endif
#elif defined(__GLIBC__) || defined(__APPLE__)
#include <execinfo.h>
#define BGTK_HAVE_BACKTRACE 1
#endif
#ifndef BGTK_HAVE_BACKTRACE
#define BGTK_HAVE_BACKTRACE 0
#endif

#include "config.h"
#include "internal.h"

/* Dedicated BGTK logs live under ~/.cache/bgtk/ (or $XDG_CACHE_HOME/bgtk/),
 * separate from BGCE server/client stderr so app and compositor output do not
 * fight over one stream or file. */
static FILE *bgtk_log_fp;
static int bgtk_log_fd = -1; /* raw fd for async-signal-safe crash lines */
static char bgtk_log_name[64] = "bgtk";
static char bgtk_log_file_path[640];
static int bgtk_crash_handlers_installed;

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

/* Async-signal-safe write of a C string to fd (best-effort). */
static void bgtk_write_str(int fd, const char *s)
{
	size_t n;
	if (fd < 0 || !s)
		return;
	n = strlen(s);
	while (n > 0) {
		ssize_t w = write(fd, s, n);
		if (w <= 0)
			break;
		s += (size_t)w;
		n -= (size_t)w;
	}
}

static void bgtk_crash_handler(int sig)
{
	const char *name = "SIGNAL";
	char buf[96];
	int n;

	if (sig == SIGSEGV)
		name = "SIGSEGV";
	else if (sig == SIGABRT)
		name = "SIGABRT";
	else if (sig == SIGBUS)
		name = "SIGBUS";
	else if (sig == SIGFPE)
		name = "SIGFPE";
	else if (sig == SIGILL)
		name = "SIGILL";

	/* Prefer the log file fd; always also try stderr. */
	n = snprintf(buf, sizeof(buf),
		     "\n*** FATAL %s pid=%ld app=%s ***\n", name,
		     (long)getpid(), bgtk_log_name);
	if (n > 0) {
		if (bgtk_log_fd >= 0)
			bgtk_write_str(bgtk_log_fd, buf);
		bgtk_write_str(STDERR_FILENO, buf);
	}
	if (bgtk_log_file_path[0]) {
		bgtk_write_str(STDERR_FILENO, "log: ");
		bgtk_write_str(STDERR_FILENO, bgtk_log_file_path);
		bgtk_write_str(STDERR_FILENO, "\n");
		if (bgtk_log_fd >= 0) {
			bgtk_write_str(bgtk_log_fd, "log: ");
			bgtk_write_str(bgtk_log_fd, bgtk_log_file_path);
			bgtk_write_str(bgtk_log_fd, "\n");
		}
	}
#if BGTK_HAVE_BACKTRACE
	{
		void *bt[32];
		int nbt = backtrace(bt, 32);
		if (bgtk_log_fd >= 0)
			backtrace_symbols_fd(bt, nbt, bgtk_log_fd);
		backtrace_symbols_fd(bt, nbt, STDERR_FILENO);
	}
#endif
	/* Re-raise with default action so core dumps still work. */
	signal(sig, SIG_DFL);
	raise(sig);
}

static void bgtk_install_crash_handlers(void)
{
	struct sigaction sa;

	if (bgtk_crash_handlers_installed)
		return;
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = bgtk_crash_handler;
	sigemptyset(&sa.sa_mask);
	sa.sa_flags = SA_RESETHAND;
	sigaction(SIGSEGV, &sa, NULL);
	sigaction(SIGABRT, &sa, NULL);
	sigaction(SIGBUS, &sa, NULL);
	sigaction(SIGFPE, &sa, NULL);
	sigaction(SIGILL, &sa, NULL);
	bgtk_crash_handlers_installed = 1;
}

static void bgtk_log_atexit(void)
{
	if (bgtk_log_fp && bgtk_log_fp != stderr) {
		char ts[40];
		bgtk_log_timestamp(ts, sizeof(ts));
		fprintf(bgtk_log_fp, "%s [%s] === process exit pid=%ld ===\n",
			ts, bgtk_log_name, (long)getpid());
		fflush(bgtk_log_fp);
	}
}

void bgtk_log_open(const char *app_name)
{
	char dir[512];
	char path[640];
	static int atexit_registered;

	/* Avoid strncpy(dst, dst) when ensure() reopens with bgtk_log_name. */
	if (app_name && app_name[0] && app_name != bgtk_log_name) {
		strncpy(bgtk_log_name, app_name, sizeof(bgtk_log_name) - 1);
		bgtk_log_name[sizeof(bgtk_log_name) - 1] = '\0';
	}

	/* Interactive launches: unbuffered stderr so crash lines appear. */
	setvbuf(stderr, NULL, _IONBF, 0);

	if (bgtk_log_fp && bgtk_log_fp != stderr) {
		fclose(bgtk_log_fp);
		bgtk_log_fp = NULL;
	}
	bgtk_log_fd = -1;
	bgtk_log_file_path[0] = '\0';

	if (bgtk_ensure_log_dir(dir, sizeof(dir)) < 0) {
		char ts[40];
		bgtk_log_timestamp(ts, sizeof(ts));
		bgtk_log_fp = stderr;
		fprintf(stderr,
			"%s [%s] log dir unavailable (HOME/XDG_CACHE_HOME?); "
			"stderr only\n",
			ts, bgtk_log_name);
		bgtk_install_crash_handlers();
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
		bgtk_install_crash_handlers();
		return;
	}
	/* Line-buffered + explicit fflush in log_v; also keep a raw fd. */
	setvbuf(bgtk_log_fp, NULL, _IOLBF, 0);
	bgtk_log_fd = fileno(bgtk_log_fp);
	strncpy(bgtk_log_file_path, path, sizeof(bgtk_log_file_path) - 1);
	bgtk_log_file_path[sizeof(bgtk_log_file_path) - 1] = '\0';

	bgtk_install_crash_handlers();
	if (!atexit_registered) {
		atexit(bgtk_log_atexit);
		atexit_registered = 1;
	}

	{
		char ts[40];
		char cwd[512];
		const char *home = getenv("HOME");
		const char *xdg = getenv("XDG_CACHE_HOME");
		const char *disp = getenv("BGCE_SOCKET");
		bgtk_log_timestamp(ts, sizeof(ts));
		fprintf(bgtk_log_fp,
			"%s [%s] === log open pid=%ld ppid=%ld path=%s ===\n",
			ts, bgtk_log_name, (long)getpid(), (long)getppid(),
			path);
		if (!getcwd(cwd, sizeof(cwd)))
			snprintf(cwd, sizeof(cwd), "(getcwd failed: %s)",
				 strerror(errno));
		fprintf(bgtk_log_fp, "%s [%s] cwd=%s\n", ts, bgtk_log_name, cwd);
		fprintf(bgtk_log_fp, "%s [%s] HOME=%s XDG_CACHE_HOME=%s\n", ts,
			bgtk_log_name, home ? home : "(unset)",
			xdg ? xdg : "(unset)");
		fprintf(bgtk_log_fp,
			"%s [%s] BGCE_SOCKET=%s (default often /tmp/bgce.sock)\n",
			ts, bgtk_log_name, disp ? disp : "(unset)");
		fflush(bgtk_log_fp);
		/* Tell the user where to look when launched from a compositor. */
		fprintf(stderr, "%s [%s] logging to %s\n", ts, bgtk_log_name,
			path);
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

void bgtk_log_flush(void)
{
	if (bgtk_log_fp)
		fflush(bgtk_log_fp);
	fflush(stderr);
}

const char *bgtk_log_path(void)
{
	return bgtk_log_file_path[0] ? bgtk_log_file_path : NULL;
}

void bgtk_log_die(int status, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	bgtk_log_v(0, fmt, ap);
	va_end(ap);
	bgtk_log("fatal: exiting status=%d (log=%s)", status,
		 bgtk_log_file_path[0] ? bgtk_log_file_path : "stderr");
	bgtk_log_flush();
	_exit(status);
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

/* Load and validate one FreeType face from path. Returns NULL on failure. */
static FT_Face bgtk_load_face(struct BGTK_Context *ctx, const char *path,
			      const char *role)
{
	FT_Face face = NULL;
	int tw = 0, th = 0;
	int err;

	if (!ctx || !ctx->ft_library || !path || !path[0]) {
		bgtk_log("skip %s font: empty path", role ? role : "font");
		return NULL;
	}
	bgtk_log("loading %s font '%s'...", role ? role : "font", path);
	bgtk_log_flush();
	err = FT_New_Face(ctx->ft_library, path, 0, &face);
	if (err != 0) {
		bgtk_log("could not load %s font '%s' (freetype err=0x%x)",
			 role ? role : "font", path, (unsigned)err);
		return NULL;
	}
	/* Prefer Unicode cmap so ASCII is not remapped to symbol glyphs. */
	FT_Select_Charmap(face, FT_ENCODING_UNICODE);
	FT_Set_Pixel_Sizes(face, 0, ctx->font_size > 0 ? ctx->font_size : 14);
	measure_text(face, "Ag", &tw, &th);
	if (tw <= 0 || th <= 0) {
		bgtk_log("%s font '%s' has no usable glyphs (tw=%d th=%d)",
			 role ? role : "font", path, tw, th);
		FT_Done_Face(face);
		return NULL;
	}
	bgtk_log("loaded %s font '%s' size=%d face_index=0",
		 role ? role : "font", path, ctx->font_size);
	bgtk_log_flush();
	return face;
}

// Common resource setup (config, freetype, font) after buffer/conn/ dims are set.
// Used by both real bgtk_init and bgtk_init_mock.
static void bgtk_init_resources(struct BGTK_Context *ctx)
{
	ctx->root_widget = NULL;
	ctx->window_focused = 1;
	ctx->shift_held = 0;
	ctx->ctrl_held = 0;
	ctx->alt_held = 0;

	// Load config (parse_config now always starts from defaults via init_config_defaults).
	struct config config = {0};
	parse_config(&config);
	ctx->theme = config.theme;
	strncpy(ctx->font_sans_path, config.font_sans_path, MAX_PATH_LEN - 1);
	ctx->font_sans_path[MAX_PATH_LEN - 1] = '\0';
	strncpy(ctx->font_mono_path, config.font_mono_path, MAX_PATH_LEN - 1);
	ctx->font_mono_path[MAX_PATH_LEN - 1] = '\0';
	strncpy(ctx->font_serif_path, config.font_serif_path, MAX_PATH_LEN - 1);
	ctx->font_serif_path[MAX_PATH_LEN - 1] = '\0';
	ctx->font_size = config.font_size;
	ctx->ft_face = NULL;
	ctx->ft_face_mono = NULL;
	ctx->ft_face_serif = NULL;

	bgtk_log("init resources %dx%d theme_bg=0x%08X text=0x%08X font_size=%d "
		 "sans='%s' mono='%s' serif='%s'",
		 ctx->width, ctx->height, ctx->theme.background,
		 ctx->theme.button_text, ctx->font_size,
		 ctx->font_sans_path[0] ? ctx->font_sans_path : "(none)",
		 ctx->font_mono_path[0] ? ctx->font_mono_path : "(none)",
		 ctx->font_serif_path[0] ? ctx->font_serif_path : "(none)");

	// 1. Initialize FreeType
	if (FT_Init_FreeType(&ctx->ft_library)) {
		bgtk_log("FreeType library init failed");
	}
	if (!ctx->ft_library) {
		return;
	}

	/* Load sans (UI), mono, serif. Reuse faces when paths match so we do
	 * not open the same file three times (some TTC/embedded faces hang
	 * or fail on the second open). */
	ctx->ft_face = bgtk_load_face(ctx, ctx->font_sans_path, "sans");
	if (!ctx->ft_face)
		bgtk_log("no usable UI/sans font; text will use placeholders");

	if (ctx->ft_face && ctx->font_mono_path[0] &&
	    strcmp(ctx->font_mono_path, ctx->font_sans_path) == 0) {
		bgtk_log("mono reuses sans face (same path)");
		ctx->ft_face_mono = ctx->ft_face;
	} else {
		ctx->ft_face_mono =
			bgtk_load_face(ctx, ctx->font_mono_path, "mono");
	}
	if (!ctx->ft_face_mono && ctx->ft_face) {
		bgtk_log("mono fallback to sans face");
		ctx->ft_face_mono = ctx->ft_face;
	}

	if (ctx->ft_face && ctx->font_serif_path[0] &&
	    strcmp(ctx->font_serif_path, ctx->font_sans_path) == 0) {
		bgtk_log("serif reuses sans face (same path)");
		ctx->ft_face_serif = ctx->ft_face;
	} else if (ctx->ft_face_mono && ctx->font_serif_path[0] &&
		   ctx->font_mono_path[0] &&
		   strcmp(ctx->font_serif_path, ctx->font_mono_path) == 0) {
		bgtk_log("serif reuses mono face (same path)");
		ctx->ft_face_serif = ctx->ft_face_mono;
	} else {
		ctx->ft_face_serif =
			bgtk_load_face(ctx, ctx->font_serif_path, "serif");
	}
	if (!ctx->ft_face_serif && ctx->ft_face) {
		bgtk_log("serif fallback to sans face");
		ctx->ft_face_serif = ctx->ft_face;
	}
	bgtk_log("font faces ready sans=%p mono=%p serif=%p",
		 (void *)ctx->ft_face, (void *)ctx->ft_face_mono,
		 (void *)ctx->ft_face_serif);
	bgtk_log_flush();
}

FT_Face bgtk_font_face(struct BGTK_Context *ctx, int role)
{
	FT_Face face = NULL;

	if (!ctx)
		return NULL;
	if (role == BGTK_FONT_MONO)
		face = ctx->ft_face_mono;
	else if (role == BGTK_FONT_SERIF)
		face = ctx->ft_face_serif;
	else
		face = ctx->ft_face;
	return face ? face : ctx->ft_face;
}

struct BGTK_Context *bgtk_init(int conn_fd, void *buffer, int width, int height)
{
	struct BGTK_Context *ctx;

	bgtk_log("bgtk_init begin conn_fd=%d buffer=%p %dx%d", conn_fd, buffer,
		 width, height);
	if (conn_fd < 0)
		bgtk_log("bgtk_init warning: conn_fd < 0 (mock/offline?)");
	if (!buffer) {
		bgtk_log("bgtk_init failed: null shm buffer");
		return NULL;
	}
	if (width < 1 || height < 1) {
		bgtk_log("bgtk_init failed: bad size %dx%d", width, height);
		return NULL;
	}

	ctx = (struct BGTK_Context *)calloc(1, sizeof(struct BGTK_Context));
	if (!ctx) {
		bgtk_log_errno("bgtk_init calloc BGTK_Context");
		return NULL;
	}

	ctx->conn_fd = conn_fd;
	ctx->shm_buffer = buffer;
	ctx->width = width;
	ctx->height = height;
	ctx->buffer_mapped = 1; /* caller mapped via bgce_get_buffer / mmap */

	bgtk_init_resources(ctx);
	if (!ctx->ft_library) {
		bgtk_log("bgtk_init failed: FreeType init failed "
			 "(fonts/libfreetype missing?)");
		free(ctx);
		return NULL;
	}
	bgtk_log("bgtk_init ok faces sans=%s mono=%s serif=%s log=%s",
		 ctx->ft_face ? "yes" : "no",
		 ctx->ft_face_mono ? "yes" : "no",
		 ctx->ft_face_serif ? "yes" : "no",
		 bgtk_log_file_path[0] ? bgtk_log_file_path : "stderr");
	bgtk_log_flush();
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
	/* Faces may be aliases of each other when paths matched — free once. */
	if (ctx->ft_face_serif && ctx->ft_face_serif != ctx->ft_face &&
	    ctx->ft_face_serif != ctx->ft_face_mono) {
		FT_Done_Face(ctx->ft_face_serif);
	}
	ctx->ft_face_serif = NULL;
	if (ctx->ft_face_mono && ctx->ft_face_mono != ctx->ft_face) {
		FT_Done_Face(ctx->ft_face_mono);
	}
	ctx->ft_face_mono = NULL;
	if (ctx->ft_face) {
		FT_Done_Face(ctx->ft_face);
		ctx->ft_face = NULL;
	}
	if (ctx->ft_library) {
		FT_Done_FreeType(ctx->ft_library);
		ctx->ft_library = NULL;
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
	/* Modifier releases only go to the focused client. Losing focus mid-
	 * chord (or a peer exiting on Ctrl+C) otherwise leaves ctrl stuck —
	 * terminal then turns j/k into Ctrl+J/K (LF/VT) and vi "mumbles". */
	bgtk_clear_modifiers(ctx);
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
	bgtk_update_modifiers(ctx, ev);

	/* Keyboard: deliver straight to the focused widget. Nested
	 * table/frame/scroll trees often fail to reach a focused text
	 * field via a root walk (esp. HTML settings pages). */
	if (ev.type == EV_KEY && ev.code < BTN_MISC && ctx->focused_widget &&
	    ctx->focused_widget->handle_event) {
		if (ctx->focused_widget->handle_event(ctx->focused_widget, ev))
			return 1;
	}

	// Start event handling from the root widget (pointer, etc.)
	if (!ctx->root_widget) {
		return 0;
	}
	// Make a copy of the event to avoid modifying the original
	struct InputEvent widget_ev = ev;

	// Pass the event to the root widget
	return ctx->root_widget->handle_event(ctx->root_widget, widget_ev);
}
