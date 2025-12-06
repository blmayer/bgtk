# BGTK (Brian's Graphical Toolkit)

A lightweight toolkit for the BGCE display server. This library allows developers to
create graphical user interfaces by directly writing to a shared graphical buffer.

## Features
- Simple widget system (labels, buttons).
- Direct rendering to a shared memory buffer.
- Event handling for user input.
- Basic font rendering using FreeType.

## Building

Requirements:
- A C compiler (GCC or Clang).
- FreeType development libraries.
- BGCE server running.

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

