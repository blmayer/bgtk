/* apps/settings.c
 *
 * BGCE + BGTK System Settings (widget UI, no HTML).
 * Sidebar categories + content panel for background, cursor, shortcuts,
 * font, and theme.
 */

#include <dirent.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>
#include <linux/input.h>

#include "bgtk.h"
#include "config.h"
#include "internal.h"

/* ------------------------------------------------------------------ */
/* Global state                                                        */
/* ------------------------------------------------------------------ */

static struct BGTK_Context *ctx;
static struct config cfg;

/* Wide enough for dual theme columns (colors | sizes); tall enough for ~10 rows. */
#define SETTINGS_W 920
#define SETTINGS_H 640

static int app_w = SETTINGS_W;
static int app_h = SETTINGS_H;
static int screen_aspect_w = 16;
static int screen_aspect_h = 9;

static struct BGTK_Widget *sidebar_list;
static struct BGTK_Widget *sidebar;
static struct BGTK_Widget *panel_rule;
static struct BGTK_Widget *content_panel;
static struct BGTK_Widget *shell_row; /* [sidebar | rule | content] */
static struct BGTK_Widget *root_frame;

static int current_page = 0;
#define NUM_PAGES 5
#define SIDEBAR_W 140

static const char *page_names[NUM_PAGES] = {
	"Background", "Cursor", "Shortcuts", "Font", "Theme"
};

static struct BGTK_Widget *bg_color_input;
static struct BGTK_Widget *bg_path_input;
static struct BGTK_Widget *cursor_path_input;
static char cursor_theme[MAX_PATH_LEN];

#define MAX_SHORTCUT_ROWS 24
static struct BGTK_Widget *shortcut_inputs[MAX_SHORTCUT_ROWS];
static int shortcut_row_count;

static struct BGTK_Widget *font_size_input;
static struct BGTK_Widget *font_sans_input;
static struct BGTK_Widget *font_mono_input;
static struct BGTK_Widget *font_serif_input;

static struct BGTK_Widget *theme_bg_input;
static struct BGTK_Widget *theme_btn_input;
static struct BGTK_Widget *theme_btn_text_input;
static struct BGTK_Widget *theme_frame_border_input;
static struct BGTK_Widget *theme_btn_border_input;
static struct BGTK_Widget *theme_input_border_input;
static struct BGTK_Widget *theme_frame_color_input;
static struct BGTK_Widget *theme_frame_unfocused_input;
static struct BGTK_Widget *theme_focus_input;
static struct BGTK_Widget *theme_focus_bg_input;
static struct BGTK_Widget *theme_input_bg_input;
static struct BGTK_Widget *theme_highlight_input;
static struct BGTK_Widget *theme_rule_color_input;
static struct BGTK_Widget *theme_margin_input;
static struct BGTK_Widget *theme_padding_input;
static struct BGTK_Widget *theme_frame_margin_input;
static struct BGTK_Widget *theme_row_gap_input;
static struct BGTK_Widget *theme_baseline_input;

static char shortcut_labels[MAX_SHORTCUT_ROWS][96];
static char shortcut_actions[MAX_SHORTCUT_ROWS][96];
static char shortcut_keys[MAX_SHORTCUT_ROWS][64];
static int shortcuts_loaded;

static void rebuild_content(void);
static void rebuild_sidebar(void);
static void reflow_shell(void);

static void shortcut_label_from_action(const char *action, char *out, size_t n)
{
	if (!action || !out || n == 0)
		return;
	if (strcmp(action, "builtin:exit") == 0)
		snprintf(out, n, "Exit");
	else if (strcmp(action, "builtin:screenshot") == 0)
		snprintf(out, n, "Screenshot");
	else if (strncmp(action, "command:", 8) == 0)
		snprintf(out, n, "%s", action + 8);
	else
		snprintf(out, n, "%s", action);
}

static void shortcut_set_row(int i, const char *label, const char *key,
			     const char *action)
{
	if (i < 0 || i >= MAX_SHORTCUT_ROWS)
		return;
	snprintf(shortcut_labels[i], sizeof(shortcut_labels[0]), "%s",
		 label ? label : "");
	snprintf(shortcut_keys[i], sizeof(shortcut_keys[0]), "%s",
		 key ? key : "");
	snprintf(shortcut_actions[i], sizeof(shortcut_actions[0]), "%s",
		 action ? action : "");
}

static void load_shortcuts_defaults(void)
{
	/* BGCE builtins + common command examples (see bgce README). */
	shortcut_set_row(0, "Exit", "ctrl+alt+q", "builtin:exit");
	shortcut_set_row(1, "Screenshot", "sysrq", "builtin:screenshot");
	shortcut_set_row(2, "Terminal", "ctrl+alt+t", "command:terminal");
	shortcut_set_row(3, "Launcher", "ctrl+alt+l", "command:launcher");
	shortcut_set_row(4, "Settings", "ctrl+alt+s", "command:settings");
	shortcut_set_row(5, "Gemini", "ctrl+alt+g", "command:gemini_browser");
	shortcut_row_count = 6;
}

/* Ensure compositor builtins always appear at the top of the list. */
static void ensure_builtin_shortcuts(void)
{
	int i, has_exit = 0, has_ss = 0, need = 0;

	for (i = 0; i < shortcut_row_count; i++) {
		if (strcmp(shortcut_actions[i], "builtin:exit") == 0)
			has_exit = 1;
		if (strcmp(shortcut_actions[i], "builtin:screenshot") == 0)
			has_ss = 1;
	}
	if (!has_exit)
		need++;
	if (!has_ss)
		need++;
	if (need < 1)
		return;
	if (shortcut_row_count + need > MAX_SHORTCUT_ROWS)
		shortcut_row_count = MAX_SHORTCUT_ROWS - need;
	if (shortcut_row_count < 0)
		shortcut_row_count = 0;
	/* Shift existing rows down so builtins stay first. */
	for (i = shortcut_row_count - 1; i >= 0; i--) {
		shortcut_set_row(i + need, shortcut_labels[i], shortcut_keys[i],
				 shortcut_actions[i]);
	}
	i = 0;
	if (!has_exit) {
		shortcut_set_row(i, "Exit", "ctrl+alt+q", "builtin:exit");
		i++;
	}
	if (!has_ss) {
		shortcut_set_row(i, "Screenshot", "sysrq", "builtin:screenshot");
		i++;
	}
	shortcut_row_count += need;
}

static void load_shortcuts_table(void)
{
	const char *home;
	char path[512];
	FILE *f;
	char line[512];
	char section[64] = "";

	load_shortcuts_defaults();
	shortcuts_loaded = 0;

	home = getenv("HOME");
	if (!home || !home[0]) {
		ensure_builtin_shortcuts();
		return;
	}
	snprintf(path, sizeof(path), "%s/.config/bgce.conf", home);
	f = fopen(path, "r");
	if (!f) {
		ensure_builtin_shortcuts();
		return;
	}

	/* Replace defaults with file contents when [shortcuts] has entries. */
	while (fgets(line, sizeof(line), f)) {
		char *p = line;
		char *eq;
		char key[128], val[256];
		char label[96];

		while (*p == ' ' || *p == '\t')
			p++;
		if (*p == '#' || *p == ';' || *p == '\n' || *p == '\0')
			continue;
		if (*p == '[') {
			char *end = strchr(p, ']');
			section[0] = '\0';
			if (end && (size_t)(end - p - 1) < sizeof(section)) {
				memcpy(section, p + 1, (size_t)(end - p - 1));
				section[end - p - 1] = '\0';
			}
			continue;
		}
		if (strcmp(section, "shortcuts") != 0)
			continue;
		eq = strchr(p, '=');
		if (!eq)
			continue;
		*eq = '\0';
		/* trim key (combo) */
		{
			char *k = p;
			char *e = eq - 1;
			while (*k == ' ' || *k == '\t')
				k++;
			while (e > k && (*e == ' ' || *e == '\t'))
				e--;
			e[1] = '\0';
			strncpy(key, k, sizeof(key) - 1);
			key[sizeof(key) - 1] = '\0';
		}
		{
			char *v = eq + 1;
			char *e;
			while (*v == ' ' || *v == '\t')
				v++;
			e = v + strlen(v) - 1;
			while (e > v && (*e == '\n' || *e == '\r' ||
					 *e == ' ' || *e == '\t'))
				e--;
			e[1] = '\0';
			strncpy(val, v, sizeof(val) - 1);
			val[sizeof(val) - 1] = '\0';
		}
		if (!shortcuts_loaded) {
			shortcut_row_count = 0;
			shortcuts_loaded = 1;
		}
		if (shortcut_row_count >= MAX_SHORTCUT_ROWS)
			break;
		shortcut_label_from_action(val, label, sizeof(label));
		shortcut_set_row(shortcut_row_count, label, key, val);
		shortcut_row_count++;
	}
	fclose(f);
	/* Empty [shortcuts] section: keep the built-in defaults. */
	if (!shortcuts_loaded)
		load_shortcuts_defaults();
	/* Always surface compositor builtins (exit / screenshot). */
	ensure_builtin_shortcuts();
}

/* ------------------------------------------------------------------ */
/* File scanning helpers                                                */
/* ------------------------------------------------------------------ */

