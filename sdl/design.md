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
   provides, which layer it belongs to, and its exact semantics. Done; see
   "Backend inventory" below.
2. Reference outputs. Done; see "Test VM and reference outputs" below.
3. Add a configuration (`config/macintosh-sdl`, later `config/linux-sdl`)
   and the build integration. Done for macOS; see "The macintosh-sdl
   configuration" below.
4. Port the canvas and platform layers from the prototype into
   `src/runtime`.

## Backend inventory

### How a backend plugs in

Icon 9.5 already has two backends: X11 (`XWindows`) and MS Windows
(`WinGraphics`). A backend consists of:

- **its implementation files**, included by `src/runtime/rwinsys.r`
  (`rxwin.ri`) and `src/runtime/rwinrsc.r` (`rxrsc.ri`);
- **a header** (`h/xwin.h`, `h/mswin.h`). It defines:
  - the drawing primitives the generic code calls as macros: `drawarcs`,
    `drawlines`, `drawpoints`, `drawrectangles`, `drawsegments`,
    `drawstrng`, `fillarcs`, `fillpolygon`;
  - the font metric macros (`ASCENT`, `DESCENT`, `FHEIGHT`, `FWIDTH`,
    `LEADING`, `TEXTWIDTH`, `MAXDESCENDER`);
  - the `DRAWOP_*` codes and the modifier masks (`ShiftMask`,
    `ControlMask`, `Mod1Mask`);
- **its own `#ifdef` sections** in `h/graphics.h`, which hold the window,
  context, font, colour and display structures, and in `h/rproto.h`.

An SDL backend (`SDLGraphics`) adds `h/sdlwin.h`, `rsdlwin.ri` and
`rsdlrsc.ri`, plus its own sections in those headers. Other upstream files
that mention `XWindows` and need a matching branch:

| Files | `XWindows` mentions each |
|---|---|
| `h/config.h` | 6 |
| `runtime/rsys.r` | 4 |
| `h/grttin.h`, `h/features.h`, `h/rexterns.h`, `h/sys.h`, `common/xwindow.c` | 2 |

`config/setup.sh` chooses between `Graphics` and `NoGraphics`; it needs an
SDL option.

### Interface facts

These are in the generic code and constrain the backend:

- **Event queue.** `qevent()` (`rwindow.r`) builds Icon list elements, so
  it must run on an Icon thread. The SDL main thread only queues raw
  events; `pollevent()` and `wgetq()` drain them into `qevent`.
- **Event values.** `qevent` takes a millisecond timestamp, used for
  `&interval`, and X-style modifier masks:
  - `ControlMask` becomes `&control`;
  - `Mod1Mask` becomes `&meta`;
  - `ShiftMask` becomes `&shift`.
- **Polling.** `interp.r` calls `pollevent()` every N instructions, where N
  is its return value; X returns 400. Ours should also present the dirty
  canvases there.
- **Special keys** are reported as X keysym integers. `Key_Left` is 65361,
  and the IPL's `keysyms.icn` depends on those values. We need a table
  mapping SDL key codes to X keysyms.
- **Drag events.** X reports drags as the most recently pressed button
  still held. A `|`-for-`&` slip in `handle_mouse` makes the X code report
  `buttonorder[0]` always. Reproduce that behaviour, not the code.
- **Lines** use projecting caps and mitre joins (`setlinestyle`). In
  Blend2D: `BL_STROKE_CAP_SQUARE`, `BL_STROKE_JOIN_MITER_CLIP`.
- **`drawop`** accepts all 16 X GC functions by name (`and`,
  `andInverted`, `andReverse`, `clear`, `copy`, `copyInverted`, `equiv`,
  `invert`, `nand`, `noop`, `nor`, `or`, `orInverted`, `orReverse`, `set`,
  `xor`), plus `reverse`, which is `xor` with `fg ^ bg`. The coverage-mask
  technique handles every one of them: for each covered pixel, apply the
  function to (fg, pixel).
- **`linestyle`**: `solid`, `dashed` (alias `onoff`), `striped` (alias
  `doubledash`).
