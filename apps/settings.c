/* apps/settings.c
 *
 * BGCE + BGTK System Settings application.
 * GTK theme-chooser style: sidebar list on the left, content panel on the right.
 *
 * Sections:
 *   - Background: type/mode toggles, color input, image path, preview
 *   - Cursor:     scan system cursor dirs, custom path, select cursor theme
 *   - Shortcuts:  editable BGCE key bindings
 *   - Font:       dropdown selector, size, preview
 *   - Theme:      all BGTK_Theme color/size options
 *
 * Uses bgtk_html_parse_inline() to build each page, then wires callbacks.
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
#include "html.h"
#include "config.h"
#include "internal.h"

/* ------------------------------------------------------------------ */
/* Global state                                                        */
/* ------------------------------------------------------------------ */

static struct BGTK_Context *ctx;
static struct config cfg;

static int app_w = 700;
static int app_h = 480;
/* Desktop aspect for wallpaper preview (from BGCE server or 16:9). */
static int screen_aspect_w = 16;
static int screen_aspect_h = 9;

static struct BGTK_Widget *sidebar_list; /* the list widget inside the scrollable */
static struct BGTK_Widget *sidebar;
static struct BGTK_Widget *panel_rule; /* vertical divider between sidebar + content */
static struct BGTK_Widget *content_panel;
static struct BGTK_Widget *root_frame;

static int current_page = 0;
#define NUM_PAGES 5

static const char *page_names[NUM_PAGES] = {
	"Background", "Cursor", "Shortcuts", "Font", "Theme"
};

/* Cached widget pointers for active page */
static struct BGTK_Widget *bg_color_input;
static struct BGTK_Widget *bg_path_input;

static struct BGTK_Widget *cursor_path_input;
/* [cursors] theme = from ~/.config/bgce.conf (name or path). */
static char cursor_theme[MAX_PATH_LEN];

#define MAX_SHORTCUT_ROWS 24
static struct BGTK_Widget *shortcut_inputs[MAX_SHORTCUT_ROWS];
static int shortcut_row_count;

static struct BGTK_Widget *font_size_input;

static struct BGTK_Widget *theme_bg_input;
static struct BGTK_Widget *theme_btn_input;
static struct BGTK_Widget *theme_btn_text_input;
static struct BGTK_Widget *theme_frame_border_input;
static struct BGTK_Widget *theme_btn_border_input;
static struct BGTK_Widget *theme_input_border_input;
static struct BGTK_Widget *theme_frame_color_input;
static struct BGTK_Widget *theme_focus_input;
static struct BGTK_Widget *theme_focus_bg_input;
static struct BGTK_Widget *theme_highlight_input;

/* Font dropdown: open for one role (sans/mono/serif) at a time. */
static int font_dropdown_open;
static int font_pick_role; /* BGTK_FONT_SANS / MONO / SERIF */
static char **font_list_cache;
static int font_list_count;

/* Shortcuts shown in UI (loaded from ~/.config/bgce.conf when present). */
static char shortcut_labels[MAX_SHORTCUT_ROWS][96];
static char shortcut_actions[MAX_SHORTCUT_ROWS][96];
static char shortcut_keys[MAX_SHORTCUT_ROWS][64];
static int shortcuts_loaded;

/* Human label for the Action column (config still stores type:value). */
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
	shortcut_row_count = 4;
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
	if (!home || !home[0])
		return;
	snprintf(path, sizeof(path), "%s/.config/bgce.conf", home);
	f = fopen(path, "r");
	if (!f)
		return;

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

