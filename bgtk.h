#ifndef BGTK_H
#define BGTK_H

#include <ft2build.h>
#include <stddef.h>
#include <stdint.h>
#include FT_FREETYPE_H
#include <bgce.h>

#include "config.h"

// CSS named colors (as 0xAARRGGBB with full alpha). Easy to use for
// widget styling, themes, etc. Not exhaustive but covers the most common
// ones from the CSS Color Module. Add more as needed.
enum BGTK_Color {
    BGTK_COLOR_ALICEBLUE            = 0xFFF0F8FF,
    BGTK_COLOR_ANTIQUEWHITE         = 0xFFFAEBD7,
    BGTK_COLOR_AQUA                 = 0xFF00FFFF,
    BGTK_COLOR_AQUAMARINE           = 0xFF7FFFD4,
    BGTK_COLOR_AZURE                = 0xFFF0FFFF,
    BGTK_COLOR_BEIGE                = 0xFFF5F5DC,
    BGTK_COLOR_BISQUE               = 0xFFFFE4C4,
    BGTK_COLOR_BLACK                = 0xFF000000,
    BGTK_COLOR_BLANCHEDALMOND       = 0xFFFFEBCD,
    BGTK_COLOR_BLUE                 = 0xFF0000FF,
    BGTK_COLOR_BLUEVIOLET           = 0xFF8A2BE2,
    BGTK_COLOR_BROWN                = 0xFFA52A2A,
    BGTK_COLOR_BURLYWOOD            = 0xFFDEB887,
    BGTK_COLOR_CADETBLUE            = 0xFF5F9EA0,
    BGTK_COLOR_CHARTREUSE           = 0xFF7FFF00,
    BGTK_COLOR_CHOCOLATE            = 0xFFD2691E,
    BGTK_COLOR_CORAL                = 0xFFFF7F50,
    BGTK_COLOR_CORNFLOWERBLUE       = 0xFF6495ED,
    BGTK_COLOR_CORNSILK             = 0xFFFFF8DC,
    BGTK_COLOR_CRIMSON              = 0xFFDC143C,
    BGTK_COLOR_CYAN                 = 0xFF00FFFF,
    BGTK_COLOR_DARKBLUE             = 0xFF00008B,
    BGTK_COLOR_DARKCYAN             = 0xFF008B8B,
    BGTK_COLOR_DARKGOLDENROD        = 0xFFB8860B,
    BGTK_COLOR_DARKGRAY             = 0xFFA9A9A9,
    BGTK_COLOR_DARKGREEN            = 0xFF006400,
    BGTK_COLOR_DARKGREY             = 0xFFA9A9A9,
    BGTK_COLOR_DARKKHAKI            = 0xFFBDB76B,
    BGTK_COLOR_DARKMAGENTA          = 0xFF8B008B,
    BGTK_COLOR_DARKOLIVEGREEN       = 0xFF556B2F,
    BGTK_COLOR_DARKORANGE           = 0xFFFF8C00,
    BGTK_COLOR_DARKORCHID           = 0xFF9932CC,
    BGTK_COLOR_DARKRED              = 0xFF8B0000,
    BGTK_COLOR_DARKSALMON           = 0xFFE9967A,
    BGTK_COLOR_DARKSEAGREEN         = 0xFF8FBC8F,
    BGTK_COLOR_DARKSLATEBLUE        = 0xFF483D8B,
    BGTK_COLOR_DARKSLATEGRAY        = 0xFF2F4F4F,
    BGTK_COLOR_DARKSLATEGREY        = 0xFF2F4F4F,
    BGTK_COLOR_DARKTURQUOISE        = 0xFF00CED1,
    BGTK_COLOR_DARKVIOLET           = 0xFF9400D3,
    BGTK_COLOR_DEEPPINK             = 0xFFFF1493,
    BGTK_COLOR_DEEPSKYBLUE          = 0xFF00BFFF,
    BGTK_COLOR_DIMGRAY              = 0xFF696969,
    BGTK_COLOR_DIMGREY              = 0xFF696969,
    BGTK_COLOR_DODGERBLUE           = 0xFF1E90FF,
    BGTK_COLOR_FIREBRICK            = 0xFFB22222,
    BGTK_COLOR_FLORALWHITE          = 0xFFFFFAF0,
    BGTK_COLOR_FORESTGREEN          = 0xFF228B22,
    BGTK_COLOR_FUCHSIA              = 0xFFFF00FF,
    BGTK_COLOR_GAINSBORO            = 0xFFDCDCDC,
    BGTK_COLOR_GHOSTWHITE           = 0xFFF8F8FF,
    BGTK_COLOR_GOLD                 = 0xFFFFD700,
    BGTK_COLOR_GOLDENROD            = 0xFFDAA520,
    BGTK_COLOR_GRAY                 = 0xFF808080,
    BGTK_COLOR_GREEN                = 0xFF008000,
    BGTK_COLOR_GREENYELLOW          = 0xFFADFF2F,
    BGTK_COLOR_GREY                 = 0xFF808080,
    BGTK_COLOR_HONEYDEW             = 0xFFF0FFF0,
    BGTK_COLOR_HOTPINK              = 0xFFFF69B4,
    BGTK_COLOR_INDIANRED            = 0xFFCD5C5C,
    BGTK_COLOR_INDIGO               = 0xFF4B0082,
    BGTK_COLOR_IVORY                = 0xFFFFFFF0,
    BGTK_COLOR_KHAKI                = 0xFFF0E68C,
    BGTK_COLOR_LAVENDER             = 0xFFE6E6FA,
    BGTK_COLOR_LAVENDERBLUSH        = 0xFFFFF0F5,
    BGTK_COLOR_LAWNGREEN            = 0xFF7CFC00,
    BGTK_COLOR_LEMONCHIFFON         = 0xFFFFFACD,
    BGTK_COLOR_LIGHTBLUE            = 0xFFADD8E6,
    BGTK_COLOR_LIGHTCORAL           = 0xFFF08080,
    BGTK_COLOR_LIGHTCYAN            = 0xFFE0FFFF,
    BGTK_COLOR_LIGHTGOLDENRODYELLOW = 0xFFFAFAD2,
    BGTK_COLOR_LIGHTGRAY            = 0xFFD3D3D3,
    BGTK_COLOR_LIGHTGREEN           = 0xFF90EE90,
    BGTK_COLOR_LIGHTGREY            = 0xFFD3D3D3,
    BGTK_COLOR_LIGHTPINK            = 0xFFFFB6C1,
    BGTK_COLOR_LIGHTSALMON          = 0xFFFFA07A,
    BGTK_COLOR_LIGHTSEAGREEN        = 0xFF20B2AA,
    BGTK_COLOR_LIGHTSKYBLUE         = 0xFF87CEFA,
    BGTK_COLOR_LIGHTSLATEGRAY       = 0xFF778899,
    BGTK_COLOR_LIGHTSLATEGREY       = 0xFF778899,
    BGTK_COLOR_LIGHTSTEELBLUE       = 0xFFB0C4DE,
    BGTK_COLOR_LIGHTYELLOW          = 0xFFFFFFE0,
    BGTK_COLOR_LIME                 = 0xFF00FF00,
    BGTK_COLOR_LIMEGREEN            = 0xFF32CD32,
    BGTK_COLOR_LINEN                = 0xFFFAF0E6,
    BGTK_COLOR_MAGENTA              = 0xFFFF00FF,
    BGTK_COLOR_MAROON               = 0xFF800000,
    BGTK_COLOR_MEDIUMAQUAMARINE     = 0xFF66CDAA,
    BGTK_COLOR_MEDIUMBLUE           = 0xFF0000CD,
    BGTK_COLOR_MEDIUMORCHID         = 0xFFBA55D3,
    BGTK_COLOR_MEDIUMPURPLE         = 0xFF9370DB,
    BGTK_COLOR_MEDIUMSEAGREEN       = 0xFF3CB371,
    BGTK_COLOR_MEDIUMSLATEBLUE      = 0xFF7B68EE,
    BGTK_COLOR_MEDIUMSPRINGGREEN    = 0xFF00FA9A,
    BGTK_COLOR_MEDIUMTURQUOISE      = 0xFF48D1CC,
    BGTK_COLOR_MEDIUMVIOLETRED      = 0xFFC71585,
    BGTK_COLOR_MIDNIGHTBLUE         = 0xFF191970,
    BGTK_COLOR_MINTCREAM            = 0xFFF5FFFA,
    BGTK_COLOR_MISTYROSE            = 0xFFFFE4E1,
    BGTK_COLOR_MOCCASIN             = 0xFFFFE4B5,
    BGTK_COLOR_NAVAJOWHITE          = 0xFFFFDEAD,
    BGTK_COLOR_NAVY                 = 0xFF000080,
    BGTK_COLOR_OLDLACE              = 0xFFFDF5E6,
    BGTK_COLOR_OLIVE                = 0xFF808000,
    BGTK_COLOR_OLIVEDRAB            = 0xFF6B8E23,
    BGTK_COLOR_ORANGE               = 0xFFFFA500,
    BGTK_COLOR_ORANGERED            = 0xFFFF4500,
    BGTK_COLOR_ORCHID               = 0xFFDA70D6,
    BGTK_COLOR_PALEGOLDENROD        = 0xFFEEE8AA,
    BGTK_COLOR_PALEGREEN            = 0xFF98FB98,
    BGTK_COLOR_PALETURQUOISE        = 0xFFAFEEEE,
    BGTK_COLOR_PALEVIOLETRED        = 0xFFDB7093,
    BGTK_COLOR_PAPAYAWHIP           = 0xFFFFEFD5,
    BGTK_COLOR_PEACHPUFF            = 0xFFFFDAB9,
    BGTK_COLOR_PERU                 = 0xFFCD853F,
    BGTK_COLOR_PINK                 = 0xFFFFC0CB,
    BGTK_COLOR_PLUM                 = 0xFFDDA0DD,
    BGTK_COLOR_POWDERBLUE           = 0xFFB0E0E6,
    BGTK_COLOR_PURPLE               = 0xFF800080,
    BGTK_COLOR_REBECCAPURPLE        = 0xFF663399,
    BGTK_COLOR_RED                  = 0xFFFF0000,
    BGTK_COLOR_ROSYBROWN            = 0xFFBC8F8F,
    BGTK_COLOR_ROYALBLUE            = 0xFF4169E1,
    BGTK_COLOR_SADDLEBROWN          = 0xFF8B4513,
    BGTK_COLOR_SALMON               = 0xFFFA8072,
    BGTK_COLOR_SANDYBROWN           = 0xFFF4A460,
    BGTK_COLOR_SEAGREEN             = 0xFF2E8B57,
    BGTK_COLOR_SEASHELL             = 0xFFFFF5EE,
    BGTK_COLOR_SIENNA               = 0xFFA0522D,
    BGTK_COLOR_SILVER               = 0xFFC0C0C0,
    BGTK_COLOR_SKYBLUE              = 0xFF87CEEB,
    BGTK_COLOR_SLATEBLUE            = 0xFF6A5ACD,
    BGTK_COLOR_SLATEGRAY            = 0xFF708090,
    BGTK_COLOR_SLATEGREY            = 0xFF708090,
    BGTK_COLOR_SNOW                 = 0xFFFFFAFA,
    BGTK_COLOR_SPRINGGREEN          = 0xFF00FF7F,
    BGTK_COLOR_STEELBLUE            = 0xFF4682B4,
    BGTK_COLOR_TAN                  = 0xFFD2B48C,
    BGTK_COLOR_TEAL                 = 0xFF008080,
    BGTK_COLOR_THISTLE              = 0xFFD8BFD8,
    BGTK_COLOR_TOMATO               = 0xFFFF6347,
    BGTK_COLOR_TURQUOISE            = 0xFF40E0D0,
    BGTK_COLOR_VIOLET               = 0xFFEE82EE,
    BGTK_COLOR_WHEAT                = 0xFFF5DEB3,
    BGTK_COLOR_WHITE                = 0xFFFFFFFF,
    BGTK_COLOR_WHITESMOKE           = 0xFFF5F5F5,
    BGTK_COLOR_YELLOW               = 0xFFFFFF00,
    BGTK_COLOR_YELLOWGREEN          = 0xFF9ACD32,
};

