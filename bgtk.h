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

	// Whether the window/surface is focused according to the server.
	// 0 = unfocused, 1 = focused.
	int window_focused;

	// Modifier state for text input (uppercase etc).
	int shift_held;
};


// Sets the focused widget for keyboard input.
// Passing NULL clears focus.
void bgtk_set_focus(struct BGTK_Context* ctx, struct BGTK_Widget* widget);

// Sets window focus state (from server focus events).
void bgtk_set_window_focus(struct BGTK_Context* ctx, int focused);


// BGTK_Widget_Type
enum BGTK_Widget_Type {
	BGTK_WIDGET_BUTTON,
	BGTK_WIDGET_LABEL,
	BGTK_WIDGET_TEXT,
	BGTK_WIDGET_SCROLLABLE,
	BGTK_WIDGET_LIST,
	BGTK_WIDGET_IMAGE,
	BGTK_WIDGET_FRAME,
	BGTK_WIDGET_TEXT_INPUT,
	// Add more types as needed
};

// Widget flags
#define BGTK_FLAG_CENTER (1 << 0)  // Center widgets horizontally

// List widget orientation
enum BGTK_List_Orientation {
	BGTK_LIST_VERTICAL,
	BGTK_LIST_HORIZONTAL,
};

// BGTK_Options: Options for widget creation (replaces flags).
typedef struct {
	int flags;      // Flags for widget behavior (e.g., BGTK_FLAG_CENTER).
	int padding;    // Internal spacing (pixels).
	int margin;     // External spacing (pixels).
	enum BGTK_List_Orientation orientation;  // For list widget: vertical or horizontal
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
			int pressed;  // non-zero while mouse button is held down on this widget
		} button;
		struct {
			char* text;
		} text;
		struct {
			uint32_t* pixels;  // Pixel buffer for image
			int img_w;  // Image intrinsic width
			int img_h;  // Image intrinsic height
		} image;
		struct {
			struct BGTK_Widget** items;  // List of child widgets
			int widget_count;
			int widget_capacity;
			int scroll_y;	     // Current scroll position
			int content_height;  // Total height of all child widgets
			uint32_t* tmp;	     // off-screen buffer
		} scrollable;
		struct {
			struct BGTK_Widget** items;  // List of child widgets
			int widget_count;
			enum BGTK_List_Orientation orientation;  // vertical or horizontal
			int content_width;   // Total width of all child widgets
			int content_height;  // Total height of all child widgets
		} list_widget;
		struct {
			struct BGTK_Widget* child;
			int border_w;
		} frame;
		struct {
			char* text;
			uint32_t cursor_pos;
			int selection_start;
			int selection_end;
			int scroll_x;
			void (*on_change)(void);
			void (*on_tab)(void);
			void (*on_enter)(void);
		} text_input;
	} data;  // End of union
};  // End of BGTK_Widget struct

// --- Core Functions ---

void bgtk_draw_widgets(struct BGTK_Context* ctx);

void bgtk_destroy(struct BGTK_Context* ctx);
void bgtk_destroy_mock(struct BGTK_Context* ctx);

// Initializes BGTK with given dimensions (real server path, caller provides buffer).
struct BGTK_Context* bgtk_init(int conn_fd, void* buffer, int width, int height);

// Initializes BGTK in mock/headless mode for testing. Owns an internal framebuffer.
// Use take_screenshot(ctx, "foo.png") after draws to inspect rendered output as an image.
struct BGTK_Context* bgtk_init_mock(int width, int height);

// Handles a single event and returns whether a redraw is needed.
int bgtk_handle_input_event(struct BGTK_Context* ctx, struct InputEvent ev);

// Take a screenshot of the current framebuffer to a PNG file.
// If path is NULL, a timestamped name is generated automatically (for KEY_SYSRQ).
int take_screenshot(struct BGTK_Context* ctx, const char* path);

// Inject a synthetic input event (for testing). Coordinates are absolute widget coords.
// Returns non-zero if a redraw was triggered.
int bgtk_inject_event(struct BGTK_Context* ctx, struct InputEvent ev);

// --- Widget Creation Functions ---
// Creates a label widget.
struct BGTK_Widget* bgtk_label(struct BGTK_Context* ctx, char* text, BGTK_Options options);
struct BGTK_Widget* bgtk_button(struct BGTK_Context* ctx,
			struct BGTK_Widget* text,
			BGTK_Callback callback, BGTK_Options options);

struct BGTK_Widget* bgtk_text(struct BGTK_Context* ctx, char* text, BGTK_Options options);

struct BGTK_Widget* bgtk_scrollable(struct BGTK_Context* ctx, struct BGTK_Widget** items, int widget_count, BGTK_Options options);

// Creates an image widget.
// width/height are the OUTER widget dimensions (including padding+margin).
// Pass 0 for width and/or height to use the image's intrinsic size for that dimension.
struct BGTK_Widget* bgtk_image(struct BGTK_Context* ctx, const char* path, int width, int height, BGTK_Options options);

// Backward-compatible helper: intrinsic image size.
static inline struct BGTK_Widget* bgtk_image_auto(struct BGTK_Context* ctx, const char* path, BGTK_Options options) {
	return bgtk_image(ctx, path, 0, 0, options);
}

// Creates a frame widget.
struct BGTK_Widget* bgtk_frame(struct BGTK_Context* ctx, struct BGTK_Widget* child, int width, int height, BGTK_Options options);
// Creates a text input widget.
struct BGTK_Widget* bgtk_text_input(struct BGTK_Context* ctx, char* initial_text, int width, int height, BGTK_Options options);

// Creates a list widget (arranges children vertically or horizontally without scrolling).
struct BGTK_Widget* bgtk_list(struct BGTK_Context* ctx, struct BGTK_Widget** items, int widget_count, BGTK_Options options);

#endif
