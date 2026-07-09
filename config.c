#include "config.h"

#include "bgtk.h"
#include "internal.h"

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>  // strcasecmp
#include <sys/stat.h>
#include <unistd.h>  // for access() in default font selection

// Helper: trim whitespace
static char *trim(char *str)
{
	while (isspace((unsigned char)*str)) {
		str++;
	}
	if (*str == 0) {
		return str;
	}

	char *end = str + strlen(str) - 1;
	while (end > str && isspace((unsigned char)*end)) {
		end--;
	}
	end[1] = '\0';

	return str;
}

// Parse hex color (#RRGGBB or #RRGGBBAA)
uint32_t parse_hex_color(const char *str)
{
	unsigned int r, g, b, a = 255;

	if (!str || str[0] != '#')
		return 0xFF000000;
	if (strlen(str) == 7) {
		if (sscanf(str + 1, "%02x%02x%02x", &r, &g, &b) != 3)
			return 0xFF000000;
	} else if (strlen(str) == 9) {
		if (sscanf(str + 1, "%02x%02x%02x%02x", &r, &g, &b, &a) != 4)
			return 0xFF000000;
	} else {
		return 0xFF000000;
	}
	return (a << 24) | (r << 16) | (g << 8) | b;
}

/* True if name looks like a font file FreeType can open. */
static int is_font_filename(const char *name)
{
	const char *dot;
	if (!name || name[0] == '.')
		return 0;
	dot = strrchr(name, '.');
	if (!dot)
		return 0;
	return !strcasecmp(dot, ".ttf") || !strcasecmp(dot, ".otf") ||
	       !strcasecmp(dot, ".ttc") || !strcasecmp(dot, ".otc");
}

/* Font family for default selection: sans (UI), mono, serif. */
enum {
	FONT_FAMILY_SANS = 0,
	FONT_FAMILY_MONO = 1,
	FONT_FAMILY_SERIF = 2
};

static void lower_copy(char *dst, size_t n, const char *src)
{
	size_t i;
	if (!dst || n == 0)
		return;
	for (i = 0; i + 1 < n && src && src[i]; i++)
		dst[i] = (char)tolower((unsigned char)src[i]);
	dst[i] = '\0';
}

/* Score a filename for a family; higher is better. 0 = not a candidate. */
static int font_family_score(const char *name, int family)
{
	char low[256];
	int mono, serif, sans;

	if (!name || !name[0])
		return 0;
	lower_copy(low, sizeof(low), name);

	mono = strstr(low, "mono") || strstr(low, "courier") ||
	       strstr(low, "consolas") || strstr(low, "menlo") ||
	       strstr(low, "monaco") || strstr(low, "inconsolata") ||
	       strstr(low, "sourcecode") || strstr(low, "source code") ||
	       strstr(low, "jetbrains") || strstr(low, "firacode") ||
	       strstr(low, "fira code") || strstr(low, "hack") ||
	       strstr(low, "fixed") || strstr(low, "sfnsmono") ||
	       strstr(low, "sf mono") || strstr(low, "liberationmono");
	serif = strstr(low, "serif") || strstr(low, "times") ||
		strstr(low, "georgia") || strstr(low, "garamond") ||
		strstr(low, "palatino") || strstr(low, "bookman") ||
		strstr(low, "libertine") || strstr(low, "charter");
	/* "sans" and common UI families; exclude mono/serif names. */
	sans = strstr(low, "sans") || strstr(low, "arial") ||
	       strstr(low, "helvetica") || strstr(low, "roboto") ||
	       strstr(low, "ubuntu") || strstr(low, "noto") ||
	       strstr(low, "dejavu") || strstr(low, "liberation") ||
	       strstr(low, "inter") || strstr(low, "segoe");

	if (family == FONT_FAMILY_MONO)
		return mono ? 3 : 0;
	if (family == FONT_FAMILY_SERIF) {
		if (mono)
			return 0;
		if (strstr(low, "serif") && !strstr(low, "sans"))
			return 4;
		return serif ? 3 : 0;
	}
	/* SANS: prefer explicit sans; reject mono; weak-accept generic UI fonts. */
	if (mono)
		return 0;
	if (strstr(low, "sans") && !strstr(low, "serif"))
		return 4;
	if (serif && !strstr(low, "sans"))
		return 0;
	if (sans)
		return 2;
	return 1; /* any other proportional font as last-resort sans */
}

