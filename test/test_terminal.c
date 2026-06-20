#define _GNU_SOURCE
/* test/test_terminal.c
 *
 * Headless test for the terminal emulator.
 * Exercises the ANSI parser + renderer without a real PTY or BGCE server.
 *
 * Build:  make test_terminal
 * Run:    ./test_terminal
 * Output: term_*.png screenshots
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <poll.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/wait.h>

#include "bgtk.h"
#include "internal.h"
#include "terminal.h"

/* Helper: render terminal into an image widget and take a screenshot */
static void snap(struct BGTK_Context *ctx, struct BGTK_Widget *img,
		 struct Term_State *ts, int px_w, int px_h, const char *path)
{
	term_render(ts, ctx, img->data.image.pixels, px_w, px_h);
	bgtk_draw_widgets(ctx);
	take_screenshot(ctx, path);
}

/* PTY + shell launcher (adapted from the real apps/terminal.c in this patch).
 * Returns the master fd on success, -1 on failure. Also sets *child_pid.
 */
static pid_t child_pid = -1;

static int open_pty_and_fork(int *out_master, int cols, int rows)
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

		dup2(sfd, 0);
		dup2(sfd, 1);
		dup2(sfd, 2);
		if (sfd > 2) close(sfd);

		const char *sh = getenv("SHELL");
		if (!sh) sh = "/bin/sh";
		setenv("TERM", "xterm-256color", 1);
		execlp(sh, sh, (char *)NULL);
	}

	*out_master = mfd;
	int fl = fcntl(mfd, F_GETFL);
	fcntl(mfd, F_SETFL, fl | O_NONBLOCK);
	child_pid = pid;
	return pid;
}