static int scan_dir(const char *dir, char ***out, int dirs_only)
{
	*out = NULL;
	DIR *d = opendir(dir);
	if (!d)
		return 0;

	int cap = 32, n = 0;
	char **list = malloc(cap * sizeof(char *));
	struct dirent *ent;
	while ((ent = readdir(d))) {
		if (ent->d_name[0] == '.')
			continue;
		if (dirs_only) {
			char full[1024];
			snprintf(full, sizeof(full), "%s/%s", dir, ent->d_name);
			struct stat st;
			if (stat(full, &st) || !S_ISDIR(st.st_mode))
				continue;
		}
		if (n >= cap) {
			cap *= 2;
			list = realloc(list, cap * sizeof(char *));
		}
		list[n++] = strdup(ent->d_name);
	}
	closedir(d);
	*out = list;
	return n;
}

static int scan_cursors(char ***out)
{
	static const char *dirs[] = {
#ifdef __linux__
		"/usr/share/icons",
		"/usr/local/share/icons",
#endif
		NULL
	};

	int cap = 32, n = 0;
	char **list = malloc(cap * sizeof(char *));
	*out = list;

	for (int i = 0; dirs[i]; i++) {
		char **sub = NULL;
		int sn = scan_dir(dirs[i], &sub, 1);
		for (int j = 0; j < sn; j++) {
			char cpath[1024];
			snprintf(cpath, sizeof(cpath), "%s/%s/cursors", dirs[i], sub[j]);
			struct stat st;
			if (stat(cpath, &st) == 0 && S_ISDIR(st.st_mode)) {
				if (n >= cap) {
					cap *= 2;
					list = realloc(list, cap * sizeof(char *));
					*out = list;
				}
				list[n++] = strdup(sub[j]);
			}
			free(sub[j]);
		}
		free(sub);
	}
	return n;
}

/* ------------------------------------------------------------------ */
/* Form helpers (pure widgets)                                         */
/* ------------------------------------------------------------------ */

static int theme_outer_pad(void)
{
	return (ctx && ctx->theme.padding > 0) ? ctx->theme.padding : 12;
}

/* Control pad: theme.pad/2+2 (matches former HTML cell pad). */
static int control_pad(void)
{
	int p = theme_outer_pad();
	int cp = p / 2 + 2;

	if (cp < 2)
		cp = 2;
	if (cp > 10)
		cp = 10;
	return cp;
}

static int panel_inset(void)
{
	return control_pad();
}

static int root_pad(void)
{
	int outer = theme_outer_pad();
	int pin = panel_inset();

	return outer > pin ? outer - pin : 0;
}

/* One side of window chrome: border + frame_margin + root padding.
 * Matches content→window-edge distance (top/bottom/left). */
static int edge_inset(void)
{
	int fmar = (ctx && ctx->theme.frame_margin >= 0) ? ctx->theme.frame_margin
							: 0;
	int fbw = ctx ? (int)ctx->theme.frame_border_size : 1;
	int e;

	if (fbw < 0)
		fbw = 0;
	e = fmar + fbw + root_pad();
	if (e < 8)
		e = 8;
	return e;
}

static BGTK_Options ctl_opts(void)
{
	return (BGTK_Options){.padding = control_pad(), .margin = 0,
			      .text_align = BGTK_ALIGN_LEFT,
			      .text_v_align = BGTK_VALIGN_CENTER};
}

static struct BGTK_Widget *ui_text(const char *s)
{
	return bgtk_text(ctx, (char *)s,
		(BGTK_Options){.padding = 2, .margin = 0,
			       .text_align = BGTK_ALIGN_LEFT,
			       .text_v_align = BGTK_VALIGN_CENTER});
}

/* Fixed-width label so form columns line up. */
static struct BGTK_Widget *ui_label(const char *s, int min_w)
{
	struct BGTK_Widget *t = ui_text(s ? s : "");

	if (t && min_w > 0 && t->w < min_w)
		t->w = min_w;
	return t;
}

static struct BGTK_Widget *ui_btn(const char *s, BGTK_Callback cb, void *data)
{
	return bgtk_button(ctx, ui_text(s), cb, data, ctl_opts());
}

static struct BGTK_Widget *ui_input(const char *val, int width)
{
	return bgtk_text_input(ctx, (char *)(val ? val : ""), width, 0,
			       ctl_opts());
}

static struct BGTK_Widget *ui_hbox(struct BGTK_Widget **items, int n)
{
	/* CENTER: vertically align label + field on form rows. */
	return bgtk_list(ctx, items, n,
		(BGTK_Options){.orientation = BGTK_LIST_HORIZONTAL,
			       .margin = 0, .padding = 0,
			       .flags = BGTK_FLAG_CENTER});
}

static struct BGTK_Widget *ui_vbox(struct BGTK_Widget **items, int n)
{
	/* List margin → 2×margin between rows. Keep small so L/R stay even
	 * (vertical lists also use margin as a side inset when packing). */
	int gap = 4;

	if (ctx && ctx->theme.margin > 0) {
		gap = ctx->theme.margin / 2;
		if (gap < 4)
			gap = 4;
		if (gap > 8)
			gap = 8;
	}
	return bgtk_list(ctx, items, n,
		(BGTK_Options){.orientation = BGTK_LIST_VERTICAL,
			       .margin = gap, .padding = 0});
}

#define FORM_LABEL_W 130
/* Theme page: color column (long names) and size column. */
#define FORM_THEME_COLOR_LAB 180
#define FORM_THEME_SIZE_LAB 160
#define FORM_FIELD_W 100 /* fixed so value columns line up */
#define FORM_THEME_SIZE_FIELD 72
#define FORM_PATH_W 420  /* bg/cursor/font path fields */
/* List gap = 2×margin between the two theme columns. */
#define FORM_COL_GAP 16
/* Sidebar nav: list gap = 2×margin → 16px between buttons. */
#define SIDEBAR_NAV_MARGIN 8

static void ui_force_field_w(struct BGTK_Widget *f, int width)
{
	if (f && width > 0)
		f->w = width;
}

static struct BGTK_Widget *ui_row(const char *label, struct BGTK_Widget *field)
{
	/* Min width for short controls; leave wider path fields alone. */
	if (field && field->w > 0 && field->w < FORM_FIELD_W)
		ui_force_field_w(field, FORM_FIELD_W);
	struct BGTK_Widget *items[2] = { ui_label(label, FORM_LABEL_W), field };
	return ui_hbox(items, 2);
}

/* Two form columns: [lab|field] —gap— [lab|field] with fixed field widths. */
static struct BGTK_Widget *ui_row2(const char *l0, struct BGTK_Widget *f0,
				   const char *l1, struct BGTK_Widget *f1)
{
	struct BGTK_Widget *c1_i[2], *c2_i[2], *cols[2];
	struct BGTK_Widget *c1, *c2;

	ui_force_field_w(f0, FORM_FIELD_W);
	ui_force_field_w(f1, FORM_FIELD_W);
	c1_i[0] = ui_label(l0, FORM_LABEL_W);
	c1_i[1] = f0;
	c2_i[0] = ui_label(l1, FORM_THEME_COLOR_LAB);
	c2_i[1] = f1;
	c1 = ui_hbox(c1_i, 2);
	c2 = ui_hbox(c2_i, 2);
	/* Pin column outer widths so value edges stay on a grid. */
	ui_force_field_w(c1, FORM_LABEL_W + FORM_FIELD_W);
	ui_force_field_w(c2, FORM_THEME_COLOR_LAB + FORM_FIELD_W);
	cols[0] = c1;
	cols[1] = c2;
	return bgtk_list(ctx, cols, 2,
		(BGTK_Options){.orientation = BGTK_LIST_HORIZONTAL,
			       .margin = FORM_COL_GAP, .padding = 0});
}

/* One theme form row: fixed label width + field. */
static struct BGTK_Widget *ui_theme_row(const char *label, int lab_w,
					struct BGTK_Widget *field, int field_w)
{
	struct BGTK_Widget *items[2];

	ui_force_field_w(field, field_w);
	items[0] = ui_label(label, lab_w);
	items[1] = field;
	return ui_hbox(items, 2);
}

/* Content size after reflow (panel filled by shell EXPAND). */
static void content_size(int *out_w, int *out_h)
{
	int w = content_panel ? content_panel->w : 400;
	int h = content_panel ? content_panel->h : 300;

	if (w < 80)
		w = 80;
	if (h < 40)
		h = 40;
	if (out_w)
		*out_w = w;
	if (out_h)
		*out_h = h;
}

/*
 * Page chrome: scrollable form on top (EXPAND_Y) + Apply pinned at bottom.
 * Uses bgtk_spacer if you need mid-form flex; here the scroll itself expands.
 */