/* Scan dir for best readable font matching family. Returns 1 if set. */
static int pick_font_in_dir(const char *dir, char *out, size_t outlen, int family)
{
	DIR *d;
	struct dirent *ent;
	char path[MAX_PATH_LEN];
	char best[MAX_PATH_LEN];
	int best_score = 0;

	if (!dir || !dir[0] || !out || outlen < 8)
		return 0;
	best[0] = '\0';
	d = opendir(dir);
	if (!d)
		return 0;
	while ((ent = readdir(d)) != NULL) {
		int sc;
		if (!is_font_filename(ent->d_name))
			continue;
		sc = font_family_score(ent->d_name, family);
		if (sc <= best_score)
			continue;
		if (snprintf(path, sizeof(path), "%s/%s", dir, ent->d_name) >=
		    (int)sizeof(path))
			continue;
		if (access(path, R_OK) != 0)
			continue;
		strncpy(best, path, sizeof(best) - 1);
		best[sizeof(best) - 1] = '\0';
		best_score = sc;
	}
	closedir(d);
	if (best_score <= 0 || !best[0])
		return 0;
	strncpy(out, best, outlen - 1);
	out[outlen - 1] = '\0';
	return 1;
}

/* Try a path that may be a font file or a directory of fonts. */
static int pick_font_path(const char *path, char *out, size_t outlen, int family)
{
	DIR *d;

	if (!path || !path[0])
		return 0;
	d = opendir(path);
	if (d) {
		closedir(d);
		return pick_font_in_dir(path, out, outlen, family);
	}
	if (access(path, R_OK) != 0)
		return 0;
	/* Exact file path: accept if name fits family (or any for explicit file). */
	if (font_family_score(path, family) <= 0 && family != FONT_FAMILY_SANS)
		return 0;
	strncpy(out, path, outlen - 1);
	out[outlen - 1] = '\0';
	return 1;
}

static void pick_default_font_family(char *out, size_t outlen, int family)
{
	const char *home = getenv("HOME");
	const char *xdg_data = getenv("XDG_DATA_HOME");
	char dir[MAX_PATH_LEN];
	const char **system_fonts;
	static const char *sans_fonts[] = {
#ifdef __linux__
		"/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
		"/usr/share/fonts/TTF/DejaVuSans.ttf",
		"/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
		"/usr/share/fonts/truetype/freefont/FreeSans.ttf",
		"/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf",
		"/share/fonts/truetype/dejavu/DejaVuSans.ttf",
		"/share/fonts/TTF/DejaVuSans.ttf",
		"/usr/share/fonts/dejavu/DejaVuSans.ttf",
#endif
#ifdef __APPLE__
		"/System/Library/Fonts/Supplemental/Arial.ttf",
		"/System/Library/Fonts/Supplemental/Arial Unicode.ttf",
		"/Library/Fonts/Arial Unicode.ttf",
		"/System/Library/Fonts/Helvetica.ttc",
#endif
		NULL
	};
	static const char *mono_fonts[] = {
#ifdef __linux__
		"/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
		"/usr/share/fonts/TTF/DejaVuSansMono.ttf",
		"/usr/share/fonts/truetype/liberation/LiberationMono-Regular.ttf",
		"/usr/share/fonts/truetype/freefont/FreeMono.ttf",
		"/usr/share/fonts/truetype/noto/NotoSansMono-Regular.ttf",
		"/usr/share/fonts/truetype/ubuntu/UbuntuMono-R.ttf",
		"/usr/share/fonts/truetype/droid/DroidSansMono.ttf",
		"/usr/share/fonts/truetype/terminus/TerminusTTF-4.49.1.ttf",
		"/usr/share/fonts/truetype/terminus/TerminusTTF.ttf",
		"/usr/share/fonts/X11/misc/ter-u14n.pcf.gz",
		"/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
		"/share/fonts/TTF/DejaVuSansMono.ttf",
		"/share/fonts/truetype/liberation/LiberationMono-Regular.ttf",
		"/share/fonts/truetype/freefont/FreeMono.ttf",
		/* lin0 / embedded flat roots */
		"/fonts/DejaVuSansMono.ttf",
		"/fonts/LiberationMono-Regular.ttf",
		"/fonts/FreeMono.ttf",
#endif
#ifdef __APPLE__
		"/System/Library/Fonts/SFNSMono.ttf",
		"/System/Library/Fonts/Monaco.ttf",
		"/System/Library/Fonts/Menlo.ttc",
		"/Library/Fonts/Courier New.ttf",
		"/System/Library/Fonts/Supplemental/Courier New.ttf",
#endif
		NULL
	};
	static const char *serif_fonts[] = {
#ifdef __linux__
		"/usr/share/fonts/truetype/dejavu/DejaVuSerif.ttf",
		"/usr/share/fonts/TTF/DejaVuSerif.ttf",
		"/usr/share/fonts/truetype/liberation/LiberationSerif-Regular.ttf",
		"/usr/share/fonts/truetype/freefont/FreeSerif.ttf",
		"/usr/share/fonts/truetype/noto/NotoSerif-Regular.ttf",
		"/share/fonts/truetype/dejavu/DejaVuSerif.ttf",
		"/share/fonts/TTF/DejaVuSerif.ttf",
#endif
#ifdef __APPLE__
		"/System/Library/Fonts/Supplemental/Times New Roman.ttf",
		"/Library/Fonts/Times New Roman.ttf",
		"/System/Library/Fonts/Supplemental/Georgia.ttf",
		"/Library/Fonts/Georgia.ttf",
		"/System/Library/Fonts/Times.ttc",
#endif
		NULL
	};
	int i;

	out[0] = '\0';
	if (family == FONT_FAMILY_MONO)
		system_fonts = mono_fonts;
	else if (family == FONT_FAMILY_SERIF)
		system_fonts = serif_fonts;
	else
		system_fonts = sans_fonts;

	/* Prefer well-known system paths first (predictable defaults). */
	for (i = 0; system_fonts[i]; i++) {
		if (pick_font_path(system_fonts[i], out, outlen, family))
			return;
	}

	/* Then scan user/system font directories for a matching family name. */
	if (xdg_data && xdg_data[0] == '/') {
		snprintf(dir, sizeof(dir), "%s/fonts", xdg_data);
		if (pick_font_path(dir, out, outlen, family))
			return;
	}
	if (home && home[0]) {
		snprintf(dir, sizeof(dir), "%s/.local/share/fonts", home);
		if (pick_font_path(dir, out, outlen, family))
			return;
		snprintf(dir, sizeof(dir), "%s/.fonts", home);
		if (pick_font_path(dir, out, outlen, family))
			return;
#ifdef __APPLE__
		snprintf(dir, sizeof(dir), "%s/Library/Fonts", home);
		if (pick_font_path(dir, out, outlen, family))
			return;
#endif
	}
#ifdef __APPLE__
	if (pick_font_path("/Library/Fonts", out, outlen, family))
		return;
	if (pick_font_path("/System/Library/Fonts", out, outlen, family))
		return;
	if (pick_font_path("/System/Library/Fonts/Supplemental", out, outlen,
			   family))
		return;
#endif
#ifdef __linux__
	if (pick_font_path("/usr/local/share/fonts", out, outlen, family))
		return;
	if (pick_font_path("/usr/share/fonts/truetype", out, outlen, family))
		return;
	if (pick_font_path("/usr/share/fonts/TTF", out, outlen, family))
		return;
#endif
}

