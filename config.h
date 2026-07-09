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
	/* Focused text-input ring + caret. */
	uint32_t focus;
	/* Focused text-input field fill. */
	uint32_t focus_bg;
	/* Unfocused text-input field fill. */
	uint32_t input_bg;
	/* Accent: headers, links, selected list rows. */
	uint32_t highlight;
	/* Horizontal/vertical rules (dividers); 0 = fall back in draw. */
	uint32_t rule_color;
	/* Global FreeType baseline tweak (px; added after per-widget baseline_offset). */
	int text_baseline_offset;
	/*
	 * Spacing (px). Apps should prefer these over magic constants:
	 *   margin       — gap between sibling widgets (lists, rows)
	 *   padding      — air inside the frame border (content inset)
	 *   frame_margin — air outside the frame border (sowm cards: 0)
	 */
	int margin;
	int padding;
	int frame_margin;
} BGTK_Theme;

// Config structure
struct config {
	/* UI / proportional sans ([font] key: sans). */
	char font_sans_path[MAX_PATH_LEN];
	/* Monospace (terminal); falls back to sans if unset/unloadable. */
	char font_mono_path[MAX_PATH_LEN];
	/* Serif (documents / gemini body text optional). */
	char font_serif_path[MAX_PATH_LEN];
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

// Parse #RRGGBB or #RRGGBBAA into 0xAARRGGBB (missing/invalid → opaque black).
uint32_t parse_hex_color(const char *str);

// Initialize a config struct with built-in sane defaults (theme, font size, etc.).
// Called by parse_config and bgtk_init so that init only loads/overrides.
void init_config_defaults(struct config *config);

#endif // CONFIG_H

