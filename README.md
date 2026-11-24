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

**Status: early work in progress.** Nothing here builds a graphics-enabled
`icont` yet.

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
- `deps/`: vendored, checksum-pinned sources for SDL3, Blend2D and AsmJit,
  with `deps/update.sh` to upgrade them.
- `sdl/`: the new backend's design notes (`sdl/design.md`); a test VM
  (`sdl/vm/`, Debian under QEMU, fully pinned); and reference graphics
  tests with their expected X11 outputs (`sdl/tests/ref/`).

Modified upstream files: none yet.

## Licence

Icon's own files remain in the public domain, as stated in `README`.
Files added by this fork are under the MIT licence; see `LICENSE.md`.
The vendored libraries in `deps/` keep their own (zlib) licences.
