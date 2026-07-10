#define _GNU_SOURCE
/* apps/terminal.c
 *
 * Minimal terminal emulator – real-server entry point.
 * Terminal core lives in apps/term_core.c.
 *
 * Build: make terminal
 */

#include <bgce.h>
#include <bgtk.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <util.h>
#elif defined(__linux__)
#include <pty.h>
#endif

#include "internal.h"
#include "terminal.h"

/* ------------------------------------------------------------------ */
/* PTY helpers                                                        */
/* ------------------------------------------------------------------ */
static int open_pty_and_fork(int *master_fd, int cols, int rows)
{
	int mfd = -1, sfd = -1;
	struct winsize ws = { .ws_row = (unsigned short)rows,
			      .ws_col = (unsigned short)cols };

#if defined(__APPLE__) || defined(__linux__)
	if (openpty(&mfd, &sfd, NULL, NULL, &ws) < 0) {
		bgtk_log_errno("openpty(%dx%d)", cols, rows);
		mfd = -1;
		sfd = -1;
	}
#endif

	if (mfd < 0) {
		mfd = posix_openpt(O_RDWR | O_NOCTTY);
		if (mfd < 0) {
			bgtk_log_errno("posix_openpt");
			return -1;
		}
		if (grantpt(mfd) < 0) {
			bgtk_log_errno("grantpt fd=%d", mfd);
			close(mfd);
			return -1;
		}
		if (unlockpt(mfd) < 0) {
			bgtk_log_errno("unlockpt fd=%d", mfd);
			close(mfd);
			return -1;
		}
		char *sname = ptsname(mfd);
		if (!sname) {
			bgtk_log_errno("ptsname fd=%d", mfd);
			close(mfd);
			return -1;
		}
		sfd = open(sname, O_RDWR);
		if (sfd < 0) {
			bgtk_log_errno("open slave '%s'", sname);
			close(mfd);
			return -1;
		}
		if (ioctl(sfd, TIOCSWINSZ, &ws) < 0)
			bgtk_log_errno("TIOCSWINSZ %dx%d", cols, rows);
	}

	pid_t pid = fork();
	if (pid < 0) {
		bgtk_log_errno("fork for shell");
		close(mfd);
		close(sfd);
		return -1;
	}

	if (pid == 0) {
		close(mfd);
		if (setsid() < 0)
			_exit(126);
#ifdef TIOCSCTTY
		(void)ioctl(sfd, TIOCSCTTY, 0);
#endif
		dup2(sfd, 0);
		dup2(sfd, 1);
		dup2(sfd, 2);
		if (sfd > 2)
			close(sfd);

		const char *sh = getenv("SHELL");
		if (!sh || !sh[0])
			sh = "/bin/sh";
		setenv("TERM", "xterm-256color", 1);
		setenv("COLORTERM", "truecolor", 0);
		execlp(sh, sh, (char *)NULL);
		execl("/bin/sh", "sh", (char *)NULL);
		_exit(127);
	}

	close(sfd);
	*master_fd = mfd;
	int fl = fcntl(mfd, F_GETFL);
	if (fl >= 0)
		fcntl(mfd, F_SETFL, fl | O_NONBLOCK);
	bgtk_log("PTY ready master_fd=%d pid=%ld cols=%d rows=%d",
		 mfd, (long)pid, cols, rows);
	return pid;
}

static void pty_set_winsize(int master_fd, int cols, int rows)
{
	struct winsize ws = {
		.ws_row = (unsigned short)rows,
		.ws_col = (unsigned short)cols,
	};
	if (ioctl(master_fd, TIOCSWINSZ, &ws) < 0)
		bgtk_log_errno("TIOCSWINSZ master %dx%d", cols, rows);
}

/*
 * Root chrome from theme: [border][frame_margin][padding][cells]…
 * Focus/unfocus border is always outermost (see draw_frame).
 */
static void term_chrome(struct BGTK_Context *ctx, int *pad, int *fmar, int *bw)
{
	int p = (ctx && ctx->theme.padding > 0) ? ctx->theme.padding : 12;
	int m = (ctx && ctx->theme.frame_margin >= 0) ? ctx->theme.frame_margin
						      : 0;
	int b = ctx ? (int)ctx->theme.frame_border_size : 1;

	if (b < 0)
		b = 0;
	if (pad)
		*pad = p;
	if (fmar)
		*fmar = m;
	if (bw)
		*bw = b;
}