- **`fillstyle`**: `solid`; `masked` (aliases `stippled`, `patterned`);
  `textured` (aliases `opaquestippled`, `opaquepatterned`). Patterns come
  from `Pattern()`: `SetPattern` takes a name, `SetPatternBits` a bit
  array.
- **Gamma.** Icon colours are linear intensities, gamma-corrected for the
  device. `GammaCorrection` defaults to 2.5 (`graphics.h`); MS Windows
  uses 1.0. `capture` and `getpixel` apply the inverse.
- **Images.** GIF reading and writing is already portable
  (`readGIF`/`writeGIF`). The X backend adds XBM and XPM. Blend2D reads and
  writes PNG, JPEG, BMP and QOI.

### Functions by layer

The counts are the line counts of the X implementation.

**Platform layer (SDL, main-thread requests or the event queue):**

| X function(s) | Notes |
|---|---|
| `wopen` (101), `wmap` (230), `makeIcon` | Create the SDL window; attribute processing stays. `wmap` is mostly X window-manager handshaking and shrinks a lot. |
| `wclose`, `free_window` (part) | Destroy the window; free the canvas. |
| `pollevent`, `wgetq`, `handle_misc` (133), `handle_mouse`, `handle_keypress`, `translate_key_event`, `handle_config` | Drain the main thread's queue into `qevent`; present dirty canvases; key and mouse translation as above. |
| `handle_exposures`, `wsync`, `wflush`, `wflushall` | Exposes are handled on the main thread; flushing presents. |
| `do_config`, `moveResizeWindow`, `moveWindow`, `setgeometry`, `setwidth`, `setheight`, `allowresize`, `getpos` | Window geometry. A resize also resizes the canvas. Position is a no-op under Wayland. |
| `setcanvas`, `getcanvas`, `seticonicstate`, `geticonic` | `normal`, `hidden`, `iconic` (minimised), `maximal` (maximised). `root` isn't supported. |
| `setwindowlabel`, `seticonlabel`, `seticonimage`, `seticonpos`, `geticonpos` | Window title, and the window icon via `SDL_SetWindowIcon`. Icon labels and positions are no-ops on modern desktops. |
| `setpointer`, `getpointername` | X's 78 cursor-font names, mapped to the nearest of SDL's system cursors. |
| `warpPointer`, `query_pointer`, `query_rootpointer` | Warping is a no-op under Wayland. |
| `raiseWindow`, `lowerWindow`, `walert` | Lowering has no SDL call. `walert` needs a beep; SDL has none, so use a platform call. |
| `setdisplay`, `getdisplay`, `getvisual`, `getdefault` | Multiple displays and X resources are dropped. `getdefault` fails. `getvisual` reports TrueColor-like values. |

**Canvas layer (portable pixels, Blend2D):**

| X function(s) | Notes |
|---|---|
| `drawarcs`, `drawlines`, `drawpoints`, `drawrectangles`, `drawsegments`, `drawstrng`, `fillarcs`, `fillpolygon` (macros) | Become functions. They take X's argument structs (`XPoint`, `XArc`, `XSegment`, `XRectangle`). `sdlwin.h` must define them; `mswin.h` maps them to Windows types, so it can't be reused. Arc angles are whatever the header's `ANGLE()`/`EXTENT()` macros produce. X converts radians to negated 64ths of a degree; MS Windows passes radians unchanged. We can pass radians straight to Blend2D, whose y-down coordinates make positive angles clockwise, the same as X's negation. |
| `fillrectangles`, `eraseArea` | |
| `copyArea` (136) | Within one canvas, and between windows (another canvas). The X code's two-display case goes away. |
| `capture` (80), `getpixel_init`, `getpixel_term`, `getpixel` (40), `getximage` | Read pixels, with inverse gamma. |
| `strimage` (85), `blimage` (68) | Write pixels from Icon image strings. |
| `readimage`, `loadimage` (145), `dumpimage` (103) | GIF stays portable. Add PNG, JPEG, BMP via Blend2D. XBM/XPM: decide (below). |
| `setclip`, `unsetclip` | |
| `SetPattern`, `SetPatternBits` | Stipple bitmaps, applied as a mask. |
| `postcursor`, `scrubcursor`, `setcursor` | Icon's own text cursor, drawn into the canvas. |
| `xdis`, `wputc` (122) | Text output and scrolling, built on `drawstrng` and `copyArea`. |
| `resizePixmap` | Becomes `canvas_resize`. |
| `TEXTWIDTH`, `ASCENT`, `DESCENT`, `FHEIGHT`, `FWIDTH`, `MAXDESCENDER` (macros) | From Blend2D font metrics. `FWIDTH` is X's `max_bounds.width`. |