static int scan_fonts(char ***out)
{
	static const char *dirs[] = {
#ifdef __linux__
		"/share/fonts/truetype",
		"/share/fonts/TTF",
		"/share/fonts/opentype",
		"/usr/share/fonts/truetype",
		"/usr/share/fonts/TTF",
		"/usr/share/fonts/opentype",
		"/usr/local/share/fonts",
#endif
#ifdef __APPLE__
		"/System/Library/Fonts",
		"/Library/Fonts",
#endif
		NULL
	};

	int cap = 64, n = 0;
	char **list = malloc(cap * sizeof(char *));

	for (int i = 0; dirs[i]; i++) {
		char **sub = NULL;
		int sn = scan_dir(dirs[i], &sub, 0);
		for (int j = 0; j < sn; j++) {
			int len = strlen(sub[j]);
			if (len > 4 && (!strcasecmp(sub[j] + len - 4, ".ttf") ||
					!strcasecmp(sub[j] + len - 4, ".otf"))) {
				if (n >= cap) {
					cap *= 2;
					list = realloc(list, cap * sizeof(char *));
				}
				char full[1024];
				snprintf(full, sizeof(full), "%s/%s", dirs[i], sub[j]);
				list[n++] = strdup(full);
			}
			free(sub[j]);
		}
		free(sub);
	}
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
/* Widget tree finder helpers                                          */
/* ------------------------------------------------------------------ */

static struct BGTK_Widget *find_nth_input(struct BGTK_Widget *w, int *counter, int target)
{
	if (!w)
		return NULL;
	if (w->type == BGTK_WIDGET_TEXT_INPUT) {
		if (*counter == target)
			return w;
		(*counter)++;
	}
	switch (w->type) {
	case BGTK_WIDGET_FRAME:
		return find_nth_input(w->data.frame.child, counter, target);
	case BGTK_WIDGET_LIST:
		for (int i = 0; i < w->data.list_widget.widget_count; i++) {
			struct BGTK_Widget *r = find_nth_input(w->data.list_widget.items[i], counter, target);
			if (r) return r;
		}
		break;
	case BGTK_WIDGET_SCROLLABLE:
		for (int i = 0; i < w->data.scrollable.widget_count; i++) {
			struct BGTK_Widget *r = find_nth_input(w->data.scrollable.items[i], counter, target);
			if (r) return r;
		}
		break;
	default:
		break;
	}
	return NULL;
}

static struct BGTK_Widget *get_input(struct BGTK_Widget *root, int idx)
{
	int c = 0;
	return find_nth_input(root, &c, idx);
}

static struct BGTK_Widget *find_nth_button(struct BGTK_Widget *w, int *counter, int target)
{
	if (!w)
		return NULL;
	if (w->type == BGTK_WIDGET_BUTTON) {
		if (*counter == target)
			return w;
		(*counter)++;
		return NULL;
	}
	switch (w->type) {
	case BGTK_WIDGET_FRAME:
		return find_nth_button(w->data.frame.child, counter, target);
	case BGTK_WIDGET_LIST:
		for (int i = 0; i < w->data.list_widget.widget_count; i++) {
			struct BGTK_Widget *r = find_nth_button(w->data.list_widget.items[i], counter, target);
			if (r) return r;
		}
		break;
	case BGTK_WIDGET_SCROLLABLE:
		for (int i = 0; i < w->data.scrollable.widget_count; i++) {
			struct BGTK_Widget *r = find_nth_button(w->data.scrollable.items[i], counter, target);
			if (r) return r;
		}
		break;
	default:
		break;
	}
	return NULL;
}

static struct BGTK_Widget *get_button(struct BGTK_Widget *root, int idx)
{
	int c = 0;
	return find_nth_button(root, &c, idx);
}

/* ------------------------------------------------------------------ */
/* Forward declarations                                                */
/* ------------------------------------------------------------------ */

static void rebuild_content(void);
static void rebuild_sidebar(void);

/* ------------------------------------------------------------------ */
/* Page switching                                                      */
/* ------------------------------------------------------------------ */

static void page_cb(void *userdata)
{
	current_page = (int)(intptr_t)userdata;
	font_dropdown_open = 0;
	rebuild_sidebar();
	rebuild_content();
}

/* ------------------------------------------------------------------ */
/* Apply callbacks                                                     */
/* ------------------------------------------------------------------ */

static uint32_t parse_color_input(const char *text)
{
	if (!text || text[0] != '#' || strlen(text) < 7)
		return 0xFF000000;
	unsigned r, g, b;
	if (sscanf(text + 1, "%02x%02x%02x", &r, &g, &b) != 3)
		return 0xFF000000;
	return 0xFF000000 | (r << 16) | (g << 8) | b;
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
	rebuild_content();
}

static void toggle_bg_mode(void *userdata)
{
	(void)userdata;
	cfg.mode = (cfg.mode == IMAGE_TILED) ? IMAGE_SCALED : IMAGE_TILED;
	rebuild_content();
}

static void toggle_bg_type(void *userdata)
{
	(void)userdata;
	cfg.type = (cfg.type == BG_COLOR) ? BG_IMAGE : BG_COLOR;
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
	rebuild_content();
}

static void pick_cursor_theme(void *userdata)
{
	const char *name = (const char *)userdata;
	if (name && name[0]) {
		strncpy(cursor_theme, name, MAX_PATH_LEN - 1);
		cursor_theme[MAX_PATH_LEN - 1] = '\0';
	}
	rebuild_content();
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
		rebuild_content();
		return;
	}
	snprintf(path, sizeof(path), "%s/.config/bgce.conf", home);
	snprintf(tmp, sizeof(tmp), "%s/.config/bgce.conf.bgtk-tmp", home);
	in = fopen(path, "r");
	out = fopen(tmp, "w");
	if (!out) {
		if (in)
			fclose(in);
		rebuild_content();
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
	rebuild_content();
}

static void apply_font(void *userdata)
{
	(void)userdata;
	if (font_size_input) {
		int sz = atoi(font_size_input->data.text_input.text);
		if (sz > 0 && sz < 200)
			cfg.font_size = sz;
	}
	write_config(&cfg);
	rebuild_content();
}

static const char *font_basename(const char *path)
{
	const char *slash;
	if (!path || !path[0])
		return "(default)";
	slash = strrchr(path, '/');
	return slash ? slash + 1 : path;
}

static char *font_path_for_role(int role)
{
	if (role == BGTK_FONT_MONO)
		return cfg.font_mono_path;
	if (role == BGTK_FONT_SERIF)
		return cfg.font_serif_path;
	return cfg.font_sans_path;
}

static void apply_theme(void *userdata)
{
	(void)userdata;
	if (theme_bg_input)
		cfg.theme.background = parse_color_input(theme_bg_input->data.text_input.text);
	if (theme_btn_input)
		cfg.theme.button = parse_color_input(theme_btn_input->data.text_input.text);
	if (theme_btn_text_input)
		cfg.theme.button_text = parse_color_input(theme_btn_text_input->data.text_input.text);
	if (theme_frame_border_input)
		cfg.theme.frame_border_size = atoi(theme_frame_border_input->data.text_input.text);
	if (theme_btn_border_input)
		cfg.theme.button_border_size = atoi(theme_btn_border_input->data.text_input.text);
	if (theme_input_border_input)
		cfg.theme.input_border_size = atoi(theme_input_border_input->data.text_input.text);
	if (theme_frame_color_input)
		cfg.theme.frame_border_color = parse_color_input(theme_frame_color_input->data.text_input.text);
	if (theme_focus_input)
		cfg.theme.focus = parse_color_input(theme_focus_input->data.text_input.text);
	if (theme_focus_bg_input)
		cfg.theme.focus_bg = parse_color_input(theme_focus_bg_input->data.text_input.text);
	if (theme_highlight_input)
		cfg.theme.highlight = parse_color_input(theme_highlight_input->data.text_input.text);
	write_config(&cfg);
	if (ctx)
		ctx->theme = cfg.theme;
	rebuild_sidebar();
	rebuild_content();
}

/* ------------------------------------------------------------------ */
/* Font dropdown                                                       */
/* ------------------------------------------------------------------ */

static void font_select_cb(void *userdata)
{
	int idx = (int)(intptr_t)userdata;
	char *dst = font_path_for_role(font_pick_role);

	if (idx >= 0 && idx < font_list_count && dst) {
		strncpy(dst, font_list_cache[idx], MAX_PATH_LEN - 1);
		dst[MAX_PATH_LEN - 1] = '\0';
	}
	font_dropdown_open = 0;
	rebuild_content();
}

static void toggle_font_dropdown(void *userdata)
{
	int role = (int)(intptr_t)userdata;

	if (font_dropdown_open && font_pick_role == role)
		font_dropdown_open = 0;
	else {
		font_dropdown_open = 1;
		font_pick_role = role;
	}
	rebuild_content();
}

/* ------------------------------------------------------------------ */
/* HTML page builders                                                  */
/* ------------------------------------------------------------------ */

static char *build_background_html(void)
{
	/* Values come from load_bgce_config / cfg (desktop = bgce.conf). */
	char color_hex[16];
	format_hex_color(cfg.color, color_hex, sizeof(color_hex));

	char *buf = malloc(4096);
	int pos = 0;
	pos += snprintf(buf + pos, 4096 - pos,
		"<html><body>"
		"<table>"
		"<tr><td>Type</td><td><button>[ %s ]</button></td></tr>",
		cfg.type == BG_IMAGE ? "Image" : "Color");

	if (cfg.type == BG_COLOR) {
		pos += snprintf(buf + pos, 4096 - pos,
			"<tr><td>Color</td><td><input type=\"text\" value=\"%s\" width=\"120\" /></td></tr>",
			color_hex);
	} else {
		pos += snprintf(buf + pos, 4096 - pos,
			"<tr><td>Path</td><td><input type=\"text\" value=\"%s\" width=\"280\" /></td></tr>"
			"<tr><td>Mode</td><td><button>[ %s ]</button></td></tr>",
			cfg.path[0] ? cfg.path : "",
			cfg.mode == IMAGE_SCALED ? "Scaled" : "Tiled");
	}

	/* Preview inserted by add_bg_preview before Apply. */
	pos += snprintf(buf + pos, 4096 - pos,
		"</table>"
		"<div><button>Apply</button></div>"
		"</body></html>");
	return buf;
}

static char *build_cursor_html(void)
{
	char **cursors = NULL;
	int ncursors = scan_cursors(&cursors);
	const char *cur = cursor_theme[0] ? cursor_theme : "";

	int buflen = 4096 + ncursors * 256;
	char *buf = malloc(buflen);
	int pos = 0;

	pos += snprintf(buf + pos, buflen - pos,
		"<html><body>"
		"<table>"
		"<tr><td>Theme</td><td><input type=\"text\" value=\"%s\" width=\"260\" /></td></tr>"
		"</table>"
		"<p>Installed themes</p>"
		"<ul>",
		cur);

	if (ncursors == 0) {
		pos += snprintf(buf + pos, buflen - pos,
			"<li>No cursor themes found in system paths</li>");
	}
	for (int i = 0; i < ncursors; i++) {
		pos += snprintf(buf + pos, buflen - pos,
			"<li><button>%s</button></li>", cursors[i]);
		free(cursors[i]);
	}
	free(cursors);

	pos += snprintf(buf + pos, buflen - pos,
		"</ul>"
		"<div><button>Apply</button></div>"
		"</body></html>");

	return buf;
}

static char *build_shortcuts_html(void)
{
	char *buf;
	int buflen = 2048 + shortcut_row_count * 256;
	int pos = 0;
	int i;

	if (!shortcut_row_count)
		load_shortcuts_table();

	buf = malloc((size_t)buflen);
	if (!buf)
		return NULL;
	pos += snprintf(buf + pos, (size_t)(buflen - pos),
		"<html><body>"
		"<table>"
		"<tr><th>Action</th><th>Key binding</th></tr>");
	for (i = 0; i < shortcut_row_count; i++) {
		const char *label = shortcut_labels[i][0]
					    ? shortcut_labels[i]
					    : shortcut_actions[i];
		pos += snprintf(buf + pos, (size_t)(buflen - pos),
			"<tr><td>%s</td><td><input type=\"text\" value=\"%s\" "
			"width=\"160\" /></td></tr>",
			label, shortcut_keys[i]);
	}
	pos += snprintf(buf + pos, (size_t)(buflen - pos),
		"</table>"
		"<div><button>Apply</button></div>"
		"</body></html>");
	(void)pos;
	return buf;
}

static char *build_font_html(void)
{
	const char *sans_name = font_basename(cfg.font_sans_path);
	const char *mono_name = font_basename(cfg.font_mono_path);
	const char *serif_name = font_basename(cfg.font_serif_path);
	const char *open_label = "";
	int buflen = 8192 + font_list_count * 256;
	char *buf = malloc(buflen);
	int pos = 0;

	if (!buf)
		return NULL;
	if (font_dropdown_open) {
		if (font_pick_role == BGTK_FONT_MONO)
			open_label = " (picking mono)";
		else if (font_pick_role == BGTK_FONT_SERIF)
			open_label = " (picking serif)";
		else
			open_label = " (picking sans)";
	}

	pos += snprintf(buf + pos, buflen - pos,
		"<html><body>"
		"<table>"
		"<tr><td>Sans (UI)</td><td><button>%s</button></td></tr>"
		"<tr><td>Mono</td><td><button>%s</button></td></tr>"
		"<tr><td>Serif</td><td><button>%s</button></td></tr>"
		"<tr><td>Size</td><td><input type=\"text\" value=\"%d\" width=\"60\" /></td></tr>"
		"</table>",
		sans_name, mono_name, serif_name, cfg.font_size);

	/* Dropdown list for the active role (only when open). */
	if (font_dropdown_open) {
		pos += snprintf(buf + pos, buflen - pos, "<p>Fonts%s:</p><ul>",
				open_label);
		int show = font_list_count > 50 ? 50 : font_list_count;
		for (int i = 0; i < show; i++) {
			const char *name = strrchr(font_list_cache[i], '/');
			name = name ? name + 1 : font_list_cache[i];
			pos += snprintf(buf + pos, buflen - pos,
				"<li><button>%s</button></li>", name);
		}
		pos += snprintf(buf + pos, buflen - pos, "</ul>");
	}

	/* Preview (rendered with the active UI/sans face). */
	pos += snprintf(buf + pos, buflen - pos,
		"<p>Preview:</p>"
		"<p>The quick brown fox jumps over the lazy dog</p>"
		"<p>ABCDEFGHIJKLM 0123456789 !@#$%%</p>"
		"<div><button>Apply</button></div>"
		"</body></html>");

	return buf;
}

static char *build_theme_html(void)
{
	char bg[16], btn[16], btxt[16], fbc[16], foc[16], fbg[16], hi[16];
	format_hex_color(cfg.theme.background, bg, sizeof(bg));
	format_hex_color(cfg.theme.button, btn, sizeof(btn));
	format_hex_color(cfg.theme.button_text, btxt, sizeof(btxt));
	format_hex_color(cfg.theme.frame_border_color, fbc, sizeof(fbc));
	format_hex_color(cfg.theme.focus, foc, sizeof(foc));
	format_hex_color(cfg.theme.focus_bg, fbg, sizeof(fbg));
	format_hex_color(cfg.theme.highlight, hi, sizeof(hi));

	char *buf = malloc(6144);
	snprintf(buf, 6144,
		"<html><body>"
		"<table>"
		"<tr><td>Background</td><td><input type=\"text\" value=\"%s\" width=\"100\" /></td></tr>"
		"<tr><td>Button</td><td><input type=\"text\" value=\"%s\" width=\"100\" /></td></tr>"
		"<tr><td>Button text</td><td><input type=\"text\" value=\"%s\" width=\"100\" /></td></tr>"
		"<tr><td>Frame border size</td><td><input type=\"text\" value=\"%u\" width=\"60\" /></td></tr>"
		"<tr><td>Button border size</td><td><input type=\"text\" value=\"%u\" width=\"60\" /></td></tr>"
		"<tr><td>Input border size</td><td><input type=\"text\" value=\"%u\" width=\"60\" /></td></tr>"
		"<tr><td>Frame border color</td><td><input type=\"text\" value=\"%s\" width=\"100\" /></td></tr>"
		"<tr><td>Focus</td><td><input type=\"text\" value=\"%s\" width=\"100\" /></td></tr>"
		"<tr><td>Focus background</td><td><input type=\"text\" value=\"%s\" width=\"100\" /></td></tr>"
		"<tr><td>Highlight</td><td><input type=\"text\" value=\"%s\" width=\"100\" /></td></tr>"
		"</table>"
		"<div><button>Apply</button></div>"
		"</body></html>",
		bg, btn, btxt,
		cfg.theme.frame_border_size,
		cfg.theme.button_border_size,
		cfg.theme.input_border_size,
		fbc, foc, fbg, hi);
	return buf;
}

/* ------------------------------------------------------------------ */
/* Background preview: aspect-correct desktop wallpaper box            */
/* ------------------------------------------------------------------ */

/* Fit preview inside max_w x max_h using screen_aspect_w:h. */
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

static void add_bg_preview(struct BGTK_Widget *page, int panel_w, int panel_h)
{
	int pw, ph;
	struct BGTK_Widget *preview = NULL;
	struct BGTK_Widget *scroll;
	struct BGTK_Widget *list;
	int n, i, reserved, max_w, max_h, total_h, max_w_list;
	struct BGTK_Widget **new_items;

	if (!page)
		return;
	scroll = page->data.frame.child;
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

	/* Height already used by form rows + Apply — preview fills the rest. */
	reserved = 0;
	for (i = 0; i < n; i++) {
		struct BGTK_Widget *ch = list->data.list_widget.items[i];
		if (!ch)
			continue;
		reserved += ch->h + 2 * list->margin;
	}
	/* Panel chrome: content frame padding, scroll padding, preview margin. */
	max_h = panel_h - reserved - 28;
	if (max_h < 80)
		max_h = 80;
	max_w = panel_w - 28;
	if (max_w < 80)
		max_w = 80;
	preview_fit(max_w, max_h, &pw, &ph);

	if (cfg.type == BG_IMAGE && cfg.path[0] && access(cfg.path, R_OK) == 0) {
		preview = bgtk_image(ctx, cfg.path, pw, ph,
				     (BGTK_Options){.padding = 0, .margin = 4});
		if (preview) {
			preview->w = pw + 8;
			preview->h = ph + 8;
		}
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

	/* Insert preview before the last item (Apply button). */
	new_items = malloc((n + 1) * sizeof(struct BGTK_Widget *));
	if (!new_items)
		return;
	for (i = 0; i < n - 1; i++)
		new_items[i] = list->data.list_widget.items[i];
	new_items[n - 1] = preview;
	new_items[n] = list->data.list_widget.items[n - 1]; /* Apply */
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
/* Rebuild the content panel for current_page                          */
/* ------------------------------------------------------------------ */

static void rebuild_content(void)
{
	bg_color_input = bg_path_input = NULL;
	cursor_path_input = NULL;
	for (int i = 0; i < MAX_SHORTCUT_ROWS; i++)
		shortcut_inputs[i] = NULL;
	if (!shortcut_row_count)
		load_shortcuts_table();
	font_size_input = NULL;
	theme_bg_input = theme_btn_input = theme_btn_text_input = NULL;
	theme_frame_border_input = theme_btn_border_input = NULL;
	theme_input_border_input = theme_frame_color_input = NULL;
	theme_focus_input = theme_focus_bg_input = theme_highlight_input = NULL;

	/* Free old font cache only when leaving font page */
	if (current_page != 3 && font_list_cache) {
		for (int i = 0; i < font_list_count; i++)
			free(font_list_cache[i]);
		free(font_list_cache);
		font_list_cache = NULL;
		font_list_count = 0;
	}

	/* Pre-scan fonts if on font page and cache is empty */
	if (current_page == 3 && !font_list_cache)
		font_list_count = scan_fonts(&font_list_cache);

	int rule_w = panel_rule ? panel_rule->w : 0;
	int panel_w = app_w - (sidebar ? sidebar->w : 140) - rule_w - 16;
	int panel_h = app_h - 16;
	if (panel_w < 80)
		panel_w = 80;
	if (panel_h < 40)
		panel_h = 40;
	if (content_panel) {
		content_panel->w = panel_w;
		content_panel->h = panel_h;
	}
	if (sidebar) {
		sidebar->h = panel_h;
		if (sidebar->h < 40)
			sidebar->h = 40;
	}
	if (panel_rule) {
		panel_rule->h = panel_h;
		if (panel_rule->h < 40)
			panel_rule->h = 40;
	}

	char *html = NULL;
	switch (current_page) {
	case 0: html = build_background_html(); break;
	case 1: html = build_cursor_html(); break;
	case 2: html = build_shortcuts_html(); break;
	case 3: html = build_font_html(); break;
	case 4: html = build_theme_html(); break;
	}

	struct BGTK_Widget *page = bgtk_html_parse_inline(ctx, html, panel_w, panel_h);
	free(html);

	if (!page)
		page = bgtk_text(ctx, "Error loading page", (BGTK_Options){.padding = 8});

	/* Wire callbacks */
	switch (current_page) {
	case 0: { /* Background: type toggle(0), then type-dependent fields, then apply */
		struct BGTK_Widget *b;
		b = get_button(page, 0);
		if (b) b->data.button.callback = toggle_bg_type;
		if (cfg.type == BG_COLOR) {
			/* buttons: type(0), apply(1); inputs: color(0) */
			bg_color_input = get_input(page, 0);
			b = get_button(page, 1);
			if (b) b->data.button.callback = apply_background;
		} else {
			/* buttons: type(0), mode(1), apply(2); inputs: path(0) */
			bg_path_input = get_input(page, 0);
			b = get_button(page, 1);
			if (b) b->data.button.callback = toggle_bg_mode;
			b = get_button(page, 2);
			if (b) b->data.button.callback = apply_background;
		}
		add_bg_preview(page, panel_w, panel_h);
		break;
	}
	case 1: { /* Cursor: input(0)=theme, buttons=themes..., last=Apply */
		cursor_path_input = get_input(page, 0);
		int bi = 0;
		while (get_button(page, bi))
			bi++;
		/* Theme buttons (all but last) select; last is Apply. */
		for (int i = 0; i + 1 < bi; i++) {
			struct BGTK_Widget *tb = get_button(page, i);
			struct BGTK_Widget *lab;
			if (!tb)
				continue;
			lab = tb->data.button.label;
			if (lab && lab->type == BGTK_WIDGET_TEXT &&
			    lab->data.text.text) {
				tb->data.button.callback = pick_cursor_theme;
				tb->data.button.cb_data = lab->data.text.text;
			}
		}
		if (bi > 0) {
			struct BGTK_Widget *ab = get_button(page, bi - 1);
			if (ab)
				ab->data.button.callback = apply_cursor;
		}
		break;
	}
	case 2: { /* Shortcuts: one input per row, last button = Apply */
		for (int i = 0; i < shortcut_row_count && i < MAX_SHORTCUT_ROWS; i++)
			shortcut_inputs[i] = get_input(page, i);
		{
			struct BGTK_Widget *b = get_button(page, 0);
			if (b)
				b->data.button.callback = apply_shortcuts;
		}
		break;
	}
	case 3: { /* Font: sans/mono/serif toggles, optional list, Apply */
		struct BGTK_Widget *b;
		int bi;

		b = get_button(page, 0);
		if (b) {
			b->data.button.callback = toggle_font_dropdown;
			b->data.button.cb_data = (void *)(intptr_t)BGTK_FONT_SANS;
		}
		b = get_button(page, 1);
		if (b) {
			b->data.button.callback = toggle_font_dropdown;
			b->data.button.cb_data = (void *)(intptr_t)BGTK_FONT_MONO;
		}
		b = get_button(page, 2);
		if (b) {
			b->data.button.callback = toggle_font_dropdown;
			b->data.button.cb_data = (void *)(intptr_t)BGTK_FONT_SERIF;
		}
		font_size_input = get_input(page, 0);

		bi = 3; /* first button after the three role toggles */
		if (font_dropdown_open) {
			int show = font_list_count > 50 ? 50 : font_list_count;
			for (int i = 0; i < show; i++) {
				struct BGTK_Widget *fb = get_button(page, bi + i);
				if (fb) {
					fb->data.button.callback = font_select_cb;
					fb->data.button.cb_data =
						(void *)(intptr_t)i;
				}
			}
			bi += show;
		}
		b = get_button(page, bi);
		if (b)
			b->data.button.callback = apply_font;
		break;
	}
	case 4: { /* Theme */
		theme_bg_input = get_input(page, 0);
		theme_btn_input = get_input(page, 1);
		theme_btn_text_input = get_input(page, 2);
		theme_frame_border_input = get_input(page, 3);
		theme_btn_border_input = get_input(page, 4);
		theme_input_border_input = get_input(page, 5);
		theme_frame_color_input = get_input(page, 6);
		theme_focus_input = get_input(page, 7);
		theme_focus_bg_input = get_input(page, 8);
		theme_highlight_input = get_input(page, 9);
		struct BGTK_Widget *b = get_button(page, 0);
		if (b) b->data.button.callback = apply_theme;
		break;
	}
	}

	content_panel->data.frame.child = page;
	bgtk_draw_widgets(ctx);
}

/* ------------------------------------------------------------------ */
/* Sidebar: rebuild with highlight on selected page                    */
/* ------------------------------------------------------------------ */

/* Selected nav: highlight fill only (no "> " prefix). */
static struct BGTK_Widget *make_nav_button(int i, int sidebar_w)
{
	int selected = (i == current_page);
	BGTK_Options to = {.padding = 2};
	if (selected)
		to.text_style = BGTK_TEXT_BOLD;
	struct BGTK_Widget *lbl =
		bgtk_text(ctx, (char *)page_names[i], to);
	struct BGTK_Widget *btn = bgtk_button(ctx, lbl, page_cb,
		(void *)(intptr_t)i,
		(BGTK_Options){.padding = 6, .margin = 2});
	btn->w = sidebar_w - 8;
	if (selected) {
		uint32_t hi = ctx->theme.highlight ? ctx->theme.highlight
						   : 0xFF505060;
		btn->data.button.bg_override = hi;
	}
	return btn;
}

static void rebuild_sidebar(void)
{
	int sidebar_w = 140;
	struct BGTK_Widget **btns = malloc(NUM_PAGES * sizeof(struct BGTK_Widget *));

	for (int i = 0; i < NUM_PAGES; i++)
		btns[i] = make_nav_button(i, sidebar_w);

	sidebar_list = bgtk_list(ctx, btns, NUM_PAGES,
		(BGTK_Options){.orientation = BGTK_LIST_VERTICAL, .margin = 2});
	free(btns);

	/* Replace the scrollable's content */
	sidebar->data.scrollable.items[0] = sidebar_list;
	sidebar->data.scrollable.widget_count = 1;
}

static struct BGTK_Widget *build_sidebar(void)
{
	int sidebar_w = 140;
	struct BGTK_Widget **btns = malloc(NUM_PAGES * sizeof(struct BGTK_Widget *));

	for (int i = 0; i < NUM_PAGES; i++)
		btns[i] = make_nav_button(i, sidebar_w);

	sidebar_list = bgtk_list(ctx, btns, NUM_PAGES,
		(BGTK_Options){.orientation = BGTK_LIST_VERTICAL, .margin = 2});
	free(btns);

	struct BGTK_Widget *scroll = bgtk_scrollable(ctx, &sidebar_list, 1,
		(BGTK_Options){.padding = 2, .margin = 2});
	scroll->w = sidebar_w;
	scroll->h = app_h - 16;

	return scroll;
}

/* ------------------------------------------------------------------ */
/* Build the full UI                                                   */
/* ------------------------------------------------------------------ */

void settings_build_ui(struct BGTK_Context *c, struct config *config,
		       int width, int height)
{
	ctx = c;
	cfg = *config;
	app_w = width;
	app_h = height;
	font_dropdown_open = 0;
	/* Keep drawing theme in sync with the config we are editing. */
	ctx->theme = cfg.theme;

	sidebar = build_sidebar();
	panel_rule = bgtk_rule(ctx, BGTK_LIST_VERTICAL, 1,
			       (BGTK_Options){.margin = 4, .padding = 0});
	panel_rule->h = app_h - 16;

	int panel_w = app_w - sidebar->w - panel_rule->w - 16;
	int panel_h = app_h - 16;
	if (panel_w < 80)
		panel_w = 80;

	struct BGTK_Widget *placeholder = bgtk_text(ctx, "Select a category",
		(BGTK_Options){.padding = 8, .margin = 4});
	/* Borderless content frame; vertical rule separates sidebar. */
	content_panel = bgtk_frame(ctx, placeholder, panel_w, panel_h,
		(BGTK_Options){.padding = 2, .margin = 2});
	content_panel->data.frame.border_w = 0;

	struct BGTK_Widget *cols[3] = { sidebar, panel_rule, content_panel };
	struct BGTK_Widget *row = bgtk_list(ctx, cols, 3,
		(BGTK_Options){.orientation = BGTK_LIST_HORIZONTAL, .margin = 2});

	root_frame = bgtk_frame(ctx, row, app_w, app_h,
		(BGTK_Options){.padding = 2, .margin = 0});

	ctx->root_widget = root_frame;
	current_page = 0;
	rebuild_content();
}

/* Reflow chrome + page after a window resize (MSG_BUFFER_CHANGE / mock resize). */
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
	/* rebuild_content resizes sidebar/content_panel and re-parses HTML. */
	rebuild_content();
}

struct config *settings_get_config(void)
{
	return &cfg;
}

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

	struct BufferRequest req = { .width = 700, .height = 480 };
	void *buf = bgce_get_buffer(conn, req);
	if (!buf) {
		bgtk_log("bgce_get_buffer 700x480 failed");
		bgce_disconnect(conn);
		return 1;
	}
	bgtk_log("bgce_get_buffer ok %p", buf);

	struct BGTK_Context *c = bgtk_init(conn, buf, 700, 480);
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
	bgtk_log("building UI %dx%d", 700, 480);
	settings_build_ui(c, &config, 700, 480);
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