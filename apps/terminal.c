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
#include <termios.h>
#include <unistd.h>

#include "internal.h"
#include "terminal.h"

/* ------------------------------------------------------------------ */
/* PTY helpers                                                        */
/* ------------------------------------------------------------------ */
static int open_pty_and_fork(int *master_fd, int cols, int rows)
{
	int mfd = posix_openpt(O_RDWR | O_NOCTTY);
	if (mfd < 0) return -1;
	if (grantpt(mfd) || unlockpt(mfd)) { close(mfd); return -1; }

	char *sname = ptsname(mfd);
	if (!sname) { close(mfd); return -1; }

	pid_t pid = fork();
	if (pid < 0) { close(mfd); return -1; }

	if (pid == 0) {
		close(mfd);
		setsid();
		int sfd = open(sname, O_RDWR);
		if (sfd < 0) _exit(127);
		struct winsize ws = { .ws_row = rows, .ws_col = cols };
		ioctl(sfd, TIOCSWINSZ, &ws);

		/* Set slave to raw mode so the shell controls echo, editing, etc.
		 * This is critical for proper terminal emulator behavior.
		 */
		struct termios tio;
		if (tcgetattr(sfd, &tio) == 0) {
			cfmakeraw(&tio);
			/* Make sure we have at least some sane settings */
			tio.c_cc[VMIN] = 1;
			tio.c_cc[VTIME] = 0;
			tcsetattr(sfd, TCSANOW, &tio);
		}

		dup2(sfd, 0);
		dup2(sfd, 1);
		dup2(sfd, 2);
		if (sfd > 2) close(sfd);
		const char *sh = getenv("SHELL");
		if (!sh) sh = "/bin/sh";
		setenv("TERM", "xterm-256color", 1);
		execlp(sh, sh, (char *)NULL);
		_exit(127);
	}

	*master_fd = mfd;
	int fl = fcntl(mfd, F_GETFL);
	fcntl(mfd, F_SETFL, fl | O_NONBLOCK);
	return pid;
}

int main(void)
{
	setvbuf(stdout, NULL, _IONBF, 0);
	setvbuf(stderr, NULL, _IONBF, 0);

	signal(SIGCHLD, SIG_IGN);

	int conn_fd = bgce_connect();
	if (conn_fd < 0) {
		fprintf(stderr, "terminal: Failed to connect to BGCE server.\n");
		return 1;
	}

	int width = 800, height = 600;
	struct BufferRequest breq = { .width = width, .height = height };
	void *buffer = bgce_get_buffer(conn_fd, breq);
	if (!buffer) {
		fprintf(stderr, "terminal: Failed to get buffer.\n");
		bgce_disconnect(conn_fd);
		return 1;
	}

	struct BGTK_Context *ctx = bgtk_init(conn_fd, buffer, width, height);
	if (!ctx) {
		fprintf(stderr, "terminal: Failed to init BGTK.\n");
		return 1;
	}

	/* Compute cell size and terminal dimensions */
	struct Term_State tmp_ts = {0};
	tmp_ts.cols = tmp_ts.rows = 1;
	term_measure_cell(&tmp_ts, ctx);
	int cols = width / tmp_ts.cell_w;
	int rows = height / tmp_ts.cell_h;
	if (cols < 1) cols = 1;
	if (rows < 1) rows = 1;

	struct Term_State *ts = term_create(cols, rows);
	if (!ts) return 1;
	ts->cell_w = tmp_ts.cell_w;
	ts->cell_h = tmp_ts.cell_h;

	/* Create image widget as the drawing surface */
	struct BGTK_Widget *img = bgtk_image(ctx, NULL, width, height,
					     (BGTK_Options){0});
	img->data.image.pixels = calloc((size_t)width * height, sizeof(uint32_t));
	img->data.image.img_w = width;
	img->data.image.img_h = height;
	ctx->root_widget = img;

	/* Open PTY */
	int master_fd;
	pid_t child = open_pty_and_fork(&master_fd, cols, rows);
	if (child < 0) {
		fprintf(stderr, "terminal: PTY failed.\n");
		return 1;
	}
	ts->pty_fd = master_fd;

	/* Initial render */
	term_render(ts, ctx, img->data.image.pixels, width, height);
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
			break;
		}

		/* PTY output */
		if (fds[1].revents & POLLIN) {
			char buf[4096];
			ssize_t n = read(master_fd, buf, sizeof(buf));
			if (n > 0) {
				term_feed(ts, buf, (int)n);
				term_render(ts, ctx, img->data.image.pixels,
					    width, height);
				bgtk_draw_widgets(ctx);
			} else if (n == 0 ||
				   (n < 0 && errno != EAGAIN && errno != EINTR)) {
				quit = 1;
			}
		}
		if (fds[1].revents & (POLLHUP | POLLERR)) {
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
			if (bytes <= 0) { quit = 1; continue; }
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

	close(master_fd);
	term_destroy(ts);
	bgtk_destroy(ctx);
	return 0;
}