static struct BGTK_Widget *make_page(struct BGTK_Widget *form,
				    BGTK_Callback apply_cb)
{
	struct BGTK_Widget *scroll, *apply, *outer, *frame;
	struct BGTK_Widget *items[2];
	int pin = panel_inset();
	int pw, ph;

	content_size(&pw, &ph);
	if (form)
		form->flags |= BGTK_FLAG_EXPAND_X;
	scroll = bgtk_scrollable(ctx, &form, 1,
		(BGTK_Options){.padding = pin, .margin = 0});
	scroll->w = pw > 0 ? pw : 80;
	/* Leave room for Apply + list gap so the last form row is not clipped. */
	scroll->h = ph > 80 ? ph - 56 : 40;
	scroll->flags |= BGTK_FLAG_EXPAND_Y | BGTK_FLAG_EXPAND_X;

	apply = ui_btn("Apply", apply_cb, NULL);
	items[0] = scroll;
	items[1] = apply;
	outer = bgtk_list(ctx, items, 2,
		(BGTK_Options){.orientation = BGTK_LIST_VERTICAL,
			       .margin = 4, .padding = pin});
	if (outer) {
		outer->w = pw;
		outer->h = ph;
		outer->flags |= BGTK_FLAG_FILL;
	}
	frame = bgtk_frame(ctx, outer ? outer : scroll, pw, ph,
		(BGTK_Options){.padding = 0, .margin = 0});
	if (frame) {
		frame->data.frame.border_w = 0;
		frame->flags |= BGTK_FLAG_FILL;
	}
	return frame ? frame : outer;
}

static uint32_t parse_color_input(const char *text)
{
	return parse_hex_color(text);
}

/* Merge [background] into ~/.config/bgce.conf (compositor owns the desktop). */
static void write_bgce_background(void)
{
	const char *home = getenv("HOME");
	char path[512], tmp[512], dir[512];
	FILE *in, *out;
	char line[512];
	char section[64] = "";
	int wrote = 0;

	if (!home || !home[0])
		return;
	snprintf(dir, sizeof(dir), "%s/.config", home);
	(void)mkdir(dir, 0755);
	snprintf(path, sizeof(path), "%s/.config/bgce.conf", home);
	snprintf(tmp, sizeof(tmp), "%s/.config/bgce.conf.bgtk-tmp", home);
	in = fopen(path, "r");
	out = fopen(tmp, "w");
	if (!out) {
		if (in)
			fclose(in);
		bgtk_log("write_bgce_background: cannot open %s", tmp);
		return;
	}

	if (in) {
		int skip = 0;
		while (fgets(line, sizeof(line), in)) {
			char *p = line;
			while (*p == ' ' || *p == '\t')
				p++;
			if (*p == '[') {
				char *end = strchr(p, ']');
				section[0] = '\0';
				if (end &&
				    (size_t)(end - p - 1) < sizeof(section)) {
					memcpy(section, p + 1,
					       (size_t)(end - p - 1));
					section[end - p - 1] = '\0';
				}
				if (strcmp(section, "background") == 0) {
					skip = 1;
					continue;
				}
				if (skip) {
					/* leaving [background]: emit new one */
					char c[16];
					fprintf(out, "[background]\n");
					if (cfg.type == BG_IMAGE) {
						fprintf(out, "type = image\n");
						fprintf(out, "path = %s\n",
							cfg.path);
						fprintf(out, "mode = %s\n",
							cfg.mode == IMAGE_SCALED
								? "scaled"
								: "tiled");
					} else {
						fprintf(out, "type = color\n");
						format_hex_color(cfg.color, c,
								 sizeof(c));
						fprintf(out, "color = %s\n", c);
					}
					fprintf(out, "\n");
					wrote = 1;
					skip = 0;
				}
				fputs(line, out);
				continue;
			}
			if (skip)
				continue;
			fputs(line, out);
		}
		if (skip) {
			char c[16];
			fprintf(out, "[background]\n");
			if (cfg.type == BG_IMAGE) {
				fprintf(out, "type = image\n");
				fprintf(out, "path = %s\n", cfg.path);
				fprintf(out, "mode = %s\n",
					cfg.mode == IMAGE_SCALED ? "scaled"
								 : "tiled");
			} else {
				fprintf(out, "type = color\n");
				format_hex_color(cfg.color, c, sizeof(c));
				fprintf(out, "color = %s\n", c);
			}
			wrote = 1;
		}
		fclose(in);
	}
	if (!wrote) {
		char c[16];
		fprintf(out, "[background]\n");
		if (cfg.type == BG_IMAGE) {
			fprintf(out, "type = image\n");
			fprintf(out, "path = %s\n", cfg.path);
			fprintf(out, "mode = %s\n",
				cfg.mode == IMAGE_SCALED ? "scaled" : "tiled");
		} else {
			fprintf(out, "type = color\n");
			format_hex_color(cfg.color, c, sizeof(c));
			fprintf(out, "color = %s\n", c);
		}
	}
	fclose(out);
	if (rename(tmp, path) != 0)
		bgtk_log_errno("write_bgce_background rename");
	else
		bgtk_log("wrote background to %s", path);
}

static void apply_background(void *userdata)
{
	(void)userdata;
	if (bg_color_input && bg_color_input->data.text_input.text)
		cfg.color = parse_color_input(
			bg_color_input->data.text_input.text);
	if (bg_path_input && bg_path_input->data.text_input.text &&
	    bg_path_input->data.text_input.text[0]) {
		strncpy(cfg.path, bg_path_input->data.text_input.text,
			MAX_PATH_LEN - 1);
		cfg.path[MAX_PATH_LEN - 1] = '\0';
	}
	/* Desktop background lives in bgce.conf; also mirror into bgtk.conf. */
	write_bgce_background();
	write_config(&cfg);
	/* Structure unchanged — redraw only (preview updates on page revisit). */
	bgtk_draw_widgets(ctx);
}

/* Switch value is already updated; sync cfg and rebuild page structure. */
static void on_bg_type_switch(void *userdata)
{
	struct BGTK_Widget *sw = userdata;

	if (sw && sw->type == BGTK_WIDGET_SWITCH)
		cfg.type = sw->data.switch_w.value ? BG_IMAGE : BG_COLOR;
	else
		cfg.type = (cfg.type == BG_COLOR) ? BG_IMAGE : BG_COLOR;
	rebuild_content();
}

static void on_bg_mode_switch(void *userdata)
{
	struct BGTK_Widget *sw = userdata;

	if (sw && sw->type == BGTK_WIDGET_SWITCH)
		cfg.mode = sw->data.switch_w.value ? IMAGE_TILED : IMAGE_SCALED;
	else
		cfg.mode = (cfg.mode == IMAGE_TILED) ? IMAGE_SCALED : IMAGE_TILED;
	rebuild_content();
}

/* Test helper: flip mode without a widget. */
static void toggle_bg_mode(void *userdata)
{
	(void)userdata;
	cfg.mode = (cfg.mode == IMAGE_TILED) ? IMAGE_SCALED : IMAGE_TILED;
	rebuild_content();
}

/* Write [cursors] theme into bgce.conf (merge, keep other sections). */
static void write_bgce_cursors(void)
{
	const char *home = getenv("HOME");
	char path[512], tmp[512], dir[512];
	FILE *in, *out;
	char line[512];
	char section[64] = "";
	int wrote = 0;

	if (!home || !home[0])
		return;
	if (cursor_path_input && cursor_path_input->data.text_input.text) {
		strncpy(cursor_theme, cursor_path_input->data.text_input.text,
			MAX_PATH_LEN - 1);
		cursor_theme[MAX_PATH_LEN - 1] = '\0';
	}
	snprintf(dir, sizeof(dir), "%s/.config", home);
	(void)mkdir(dir, 0755);
	snprintf(path, sizeof(path), "%s/.config/bgce.conf", home);
	snprintf(tmp, sizeof(tmp), "%s/.config/bgce.conf.bgtk-tmp", home);
	in = fopen(path, "r");
	out = fopen(tmp, "w");
	if (!out) {
		if (in)
			fclose(in);
		return;
	}
	if (in) {
		int skip = 0;
		while (fgets(line, sizeof(line), in)) {
			char *p = line;
			while (*p == ' ' || *p == '\t')
				p++;
			if (*p == '[') {
				char *end = strchr(p, ']');
				section[0] = '\0';
				if (end &&
				    (size_t)(end - p - 1) < sizeof(section)) {
					memcpy(section, p + 1,
					       (size_t)(end - p - 1));
					section[end - p - 1] = '\0';
				}
				if (strcmp(section, "cursors") == 0) {
					skip = 1;
					continue;
				}
				if (skip) {
					fprintf(out, "[cursors]\n");
					if (cursor_theme[0])
						fprintf(out, "theme = %s\n",
							cursor_theme);
					fprintf(out, "\n");
					wrote = 1;
					skip = 0;
				}
				fputs(line, out);
				continue;
			}
			if (skip)
				continue;
			fputs(line, out);
		}
		if (skip) {
			fprintf(out, "[cursors]\n");
			if (cursor_theme[0])
				fprintf(out, "theme = %s\n", cursor_theme);
			wrote = 1;
		}
		fclose(in);
	}
	if (!wrote) {
		fprintf(out, "[cursors]\n");
		if (cursor_theme[0])
			fprintf(out, "theme = %s\n", cursor_theme);
	}
	fclose(out);
	if (rename(tmp, path) != 0)
		bgtk_log_errno("write_bgce_cursors rename");
	else
		bgtk_log("wrote cursors theme='%s' to %s",
			 cursor_theme[0] ? cursor_theme : "(empty)", path);
}

static void apply_cursor(void *userdata)
{
	(void)userdata;
	write_bgce_cursors();
	bgtk_draw_widgets(ctx);
}