**Context and resources (portable bookkeeping):**

| X function(s) | Notes |
|---|---|
| `setfg`, `setbg`, `isetfg`, `isetbg`, `setfgrgb`, `setbgrgb`, `resetfg`, `toggle_fgbg`, `getfg`, `getbg`, `setgamma`, `freecolor` | Colours become plain RGB values plus the name `WAttrib` reports. |
| `nativecolor` | X colour names (`rgb.txt`, such as `LightSlateGray`) need a built-in table; Icon's own names are parsed portably by `parsecolor`. |
| `alc_rgb` (126), `alc_centry`, `alc_color`, `copy_colors`, `free_xcolor`, `free_xcolors`, `go_virtual`, `remap`, `xcolor`, `lcolor` | X colormap management. Almost all of it disappears with TrueColor. |
| `mutable_color` (77), `set_mutable`, `get_mutable_name`, `free_mutable` | Need colormap cells, which modern TrueColor X servers don't offer either. Decide (below). |
| `setdrawop`, `getdrawop`, `setlinestyle`, `getlinestyle`, `setlinewidth`, `setfillstyle`, `setleading`, `setimage`, `rebind` | Context fields. |
| `alc_context`, `clone_context`, `free_context`, `alc_winstate` | Structure allocation, minus the X fields. |
| `setfont`, `getfntnam`, `alc_font` (58), `findfont` (93), `tryfont`, `okfont`, `fontcmp`, `fieldcmp`, `xlfd_field`, `xlfd_size` | X's XLFD font matching is replaced by `parsefont` (portable) → CoreText or fontconfig → Blend2D face. |
| `alc_display` (127), `free_display` | One global SDL "display". |

### Decisions (agreed 2026-11-24)

1. **Mutable colours:** `NewColor` fails, for now. The X code already
   fails when it can't get a colour cell, even after falling back to a
   virtual colormap (`mutable_color`), which TrueColor X servers can't
   provide. So failing is existing behaviour. Emulating them, by tracking
   the pixels that use each entry, stays possible later.
2. **XBM/XPM images:** dropped at first. GIF stays (portable code), and
   PNG, JPEG and BMP come from Blend2D. A small XBM reader can be added
   later if IPL programs need it.
3. **Gamma:** 2.5, like X, so that output can be compared pixel for pixel
   against the X11 reference.
4. **`&meta` on macOS:** the Option key, as XQuartz maps it; Command
   available through a setting.
5. **Antialiasing:** accepted. Axis-aligned shapes match X exactly at
   integer scale. Diagonals and arcs are compared against the reference
   with a tolerance.

## Test VM and reference outputs

`sdl/vm/vm.sh` manages a headless Debian 13 arm64 VM under QEMU with
macOS's Hypervisor.framework (`create`, `start`, `stop`, `ssh`, `status`,
`destroy`). Its inputs are pinned:

- the Debian cloud image, build `20261116-2664`, by SHA-512;
- its packages, through snapshot.debian.org at `20261116T000000Z`.
  `sdl/vm/provision.sh` points apt there and disables automatic upgrades;
  the exact versions installed are recorded in `sdl/vm/packages.lock`.

Only `create` uses the network. The VM's state lives in `sdl/vm/state/`,
which git ignores.

`sdl/tests/ref/make-reference.sh` builds unmodified upstream Icon
(`master`) with X11 graphics in the VM. It then runs each
`sdl/tests/ref/*.icn` under Xvfb (24-bit TrueColor) and stores the image
(`NAME.gif`, from `WriteImage`) and standard output (`NAME.out`) in
`sdl/tests/ref/expected/`. Two runs produce byte-identical files.