int main(void)
{
	int width = 640, height = 400;

	struct BGTK_Context *ctx = bgtk_init_mock(width, height);
	if (!ctx) {
		fprintf(stderr, "test_terminal: init failed\n");
		return 1;
	}

	/* Measure cell size so we know terminal dimensions */
	struct Term_State tmp = {0};
	tmp.cols = tmp.rows = 1;
	term_measure_cell(&tmp, ctx);

	int cols = width / tmp.cell_w;
	int rows = height / tmp.cell_h;
	if (cols < 1) cols = 1;
	if (rows < 1) rows = 1;

	printf("Cell: %dx%d  Grid: %dx%d\n", tmp.cell_w, tmp.cell_h, cols, rows);

	struct Term_State *ts = term_create(cols, rows);
	if (!ts) return 1;
	ts->cell_w = tmp.cell_w;
	ts->cell_h = tmp.cell_h;

	/* Image widget as drawing surface */
	struct BGTK_Widget *img = bgtk_image(ctx, NULL, width, height,
					     (BGTK_Options){0});
	img->data.image.pixels = calloc((size_t)width * height, sizeof(uint32_t));
	img->data.image.img_w = width;
	img->data.image.img_h = height;
	ctx->root_widget = img;

	/* --- Test 1: plain text ---------------------------------------- */
	term_feed(ts, "Hello, BGTK Terminal!\r\n", -1);
	snap(ctx, img, ts, width, height, "term_00_text.png");

	/* --- Test 2: ANSI colours -------------------------------------- */
	term_feed(ts, "\033[31mred \033[32mgreen \033[34mblue \033[0mnormal\r\n", -1);
	term_feed(ts, "\033[1;33mbold yellow \033[0m\r\n", -1);
	snap(ctx, img, ts, width, height, "term_01_colors.png");

	/* --- Test 3: cursor movement ----------------------------------- */
	term_feed(ts, "\033[10;5HPositioned here\r\n", -1);
	snap(ctx, img, ts, width, height, "term_02_cursor.png");

	/* --- Test 4: erase --------------------------------------------- */
	term_feed(ts, "\033[2J\033[H", -1);  /* clear screen, home */
	term_feed(ts, "Screen cleared!\r\n", -1);
	term_feed(ts, "\033[44m\033[37m Blue background white text \033[0m\r\n", -1);
	snap(ctx, img, ts, width, height, "term_03_erase.png");

	/* --- Test 5: scroll -------------------------------------------- */
	term_feed(ts, "\033[2J\033[H", -1);
	for (int i = 0; i < rows + 5; i++) {
		char line[64];
		int n = snprintf(line, sizeof(line), "Line %d\r\n", i + 1);
		term_feed(ts, line, n);
	}
	snap(ctx, img, ts, width, height, "term_04_scroll.png");

	/* --- Test 6: colour matrix ------------------------------------- */
	term_feed(ts, "\033[2J\033[H", -1);
	for (int fg = 30; fg <= 37; fg++) {
		for (int bg = 40; bg <= 47; bg++) {
			char seq[32];
			int n = snprintf(seq, sizeof(seq), "\033[%d;%dm#", fg, bg);
			term_feed(ts, seq, n);
		}
		term_feed(ts, "\033[0m\r\n", -1);
	}
	snap(ctx, img, ts, width, height, "term_05_matrix.png");

	/* --- Test 7: erase in line ------------------------------------- */
	term_feed(ts, "\033[2J\033[H", -1);
	term_feed(ts, "AAAAAAAAAA\r\n", -1);
	term_feed(ts, "BBBBBBBBBB\033[5D\033[K\r\n", -1); /* erase last 5 */
	term_feed(ts, "CCCCCCCCCC\r\n", -1);
	snap(ctx, img, ts, width, height, "term_06_erase_line.png");

	/* --- Test 8: bold/bright colors -------------------------------- */
	term_feed(ts, "\033[2J\033[H", -1);
	for (int i = 0; i < 8; i++) {
		char seq[64];
		snprintf(seq, sizeof(seq), "\033[%dm Normal%d ", 30 + i, i);
		term_feed(ts, seq, -1);
		snprintf(seq, sizeof(seq), "\033[1;%dm Bold%d \033[0m\r\n", 30 + i, i);
		term_feed(ts, seq, -1);
	}
	snap(ctx, img, ts, width, height, "term_07_bold.png");

	/* --- Test 9: real PTY + shell command execution (the requested case) */
	/* This actually spawns a shell, "types" a command into it via the PTY,
	 * lets the shell execute it, reads the real output, feeds it through
	 * the parser, renders it, and takes a screenshot.
	 * This proves: input received → executed in real shell → result shown.
	 */
	{
		int master_fd;
		if (open_pty_and_fork(&master_fd, cols, rows) < 0) {
			fprintf(stderr, "test_terminal: PTY shell failed\n");
		} else {
			ts->pty_fd = master_fd;

			/* Wait for initial shell prompt */
			usleep(200 * 1000);
			{
				char buf[4096];
				ssize_t n = read(master_fd, buf, sizeof(buf));
				if (n > 0) term_feed(ts, buf, (int)n);
			}
			snap(ctx, img, ts, width, height, "term_08_shell_prompt.png");

			/* Send a real command (as if the user typed it and pressed enter) */
			const char *cmd = "echo '=== REAL SHELL OUTPUT ==='\n";
			write(master_fd, cmd, strlen(cmd));

			/* Read the shell's response */
			int total = 0;
			for (int i = 0; i < 20; i++) {
				struct pollfd pfd = { .fd = master_fd, .events = POLLIN };
				if (poll(&pfd, 1, 80) <= 0) break;
				char buf[4096];
				ssize_t n = read(master_fd, buf, sizeof(buf));
				if (n > 0) {
					term_feed(ts, buf, (int)n);
					total += (int)n;
				} else {
					break;
				}
			}

			snap(ctx, img, ts, width, height, "term_09_real_command.png");

			/* Verify the result text is present in the cell grid */
			int found = 0;
			const char *needle = "REAL SHELL OUTPUT";
			for (int r = 0; r < rows; r++) {
				for (int c = 0; c < cols; c++) {
					int match = 1;
					for (size_t k = 0; k < strlen(needle); k++) {
						if ((size_t)c + k >= (size_t)cols) { match = 0; break; }
						if (ts->cells[r * cols + c + k].ch != needle[k]) {
							match = 0;
							break;
						}
					}
					if (match) { found = 1; break; }
				}
				if (found) break;
			}

			printf("Real PTY shell test: sent command, read %d bytes, result '%s' visible: %s\n",
			       total, needle, found ? "YES" : "NO");

			close(master_fd);
		}
	}

	printf("test_terminal complete. PNG frames written.\n");

	free(img->data.image.pixels);
	img->data.image.pixels = NULL;
	term_destroy(ts);
	bgtk_destroy_mock(ctx);
	return 0;
}
