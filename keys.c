/* keys.c — shared keycode → bytes (US QWERTY), TEXT vs TTY modes */
#include "bgtk.h"

#include <linux/input.h>
#include <string.h>

void bgtk_update_modifiers(struct BGTK_Context *ctx, struct InputEvent ev)
{
	int held;

	if (!ctx || ev.type != EV_KEY)
		return;
	held = (ev.value != 0);
	if (ev.code == KEY_LEFTSHIFT || ev.code == KEY_RIGHTSHIFT)
		ctx->shift_held = held;
	else if (ev.code == KEY_LEFTCTRL || ev.code == KEY_RIGHTCTRL)
		ctx->ctrl_held = held;
	else if (ev.code == KEY_LEFTALT || ev.code == KEY_RIGHTALT)
		ctx->alt_held = held;
}

int bgtk_mods_from_ctx(const struct BGTK_Context *ctx)
{
	int m = 0;

	if (!ctx)
		return 0;
	if (ctx->shift_held)
		m |= BGTK_MOD_SHIFT;
	if (ctx->ctrl_held)
		m |= BGTK_MOD_CTRL;
	if (ctx->alt_held)
		m |= BGTK_MOD_ALT;
	return m;
}

int bgtk_is_app_quit_event(const struct BGTK_Context *ctx, struct InputEvent ev)
{
	if (ev.type != EV_KEY || (ev.value != 1 && ev.value != 2))
		return 0;
	if (ev.code == KEY_ESC)
		return 1;
	/* Ctrl+C: quit normal apps (terminal must not call this). */
	if (ev.code == KEY_C && ctx && ctx->ctrl_held)
		return 1;
	return 0;
}

static int letter_index(int code)
{
	static const int kmap[] = {
		KEY_Q, KEY_W, KEY_E, KEY_R, KEY_T, KEY_Y, KEY_U, KEY_I, KEY_O,
		KEY_P, KEY_A, KEY_S, KEY_D, KEY_F, KEY_G, KEY_H, KEY_J, KEY_K,
		KEY_L, KEY_Z, KEY_X, KEY_C, KEY_V, KEY_B, KEY_N, KEY_M, 0
	};
	for (int i = 0; kmap[i]; i++)
		if (kmap[i] == code)
			return i;
	return -1;
}

int bgtk_key_to_bytes(int code, int mods, int mode, char *out, int max)
{
	int shift = mods & BGTK_MOD_SHIFT;
	int ctrl = mods & BGTK_MOD_CTRL;
	int li;

	if (!out || max < 1)
		return 0;

	/* Ctrl+letter: TTY → C0; TEXT → no printable (caller handles chords). */
	li = letter_index(code);
	if (ctrl && li >= 0) {
		if (mode == BGTK_KEY_TTY) {
			static const char cmap[] = "qwertyuiopasdfghjklzxcvbnm";
			if (max < 1)
				return 0;
			out[0] = (char)(cmap[li] - 'a' + 1);
			return 1;
		}
		return 0;
	}

	if (li >= 0) {
		static const char map[] = "qwertyuiopasdfghjklzxcvbnm";
		out[0] = shift ? (char)(map[li] - 32) : map[li];
		return 1;
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
	case KEY_SPACE:
		out[0] = ' ';
		return 1;
	case KEY_ENTER:
	case KEY_KPENTER:
		if (mode == BGTK_KEY_TTY) {
			out[0] = '\r';
			return 1;
		}
		return 0; /* text field handles enter via callback */
	case KEY_TAB:
		if (mode == BGTK_KEY_TTY) {
			out[0] = '\t';
			return 1;
		}
		return 0;
	case KEY_BACKSPACE:
		if (mode == BGTK_KEY_TTY) {
			out[0] = 0x7F;
			return 1;
		}
		return 0;
	case KEY_ESC:
		out[0] = 0x1B;
		return 1;
	case KEY_MINUS:
		out[0] = shift ? '_' : '-';
		return 1;
	case KEY_EQUAL:
		out[0] = shift ? '+' : '=';
		return 1;
	case KEY_LEFTBRACE:
		out[0] = shift ? '{' : '[';
		return 1;
	case KEY_RIGHTBRACE:
		out[0] = shift ? '}' : ']';
		return 1;
	case KEY_BACKSLASH:
		out[0] = shift ? '|' : '\\';
		return 1;
	case KEY_SEMICOLON:
		out[0] = shift ? ':' : ';';
		return 1;
	case KEY_APOSTROPHE:
		out[0] = shift ? '"' : '\'';
		return 1;
	case KEY_GRAVE:
		out[0] = shift ? '~' : '`';
		return 1;
	case KEY_COMMA:
		out[0] = shift ? '<' : ',';
		return 1;
	case KEY_DOT:
		out[0] = shift ? '>' : '.';
		return 1;
	case KEY_SLASH:
		out[0] = shift ? '?' : '/';
		return 1;
	default:
		break;
	}

	if (mode != BGTK_KEY_TTY || max < 4)
		return 0;

	switch (code) {
	case KEY_UP:
		memcpy(out, "\033[A", 3);
		return 3;
	case KEY_DOWN:
		memcpy(out, "\033[B", 3);
		return 3;
	case KEY_RIGHT:
		memcpy(out, "\033[C", 3);
		return 3;
	case KEY_LEFT:
		memcpy(out, "\033[D", 3);
		return 3;
	case KEY_HOME:
		memcpy(out, "\033[H", 3);
		return 3;
	case KEY_END:
		memcpy(out, "\033[F", 3);
		return 3;
	case KEY_DELETE:
		memcpy(out, "\033[3~", 4);
		return 4;
	case KEY_PAGEUP:
		memcpy(out, "\033[5~", 4);
		return 4;
	case KEY_PAGEDOWN:
		memcpy(out, "\033[6~", 4);
		return 4;
	default:
		break;
	}
	return 0;
}
