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
	memmove(&t->cells[top * t->cols], &t->cells[(top + 1) * t->cols],
		(size_t)(bot - top) * t->cols * sizeof(struct Term_Cell));
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
		t->scroll_top = (p0 ? p0 : 1) - 1;
		t->scroll_bot = (p1 ? p1 : t->rows) - 1;
		if (t->scroll_top < 0) t->scroll_top = 0;
		if (t->scroll_bot >= t->rows) t->scroll_bot = t->rows - 1;
		t->cur_row = 0;
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
	if (!ctx->ft_face) return;
	FT_Face face = ctx->ft_face;
	FT_Set_Pixel_Sizes(face, 0, ctx->font_size);

	int cw = t->cell_w, ch = t->cell_h;
	if (cw < 1 || ch < 1) return;
	int asc = face->size->metrics.ascender >> 6;

	/* background */
	for (int r = 0; r < t->rows; r++) {
		for (int c = 0; c < t->cols; c++) {
			struct Term_Cell *cl = cell_at(t, c, r);
			int bi = cl->bg;
			if (bi < 0 || bi > 15) bi = 0;
			uint32_t bg = t->palette[bi];
			int x0 = c * cw, y0 = r * ch;
			for (int dy = 0; dy < ch && y0 + dy < px_h; dy++)
				for (int dx = 0; dx < cw && x0 + dx < px_w; dx++)
					pixels[(y0 + dy) * px_w + (x0 + dx)] = bg;
		}
	}
	/* fill remainder with black */
	int used_w = t->cols * cw;
	int used_h = t->rows * ch;
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

	/* glyphs */
	for (int r = 0; r < t->rows; r++) {
		for (int c = 0; c < t->cols; c++) {
			struct Term_Cell *cl = cell_at(t, c, r);
			if (cl->ch <= ' ') continue;
			int fi = cl->fg;
			if (cl->bold && fi < 8) fi += 8;
			if (fi < 0 || fi > 15) fi = 7;
			uint32_t fg = t->palette[fi];

			FT_UInt idx = FT_Get_Char_Index(face, cl->ch);
			if (FT_Load_Glyph(face, idx,
					  FT_LOAD_DEFAULT | FT_LOAD_TARGET_LIGHT))
				continue;
			FT_Render_Glyph(face->glyph, FT_RENDER_MODE_NORMAL);
			FT_Bitmap *bmp = &face->glyph->bitmap;
			int gx = c * cw + face->glyph->bitmap_left;
			int gy = r * ch + asc - face->glyph->bitmap_top;

			for (unsigned int br = 0; br < bmp->rows; br++) {
				int py = gy + (int)br;
				if (py < 0 || py >= px_h) continue;
				for (unsigned int bc = 0; bc < bmp->width; bc++) {
					int px = gx + (int)bc;
					if (px < 0 || px >= px_w) continue;
					uint8_t a = bmp->buffer[br * bmp->pitch + bc];
					if (!a) continue;
					uint8_t inv = 255 - a;
					uint32_t dst = pixels[py * px_w + px];
					uint8_t ro = (uint8_t)((((fg >> 16) & 0xFF) * a +
						     ((dst >> 16) & 0xFF) * inv) / 255);
					uint8_t go = (uint8_t)((((fg >> 8) & 0xFF) * a +
						     ((dst >> 8) & 0xFF) * inv) / 255);
					uint8_t bo = (uint8_t)((((fg) & 0xFF) * a +
						     ((dst) & 0xFF) * inv) / 255);
					pixels[py * px_w + px] =
						(ro << 16) | (go << 8) | bo;
				}
			}
		}
	}

	/* cursor (thin bar) */
	if (t->cur_row >= 0 && t->cur_row < t->rows &&
	    t->cur_col >= 0 && t->cur_col < t->cols) {
		int cx = t->cur_col * cw, cy = t->cur_row * ch;
		for (int dy = 0; dy < ch && cy + dy < px_h; dy++)
			for (int dx = 0; dx < 2 && cx + dx < px_w; dx++)
				pixels[(cy + dy) * px_w + (cx + dx)] = 0xFF00FF00;
	}
}

/* ------------------------------------------------------------------ */
/* Measure cell from font                                             */
/* ------------------------------------------------------------------ */

