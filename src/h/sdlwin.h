/*
 * sdlwin.h - macros and types used in the SDL3 + Blend2D graphics
 * interface (icon-sdl fork; not part of upstream Icon).
 *
 * Only plain C types appear here and in the runtime's SDL backend files:
 * rtt processes them, and never sees SDL or Blend2D headers. The code
 * that uses SDL and Blend2D lives in src/sdl, behind ../sdl/sdlgfx.h.
 */

/*
 * Drawing operations: X's GC function codes, so that the bitwise
 * semantics (fg op pixel) are those of the X11 reference.
 */
#define DRAWOP_CLEAR		0
#define DRAWOP_AND		1
#define DRAWOP_ANDREVERSE	2
#define DRAWOP_COPY		3
#define DRAWOP_ANDINVERTED	4
#define DRAWOP_NOOP		5
#define DRAWOP_XOR		6
#define DRAWOP_OR		7
#define DRAWOP_NOR		8
#define DRAWOP_EQUIV		9
#define DRAWOP_INVERT		10
#define DRAWOP_ORREVERSE	11
#define DRAWOP_COPYINVERTED	12
#define DRAWOP_ORINVERTED	13
#define DRAWOP_NAND		14
#define DRAWOP_SET		15

/*
 * Line and fill styles (X's values).
 */
#define LS_SOLID		0
#define LS_DASHED		1
#define LS_STRIPED		2
#define FS_SOLID		0
#define FS_STIPPLE		2
#define FS_OPAQUESTIPPLE	3

/*
 * Modifier key masks passed to qevent() (rwindow.r).
 */
#define ShiftMask		(1L << 0)
#define ControlMask		(1L << 2)
#define Mod1Mask		(1L << 3)

/*
 * Window states, as reported by WAttrib("canvas").
 */
#define NormalState		1
#define IconicState		3
#define RootState		(IconicState + 1)
#define MaximizedState		(IconicState + 2)
#define HiddenState		(IconicState + 3)

/*
 * Argument structures for the drawing primitives. The generic code fills
 * them in; angles are in radians (see ANGLE and EXTENT).
 */
typedef struct { int x, y; } XPoint;
typedef struct { int x1, y1, x2, y2; } XSegment;
typedef struct { int x, y, width, height; } XRectangle;
typedef struct { int x, y, width, height; double angle1, angle2; } XArc;

#define ARCWIDTH(arc) ((arc).width)
#define ARCHEIGHT(arc) ((arc).height)
#define RECX(rec) ((rec).x)
#define RECY(rec) ((rec).y)
#define RECWIDTH(rec) ((rec).width)
#define RECHEIGHT(rec) ((rec).height)

/*
 * Icon measures angles in radians, clockwise on the screen; Blend2D's
 * y-down coordinates already turn positive angles clockwise.
 */
#define ANGLE(ang) (ang)
#define EXTENT(ang) (ang)
#define FULLARC (2 * Pi)

/*
 * Font metrics and text.
 */
#define TEXTWIDTH(w,s,n) textWidth(w, s, n)
#define ASCENT(w) ((w)->context->font->ascent)
#define DESCENT(w) ((w)->context->font->descent)
#define FHEIGHT(w) ((w)->context->font->height)
#define FWIDTH(w) ((w)->context->font->fwidth)
#define LEADING(w) ((w)->context->leading)
#define MAXDESCENDER(w) DESCENT(w)
#define LINEWIDTH(w) ((w)->context->linewidth)

#define SCREENDEPTH(w) 24
#define DISPLAYHEIGHT(w) displayHeight(w)
#define DISPLAYWIDTH(w) displayWidth(w)

#define ISICONIC(w) ((w)->window->iconic == IconicState)
#define ISFULLSCREEN(w) (0)
#define ISROOTWIN(w) (0)
#define ISNORMALWINDOW(w) ((w)->window->iconic == NormalState)
#define ICONFILENAME(w) ((w)->window->iconimage)
#define ICONLABEL(w) ((w)->window->iconlabel)
#define WINDOWLABEL(w) ((w)->window->windowlabel)

/*
 * The text cursor is drawn by the backend itself; nothing to hide.
 */
#define hidecrsr(x) (void)x /* noop */
#define showcrsr(x) (void)x /* noop */
#define UpdateCursorPos(x, y) /* noop */

#define SysColor unsigned long
#define WMAXCOLORS 256
#define MAXCOLORNAME 40
#define NUMCURSORSYMS 78

/*
 * Row/column to pixel translations (as in xwin.h).
 */
#define ROWTOY(w,row) ((row-1) * LEADING(w) + ASCENT(w))
#define COLTOX(w,col) ((col-1) * FWIDTH(w))
#define YTOROW(w,y)   ((y>0) ? ((y) / LEADING(w) + 1) : ((y) / LEADING(w)))
#define XTOCOL(w,x)  (!FWIDTH(w) ? (x) : ((x) / FWIDTH(w)))

/*
 * Event queue access (as in xwin.h): "get" removes an event from the
 * Icon list and records keystrokes on the ghost queue.
 */
#define EVQUEGET(w,d) { \
   wsp ws = (w)->window; \
   if (!c_get((struct b_list *)BlkLoc(ws->listp),&d)) fatalerr(0,NULL); \
   if (Qual(d)) {\
      ws->eventQueue[ws->eQfront++] = *StrLoc(d); \
      if (ws->eQfront >= EQUEUELEN) ws->eQfront = 0; \
      ws->eQback = ws->eQfront; \
      } \
   }
#define EVQUEEMPTY(w) (BlkLoc((w)->window->listp)->list.size == 0)
