# BGTK (Brian's Graphical Toolkit)

This is a toolkit for the BGCE display server. It works by directly writing to
a graphical buffer, this library lets developers create user interfaces easily.


## Style guide

Prefer passing struct pointers to functions instead of malloc'ing them in the function:

struct ctx = ...
int res = bgtk_init(&ctx);


## Roadmap

[ ] **Step 1: Define BGTK Interface (`bgtk.h`)**
    * Define the core structure for a widget (`struct BGTK_Widget`).
    * Define the event structure (`struct BGTK_Event`).
    [ ] Improve widget structure: add options (like alignment), child widgets.
    [ ] Add methods to update a widget's property so it redraws automaticaly.
    * Define API functions:
        * `BGTK_Context *bgtk_init(void)`: Connects to BGCE server, gets buffer/dimensions.
        * `void bgtk_main_loop(BGTK_Context *ctx)`: Blocking loop to handle events and redraws.
        * `BGTK_Widget *bgtk_label_new(const char *text)`: Creates a label.
        * `BGTK_Widget *bgtk_button_new(const char *text, void (*callback)(void))`
        * `void bgtk_add_widget(BGTK_Context *ctx, BGTK_Widget *widget, int x, int y, int w, int h)`: Simple absolute positioning for now.

[ ] **Step 2: Implement BGTK Core (`bgtk.c`)**
    [X] Implement initialization, event queueing, and simple drawing (e.g., drawing rectangles for buttons, text rendering).
    [ ] Only call draw when changes are made, like input.
    [ ] Implement proper hit detection: using widget trees and coordinates, e.g. click on x,y -> search the tree until last widget is found, then send the input to that widget.
    [ ] Add a generic scroll widget, that scrolls content

[x] **Step 3: Integrate and Test**
    [X] Create a new client file (or update `client.c`) to demonstrate a basic BGTK application. -> app.c


## Useful Hints for Agents

- The project uses **absolute positioning** for widgets. Future work includes adding
  layout managers (e.g., box, grid).
- **FreeType** is used for font rendering. Ensure the font path in `bgtk.c` is valid
  or make it configurable.
- **BGCE** is the display server. The connection is established in `bgtk_init()`.
- **Event handling** is done in `bgtk_main_loop()`. It currently processes input events
  and redraws the screen.
- **Widget updates**: When a widget's property changes (e.g., label text), it should
  trigger a redraw. This is partially implemented.
- **Code style**: Use the provided `.clang-format` file. Key rules:
  - Braces on the same line for `if`, `for`, `while`, etc.
  - Never omit braces for single-line blocks.
  - If function arguments don't fit on one line, place each on a new line and close
    the parenthesis on its own line.
- **Testing**: The `app.c` file is a demo application. Use it to test new features.
