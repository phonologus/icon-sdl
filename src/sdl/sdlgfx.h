/*
 * sdlgfx.h -- interface between Icon's runtime (rsdlwin.ri, processed by
 * rtt) and the SDL3 + Blend2D code in src/sdl (plain C).
 * (icon-sdl fork; MIT licence, see LICENSE.md.)
 *
 * Only plain C types may appear here: rtt never sees SDL or Blend2D
 * headers. Canvases, fonts and windows are opaque pointers.
 *
 * Coordinates are Icon's: integer logical pixels, origin top left. A
 * canvas stores its pixels at an integer scale (1 for hidden canvases,
 * the display's backing scale for windows); all reads and writes of
 * pixel values are in logical pixels. Colours are device values
 * 0xRRGGBB, as on a 24-bit TrueColor X display.
 */

/* Check that SDL3 and Blend2D are linked and working; returns the SDL
 * version as major*10000 + minor*100 + patch, or 0 on failure. */
int sdlgfx_probe(void);

/*
 * The graphics state of one drawing call, filled in from Icon's context.
 */
typedef struct {
   unsigned int fg, bg;		/* device colours, 0xRRGGBB */
   int drawop;			/* X GC function, 0..15 (DRAWOP_*) */
   int reverse;			/* drawop=reverse: xor with fg ^ bg */
   int linewidth;
   int linestyle;		/* 0 solid, 1 dashed, 2 striped */
   int fillstyle;		/* 0 solid, 2 masked, 3 textured */
   int patw, path;		/* pattern size; 0 if none */
   const unsigned char *pat;	/* patw * path cells, nonzero = set */
   int clipx, clipy, clipw, cliph; /* clipw < 0: no clipping */
   } sdlgc;

/* canvases */
void *cv_new(int w, int h, int scale, unsigned int bg);
void cv_free(void *cv);
void cv_resize(void *cv, int w, int h, unsigned int bg);
int cv_scale(void *cv);

/* drawing; coordinate arrays are flat lists of ints */
void cv_points(void *cv, const sdlgc *gc, const int *xy, int n);
void cv_lines(void *cv, const sdlgc *gc, const int *xy, int n);
void cv_segments(void *cv, const sdlgc *gc, const int *xyxy, int n);
void cv_rects(void *cv, const sdlgc *gc, const int *xywh, int n);
void cv_fillrects(void *cv, const sdlgc *gc, const int *xywh, int n);
void cv_arcs(void *cv, const sdlgc *gc, const int *xywh,
   const double *angles, int n, int fill);
void cv_fillpoly(void *cv, const sdlgc *gc, const int *xy, int n);
void cv_erase(void *cv, const sdlgc *gc, int x, int y, int w, int h);
void cv_copy(void *src, void *dst, const sdlgc *gc,
   int x, int y, int w, int h, int x2, int y2);
/* returns the text's width */
int cv_text(void *cv, const sdlgc *gc, void *font, int x, int y,
   const char *s, int n, int image);

/* pixel access, in logical pixels; out-of-range pixels read as bg */
void cv_read(void *cv, int x, int y, int w, int h, unsigned int bg,
   unsigned int *out);
void cv_write(void *cv, const sdlgc *gc, int x, int y, int w, int h,
   const unsigned int *px, const unsigned char *opaque);

/* image files: returns 1 written, 0 failed, -1 format not handled */
int cv_save(void *cv, const char *path, int x, int y, int w, int h);
/* returns 0xAARRGGBB pixels (malloc'd) or NULL */
unsigned int *cv_load(const char *path, int *w, int *h);

/* mark a logical area as changed (to be shown again) */
void cv_dirty(void *cv, int x, int y, int w, int h);

/* X's colour names (xcolors.c): 8-bit device values; 1 if found */
int xcolor_lookup(const char *name, int *r, int *g, int *b);