// Function pointer for button callbacks
typedef void (*BGTK_Callback)(void *userdata);

// BGTK_Context: Holds the state of the BGTK application
struct BGTK_Context {
	int conn_fd;  // File descriptor for BGCE connection
	void* shm_buffer;
	// 1 = shm_buffer is mmap'd (real BGCE); 0 = malloc'd (mock).
	// Used so resize can munmap/free the previous mapping correctly.
	int buffer_mapped;
	int width;
	int height;
	/* Staging buffer for atomic present (real BGCE only). Avoids
	 * mid-frame black flashes while painting into live shm. */
	uint32_t *draw_back;
	int draw_back_n; /* capacity in pixels */

	// FreeType data (ft_face is the UI/sans face; mono/serif optional)
	FT_Library ft_library;
	FT_Face ft_face;
	FT_Face ft_face_mono;
	FT_Face ft_face_serif;
	int font_size;

	// Theme data
	BGTK_Theme theme;
	char font_sans_path[MAX_PATH_LEN];
	char font_mono_path[MAX_PATH_LEN];
	char font_serif_path[MAX_PATH_LEN];

	// Single root widget for the widget tree
	struct BGTK_Widget* root_widget;

	// Currently focused widget (for keyboard input)
	struct BGTK_Widget* focused_widget;

