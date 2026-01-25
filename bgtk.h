#ifndef BGTK_H
#define BGTK_H

#include <ft2build.h>
#include <stddef.h>
#include <stdint.h>
#include FT_FREETYPE_H
#include <bgce.h>

#include "config.h"

// Function pointer for button callbacks
typedef void (*BGTK_Callback)(void);

// BGTK_Context: Holds the state of the BGTK application
struct BGTK_Context {
	int conn_fd;  // File descriptor for BGCE connection
	void* shm_buffer;
	int width;
	int height;

	// FreeType data
	FT_Library ft_library;
	FT_Face ft_face;
	int font_size;

	// Theme data
	BGTK_Theme theme;
	char font_path[MAX_PATH_LEN];

	// Single root widget for the widget tree
	struct BGTK_Widget* root_widget;

	// Currently focused widget (for keyboard input)
	struct BGTK_Widget* focused_widget;
};

// BGTK_Widget_Type
enum BGTK_Widget_Type {
	BGTK_WIDGET_BUTTON,
	BGTK_WIDGET_LABEL,
	BGTK_WIDGET_TEXT,
	BGTK_WIDGET_SCROLLABLE,
	BGTK_WIDGET_IMAGE,
	BGTK_WIDGET_FRAME,
	BGTK_WIDGET_TEXT_INPUT,
	// Add more types as needed
};

// Widget flags
#define BGTK_FLAG_CENTER (1 << 0)  // Center widgets horizontally

// BGTK_Options: Options for widget creation (replaces flags).
typedef struct {
	int flags;      // Flags for widget behavior (e.g., BGTK_FLAG_CENTER).
	int padding;   // Internal spacing (pixels).
	int margin;    // External spacing (pixels).
} BGTK_Options;

// BGTK_Widget: Base structure for all widgets
struct BGTK_Widget {
	struct BGTK_Context* ctx;
	enum BGTK_Widget_Type type;
	int x, y, w, h;      // Absolute position and size
	int flags;          // Flags for widget behavior
	int padding;        // Internal spacing (pixels)
	int margin;         // External spacing (pixels)
	
	// Function pointer for event handling
	int (*handle_event)(struct BGTK_Widget* widget, struct InputEvent ev);
	
	// Union for specific widget data
	union {
		struct {
			struct BGTK_Widget* text;  // Text widget for label
			void (*set_label)(struct BGTK_Widget* self, char*);
		} label;
		struct {
			struct BGTK_Widget* label;  // Label widget for button
			BGTK_Callback callback;
		} button;
		struct {
			char* text;
		} text;
		struct {
			struct BGTK_Widget* child;
			int border_w;
		} frame;
		struct {
			struct BGTK_Widget** items;  // List of child widgets
			int widget_count;
			int widget_capacity;
			int scroll_y;	     // Current scroll position
			int content_height;  // Total height of all
					     // child widgets
			uint32_t* tmp;	     // off-screen buffer
		} scrollable;
		struct {
			uint32_t* pixels;  // Pixel buffer (RGBA)
			int img_w;	   // Image width
			int img_h;	   // Image height
		} image;
		struct {
			char* text;           // Input text
			uint32_t cursor_pos;       // Cursor position
			int selection_start;  // Selection start (-1 if none)
			int selection_end;    // Selection end (-1 if none)
			bool focused;         // Whether the widget is focused
			BGTK_Callback on_change; // Callback for text changes
		} text_input;
	} data;
};

// --- Core Functions ---

void bgtk_draw_widgets(struct BGTK_Context* ctx);

void bgtk_destroy(struct BGTK_Context* ctx);

// Initializes BGTK with given dimensions.
struct BGTK_Context* bgtk_init(int conn_fd, void* buffer, int width, int height);

// Handles a single event and returns whether a redraw is needed.
int bgtk_handle_input_event(struct BGTK_Context* ctx, struct InputEvent ev);

// --- Widget Creation Functions ---
// Creates a label widget.
struct BGTK_Widget* bgtk_label(struct BGTK_Context* ctx, char* text, BGTK_Options options);
struct BGTK_Widget* bgtk_button(struct BGTK_Context* ctx,
			struct BGTK_Widget* text,
			BGTK_Callback callback, BGTK_Options options);

struct BGTK_Widget* bgtk_text(struct BGTK_Context* ctx, char* text, BGTK_Options options);

struct BGTK_Widget* bgtk_scrollable(struct BGTK_Context* ctx, struct BGTK_Widget** items, int widget_count, BGTK_Options options);

// Creates an image widget.
struct BGTK_Widget* bgtk_image(struct BGTK_Context* ctx, const char* path, BGTK_Options options);

// Creates a frame widget.
struct BGTK_Widget* bgtk_frame(struct BGTK_Context* ctx, struct BGTK_Widget* child, int width, int height, BGTK_Options options);

struct BGTK_Widget* bgtk_text_input(struct BGTK_Context* ctx, char* initial_text, int width, int height, BGTK_Options options);
#endif
