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
## Themes
Theme config files allow customizing the appearance of BGTK applications.
To use a theme:
1. Create a config file in `~/.config/bgtk.conf`.
2. Define theme properties like colors and fonts in key-value pairs:

```
background_color = #282828
text_color = #ebdbb2
font = /opt/fonts/.../DejaVu Sans.otf
```


## Building

Requirements:
- A C compiler (GCC or Clang).
- FreeType development libraries.
- BGCE.

```sh
make
```


## Running

Start the BGCE server, then run the demo application:

```sh
./app
```


## Project Structure
- `bgtk.h`: Public API and type definitions.
- `bgtk.c`: Core implementation.
- `app.c`: Demo application.
- `Makefile`: Build system.
- `.clang-format`: Code style configuration.