void init_config_defaults(struct config *config)
{
	config->type = BG_COLOR;
	/*
	 * Default "Goldie" (sowm-inspired): mustard desktop, black floating
	 * cards, thick warm borders, sand accent for selection/focus.
	 */
	config->color = 0xFFF5C078;

	config->theme.background = 0xFF0A0A0A;
	config->theme.button = 0xFF1C1814;
	config->theme.button_text = 0xFFF5E6D3;
	config->theme.button_border_size = 3;
	config->theme.input_border_size = 3;
	config->theme.frame_border_size = 6;
	/* Same as background — no contrasting window chrome ring. */
	config->theme.frame_border_color = 0xFF0A0A0A;
	config->theme.focus = 0xFFE0A060;
	config->theme.focus_bg = 0xFF2A2018;
	config->theme.input_bg = 0xFF1C1814;
	config->theme.highlight = 0xFFD4B8A0;
	/* Panel dividers (e.g. settings sidebar rule). */
	config->theme.rule_color = 0xFFF5E6D3;
	config->theme.text_baseline_offset = 0;
	/*
	 * Sowm-style floating cards:
	 *   frame_margin = 0  → border is the window edge
	 *   padding           → air inside the border
	 *   margin            → gap between sibling widgets
	 */
	config->theme.margin = 8;
	config->theme.padding = 12;
	config->theme.frame_margin = 0;

	/* Font defaults under [font]: sans, mono, serif, size. */
	config->font_sans_path[0] = '\0';
	config->font_mono_path[0] = '\0';
	config->font_serif_path[0] = '\0';
	config->font_size = 14;

	// Background image fields (safe defaults; mode matches BGCE default)
	config->path[0] = '\0';
	config->mode = IMAGE_SCALED;

	pick_default_font_family(config->font_sans_path, MAX_PATH_LEN,
				 FONT_FAMILY_SANS);
	pick_default_font_family(config->font_mono_path, MAX_PATH_LEN,
				 FONT_FAMILY_MONO);
	pick_default_font_family(config->font_serif_path, MAX_PATH_LEN,
				 FONT_FAMILY_SERIF);
	/* Mono/serif fall back to UI font if nothing family-specific found. */
	if (!config->font_mono_path[0] && config->font_sans_path[0])
		strncpy(config->font_mono_path, config->font_sans_path,
			MAX_PATH_LEN - 1);
	if (!config->font_serif_path[0] && config->font_sans_path[0])
		strncpy(config->font_serif_path, config->font_sans_path,
			MAX_PATH_LEN - 1);
}