static void pick_cursor_theme(void *userdata)
{
	const char *name = (const char *)userdata;
	if (name && name[0]) {
		strncpy(cursor_theme, name, MAX_PATH_LEN - 1);
		cursor_theme[MAX_PATH_LEN - 1] = '\0';
	}
	if (cursor_path_input && cursor_path_input->type == BGTK_WIDGET_TEXT_INPUT) {
		free(cursor_path_input->data.text_input.text);
		cursor_path_input->data.text_input.text = strdup(cursor_theme);
		cursor_path_input->data.text_input.cursor_pos =
			(uint32_t)strlen(cursor_theme);
	}
	bgtk_draw_widgets(ctx);
}

static void apply_shortcuts(void *userdata)
{
	const char *home;
	char path[512], tmp[512];
	FILE *in, *out;
	char line[512];
	char section[64] = "";
	int i;

	(void)userdata;
	for (i = 0; i < shortcut_row_count; i++) {
		if (shortcut_inputs[i] &&
		    shortcut_inputs[i]->data.text_input.text)
			strncpy(shortcut_keys[i],
				shortcut_inputs[i]->data.text_input.text,
				sizeof(shortcut_keys[0]) - 1);
	}

	/* Persist to ~/.config/bgce.conf [shortcuts] (BGCE owns these). */
	home = getenv("HOME");
	if (!home || !home[0]) {
		bgtk_draw_widgets(ctx);
		return;
	}
	snprintf(path, sizeof(path), "%s/.config/bgce.conf", home);
	snprintf(tmp, sizeof(tmp), "%s/.config/bgce.conf.bgtk-tmp", home);
	in = fopen(path, "r");
	out = fopen(tmp, "w");
	if (!out) {
		if (in)
			fclose(in);
		bgtk_draw_widgets(ctx);
		return;
	}
	if (in) {
		int skip_shortcuts = 0;
		while (fgets(line, sizeof(line), in)) {
			char *p = line;
			while (*p == ' ' || *p == '\t')
				p++;
			if (*p == '[') {
				char *end = strchr(p, ']');
				section[0] = '\0';
				if (end &&
				    (size_t)(end - p - 1) < sizeof(section)) {
					memcpy(section, p + 1,
					       (size_t)(end - p - 1));
					section[end - p - 1] = '\0';
				}
				if (strcmp(section, "shortcuts") == 0) {
					skip_shortcuts = 1;
					continue;
				}
				if (skip_shortcuts) {
					/* leaving shortcuts: emit new section once */
					fprintf(out, "[shortcuts]\n");
					for (i = 0; i < shortcut_row_count; i++)
						if (shortcut_keys[i][0] &&
						    shortcut_actions[i][0])
							fprintf(out, "%s = %s\n",
								shortcut_keys[i],
								shortcut_actions[i]);
					fprintf(out, "\n");
					skip_shortcuts = 0;
				}
				fputs(line, out);
				continue;
			}
			if (skip_shortcuts)
				continue;
			fputs(line, out);
		}
		if (skip_shortcuts) {
			fprintf(out, "[shortcuts]\n");
			for (i = 0; i < shortcut_row_count; i++)
				if (shortcut_keys[i][0] && shortcut_actions[i][0])
					fprintf(out, "%s = %s\n",
						shortcut_keys[i],
						shortcut_actions[i]);
		}
		fclose(in);
	} else {
		fprintf(out, "[shortcuts]\n");
		for (i = 0; i < shortcut_row_count; i++)
			if (shortcut_keys[i][0] && shortcut_actions[i][0])
				fprintf(out, "%s = %s\n", shortcut_keys[i],
					shortcut_actions[i]);
	}
	fclose(out);
	rename(tmp, path);
	bgtk_draw_widgets(ctx);
}

static void apply_font(void *userdata);

/* text_input on_enter is void (*)(void); bridge to Apply callback. */
static void font_path_on_enter(void)
{
	apply_font(NULL);
}

static void apply_font(void *userdata)
{
	(void)userdata;
	if (font_sans_input && font_sans_input->data.text_input.text) {
		strncpy(cfg.font_sans_path,
			font_sans_input->data.text_input.text,
			MAX_PATH_LEN - 1);
		cfg.font_sans_path[MAX_PATH_LEN - 1] = '\0';
	}
	if (font_mono_input && font_mono_input->data.text_input.text) {
		strncpy(cfg.font_mono_path,
			font_mono_input->data.text_input.text,
			MAX_PATH_LEN - 1);
		cfg.font_mono_path[MAX_PATH_LEN - 1] = '\0';
	}
	if (font_serif_input && font_serif_input->data.text_input.text) {
		strncpy(cfg.font_serif_path,
			font_serif_input->data.text_input.text,
			MAX_PATH_LEN - 1);
		cfg.font_serif_path[MAX_PATH_LEN - 1] = '\0';
	}
	if (font_size_input && font_size_input->data.text_input.text) {
		int sz = atoi(font_size_input->data.text_input.text);
		if (sz > 0 && sz < 200)
			cfg.font_size = sz;
	}
	write_config(&cfg);
	/* Live preview: reload faces and rebuild labels with new metrics. */
	if (ctx)
		bgtk_reload_fonts(ctx, cfg.font_sans_path, cfg.font_mono_path,
				  cfg.font_serif_path, cfg.font_size);
	rebuild_content();
}

static void apply_theme(void *userdata)
{
	(void)userdata;
	if (theme_bg_input && theme_bg_input->data.text_input.text)
		cfg.theme.background =
			parse_color_input(theme_bg_input->data.text_input.text);
	if (theme_btn_input && theme_btn_input->data.text_input.text)
		cfg.theme.button =
			parse_color_input(theme_btn_input->data.text_input.text);
	if (theme_btn_text_input && theme_btn_text_input->data.text_input.text)
		cfg.theme.button_text = parse_color_input(
			theme_btn_text_input->data.text_input.text);
	if (theme_frame_border_input &&
	    theme_frame_border_input->data.text_input.text)
		cfg.theme.frame_border_size =
			(uint32_t)atoi(theme_frame_border_input->data.text_input.text);
	if (theme_btn_border_input &&
	    theme_btn_border_input->data.text_input.text)
		cfg.theme.button_border_size =
			(uint32_t)atoi(theme_btn_border_input->data.text_input.text);
	if (theme_input_border_input &&
	    theme_input_border_input->data.text_input.text)
		cfg.theme.input_border_size =
			(uint32_t)atoi(theme_input_border_input->data.text_input.text);
	if (theme_frame_color_input &&
	    theme_frame_color_input->data.text_input.text)
		cfg.theme.frame_border_color = parse_color_input(
			theme_frame_color_input->data.text_input.text);
	if (theme_frame_unfocused_input &&
	    theme_frame_unfocused_input->data.text_input.text)
		cfg.theme.frame_border_unfocused = parse_color_input(
			theme_frame_unfocused_input->data.text_input.text);
	if (theme_focus_input && theme_focus_input->data.text_input.text)
		cfg.theme.focus =
			parse_color_input(theme_focus_input->data.text_input.text);
	if (theme_focus_bg_input && theme_focus_bg_input->data.text_input.text)
		cfg.theme.focus_bg = parse_color_input(
			theme_focus_bg_input->data.text_input.text);
	if (theme_input_bg_input && theme_input_bg_input->data.text_input.text)
		cfg.theme.input_bg = parse_color_input(
			theme_input_bg_input->data.text_input.text);
	if (theme_highlight_input && theme_highlight_input->data.text_input.text)
		cfg.theme.highlight = parse_color_input(
			theme_highlight_input->data.text_input.text);
	if (theme_rule_color_input &&
	    theme_rule_color_input->data.text_input.text)
		cfg.theme.rule_color = parse_color_input(
			theme_rule_color_input->data.text_input.text);
	if (theme_margin_input && theme_margin_input->data.text_input.text)
		cfg.theme.margin =
			atoi(theme_margin_input->data.text_input.text);
	if (theme_padding_input && theme_padding_input->data.text_input.text)
		cfg.theme.padding =
			atoi(theme_padding_input->data.text_input.text);
	if (theme_frame_margin_input &&
	    theme_frame_margin_input->data.text_input.text)
		cfg.theme.frame_margin =
			atoi(theme_frame_margin_input->data.text_input.text);
	if (theme_row_gap_input && theme_row_gap_input->data.text_input.text)
		cfg.theme.row_gap =
			atoi(theme_row_gap_input->data.text_input.text);
	if (theme_baseline_input && theme_baseline_input->data.text_input.text)
		cfg.theme.text_baseline_offset =
			atoi(theme_baseline_input->data.text_input.text);
	write_config(&cfg);
	if (ctx)
		ctx->theme = cfg.theme;
	reflow_shell();
	bgtk_draw_widgets(ctx);
}

/* Wallpaper preview */
static void preview_fit(int max_w, int max_h, int *out_w, int *out_h)
{
	int aw = screen_aspect_w > 0 ? screen_aspect_w : 16;
	int ah = screen_aspect_h > 0 ? screen_aspect_h : 9;
	int pw, ph;

	if (max_w < 80)
		max_w = 80;
	if (max_h < 48)
		max_h = 48;
	/* Prefer using width, then clamp height. */
	pw = max_w;
	ph = (pw * ah) / aw;
	if (ph > max_h) {
		ph = max_h;
		pw = (ph * aw) / ah;
	}
	if (pw < 1)
		pw = 1;
	if (ph < 1)
		ph = 1;
	*out_w = pw;
	*out_h = ph;
}

