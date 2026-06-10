#include "config.h"

#include "bgtk.h"
#include "internal.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

void init_config_defaults(struct config *config)
{
	config->type = BG_COLOR;
	config->color = 0xAAAAAAAA;	// Default gray

	// Theme defaults
	config->theme.background = 0xAAAAAAAA;
	config->theme.button = 0x88888888;
	config->theme.button_text = 0xFFFFFFFF;
	config->theme.button_border_size = 1;
	config->theme.input_border_size = 1;
	config->theme.frame_border_size = 4;
	config->theme.frame_border_color = 0xFFFFFFFF;

	// Font defaults (loaded at runtime from the config file under [font]).
	// If the user does not provide a path, we select a sane platform default here
	// (the #ifdefs live in the config package).
	config->font_path[0] = '\0';
	config->font_size = 12;

	// Background image fields (safe defaults)
	config->path[0] = '\0';
	config->mode = IMAGE_TILED;

	// Default font selection (platform-specific). This used to be "n2" in
	// bgtk_init_resources. We pick the first accessible one if no path was
	// given in the config file.
	if (config->font_path[0] == '\0') {
		static const char *d[] = {
#ifdef __linux__
			"/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
			"/usr/share/fonts/TTF/DejaVuSans.ttf",
#endif
#ifdef __APPLE__
			"/System/Library/Fonts/SFNSMono.ttf",
#endif
			NULL
		};
		for (int i = 0; d[i]; i++) {
			if (access(d[i], R_OK) == 0) {
				strncpy(config->font_path, d[i], MAX_PATH_LEN - 1);
				config->font_path[MAX_PATH_LEN - 1] = '\0';
				break;
			}
		}
	}
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
		perror("[BGTK] Open config file");
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
