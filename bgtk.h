#ifndef BGTK_H
#define BGTK_H

#include <ft2build.h>
#include <stddef.h>
#include <stdint.h>
#include FT_FREETYPE_H

// Forward declarations
typedef struct BGTK_Context BGTK_Context;
typedef struct BGTK_Widget BGTK_Widget;

// Function pointer for button callbacks
typedef void (*BGTK_Callback)(void);

// BGTK_Context: Holds the state of the BGTK application
struct BGTK_Context {
	int conn_fd; // File descriptor for BGCE connection
	void* shm_buffer;
	uint32_t buf_width; // The actual width of the buffer (might differ from server width)
	uint32_t buf_height;
	int width;
	// FreeType data
	FT_Library ft_library;
	FT_Face ft_face;
	int font_size;

	// For now, simple absolute positioning
	int height;
	// For now, simple absolute positioning
	BGTK_Widget** widgets;
	size_t widget_count;
	size_t widget_capacity;
};

// BGTK_Widget_Type
typedef enum {
	BGTK_WIDGET_LABEL,
	BGTK_WIDGET_BUTTON,
	// Add more types as needed
} BGTK_Widget_Type;

// BGTK_Widget: Base structure for all widgets
struct BGTK_Widget {
	BGTK_Context* ctx;
	BGTK_Widget_Type type;
	int x, y, w, h; // Absolute position and size

	// Union for specific widget data
	union {
		struct {
			char* text;
		} label;
		struct {
			char* text;
			BGTK_Callback callback;
		} button;
	} data;

	void (*set_label)(BGTK_Widget* self, char*);
};

// --- Core Functions ---

// Initializes BGTK, connects to BGCE server, gets buffer/dimensions.
BGTK_Context* bgtk_init(void);

// Blocking loop to handle events and redraws.
void bgtk_main_loop(BGTK_Context* ctx);

// Cleans up the BGTK context and resources.
void bgtk_destroy(BGTK_Context* ctx);

// --- Widget Creation Functions ---

// Creates a label widget.
BGTK_Widget* bgtk_label_new(BGTK_Context* ctx, char* text);

// Creates a button widget.
BGTK_Widget* bgtk_button_new(BGTK_Context* ctx, char* text, BGTK_Callback callback);

// --- Layout/Management Functions ---

// Adds a widget to the context with absolute positioning.
void bgtk_add_widget(BGTK_Context* ctx, BGTK_Widget* widget, int x, int y, int w, int h);

#endif // BGTK_H