	// Whether the window/surface is focused according to the server.
	// 0 = unfocused, 1 = focused.
	int window_focused;

	/* Modifier state: bit0=left, bit1=right (see bgtk_update_modifiers).
	 * Non-zero means that mod is active. Cleared on focus change. */
	int shift_held;
	int ctrl_held;
	int alt_held;
};

/* Modifier bitflags for bgtk_key_to_bytes */
enum {
	BGTK_MOD_SHIFT = 1,
	BGTK_MOD_CTRL = 2,
	BGTK_MOD_ALT = 4
};

/* Key translation mode */
enum {
	BGTK_KEY_TEXT = 0, /* printable for text fields; Ctrl+letter → 0 */
	BGTK_KEY_TTY = 1   /* PTY: Ctrl+letter → C0, arrows → CSI */
};

// Track Shift/Ctrl/Alt from an EV_KEY event (press/release/repeat).
// Left/right keys are tracked separately (sticky-mod safe).
void bgtk_update_modifiers(struct BGTK_Context *ctx, struct InputEvent ev);
// Build mod bitflags from context held state.
int bgtk_mods_from_ctx(const struct BGTK_Context *ctx);
// Clear all mod bits (call on window focus change).
void bgtk_clear_modifiers(struct BGTK_Context *ctx);
// US QWERTY keycode → bytes. Returns length written (0 if unmapped).
int bgtk_key_to_bytes(int code, int mods, int mode, char *out, int max);

