/* apps/term_core.c
 *
 * Terminal emulator core: cell buffer, ANSI parser, renderer, keymap.
 * No main() – linked by both apps/terminal.c and test/test_terminal.c.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <bgtk.h>
#include <linux/input.h>

#include "internal.h"
#include "terminal.h"

/* ------------------------------------------------------------------ */
/* Default 16-colour palette (matches xterm)                          */
/* ------------------------------------------------------------------ */
/* Palette stored in framebuffer byte order: 0xAABBGGRR on LE.
 * The BGTK framebuffer is uint32 with (r<<16)|(g<<8)|b in code, but
 * stbi_write_png reads bytes as R,G,B,A so R and B appear swapped in
 * the PNG. We pre-swap here so PNG screenshots match the intended
 * colour AND the real display (which also reads memory in byte order).
 */
static const uint32_t default_palette[16] = {
	0xFF000000, 0xFF0000CD, 0xFF00CD00, 0xFF00CDCD,
	0xFFEE0000, 0xFFCD00CD, 0xFFCDCD00, 0xFFE5E5E5,
	0xFF7F7F7F, 0xFF0000FF, 0xFF00FF00, 0xFF00FFFF,
	0xFFFF5C5C, 0xFFFF00FF, 0xFFFFFF00, 0xFFFFFFFF,
};

static struct Term_Cell default_cell(void)
{
	return (struct Term_Cell){ .ch = ' ', .fg = 7, .bg = 0, .bold = 0 };
}

/* ------------------------------------------------------------------ */
/* Create / destroy                                                   */
/* ------------------------------------------------------------------ */

struct Term_State *term_create(int cols, int rows)
{
	struct Term_State *t = calloc(1, sizeof(*t));
	if (!t) return NULL;
	t->cols = cols;
	t->rows = rows;
	t->cells = calloc((size_t)cols * rows, sizeof(struct Term_Cell));
	if (!t->cells) { free(t); return NULL; }
	for (int i = 0; i < cols * rows; i++)
		t->cells[i] = default_cell();
	t->cur_fg = 7;
	t->scroll_top = 0;
	t->scroll_bot = rows - 1;
	t->pty_fd = -1;
	memcpy(t->palette, default_palette, sizeof(default_palette));
	return t;
}

void term_destroy(struct Term_State *t)
{
	if (!t) return;
	free(t->cells);
	free(t);
}

int term_resize(struct Term_State *t, int cols, int rows)
{
	if (!t)
		return -1;
	if (cols < 1)
		cols = 1;
	if (rows < 1)
		rows = 1;
	if (cols == t->cols && rows == t->rows)
		return 0;

	struct Term_Cell *nc = calloc((size_t)cols * (size_t)rows, sizeof(*nc));
	if (!nc)
		return -1;

	int copy_c = cols < t->cols ? cols : t->cols;
	int copy_r = rows < t->rows ? rows : t->rows;
	for (int r = 0; r < rows; r++) {
		for (int c = 0; c < cols; c++) {
			if (r < copy_r && c < copy_c)
				nc[r * cols + c] = t->cells[r * t->cols + c];
			else
				nc[r * cols + c] = default_cell();
		}
	}
	free(t->cells);
	t->cells = nc;
	t->cols = cols;
	t->rows = rows;
	if (t->cur_col >= cols)
		t->cur_col = cols - 1;
	if (t->cur_row >= rows)
		t->cur_row = rows - 1;
	t->scroll_top = 0;
	t->scroll_bot = rows - 1;
	return 0;
}

/* ------------------------------------------------------------------ */
/* Internal helpers                                                   */
/* ------------------------------------------------------------------ */

static struct Term_Cell *cell_at(struct Term_State *t, int col, int row)
{
	return &t->cells[row * t->cols + col];
}