static int term_inner_size(struct BGTK_Context *ctx, int win_w, int win_h,
			   int *out_w, int *out_h)
{
	int pad, fmar, bw, iw, ih;

	term_chrome(ctx, &pad, &fmar, &bw);
	iw = win_w - 2 * (fmar + bw + pad);
	ih = win_h - 2 * (fmar + bw + pad);
	if (iw < 1)
		iw = 1;
	if (ih < 1)
		ih = 1;
	if (out_w)
		*out_w = iw;
	if (out_h)
		*out_h = ih;
	return 0;
}

/* Rebuild image surface + cell grid for a new window size. */
static int terminal_apply_size(struct BGTK_Context *ctx, struct Term_State *ts,
			       struct BGTK_Widget *img, struct BGTK_Widget *frame,
			       int master_fd, int *inner_w, int *inner_h)
{
	int pad, fmar, bw, iw, ih;

	term_chrome(ctx, &pad, &fmar, &bw);
	term_inner_size(ctx, ctx->width, ctx->height, &iw, &ih);

	term_measure_cell(ts, ctx);
	int cols = iw / ts->cell_w;
	int rows = ih / ts->cell_h;
	if (cols < 1)
		cols = 1;
	if (rows < 1)
		rows = 1;

	if (term_resize(ts, cols, rows) < 0) {
		bgtk_log("term_resize(%d,%d) failed", cols, rows);
		return -1;
	}

	uint32_t *np = realloc(img->data.image.pixels,
			       (size_t)iw * (size_t)ih * sizeof(uint32_t));
	if (!np) {
		bgtk_log_errno("realloc terminal pixels %dx%d", iw, ih);
		return -1;
	}
	img->data.image.pixels = np;
	img->data.image.img_w = iw;
	img->data.image.img_h = ih;
	img->w = iw;
	img->h = ih;
	img->padding = 0;
	img->margin = 0;

	if (frame) {
		frame->w = ctx->width;
		frame->h = ctx->height;
		frame->padding = pad;
		frame->margin = fmar;
	}

	*inner_w = iw;
	*inner_h = ih;

	if (master_fd >= 0)
		pty_set_winsize(master_fd, cols, rows);

	bgtk_log("terminal size window=%dx%d inner=%dx%d grid=%dx%d "
		 "pad=%d fmar=%d bw=%d cell=%dx%d",
		 ctx->width, ctx->height, iw, ih, cols, rows, pad, fmar, bw,
		 ts->cell_w, ts->cell_h);
	return 0;
}

