/* apps/terminal.h
 *
 * Public interface for the terminal state machine.
 * Shared between apps/terminal.c (real app) and test/test_terminal.c (headless).
 */

#ifndef TERMINAL_H
#define TERMINAL_H

#include <stdint.h>

struct BGTK_Context;

/* A single character cell */
struct Term_Cell {
	char ch;
	int8_t fg;   /* palette index 0-15 */
	int8_t bg;   /* palette index 0-15 */
	int8_t bold;
};

/* Full terminal state */
struct Term_State {
	int cols, rows;
	struct Term_Cell *cells;

	/* Cursor */
	int cur_row, cur_col;

	/* Current SGR attributes */
	int cur_fg, cur_bg, cur_bold;

	/* Scroll region */
	int scroll_top, scroll_bot;

	/* Escape sequence parser state */
	int esc_state;
	int csi_params[8];
	int csi_nparam;
	int csi_priv;

	/* PTY master fd (for DSR replies; -1 if headless) */
	int pty_fd;

	/* Cell pixel dimensions (set by term_measure_cell) */
	int cell_w, cell_h;

	/* 16-colour palette */
	uint32_t palette[16];
};

/* Create / destroy terminal state */
struct Term_State *term_create(int cols, int rows);
void term_destroy(struct Term_State *t);
/* Resize the cell grid (preserves overlapping content). Returns 0 on success. */
int term_resize(struct Term_State *t, int cols, int rows);

/* Feed raw bytes (from PTY or test harness) through the ANSI parser */
void term_feed(struct Term_State *t, const char *data, int len);

/* Render the cell grid into a pixel buffer */
void term_render(struct Term_State *t, struct BGTK_Context *ctx,
		 uint32_t *pixels, int px_w, int px_h);

/* Compute cell_w / cell_h from the context's font */
void term_measure_cell(struct Term_State *t, struct BGTK_Context *ctx);

/* Translate a linux keycode to bytes suitable for writing to a PTY.
 * Returns number of bytes written to out (0 if unknown key). */
int term_keycode_to_bytes(int code, int shift, int ctrl,
			  char *out, int max);

#endif /* TERMINAL_H */