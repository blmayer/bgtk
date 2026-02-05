# BGTK - Brian's GUI Toolkit - Agent Guide

## Project Overview

BGTK (Brian's GUI Toolkit) is a lightweight GUI toolkit designed for Linux systems. It provides a set of widgets and a rendering system for building graphical user interfaces.

## Codebase Structure

```
/
├── AGENTS.md         - This file, guidance for agents working on the project
├── app.c             - Test application entry point
├── bgtk.c            - Core BGTK implementation
├── bgtk.h            - Public API headers
├── config.c          - Configuration file parsing
├── config.h          - Configuration definitions
├── drawing.c         - Drawing and rendering functions
├── internal.h        - Internal headers and definitions
├── Makefile          - Build configuration
├── README.md         - Project overview
├── stb_image.h       - STB image loading library
├── stb_image_write.h - STB image writing library
├── test/             - Test scripts and utilities
└── widgets.c         - Widget implementations
```

## Key Components

### 1. BGTK Context (`struct BGTK_Context`)
- Central structure holding application state
- Manages the connection to the display server
- Contains the root widget and focused widget
- Handles theme and font information

### 2. Widget System
- Base widget structure: `struct BGTK_Widget`
- Widget types: Button, Label, Text, Scrollable, Image, Frame, TextInput
- Each widget has:
  - Position and dimensions (x, y, w, h)
  - Styling properties (padding, margin, flags)
  - Type-specific data in a union
  - Event handling function (`handle_event`)

### 3. Event Handling
- Events start at the root widget and propagate down to children
- Each widget can handle or pass through events
- Mouse and keyboard events are supported
- Focus management for text input

### 4. Drawing System
- Widgets are drawn to a shared memory buffer
- Drawing functions in `drawing.c`
- Supports text rendering with FreeType
- Handles widget layout and positioning

## Development Guidelines

- Create top quality code: optimize code for performance.
- Don't be verbose: prefer solutions with fewer lines of code.
- Keep good principles in mind: modular design, data structures and good
  organization matters, suggest if a refactor is beneficial.
- Keep imports lean: we should only need small and focused libraries.
 
### Coding Style
- Follow existing code style (indentation, naming, etc.)
- Use defensive ifs so code is less indented.
- Use clear, descriptive variable and function names
- Keep functions focused on single responsibilities
- Add comments for complex logic
- Ident using tabs

### Widget Implementation
1. Add new widget type to `enum BGTK_Widget_Type`
2. Add widget-specific data to the union in `struct BGTK_Widget`
3. Implement creation function in `widgets.c`
4. Implement drawing logic in `drawing.c`
5. Implement event handler (or use default)
6. Add declaration to `bgtk.h`

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

### Modifying Event Handling
1. Update widget-specific handlers in `widgets.c`
2. Modify `bgtk_handle_input_event` in `bgtk.c` if needed
3. Ensure proper event propagation

### Adding New Features
1. Add necessary data structures
2. Implement core functionality
3. Add configuration options if needed
4. Update documentation


### Debugging Tips
- Use `printf` for debugging output
- Check widget bounds and positions
- Verify event coordinates

## Important Notes for Agents

1. **Event Coordinate System**: Events use absolute screen coordinates. When propagating to children, transform coordinates to be relative to the child widget.
2. **Widget Hierarchy**: The root widget is in `ctx->root_widget`. Child widgets are stored in parent widgets.
3. **Focus Management**: Text input widgets need special handling for keyboard events. Use `bgtk_set_focus()` to manage focus.
4. **Drawing**: Widgets are drawn to `ctx->shm_buffer`. Use the drawing functions in `drawing.c`.
5. **Memory Management**: Widgets are responsible for freeing their own resources in the destructor.
6. **Configuration**: Theme and font settings are loaded from a config file at startup.


## Useful Functions

- `bgtk_init()`: Initialize the BGTK context
- `bgtk_destroy()`: Clean up resources
- `bgtk_handle_input_event()`: Handle input events
- `bgtk_draw_widgets()`: Render all widgets
- `bgtk_set_focus()`: Set the focused widget
- `draw_widget()`: Draw a single widget
- `calculate_widget_size()`: Calculate widget dimensions and positions

This is a toolkit for the BGCE display server. It works by directly writing to
a graphical buffer, this library lets developers create user interfaces easily.