/* for the platform layer: the device pixels and what changed */
void cv_pixels(void *cv, unsigned char **p, long *stride, int *pw, int *ph);
int cv_take_dirty(void *cv, int r[4]);

/* fonts: family is a CoreText/fontconfig name; size in pixels */
void *ft_open(const char *family, int bold, int italic, int size);
void ft_metrics(void *f, int *ascent, int *descent, int *maxwidth);
int ft_width(void *f, const char *s, int n);

/*
 * The platform layer (plat.c): windows and events. All SDL calls run on
 * the process's main thread; these functions may be called from any
 * Icon thread.
 */

/* Run the interpreter, f(argc, argv), on a new thread; serve windows
 * from this (the main) thread. Does not return. */
int pl_main(int (*f)(int, char **), int argc, char **argv);

/* Before the interpreter exits: close the windows and shut SDL down on
 * the main thread (called from c_exit; also registered with atexit). */
void pl_shutdown(void);

/* window states */
#define PL_NORMAL	0
#define PL_ICONIC	1
#define PL_MAXIMAL	2
#define PL_HIDDEN	3

/* x, y may be PL_NOPOS (INT_MIN) to let the system place the window;
 * *scale receives the canvas scale to use (the display's density) */
#define PL_NOPOS	(-2147483647 - 1)
void *pl_open(const char *title, int x, int y, int w, int h, int state,
   int resizable, int *scale);
void pl_close(void *win);
void pl_show(void *win);
void pl_present(void *win, void *canvas, int lw, int lh, int full);
void pl_title(void *win, const char *s);
void pl_geometry(void *win, int x, int y, int w, int h);
int pl_position(void *win, int *x, int *y);
void pl_state(void *win, int state);
void pl_resizable(void *win, int on);
void pl_raise(void *win);
void pl_cursor(void *win, int cursor);	/* PLC_* */

/* pointer shapes: SDL's SDL_SystemCursor values */
#define PLC_DEFAULT	0
#define PLC_TEXT	1
#define PLC_WAIT	2
#define PLC_CROSSHAIR	3
#define PLC_NS		8
#define PLC_EW		7
#define PLC_MOVE	9
#define PLC_NO		10
#define PLC_POINTER	11
#define PLC_NW		12
#define PLC_N		13
#define PLC_NE		14
#define PLC_E		15
#define PLC_SE		16
#define PLC_S		17
#define PLC_SW		18
#define PLC_W		19
void pl_warp(void *win, int x, int y);
void pl_beep(int volume);
void pl_lower(void *win);
/* the window's icon, from w x h pixels 0xAARRGGBB */
void pl_icon(void *win, const unsigned int *px, int w, int h);
/* show a filled rectangle (logical coordinates, colour 0xRRGGBB) over the
 * window's current contents, without changing its canvas: the text cursor */
void pl_overlay(void *win, int x, int y, int w, int h, unsigned int rgb, int scale);
void pl_display_size(void *win, int *w, int *h);
int pl_global_mouse(int *x, int *y);

/* events, translated to X conventions: characters, X keysyms, X
 * modifier masks, times in milliseconds */
#define PL_KEYCHAR	1	/* code: the character */
#define PL_KEYSYM	2	/* code: an X keysym */
#define PL_PRESS	3	/* code: button 1, 2, 3 */
#define PL_RELEASE	4
#define PL_MOTION	5	/* code: SDL button mask */
#define PL_RESIZE	6	/* x, y: new size */
#define PL_MOVE		7	/* x, y: new position */
#define PL_EXPOSE	8
#define PL_STATE	9	/* code: PL_NORMAL, PL_ICONIC, PL_MAXIMAL */
#define PL_CLOSE	10

typedef struct {
   void *win;
   int kind;
   long code;
   int x, y;
   long mods;
   unsigned long ms;
   } pl_event;

int pl_poll(pl_event *e);		/* next event, or 0 if none */
int pl_wait(int timeout_ms);		/* wait for one (-1: forever) */