int main(void)
{
	setvbuf(stdout, NULL, _IONBF, 0);
	setvbuf(stderr, NULL, _IONBF, 0);
	bgtk_log_open("terminal");

	signal(SIGCHLD, SIG_IGN);

	bgtk_log("starting terminal emulator");

	int conn_fd = bgce_connect();
	if (conn_fd < 0) {
		bgtk_log_errno("bgce_connect (is bgce running?)");
		return 1;
	}
	bgtk_log("connected to BGCE conn_fd=%d", conn_fd);

	int width = 800, height = 600;
	struct BufferRequest breq = { .width = width, .height = height };
	void *buffer = bgce_get_buffer(conn_fd, breq);
	if (!buffer) {
		bgtk_log("bgce_get_buffer %dx%d failed", width, height);
		bgce_disconnect(conn_fd);
		return 1;
	}
	bgtk_log("got buffer %p %dx%d", buffer, width, height);

	struct BGTK_Context *ctx = bgtk_init(conn_fd, buffer, width, height);
	if (!ctx) {
		bgtk_log("bgtk_init failed — check fonts / FreeType / log above");
		return 1;
	}

	struct Term_State tmp_ts = {0};
	int pad, fmar, bw, inner_w, inner_h, cols, rows;

	tmp_ts.cols = tmp_ts.rows = 1;
	term_measure_cell(&tmp_ts, ctx);

	term_chrome(ctx, &pad, &fmar, &bw);
	term_inner_size(ctx, width, height, &inner_w, &inner_h);
	cols = inner_w / tmp_ts.cell_w;
	rows = inner_h / tmp_ts.cell_h;
	if (cols < 1)
		cols = 1;
	if (rows < 1)
		rows = 1;
	{
		FT_Face mono = bgtk_font_face(ctx, BGTK_FONT_MONO);
		bgtk_log("cell=%dx%d grid=%dx%d inner=%dx%d pad=%d fmar=%d bw=%d "
			 "mono='%s' fixed=%d family='%s'",
			 tmp_ts.cell_w, tmp_ts.cell_h, cols, rows, inner_w,
			 inner_h, pad, fmar, bw,
			 ctx->font_mono_path[0] ? ctx->font_mono_path
						: "(none)",
			 mono && FT_IS_FIXED_WIDTH(mono) ? 1 : 0,
			 mono && mono->family_name ? mono->family_name : "?");
	}

	struct Term_State *ts = term_create(cols, rows);
	if (!ts) {
		bgtk_log("term_create(%d,%d) failed", cols, rows);
		return 1;
	}
	ts->cell_w = tmp_ts.cell_w;
	ts->cell_h = tmp_ts.cell_h;
	term_apply_theme(ts, ctx);

	struct BGTK_Widget *img = bgtk_image(ctx, NULL, inner_w, inner_h,
					     (BGTK_Options){.padding = 0,
							    .margin = 0});
	img->data.image.pixels = calloc((size_t)inner_w * inner_h,
					sizeof(uint32_t));
	if (!img->data.image.pixels) {
		bgtk_log_errno("calloc terminal pixels %dx%d", inner_w, inner_h);
		return 1;
	}
	img->data.image.img_w = inner_w;
	img->data.image.img_h = inner_h;
	struct BGTK_Widget *frame = bgtk_frame(ctx, img, width, height,
					       (BGTK_Options){.padding = pad,
							      .margin = fmar});
	ctx->root_widget = frame;

	int master_fd;
	pid_t child = open_pty_and_fork(&master_fd, cols, rows);
	if (child < 0) {
		bgtk_log("PTY failed (see prior errors); aborting");
		return 1;
	}
	ts->pty_fd = master_fd;

	term_render(ts, ctx, img->data.image.pixels, inner_w, inner_h);
	bgtk_draw_widgets(ctx);

	struct pollfd fds[2];
	fds[0].fd = conn_fd;
	fds[0].events = POLLIN;
	fds[1].fd = master_fd;
	fds[1].events = POLLIN;

	int quit = 0;

	while (!quit) {
		int ret = poll(fds, 2, -1);
		if (ret < 0) {
			if (errno == EINTR)
				continue;
			bgtk_log_errno("poll");
			break;
		}

		if (fds[1].revents & POLLIN) {
			char buf[4096];
			ssize_t n = read(master_fd, buf, sizeof(buf));
			if (n > 0) {
				term_feed(ts, buf, (int)n);
				term_render(ts, ctx, img->data.image.pixels,
					    inner_w, inner_h);
				bgtk_draw_widgets(ctx);
			} else if (n == 0 ||
				   (n < 0 && errno != EAGAIN && errno != EINTR)) {
				if (n < 0)
					bgtk_log_errno("PTY read");
				else
					bgtk_log("PTY EOF (shell exited?)");
				quit = 1;
			}
		}
		if (fds[1].revents & (POLLHUP | POLLERR)) {
			bgtk_log("PTY hangup/error revents=0x%x", fds[1].revents);
			for (;;) {
				char buf[4096];
				ssize_t n = read(master_fd, buf, sizeof(buf));
				if (n <= 0)
					break;
				term_feed(ts, buf, (int)n);
			}
			quit = 1;
		}

		if (fds[0].revents & POLLIN) {
			struct BGCEMessage msg;
			ssize_t bytes = bgce_recv_msg(conn_fd, &msg);
			if (bytes <= 0) {
				bgtk_log("BGCE connection closed (recv=%zd)", bytes);
				quit = 1;
				continue;
			}
			switch (msg.type) {
			case MSG_INPUT_EVENT: {
				struct InputEvent *ev = &msg.data.input_event;
				int mods;
				char out[8];
				int n;
				int need_draw = 0;

				/* Mouse wheel → scrollback view (not PTY). */
				if (ev->type == EV_REL &&
				    ev->code == REL_WHEEL) {
					/* value>0 = wheel up = older history */
					int lines = ev->value * 3;
					if (lines == 0)
						break;
					if (term_view_scroll(ts, lines)) {
						term_render(ts, ctx,
							    img->data.image.pixels,
							    inner_w, inner_h);
						bgtk_draw_widgets(ctx);
					}
					break;
				}
				if (ev->type != EV_KEY)
					break;
				/* Ignore input while unfocused (stale mod
				 * edges and key repeats from other clients). */
				if (!ctx->window_focused) {
					/* Still track mod releases so a Ctrl
					 * up after focus-loss cannot be lost
					 * forever if focus returns mid-chord. */
					if (ev->value == 0)
						bgtk_update_modifiers(ctx, *ev);
					break;
				}
				bgtk_update_modifiers(ctx, *ev);
				if (ev->code == KEY_LEFTSHIFT ||
				    ev->code == KEY_RIGHTSHIFT ||
				    ev->code == KEY_LEFTCTRL ||
				    ev->code == KEY_RIGHTCTRL ||
				    ev->code == KEY_LEFTALT ||
				    ev->code == KEY_RIGHTALT)
					break;
				/* Press / autorepeat only — never synthesize
				 * on release. */
				if (ev->value != 1 && ev->value != 2)
					break;

				/* Shift+PageUp/PageDown or PageUp/Down:
				 * scroll history without sending to the shell. */
				mods = bgtk_mods_from_ctx(ctx);
				if (ev->code == KEY_PAGEUP ||
				    ev->code == KEY_PAGEDOWN) {
					int delta = (ev->code == KEY_PAGEUP)
							    ? ts->rows
							    : -ts->rows;
					if (delta == 0)
						delta = (ev->code == KEY_PAGEUP)
								? 1
								: -1;
					if (term_view_scroll(ts, delta)) {
						term_render(ts, ctx,
							    img->data.image.pixels,
							    inner_w, inner_h);
						bgtk_draw_widgets(ctx);
					}
					break;
				}
				/* Shift+Up/Down: line-wise history scroll. */
				if ((mods & BGTK_MOD_SHIFT) &&
				    (ev->code == KEY_UP ||
				     ev->code == KEY_DOWN)) {
					int delta =
						(ev->code == KEY_UP) ? 1 : -1;
					if (term_view_scroll(ts, delta)) {
						term_render(ts, ctx,
							    img->data.image.pixels,
							    inner_w, inner_h);
						bgtk_draw_widgets(ctx);
					}
					break;
				}

				/* Typing returns to live bottom, then to PTY. */
				if (term_view_to_bottom(ts))
					need_draw = 1;

				n = bgtk_key_to_bytes(ev->code, mods,
						      BGTK_KEY_TTY, out,
						      sizeof(out));
				/*
				 * Stuck-Ctrl safety: Ctrl+J/K are LF/VT. In vi
				 * that scrolls the region ("cursor down but
				 * every line jumps up"). Prefer plain j/k and
				 * clear sticky ctrl — intentional Ctrl+J is
				 * rare interactively.
				 */
				if (n == 1 && (unsigned char)out[0] < 0x20 &&
				    (ev->code == KEY_J || ev->code == KEY_K)) {
					bgtk_log("demote Ctrl+%c -> '%c' "
						 "(clear stuck mods)",
						 ev->code == KEY_J ? 'J' : 'K',
						 ev->code == KEY_J ? 'j' : 'k');
					out[0] = (ev->code == KEY_J) ? 'j'
								     : 'k';
					bgtk_clear_modifiers(ctx);
				}
				if (n > 0) {
					if (write(master_fd, out, (size_t)n) < 0)
						bgtk_log_errno("PTY write key");
				}
				/* Esc resets sticky mods for the next motion. */
				if (ev->code == KEY_ESC)
					bgtk_clear_modifiers(ctx);
				if (need_draw) {
					term_render(ts, ctx,
						    img->data.image.pixels,
						    inner_w, inner_h);
					bgtk_draw_widgets(ctx);
				}
				break;
			}
			case MSG_FOCUS_CHANGE:
				/* Clear sticky mod bits (a lost Ctrl release
				 * otherwise turns j/k into LF/VT and vi insert
				 * mode corrupts the buffer). Re-render cells
				 * before compositing so focus dim is clean. */
				ctx->window_focused = msg.data.focus_event.state;
				bgtk_clear_modifiers(ctx);
				term_render(ts, ctx, img->data.image.pixels,
					    inner_w, inner_h);
				bgtk_draw_widgets(ctx);
				break;
			case MSG_BUFFER_CHANGE:
				if (bgtk_handle_buffer_change(
					    ctx, &msg.data.buffer_reply) == 0) {
					if (terminal_apply_size(ctx, ts, img,
								frame, master_fd,
								&inner_w,
								&inner_h) == 0) {
						term_render(ts, ctx,
							    img->data.image.pixels,
							    inner_w, inner_h);
						bgtk_draw_widgets(ctx);
					}
				}
				break;
			default:
				break;
			}
		}
	}

	bgtk_log("shutting down master_fd=%d", master_fd);
	close(master_fd);
	term_destroy(ts);
	bgtk_destroy(ctx);
	return 0;
}