/* Render source into dest (pw x ph) like BGCE apply_background. */
static void blit_wallpaper_preview(uint32_t *dst, int pw, int ph,
				   const uint32_t *src, int sw, int sh,
				   ImageMode mode)
{
	int x, y;

	if (!dst || !src || pw < 1 || ph < 1 || sw < 1 || sh < 1)
		return;
	if (mode == IMAGE_TILED) {
		for (y = 0; y < ph; y++) {
			int sy = y % sh;
			for (x = 0; x < pw; x++)
				dst[y * pw + x] = src[sy * sw + (x % sw)];
		}
		return;
	}
	/* IMAGE_SCALED: stretch to fill preview (nearest). */
	for (y = 0; y < ph; y++) {
		int sy = (int)((long)y * sh / ph);
		if (sy >= sh)
			sy = sh - 1;
		for (x = 0; x < pw; x++) {
			int sx = (int)((long)x * sw / pw);
			if (sx >= sw)
				sx = sw - 1;
			dst[y * pw + x] = src[sy * sw + sx];
		}
	}
}

/* Load wallpaper and paint preview buffer in scaled or tiled mode. */
static struct BGTK_Widget *make_image_preview(int pw, int ph, const char *path,
					     ImageMode mode)
{
	struct BGTK_Widget *img;
	uint32_t *src = NULL, *dst = NULL;
	int sw = 0, sh = 0;

	if (pw < 1)
		pw = 1;
	if (ph < 1)
		ph = 1;
	if (load_image(path, &src, &sw, &sh) != 0 || !src || sw < 1 || sh < 1) {
		free(src);
		return NULL;
	}
	dst = malloc((size_t)pw * (size_t)ph * sizeof(uint32_t));
	if (!dst) {
		free(src);
		return NULL;
	}
	blit_wallpaper_preview(dst, pw, ph, src, sw, sh, mode);
	free(src);

	img = bgtk_image(ctx, NULL, pw, ph, (BGTK_Options){.padding = 0, .margin = 4});
	if (!img) {
		free(dst);
		return NULL;
	}
	img->data.image.pixels = dst;
	img->data.image.img_w = pw;
	img->data.image.img_h = ph;
	img->w = pw + 8;
	img->h = ph + 8;
	return img;
}

/* Solid fill in framebuffer format 0xAARRGGBB (matches draw_rect / BGCE). */
static struct BGTK_Widget *make_solid_preview(int pw, int ph, uint32_t color)
{
	struct BGTK_Widget *img;
	uint32_t *pix;
	int i;
	uint8_t r = (uint8_t)((color >> 16) & 0xFF);
	uint8_t g = (uint8_t)((color >> 8) & 0xFF);
	uint8_t b = (uint8_t)(color & 0xFF);
	/* Full alpha; channels in the same order as clear_buffer / draw_rect. */
	uint32_t c = (0xFFu << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) |
		     (uint32_t)b;

	if (pw < 1)
		pw = 1;
	if (ph < 1)
		ph = 1;
	pix = malloc((size_t)pw * (size_t)ph * sizeof(uint32_t));
	if (!pix)
		return NULL;
	for (i = 0; i < pw * ph; i++)
		pix[i] = c;
	img = bgtk_image(ctx, NULL, pw, ph, (BGTK_Options){.padding = 0, .margin = 4});
	if (!img) {
		free(pix);
		return NULL;
	}
	img->data.image.pixels = pix;
	img->data.image.img_w = pw;
	img->data.image.img_h = ph;
	img->w = pw + 8;
	img->h = ph + 8;
	return img;
}

static void add_bg_preview(struct BGTK_Widget *page)
{
	int pw, ph, panel_w, panel_h;
	struct BGTK_Widget *preview = NULL;
	struct BGTK_Widget *outer, *scroll, *list;
	int n, i, reserved, max_w, max_h, total_h, max_w_list;
	struct BGTK_Widget **new_items;

	if (!page || page->type != BGTK_WIDGET_FRAME)
		return;
	/* page → outer vbox [scroll | Apply] → form list inside scroll. */
	outer = page->data.frame.child;
	if (!outer || outer->type != BGTK_WIDGET_LIST ||
	    outer->data.list_widget.widget_count < 1)
		return;
	scroll = outer->data.list_widget.items[0];
	if (!scroll || scroll->type != BGTK_WIDGET_SCROLLABLE)
		return;
	if (scroll->data.scrollable.widget_count < 1)
		return;
	list = scroll->data.scrollable.items[0];
	if (!list || list->type != BGTK_WIDGET_LIST)
		return;

	n = list->data.list_widget.widget_count;
	if (n < 1)
		return;

	content_size(&panel_w, &panel_h);

	/* Form rows already in list; preview fills leftover panel height. */
	reserved = 0;
	for (i = 0; i < n; i++) {
		struct BGTK_Widget *ch = list->data.list_widget.items[i];
		if (!ch)
			continue;
		reserved += ch->h + 2 * list->margin;
	}
	max_h = panel_h - reserved - 72; /* chrome + Apply row */
	if (max_h < 80)
		max_h = 80;
	max_w = panel_w - 28;
	if (max_w < 80)
		max_w = 80;
	preview_fit(max_w, max_h, &pw, &ph);

	if (cfg.type == BG_IMAGE && cfg.path[0] && access(cfg.path, R_OK) == 0) {
		/* Preview respects cfg.mode (scaled stretch vs tile repeat). */
		preview = make_image_preview(pw, ph, cfg.path, cfg.mode);
		if (!preview)
			preview = make_solid_preview(pw, ph, 0xFF333333);
	} else if (cfg.type == BG_IMAGE) {
		/* Image selected but missing file — dark placeholder. */
		preview = make_solid_preview(pw, ph, 0xFF333333);
	} else {
		preview = make_solid_preview(pw, ph, cfg.color);
	}
	if (!preview)
		return;

	/* Thin border frame around the aspect box. */
	{
		struct BGTK_Widget *framed =
			bgtk_frame(ctx, preview, pw + 12, ph + 12,
				   (BGTK_Options){.padding = 2, .margin = 4});
		if (framed) {
			framed->data.frame.border_w = 1;
			framed->data.frame.border_color =
				ctx->theme.frame_border_color
					? ctx->theme.frame_border_color
					: 0xFF888888;
			preview = framed;
		}
	}

	/* Append preview at end of form (Apply lives outside the scroll). */
	new_items = malloc((n + 1) * sizeof(struct BGTK_Widget *));
	if (!new_items)
		return;
	for (i = 0; i < n; i++)
		new_items[i] = list->data.list_widget.items[i];
	new_items[n] = preview;
	free(list->data.list_widget.items);
	list->data.list_widget.items = new_items;
	list->data.list_widget.widget_count = n + 1;

	total_h = 0;
	max_w_list = 0;
	for (i = 0; i < list->data.list_widget.widget_count; i++) {
		struct BGTK_Widget *ch = list->data.list_widget.items[i];
		if (!ch)
			continue;
		if (ch->w > max_w_list)
			max_w_list = ch->w;
		total_h += ch->h + 2 * list->margin;
	}
	if (list->data.list_widget.widget_count > 0)
		total_h -= 2 * list->margin;
	list->data.list_widget.content_height = total_h;
	list->data.list_widget.content_width = max_w_list;
	list->h = total_h + 2 * (list->margin + list->padding);
	if (list->w < max_w_list + 2 * (list->margin + list->padding))
		list->w = max_w_list + 2 * (list->margin + list->padding);
}

/* ------------------------------------------------------------------ */
/* Page builders                                                       */
/* ------------------------------------------------------------------ */

static void page_cb(void *userdata)
{
	current_page = (int)(intptr_t)userdata;
	rebuild_sidebar();
	rebuild_content();
}

static struct BGTK_Widget *build_background_page(void)
{
	char color_hex[16];
	struct BGTK_Widget *rows[8];
	int n = 0;
	struct BGTK_Widget *body;
	struct BGTK_Widget *page;
	struct BGTK_Widget *type_sw;
	struct BGTK_Widget *mode_sw;

	format_hex_color(cfg.color, color_hex, sizeof(color_hex));

	/* Pill switch: 0 = Color, 1 = Image. pad 0 so "Color" lines up with inputs. */
	type_sw = bgtk_switch(ctx, "Color", "Image",
			      cfg.type == BG_IMAGE ? 1 : 0, on_bg_type_switch,
			      NULL, (BGTK_Options){.padding = 0, .margin = 0});
	if (type_sw)
		type_sw->data.switch_w.cb_data = type_sw;
	rows[n++] = ui_row("Type", type_sw ? type_sw : ui_text("?"));

	if (cfg.type == BG_COLOR) {
		bg_color_input = ui_input(color_hex, 120);
		rows[n++] = ui_row("Color", bg_color_input);
	} else {
		bg_path_input = ui_input(cfg.path, FORM_PATH_W);
		ui_force_field_w(bg_path_input, FORM_PATH_W);
		rows[n++] = ui_row("Path", bg_path_input);
		/* 0 = Scaled, 1 = Tiled */
		mode_sw = bgtk_switch(ctx, "Scaled", "Tiled",
				      cfg.mode == IMAGE_TILED ? 1 : 0,
				      on_bg_mode_switch, NULL,
				      (BGTK_Options){.padding = 0, .margin = 0});
		if (mode_sw)
			mode_sw->data.switch_w.cb_data = mode_sw;
		rows[n++] = ui_row("Mode", mode_sw ? mode_sw : ui_text("?"));
	}
	body = ui_vbox(rows, n);
	page = make_page(body, apply_background);
	add_bg_preview(page);
	return page;
}

