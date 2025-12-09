#ifndef BGTK_H
#define BGTK_H

#include <ft2build.h>
#include <stddef.h>
#include <stdint.h>
#include FT_FREETYPE_H

// Function pointer for button callbacks
typedef void (*BGTK_Callback)(void);

// BGTK_Context: Holds the state of the BGTK application
struct BGTK_Context {
	int conn_fd;  // File descriptor for BGCE connection
	void* shm_buffer;
	uint32_t buf_width;  // The actual width of the buffer (might differ
			     // from server width)
	uint32_t buf_height;
	int width;
	// FreeType data
	FT_Library ft_library;
	FT_Face ft_face;
	int font_size;

	// For now, simple absolute positioning
	int height;
	// For now, simple absolute positioning
	struct BGTK_Widget** widgets;
	size_t widget_count;
	size_t widget_capacity;
};

// BGTK_Widget_Type
enum BGTK_Widget_Type {
	BGTK_WIDGET_BUTTON,
	BGTK_WIDGET_LABEL,
	BGTK_WIDGET_TEXT,
	BGTK_WIDGET_SCROLLABLE,
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
			size_t widget_count;
			size_t widget_capacity;
			int scroll_y;	     // Current scroll position
			int content_height;  // Total height of all child
					     // widgets
			uint32_t* tmp;
		} scrollable;
	} data;

	void (*set_label)(struct BGTK_Widget* self, char*);
};

// --- Core Functions ---

// Initializes BGTK, connects to BGCE server, gets buffer/dimensions.
struct BGTK_Context* bgtk_init(void);

void bgtk_main_loop(struct BGTK_Context* ctx);

// Cleans up the BGTK context and resources.
void bgtk_destroy(struct BGTK_Context* ctx);

// --- Widget Creation Functions ---

// Creates a label widget.

struct BGTK_Widget* bgtk_label(struct BGTK_Context* ctx, char* text);
struct BGTK_Widget* bgtk_button(struct BGTK_Context* ctx,
				struct BGTK_Widget* text,
				BGTK_Callback callback, int flags);

// Adds a widget to the context with absolute positioning.
void bgtk_add_widget(struct BGTK_Context* ctx, struct BGTK_Widget* widget,
		     int x, int y, int w, int h);

// Creates a text widget (label) for use in other widgets (e.g., buttons).
struct BGTK_Widget* bgtk_text(struct BGTK_Context* ctx, char* text, int flags);

struct BGTK_Widget* bgtk_scrollable(struct BGTK_Context* ctx,
				    struct BGTK_Widget** widgets,
				    size_t widget_count, int flags);
#endif