// True if this key should exit a normal GUI app: Esc, or Ctrl+C.
// Call after bgtk_update_modifiers (or bgtk_handle_input_event's mod update).
// Do NOT use in the terminal emulator (Ctrl+C must go to the PTY).
int bgtk_is_app_quit_event(const struct BGTK_Context *ctx, struct InputEvent ev);

// Sets the focused widget for keyboard input.
// Passing NULL clears focus.
void bgtk_set_focus(struct BGTK_Context* ctx, struct BGTK_Widget* widget);

// Sets window focus state (from server focus events).
// Also clears shift/ctrl/alt held flags — releases while unfocused are lost.
void bgtk_set_window_focus(struct BGTK_Context* ctx, int focused);

/* Font roles for multi-face support (sans = UI default). */
enum {
	BGTK_FONT_SANS = 0,
	BGTK_FONT_MONO = 1,
	BGTK_FONT_SERIF = 2
};

/* Face for role, falling back to UI/sans (ft_face) if that role is missing. */
FT_Face bgtk_font_face(struct BGTK_Context *ctx, int role);


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
	BGTK_WIDGET_RULE,
	/* Binary pill: left / right labels + sliding knob (0 = left, 1 = right). */
	BGTK_WIDGET_SWITCH,
	/* Empty flex space; pair with BGTK_FLAG_EXPAND_* to push siblings. */
	BGTK_WIDGET_SPACER,
};