void format_hex_color(uint32_t color, char *buf, int buflen)
{
	unsigned r = (color >> 16) & 0xFF;
	unsigned g = (color >> 8) & 0xFF;
	unsigned b = color & 0xFF;
	snprintf(buf, buflen, "#%02X%02X%02X", r, g, b);
}

int write_config(const struct config *config)
{
	const char *home = getenv("HOME");
	char dir[512];
	char path[512];
	FILE *f;

	if (!home)
		return -1;

	snprintf(dir, sizeof(dir), "%s/.config", home);
	(void)mkdir(dir, 0755);
	snprintf(path, sizeof(path), "%s/.config/bgtk.conf", home);
	f = fopen(path, "w");
	if (!f) {
		perror("[BGTK] Write config file");
		return -1;
	}

	char c[16];

	fprintf(f, "[background]\n");
	if (config->type == BG_IMAGE) {
		fprintf(f, "type = image\n");
		fprintf(f, "path = %s\n", config->path);
		fprintf(f, "mode = %s\n", config->mode == IMAGE_SCALED ? "scaled" : "tiled");
	} else {
		fprintf(f, "type = color\n");
		format_hex_color(config->color, c, sizeof(c));
		fprintf(f, "color = %s\n", c);
	}

	fprintf(f, "\n[theme]\n");
	format_hex_color(config->theme.background, c, sizeof(c));
	fprintf(f, "background = %s\n", c);
	format_hex_color(config->theme.button, c, sizeof(c));
	fprintf(f, "button = %s\n", c);
	format_hex_color(config->theme.button_text, c, sizeof(c));
	fprintf(f, "button_text = %s\n", c);
	fprintf(f, "button_border_size = %u\n", config->theme.button_border_size);
	fprintf(f, "input_border_size = %u\n", config->theme.input_border_size);
	fprintf(f, "frame_border_size = %u\n", config->theme.frame_border_size);
	format_hex_color(config->theme.frame_border_color, c, sizeof(c));
	fprintf(f, "frame_border_color = %s\n", c);
	format_hex_color(config->theme.focus, c, sizeof(c));
	fprintf(f, "focus = %s\n", c);
	format_hex_color(config->theme.focus_bg, c, sizeof(c));
	fprintf(f, "focus_bg = %s\n", c);
	format_hex_color(config->theme.input_bg, c, sizeof(c));
	fprintf(f, "input_bg = %s\n", c);
	format_hex_color(config->theme.highlight, c, sizeof(c));
	fprintf(f, "highlight = %s\n", c);
	format_hex_color(config->theme.rule_color, c, sizeof(c));
	fprintf(f, "rule_color = %s\n", c);
	fprintf(f, "text_baseline_offset = %d\n",
		config->theme.text_baseline_offset);
	fprintf(f, "margin = %d\n", config->theme.margin);
	fprintf(f, "padding = %d\n", config->theme.padding);
	fprintf(f, "frame_margin = %d\n", config->theme.frame_margin);

	fprintf(f, "\n[font]\n");
	if (config->font_sans_path[0])
		fprintf(f, "sans = %s\n", config->font_sans_path);
	if (config->font_mono_path[0])
		fprintf(f, "mono = %s\n", config->font_mono_path);
	if (config->font_serif_path[0])
		fprintf(f, "serif = %s\n", config->font_serif_path);
	fprintf(f, "size = %d\n", config->font_size);

	fclose(f);
	return 0;
}

