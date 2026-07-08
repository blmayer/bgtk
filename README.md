# BGTK (Brian's Graphical Toolkit)

A lightweight toolkit for the BGCE display server. This library allows developers to
create graphical user interfaces by directly writing to a shared graphical buffer.


## Features
- Simple widget system (labels, buttons).
- Direct rendering to a shared memory buffer.
- Event handling for user input.
- Basic font rendering using FreeType.
- Theme support via config files (e.g., colors, fonts).


## Available Widgets
- Text: bgtk_text(ctx, text, options)
- Label: bgtk_label(ctx, text, options)
- Button: bgtk_button(ctx, label_widget, callback, options)
- Scrollable: bgtk_scrollable(ctx, items, widget_count, options)
- Image: bgtk_image(ctx, path, width, height, options)  (use 0,0 for intrinsic size)
- Frame: bgtk_frame(ctx, child, width, height, options)
- Text input: bgtk_text_input(ctx, initial_text, width, height, options)

## Key Testing APIs

When using mocks, the following functions are the primary tools:

- `bgtk_init_mock(width, height)` — create a context with its own in-memory framebuffer.
- `take_screenshot(ctx, "name.png")` — dump the current buffer to a PNG (pass `NULL` for a timestamped name).
- `bgtk_inject_event(ctx, ev)` — feed synthetic mouse/keyboard events.
- `bgtk_destroy_mock(ctx)` — clean up a mock context (frees the internal buffer).

## Testing with Mocks (Headless Mode)

BGTK supports a mock/headless mode so you can develop, debug, and visually inspect UIs **without** a running BGCE server, real display, or input devices.

This is the recommended way to test widgets in isolation:

```c
struct BGTK_Context *ctx = bgtk_init_mock(600, 400);

// Build your widget tree exactly as you would in a real app
ctx->root_widget = my_ui_builder(ctx);
bgtk_draw_widgets(ctx);

// Dump the current rendered state to a PNG you can open in any viewer
take_screenshot(ctx, "before.png");

// Simulate user input (mouse clicks, keyboard, etc.)
struct InputEvent click = {
    .type = EV_KEY,
    .code = BTN_LEFT,
    .value = 1,
    .x = 120,
    .y = 80,
};
bgtk_inject_event(ctx, click);

take_screenshot(ctx, "after_click.png");

bgtk_destroy_mock(ctx);
```

### Built-in headless test

```sh
make headless
./headless
```

This builds `test/headless.c` and generates several `headless_*.png` files demonstrating:
- Layout and widget sizing
- Button press / callback
- Text input focus + typing (via injected key events)
- Backspace, etc.

Open the PNGs to see exactly what the UI looked like at each step. This is extremely useful for catching layout or rendering bugs quickly on a development machine (works on both Linux and macOS).

No `bgce` process or special permissions are required.

## Configuration

BGTK is configured via `~/.config/bgtk.conf`. Example with all options and their defaults (also used when the file is missing):

```ini
# Background settings
[background]
type = color
color = #E8E8E8
# Or use an image:
# type = image
# path = /path/to/wallpaper.png
# mode = tiled

# Theme colors and border sizes
[theme]
background = #E8E8E8
button = #D0D0D0
button_text = #111111
button_border_size = 1
input_border_size = 2
frame_border_size = 4
frame_border_color = #333333

# Font settings (sans = UI; mono = terminal; serif = documents)
[font]
sans = /usr/share/fonts/truetype/dejavu/DejaVuSans.ttf
mono = /usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf
serif = /usr/share/fonts/truetype/dejavu/DejaVuSerif.ttf
size = 14
```

`sans` is the UI face. `mono` is used by the terminal; `serif` is available via `bgtk_font_face(ctx, BGTK_FONT_SERIF)`. If mono/serif are omitted, platform defaults are chosen (falling back to the UI/sans font).

Colors use `#RRGGBB` or `#RRGGBBAA` format. Lines starting with `#` or `;` are comments.

### Logging

Each process writes a dedicated log under `~/.cache/bgtk/` (or `$XDG_CACHE_HOME/bgtk/`), separate from BGCE server logs:

| Process | Log file |
|---------|----------|
| library fallback | `~/.cache/bgtk/bgtk.log` |
| `terminal` | `~/.cache/bgtk/terminal.log` |
| `launcher` | `~/.cache/bgtk/launcher.log` |
| `test_app` | `~/.cache/bgtk/test_app.log` |
| … | `~/.cache/bgtk/<app_name>.log` |

Apps call `bgtk_log_open("app_name")` at startup; use `bgtk_log()` / `bgtk_log_errno()` for diagnostics.


## Download

A source snapshot is published with the project site:

- **[bgtk.tar.gz](https://terminal.pink/bgtk/bgtk.tar.gz)** — current tree (no git history)

```bash
curl -fsSL -o bgtk.tar.gz https://terminal.pink/bgtk/bgtk.tar.gz
tar xzf bgtk.tar.gz
cd bgtk
```

Or clone the repository if you prefer full history:

```bash
git clone https://terminal.pink/bgtk
cd bgtk
```

Regenerate the site snapshot after tagging a release (or anytime):

```bash
git archive --worktree-attributes --format=tar.gz --prefix=bgtk/ -o www/bgtk.tar.gz HEAD
```

(`www/bgtk.tar.gz` is omitted from the archive via `.gitattributes`.)


## Building

Requirements:
- A C compiler (`cc` on lin0 / TinyCC, or GCC/Clang elsewhere).
- FreeType and libxml2 libraries/headers.
- BGCE (`libbgce`, `bgce.h`) for real apps.

On lin0 (flat `/bin` `/lib` `/include`, compiler `/bin/cc`):

```sh
make CC=cc
make install
```

Defaults install to `/lib` and `/include` (override with `INSTALL_LIB` / `INSTALL_INCLUDE` / `INSTALL_BIN`). A running BGCE server is only required for real applications; `make headless` does not need BGCE.


## Running

### Real applications (with BGCE)

You need a running BGCE compositor. See the example programs in `apps/` (e.g. `image_viewer`, `launcher`, `sys_status`, `test_app`).

### Headless testing / development (recommended for UI work)

No server or display needed. Use the built-in test:

```sh
make headless
./headless
```

This produces `headless_*.png` files you can open to visually inspect the rendered UI at each step (layout, interaction, text input, etc.).

See the "Testing with Mocks" section above for how to integrate this style of testing into your own code.


## Project Structure
- `bgtk.h`: Public API and type definitions.
- `bgtk.c`, `drawing.c`, `widgets.c`, `config.c`: Core implementation.
- `apps/`: Example real applications (require BGCE).
- `test/headless.c`: Standalone headless test (no BGCE required). Produces PNG snapshots.
- `Makefile`: Build system (including `make headless`).
- `.clang-format`: Code style configuration.

