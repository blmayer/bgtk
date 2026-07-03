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

static struct BGTK_Widget *sidebar_list; /* the list widget inside the scrollable */
static struct BGTK_Widget *sidebar;
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

/* Shortcut inputs (4 rows) */
static struct BGTK_Widget *shortcut_inputs[4];

static struct BGTK_Widget *font_size_input;

static struct BGTK_Widget *theme_bg_input;
static struct BGTK_Widget *theme_btn_input;
static struct BGTK_Widget *theme_btn_text_input;
static struct BGTK_Widget *theme_frame_border_input;
static struct BGTK_Widget *theme_btn_border_input;
static struct BGTK_Widget *theme_input_border_input;
static struct BGTK_Widget *theme_frame_color_input;

/* Font dropdown state: 0=closed, 1=open */
static int font_dropdown_open;
static char **font_list_cache;
static int font_list_count;

/* Shortcut names/defaults */
static const char *shortcut_actions[4] = {
	"Screenshot", "Close window", "Switch window", "Terminal"
};
static char shortcut_keys[4][64] = {
	"SysRq", "Super+Q", "Alt+Tab", "Super+Enter"
};

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

static void apply_background(void *userdata)
{
	(void)userdata;
	if (bg_color_input)
		cfg.color = parse_color_input(bg_color_input->data.text_input.text);
	if (bg_path_input && bg_path_input->data.text_input.text[0])
		strncpy(cfg.path, bg_path_input->data.text_input.text, MAX_PATH_LEN - 1);
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

static void apply_cursor(void *userdata)
{
	(void)userdata;
	/* Cursor path saved for future BGCE integration */
	write_config(&cfg);
	rebuild_content();
}

static void apply_shortcuts(void *userdata)
{
	(void)userdata;
	for (int i = 0; i < 4; i++) {
		if (shortcut_inputs[i] && shortcut_inputs[i]->data.text_input.text[0])
			strncpy(shortcut_keys[i], shortcut_inputs[i]->data.text_input.text,
				sizeof(shortcut_keys[0]) - 1);
	}
	/* Shortcuts are stored in memory; would be persisted via BGCE config */
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
	write_config(&cfg);
	rebuild_content();
}

/* ------------------------------------------------------------------ */
/* Font dropdown                                                       */
/* ------------------------------------------------------------------ */

static void font_select_cb(void *userdata)
{
	int idx = (int)(intptr_t)userdata;
	if (idx >= 0 && idx < font_list_count) {
		strncpy(cfg.font_path, font_list_cache[idx], MAX_PATH_LEN - 1);
		cfg.font_path[MAX_PATH_LEN - 1] = '\0';
	}
	font_dropdown_open = 0;
	rebuild_content();
}

static void toggle_font_dropdown(void *userdata)
{
	(void)userdata;
	font_dropdown_open = !font_dropdown_open;
	rebuild_content();
}

/* ------------------------------------------------------------------ */
/* HTML page builders                                                  */
/* ------------------------------------------------------------------ */

static char *build_background_html(void)
{
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

	pos += snprintf(buf + pos, 4096 - pos,
		"</table>"
		"<p>Preview:</p>"
		"<div padding=\"2\"></div>"
		"<div><button>Apply</button></div>"
		"</body></html>");
	return buf;
}

static char *build_cursor_html(void)
{
	char **cursors = NULL;
	int ncursors = scan_cursors(&cursors);

	int buflen = 4096 + ncursors * 256;
	char *buf = malloc(buflen);
	int pos = 0;

	pos += snprintf(buf + pos, buflen - pos,
		"<html><body>"
		"<table>"
		"<tr><td>Custom path</td><td><input type=\"text\" value=\"\" width=\"260\" /></td></tr>"
		"</table>"
		"<p>Available themes:</p>"
		"<ul>");

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
	char *buf = malloc(4096);
	snprintf(buf, 4096,
		"<html><body>"
		"<table>"
		"<tr><th>Action</th><th>Key Binding</th></tr>"
		"<tr><td>%s</td><td><input type=\"text\" value=\"%s\" width=\"140\" /></td></tr>"
		"<tr><td>%s</td><td><input type=\"text\" value=\"%s\" width=\"140\" /></td></tr>"
		"<tr><td>%s</td><td><input type=\"text\" value=\"%s\" width=\"140\" /></td></tr>"
		"<tr><td>%s</td><td><input type=\"text\" value=\"%s\" width=\"140\" /></td></tr>"
		"</table>"
		"<div><button>Apply</button></div>"
		"</body></html>",
		shortcut_actions[0], shortcut_keys[0],
		shortcut_actions[1], shortcut_keys[1],
		shortcut_actions[2], shortcut_keys[2],
		shortcut_actions[3], shortcut_keys[3]);
	return buf;
}

static char *build_font_html(void)
{
	/* Extract display name from current font path */
	const char *cur_name = cfg.font_path;
	const char *slash = strrchr(cfg.font_path, '/');
	if (slash) cur_name = slash + 1;
	if (!cfg.font_path[0]) cur_name = "(default)";

	int buflen = 8192 + font_list_count * 256;
	char *buf = malloc(buflen);
	int pos = 0;

	pos += snprintf(buf + pos, buflen - pos,
		"<html><body>"
		"<table>"
		"<tr><td>Font</td><td><button>%s</button></td></tr>"
		"<tr><td>Size</td><td><input type=\"text\" value=\"%d\" width=\"60\" /></td></tr>"
		"</table>",
		cur_name, cfg.font_size);

	/* Dropdown list (only when open) */
	if (font_dropdown_open) {
		pos += snprintf(buf + pos, buflen - pos, "<ul>");
		int show = font_list_count > 50 ? 50 : font_list_count;
		for (int i = 0; i < show; i++) {
			const char *name = strrchr(font_list_cache[i], '/');
			name = name ? name + 1 : font_list_cache[i];
			pos += snprintf(buf + pos, buflen - pos,
				"<li><button>%s</button></li>", name);
		}
		pos += snprintf(buf + pos, buflen - pos, "</ul>");
	}

	/* Preview */
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
	char bg[16], btn[16], btxt[16], fbc[16];
	format_hex_color(cfg.theme.background, bg, sizeof(bg));
	format_hex_color(cfg.theme.button, btn, sizeof(btn));
	format_hex_color(cfg.theme.button_text, btxt, sizeof(btxt));
	format_hex_color(cfg.theme.frame_border_color, fbc, sizeof(fbc));

	char *buf = malloc(4096);
	snprintf(buf, 4096,
		"<html><body>"
		"<table>"
		"<tr><td>Background</td><td><input type=\"text\" value=\"%s\" width=\"100\" /></td></tr>"
		"<tr><td>Button</td><td><input type=\"text\" value=\"%s\" width=\"100\" /></td></tr>"
		"<tr><td>Button text</td><td><input type=\"text\" value=\"%s\" width=\"100\" /></td></tr>"
		"<tr><td>Frame border size</td><td><input type=\"text\" value=\"%u\" width=\"60\" /></td></tr>"
		"<tr><td>Button border size</td><td><input type=\"text\" value=\"%u\" width=\"60\" /></td></tr>"
		"<tr><td>Input border size</td><td><input type=\"text\" value=\"%u\" width=\"60\" /></td></tr>"
		"<tr><td>Frame border color</td><td><input type=\"text\" value=\"%s\" width=\"100\" /></td></tr>"
		"</table>"
		"<div><button>Apply</button></div>"
		"</body></html>",
		bg, btn, btxt,
		cfg.theme.frame_border_size,
		cfg.theme.button_border_size,
		cfg.theme.input_border_size,
		fbc);
	return buf;
}

/* ------------------------------------------------------------------ */
/* Background preview: draw a colored/image rect into the content      */
/* ------------------------------------------------------------------ */

static void add_bg_preview(struct BGTK_Widget *page, int panel_w)
{
	/* Build a preview widget: a frame filled with the background color */
	int pw = panel_w - 40;
	if (pw < 60) pw = 60;
	int ph = 60;

	struct BGTK_Widget *preview;
	if (cfg.type == BG_IMAGE && cfg.path[0] && access(cfg.path, R_OK) == 0) {
		preview = bgtk_image(ctx, cfg.path, pw, ph, (BGTK_Options){.padding = 0, .margin = 4});
	} else {
		/* Color preview: a frame filled with bg color */
		struct BGTK_Widget *inner = bgtk_text(ctx, " ", (BGTK_Options){0});
		preview = bgtk_frame(ctx, inner, pw, ph, (BGTK_Options){.padding = 0, .margin = 4});
		preview->data.frame.border_w = 1;
		preview->data.frame.border_color = BGTK_COLOR_BLACK;
		/* We'll draw with the configured color by setting the frame bg
		 * via a custom approach: use the frame, then on draw the bg rect
		 * will use theme.background. We'll fake it by temporarily overriding.
		 * Simpler: just draw a colored rect via an image widget with solid pixels. */
		uint32_t *pix = malloc(pw * ph * sizeof(uint32_t));
		uint32_t c = cfg.color | 0xFF000000;
		for (int i = 0; i < pw * ph; i++) pix[i] = c;
		inner->type = BGTK_WIDGET_IMAGE;
		inner->data.image.pixels = pix;
		inner->data.image.img_w = pw;
		inner->data.image.img_h = ph;
		inner->w = pw;
		inner->h = ph;
	}

	/* Find the scrollable and insert preview into its items before the Apply button */
	/* The page is a frame->scrollable->list. We append preview to the list. */
	if (!page) return;
	struct BGTK_Widget *scroll = page->data.frame.child;
	if (!scroll || scroll->type != BGTK_WIDGET_SCROLLABLE) return;
	if (scroll->data.scrollable.widget_count < 1) return;
	struct BGTK_Widget *list = scroll->data.scrollable.items[0];
	if (!list || list->type != BGTK_WIDGET_LIST) return;

	/* Insert preview before the last item (Apply button) */
	int n = list->data.list_widget.widget_count;
	struct BGTK_Widget **new_items = malloc((n + 1) * sizeof(struct BGTK_Widget *));
	for (int i = 0; i < n - 1; i++)
		new_items[i] = list->data.list_widget.items[i];
	new_items[n - 1] = preview;
	new_items[n] = list->data.list_widget.items[n - 1]; /* Apply button */
	free(list->data.list_widget.items);
	list->data.list_widget.items = new_items;
	list->data.list_widget.widget_count = n + 1;
}

/* ------------------------------------------------------------------ */
/* Rebuild the content panel for current_page                          */
/* ------------------------------------------------------------------ */

static void rebuild_content(void)
{
	bg_color_input = bg_path_input = NULL;
	cursor_path_input = NULL;
	for (int i = 0; i < 4; i++) shortcut_inputs[i] = NULL;
	font_size_input = NULL;
	theme_bg_input = theme_btn_input = theme_btn_text_input = NULL;
	theme_frame_border_input = theme_btn_border_input = NULL;
	theme_input_border_input = theme_frame_color_input = NULL;

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

	int panel_w = app_w - sidebar->w - 20;
	int panel_h = app_h - 16;

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
		add_bg_preview(page, panel_w);
		break;
	}
	case 1: { /* Cursor: input(0)=custom path, buttons=cursor themes..., last=Apply */
		cursor_path_input = get_input(page, 0);
		/* Find total button count; last is Apply */
		int bi = 0;
		while (get_button(page, bi)) bi++;
		if (bi > 0) {
			struct BGTK_Widget *ab = get_button(page, bi - 1);
			if (ab) ab->data.button.callback = apply_cursor;
		}
		break;
	}
	case 2: { /* Shortcuts: inputs 0-3, button 0=Apply */
		for (int i = 0; i < 4; i++)
			shortcut_inputs[i] = get_input(page, i);
		struct BGTK_Widget *b = get_button(page, 0);
		if (b) b->data.button.callback = apply_shortcuts;
		break;
	}
	case 3: { /* Font: button(0)=dropdown toggle, then dropdown items, then Apply */
		struct BGTK_Widget *b = get_button(page, 0);
		if (b) b->data.button.callback = toggle_font_dropdown;
		font_size_input = get_input(page, 0);

		if (font_dropdown_open) {
			int show = font_list_count > 50 ? 50 : font_list_count;
			for (int i = 0; i < show; i++) {
				struct BGTK_Widget *fb = get_button(page, 1 + i);
				if (fb) {
					fb->data.button.callback = font_select_cb;
					fb->data.button.cb_data = (void *)(intptr_t)i;
				}
			}
			struct BGTK_Widget *ab = get_button(page, 1 + show);
			if (ab) ab->data.button.callback = apply_font;
		} else {
			struct BGTK_Widget *ab = get_button(page, 1);
			if (ab) ab->data.button.callback = apply_font;
		}
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

static void rebuild_sidebar(void)
{
	int sidebar_w = 140;
	struct BGTK_Widget **btns = malloc(NUM_PAGES * sizeof(struct BGTK_Widget *));

	for (int i = 0; i < NUM_PAGES; i++) {
		/* Selected page gets inverted colors via header_level trick */
		char label[64];
		if (i == current_page)
			snprintf(label, sizeof(label), "> %s", page_names[i]);
		else
			snprintf(label, sizeof(label), "  %s", page_names[i]);

		struct BGTK_Widget *lbl = bgtk_text(ctx, label, (BGTK_Options){.padding = 2});

		/* Use header_level = 10 (fuchsia/bold) for selected item */
		if (i == current_page)
			lbl->data.text.header_level = 10;

		btns[i] = bgtk_button(ctx, lbl, page_cb, (void *)(intptr_t)i,
			(BGTK_Options){.padding = 6, .margin = 2});
		btns[i]->w = sidebar_w - 8;
		if (i == current_page)
			btns[i]->data.button.bg_override = 0xFF505060;
	}

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

	for (int i = 0; i < NUM_PAGES; i++) {
		char label[64];
		if (i == current_page)
			snprintf(label, sizeof(label), "> %s", page_names[i]);
		else
			snprintf(label, sizeof(label), "  %s", page_names[i]);

		struct BGTK_Widget *lbl = bgtk_text(ctx, label, (BGTK_Options){.padding = 2});
		if (i == current_page)
			lbl->data.text.header_level = 10;

		btns[i] = bgtk_button(ctx, lbl, page_cb, (void *)(intptr_t)i,
			(BGTK_Options){.padding = 6, .margin = 2});
		btns[i]->w = sidebar_w - 8;
		if (i == current_page)
			btns[i]->data.button.bg_override = 0xFF505060;
	}

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

	sidebar = build_sidebar();

	int panel_w = app_w - sidebar->w - 20;
	int panel_h = app_h - 16;

	struct BGTK_Widget *placeholder = bgtk_text(ctx, "Select a category",
		(BGTK_Options){.padding = 8, .margin = 4});
	content_panel = bgtk_frame(ctx, placeholder, panel_w, panel_h,
		(BGTK_Options){.padding = 2, .margin = 2});
	content_panel->data.frame.border_w = 1;
	content_panel->data.frame.border_color = BGTK_COLOR_GRAY;

	struct BGTK_Widget *cols[2] = { sidebar, content_panel };
	struct BGTK_Widget *row = bgtk_list(ctx, cols, 2,
		(BGTK_Options){.orientation = BGTK_LIST_HORIZONTAL, .margin = 2});

	root_frame = bgtk_frame(ctx, row, app_w, app_h,
		(BGTK_Options){.padding = 2, .margin = 0});

	ctx->root_widget = root_frame;
	current_page = 0;
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

int main(void)
{
	int conn = bgce_connect();
	if (conn < 0) {
		fprintf(stderr, "settings: can't connect to BGCE\n");
		return 1;
	}

	struct ServerInfo info;
	bgce_get_server_info(conn, &info);

	struct BufferRequest req = { .width = 700, .height = 480 };
	void *buf = bgce_get_buffer(conn, req);
	if (!buf) {
		fprintf(stderr, "settings: can't get buffer\n");
		bgce_disconnect(conn);
		return 1;
	}

	struct BGTK_Context *c = bgtk_init(conn, buf, 700, 480);
	if (!c) {
		bgce_disconnect(conn);
		return 1;
	}

	struct config config;
	parse_config(&config);
	settings_build_ui(c, &config, 700, 480);

	struct BGCEMessage msg;
	while (bgce_recv_msg(conn, &msg) > 0) {
		if (msg.type == MSG_INPUT_EVENT) {
			bgtk_handle_input_event(c, msg.data.input_event);
			bgtk_draw_widgets(c);
			bgce_draw(conn);
		} else if (msg.type == MSG_FOCUS_CHANGE) {
			bgtk_set_window_focus(c, msg.data.focus_event.state);
			bgce_draw(conn);
		}
	}

	bgtk_destroy(c);
	bgce_disconnect(conn);
	return 0;
}

#endif /* SETTINGS_TEST_MODE */