// Parse config file
int parse_config(struct config *config)
{
	init_config_defaults(config);

	const char *home = getenv("HOME");
	char user_config[512];
	if (!home) {
		return -1;
	}
	snprintf(user_config, sizeof(user_config), "%s/.config/bgtk.conf",
		 home);
	FILE *file = fopen(user_config, "r");
	if (!file) {
		/* Missing config is normal — defaults from init_config_defaults
		 * already applied. Do not spam perror into a shared stream. */
		return -1;
	}

	char line[1024];
	char current_section[256] = "";

	while (fgets(line, sizeof(line), file)) {
		char *trimmed = trim(line);

		// Skip empty lines and comments
		if (trimmed[0] == '\0' || trimmed[0] == '#' ||
		    trimmed[0] == ';') {
			continue;
		}
		// Check for section
		if (trimmed[0] == '[' && trimmed[strlen(trimmed) - 1] == ']') {
			strncpy(current_section, trimmed + 1,
				strlen(trimmed) - 2);
			current_section[strlen(trimmed) - 2] = '\0';
			continue;
		}
		// Parse key-value pairs
		char *equals = strchr(trimmed, '=');
		if (!equals) {
			continue;
		}

		char key[32];
		char value[MAX_PATH_LEN];
		key[0] = value[0] = '\0';
		sscanf(trimmed, "%31s = %511[^\n]", key, value);

		if (strcmp(current_section, "background") == 0) {
			if (strcmp(key, "type") == 0) {
				if (strcmp(value, "color") == 0) {
					config->type = BG_COLOR;
				} else if (strcmp(value, "image") == 0) {
					config->type = BG_IMAGE;
				}
			} else if (strcmp(key, "color") == 0 &&
				   config->type == BG_COLOR) {
				config->color = parse_hex_color(value);
			} else if (strcmp(key, "path") == 0) {
				/* Accept path even if type= comes later (BGCE order). */
				strncpy(config->path, value, MAX_PATH_LEN - 1);
				config->path[MAX_PATH_LEN - 1] = '\0';
			} else if (strcmp(key, "mode") == 0) {
				if (strcmp(value, "tiled") == 0) {
					config->mode = IMAGE_TILED;
				} else if (strcmp(value, "scaled") == 0) {
					config->mode = IMAGE_SCALED;
				}
			}
		} else if (strcmp(current_section, "theme") == 0) {
			if (strcmp(key, "background") == 0) {
				config->theme.background =
				    parse_hex_color(value);
			} else if (strcmp(key, "button") == 0) {
				config->theme.button = parse_hex_color(value);
			} else if (strcmp(key, "button_text") == 0) {
				config->theme.button_text =
				    parse_hex_color(value);
			} else if (strcmp(key, "frame_border_size") == 0) {
				config->theme.frame_border_size = atoi(value);
			} else if (strcmp(key, "input_border_size") == 0) {
				config->theme.input_border_size = atoi(value);
			} else if (strcmp(key, "button_border_size") == 0) {
				config->theme.button_border_size = atoi(value);
			} else if (strcmp(key, "frame_border_color") == 0) {
				config->theme.frame_border_color =
				    parse_hex_color(value);
			} else if (strcmp(key, "focus") == 0) {
				config->theme.focus = parse_hex_color(value);
			} else if (strcmp(key, "focus_bg") == 0) {
				config->theme.focus_bg = parse_hex_color(value);
			} else if (strcmp(key, "input_bg") == 0) {
				config->theme.input_bg = parse_hex_color(value);
			} else if (strcmp(key, "highlight") == 0) {
				config->theme.highlight = parse_hex_color(value);
			} else if (strcmp(key, "rule_color") == 0) {
				config->theme.rule_color = parse_hex_color(value);
			} else if (strcmp(key, "text_baseline_offset") == 0) {
				config->theme.text_baseline_offset = atoi(value);
			} else if (strcmp(key, "margin") == 0) {
				config->theme.margin = atoi(value);
			} else if (strcmp(key, "padding") == 0) {
				config->theme.padding = atoi(value);
			} else if (strcmp(key, "frame_margin") == 0) {
				config->theme.frame_margin = atoi(value);
			}
		} else if (strcmp(current_section, "font") == 0) {
			if (strcmp(key, "sans") == 0) {
				strncpy(config->font_sans_path, value,
					MAX_PATH_LEN - 1);
				config->font_sans_path[MAX_PATH_LEN - 1] = '\0';
			} else if (strcmp(key, "mono") == 0) {
				strncpy(config->font_mono_path, value,
					MAX_PATH_LEN - 1);
				config->font_mono_path[MAX_PATH_LEN - 1] = '\0';
			} else if (strcmp(key, "serif") == 0) {
				strncpy(config->font_serif_path, value,
					MAX_PATH_LEN - 1);
				config->font_serif_path[MAX_PATH_LEN - 1] =
					'\0';
			} else if (strcmp(key, "size") == 0) {
				config->font_size = atoi(value);
			}
		}
	}

	fclose(file);
	return 0;
}