void term_measure_cell(struct Term_State *t, struct BGTK_Context *ctx)
{
	if (!ctx->ft_face) {
		t->cell_w = 7; t->cell_h = 14;
		return;
	}
	FT_Set_Pixel_Sizes(ctx->ft_face, 0, ctx->font_size);
	int asc = ctx->ft_face->size->metrics.ascender >> 6;
	int desc = -(ctx->ft_face->size->metrics.descender >> 6);
	t->cell_h = asc + desc;
	if (FT_Load_Char(ctx->ft_face, 'M', FT_LOAD_DEFAULT) == 0)
		t->cell_w = ctx->ft_face->glyph->advance.x >> 6;
	else
		t->cell_w = t->cell_h / 2;
	if (t->cell_w < 1) t->cell_w = 1;
	if (t->cell_h < 1) t->cell_h = 1;
}

/* ------------------------------------------------------------------ */
/* Keycode translation                                                */
/* ------------------------------------------------------------------ */

int term_keycode_to_bytes(int code, int shift, int ctrl, char *out, int max)
{
	if (max < 4) return 0;

	if (ctrl) {
		static const int kmap[] = {
			KEY_Q, KEY_W, KEY_E, KEY_R, KEY_T, KEY_Y, KEY_U,
			KEY_I, KEY_O, KEY_P, KEY_A, KEY_S, KEY_D, KEY_F,
			KEY_G, KEY_H, KEY_J, KEY_K, KEY_L, KEY_Z, KEY_X,
			KEY_C, KEY_V, KEY_B, KEY_N, KEY_M, 0
		};
		static const char cmap[] = "qwertyuiopasdfghjklzxcvbnm";
		for (int i = 0; kmap[i]; i++) {
			if (kmap[i] == code) {
				out[0] = cmap[i] - 'a' + 1;
				return 1;
			}
		}
	}
	{
		static const int kmap[] = {
			KEY_Q, KEY_W, KEY_E, KEY_R, KEY_T, KEY_Y, KEY_U,
			KEY_I, KEY_O, KEY_P, KEY_A, KEY_S, KEY_D, KEY_F,
			KEY_G, KEY_H, KEY_J, KEY_K, KEY_L, KEY_Z, KEY_X,
			KEY_C, KEY_V, KEY_B, KEY_N, KEY_M, 0
		};
		static const char map[] = "qwertyuiopasdfghjklzxcvbnm";
		for (int i = 0; kmap[i]; i++) {
			if (kmap[i] == code) {
				out[0] = shift ? (map[i] - 32) : map[i];
				return 1;
			}
		}
	}
	if (code >= KEY_1 && code <= KEY_0) {
		static const char norm[] = "1234567890";
		static const char shft[] = "!@#$%^&*()";
		int idx = code - KEY_1;
		if (idx >= 0 && idx < 10) {
			out[0] = shift ? shft[idx] : norm[idx];
			return 1;
		}
	}
	switch (code) {
	case KEY_SPACE:      out[0] = ' '; return 1;
	case KEY_ENTER:
	case KEY_KPENTER:    out[0] = '\r'; return 1;
	case KEY_TAB:        out[0] = '\t'; return 1;
	case KEY_BACKSPACE:  out[0] = 0x7F; return 1;
	case KEY_ESC:        out[0] = 0x1B; return 1;
	case KEY_MINUS:      out[0] = shift ? '_' : '-'; return 1;
	case KEY_EQUAL:      out[0] = shift ? '+' : '='; return 1;
	case KEY_LEFTBRACE:  out[0] = shift ? '{' : '['; return 1;
	case KEY_RIGHTBRACE: out[0] = shift ? '}' : ']'; return 1;
	case KEY_BACKSLASH:  out[0] = shift ? '|' : '\\'; return 1;
	case KEY_SEMICOLON:  out[0] = shift ? ':' : ';'; return 1;
	case KEY_APOSTROPHE: out[0] = shift ? '"' : '\''; return 1;
	case KEY_GRAVE:      out[0] = shift ? '~' : '`'; return 1;
	case KEY_COMMA:      out[0] = shift ? '<' : ','; return 1;
	case KEY_DOT:        out[0] = shift ? '>' : '.'; return 1;
	case KEY_SLASH:      out[0] = shift ? '?' : '/'; return 1;
	case KEY_UP:    memcpy(out, "\033[A", 3); return 3;
	case KEY_DOWN:  memcpy(out, "\033[B", 3); return 3;
	case KEY_RIGHT: memcpy(out, "\033[C", 3); return 3;
	case KEY_LEFT:  memcpy(out, "\033[D", 3); return 3;
	case KEY_HOME:  memcpy(out, "\033[H", 3); return 3;
	case KEY_END:   memcpy(out, "\033[F", 3); return 3;
	case KEY_DELETE:   memcpy(out, "\033[3~", 4); return 4;
	case KEY_PAGEUP:   memcpy(out, "\033[5~", 4); return 4;
	case KEY_PAGEDOWN: memcpy(out, "\033[6~", 4); return 4;
	default: break;
	}
	return 0;
}