static struct BGTK_Widget *build_cursor_page(void)
{
	char **cursors = NULL;
	int nc = scan_cursors(&cursors);
	struct BGTK_Widget **rows;
	int n = 0, i, cap;
	struct BGTK_Widget *body;

	cap = nc + 4;
	rows = calloc(cap, sizeof(*rows));
	if (!rows) {
		for (i = 0; i < nc; i++)
			free(cursors[i]);
		free(cursors);
		return ui_text("Out of memory");
	}
	cursor_path_input = ui_input(cursor_theme, FORM_PATH_W);
	ui_force_field_w(cursor_path_input, FORM_PATH_W);
	rows[n++] = ui_row("Theme", cursor_path_input);
	for (i = 0; i < nc; i++) {
		rows[n++] = ui_btn(cursors[i], pick_cursor_theme, cursors[i]);
		/* cursors[i] kept as cb_data; free list array only */
	}
	free(cursors);
	body = ui_vbox(rows, n);
	free(rows);
	return make_page(body, apply_cursor);
}

static struct BGTK_Widget *build_shortcuts_page(void)
{
	struct BGTK_Widget **rows;
	int n = 0, i;
	struct BGTK_Widget *body;

	/* Always reload so conf + compositor builtins stay in sync. */
	load_shortcuts_table();
	rows = calloc((size_t)shortcut_row_count + 4, sizeof(*rows));
	if (!rows)
		return ui_text("Out of memory");
	rows[n++] = ui_text("Key combo → action (builtins first)");
	for (i = 0; i < shortcut_row_count; i++) {
		char lab[128];

		shortcut_inputs[i] = ui_input(shortcut_keys[i], 160);
		/* Label shows name + action so builtin:exit is visible. */
		snprintf(lab, sizeof(lab), "%s", shortcut_labels[i]);
		rows[n++] = ui_row2(lab, shortcut_inputs[i], "→",
				    ui_text(shortcut_actions[i]));
	}
	body = ui_vbox(rows, n);
	free(rows);
	return make_page(body, apply_shortcuts);
}

static struct BGTK_Widget *build_font_page(void)
{
	char size_buf[16];
	struct BGTK_Widget *rows[16];
	int n = 0;
	struct BGTK_Widget *body;

	snprintf(size_buf, sizeof(size_buf), "%d", cfg.font_size);
	font_sans_input = ui_input(cfg.font_sans_path, FORM_PATH_W);
	font_mono_input = ui_input(cfg.font_mono_path, FORM_PATH_W);
	font_serif_input = ui_input(cfg.font_serif_path, FORM_PATH_W);
	ui_force_field_w(font_sans_input, FORM_PATH_W);
	ui_force_field_w(font_mono_input, FORM_PATH_W);
	ui_force_field_w(font_serif_input, FORM_PATH_W);
	/* Enter applies path/size and reloads FreeType for live preview. */
	font_sans_input->data.text_input.on_enter = font_path_on_enter;
	font_mono_input->data.text_input.on_enter = font_path_on_enter;
	font_serif_input->data.text_input.on_enter = font_path_on_enter;

	rows[n++] = ui_text("Type a font path, then Enter to preview");
	rows[n++] = ui_row("Sans (UI)", font_sans_input);
	rows[n++] = ui_row("Mono", font_mono_input);
	rows[n++] = ui_row("Serif", font_serif_input);

	font_size_input = ui_input(size_buf, 80);
	ui_force_field_w(font_size_input, 80);
	font_size_input->data.text_input.on_enter = font_path_on_enter;
	rows[n++] = ui_row("Size", font_size_input);

	body = ui_vbox(rows, n);
	return make_page(body, apply_font);
}

static struct BGTK_Widget *theme_color_input(uint32_t c, int w)
{
	char hex[16];
	format_hex_color(c, hex, sizeof(hex));
	return ui_input(hex, w);
}

static struct BGTK_Widget *theme_int_input(int v, int w)
{
	char buf[32];
	snprintf(buf, sizeof(buf), "%d", v);
	return ui_input(buf, w);
}

static struct BGTK_Widget *build_theme_page(void)
{
	struct BGTK_Widget *color_rows[12];
	struct BGTK_Widget *size_rows[12];
	struct BGTK_Widget *cols[2];
	struct BGTK_Widget *color_col, *size_col, *body;
	int nc = 0, ns = 0;
	int clab = FORM_THEME_COLOR_LAB;
	int slab = FORM_THEME_SIZE_LAB;
	int cfw = FORM_FIELD_W;
	int sfw = FORM_THEME_SIZE_FIELD;

	/* Left column: colors only. */
	theme_bg_input = theme_color_input(cfg.theme.background, cfw);
	color_rows[nc++] =
		ui_theme_row("Background", clab, theme_bg_input, cfw);
	theme_btn_input = theme_color_input(cfg.theme.button, cfw);
	color_rows[nc++] = ui_theme_row("Button", clab, theme_btn_input, cfw);
	theme_btn_text_input = theme_color_input(cfg.theme.button_text, cfw);
	color_rows[nc++] =
		ui_theme_row("Button text", clab, theme_btn_text_input, cfw);
	theme_focus_input = theme_color_input(cfg.theme.focus, cfw);
	color_rows[nc++] = ui_theme_row("Focus", clab, theme_focus_input, cfw);
	theme_focus_bg_input = theme_color_input(cfg.theme.focus_bg, cfw);
	color_rows[nc++] =
		ui_theme_row("Focus background", clab, theme_focus_bg_input, cfw);
	theme_input_bg_input = theme_color_input(cfg.theme.input_bg, cfw);
	color_rows[nc++] =
		ui_theme_row("Input background", clab, theme_input_bg_input, cfw);
	theme_highlight_input = theme_color_input(cfg.theme.highlight, cfw);
	color_rows[nc++] =
		ui_theme_row("Highlight", clab, theme_highlight_input, cfw);
	theme_rule_color_input = theme_color_input(cfg.theme.rule_color, cfw);
	color_rows[nc++] =
		ui_theme_row("Rule color", clab, theme_rule_color_input, cfw);
	theme_frame_color_input =
		theme_color_input(cfg.theme.frame_border_color, cfw);
	color_rows[nc++] = ui_theme_row("Frame border color", clab,
					theme_frame_color_input, cfw);
	theme_frame_unfocused_input =
		theme_color_input(cfg.theme.frame_border_unfocused, cfw);
	color_rows[nc++] = ui_theme_row("Frame border unfocused", clab,
					theme_frame_unfocused_input, cfw);

	/* Right column: sizes only. */
	theme_frame_border_input =
		theme_int_input((int)cfg.theme.frame_border_size, sfw);
	size_rows[ns++] = ui_theme_row("Frame border size", slab,
				       theme_frame_border_input, sfw);
	theme_btn_border_input =
		theme_int_input((int)cfg.theme.button_border_size, sfw);
	size_rows[ns++] = ui_theme_row("Button border size", slab,
				       theme_btn_border_input, sfw);
	theme_input_border_input =
		theme_int_input((int)cfg.theme.input_border_size, sfw);
	size_rows[ns++] = ui_theme_row("Input border size", slab,
				       theme_input_border_input, sfw);
	theme_margin_input = theme_int_input(cfg.theme.margin, sfw);
	size_rows[ns++] =
		ui_theme_row("Widget margin", slab, theme_margin_input, sfw);
	theme_padding_input = theme_int_input(cfg.theme.padding, sfw);
	size_rows[ns++] =
		ui_theme_row("Frame padding", slab, theme_padding_input, sfw);
	theme_frame_margin_input = theme_int_input(cfg.theme.frame_margin, sfw);
	size_rows[ns++] =
		ui_theme_row("Frame margin", slab, theme_frame_margin_input, sfw);
	theme_row_gap_input = theme_int_input(cfg.theme.row_gap, sfw);
	size_rows[ns++] =
		ui_theme_row("Row gap", slab, theme_row_gap_input, sfw);
	theme_baseline_input =
		theme_int_input(cfg.theme.text_baseline_offset, sfw);
	size_rows[ns++] =
		ui_theme_row("Text baseline", slab, theme_baseline_input, sfw);

	color_col = ui_vbox(color_rows, nc);
	size_col = ui_vbox(size_rows, ns);
	if (color_col)
		color_col->flags |= BGTK_FLAG_EXPAND_Y;
	if (size_col)
		size_col->flags |= BGTK_FLAG_EXPAND_Y;
	cols[0] = color_col;
	cols[1] = size_col;
	body = bgtk_list(ctx, cols, 2,
			 (BGTK_Options){.orientation = BGTK_LIST_HORIZONTAL,
					.margin = FORM_COL_GAP,
					.padding = 0});
	if (body)
		body->flags |= BGTK_FLAG_EXPAND_X;
	return make_page(body, apply_theme);
}