static void scroll_up(struct Term_State *t)
{
	int top = t->scroll_top, bot = t->scroll_bot;
	size_t row_bytes;

	if (top < 0)
		top = 0;
	if (bot >= t->rows)
		bot = t->rows - 1;
	if (top >= bot)
		return;
	/* Move rows top+1..bot → top..bot-1; clear bot. Overlap-safe. */
	row_bytes = (size_t)(bot - top) * (size_t)t->cols * sizeof(struct Term_Cell);
	memmove(&t->cells[top * t->cols], &t->cells[(top + 1) * t->cols],
		row_bytes);
	for (int c = 0; c < t->cols; c++)
		t->cells[bot * t->cols + c] = default_cell();
}

static void scroll_down(struct Term_State *t)
{
	int top = t->scroll_top, bot = t->scroll_bot;
	memmove(&t->cells[(top + 1) * t->cols], &t->cells[top * t->cols],
		(size_t)(bot - top) * t->cols * sizeof(struct Term_Cell));
	for (int c = 0; c < t->cols; c++)
		t->cells[top * t->cols + c] = default_cell();
}

/* ------------------------------------------------------------------ */
/* CSI dispatch                                                       */
/* ------------------------------------------------------------------ */
#define MAX_CSI_PARAMS 8

static void csi_dispatch(struct Term_State *t, char final,
			 int *params, int nparam)
{
	int p0 = nparam > 0 ? params[0] : 0;
	int p1 = nparam > 1 ? params[1] : 0;

	switch (final) {
	case 'A':
		t->cur_row -= p0 ? p0 : 1;
		if (t->cur_row < 0) t->cur_row = 0;
		break;
	case 'B':
		t->cur_row += p0 ? p0 : 1;
		if (t->cur_row >= t->rows) t->cur_row = t->rows - 1;
		break;
	case 'C':
		t->cur_col += p0 ? p0 : 1;
		if (t->cur_col >= t->cols) t->cur_col = t->cols - 1;
		break;
	case 'D':
		t->cur_col -= p0 ? p0 : 1;
		if (t->cur_col < 0) t->cur_col = 0;
		break;
	case 'H':
	case 'f':
		t->cur_row = (p0 ? p0 : 1) - 1;
		t->cur_col = (p1 ? p1 : 1) - 1;
		if (t->cur_row >= t->rows) t->cur_row = t->rows - 1;
		if (t->cur_col >= t->cols) t->cur_col = t->cols - 1;
		if (t->cur_row < 0) t->cur_row = 0;
		if (t->cur_col < 0) t->cur_col = 0;
		break;
	case 'J':
		if (p0 == 0) {
			for (int i = t->cur_row * t->cols + t->cur_col;
			     i < t->rows * t->cols; i++)
				t->cells[i] = default_cell();
		} else if (p0 == 1) {
			for (int i = 0;
			     i <= t->cur_row * t->cols + t->cur_col; i++)
				t->cells[i] = default_cell();
		} else if (p0 == 2) {
			for (int i = 0; i < t->rows * t->cols; i++)
				t->cells[i] = default_cell();
		}
		break;
	case 'K':
		if (p0 == 0) {
			for (int c = t->cur_col; c < t->cols; c++)
				*cell_at(t, c, t->cur_row) = default_cell();
		} else if (p0 == 1) {
			for (int c = 0; c <= t->cur_col; c++)
				*cell_at(t, c, t->cur_row) = default_cell();
		} else if (p0 == 2) {
			for (int c = 0; c < t->cols; c++)
				*cell_at(t, c, t->cur_row) = default_cell();
		}
		break;
	case 'S':
		for (int i = 0; i < (p0 ? p0 : 1); i++) scroll_up(t);
		break;
	case 'T':
		for (int i = 0; i < (p0 ? p0 : 1); i++) scroll_down(t);
		break;
	case 'L': {
		int n = p0 ? p0 : 1;
		for (int i = 0; i < n && t->cur_row <= t->scroll_bot; i++) {
			memmove(&t->cells[(t->cur_row + 1) * t->cols],
				&t->cells[t->cur_row * t->cols],
				(size_t)(t->scroll_bot - t->cur_row) *
				t->cols * sizeof(struct Term_Cell));
			for (int c = 0; c < t->cols; c++)
				*cell_at(t, c, t->cur_row) = default_cell();
		}
		break;
	}
	case 'M': {
		int n = p0 ? p0 : 1;
		for (int i = 0; i < n && t->cur_row <= t->scroll_bot; i++) {
			memmove(&t->cells[t->cur_row * t->cols],
				&t->cells[(t->cur_row + 1) * t->cols],
				(size_t)(t->scroll_bot - t->cur_row) *
				t->cols * sizeof(struct Term_Cell));
			for (int c = 0; c < t->cols; c++)
				*cell_at(t, c, t->scroll_bot) = default_cell();
		}
		break;
	}
	case 'P': {
		int n = p0 ? p0 : 1;
		int remain = t->cols - t->cur_col - n;
		if (remain > 0)
			memmove(cell_at(t, t->cur_col, t->cur_row),
				cell_at(t, t->cur_col + n, t->cur_row),
				(size_t)remain * sizeof(struct Term_Cell));
		for (int c = t->cols - n; c < t->cols; c++)
			if (c >= 0)
				*cell_at(t, c, t->cur_row) = default_cell();
		break;
	}
	case '@': {
		int n = p0 ? p0 : 1;
		int remain = t->cols - t->cur_col - n;
		if (remain > 0)
			memmove(cell_at(t, t->cur_col + n, t->cur_row),
				cell_at(t, t->cur_col, t->cur_row),
				(size_t)remain * sizeof(struct Term_Cell));
		for (int i = 0; i < n && t->cur_col + i < t->cols; i++)
			*cell_at(t, t->cur_col + i, t->cur_row) = default_cell();
		break;
	}
	case 'd':
		t->cur_row = (p0 ? p0 : 1) - 1;
		if (t->cur_row >= t->rows) t->cur_row = t->rows - 1;
		if (t->cur_row < 0) t->cur_row = 0;
		break;
	case 'G':
		t->cur_col = (p0 ? p0 : 1) - 1;
		if (t->cur_col >= t->cols) t->cur_col = t->cols - 1;
		if (t->cur_col < 0) t->cur_col = 0;
		break;
	case 'r':
		/* DECSTBM — vim uses e.g. ESC[1;9r then LF to scroll the
		 * text region while the status line stays put. */
		t->scroll_top = (p0 ? p0 : 1) - 1;
		t->scroll_bot = (p1 ? p1 : t->rows) - 1;
		if (t->scroll_top < 0)
			t->scroll_top = 0;
		if (t->scroll_bot >= t->rows)
			t->scroll_bot = t->rows - 1;
		if (t->scroll_top > t->scroll_bot) {
			/* Invalid region — reset to full screen. */
			t->scroll_top = 0;
			t->scroll_bot = t->rows - 1;
		}
		t->cur_row = t->scroll_top;
		t->cur_col = 0;
		break;
	case 'm':
		if (nparam == 0) {
			t->cur_fg = 7; t->cur_bg = 0; t->cur_bold = 0;
			break;
		}
		for (int i = 0; i < nparam; i++) {
			int v = params[i];
			if (v == 0) { t->cur_fg = 7; t->cur_bg = 0; t->cur_bold = 0; }
			else if (v == 1) t->cur_bold = 1;
			else if (v == 22) t->cur_bold = 0;
			else if (v >= 30 && v <= 37) t->cur_fg = v - 30;
			else if (v == 39) t->cur_fg = 7;
			else if (v >= 40 && v <= 47) t->cur_bg = v - 40;
			else if (v == 49) t->cur_bg = 0;
			else if (v >= 90 && v <= 97) t->cur_fg = v - 90 + 8;
			else if (v >= 100 && v <= 107) t->cur_bg = v - 100 + 8;
			else if (v == 7) { int tmp = t->cur_fg; t->cur_fg = t->cur_bg; t->cur_bg = tmp; }
		}
		break;
	case 'n':
		if (p0 == 6 && t->pty_fd >= 0) {
			char buf[32];
			int n = snprintf(buf, sizeof(buf),
					 t->csi_priv ? "\033[?%d;%dR"
						     : "\033[%d;%dR",
					 t->cur_row + 1, t->cur_col + 1);
			(void)write(t->pty_fd, buf, n);
		}
		break;
	case 'c':
		if (t->pty_fd >= 0) {
			const char *resp = t->csi_priv
				? "\033[>0;0;0c"   /* Secondary DA */
				: "\033[?1;2c";    /* Primary DA */
			(void)write(t->pty_fd, resp, strlen(resp));
		}
		break;
	case 'h':
	case 'l':
		break;
	default:
		break;
	}
}

