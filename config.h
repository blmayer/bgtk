#ifndef CONFIG_H
#define CONFIG_H

#include <stdint.h>

#define MAX_PATH_LEN 512

// Forward declaration so config can declare functions that take BGTK_Context
// without including the full bgtk.h (avoids include-order issues).
struct BGTK_Context;

// Background type
typedef enum {
	BG_COLOR,
	BG_IMAGE
} BackgroundType;

// Image mode
typedef enum {
	IMAGE_TILED,
	IMAGE_SCALED
} ImageMode;

// Theme stuff
typedef struct {
	uint32_t background;
	uint32_t button;
	uint32_t button_text;
	uint32_t frame_border_size;
	uint32_t button_border_size;
	uint32_t input_border_size;
	uint32_t frame_border_color;
} BGTK_Theme;

// Config structure
struct config {
	char font_path[MAX_PATH_LEN];
	int font_size;

	BackgroundType type;
	uint32_t color;
	char path[MAX_PATH_LEN];
	ImageMode mode;

	BGTK_Theme theme;
};

int parse_config(struct config* config);

// Write the config struct back to ~/.config/bgtk.conf.
int write_config(const struct config* config);

// Format a 0xAARRGGBB color as #RRGGBB (buf must be >= 8 bytes).
void format_hex_color(uint32_t color, char *buf, int buflen);

// Initialize a config struct with built-in sane defaults (theme, font size, etc.).
// Called by parse_config and bgtk_init so that init only loads/overrides.
void init_config_defaults(struct config *config);

#endif // CONFIG_H

