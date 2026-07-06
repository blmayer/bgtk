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
		/* Fall through to posix_openpt path */
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
		if (ioctl(sfd, TIOCSCTTY, 0) < 0) {
			/* Non-fatal on some platforms; shell may still work. */
		}
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
		/* execlp failed — try /bin/sh explicitly */
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

int main(void)
{
	bgtk_log_open("terminal");
	setvbuf(stdout, NULL, _IONBF, 0);
	setvbuf(stderr, NULL, _IONBF, 0);

	signal(SIGCHLD, SIG_IGN);

	bgtk_log("starting terminal emulator");

	int conn_fd = bgce_connect();
	if (conn_fd < 0) {
		bgtk_log_errno("bgce_connect");
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
	bgtk_log("got buffer %dx%d", width, height);

	struct BGTK_Context *ctx = bgtk_init(conn_fd, buffer, width, height);
	if (!ctx) {
		bgtk_log("bgtk_init failed");
		return 1;
	}

	/* Compute inner area (frame border eats into the window) */
	int bw = ctx->theme.frame_border_size;
	int inner_w = width - 2 * bw;
	int inner_h = height - 2 * bw;

	/* Compute cell size and terminal dimensions */
	struct Term_State tmp_ts = {0};
	tmp_ts.cols = tmp_ts.rows = 1;
	term_measure_cell(&tmp_ts, ctx);
	int cols = inner_w / tmp_ts.cell_w;
	int rows = inner_h / tmp_ts.cell_h;
	if (cols < 1) cols = 1;
	if (rows < 1) rows = 1;
	bgtk_log("cell=%dx%d grid=%dx%d inner=%dx%d font='%s'",
		 tmp_ts.cell_w, tmp_ts.cell_h, cols, rows, inner_w, inner_h,
		 ctx->font_path[0] ? ctx->font_path : "(none)");

	struct Term_State *ts = term_create(cols, rows);
	if (!ts) {
		bgtk_log("term_create(%d,%d) failed", cols, rows);
		return 1;
	}
	ts->cell_w = tmp_ts.cell_w;
	ts->cell_h = tmp_ts.cell_h;

	/* Create image widget as the drawing surface, wrapped in a frame */
	struct BGTK_Widget *img = bgtk_image(ctx, NULL, inner_w, inner_h,
					     (BGTK_Options){0});
	img->data.image.pixels = calloc((size_t)inner_w * inner_h,
					sizeof(uint32_t));
	if (!img->data.image.pixels) {
		bgtk_log_errno("calloc terminal pixels %dx%d", inner_w, inner_h);
		return 1;
	}
	img->data.image.img_w = inner_w;
	img->data.image.img_h = inner_h;
	struct BGTK_Widget *frame = bgtk_frame(ctx, img, width, height,
					       (BGTK_Options){0});
	ctx->root_widget = frame;

	/* Open PTY */
	int master_fd;
	pid_t child = open_pty_and_fork(&master_fd, cols, rows);
	if (child < 0) {
		bgtk_log("PTY failed (see prior errors); aborting");
		return 1;
	}
	ts->pty_fd = master_fd;

	/* Initial render */
	term_render(ts, ctx, img->data.image.pixels, inner_w, inner_h);
	bgtk_draw_widgets(ctx);

	/* Main loop: poll BGCE connection and PTY */
	struct pollfd fds[2];
	fds[0].fd = conn_fd;
	fds[0].events = POLLIN;
	fds[1].fd = master_fd;
	fds[1].events = POLLIN;

	int ctrl_held = 0;
	int shift_held = 0;
	int quit = 0;

	while (!quit) {
		int ret = poll(fds, 2, -1);
		if (ret < 0) {
			if (errno == EINTR) continue;
			bgtk_log_errno("poll");
			break;
		}

		/* PTY output */
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
				if (n <= 0) break;
				term_feed(ts, buf, (int)n);
			}
			quit = 1;
		}

		/* BGCE events */
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
				if (ev->type != EV_KEY) break;
				if (ev->code == KEY_LEFTSHIFT ||
				    ev->code == KEY_RIGHTSHIFT) {
					shift_held = (ev->value != 0);
					break;
				}
				if (ev->code == KEY_LEFTCTRL ||
				    ev->code == KEY_RIGHTCTRL) {
					ctrl_held = (ev->value != 0);
					break;
				}
				if (ev->value == 1 || ev->value == 2) {
					char out[8];
					int n = term_keycode_to_bytes(
						ev->code, shift_held,
						ctrl_held, out, sizeof(out));
					if (n > 0)
						(void)write(master_fd, out, n);
				}
				break;
			}
			case MSG_FOCUS_CHANGE:
				ctx->window_focused = msg.data.focus_event.state;
				break;
			default: break;
			}
		}
	}

	bgtk_log("shutting down master_fd=%d", master_fd);
	close(master_fd);
	term_destroy(ts);
	bgtk_destroy(ctx);
	return 0;
}
