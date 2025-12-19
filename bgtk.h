#ifndef BGTK_H
#define BGTK_H

#include <ft2build.h>
#include <stddef.h>
#include <stdint.h>
#include FT_FREETYPE_H
#include <bgce.h>

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

	// Single root widget for the widget tree
	struct BGTK_Widget* root_widget;
};

// BGTK_Widget_Type
enum BGTK_Widget_Type {
	BGTK_WIDGET_BUTTON,
	BGTK_WIDGET_LABEL,
	BGTK_WIDGET_TEXT,
	BGTK_WIDGET_SCROLLABLE,
	BGTK_WIDGET_IMAGE,
	// Add more types as needed
};

// Widget flags
#define BGTK_FLAG_CENTER (1 << 0)  // Center widgets horizontally

// BGTK_Widget: Base structure for all widgets
struct BGTK_Widget {
	struct BGTK_Context* ctx;
	enum BGTK_Widget_Type type;
	int x, y, w, h;	 // Absolute position and size
	int flags;	 // Flags for widget behavior

	// Union for specific widget data
	union {
		struct {
			struct BGTK_Widget* text;  // Text widget for label
		} label;
		struct {
			struct BGTK_Widget* label;  // Label widget for button
			BGTK_Callback callback;
		} button;
		struct {
			char* text;
		} text;
		struct {
			struct BGTK_Widget** widgets;  // List of child widgets
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
	} data;

	void (*set_label)(struct BGTK_Widget* self, char*);
};

// --- Core Functions ---

// Initializes BGTK with given dimensions.
struct BGTK_Context* bgtk_init(int conn_fd, void* buffer, int width,
			       int height);

// Handles a single event and returns whether a redraw is
// needed.
int bgtk_handle_input_event(struct BGTK_Context* ctx, struct InputEvent ev);

void bgtk_destroy(struct BGTK_Context* ctx);

// --- Widget Creation Functions ---

// Creates a label widget.
struct BGTK_Widget* bgtk_label(struct BGTK_Context* ctx, char* text);
struct BGTK_Widget* bgtk_button(struct BGTK_Context* ctx,
				struct BGTK_Widget* text,
				BGTK_Callback callback, int flags);

void bgtk_draw_widgets(struct BGTK_Context* ctx);

// Creates a text widget (label) for use in other widgets (e.g.,
// buttons).
struct BGTK_Widget* bgtk_text(struct BGTK_Context* ctx, char* text, int flags);

struct BGTK_Widget* bgtk_scrollable(struct BGTK_Context* ctx,
				    struct BGTK_Widget** widgets,
				    int widget_count, int flags);
// Creates an image widget.
struct BGTK_Widget* bgtk_image(struct BGTK_Context* ctx, const char* path,
			       int flags);
#endif