// Widget flags
#define BGTK_FLAG_CENTER (1 << 0) /* Center children on cross-axis (lists). */
/* Grow into free space along parent list main/cross axis (see layout). */
#define BGTK_FLAG_EXPAND_X (1 << 1)
#define BGTK_FLAG_EXPAND_Y (1 << 2)
#define BGTK_FLAG_FILL (BGTK_FLAG_EXPAND_X | BGTK_FLAG_EXPAND_Y)
/* x/y are parent-relative; abs_x/abs_y hold screen position after layout. */
#define BGTK_FLAG_RELATIVE (1 << 3)

/* Text style bits (bgtk_text / BGTK_Options.text_style). Synthetic via FreeType. */
#define BGTK_TEXT_BOLD   (1 << 0)
#define BGTK_TEXT_ITALIC (1 << 1)

// Text alignment within a widget's content area (like CSS text-align)
enum BGTK_Text_Align {
	BGTK_ALIGN_LEFT = 0,
	BGTK_ALIGN_CENTER,
	BGTK_ALIGN_RIGHT,
};

/* Vertical placement of text within the widget content box. */
enum BGTK_VAlign {
	BGTK_VALIGN_TOP = 0,
	BGTK_VALIGN_CENTER,
	BGTK_VALIGN_BOTTOM,
};

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
	enum BGTK_Text_Align text_align;  // Horizontal text alignment (default: left)
	enum BGTK_VAlign text_v_align;    // Vertical text alignment (default: top)
	int baseline_offset; // Added to FreeType baseline (px; can be negative)
	enum BGTK_List_Orientation orientation;  // For list/rule: vertical or horizontal
	int text_style; // BGTK_TEXT_BOLD | BGTK_TEXT_ITALIC (text widgets)
} BGTK_Options;