/* ------------------------------------------------------------------ */
/* Rebuild content / sidebar / shell                                   */
/* ------------------------------------------------------------------ */

static void clear_page_ptrs(void)
{
	bg_color_input = bg_path_input = NULL;
	cursor_path_input = NULL;
	for (int i = 0; i < MAX_SHORTCUT_ROWS; i++)
		shortcut_inputs[i] = NULL;
	font_size_input = NULL;
	font_sans_input = font_mono_input = font_serif_input = NULL;
	theme_bg_input = theme_btn_input = theme_btn_text_input = NULL;
	theme_frame_border_input = theme_btn_border_input = NULL;
	theme_input_border_input = theme_frame_color_input = NULL;
	theme_frame_unfocused_input = NULL;
	theme_focus_input = theme_focus_bg_input = theme_input_bg_input = NULL;
	theme_highlight_input = theme_rule_color_input = NULL;
	theme_margin_input = theme_padding_input = NULL;
	theme_frame_margin_input = theme_row_gap_input = NULL;
	theme_baseline_input = NULL;
}

/* Size root + shell_row; EXPAND fills content_panel / sidebar height. */
static void reflow_shell(void)
{
	int rpad, fmar, fbw, inner_w, inner_h, edge, gap;

	if (!ctx || !root_frame)
		return;
	rpad = root_pad();
	fmar = ctx->theme.frame_margin >= 0 ? ctx->theme.frame_margin : 0;
	fbw = (int)ctx->theme.frame_border_size;
	if (fbw < 0)
		fbw = 0;
	root_frame->w = app_w;
	root_frame->h = app_h;
	root_frame->padding = rpad;
	root_frame->margin = fmar;

	/* Content box inside root frame (margin + border + padding). */
	inner_w = app_w - 2 * (fmar + fbw + rpad);
	inner_h = app_h - 2 * (fmar + fbw + rpad);
	if (inner_w < 80)
		inner_w = 80;
	if (inner_h < 40)
		inner_h = 40;

	/* Match button↔rule (and rule↔content) gap to content↔window edge. */
	edge = edge_inset();
	gap = (edge + 1) / 2; /* list inter-item gap = 2×margin */

	if (sidebar) {
		sidebar->w = SIDEBAR_W;
		sidebar->padding = 0; /* gap to rule is shell_row margin */
		sidebar->flags |= BGTK_FLAG_EXPAND_Y;
	}
	if (panel_rule)
		panel_rule->flags |= BGTK_FLAG_EXPAND_Y;
	if (content_panel)
		content_panel->flags |= BGTK_FLAG_FILL;

	if (shell_row) {
		shell_row->w = inner_w;
		shell_row->h = inner_h;
		shell_row->margin = gap;
		/* Same expand pass as draw — content_panel gets free width. */
		bgtk_list_layout_expand(shell_row);
	}
}

static void rebuild_content(void)
{
	struct BGTK_Widget *page = NULL;
	struct BGTK_Widget *old;

	clear_page_ptrs();
	if (!shortcut_row_count)
		load_shortcuts_table();

	reflow_shell();

	switch (current_page) {
	case 0: page = build_background_page(); break;
	case 1: page = build_cursor_page(); break;
	case 2: page = build_shortcuts_page(); break;
	case 3: page = build_font_page(); break;
	case 4: page = build_theme_page(); break;
	}
	if (!page)
		page = ui_text("Error loading page");

	old = content_panel->data.frame.child;
	content_panel->data.frame.child = page;
	bgtk_widget_set_parent(page, content_panel);
	bgtk_widget_destroy(old);
	bgtk_draw_widgets(ctx);
}

static void sidebar_spacing(int *pad, int *mar, int *scroll_pad, int *btn_w)
{
	int p = theme_outer_pad();
	int m = (ctx && ctx->theme.margin > 0) ? ctx->theme.margin / 2 : 4;
	/* No side pad on the scroll — button↔rule gap comes from shell_row
	 * margin (= edge_inset), matching content↔window chrome. */
	int sp = 0;
	int bw = SIDEBAR_W;

	if (bw < 40)
		bw = 40;
	if (pad)
		*pad = p;
	if (mar)
		*mar = m;
	if (scroll_pad)
		*scroll_pad = sp;
	if (btn_w)
		*btn_w = bw;
}

static struct BGTK_Widget *make_nav_button(int i, int btn_w)
{
	int selected = (i == current_page);
	int pad, bw, hpad;
	BGTK_Options to = {.padding = 2, .text_align = BGTK_ALIGN_LEFT,
			   .text_v_align = BGTK_VALIGN_CENTER};
	struct BGTK_Widget *lbl, *btn;

	sidebar_spacing(&pad, NULL, NULL, &bw);
	if (btn_w > 0)
		bw = btn_w;
	if (selected)
		to.text_style = BGTK_TEXT_BOLD;
	lbl = bgtk_text(ctx, (char *)page_names[i], to);
	hpad = pad / 2 + 2;
	if (hpad < 6)
		hpad = 6;
	btn = bgtk_button(ctx, lbl, page_cb, (void *)(intptr_t)i,
			  (BGTK_Options){.padding = hpad, .margin = 0,
					 .text_align = BGTK_ALIGN_LEFT,
					 .text_v_align = BGTK_VALIGN_CENTER});
	btn->w = bw;
	if (selected) {
		uint32_t hi = ctx->theme.highlight ? ctx->theme.highlight
						   : 0xFF505060;
		btn->data.button.bg_override = hi;
	}
	return btn;
}

static void rebuild_sidebar(void)
{
	int btn_w, i;
	struct BGTK_Widget **btns;
	struct BGTK_Widget *old;

	sidebar_spacing(NULL, NULL, NULL, &btn_w);
	btns = malloc(NUM_PAGES * sizeof(*btns));
	if (!btns)
		return;
	for (i = 0; i < NUM_PAGES; i++)
		btns[i] = make_nav_button(i, btn_w);
	old = sidebar_list;
	/* SIDEBAR_NAV_MARGIN → 2× between nav buttons. */
	sidebar_list = bgtk_list(ctx, btns, NUM_PAGES,
		(BGTK_Options){.orientation = BGTK_LIST_VERTICAL,
			       .margin = SIDEBAR_NAV_MARGIN, .padding = 0});
	free(btns);
	sidebar->data.scrollable.items[0] = sidebar_list;
	sidebar->data.scrollable.widget_count = 1;
	sidebar->padding = 0;
	sidebar->margin = 0;
	bgtk_widget_set_parent(sidebar_list, sidebar);
	if (old && old != sidebar_list)
		bgtk_widget_destroy(old);
}

static struct BGTK_Widget *build_sidebar(void)
{
	int pad, mar, scroll_pad, btn_w, i;
	struct BGTK_Widget **btns;
	struct BGTK_Widget *scroll;

	sidebar_spacing(&pad, &mar, &scroll_pad, &btn_w);
	btns = malloc(NUM_PAGES * sizeof(*btns));
	if (!btns)
		return NULL;
	for (i = 0; i < NUM_PAGES; i++)
		btns[i] = make_nav_button(i, btn_w);
	sidebar_list = bgtk_list(ctx, btns, NUM_PAGES,
		(BGTK_Options){.orientation = BGTK_LIST_VERTICAL,
			       .margin = SIDEBAR_NAV_MARGIN, .padding = 0});
	free(btns);
	scroll = bgtk_scrollable(ctx, &sidebar_list, 1,
		(BGTK_Options){.padding = scroll_pad, .margin = 0});
	scroll->w = SIDEBAR_W;
	scroll->h = 40; /* reflow_shell / EXPAND_Y set real height */
	scroll->flags |= BGTK_FLAG_EXPAND_Y;
	return scroll;
}

void settings_build_ui(struct BGTK_Context *c, struct config *config,
		       int width, int height)
{
	ctx = c;
	cfg = *config;
	app_w = width;
	app_h = height;
	ctx->theme = cfg.theme;

	{
		int rpad = root_pad();
		int fmar = cfg.theme.frame_margin >= 0 ? cfg.theme.frame_margin
							: 0;
		struct BGTK_Widget *placeholder;
		struct BGTK_Widget *cols[3];

		sidebar = build_sidebar();
		panel_rule = bgtk_rule(ctx, BGTK_LIST_VERTICAL, 1,
				       (BGTK_Options){.margin = 0, .padding = 0});
		panel_rule->w = 1;
		panel_rule->data.rule.thickness = 1;
		panel_rule->data.rule.color = 0;
		panel_rule->flags |= BGTK_FLAG_EXPAND_Y;
		placeholder = bgtk_text(ctx, "Select a category",
			(BGTK_Options){.padding = theme_outer_pad(), .margin = 0});
		/* Size seeded in reflow_shell; FILL takes free width/height. */
		content_panel = bgtk_frame(ctx, placeholder, 80, 40,
			(BGTK_Options){.padding = 0, .margin = 0});
		content_panel->data.frame.border_w = 0;
		content_panel->flags |= BGTK_FLAG_FILL;
		cols[0] = sidebar;
		cols[1] = panel_rule;
		cols[2] = content_panel;
		/* margin set in reflow_shell to match edge_inset (button↔rule). */
		shell_row = bgtk_list(ctx, cols, 3,
			(BGTK_Options){.orientation = BGTK_LIST_HORIZONTAL,
				       .margin = (edge_inset() + 1) / 2,
				       .padding = 0});
		root_frame = bgtk_frame(ctx, shell_row, app_w, app_h,
			(BGTK_Options){.padding = rpad, .margin = fmar});
		ctx->root_widget = root_frame;
		reflow_shell();
	}
	current_page = 0;
	rebuild_content();
}

