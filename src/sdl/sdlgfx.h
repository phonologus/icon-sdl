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
void cv_text(void *cv, const sdlgc *gc, void *font, int x, int y,
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

/* for the platform layer: the device pixels and what changed */
void cv_pixels(void *cv, unsigned char **p, long *stride, int *pw, int *ph);
int cv_take_dirty(void *cv, int r[4]);

/* fonts: family is a CoreText/fontconfig name; size in pixels */
void *ft_open(const char *family, int bold, int italic, int size);
void ft_metrics(void *f, int *ascent, int *descent, int *maxwidth);
int ft_width(void *f, const char *s, int n);
