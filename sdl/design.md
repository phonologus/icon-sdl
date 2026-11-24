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
   `src/runtime`. Done; see "The canvas layer" and "The platform layer".

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
3. **Gamma:** like X, so that output can be compared pixel for pixel
   against the X11 reference. X's effective default turned out to be
   2.27630272, not the nominal 2.5; see "Gamma" below.
4. **`&meta` on macOS:** the Option key, as XQuartz maps it; Command
   available through a setting.
5. **Antialiasing:** superseded. Geometry is aliased like X's: every shape
   goes through a coverage mask with a 50% threshold, which the drawops
   need anyway. That makes most of the output pixel-identical to X.
   Plain text stays antialiased. See "The canvas layer" below.
   Confirmed by Sean, 2026-11-24.
6. **Window scale:** visible windows are drawn at the display's scale
   (2× on Retina) by default, for sharp output. `ICONSDL_SCALE=1` gives
   pixels identical to a hidden canvas (and to X). Agreed 2026-11-24.

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

## The canvas layer (src/sdl/canvas.c)

Each window state owns a canvas, the equivalent of X's backing pixmap:
an opaque PRGB32 Blend2D image at an integer scale (1 for hidden
canvases; the display's backing scale for windows). Logical pixel
(x, y) is the s-by-s block of device pixels at (s*x, s*y). Pixel reads
(`Pixel`, `WriteImage`, `capture`) sample the block's top-left device
pixel.

Pixels hold device colours 0xRRGGBB, exactly what a 24-bit TrueColor X
display holds:

1. An Icon colour (linear, 16 bits per channel) is gamma-corrected to a
   16-bit "X colour", truncated as X's `xcolor()` does.
2. The top 8 bits per channel are the pixel.
3. `rsdlrsc.ri` keeps X's table from pixel to the X colour that made it,
   with the first allocation winning. `Pixel()` returns exactly what the
   X backend returns: the stored colour if there is one, otherwise the
   8-bit value × 257, each passed back through gamma.

Drawing a shape:

1. Blend2D rasterises the shape into an A8 coverage mask over its
   bounding box.
2. Every device pixel with at least half coverage is combined with the
   canvas by the X GC function: 16 drawops; `reverse` is xor with
   fg ^ bg.
3. The fill pattern applies: `masked` draws only set bits, `textured`
   draws the others in bg.

One code path therefore gives aliased geometry, every drawop and stippled
fills.

Geometry follows X:

- **Coordinates:** integer coordinates are pixel centres, so strokes,
  arcs and polygons are drawn through (x + 0.5, y + 0.5). Rectangle fills
  cover [x, x+w).
- **Fills:** polygons use the even-odd rule; partial arc fills are pie
  slices.
- **Caps and joins:** lines have projecting (square) caps and mitre
  joins.
- **Dashes:** 4 on, 4 off for width ≤ 1, and 3 × width for wider lines
  (Icon sets the dash list). The pattern continues across a polyline's
  vertices.
- **`striped`:** X's LineDoubleDash. The whole line is drawn in bg with
  its ends capped, then the even dashes in fg with butt ends.
- **The tie rule:** X includes a pixel whose centre lies exactly on a
  shape's top or left edge and excludes one on its bottom or right edge.
  Such a pixel is exactly half covered, so all geometry is shifted up and
  left by 1/64 device pixel to resolve ties the same way. For straight
  edges, coverage ≥ 50% is otherwise the same test as "centre inside".

Text:

- Drawn with Blend2D from fonts found by CoreText. Icon strings are
  treated as Latin-1, like X's ISO 8859-1 fonts.
- Plain text in copy mode is antialiased. With other drawops or patterns
  it goes through the mask like geometry.
- Text written with `write()` first fills its cell with bg in copy mode
  (X's `XDrawImageString`).

Font metrics follow X:

- `ascent` and `descent` come from the font.
- `fwidth` is the widest printable Latin-1 character, like X's
  `max_bounds.width` for an ISO 8859-1 font. The whole font's maximum
  advance includes far wider symbols.
- Loading a font sets `leading` to its height, but finding a font already
  loaded does not (X's `tryfont`). The default font is preloaded, so
  `Font("fixed")` leaves `leading` alone, as in X.

## Gamma

The X backend does not use `GammaCorrection` (2.5). `alc_display` asks
Xlib for the device value of `RGBi:.5/.5/.5` and sets the gamma to
`log(.5)/log(g)`. On a display without colour calibration (the usual
case, and the reference setup), Xlib answers 49408,47872,47872, so the
gamma is 2.27630272. `rsdlrsc.ri` computes it the same way, from those
values, so the result is the identical double. With 2.5, colours drawn
with drawops other than copy came out wrong. Plain colours looked right,
because `Pixel()` round-trips through the same gamma.

## Comparing with the X11 reference

    sdl/tests/ref/run-sdl.sh ICONDIR [OUTDIR]

This runs every reference program with the Icon build in ICONDIR and
compares the results with `expected/`:

- standard output must match exactly;
- images are compared pixel by pixel by `compare.py` (a GIF decoder in
  the Python standard library, checked against macOS's own decoder),
  which also writes `NAME.diff.png` for each image;
- `sdl/tests/ref/tolerance` lists the differences accepted per test.

Results with hidden canvases (2026-11-24):

| Test | Result |
|---|---|
| `drawops`, `images`, `patterns`, `bounds` | pixel-identical; `Pixel()` output identical |
| `lines` | 7 pixels differ: corners of even-width line caps, where coverage and "centre inside" disagree |
| `shapes` | 50 pixels differ: edge pixels of filled circles and slanted 1-pixel outlines |
| `clipcopy` | 22 pixels differ: X's own algorithm for thin arcs |
| `text` | differs, as expected: other fonts. Metrics are close (fwidth within a pixel) and `leading` behaves identically |

The `Pixel()` output compares exactly only because a colour reads back
the same on both sides. The probe that found the gamma used the fact
that a pixel the colour table doesn't know reads back as its 8-bit value
× 257 through gamma, which can be inverted to see the raw pixel.

## The platform layer (src/sdl/plat.c)

With `SDLGraphics`, iconx's `main()` (`imain.r`) calls `pl_main(iconx,
...)`. That runs the interpreter on a new thread with a stack as large as
the main thread's (the stack rlimit, at least 8 MB), and serves it from
the main thread:

- **Before the first window,** the main thread waits on a condition
  variable. Programs without windows never start SDL, and `pollevent()`
  returns at once while no window is open.
- **The first request starts SDL.** From then on the main thread waits in
  `SDL_WaitEvent`, and requests (open, close, present, title, geometry,
  state, cursor, warp) arrive as SDL user events. Each is synchronous:
  the Icon thread waits for it.
- **Events are translated on the main thread and queued:**
  - characters with shift and control applied, as `XLookupString` gives
    them;
  - X keysyms for other keys (`Key_Left` = 65361 and so on);
  - X modifier masks;
  - times in milliseconds;
  - button presses and releases, motion, resizes, moves, state changes,
    exposes and close requests.

  `pollevent()` and `wgetq()` move the queued events into the windows'
  Icon event lists (`qevent`), following rxwin.ri:
  - a drag reports the most recently pressed button, as X does;
  - a resize grows the canvas and queues `&resize`;
  - closing a window from its title bar raises run-time error 141.
- **Presenting:** a window shows its canvas through a streaming texture
  updated with the canvas's dirty rectangle. This happens at most 60
  times a second from `pollevent()`, and always before `wgetq()` waits
  and on `WFlush()`/`WSync()`.
- **`&meta` is the Option key,** or Command with `ICONSDL_META=command`.

Windows are drawn at the display's scale (2 on Retina) for sharp output;
`ICONSDL_SCALE=1` forces 1×. At 1×, a visible window's pixels are
identical to a hidden canvas's, and so to X's. At 2×, edge pixels of
diagonals, arcs and text read back slightly differently through
`Pixel()`/`WriteImage()`: 277 of 60,000 pixels in
`sdl/tests/win/window.icn`.

### Window tests

    sdl/tests/win/run.sh ICONDIR

These open real windows briefly, so they need a display. `plat.c` has a
test hook: if `ICONSDL_TEST_EVENTS` is set, the first window to open is
sent a scripted sequence of key presses, mouse events and a close
request, pushed into SDL's queue so they go through the same
translation as real input.

| Test | Checks |
|---|---|
| `events` | characters, control and shift, X keysyms, buttons, drags (including X's most-recent-button rule), `&meta`, against X's answers |
| `window` | at 1×, a visible window's pixels equal a hidden canvas's; resize, hide and show, label, display size |
| `coexp` | a co-expression (a thread with a 16 KB stack) opens a window, loads a font, draws and reads events; the main program then uses it |
| `close` | the close box ends the program with error 141, as with X |

### Not yet done

- The text cursor (`WAttrib("cursor=on")`) isn't drawn.
- X colour names beyond Icon's own (`rgb.txt` names such as
  `LightSlateGray`): `nativecolor()` fails.
- `Lower()`: SDL cannot lower a window. `Alert()`: SDL has no beep.
- Icon images and icon labels (`iconimage=`, `iconlabel=`) are ignored.
- Keypad keys report their characters; keypad-specific keysyms are not
  distinguished.
- A hidden canvas shown later keeps scale 1, so it is pixel-doubled on
  Retina.
- Linux: fontconfig font lookup and a `linux-sdl` configuration.

## Keysyms in the IPL

Programs get key names such as `Key_Left` from the IPL's
`gincl/keysyms.icn`, which defined X's values only under
`$ifdef _X_WINDOW_SYSTEM`. With SDL graphics they were undefined: an error
with `icont -u` (as `vib` builds), and otherwise a silent `&null` global.
Our backend reports X's keysyms, so `keysyms.icn` now defines a helper
symbol `_X_KEYSYMS` under either `_X_WINDOW_SYSTEM` or `_SDL_GRAPHICS`
and guards the X table with it. `gprocs/vtext.icn` and
`gpacks/tiger/tgrmap.icn` alias the keypad arrows to the plain arrows
when there are no X keysyms, and now test `_X_KEYSYMS` too: Icon's
preprocessor rejects a redefinition with a different value. Under X
nothing changes (checked in the VM: `make Ilib`, `vib`, `tgrmap`, the test
suite, and the reference output). The `events` window test names keys
through `keysyms.icn`, so this is covered.

Only these four IPL files test `_X_WINDOW_SYSTEM`. The fourth,
`gprocs/viface.icn`, chooses X font names, which correctly doesn't apply
to SDL.

## Fixes from first use (2026-11-24)

- **`EraseArea` touched one pixel too many on every side.** It overwrote
  the border of a VIB region whose usable area was being erased. The
  helper that computes an operation's area added a stroke margin even for
  exact areas. `region()` now takes the margin explicitly: zero for
  erase, copy, image writes and points. The `bounds` reference test pins
  these edges with zero tolerance. (`clipcopy`'s tolerance had absorbed
  the error.)
- **`CopyArea` now follows rxwin.ri exactly.** It always copies (X sets
  the GC function to `GXcopy` for it, so `drawop` does not apply). Any
  part of the source beyond its canvas is filled with the destination's
  background, after the copy.

## Performance

Strokes used to clear and combine the mask over their whole bounding
box, so a long diagonal line cost in proportion to its box's area: about
27 µs per random long segment on a hidden canvas, and 92 µs in a 2×
window. Strokes now work row by row: `spans()` computes, for each device
row, the x range within reach of the stroke (half its width, plus room
for square caps and antialiasing), and only those spans are cleared and
combined. The pixels are identical. Measured on a 1363×759 window:

| | before | after |
|---|---|---|
| 10,000 long random diagonals, hidden | 270 ms | 40 ms |
| 10,000 long random diagonals, 2× window | 920 ms | 130 ms |
| 2,500 dashed segments, 2× window | 260 ms | 60 ms |
| 25,000 short segments, 2× window | — | 40 ms |

Text and scrolling, measured by profiling (`sample`, with symbols from
the unstripped `src/runtime/iconx`):

- **Scrolling.** `write()` past the bottom of a window scrolls with
  `CopyArea`, which went through a temporary copy and the per-pixel GC
  function: about 4 million pixels per line at 2×. `CopyArea` always
  copies, so at equal scales it now moves whole rows with `memmove`,
  bottom-up when the source is above the destination. 40 lines of
  `write()` in `sans,14`: 100 → 20 ms.
- **Text.** Strings are shaped once per call, and single characters
  (`write()` draws one at a time) are shaped once per font and cached.
  The antialiasing blend divides by 255 with an exact shift formula. Text
  is processed over its ink width only. These are smaller gains than
  hoped: the profile showed scrolling, not shaping, to be the cost.

**Showing windows.** A window is shown at once whenever the program
pauses naturally: waiting in `Event()`, `Pending()` (which calls
`wsync`), `WFlush()`, `WSync()` or `WDelay()`. `pollevent()` also shows
it, but only after `ICONSDL_FRAME_MS` (default 100) of drawing without
such a pause. Showing more often, as at first (every 16 ms), catches
repaints half done, a pane erased with its text not yet redrawn, which
flickers. A program that never pauses still updates ten times a second.

Drawing all the segments of one `DrawSegment` call as a single path (safe
for drawops where overlaps don't matter) was tried and rejected. The
spans of scattered segments merge into nearly full-width rows, which made
it three times slower than drawing them one by one. The interpreter's
speed with a window open equals upstream's: `pollevent()` costs nothing
measurable.
