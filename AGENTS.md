# BGTK - Brian's GUI Toolkit - Agent Guide

## Project Overview

BGTK (Brian's GUI Toolkit) is a lightweight GUI toolkit for the BGCE display
server. It provides widgets and a rendering system for building graphical user
interfaces by writing directly to a graphical buffer. Real apps run against a
BGCE compositor on Linux; development and testing use a headless/mock mode that
works on Linux and macOS without BGCE.

## Codebase Structure

```
/
├── AGENTS.md         - This file, guidance for agents working on the project
├── bgtk.c            - Core BGTK implementation (init, events, mock mode, screenshots)
├── bgtk.h            - Public API headers
├── config.c / config.h - Configuration file parsing and theme/font settings
├── drawing.c         - Drawing and rendering functions
├── widgets.c         - Widget implementations
├── html.c / html.h   - HTML → widget tree parser (libxml2)
├── internal.h        - Internal headers and definitions
├── Makefile          - Build configuration (real apps + headless/test targets)
├── README.md         - Project overview and mock testing examples
├── stb_image.h       - STB image loading library
├── stb_image_write.h - STB image writing library
├── apps/             - Example / real applications
│   ├── test_app.c
│   ├── image_viewer.c
│   ├── launcher.c
│   ├── terminal.c / terminal.h / term_core.c
│   ├── gemini_browser.c
│   └── labyrinth.c   - Labyrinth web browser (HTML shell, bottom URL bar)
├── compat/           - Stubs/headers so headless builds work off Linux (bgce, input.h)
├── test/             - Headless/mock tests
│   ├── headless.c
│   ├── test_terminal.c
│   ├── test_html.c
│   ├── test_gemini_browser.c
│   ├── test_labyrinth.c
│   ├── server_client.sh
│   └── screenshots/  - PNG output from headless tests (gitignored)
└── www/              - Sample HTML for html tests
```

## Key Components

### 1. BGTK Context (`struct BGTK_Context`)
- Central structure holding application state
- Manages the connection to the display server (or an owned in-memory buffer in mock mode)
- Contains the root widget and focused widget
- Handles theme and font information (FreeType)

### 2. Widget System
- Base widget structure: `struct BGTK_Widget`
- Widget types: Button, Label, Text, Scrollable, List, Image, Frame, TextInput
- Each widget has:
  - Position and dimensions (x, y, w, h)
  - Styling properties (padding, margin, flags, text_align)
  - Type-specific data in a union
  - Event handling function (`handle_event`)
- Constructors take a `BGTK_Options` struct (padding, margin, flags, alignment, etc.)

### 3. Event Handling
- Events start at the root widget and propagate down to children
- Each widget can handle or pass through events
- Mouse and keyboard events are supported
- Focus management for text input via `bgtk_set_focus()`
- In mock/headless tests, feed events with `bgtk_inject_event()`

### 4. Drawing System
- Widgets are drawn to a shared memory buffer (`ctx->shm_buffer`)
- Drawing functions in `drawing.c`
- Supports text rendering with FreeType
- Handles widget layout and positioning
- `take_screenshot()` dumps the buffer to PNG for visual inspection

### 5. HTML Support
- `html.c` / `html.h` parse HTML (file or inline string) into a widget tree
- Entry points: `bgtk_html_parse()`, `bgtk_html_parse_inline()`
- Returns a frame widget; assign to `ctx->root_widget` before drawing
- **Labyrinth** (`apps/labyrinth.c`) is the HTML browser shell (gemini-like
  chrome: content + bottom URL bar). Fetches `https://` via **libtls** (same
  stack as gemini_browser). CSS/JS/`<a href>` click remain planned hooks.

### 6. Apps
- Real BGCE apps live under `apps/` (terminal, gemini_browser, labyrinth, launcher, image_viewer, sys_status, test_app)
- Terminal logic is split: `term_core.c` (shared) + `terminal.c` (real main) + `test/test_terminal.c` (headless main)

## Development Guidelines

- Create top quality code: optimize code for performance.
- Don't be verbose: prefer solutions with fewer lines of code.
- Keep good principles in mind: modular design, data structures and good
  organization matters, suggest if a refactor is beneficial.
- Keep imports lean: we should only need small and focused libraries.
- Avoid creating variables, use the fields from structs directly if possible.
- **Every new feature or behavior change must be tested** (see Testing below).

### Coding Style
- Follow existing code style (indentation, naming, etc.)
- Use defensive ifs so code is less indented.
- Use clear, descriptive variable and function names
- Keep functions focused on single responsibilities
- Add comments for complex logic
- Indent using tabs

### Widget Implementation
1. Add new widget type to `enum BGTK_Widget_Type`
2. Add widget-specific data to the union in `struct BGTK_Widget`
3. Implement creation function in `widgets.c`
4. Implement drawing logic in `drawing.c`
5. Implement event handler (or use default)
6. Add declaration to `bgtk.h`
7. Add or extend a headless test that screenshots the new widget

### Event Handling
- Each widget should implement `handle_event`
- Return 1 if event is handled, 0 otherwise
- For container widgets, propagate events to children
- Transform event coordinates for child widgets

## Common Tasks

### Adding a New Widget
1. Define the widget data structure in the union
2. Create a constructor function in `widgets.c`
3. Implement the drawing function in `drawing.c`
4. Implement event handling if needed
5. Add the constructor to `bgtk.h`
6. Write/extend a headless test; inspect the PNG screenshots

### Modifying Event Handling
1. Update widget-specific handlers in `widgets.c`
2. Modify `bgtk_handle_input_event` in `bgtk.c` if needed
3. Ensure proper event propagation
4. Inject events in a headless test and verify via screenshots

### Adding New Features
1. Add necessary data structures
2. Implement core functionality
3. Add configuration options if needed
4. **Add or update headless tests** (mocks + screenshots)
5. Update documentation (`README.md`, this file if structure/API changed)

### Debugging Tips
- Prefer headless/mock runs over attaching a real BGCE server
- Use `take_screenshot()` at intermediate steps and open the PNGs
- Use `printf` for debugging output
- Check widget bounds and positions
- Verify event coordinates (absolute at root, relative when propagating)

## Testing (required for features)

**Features must be tested.** Do not ship a new widget, rendering path, event
behavior, or app-facing capability without a headless/mock test that exercises
it and produces inspectable screenshots. If you change existing behavior,
update or extend the relevant test and re-inspect the PNGs.

### Methodology: mocks + screenshot inspection

BGTK is primarily validated visually, not via pixel-diff assertions. The flow:

1. **Init a mock context** — `bgtk_init_mock(width, height)` allocates an
   in-memory framebuffer. No BGCE server, DRM, or real input devices required.
2. **Build the widget tree** — same constructors and layout as a real app;
   set `ctx->root_widget`.
3. **Draw** — `bgtk_draw_widgets(ctx)`.
4. **Screenshot** — `take_screenshot(ctx, "descriptive_name.png")` writes the
   current buffer as PNG under `test/screenshots/` (bare basenames; pass `NULL`
   for a timestamped name there).
5. **Simulate input** (when relevant) — build a `struct InputEvent` (mouse
   click, key press, etc.) and call `bgtk_inject_event(ctx, ev)`, then draw
   and screenshot again.
6. **Inspect the PNGs** — open the generated images and verify layout, colors,
   text, focus, scrolling, terminal output, etc. This is the pass/fail gate:
   if the screenshot does not look correct, the feature is not done.
7. **Cleanup** — `bgtk_destroy_mock(ctx)`.

Typical test shape:

```c
struct BGTK_Context *ctx = bgtk_init_mock(600, 400);
ctx->root_widget = /* build UI */;
bgtk_draw_widgets(ctx);
take_screenshot(ctx, "feature_00_init.png");

struct InputEvent click = {
	.type = EV_KEY,
	.code = BTN_LEFT,
	.value = 1,
	.x = 120,
	.y = 80,
};
bgtk_inject_event(ctx, click);
take_screenshot(ctx, "feature_01_after_click.png");

bgtk_destroy_mock(ctx);
```

### Key mock / testing APIs

| Function | Role |
|----------|------|
| `bgtk_init_mock(w, h)` | Headless context with owned framebuffer |
| `bgtk_destroy_mock(ctx)` | Free mock context and its buffer |
| `bgtk_draw_widgets(ctx)` | Render the widget tree into the buffer |
| `take_screenshot(ctx, path)` | Dump buffer to PNG under `test/screenshots/` |
| `bgtk_inject_event(ctx, ev)` | Synthetic mouse/keyboard input |
| `bgtk_set_focus(ctx, widget)` | Focus (e.g. text input) before key events |

### Existing headless tests

Build/run examples (on macOS, `make` defaults to headless targets):

| Target | Source | What to inspect |
|--------|--------|-----------------|
| `make headless && ./headless` | `test/headless.c` | Basic widgets (`test/screenshots/headless_*.png`) |
| `make test_terminal && ./test_terminal` | `test/test_terminal.c` | Terminal/ANSI (`test/screenshots/term_*.png`) |
| `make test_html && ./test_html` | `test/test_html.c` | HTML → widgets (`test/screenshots/test_html_*.png`) |
| `make test_gemini_browser && ./test_gemini_browser` | `test/test_gemini_browser.c` | Gemini browser (`test/screenshots/gemini_browser_*.png`) |
| `make test_sys_status && ./test_sys_status` | `test/test_sys_status.c` | System status (`test/screenshots/sys_status_*.png`) |
| `make test_labyrinth && ./test_labyrinth` | `test/test_labyrinth.c` | Labyrinth web browser (`test/screenshots/labyrinth_*.png`) |

Headless binaries link `compat/bgce_stub.c` and use `-Icompat` so they compile
on non-Linux machines. Real apps (`test_app`, `terminal`, `gemini_browser`,
etc.) still need BGCE on Linux.

### Agent obligations when implementing features

1. Prefer extending an existing `test/*.c` program, or add a focused headless
   test binary if the feature is standalone.
2. Screenshot **before and after** meaningful state changes (init, after click,
   after text entry, after scroll, etc.) with clear sequential filenames
   (`feature_00_...`, `feature_01_...`).
3. **Read the PNGs** (via image/read tools or by path) and confirm the output
   matches intent — do not only check that the binary exited 0.
4. Keep test helpers small; mirror patterns in `test/headless.c` and
   `test/test_terminal.c` (shared core code in `apps/` when an app already
   has a real + test split, like the terminal).
5. The `make test` target (BGCE + live apps) is supplementary and Linux-only;
   **headless mock tests are the primary development and agent workflow**.

## Important Notes for Agents

1. **Event Coordinate System**: Pointer events use screen coordinates at the root.
   Hit-test with `bgtk_widget_hit()` (handles `BGTK_FLAG_RELATIVE`). Scrollables
   still transform events into content space for their children.
2. **Widget Hierarchy**: Root is `ctx->root_widget`. Containers set `child->parent`.
3. **Focus Management**: Text input widgets need special handling for keyboard events. Use `bgtk_set_focus()` to manage focus.
4. **Drawing**: Widgets are drawn to `ctx->shm_buffer`. Use the drawing functions in `drawing.c`.
5. **Memory Management**: Widgets are responsible for freeing their own resources in the destructor. Mock contexts are freed with `bgtk_destroy_mock()`, real ones with `bgtk_destroy()`.
6. **Configuration**: Theme and font settings are loaded from a config file at startup.
7. **Platform**: Full BGCE apps are Linux-oriented. Headless/mock builds are the default on macOS (`Makefile` sets `TARGET = headless test_terminal test_html` on Darwin).
8. **Layout flags** (optional; default is legacy absolute placement):
   - `BGTK_FLAG_EXPAND_X` / `BGTK_FLAG_EXPAND_Y` / `BGTK_FLAG_FILL` — grow into free space in a parent list (main axis shared among expanders; cross axis fills content box). Parent list needs a fixed/pre-set size larger than content (e.g. filled by a frame).
   - `BGTK_FLAG_RELATIVE` — `x`/`y` are parent-relative; `abs_x`/`abs_y` are screen (or scroll content) coords after layout. Set on a container to opt a subtree in; children inherit during place.
   - `bgtk_spacer(ctx, min_w, min_h, opts)` — empty flex cell; combine with EXPAND_* to push siblings (e.g. Apply to bottom).
   - `bgtk_widget_screen_pos()`, `bgtk_widget_hit()`, `bgtk_widget_set_parent()`, `bgtk_list_layout_expand()`.

## Useful Functions

### Runtime (real or mock)
- `bgtk_init()` / `bgtk_init_mock()`: Initialize the BGTK context
- `bgtk_destroy()` / `bgtk_destroy_mock()`: Clean up resources
- `bgtk_handle_input_event()` / `bgtk_inject_event()`: Handle or inject input
- `bgtk_draw_widgets()`: Render all widgets
- `bgtk_set_focus()`: Set the focused widget
- `take_screenshot()`: Write framebuffer to PNG
- `draw_widget()`: Draw a single widget
- `calculate_widget_size()`: Calculate widget dimensions and positions
- `bgtk_widget_screen_pos()` / `bgtk_widget_hit()`: Screen bounds after layout

### Widget constructors
- `bgtk_text()`, `bgtk_label()`, `bgtk_button()`, `bgtk_scrollable()`,
  `bgtk_list()`, `bgtk_image()`, `bgtk_frame()`, `bgtk_text_input()`

### HTML
- `bgtk_html_parse()`, `bgtk_html_parse_inline()`

This is a toolkit for the BGCE display server. It works by directly writing to
a graphical buffer; this library lets developers create user interfaces easily.
Headless/mock mode lets you develop and verify UIs by inspecting PNG screenshots
without a running compositor.