void settings_layout(void)
{
	if (!ctx || !root_frame)
		return;
	app_w = ctx->width;
	app_h = ctx->height;
	if (app_w < 200)
		app_w = 200;
	if (app_h < 120)
		app_h = 120;
	root_frame->w = app_w;
	root_frame->h = app_h;
	rebuild_content();
}

struct config *settings_get_config(void)
{
	return &cfg;
}

#ifdef SETTINGS_TEST_MODE
/* Test helpers: set image wallpaper + mode and rebuild (exercises preview). */
void settings_test_set_image_bg(const char *path, ImageMode mode)
{
	cfg.type = BG_IMAGE;
	cfg.mode = mode;
	if (path) {
		strncpy(cfg.path, path, MAX_PATH_LEN - 1);
		cfg.path[MAX_PATH_LEN - 1] = '\0';
	}
	rebuild_content();
}

void settings_test_toggle_bg_mode(void)
{
	toggle_bg_mode(NULL);
}
#endif

/* ------------------------------------------------------------------ */
/* Real BGCE app main (Linux only)                                     */
/* ------------------------------------------------------------------ */

#ifndef SETTINGS_TEST_MODE

/*
 * Load full ~/.config/bgce.conf into settings state:
 *   [background] type/color/path/mode
 *   [cursors]    theme
 *   [shortcuts]  combo = action
 * Matches BGCE's load_config sections (see bgce/config.c).
 */
static void load_bgce_config(struct config *c)
{
	const char *home = getenv("HOME");
	char path[512], line[1024], section[64] = "";
	FILE *f;
	char key[64], value[MAX_PATH_LEN];
	char *valp;

	if (!c || !home || !home[0])
		return;
	snprintf(path, sizeof(path), "%s/.config/bgce.conf", home);
	f = fopen(path, "r");
	if (!f) {
		bgtk_log("no %s; keeping bgtk defaults + builtin shortcuts",
			 path);
		return;
	}

	/* Reset shortcuts so file entries replace defaults when present. */
	shortcuts_loaded = 0;
	shortcut_row_count = 0;
	cursor_theme[0] = '\0';

	while (fgets(line, sizeof(line), f)) {
		char *trimmed = line;
		while (*trimmed == ' ' || *trimmed == '\t')
			trimmed++;
		if (*trimmed == '\0' || *trimmed == '#' || *trimmed == ';' ||
		    *trimmed == '\n')
			continue;
		if (*trimmed == '[') {
			char *end = strchr(trimmed, ']');
			section[0] = '\0';
			if (end &&
			    (size_t)(end - trimmed - 1) < sizeof(section)) {
				memcpy(section, trimmed + 1,
				       (size_t)(end - trimmed - 1));
				section[end - trimmed - 1] = '\0';
			}
			continue;
		}
		/* Accept "key = value" and "key=value" like BGCE. */
		if (sscanf(trimmed, "%63s = %511[^\n]", key, value) != 2 &&
		    sscanf(trimmed, "%63[^=]=%511[^\n]", key, value) != 2)
			continue;
		{
			char *kp = key;
			while (*kp == ' ' || *kp == '\t')
				kp++;
			if (kp != key)
				memmove(key, kp, strlen(kp) + 1);
			/* strip trailing spaces from key when key=value form */
			{
				char *ke = key + strlen(key);
				while (ke > key &&
				       (ke[-1] == ' ' || ke[-1] == '\t'))
					*--ke = '\0';
			}
		}
		valp = value;
		while (*valp == ' ' || *valp == '\t')
			valp++;
		{
			char *ve = valp + strlen(valp);
			while (ve > valp &&
			       (ve[-1] == '\n' || ve[-1] == '\r' ||
				ve[-1] == ' ' || ve[-1] == '\t'))
				*--ve = '\0';
		}

		if (strcmp(section, "background") == 0) {
			if (strcmp(key, "type") == 0) {
				if (strcasecmp(valp, "image") == 0)
					c->type = BG_IMAGE;
				else if (strcasecmp(valp, "color") == 0)
					c->type = BG_COLOR;
			} else if (strcmp(key, "color") == 0) {
				c->color = parse_color_input(valp);
			} else if (strcmp(key, "path") == 0) {
				strncpy(c->path, valp, MAX_PATH_LEN - 1);
				c->path[MAX_PATH_LEN - 1] = '\0';
				/* Non-empty path implies image wallpaper. */
				if (c->path[0])
					c->type = BG_IMAGE;
			} else if (strcmp(key, "mode") == 0) {
				if (strcasecmp(valp, "scaled") == 0)
					c->mode = IMAGE_SCALED;
				else if (strcasecmp(valp, "tiled") == 0)
					c->mode = IMAGE_TILED;
			}
		} else if (strcmp(section, "cursors") == 0) {
			if (strcmp(key, "theme") == 0) {
				strncpy(cursor_theme, valp, MAX_PATH_LEN - 1);
				cursor_theme[MAX_PATH_LEN - 1] = '\0';
			}
		} else if (strcmp(section, "shortcuts") == 0) {
			char label[96];
			if (shortcut_row_count >= MAX_SHORTCUT_ROWS)
				continue;
			if (!shortcuts_loaded) {
				shortcut_row_count = 0;
				shortcuts_loaded = 1;
			}
			shortcut_label_from_action(valp, label, sizeof(label));
			shortcut_set_row(shortcut_row_count, label, key, valp);
			shortcut_row_count++;
		}
	}
	fclose(f);

	if (!shortcuts_loaded)
		load_shortcuts_defaults();
	if (c->type == BG_IMAGE && !c->path[0])
		bgtk_log("bgce type=image but path empty (keep Image UI)");
	bgtk_log("bgce conf: bg=%s path='%s' mode=%s cursor='%s' shortcuts=%d",
		 c->type == BG_IMAGE ? "image" : "color",
		 c->path[0] ? c->path : "(none)",
		 c->mode == IMAGE_SCALED ? "scaled" : "tiled",
		 cursor_theme[0] ? cursor_theme : "(none)",
		 shortcut_row_count);
}

int main(void)
{
	setvbuf(stderr, NULL, _IONBF, 0);
	bgtk_log_open("settings");
	bgtk_log("starting settings");

	int conn = bgce_connect();
	if (conn < 0) {
		bgtk_log_errno("bgce_connect (is bgce running?)");
		return 1;
	}
	bgtk_log("bgce_connect ok fd=%d", conn);

	struct ServerInfo info;
	if (bgce_get_server_info(conn, &info) != 0) {
		bgtk_log("bgce_get_server_info failed (continuing)");
	} else {
		bgtk_log("server display %ux%u", info.width, info.height);
		if (info.width > 0 && info.height > 0) {
			screen_aspect_w = (int)info.width;
			screen_aspect_h = (int)info.height;
		}
	}

	struct BufferRequest req = { .width = SETTINGS_W, .height = SETTINGS_H };
	void *buf = bgce_get_buffer(conn, req);
	if (!buf) {
		bgtk_log("bgce_get_buffer %dx%d failed", SETTINGS_W, SETTINGS_H);
		bgce_disconnect(conn);
		return 1;
	}
	bgtk_log("bgce_get_buffer ok %p", buf);

	struct BGTK_Context *c = bgtk_init(conn, buf, SETTINGS_W, SETTINGS_H);
	if (!c) {
		bgtk_log("bgtk_init failed — check fonts / FreeType");
		bgce_disconnect(conn);
		return 1;
	}

	struct config config;
	parse_config(&config);
	bgtk_log("bgtk.conf loaded; overlaying bgce.conf");
	/* Full BGCE file: background + cursors + shortcuts. */
	load_bgce_config(&config);
	bgtk_log("building UI %dx%d", SETTINGS_W, SETTINGS_H);
	settings_build_ui(c, &config, SETTINGS_W, SETTINGS_H);
	bgtk_log("UI ready; entering main loop");
	bgtk_log_flush();

	struct BGCEMessage msg;
	while (bgce_recv_msg(conn, &msg) > 0) {
		if (msg.type == MSG_INPUT_EVENT) {
			struct InputEvent *ev = &msg.data.input_event;
			/* Ignore pure pointer motion — redrawing every move
			 * freezes the UI under hover. */
			if (ev->type == EV_REL || ev->type == EV_ABS)
				continue;
			bgtk_update_modifiers(c, *ev);
			if (bgtk_is_app_quit_event(c, *ev))
				break;
			if (bgtk_handle_input_event(c, *ev))
				bgtk_draw_widgets(c);
		} else if (msg.type == MSG_FOCUS_CHANGE) {
			bgtk_set_window_focus(c, msg.data.focus_event.state);
		} else if (msg.type == MSG_BUFFER_CHANGE) {
			if (bgtk_handle_buffer_change(c, &msg.data.buffer_reply) == 0)
				settings_layout();
		}
	}

	bgtk_destroy(c);
	bgce_disconnect(conn);
	return 0;
}

#endif /* SETTINGS_TEST_MODE */