The test programs use the built-in `open(..., "g", ...)` and assign
`&window`, so they need no IPL. (`WOpen` is an IPL procedure in
`gprocs/graphics`, not a built-in.)

| Test | Covers |
|---|---|
| `lines` | widths, `solid`/`dashed`/`striped` (striped gaps are drawn in bg), points, segments, polylines, `DrawCurve` |
| `shapes` | rectangles (including degenerate ones), circles, arcs, polygons; a self-intersecting star shows X's even-odd fill rule |
| `drawops` | all 16 X drawops plus `reverse`, over four background colours |
| `patterns` | named and bit-string patterns, `fillstyle=masked` and `textured` |
| `clipcopy` | `Clip`, overlapping `CopyArea`, `CopyArea` between windows, `EraseArea` |
| `images` | `DrawImage` with `c1`, `g16`, bilevel and transparent bilevel strings; `Pixel` readback |
| `text` | the portable font families, `fixed`, and their metrics |

Facts the reference shows:

- `Pixel()` returns `0,32767,65535` for a colour set as `0,32768,65535`.
  The value is quantised to 8 bits per channel and passed through gamma
  2.5; we should reproduce that.
- X's metrics for `mono,12`: ascent 11, descent 2, fheight 13, fwidth 7,
  leading 13. For `sans,12`: fwidth is 12, the font's maximum glyph width,
  far more than its average.

## The macintosh-sdl configuration

    make SDL-Configure name=macintosh-sdl && make Icont

`SDL-Configure` is `X-Configure` plus a check that the configuration
defines `SDLGraphics`. Both run `setup.sh NAME Graphics`, so `define.h`
gets `Graphics`, and `config/macintosh-sdl/define.h` adds `SDLGraphics`.
`config.h` then defines neither `XWindows` nor `WinGraphics`.

The build has two layers:

- **`src/sdl`** (plain C, built with CMake) holds everything that includes
  SDL or Blend2D headers. It builds `libiconsdl.a`, plus SDL3, Blend2D and
  AsmJit from `deps/`. Its `build.sh` verifies the checksums, unpacks the
  tarballs into `src/sdl/build`, and runs one CMake build without network
  access. In other configurations, `src/sdl`'s Makefile does nothing.
- **The runtime backend** (`rsdlwin.ri`, `rsdlrsc.ri`, `h/sdlwin.h`) goes
  through rtt. rtt never reads system headers, so these files use only
  plain C types and call into `src/sdl` through `src/sdl/sdlgfx.h`.
  Window and canvas handles are `void *`.

`config/macintosh-sdl/Makedefs` links iconx with `libiconsdl.a`, the
static SDL3 and Blend2D libraries, the macOS frameworks SDL uses, and
`-lc++ -lobjc`.

Notes:

- **The top-level `.gitignore` ignores every file named `Makedefs`.** It is
  meant for the generated top-level one. `config/macintosh-sdl/Makedefs`
  had to be added with `git add -f`, and copies made with
  `git ls-files --exclude-standard` miss it.
- **The linker warns** "reducing alignment of section `__DATA,__common`
  from 0x8000 to 0x4000". This is upstream code, not ours.
  `fwindow.r` has `char ConsoleStringBuf[MaxReadStr * 48]`, a 24 KB
  tentative definition. Apple's toolchain aligns common symbols by size
  and then caps them at the segment limit. A `char` buffer needs no
  alignment, and any graphics build on macOS would warn the same way.
- **`pollevent()` runs every 400 instructions in any graphics build.**
  With no window open, the SDL version returns at once and never starts
  SDL, so non-graphics programs are unaffected.

Verified (2026-11-24):

| Build | Result |
|---|---|
| `macintosh-sdl` | full test suite passes (`make Ilib && make Test`); `&features` includes "graphics" and "SDL graphics"; `open(..., "g")` fails cleanly |
| `macintosh` without graphics, from this branch | full test suite passes; `src/sdl` does nothing |
| Linux X11, from this branch, in the VM | full test suite passes; `make-reference.sh --check` gives byte-identical output |
| `macintosh-sdl` from a fresh copy, network denied by `sandbox-exec` | builds |
