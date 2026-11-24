# icon-sdl

An **unofficial** fork of the [Icon programming language](https://github.com/gtownsend/icon)
(Arizona Icon 9.5). The only change is a new graphics backend: SDL3 for
windows and events, Blend2D for drawing. It targets macOS first, then
Linux, without X11 or XQuartz.

The language, its name and the reference implementation belong to the
Icon project at the University of Arizona:

- upstream source: <https://github.com/gtownsend/icon>
- Icon website: <https://www.cs.arizona.edu/icon>

Please don't report problems with this fork upstream.

**Status: work in progress, usable.** The `macintosh-sdl` configuration
builds and passes Icon's test suite. Windows, events and hidden canvases
work. Drawing, images and `Pixel()` match the X11 backend, most of it
pixel for pixel; events (keys, X keysyms, mouse, drags, resizes) are
reported as X reports them. `sdl/design.md` lists what is still missing.

## Building (macOS)

    make SDL-Configure name=macintosh-sdl
    make Icont

You need the Xcode Command Line Tools and CMake. SDL3, Blend2D and AsmJit
are built from the checksum-pinned sources in `deps/`; no network access,
Homebrew or XQuartz is used. The other configurations (`make Configure`,
`make X-Configure`) work exactly as upstream.

## Branches

- `master` mirrors `gtownsend/icon` unchanged.
- `sdl-graphics` holds all of this fork's work.

## Modifications to the original source

Icon's licence asks that modified versions "clearly identify your
modifications as well as the original source". The original source is
upstream `master` at the commit this branch forks from. Every change is
listed here and is visible with `git diff master...sdl-graphics`.

New files:

- `README.md`, `LICENSE.md`: this notice and the licence of the new code.
- `config/macintosh-sdl/`: the macOS configuration with the SDL backend.
- `src/h/sdlwin.h`, `src/runtime/rsdlwin.ri`, `src/runtime/rsdlrsc.ri`:
  the SDL backend (`SDLGraphics`), alongside the X11 and MS Windows ones.
- `src/sdl/`: the backend's C code that uses SDL3 and Blend2D, and the
  build of the vendored libraries.
- `deps/`: vendored, checksum-pinned sources for SDL3, Blend2D and AsmJit,
  with `deps/update.sh` to upgrade them.
- `sdl/`: the new backend's design notes (`sdl/design.md`); a test VM
  (`sdl/vm/`, Debian under QEMU, fully pinned); and reference graphics
  tests with their expected X11 outputs (`sdl/tests/ref/`).

Modified upstream files. Each change only adds an SDL graphics case,
except the test:

- `Makefile`: the `SDL-Configure` target, its usage line, and building
  `src/sdl` (which does nothing in other configurations).
- `src/Makefile`: `Clean`/`Pure` also clean `src/sdl`.
- `src/h/config.h`: `SDLGraphics` selects neither X11 nor MS Windows.
- `src/h/features.h`: the `&features` entry "SDL graphics".
- `src/h/graphics.h`: include `sdlwin.h`; SDL fields in the font,
  image, context and window structures.
- `src/h/rproto.h`: prototypes of the SDL backend's own routines.
- `src/runtime/rwinsys.r`, `src/runtime/rwinrsc.r`: include the SDL
  backend files.
- `src/runtime/imain.r`: with `SDLGraphics`, `main()` runs the
  interpreter on a new thread and keeps the main thread for windows, as
  macOS requires.
- `src/runtime/Makefile`: dependencies on the SDL backend files, and
  `$(XDEPS)` (empty unless the configuration sets it) so that iconx is
  relinked when `libiconsdl.a` changes.
- `tests/general/kwds.icn`: also ignore the optional feature
  "SDL graphics", as it already ignores "graphics" and "X Windows".
- `ipl/gincl/keysyms.icn`: the X keysym names (`Key_Left` and so on) are
  defined when `_X_KEYSYMS` is, which is defined under X (as before) and
  with SDL graphics, which reports keys as X keysyms.
  `ipl/gprocs/vtext.icn` and `ipl/gpacks/tiger/tgrmap.icn` test
  `_X_KEYSYMS` instead of `_X_WINDOW_SYSTEM` for the same reason.

## Licence

Icon's own files remain in the public domain, as stated in `README`.
Files added by this fork are under the MIT licence; see `LICENSE.md`.
The vendored libraries in `deps/` keep their own (zlib) licences.