/* ------------------------------------------------------------------ */
/* Feed bytes through the parser                                      */
/* ------------------------------------------------------------------ */

void term_feed(struct Term_State *t, const char *data, int len)
{
	if (len < 0) len = (int)strlen(data);
	for (int i = 0; i < len; i++) {
		unsigned char ch = (unsigned char)data[i];

		switch (t->esc_state) {
		case 0:
			if (ch == '\033') {
				t->esc_state = 1;
			} else if (ch == '\n') {
				t->cur_row++;
				if (t->cur_row > t->scroll_bot) {
					t->cur_row = t->scroll_bot;
					scroll_up(t);
				}
			} else if (ch == '\r') {
				t->cur_col = 0;
			} else if (ch == '\t') {
				t->cur_col = (t->cur_col + 8) & ~7;
				if (t->cur_col >= t->cols)
					t->cur_col = t->cols - 1;
			} else if (ch == '\b') {
				if (t->cur_col > 0) t->cur_col--;
			} else if (ch == '\a') {
				/* bell */
			} else if (ch >= 0x20 && ch < 0x7F) {
				struct Term_Cell *c = cell_at(t, t->cur_col, t->cur_row);
				c->ch = ch;
				c->fg = t->cur_fg;
				c->bg = t->cur_bg;
				c->bold = t->cur_bold;
				t->cur_col++;
				if (t->cur_col >= t->cols) {
					t->cur_col = 0;
					t->cur_row++;
					if (t->cur_row > t->scroll_bot) {
						t->cur_row = t->scroll_bot;
						scroll_up(t);
					}
				}
			}
			break;
		case 1:
			if (ch == '[') {
				t->esc_state = 2;
				t->csi_nparam = 0;
				memset(t->csi_params, 0, sizeof(t->csi_params));
				t->csi_priv = 0;
			} else if (ch == ']') {
				t->esc_state = 4;
			} else if (ch == '(' || ch == ')') {
				t->esc_state = 5;
			} else if (ch == 'D') {
				t->cur_row++;
				if (t->cur_row > t->scroll_bot) {
					t->cur_row = t->scroll_bot;
					scroll_up(t);
				}
				t->esc_state = 0;
			} else if (ch == 'M') {
				t->cur_row--;
				if (t->cur_row < t->scroll_top) {
					t->cur_row = t->scroll_top;
					scroll_down(t);
				}
				t->esc_state = 0;
			} else if (ch == 'c') {
				for (int j = 0; j < t->rows * t->cols; j++)
					t->cells[j] = default_cell();
				t->cur_row = t->cur_col = 0;
				t->cur_fg = 7; t->cur_bg = 0; t->cur_bold = 0;
				t->scroll_top = 0; t->scroll_bot = t->rows - 1;
				t->esc_state = 0;
			} else {
				t->esc_state = 0;
			}
			break;
		case 2:
			if (ch == '?') {
				t->csi_priv = 1;
			} else if (ch >= '0' && ch <= '9') {
				if (t->csi_nparam == 0) t->csi_nparam = 1;
				t->csi_params[t->csi_nparam - 1] =
					t->csi_params[t->csi_nparam - 1] * 10 + (ch - '0');
			} else if (ch == ';') {
				if (t->csi_nparam < MAX_CSI_PARAMS)
					t->csi_nparam++;
			} else if (ch >= 0x40 && ch <= 0x7E) {
				csi_dispatch(t, ch, t->csi_params, t->csi_nparam);
				t->esc_state = 0;
			} else {
				t->esc_state = 0;
			}
			break;
		case 4:
			if (ch == '\a') t->esc_state = 0;
			else if (ch == '\033') t->esc_state = 3;
			break;
		case 3:
			t->esc_state = (ch == '\\') ? 0 : 4;
			break;
		case 5:
			t->esc_state = 0;
			break;
		}
	}
}

