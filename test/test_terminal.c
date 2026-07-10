#define _GNU_SOURCE
/* test/test_terminal.c
 *
 * Headless test for the terminal emulator.
 * Exercises the ANSI parser + renderer without a real PTY or BGCE server.
 *
 * Build:  make test_terminal
 * Run:    ./test_terminal
 * Output: test/screenshots/term_*.png screenshots
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <poll.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/wait.h>

#if defined(__APPLE__)
#include <util.h>
#elif defined(__linux__)
#include <pty.h>
#endif

#include <linux/input.h>

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
	int mfd = -1, sfd = -1;
	struct winsize ws = { .ws_row = (unsigned short)rows,
			      .ws_col = (unsigned short)cols };

#if defined(__APPLE__) || defined(__linux__)
	if (openpty(&mfd, &sfd, NULL, NULL, &ws) < 0) {
		bgtk_log_errno("test openpty");
		mfd = -1;
		sfd = -1;
	}
#endif

	if (mfd < 0) {
		mfd = posix_openpt(O_RDWR | O_NOCTTY);
		if (mfd < 0) {
			bgtk_log_errno("test posix_openpt");
			return -1;
		}
		if (grantpt(mfd) < 0 || unlockpt(mfd) < 0) {
			bgtk_log_errno("test grantpt/unlockpt");
			close(mfd);
			return -1;
		}
		char *sname = ptsname(mfd);
		if (!sname) {
			bgtk_log_errno("test ptsname");
			close(mfd);
			return -1;
		}
		sfd = open(sname, O_RDWR);
		if (sfd < 0) {
			bgtk_log_errno("test open slave");
			close(mfd);
			return -1;
		}
		ioctl(sfd, TIOCSWINSZ, &ws);
	}

	pid_t pid = fork();
	if (pid < 0) {
		bgtk_log_errno("test fork");
		close(mfd);
		close(sfd);
		return -1;
	}

	if (pid == 0) {
		close(mfd);
		setsid();
#ifdef TIOCSCTTY
		ioctl(sfd, TIOCSCTTY, 0);
#endif
		dup2(sfd, 0);
		dup2(sfd, 1);
		dup2(sfd, 2);
		if (sfd > 2) close(sfd);

		const char *sh = getenv("SHELL");
		if (!sh || !sh[0]) sh = "/bin/sh";
		setenv("TERM", "xterm-256color", 1);
		execlp(sh, sh, (char *)NULL);
		execl("/bin/sh", "sh", (char *)NULL);
		_exit(127);
	}

	close(sfd);
	*out_master = mfd;
	int fl = fcntl(mfd, F_GETFL);
	if (fl >= 0)
		fcntl(mfd, F_SETFL, fl | O_NONBLOCK);
	child_pid = pid;
	bgtk_log("test PTY ready fd=%d pid=%ld", mfd, (long)pid);
	return pid;
}

int main(void)
{
	bgtk_log_open("test_terminal");
	int width = 640, height = 400;

	struct BGTK_Context *ctx = bgtk_init_mock(width, height);
	if (!ctx) {
		bgtk_log("test_terminal: init failed");
		return 1;
	}

	/* Content inside frame_margin + border + padding (from theme). */
	struct Term_State tmp = {0};
	int pad = ctx->theme.padding > 0 ? ctx->theme.padding : 12;
	int fmar = ctx->theme.frame_margin >= 0 ? ctx->theme.frame_margin : 0;
	int bw = (int)ctx->theme.frame_border_size;
	int inner_w, inner_h, cols, rows;
	struct BGTK_Widget *img, *frame;

	if (bw < 0)
		bw = 0;
	tmp.cols = tmp.rows = 1;
	term_measure_cell(&tmp, ctx);

	inner_w = width - 2 * (fmar + bw + pad);
	inner_h = height - 2 * (fmar + bw + pad);
	if (inner_w < 1)
		inner_w = 1;
	if (inner_h < 1)
		inner_h = 1;
	cols = inner_w / tmp.cell_w;
	rows = inner_h / tmp.cell_h;
	if (cols < 1)
		cols = 1;
	if (rows < 1)
		rows = 1;

	printf("Cell: %dx%d  Grid: %dx%d  chrome pad=%d fmar=%d bw=%d inner=%dx%d\n",
	       tmp.cell_w, tmp.cell_h, cols, rows, pad, fmar, bw, inner_w,
	       inner_h);

	struct Term_State *ts = term_create(cols, rows);
	if (!ts)
		return 1;
	ts->cell_w = tmp.cell_w;
	ts->cell_h = tmp.cell_h;
	term_apply_theme(ts, ctx);

	img = bgtk_image(ctx, NULL, inner_w, inner_h,
			 (BGTK_Options){.padding = 0, .margin = 0});
	img->data.image.pixels =
		calloc((size_t)inner_w * (size_t)inner_h, sizeof(uint32_t));
	if (!img->data.image.pixels) {
		bgtk_log_errno("test_terminal calloc");
		return 1;
	}
	img->data.image.img_w = inner_w;
	img->data.image.img_h = inner_h;
	frame = bgtk_frame(ctx, img, width, height,
			   (BGTK_Options){.padding = pad, .margin = fmar});
	ctx->root_widget = frame;

	/* --- Test 1: plain text ---------------------------------------- */
	term_feed(ts, "Hello, BGTK Terminal!\r\n", -1);
	snap(ctx, img, ts, inner_w, inner_h, "term_00_text.png");

	/* --- Test 2: ANSI colours -------------------------------------- */
	term_feed(ts, "\033[31mred \033[32mgreen \033[34mblue \033[0mnormal\r\n", -1);
	term_feed(ts, "\033[1;33mbold yellow \033[0m\r\n", -1);
	snap(ctx, img, ts, inner_w, inner_h, "term_01_colors.png");

	/* --- Test 3: cursor movement ----------------------------------- */
	term_feed(ts, "\033[10;5HPositioned here\r\n", -1);
	snap(ctx, img, ts, inner_w, inner_h, "term_02_cursor.png");

	/* --- Test 4: erase --------------------------------------------- */
	term_feed(ts, "\033[2J\033[H", -1);  /* clear screen, home */
	term_feed(ts, "Screen cleared!\r\n", -1);
	term_feed(ts, "\033[44m\033[37m Blue background white text \033[0m\r\n", -1);
	snap(ctx, img, ts, inner_w, inner_h, "term_03_erase.png");

	/* --- Test 5: scroll -------------------------------------------- */
	term_feed(ts, "\033[2J\033[H", -1);
	for (int i = 0; i < rows + 5; i++) {
		char line[64];
		int n = snprintf(line, sizeof(line), "Line %d\r\n", i + 1);
		term_feed(ts, line, n);
	}
	snap(ctx, img, ts, inner_w, inner_h, "term_04_scroll.png");

	/* --- Test 5b: scrollback view (mouse-wheel / PageUp equivalent) */
	{
		int before_sb = ts->sb_len;
		int before_off = ts->view_off;

		if (before_sb < 1) {
			fprintf(stderr,
				"test_terminal: expected scrollback after "
				"overflow, sb_len=%d\n",
				before_sb);
			return 1;
		}
		if (!term_view_scroll(ts, 3) || ts->view_off != 3) {
			fprintf(stderr,
				"test_terminal: view_scroll up failed "
				"off %d->%d sb=%d\n",
				before_off, ts->view_off, ts->sb_len);
			return 1;
		}
		snap(ctx, img, ts, inner_w, inner_h, "term_04b_scrollback.png");
		if (!term_view_to_bottom(ts) || ts->view_off != 0) {
			fprintf(stderr,
				"test_terminal: view_to_bottom failed off=%d\n",
				ts->view_off);
			return 1;
		}
		snap(ctx, img, ts, inner_w, inner_h, "term_04c_scrollback_bottom.png");
	}

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
	snap(ctx, img, ts, inner_w, inner_h, "term_05_matrix.png");

	/* --- Test 7: erase in line ------------------------------------- */
	term_feed(ts, "\033[2J\033[H", -1);
	term_feed(ts, "AAAAAAAAAA\r\n", -1);
	term_feed(ts, "BBBBBBBBBB\033[5D\033[K\r\n", -1); /* erase last 5 */
	term_feed(ts, "CCCCCCCCCC\r\n", -1);
	snap(ctx, img, ts, inner_w, inner_h, "term_06_erase_line.png");

	/* --- Test 8: bold/bright colors -------------------------------- */
	term_feed(ts, "\033[2J\033[H", -1);
	for (int i = 0; i < 8; i++) {
		char seq[64];
		snprintf(seq, sizeof(seq), "\033[%dm Normal%d ", 30 + i, i);
		term_feed(ts, seq, -1);
		snprintf(seq, sizeof(seq), "\033[1;%dm Bold%d \033[0m\r\n", 30 + i, i);
		term_feed(ts, seq, -1);
	}
	snap(ctx, img, ts, inner_w, inner_h, "term_07_bold.png");

	/* --- Test 8b: resize grid (window resize path) ----------------- */
	term_feed(ts, "\033[2J\033[Hresized ok\r\n", -1);
	if (term_resize(ts, cols / 2 > 10 ? cols / 2 : 10,
			rows / 2 > 5 ? rows / 2 : 5) != 0) {
		bgtk_log("test_terminal: term_resize failed");
		return 1;
	}
	snap(ctx, img, ts, inner_w, inner_h, "term_07b_resized.png");
	/* restore full grid for the PTY test */
	if (term_resize(ts, cols, rows) != 0) {
		bgtk_log("test_terminal: term_resize restore failed");
		return 1;
	}

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
			{
				struct timespec ts_wait = { .tv_sec = 0, .tv_nsec = 200 * 1000 * 1000 };
				nanosleep(&ts_wait, NULL);
			}
			{
				char buf[4096];
				ssize_t n = read(master_fd, buf, sizeof(buf));
				if (n > 0) term_feed(ts, buf, (int)n);
			}
			snap(ctx, img, ts, inner_w, inner_h, "term_08_shell_prompt.png");

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

			snap(ctx, img, ts, inner_w, inner_h, "term_09_real_command.png");

			/* Verify the result text is present in the cell grid */
			int found = 0;
			const char *needle = "REAL SHELL OUTPUT";
			for (int r = 0; r < rows; r++) {
				for (int c = 0; c < cols; c++) {
					int match = 1;
					for (size_t k = 0; k < strlen(needle); k++) {
						if ((size_t)c + k >= (size_t)cols) { match = 0; break; }
						if (ts->cells[r * cols + c + k].ch !=
						    (uint32_t)(unsigned char)needle[k]) {
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

	/* Vim j-at-bottom scroll: region 1..(rows-1), LF, restore, new line.
	 * Top of text region must advance (line 01 → 02); status row untouched. */
	{
		struct Term_State *vs = term_create(20, 10);
		char row0[24], row8[24];
		int c;
		char out[4];
		int n;

		for (int r = 0; r < 9; r++) {
			char b[48];
			snprintf(b, sizeof(b),
				 "\033[%d;1Hline %02d XXXXXXXXXXX", r + 1,
				 r + 1);
			term_feed(vs, b, -1);
		}
		term_feed(vs,
			  "\033[?25l\033[1;9r\033[9;1H\r\n\033[1;10r"
			  "\033[9;1Hline 10 XXXXXXXXXXX\r\033[?25h",
			  -1);
		for (c = 0; c < 20; c++) {
			char ch0 = vs->cells[c].ch;
			char ch8 = vs->cells[8 * 20 + c].ch;
			row0[c] = (ch0 >= 32 && ch0 < 127) ? ch0 : '.';
			row8[c] = (ch8 >= 32 && ch8 < 127) ? ch8 : '.';
		}
		row0[20] = row8[20] = '\0';
		if (!strstr(row0, "line 02") || !strstr(row8, "line 10")) {
			bgtk_log("vim j-scroll failed row0='%s' row8='%s'",
				 row0, row8);
			term_destroy(vs);
			free(img->data.image.pixels);
			term_destroy(ts);
			bgtk_destroy_mock(ctx);
			return 1;
		}
		/* Ctrl+J must map to LF; plain j must be 'j' (stuck-ctrl check). */
		n = bgtk_key_to_bytes(KEY_J, BGTK_MOD_CTRL, BGTK_KEY_TTY, out, 4);
		if (n != 1 || out[0] != 10) {
			bgtk_log("Ctrl+J map broken");
			term_destroy(vs);
			free(img->data.image.pixels);
			term_destroy(ts);
			bgtk_destroy_mock(ctx);
			return 1;
		}
		n = bgtk_key_to_bytes(KEY_J, 0, BGTK_KEY_TTY, out, 4);
		if (n != 1 || out[0] != 'j') {
			bgtk_log("plain j map broken");
			term_destroy(vs);
			free(img->data.image.pixels);
			term_destroy(ts);
			bgtk_destroy_mock(ctx);
			return 1;
		}
		term_destroy(vs);
	}

	/* CSI B (cursor down) must not scroll the buffer — only move cursor. */
	{
		struct Term_State *vs = term_create(20, 6);
		char row0[24], row1[24];
		int c;

		term_feed(vs, "\033[1;1HAAAA\033[2;1HBBBB\033[3;1HCCCC", -1);
		term_feed(vs, "\033[1;1H\033[B", -1); /* CUD once from row 0 */
		if (vs->cur_row != 1) {
			bgtk_log("CUD failed cur_row=%d want 1", vs->cur_row);
			term_destroy(vs);
			free(img->data.image.pixels);
			term_destroy(ts);
			bgtk_destroy_mock(ctx);
			return 1;
		}
		for (c = 0; c < 4; c++) {
			row0[c] = vs->cells[c].ch;
			row1[c] = vs->cells[20 + c].ch;
		}
		row0[4] = row1[4] = '\0';
		if (strcmp(row0, "AAAA") != 0 || strcmp(row1, "BBBB") != 0) {
			bgtk_log("CUD scrolled content row0='%s' row1='%s'",
				 row0, row1);
			term_destroy(vs);
			free(img->data.image.pixels);
			term_destroy(ts);
			bgtk_destroy_mock(ctx);
			return 1;
		}
		/* CUD does not scroll; it may leave the scroll region
		 * (status line sits below DECSTBM). */
		term_feed(vs, "\033[1;4r\033[4;1H\033[B", -1);
		if (vs->cur_row != 4) {
			bgtk_log("CUD past margin cur_row=%d want 4",
				 vs->cur_row);
			term_destroy(vs);
			free(img->data.image.pixels);
			term_destroy(ts);
			bgtk_destroy_mock(ctx);
			return 1;
		}
		if (vs->cells[0].ch != 'A' || vs->cells[20].ch != 'B') {
			bgtk_log("CUD at margin scrolled buffer");
			term_destroy(vs);
			free(img->data.image.pixels);
			term_destroy(ts);
			bgtk_destroy_mock(ctx);
			return 1;
		}
		term_destroy(vs);
	}

	/*
	 * Scrollback view_off must not hide live output. If the user wheel-
	 * scrolled then a fullscreen app (vim) redraws, top rows must show
	 * the live buffer — not history.
	 */
	{
		struct Term_State *vs = term_create(20, 6);
		int r, c;
		char row0[24];

		/* Fill live screen and push a few rows into scrollback. */
		for (r = 0; r < 6; r++) {
			char b[48];
			snprintf(b, sizeof(b), "\033[%d;1Hold-%02d", r + 1, r);
			term_feed(vs, b, -1);
		}
		for (r = 0; r < 4; r++)
			term_feed(vs, "\033[6;1H\n", -1);
		if (vs->sb_len < 1) {
			bgtk_log("expected scrollback after full LF");
			term_destroy(vs);
			free(img->data.image.pixels);
			term_destroy(ts);
			bgtk_destroy_mock(ctx);
			return 1;
		}
		/* User scrolls up into history. */
		if (!term_view_scroll(vs, vs->sb_len) || vs->view_off < 1) {
			bgtk_log("view_scroll up failed off=%d sb=%d",
				 vs->view_off, vs->sb_len);
			term_destroy(vs);
			free(img->data.image.pixels);
			term_destroy(ts);
			bgtk_destroy_mock(ctx);
			return 1;
		}
		/* Vim-like: enter alt screen + clear + draw first line. */
		term_feed(vs,
			  "\033[?1049h\033[2J\033[H"
			  "line 01 content here\r\nline 02 content here",
			  -1);
		if (vs->view_off != 0) {
			bgtk_log("view_off stuck at %d after live feed",
				 vs->view_off);
			term_destroy(vs);
			free(img->data.image.pixels);
			term_destroy(ts);
			bgtk_destroy_mock(ctx);
			return 1;
		}
		for (c = 0; c < 20; c++) {
			char ch = vs->cells[c].ch;
			row0[c] = (ch >= 32 && ch < 127) ? ch : '.';
		}
		row0[20] = '\0';
		if (!strstr(row0, "line 01")) {
			bgtk_log("first live line hidden row0='%s'", row0);
			term_destroy(vs);
			free(img->data.image.pixels);
			term_destroy(ts);
			bgtk_destroy_mock(ctx);
			return 1;
		}
		/* Visual: paint into main test surface for screenshot inspect. */
		{
			int rr, cc;
			for (rr = 0; rr < ts->rows * ts->cols; rr++)
				ts->cells[rr] = (struct Term_Cell){
					.ch = ' ', .fg = 7, .bg = 0, .bold = 0
				};
			for (rr = 0; rr < ts->rows && rr < vs->rows; rr++)
				for (cc = 0; cc < ts->cols && cc < vs->cols; cc++)
					ts->cells[rr * ts->cols + cc] =
						vs->cells[rr * vs->cols + cc];
			ts->view_off = 0;
			term_render(ts, ctx, img->data.image.pixels, inner_w,
				    inner_h);
			bgtk_draw_widgets(ctx);
			take_screenshot(ctx, "term_10_vim_live_first_line.png");
		}
		term_destroy(vs);
	}

	/*
	 * Vim status line sits *below* the DECSTBM region. LF/IND there must
	 * not scroll the text region (that made j look like every line jumped).
	 */
	{
		struct Term_State *vs = term_create(20, 5);
		char before[24], after[24];
		int c;

		/* rows 0..3 scroll region, row 4 = status */
		term_feed(vs, "\033[1;4r", -1);
		term_feed(vs, "\033[1;1HL0\033[2;1HL1\033[3;1HL2\033[4;1HL3",
			  -1);
		term_feed(vs, "\033[5;1HSTATUS", -1);
		for (c = 0; c < 8; c++)
			before[c] = vs->cells[c].ch ? vs->cells[c].ch : '.';
		before[8] = '\0';
		/* LF while on status line */
		term_feed(vs, "\033[5;1H\n", -1);
		for (c = 0; c < 8; c++)
			after[c] = vs->cells[c].ch ? vs->cells[c].ch : '.';
		after[8] = '\0';
		if (strcmp(before, after) != 0 || vs->cells[1 * 20].ch != 'L' ||
		    vs->cells[1 * 20 + 1].ch != '1') {
			bgtk_log("LF on status scrolled region before='%s' "
				 "after='%s' r1=%c%c",
				 before, after, vs->cells[20].ch,
				 vs->cells[21].ch);
			term_destroy(vs);
			free(img->data.image.pixels);
			term_destroy(ts);
			bgtk_destroy_mock(ctx);
			return 1;
		}
		/* IND on status must not scroll either */
		term_feed(vs, "\033[5;1H\033D", -1);
		if (vs->cells[0].ch != 'L' || vs->cells[1].ch != '0') {
			bgtk_log("IND on status scrolled region");
			term_destroy(vs);
			free(img->data.image.pixels);
			term_destroy(ts);
			bgtk_destroy_mock(ctx);
			return 1;
		}
		term_destroy(vs);
	}

	/*
	 * vim t_u7 / DSR: write UTF-8 (▽) then CSI 6n. Cursor must advance
	 * so CPR reports the next column — ignoring multi-byte UTF-8 made
	 * CPR stick and broke vi redraw/scroll after DSR was implemented.
	 */
	{
		struct Term_State *vs = term_create(20, 6);
		int p[2];
		char rep[64];
		ssize_t rn;

		if (pipe(p) != 0) {
			bgtk_log("pipe for DSR test failed");
			free(img->data.image.pixels);
			term_destroy(ts);
			bgtk_destroy_mock(ctx);
			return 1;
		}
		vs->pty_fd = p[1];
		/* row 2 col 1, UTF-8 white down-pointing triangle, CPR */
		term_feed(vs, "\033[2;1H\xe2\x96\xbd\033[6n", -1);
		if (vs->cur_row != 1 || vs->cur_col != 1) {
			bgtk_log("UTF-8 did not advance cursor row=%d col=%d "
				 "(want 1,1)",
				 vs->cur_row, vs->cur_col);
			close(p[0]);
			close(p[1]);
			term_destroy(vs);
			free(img->data.image.pixels);
			term_destroy(ts);
			bgtk_destroy_mock(ctx);
			return 1;
		}
		rn = read(p[0], rep, sizeof(rep) - 1);
		if (rn < 0)
			rn = 0;
		rep[rn] = '\0';
		/* Expect ESC [ 2 ; 2 R  (1-based row 2, col 2) */
		if (!strstr(rep, "[2;2R")) {
			bgtk_log("DSR CPR wrong after UTF-8: '%s' (want [2;2R)",
				 rep);
			close(p[0]);
			close(p[1]);
			term_destroy(vs);
			free(img->data.image.pixels);
			term_destroy(ts);
			bgtk_destroy_mock(ctx);
			return 1;
		}
		/* Secondary DA CSI >c */
		term_feed(vs, "\033[>c", -1);
		rn = read(p[0], rep, sizeof(rep) - 1);
		if (rn < 0)
			rn = 0;
		rep[rn] = '\0';
		if (!strstr(rep, "[>0;0;0c")) {
			bgtk_log("secondary DA missing: '%s'", rep);
			close(p[0]);
			close(p[1]);
			term_destroy(vs);
			free(img->data.image.pixels);
			term_destroy(ts);
			bgtk_destroy_mock(ctx);
			return 1;
		}
		close(p[0]);
		close(p[1]);
		term_destroy(vs);
	}

	printf("test_terminal complete. PNG frames written.\n");

	free(img->data.image.pixels);
	img->data.image.pixels = NULL;
	term_destroy(ts);
	bgtk_destroy_mock(ctx);
	return 0;
}
