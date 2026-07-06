#include "config.h"

#include "bgtk.h"
#include "internal.h"

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>  // strcasecmp
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

// Helper: parse hex color (#RRGGBB or #RRGGBBAA)
static uint32_t parse_hex_color(const char *str)
{
	if (str[0] != '#') {
		return 0xFF000000;	// Default to black with full opacity
	}

	unsigned int r, g, b, a = 255;
	if (strlen(str) == 7) {	// #RRGGBB
		sscanf(str + 1, "%02x%02x%02x", &r, &g, &b);
	} else if (strlen(str) == 9) {	// #RRGGBBAA
		sscanf(str + 1, "%02x%02x%02x%02x", &r, &g, &b, &a);
	} else {
		return 0xFF000000;	// Default to black with full opacity
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

/* Scan dir (non-recursive) for the first readable font file. Returns 1 if set. */
static int pick_font_in_dir(const char *dir, char *out, size_t outlen)
{
	DIR *d;
	struct dirent *ent;
	char path[MAX_PATH_LEN];

	if (!dir || !dir[0] || !out || outlen < 8)
		return 0;
	d = opendir(dir);
	if (!d)
		return 0;
	while ((ent = readdir(d)) != NULL) {
		if (!is_font_filename(ent->d_name))
			continue;
		if (snprintf(path, sizeof(path), "%s/%s", dir, ent->d_name) >=
		    (int)sizeof(path))
			continue;
		if (access(path, R_OK) != 0)
			continue;
		strncpy(out, path, outlen - 1);
		out[outlen - 1] = '\0';
		closedir(d);
		return 1;
	}
	closedir(d);
	return 0;
}

/* Try a path that may be a font file or a directory of fonts. */
static int pick_font_path(const char *path, char *out, size_t outlen)
{
	DIR *d;

	if (!path || !path[0])
		return 0;
	d = opendir(path);
	if (d) {
		closedir(d);
		return pick_font_in_dir(path, out, outlen);
	}
	if (access(path, R_OK) != 0)
		return 0;
	strncpy(out, path, outlen - 1);
	out[outlen - 1] = '\0';
	return 1;
}

static void pick_default_font(char *out, size_t outlen)
{
	const char *home = getenv("HOME");
	const char *xdg_data = getenv("XDG_DATA_HOME");
	char dir[MAX_PATH_LEN];

	/* User font folders (highest priority). */
	if (xdg_data && xdg_data[0] == '/') {
		snprintf(dir, sizeof(dir), "%s/fonts", xdg_data);
		if (pick_font_path(dir, out, outlen))
			return;
	}
	if (home && home[0]) {
		snprintf(dir, sizeof(dir), "%s/.local/share/fonts", home);
		if (pick_font_path(dir, out, outlen))
			return;
		snprintf(dir, sizeof(dir), "%s/.fonts", home);
		if (pick_font_path(dir, out, outlen))
			return;
#ifdef __APPLE__
		snprintf(dir, sizeof(dir), "%s/Library/Fonts", home);
		if (pick_font_path(dir, out, outlen))
			return;
#endif
	}
#ifdef __APPLE__
	if (pick_font_path("/Library/Fonts", out, outlen))
		return;
#endif
#ifdef __linux__
	/* Common admin/user install location on Linux. */
	if (pick_font_path("/usr/local/share/fonts", out, outlen))
		return;
#endif

	/* Fixed system candidates. */
	{
		static const char *system_fonts[] = {
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
			"/System/Library/Fonts/SFNSMono.ttf",
			"/System/Library/Fonts/Monaco.ttf",
#endif
			NULL
		};
		for (int i = 0; system_fonts[i]; i++) {
			if (pick_font_path(system_fonts[i], out, outlen))
				return;
		}
	}
}

void init_config_defaults(struct config *config)
{
	config->type = BG_COLOR;
	/* High-contrast defaults so labels/text stay readable without a
	 * config file (previous semi-transparent grays made dark text vanish
	 * on some compositors). Full alpha, light panel, dark ink. */
	config->color = 0xFFE8E8E8;

	config->theme.background = 0xFFE8E8E8;
	config->theme.button = 0xFFD0D0D0;
	config->theme.button_text = 0xFF111111;
	config->theme.button_border_size = 1;
	config->theme.input_border_size = 2;
	config->theme.frame_border_size = 4;
	config->theme.frame_border_color = 0xFF333333;

	// Font defaults (loaded at runtime from the config file under [font]).
	// If the user does not provide a path, we select a sane platform default here
	// (the #ifdefs live in the config package).
	config->font_path[0] = '\0';
	config->font_size = 14;

	// Background image fields (safe defaults)
	config->path[0] = '\0';
	config->mode = IMAGE_TILED;

	// Prefer user font folders, then system UI fonts.
	if (config->font_path[0] == '\0')
		pick_default_font(config->font_path, MAX_PATH_LEN);
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
	if (!home)
		return -1;

	char path[512];
	snprintf(path, sizeof(path), "%s/.config/bgtk.conf", home);
	FILE *f = fopen(path, "w");
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

	fprintf(f, "\n[font]\n");
	if (config->font_path[0])
		fprintf(f, "path = %s\n", config->font_path);
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
		char value[128];
		sscanf(trimmed, "%s = %[^\n]", key, value);

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
			} else if (strcmp(key, "path") == 0 &&
				   config->type == BG_IMAGE) {
				strncpy(config->path, value, MAX_PATH_LEN - 1);
				config->path[MAX_PATH_LEN - 1] = '\0';
			} else if (strcmp(key, "mode") == 0 &&
				   config->type == BG_IMAGE) {
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
			}
		} else if (strcmp(current_section, "font") == 0) {

			if (strcmp(key, "path") == 0) {
				strncpy(config->font_path, value,
					MAX_PATH_LEN - 1);
				config->font_path[MAX_PATH_LEN - 1] = '\0';
			} else if (strcmp(key, "size") == 0) {
				config->font_size = atoi(value);
			}
		}
	}

	fclose(file);
	return 0;
}