// BGTK_Widget: Base structure for all widgets
struct BGTK_Widget {
	struct BGTK_Context* ctx;
	enum BGTK_Widget_Type type;
	/* Position: absolute by default. With BGTK_FLAG_RELATIVE, x/y are
	 * parent-relative and abs_x/abs_y are the screen origin after layout. */
	int x, y, w, h;
	int abs_x, abs_y;
	struct BGTK_Widget *parent; /* set by list/frame/scrollable constructors */
	int flags;          // Flags for widget behavior
	int padding;        // Internal spacing (pixels)
	int margin;         // External spacing (pixels)
	enum BGTK_Text_Align text_align;  // Horizontal text alignment
	enum BGTK_VAlign text_v_align;    // Vertical text alignment
	int baseline_offset;              // FreeType baseline tweak (px)
	/* CSS / explicit colors: 0 = use theme default (no override). */
	uint32_t color_fg;
	uint32_t color_bg;

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
			void *cb_data;  // user data forwarded to callback
			int pressed;  // non-zero while mouse button is held down on this widget
			uint32_t bg_override;  // 0 = use theme, non-zero = custom bg color
			/* -1 = theme.button_border_size; >=0 forces this border width. */
			int border_w;
		} button;
		struct {
			char* text;
			int header_level;  // 0=normal, 1/2/3 headers; 10=accent line
			int style;         // BGTK_TEXT_BOLD | BGTK_TEXT_ITALIC
			/* Optional owned link target for HTML <a href>. */
			char *href;
			/* BGTK_FONT_SANS/MONO/SERIF — which face to measure/draw. */
			int font_role;
		} text;
		struct {
			enum BGTK_List_Orientation orientation;
			int thickness;     // line width in px (>=1)
			uint32_t color;    // 0 = theme.rule_color
		} rule;
		struct {
			uint32_t* pixels;  // Pixel buffer for image
			int img_w;  // Image intrinsic width
			int img_h;  // Image intrinsic height
		} image;
		struct {
			struct BGTK_Widget** items;  // List of child widgets
			int widget_count;
			int widget_capacity; /* pixel count of tmp, not item slots */
			int scroll_y;	     // Vertical scroll (px into content)
			int scroll_x;	     // Horizontal scroll (Shift+wheel)
			int content_height;  // Total height of all child widgets
			int content_width;   // Total width (≥ view w; wide tables/pre)
			uint32_t* tmp;	     // off-screen buffer
			/* Identity of content last painted into tmp — if items
			 * are swapped (settings sidebar) without freeing tmp,
			 * cache must not be reused (buttons would not hit). */
			struct BGTK_Widget **tmp_items;
			struct BGTK_Widget *tmp_item0;
			int tmp_nitems;
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
			uint32_t border_color; // 0 = use theme default
		} frame;
		struct {
			char* text;
			uint32_t cursor_pos;
			int selection_start;
			int selection_end;
			int scroll_x;
			/* -1 = theme.input_border_size; 0 = no border (minimal). */
			int border_w;
			void (*on_change)(void);
			void (*on_tab)(void);
			void (*on_enter)(void);
		} text_input;
		struct {
			char *left;   /* owned */
			char *right;  /* owned */
			int value;    /* 0 = left, 1 = right */
			BGTK_Callback callback;
			void *cb_data;
		} switch_w;
	} data;  // End of union
};  // End of BGTK_Widget struct

/* Free a widget subtree (safe on NULL). Clears ctx focus if it was inside. */
void bgtk_widget_destroy(struct BGTK_Widget *w);

/* Screen-space origin after the last layout/draw (handles RELATIVE). */
void bgtk_widget_screen_pos(const struct BGTK_Widget *w, int *x, int *y);
/* 1 if screen point (x,y) is inside the widget's outer box. */
int bgtk_widget_hit(const struct BGTK_Widget *w, int x, int y);
/* Link child→parent (constructors do this; use when re-homing a subtree). */
void bgtk_widget_set_parent(struct BGTK_Widget *child, struct BGTK_Widget *parent);
/* Apply EXPAND_X/Y / FILL to list children using list's current w/h. */
void bgtk_list_layout_expand(struct BGTK_Widget *list);

// --- Logging (dedicated files under ~/.cache/bgtk/, not shared with BGCE) ---
// Open $XDG_CACHE_HOME/bgtk/<app_name>.log (or ~/.cache/bgtk/...). Creates dirs.
// Call once at process start with a per-app name (e.g. "terminal", "launcher").
// Installs SIGSEGV/ABRT/BUS/FPE/ILL handlers that append a last line to the log.
// Library code uses "bgtk" if nothing was opened yet.
void bgtk_log_open(const char *app_name);
// Timestamped line to the app log (and stderr). Safe before bgtk_log_open.
void bgtk_log(const char *fmt, ...);
// Like bgtk_log but appends ": <strerror(errno)>" for the current errno.
void bgtk_log_errno(const char *fmt, ...);
// Force flush log file + stderr (call before risky work or after fatal errors).
void bgtk_log_flush(void);
// Path of the open log file, or NULL if logging only to stderr.
const char *bgtk_log_path(void);
// Log a fatal line, flush, then exit(status). Use instead of silent abort.
void bgtk_log_die(int status, const char *fmt, ...);