/* ------------------------------------------------------------------ */
/* Render                                                             */
/* ------------------------------------------------------------------ */

void term_render(struct Term_State *t, struct BGTK_Context *ctx,
		 uint32_t *pixels, int px_w, int px_h)
{
	/* Always draw with the mono face (falls back to sans only if mono
	 * failed to load in bgtk_init). */
	FT_Face face = bgtk_font_face(ctx, BGTK_FONT_MONO);
	int cw, ch, asc;
	int used_w, used_h;

	if (!face || !pixels)
		return;
	FT_Set_Pixel_Sizes(face, 0, ctx->font_size > 0 ? ctx->font_size : 14);

	cw = t->cell_w;
	ch = t->cell_h;
	if (cw < 1 || ch < 1)
		return;
	asc = face->size->metrics.ascender >> 6;

	/* background (full cell rects — also clears any prior glyph bleed) */
	for (int r = 0; r < t->rows; r++) {
		for (int c = 0; c < t->cols; c++) {
			struct Term_Cell *cl = cell_at(t, c, r);
			int bi = cl->bg;
			uint32_t bg;
			int x0, y0;

			if (bi < 0 || bi > 15)
				bi = 0;
			bg = t->palette[bi];
			x0 = c * cw;
			y0 = r * ch;
			for (int dy = 0; dy < ch && y0 + dy < px_h; dy++)
				for (int dx = 0; dx < cw && x0 + dx < px_w; dx++)
					pixels[(y0 + dy) * px_w + (x0 + dx)] = bg;
		}
	}
	/* fill remainder with black */
	used_w = t->cols * cw;
	used_h = t->rows * ch;
	if (used_w < px_w) {
		for (int y = 0; y < px_h; y++)
			for (int x = used_w; x < px_w; x++)
				pixels[y * px_w + x] = t->palette[0];
	}
	if (used_h < px_h) {
		for (int y = used_h; y < px_h; y++)
			for (int x = 0; x < px_w; x++)
				pixels[y * px_w + x] = t->palette[0];
	}

	/* glyphs — clip strictly to the cell so wide/offset glyphs cannot
	 * overwrite neighbours (looks like "mumbled" text under vi j/k). */
	for (int r = 0; r < t->rows; r++) {
		for (int c = 0; c < t->cols; c++) {
			struct Term_Cell *cl = cell_at(t, c, r);
			int fi;
			uint32_t fg;
			FT_UInt gidx;
			FT_Bitmap *bmp;
			int gx, gy;
			int cell_x0, cell_y0, cell_x1, cell_y1;
			unsigned char uch;

			uch = (unsigned char)cl->ch;
			if (uch <= (unsigned char)' ')
				continue;
			fi = cl->fg;
			if (cl->bold && fi < 8)
				fi += 8;
			if (fi < 0 || fi > 15)
				fi = 7;
			fg = t->palette[fi];

			gidx = FT_Get_Char_Index(face, (FT_ULong)uch);
			if (FT_Load_Glyph(face, gidx,
					  FT_LOAD_DEFAULT | FT_LOAD_TARGET_LIGHT))
				continue;
			if (FT_Render_Glyph(face->glyph, FT_RENDER_MODE_NORMAL))
				continue;
			bmp = &face->glyph->bitmap;
			gx = c * cw + face->glyph->bitmap_left;
			gy = r * ch + asc - face->glyph->bitmap_top;
			cell_x0 = c * cw;
			cell_y0 = r * ch;
			cell_x1 = cell_x0 + cw;
			cell_y1 = cell_y0 + ch;

			for (unsigned int br = 0; br < bmp->rows; br++) {
				int py = gy + (int)br;
				if (py < cell_y0 || py >= cell_y1 ||
				    py < 0 || py >= px_h)
					continue;
				for (unsigned int bc = 0; bc < bmp->width;
				     bc++) {
					int px = gx + (int)bc;
					uint8_t a, inv;
					uint32_t dst;
					uint8_t ro, go, bo;

					if (px < cell_x0 || px >= cell_x1 ||
					    px < 0 || px >= px_w)
						continue;
					a = bmp->buffer[br * (unsigned)bmp->pitch +
							bc];
					if (!a)
						continue;
					inv = (uint8_t)(255 - a);
					dst = pixels[py * px_w + px];
					ro = (uint8_t)((((fg >> 16) & 0xFF) * a +
							((dst >> 16) & 0xFF) *
								inv) /
						       255);
					go = (uint8_t)((((fg >> 8) & 0xFF) * a +
							((dst >> 8) & 0xFF) *
								inv) /
						       255);
					bo = (uint8_t)((((fg) & 0xFF) * a +
							((dst) & 0xFF) * inv) /
						       255);
					pixels[py * px_w + px] =
						(ro << 16) | (go << 8) | bo |
						0xFF000000;
				}
			}
		}
	}

	/* cursor (thin bar, clipped to cell) */
	if (t->cur_row >= 0 && t->cur_row < t->rows &&
	    t->cur_col >= 0 && t->cur_col < t->cols) {
		int cx = t->cur_col * cw, cy = t->cur_row * ch;
		int bar = cw > 2 ? 2 : 1;
		for (int dy = 0; dy < ch && cy + dy < px_h; dy++)
			for (int dx = 0; dx < bar && cx + dx < px_w; dx++)
				pixels[(cy + dy) * px_w + (cx + dx)] =
					0xFF00FF00;
	}
}

