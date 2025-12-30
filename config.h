#ifndef CONFIG_H
#define CONFIG_H

#include <stdint.h>

#define MAX_PATH_LEN 512

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

// Theme colors
typedef struct {
	uint32_t background;
	uint32_t button;
	uint32_t button_text;
} BGTK_Theme;

// Config structure
struct config {
	BackgroundType type;
	uint32_t color;
	char path[MAX_PATH_LEN];
	ImageMode mode;
	BGTK_Theme theme;
	char font_path[MAX_PATH_LEN];
	int font_size;
};

int parse_config(struct config* config);

#endif // CONFIG_H