// --- Core Functions ---

void bgtk_draw_widgets(struct BGTK_Context* ctx);

void bgtk_destroy(struct BGTK_Context* ctx);
void bgtk_destroy_mock(struct BGTK_Context* ctx);

// Initializes BGTK with given dimensions (real server path, caller provides buffer).
struct BGTK_Context* bgtk_init(int conn_fd, void* buffer, int width, int height);

// Initializes BGTK in mock/headless mode for testing. Owns an internal framebuffer.
// Use take_screenshot(ctx, "foo.png") after draws (writes under test/screenshots/).
struct BGTK_Context* bgtk_init_mock(int width, int height);

// Handles a single event and returns whether a redraw is needed.
int bgtk_handle_input_event(struct BGTK_Context* ctx, struct InputEvent ev);

// Take a screenshot of the current framebuffer to a PNG file.
// If path is NULL, a timestamped name is written under BGTK_TEST_SCREENSHOT_DIR
// (for KEY_SYSRQ / convenience). A bare basename (no '/') is also placed there so
// headless tests do not drop PNGs in the repo root. Paths with '/' are used as-is.
// Parent directories are created as needed.
#define BGTK_TEST_SCREENSHOT_DIR "test/screenshots"
int take_screenshot(struct BGTK_Context* ctx, const char* path);

// Inject a synthetic input event (for testing). Coordinates are absolute widget coords.
// Returns non-zero if a redraw was triggered.
int bgtk_inject_event(struct BGTK_Context* ctx, struct InputEvent ev);

// Handle MSG_BUFFER_CHANGE from BGCE: unmap the old framebuffer, mmap the new
// one from reply->shm_name, update ctx size, and size the root widget to the
// new window. Does not draw — call bgtk_draw_widgets() after any app-specific
// reflow. Returns 0 on success, -1 on failure.
int bgtk_handle_buffer_change(struct BGTK_Context *ctx,
			      const struct BufferReply *reply);

// Mock/test helper: reallocate the owned framebuffer to a new size and size
// the root widget. Returns 0 on success.
int bgtk_resize_mock(struct BGTK_Context *ctx, int width, int height);

/* Reload FreeType faces from paths (NULL path = keep current for that role).
 * size <= 0 keeps current font_size. Returns 0 if at least sans loaded. */
int bgtk_reload_fonts(struct BGTK_Context *ctx, const char *sans,
		      const char *mono, const char *serif, int size);

// --- Widget Creation Functions ---
// Creates a label widget.
struct BGTK_Widget* bgtk_label(struct BGTK_Context* ctx, char* text, BGTK_Options options);
struct BGTK_Widget* bgtk_button(struct BGTK_Context* ctx,
			struct BGTK_Widget* text,
			BGTK_Callback callback, void *cb_data,
			BGTK_Options options);

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

/* Divider line. orientation: BGTK_LIST_HORIZONTAL = bar across width,
 * BGTK_LIST_VERTICAL = bar down height. thickness < 1 → 1.
 * Long axis is usually set by the parent (list/frame); short axis = thickness. */
struct BGTK_Widget *bgtk_rule(struct BGTK_Context *ctx,
			      enum BGTK_List_Orientation orientation,
			      int thickness, BGTK_Options options);

/* Binary switch / pill: value 0 selects left label, 1 selects right.
 * Click left/right half of the track; callback fires after value changes. */
struct BGTK_Widget *bgtk_switch(struct BGTK_Context *ctx, const char *left,
				const char *right, int value,
				BGTK_Callback callback, void *cb_data,
				BGTK_Options options);

/* Invisible box of at least min_w × min_h; use EXPAND_X/Y/FILL to stretch. */
struct BGTK_Widget *bgtk_spacer(struct BGTK_Context *ctx, int min_w, int min_h,
				BGTK_Options options);

#endif
