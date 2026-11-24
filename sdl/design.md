# SDL3 + Blend2D graphics backend: design notes

These notes record the facts the backend relies on, the decisions taken and
why. Icon's C sources are the authority: check claims against them, and
cite the file.

## Goal

A graphics backend for Icon 9.5's built-in graphics on macOS (first) and
Linux, without X11 or XQuartz. It should behave as closely as practical
like the X11 backend (`src/runtime/rxwin.ri`, `rxrsc.ri`), which serves as
the reference.

## Architecture

The backend is split into two layers:

- **Portable canvas layer.** Each Icon window has its own RGBA image, held
  at the display's backing scale and drawn with Blend2D. This layer holds
  the graphics context state: colours, line style and width, drawop,
  fonts, clipping, patterns. It also does the operations Icon defines in
  terms of pixels: `drawop=reverse`, `CopyArea`, `Pixel()`, images.
- **Platform layer (SDL3).** It opens, closes and moves windows, presents
  changed pixels, translates SDL events into Icon's event queue, and
  handles cursors and titles. Font discovery is also per-platform:
  CoreText on macOS, fontconfig on Linux.

Exposes and resizes need no help from the interpreter. The platform layer
re-presents the canvas, and a resized canvas keeps its pixels and fills
the new area with the background colour.

## Facts and decisions

### Co-expressions are threads, so SDL owns the main thread

Icon 9.5 implements co-expressions with POSIX threads and semaphores on
both macOS and Linux (`src/common/rswitch.c`). The threads take strict
turns, and each gets `COEXPSIZE` = 2000 words of C stack (`init.r`),
rounded up to the thread-stack minimum, about 16 KB. Icon code can call
`WOpen()` or `Event()` from any co-expression. macOS requires window and
event calls on the process's main thread, so:

- the process's main thread runs SDL: it waits for events, queues them per
  window, and redraws on expose;
- the interpreter and every co-expression run on other threads;
- drawing goes into canvases from whichever Icon thread is current. No
  locking is needed, because the threads alternate strictly;
- platform requests (open, close, present, title, position, cursor) are
  passed to the main thread synchronously.

As a result, iconx must start the interpreter on a second thread when it
is built with graphics. SDL itself is initialised lazily, on the first
`WOpen`.

The prototype showed that a co-expression on a 16 KB stack can open a
window, draw in it (including text) and take its events.

### Requests to the main thread: our own mechanism

`SDL_RunOnMainThread` (SDL 3.4.20) does not wake a blocked `SDL_WaitEvent`
on macOS: with a window open, 500 calls didn't finish within 30 s. We
instead push an SDL user event carrying the request and wait on a
condition variable. That takes about 35 µs per round trip once a window
exists, and about 0.8 ms during app startup.

### Blend2D doesn't dash strokes

Blend2D 0.21 stores a dash array, but its stroker (`core/pathstroke.cpp`)
never reads it. The canvas layer splits lines, rectangles and arcs into
dash segments itself; arcs go through polylines. `linestyle=striped` uses
the same code, drawing the gaps in the background colour.

### `drawop=reverse` is bitwise XOR, done by hand

No rasteriser offers Icon's bitwise reverse; Cairo's and Blend2D's `XOR`
are alpha compositing. Instead:

1. rasterise the shape into an A8 coverage mask over its bounding box;
2. XOR every pixel with at least 50% coverage with `fg ^ bg`.

Drawing the same shape twice restores the canvas exactly. The prototype
checks this by checksum.

### HiDPI

One Icon pixel is one point. The canvas is drawn at the backing scale (2×
on Retina) through a Blend2D scale transform. Strokes go through pixel
centres (+0.5), which keeps axis-aligned 1-pixel lines crisp; diagonals
are antialiased.

`Pixel()` reads the device pixel at the logical pixel's top-left corner.
Still to decide: whether to offer an aliased mode for programs that depend
on exact pixel values at edges.

### Fonts

Icon's portable families map to macOS fonts:

| Icon family | macOS font |
|---|---|
| `mono` | Menlo |
| `typewriter` | Courier |
| `sans` | Helvetica |
| `serif` | Times |

Other names go to CoreText as they are. CoreText returns the font's file
and PostScript name. Blend2D loads the face from that file; in a `.ttc`
collection we pick the face whose PostScript name matches. Metrics come
from Blend2D. Still to check against icont: `fwidth`, which currently uses
the font's bounding box, like X's `max_bounds`.

## Dependencies and reproducible builds

SDL3, Blend2D and AsmJit are vendored as source tarballs in `deps/`,
pinned by SHA-256 in `deps/deps.lock`. All three are under the zlib
licence.

- The build verifies the checksums and never uses the network.
- `deps/update.sh NAME VERSION` is the only tool that downloads. An
  upgrade is committed together with any code changes it needs.
- All three are linked statically, so the result depends only on system
  libraries.
- Required tools: a C/C++ compiler (Xcode command-line tools on macOS) and
  CMake.
- On Linux, SDL3 also needs the distribution's X11, Wayland and
  xkbcommon development headers, and fonts need fontconfig.

The prototype this design comes from (the `gfxproto` workspace directory)
built offline from these exact tarballs, in about 30 s.

## Plan

1. Inventory the backend interface: every function icont's X11 backend
   provides, which layer it belongs to, and its exact semantics (below,
   to come).
2. Reference outputs: run the X11 icont under Xvfb on Linux and save
   images of graphics test programs with `WriteImage`. The new backend
   renders the same programs without a window, and the pixels are
   compared. Text will differ; geometry, fills, XOR and CopyArea should
   match.
3. Add a configuration (`config/macintosh-sdl`, later `config/linux-sdl`)
   and the build integration.
4. Port the canvas and platform layers from the prototype into
   `src/runtime`.