/* ------------------------------------------------------------------ */
/* Measure cell from font                                             */
/* ------------------------------------------------------------------ */

void term_measure_cell(struct Term_State *t, struct BGTK_Context *ctx)
{
	FT_Face face = bgtk_font_face(ctx, BGTK_FONT_MONO);
	int asc, desc;
	int max_adv = 0;
	int max_extent = 0;
	int c;
	int fixed;

	if (!face) {
		t->cell_w = 8;
		t->cell_h = 14;
		bgtk_log("term_measure_cell: no mono face; using 8x14 cells");
		return;
	}
	FT_Set_Pixel_Sizes(face, 0, ctx->font_size > 0 ? ctx->font_size : 14);
	fixed = FT_IS_FIXED_WIDTH(face) ? 1 : 0;
	asc = face->size->metrics.ascender >> 6;
	desc = -(face->size->metrics.descender >> 6);
	t->cell_h = asc + desc;
	if (t->cell_h < 8)
		t->cell_h = 8;

	/* True mono: use max_advance if set; else measure printable ASCII so a
	 * proportional fallback cannot size cells from a skinny glyph. */
	if (fixed && face->size->metrics.max_advance > 0)
		max_adv = (int)(face->size->metrics.max_advance >> 6);

	for (c = 32; c < 127; c++) {
		int adv, extent;

		if (FT_Load_Char(face, (FT_ULong)c,
				 FT_LOAD_DEFAULT | FT_LOAD_RENDER) != 0)
			continue;
		adv = (int)(face->glyph->advance.x >> 6);
		extent = face->glyph->bitmap_left +
			 (int)face->glyph->bitmap.width;
		if (adv > max_adv)
			max_adv = adv;
		if (extent > max_extent)
			max_extent = extent;
	}
	t->cell_w = max_adv > max_extent ? max_adv : max_extent;
	if (t->cell_w < 6)
		t->cell_w = 6;
	bgtk_log("term_measure_cell: %dx%d fixed=%d family='%s' mono_path='%s'",
		 t->cell_w, t->cell_h, fixed,
		 face->family_name ? face->family_name : "?",
		 ctx->font_mono_path[0] ? ctx->font_mono_path : "(none)");
}

/* ------------------------------------------------------------------ */
/* Keycode translation                                                */
/* ------------------------------------------------------------------ */

int term_keycode_to_bytes(int code, int shift, int ctrl, char *out, int max)
{
	int mods = 0;
	if (shift)
		mods |= BGTK_MOD_SHIFT;
	if (ctrl)
		mods |= BGTK_MOD_CTRL;
	return bgtk_key_to_bytes(code, mods, BGTK_KEY_TTY, out, max);
}