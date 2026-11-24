/*
 * canvas.c -- the portable pixel layer of Icon's SDL graphics backend:
 * one canvas per Icon window, drawn with Blend2D.
 * (icon-sdl fork; MIT licence, see LICENSE.md.)
 *
 * Pixels are 0xFFRRGGBB (opaque PRGB32), at an integer scale s: logical
 * pixel (x, y) is the s-by-s block of device pixels at (s*x, s*y).
 *
 * Every geometric operation is rasterised by Blend2D into an A8 coverage
 * mask, then each device pixel with at least half coverage is combined
 * with the canvas using the X GC function (drawop), the fill pattern and
 * the colours, exactly as an X server would combine them. That gives X's
 * aliased geometry, all sixteen drawops, drawop=reverse, and stippled
 * fills, through one code path. Only plain text in copy mode is blended
 * by coverage (antialiased).
 *
 * Geometry follows X's conventions: integer coordinates are pixel
 * centres, so outlines, lines, arcs and polygons are drawn through
 * (x + 0.5, y + 0.5); rectangle fills cover [x, x+w) by [y, y+h).
 */
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <blend2d/blend2d.h>
#include "sdlgfx.h"

typedef struct {
   int w, h;			/* logical size */
   int pw, ph;			/* device size */
   int s;			/* scale */
   BLImageCore img;		/* PRGB32 */
   BLImageCore mask;		/* A8 coverage */
   BLContextCore mctx;		/* draws into mask, scaled */
   int dirty, d[4];		/* device x0, y0, x1, y1 */
   } Canvas;

/* A shaped single character, cached: write() draws text one character
 * at a time, and shaping (with OpenType positioning) is the costly part. */
typedef struct {
   uint32_t gid;
   BLGlyphPlacement pl;
   uint8_t ptype;
   uint32_t flags;
   int adv;			/* advance width */
   double x0, x1;		/* horizontal ink extent */
   } Glyph1;

typedef struct {
   BLFontFaceCore face;
   BLFontCore font;
   int ascent, descent, maxw;
   Glyph1 g1[256];
   unsigned char have1[256];
   } Font;

#define OPAQUE 0xFF000000u

/* ---------- canvas life cycle ---------- */

/*
 * X includes a pixel whose centre lies exactly on a shape's top or left
 * edge, and excludes one exactly on its bottom or right edge. Such a
 * pixel is exactly half covered; shifting all geometry up and left by
 * TIE device pixels resolves the tie the same way. (For straight edges,
 * coverage >= 50% is otherwise the same test as "centre inside".)
 */
#define TIE (1.0 / 64)

static void mask_begin(Canvas *c)
{
   BLContextCreateInfo cci;
   double sc[2], tr[2];

   memset(&cci, 0, sizeof cci);			/* synchronous rendering */
   sc[0] = sc[1] = c->s;
   tr[0] = tr[1] = -TIE / c->s;
   bl_context_init_as(&c->mctx, &c->mask, &cci);
   bl_context_apply_transform_op(&c->mctx, BL_TRANSFORM_OP_SCALE, sc);
   bl_context_apply_transform_op(&c->mctx, BL_TRANSFORM_OP_TRANSLATE, tr);
   bl_context_set_fill_rule(&c->mctx, BL_FILL_RULE_EVEN_ODD); /* X's default */
   bl_context_set_stroke_join(&c->mctx, BL_STROKE_JOIN_MITER_CLIP);
   bl_context_set_comp_op(&c->mctx, BL_COMP_OP_SRC_OVER);
}

static void fill_device(Canvas *c, int x0, int y0, int x1, int y1, unsigned int v)
{
   BLImageData d;
   bl_image_make_mutable(&c->img, &d);
   for (int y = y0; y < y1; y++) {
      uint32_t *p = (uint32_t *)((uint8_t *)d.pixel_data + y * d.stride);
      for (int x = x0; x < x1; x++)
         p[x] = v;
   }
}

static void touch(Canvas *c, int x0, int y0, int x1, int y1)
{
   if (x0 >= x1 || y0 >= y1)
      return;
   if (!c->dirty) {
      c->d[0] = x0; c->d[1] = y0; c->d[2] = x1; c->d[3] = y1;
      c->dirty = 1;
   }
   else {
      if (x0 < c->d[0]) c->d[0] = x0;
      if (y0 < c->d[1]) c->d[1] = y0;
      if (x1 > c->d[2]) c->d[2] = x1;
      if (y1 > c->d[3]) c->d[3] = y1;
   }
}

void *cv_new(int w, int h, int scale, unsigned int bg)
{
   Canvas *c = calloc(1, sizeof *c);
   if (!c)
      return NULL;
   if (w < 1) w = 1;
   if (h < 1) h = 1;
   c->w = w; c->h = h; c->s = scale < 1 ? 1 : scale;
   c->pw = w * c->s; c->ph = h * c->s;
   bl_image_init(&c->img);
   bl_image_init(&c->mask);
   if (bl_image_create(&c->img, c->pw, c->ph, BL_FORMAT_PRGB32) != BL_SUCCESS
       || bl_image_create(&c->mask, c->pw, c->ph, BL_FORMAT_A8) != BL_SUCCESS) {
      bl_image_destroy(&c->img);
      bl_image_destroy(&c->mask);
      free(c);
      return NULL;
   }
   fill_device(c, 0, 0, c->pw, c->ph, OPAQUE | bg);
   mask_begin(c);
   touch(c, 0, 0, c->pw, c->ph);
   return c;
}

void cv_free(void *cv)
{
   Canvas *c = cv;
   if (!c)
      return;
   bl_context_destroy(&c->mctx);
   bl_image_destroy(&c->img);
   bl_image_destroy(&c->mask);
   free(c);
}

int cv_scale(void *cv) { return ((Canvas *)cv)->s; }

/* Grow (never shrink) to at least w x h, keeping the pixels and filling
 * new area with bg, as X's resizePixmap does. */
void cv_resize(void *cv, int w, int h, unsigned int bg)
{
   Canvas *c = cv;
   BLImageCore old;
   BLImageData od, nd;
   int ow = c->pw, oh = c->ph;

   if (w <= c->w && h <= c->h)
      return;
   if (w < c->w) w = c->w;
   if (h < c->h) h = c->h;
   bl_context_destroy(&c->mctx);
   bl_image_init_move(&old, &c->img);
   c->w = w; c->h = h;
   c->pw = w * c->s; c->ph = h * c->s;
   bl_image_init(&c->img);
   bl_image_create(&c->img, c->pw, c->ph, BL_FORMAT_PRGB32);
   bl_image_destroy(&c->mask);
   bl_image_init(&c->mask);
   bl_image_create(&c->mask, c->pw, c->ph, BL_FORMAT_A8);
   fill_device(c, 0, 0, c->pw, c->ph, OPAQUE | bg);
   bl_image_get_data(&old, &od);
   bl_image_make_mutable(&c->img, &nd);
   for (int y = 0; y < oh; y++)
      memcpy((uint8_t *)nd.pixel_data + y * nd.stride,
             (uint8_t *)od.pixel_data + y * od.stride, ow * 4);
   bl_image_destroy(&old);
   mask_begin(c);
   touch(c, 0, 0, c->pw, c->ph);
}

/* ---------- combining pixels: the X GC function ---------- */

static inline uint32_t gx(int op, uint32_t s, uint32_t d)
{
   uint32_t r;
   switch (op) {
   case 0:  r = 0; break;			/* clear */
   case 1:  r = s & d; break;			/* and */
   case 2:  r = s & ~d; break;			/* andReverse */
   case 3:  r = s; break;			/* copy */
   case 4:  r = ~s & d; break;			/* andInverted */
   case 5:  r = d; break;			/* noop */
   case 6:  r = s ^ d; break;			/* xor */
   case 7:  r = s | d; break;			/* or */
   case 8:  r = ~(s | d); break;		/* nor */
   case 9:  r = ~s ^ d; break;			/* equiv */
   case 10: r = ~d; break;			/* invert */
   case 11: r = s | ~d; break;			/* orReverse */
   case 12: r = ~s; break;			/* copyInverted */
   case 13: r = ~s | d; break;			/* orInverted */
   case 14: r = ~(s & d); break;		/* nand */
   default: r = 0xFFFFFF; break;		/* set */
   }
   return OPAQUE | (r & 0xFFFFFF);
}

/*
 * The device rectangle an operation may touch: the logical box [x0, x1)
 * by [y0, y1), widened by margin m on every side, clipped to the clip
 * rectangle and the canvas. Exact areas (erase, copy, images, points)
 * pass m = 0; strokes pass STROKE_MARGIN, enough for caps and joins.
 */
#define STROKE_MARGIN(gc) (((gc)->linewidth > 1 ? (gc)->linewidth : 1) + 1.0)

static int region(Canvas *c, const sdlgc *gc, double x0, double y0,
   double x1, double y1, double m, int r[4])
{
   int cx0 = 0, cy0 = 0, cx1 = c->w, cy1 = c->h;

   x0 -= m; y0 -= m; x1 += m; y1 += m;
   if (gc && gc->clipw >= 0) {
      cx0 = gc->clipx; cy0 = gc->clipy;
      cx1 = gc->clipx + gc->clipw; cy1 = gc->clipy + gc->cliph;
   }
   if (x0 < cx0) x0 = cx0;
   if (y0 < cy0) y0 = cy0;
   if (x1 > cx1) x1 = cx1;
   if (y1 > cy1) y1 = cy1;
   if (x0 < 0) x0 = 0;
   if (y0 < 0) y0 = 0;
   if (x1 > c->w) x1 = c->w;
   if (y1 > c->h) y1 = c->h;
   r[0] = (int)floor(x0) * c->s; r[1] = (int)floor(y0) * c->s;
   r[2] = (int)ceil(x1) * c->s;  r[3] = (int)ceil(y1) * c->s;
   return r[0] < r[2] && r[1] < r[3];
}

static void mask_clear(Canvas *c, const int r[4])
{
   BLContextCookie k;
   BLRectI dr;
   dr.x = r[0]; dr.y = r[1]; dr.w = r[2] - r[0]; dr.h = r[3] - r[1];
   bl_context_save(&c->mctx, &k);
   bl_context_apply_transform_op(&c->mctx, BL_TRANSFORM_OP_RESET, NULL);
   bl_context_clear_rect_i(&c->mctx, &dr);
   bl_context_restore(&c->mctx, &k);
}

/*
 * Apply the mask over r to the canvas. which: 0 = normal (fg, with the
 * fill pattern), 1 = background (the off dashes of a striped line).
 * blend: antialiased copy of fg by coverage (plain text only).
 */
/*
 * Row spans: for strokes, the part of each device row of r that the
 * stroke can touch, so that clearing and combining cost in proportion to
 * the line's length rather than its bounding box's area. sa[i], sb[i]
 * bound row r[1] + i; sa >= sb means the row is untouched.
 */
static int *sa, *sb, nspan;

static void apply_rows(Canvas *c, const sdlgc *gc, const int r[4], int which,
   int blend, const int *ra, const int *rb);

static void apply(Canvas *c, const sdlgc *gc, const int r[4], int which, int blend)
{
   apply_rows(c, gc, r, which, blend, NULL, NULL);
}

static void apply_rows(Canvas *c, const sdlgc *gc, const int r[4], int which,
   int blend, const int *ra, const int *rb)
{
   BLImageData id, md;
   uint32_t fg, bg;
   int s = c->s, stip = gc->fillstyle != 0 && gc->pat && gc->patw > 0 && gc->path > 0;

   bl_context_flush(&c->mctx, BL_CONTEXT_FLUSH_SYNC);
   bl_image_make_mutable(&c->img, &id);
   bl_image_get_data(&c->mask, &md);
   fg = gc->fg & 0xFFFFFF;
   bg = gc->bg & 0xFFFFFF;
   if (gc->reverse)
      fg ^= bg;
   for (int y = r[1]; y < r[3]; y++) {
      uint32_t *p = (uint32_t *)((uint8_t *)id.pixel_data + y * id.stride);
      const uint8_t *m = (const uint8_t *)md.pixel_data + y * md.stride;
      int ly = y / s;
      int xa = ra ? ra[y - r[1]] : r[0], xb = rb ? rb[y - r[1]] : r[2];
      for (int x = xa; x < xb; x++) {
         uint32_t src;
         if (m[x] == 0 || (!blend && m[x] < 128))
            continue;
         if (blend) {
            /* (fg * a + d * (255 - a)) / 255, rounded: DIV255 is exact */
            #define DIV255(t) (((t) + 128 + (((t) + 128) >> 8)) >> 8)
            uint32_t d = p[x], a = m[x], ia = 255 - a, rr, g, b;
            rr = DIV255((fg >> 16 & 255) * a + (d >> 16 & 255) * ia);
            g = DIV255((fg >> 8 & 255) * a + (d >> 8 & 255) * ia);
            b = DIV255((fg & 255) * a + (d & 255) * ia);
            p[x] = OPAQUE | rr << 16 | g << 8 | b;
            continue;
         }
         if (which == 1)
            src = bg;
         else if (stip) {
            int lx = x / s;
            if (gc->pat[(ly % gc->path) * gc->patw + (lx % gc->patw)])
               src = fg;
            else if (gc->fillstyle == 3)		/* textured */
               src = bg;
            else					/* masked */
               continue;
         }
         else
            src = fg;
         p[x] = gx(gc->reverse ? 6 : gc->drawop, src, p[x]);
      }
   }
   touch(c, r[0], r[1], r[2], r[3]);
}

/* ---------- geometry ---------- */

static void stroke_setup(Canvas *c, const sdlgc *gc, int caps)
{
   bl_context_set_stroke_width(&c->mctx, gc->linewidth > 0 ? gc->linewidth : 1);
   bl_context_set_stroke_caps(&c->mctx, caps);
}

/* Draw one shape (fill or stroke) through the mask. */
static void shape(Canvas *c, const sdlgc *gc, BLGeometryType type, const void *data,
   int stroke, double x0, double y0, double x1, double y1)
{
   int r[4];
   if (!region(c, gc, x0, y0, x1, y1, STROKE_MARGIN(gc), r))
      return;
   mask_clear(c, r);
   if (stroke) {
      stroke_setup(c, gc, BL_STROKE_CAP_SQUARE);
      bl_context_stroke_geometry_rgba32(&c->mctx, type, data, 0xFFFFFFFFu);
   }
   else
      bl_context_fill_geometry_rgba32(&c->mctx, type, data, 0xFFFFFFFFu);
   apply(c, gc, r, 0, 0);
}

/*
 * Stroke a polyline (closed if the last point equals the first) with
 * the line style. Dashes follow X: on = off = 4 pixels for width <= 1,
 * 3 * width otherwise (as Icon sets them), continuing across vertices.
 * Striped lines draw the off dashes in the background colour.
 */
/*
 * Compute the row spans of a stroked polyline within r into sa/sb:
 * for each device row, the x range within e (half the line width, with
 * room for square caps and antialiasing) of some segment.
 */
static void spans_add(Canvas *c, const sdlgc *gc, const BLPoint *p, int n, const int r[4]);

static int spans(Canvas *c, const sdlgc *gc, const BLPoint *p, int n, const int r[4])
{
   int rows = r[3] - r[1];

   if (rows > nspan) {
      int *na = realloc(sa, rows * sizeof *sa), *nb;
      if (!na) return 0;
      sa = na;
      nb = realloc(sb, rows * sizeof *sb);
      if (!nb) return 0;
      sb = nb;
      nspan = rows;
   }
   for (int i = 0; i < rows; i++) {
      sa[i] = r[2];
      sb[i] = r[0];
   }
   spans_add(c, gc, p, n, r);
   return 1;
}

/* Add a polyline's spans to sa/sb. */
static void spans_add(Canvas *c, const sdlgc *gc, const BLPoint *p, int n, const int r[4])
{
   int s = c->s;
   double e = (gc->linewidth > 1 ? gc->linewidth : 1) * 0.75 + 1.5;

   for (int i = 0; i + 1 < n; i++) {
      double X0 = p[i].x, Y0 = p[i].y, X1 = p[i + 1].x, Y1 = p[i + 1].y;
      double dx = X1 - X0, dy = Y1 - Y0;
      double ylo = fmin(Y0, Y1), yhi = fmax(Y0, Y1);
      double xlo = fmin(X0, X1) - e, xhi = fmax(X0, X1) + e;
      double slope = fabs(dy) > 1e-9 ? dx / dy : 0;
      double hw = fabs(dy) > 1e-9 ? e * hypot(dx, dy) / fabs(dy) + fabs(slope) / s : 0;
      int ya = (int)floor((ylo - e) * s), yb = (int)ceil((yhi + e) * s);
      if (ya < r[1]) ya = r[1];
      if (yb > r[3]) yb = r[3];
      for (int y = ya; y < yb; y++) {
         double lo = xlo, hi = xhi;
         if (fabs(dy) > 1e-9) {
            double yc = (y + 0.5) / s;
            double xc = X0 + (fmin(fmax(yc, ylo), yhi) - Y0) * slope;
            lo = fmax(lo, xc - hw);
            hi = fmin(hi, xc + hw);
         }
         int a = (int)floor(lo * s) - 1, b = (int)ceil(hi * s) + 1;
         if (a < r[0]) a = r[0];
         if (b > r[2]) b = r[2];
         if (a < sa[y - r[1]]) sa[y - r[1]] = a;
         if (b > sb[y - r[1]]) sb[y - r[1]] = b;
      }
   }
}

/* Clear the mask over the row spans. */
static void mask_clear_rows(Canvas *c, const int r[4])
{
   BLImageData md;
   bl_context_flush(&c->mctx, BL_CONTEXT_FLUSH_SYNC);
   bl_image_get_data(&c->mask, &md);
   for (int y = r[1]; y < r[3]; y++) {
      int a = sa[y - r[1]], b = sb[y - r[1]];
      if (a < b)
         memset((uint8_t *)md.pixel_data + y * md.stride + a, 0, (size_t)(b - a));
   }
}

static void polyline(Canvas *c, const sdlgc *gc, const BLPoint *p, int n,
   double x0, double y0, double x1, double y1)
{
   BLPathCore on, off;
   double len, phase = 0;
   int r[4];

   if (n < 2 || !region(c, gc, x0, y0, x1, y1, STROKE_MARGIN(gc), r))
      return;
   if (!spans(c, gc, p, n, r))
      return;
   bl_path_init(&on);
   if (gc->linestyle == 0) {
      bl_path_move_to(&on, p[0].x, p[0].y);
      for (int i = 1; i < n; i++)
         bl_path_line_to(&on, p[i].x, p[i].y);
      if (n > 2 && p[0].x == p[n - 1].x && p[0].y == p[n - 1].y)
         bl_path_close(&on);
      mask_clear_rows(c, r);
      stroke_setup(c, gc, BL_STROKE_CAP_SQUARE);
      bl_context_stroke_geometry_rgba32(&c->mctx, BL_GEOMETRY_TYPE_PATH, &on, 0xFFFFFFFFu);
      apply_rows(c, gc, r, 0, 0, sa, sb);
      bl_path_destroy(&on);
      return;
   }
   bl_path_init(&off);
   len = gc->linewidth > 1 ? 3.0 * gc->linewidth : 4.0;
   for (int i = 0; i + 1 < n; i++) {
      double dx = p[i + 1].x - p[i].x, dy = p[i + 1].y - p[i].y;
      double seg = hypot(dx, dy), t = 0;
      while (t < seg) {
         double inper = fmod(phase, 2 * len), step;
         int isOn = inper < len;
         BLPathCore *pp = isOn ? &on : &off;
         step = fmin((isOn ? len : 2 * len) - inper, seg - t);
         bl_path_move_to(pp, p[i].x + dx * t / seg, p[i].y + dy * t / seg);
         bl_path_line_to(pp, p[i].x + dx * (t + step) / seg, p[i].y + dy * (t + step) / seg);
         t += step;
         phase += step;
      }
   }
   if (gc->linestyle == 2) {
      /*
       * Striped (X's LineDoubleDash): the whole line is drawn, its ends
       * capped; the odd dashes are in bg, the even ones in fg, meeting
       * with butt ends.
       */
      BLPathCore all;
      bl_path_init(&all);
      bl_path_move_to(&all, p[0].x, p[0].y);
      for (int i = 1; i < n; i++)
         bl_path_line_to(&all, p[i].x, p[i].y);
      mask_clear_rows(c, r);
      stroke_setup(c, gc, BL_STROKE_CAP_SQUARE);
      bl_context_stroke_geometry_rgba32(&c->mctx, BL_GEOMETRY_TYPE_PATH, &all, 0xFFFFFFFFu);
      apply_rows(c, gc, r, 1, 0, sa, sb);
      bl_path_destroy(&all);
   }
   mask_clear_rows(c, r);
   stroke_setup(c, gc, gc->linestyle == 2 ? BL_STROKE_CAP_BUTT : BL_STROKE_CAP_SQUARE);
   bl_context_stroke_geometry_rgba32(&c->mctx, BL_GEOMETRY_TYPE_PATH, &on, 0xFFFFFFFFu);
   apply_rows(c, gc, r, 0, 0, sa, sb);
   bl_path_destroy(&on);
   bl_path_destroy(&off);
}

static void bounds(const BLPoint *p, int n, double *x0, double *y0, double *x1, double *y1)
{
   *x0 = *y0 = 1e30; *x1 = *y1 = -1e30;
   for (int i = 0; i < n; i++) {
      if (p[i].x < *x0) *x0 = p[i].x;
      if (p[i].y < *y0) *y0 = p[i].y;
      if (p[i].x > *x1) *x1 = p[i].x;
      if (p[i].y > *y1) *y1 = p[i].y;
   }
}

void cv_points(void *cv, const sdlgc *gc, const int *xy, int n)
{
   Canvas *c = cv;
   for (int i = 0; i < n; i++) {
      int r[4], x = xy[2 * i], y = xy[2 * i + 1];
      if (!region(c, gc, x, y, x + 1, y + 1, 0, r))
         continue;
      r[0] = x * c->s; r[1] = y * c->s; r[2] = r[0] + c->s; r[3] = r[1] + c->s;
      if (gc->clipw >= 0 && (x < gc->clipx || y < gc->clipy ||
          x >= gc->clipx + gc->clipw || y >= gc->clipy + gc->cliph))
         continue;
      if (x < 0 || y < 0 || x >= c->w || y >= c->h)
         continue;
      mask_clear(c, r);
      {
         BLRectI pr = { x, y, 1, 1 };
         bl_context_fill_rect_i_rgba32(&c->mctx, &pr, 0xFFFFFFFFu);
      }
      apply(c, gc, r, 0, 0);
   }
}

void cv_lines(void *cv, const sdlgc *gc, const int *xy, int n)
{
   BLPoint *p = malloc((n > 0 ? n : 1) * sizeof *p);
   double x0, y0, x1, y1;
   if (!p)
      return;
   for (int i = 0; i < n; i++) {
      p[i].x = xy[2 * i] + .5;
      p[i].y = xy[2 * i + 1] + .5;
   }
   if (n == 1) {				/* a single point */
      cv_points(cv, gc, xy, 1);
      free(p);
      return;
   }
   bounds(p, n, &x0, &y0, &x1, &y1);
   polyline(cv, gc, p, n, x0, y0, x1, y1);
   free(p);
}

/* DrawSegment: each segment separately, as X does. */
void cv_segments(void *cv, const sdlgc *gc, const int *sg, int n)
{
   for (int i = 0; i < n; i++)
      cv_lines(cv, gc, sg + 4 * i, 2);
}

void cv_rects(void *cv, const sdlgc *gc, const int *r, int n)
{
   for (int i = 0; i < n; i++) {
      double x = r[4 * i] + .5, y = r[4 * i + 1] + .5;
      double w = r[4 * i + 2], h = r[4 * i + 3];
      BLPoint p[5] = { { x, y }, { x + w, y }, { x + w, y + h }, { x, y + h }, { x, y } };
      if (w == 0 && h == 0) {
         cv_points(cv, gc, r + 4 * i, 1);
         continue;
      }
      polyline(cv, gc, p, 5, x, y, x + w, y + h);
   }
}

void cv_fillrects(void *cv, const sdlgc *gc, const int *r, int n)
{
   for (int i = 0; i < n; i++) {
      BLRect rr;
      rr.x = r[4 * i]; rr.y = r[4 * i + 1]; rr.w = r[4 * i + 2]; rr.h = r[4 * i + 3];
      if (rr.w <= 0 || rr.h <= 0)
         continue;
      shape(cv, gc, BL_GEOMETRY_TYPE_RECTD, &rr, 0, rr.x, rr.y, rr.x + rr.w, rr.y + rr.h);
   }
}

void cv_arcs(void *cv, const sdlgc *gc, const int *r, const double *a, int n, int fill)
{
   Canvas *c = cv;
   for (int i = 0; i < n; i++) {
      double x = r[4 * i], y = r[4 * i + 1], w = r[4 * i + 2], h = r[4 * i + 3];
      BLArc arc;
      arc.cx = x + w / 2 + .5; arc.cy = y + h / 2 + .5;
      arc.rx = w / 2; arc.ry = h / 2;
      arc.start = a[2 * i]; arc.sweep = a[2 * i + 1];
      if (fill) {
         if (fabs(arc.sweep) >= 2 * M_PI - 1e-9) {
            BLEllipse e = { arc.cx, arc.cy, arc.rx, arc.ry };
            shape(c, gc, BL_GEOMETRY_TYPE_ELLIPSE, &e, 0, x, y, x + w + 1, y + h + 1);
         }
         else
            shape(c, gc, BL_GEOMETRY_TYPE_PIE, &arc, 0, x, y, x + w + 1, y + h + 1);
      }
      else {
         /* As a polyline, so that line styles apply. */
         int k = (int)(fabs(arc.sweep) * (arc.rx + arc.ry) / 2) + 8;
         BLPoint *p;
         if (k > 4096) k = 4096;
         p = malloc((k + 1) * sizeof *p);
         if (!p)
            continue;
         for (int j = 0; j <= k; j++) {
            double t = arc.start + arc.sweep * j / k;
            p[j].x = arc.cx + arc.rx * cos(t);
            p[j].y = arc.cy + arc.ry * sin(t);
         }
         polyline(c, gc, p, k + 1, x, y, x + w + 1, y + h + 1);
         free(p);
      }
   }
}

void cv_fillpoly(void *cv, const sdlgc *gc, const int *xy, int n)
{
   BLPoint *p = malloc((n > 0 ? n : 1) * sizeof *p);
   BLArrayView v;
   double x0, y0, x1, y1;
   if (!p || n < 3) {
      free(p);
      return;
   }
   for (int i = 0; i < n; i++) {
      p[i].x = xy[2 * i] + .5;
      p[i].y = xy[2 * i + 1] + .5;
   }
   bounds(p, n, &x0, &y0, &x1, &y1);
   v.data = p;
   v.size = n;
   shape(cv, gc, BL_GEOMETRY_TYPE_POLYGOND, &v, 0, x0, y0, x1, y1);
   free(p);
}

/* EraseArea: fill with bg, copy mode, solid (clip still applies). */
void cv_erase(void *cv, const sdlgc *gc, int x, int y, int w, int h)
{
   Canvas *c = cv;
   int r[4];
   if (!region(c, gc, x, y, x + w, y + h, 0, r))
      return;
   fill_device(c, r[0], r[1], r[2], r[3], OPAQUE | gc->bg);
   touch(c, r[0], r[1], r[2], r[3]);
}

/*
 * CopyArea: copy a region of src to dst at (x2, y2), combining with the
 * drawop and honouring dst's clip. Overlap within one canvas is safe.
 * Between canvases of different scale, logical pixels are resampled.
 */
void cv_copy(void *srcp, void *dstp, const sdlgc *gc,
   int x, int y, int w, int h, int x2, int y2)
{
   Canvas *src = srcp, *dst = dstp;
   BLImageData sd, dd;
   uint32_t *tmp;
   int r[4], op = gc->reverse ? 6 : gc->drawop;
   int ds = dst->s, ss = src->s;
   sdlgc g = *gc;

   /* clamp to the source */
   if (x < 0) { w += x; x2 -= x; x = 0; }
   if (y < 0) { h += y; y2 -= y; y = 0; }
   if (x + w > src->w) w = src->w - x;
   if (y + h > src->h) h = src->h - y;
   if (w <= 0 || h <= 0)
      return;
   if (!region(dst, &g, x2, y2, x2 + w, y2 + h, 0, r))
      return;

   /*
    * The usual case, copy mode at the same scale: move whole rows, in
    * the order that is safe when source and destination overlap.
    */
   if (op == 3 && ss == ds) {
      int ox = x * ss - x2 * ds, oy = y * ss - y2 * ds, n = r[2] - r[0];
      bl_image_get_data(&src->img, &sd);
      bl_image_make_mutable(&dst->img, &dd);
      if (src == dst)
         bl_image_get_data(&src->img, &sd);	/* same (now mutable) pixels */
      if (src == dst && oy < 0)			/* source above: bottom up */
         for (int dy = r[3] - 1; dy >= r[1]; dy--)
            memmove((uint8_t *)dd.pixel_data + dy * dd.stride + r[0] * 4,
                    (uint8_t *)sd.pixel_data + (dy + oy) * sd.stride + (r[0] + ox) * 4,
                    (size_t)n * 4);
      else
         for (int dy = r[1]; dy < r[3]; dy++)
            memmove((uint8_t *)dd.pixel_data + dy * dd.stride + r[0] * 4,
                    (uint8_t *)sd.pixel_data + (dy + oy) * sd.stride + (r[0] + ox) * 4,
                    (size_t)n * 4);
      touch(dst, r[0], r[1], r[2], r[3]);
      return;
   }

   /* otherwise: take a copy of the source region first (overlap) */
   bl_image_get_data(&src->img, &sd);
   tmp = malloc((size_t)w * ss * h * ss * 4);
   if (!tmp)
      return;
   for (int j = 0; j < h * ss; j++)
      memcpy(tmp + (size_t)j * w * ss,
             (uint8_t *)sd.pixel_data + (y * ss + j) * sd.stride + x * ss * 4, (size_t)w * ss * 4);

   bl_image_make_mutable(&dst->img, &dd);
   for (int dy = r[1]; dy < r[3]; dy++) {
      uint32_t *p = (uint32_t *)((uint8_t *)dd.pixel_data + dy * dd.stride);
      int sy = (dy - y2 * ds) * ss / ds;		/* row in tmp */
      if (sy < 0 || sy >= h * ss) continue;
      for (int dx = r[0]; dx < r[2]; dx++) {
         int sx = (dx - x2 * ds) * ss / ds;
         if (sx < 0 || sx >= w * ss) continue;
         p[dx] = gx(op, tmp[(size_t)sy * w * ss + sx] & 0xFFFFFF, p[dx]);
      }
   }
   free(tmp);
   touch(dst, r[0], r[1], r[2], r[3]);
}

/* ---------- pixels ---------- */

void cv_read(void *cv, int x, int y, int w, int h, unsigned int bg, unsigned int *out)
{
   Canvas *c = cv;
   BLImageData d;
   bl_image_get_data(&c->img, &d);
   for (int j = 0; j < h; j++)
      for (int i = 0; i < w; i++) {
         int lx = x + i, ly = y + j;
         if (lx < 0 || ly < 0 || lx >= c->w || ly >= c->h)
            *out++ = bg & 0xFFFFFF;
         else
            *out++ = ((uint32_t *)((uint8_t *)d.pixel_data + ly * c->s * d.stride))[lx * c->s]
                     & 0xFFFFFF;
      }
}

/* Write logical pixels (copy mode), skipping those not opaque, within
 * the clip rectangle. */
void cv_write(void *cv, const sdlgc *gc, int x, int y, int w, int h,
   const unsigned int *px, const unsigned char *opaque)
{
   Canvas *c = cv;
   BLImageData d;
   int r[4], s = c->s;
   if (!region(c, gc, x, y, x + w, y + h, 0, r))
      return;
   bl_image_make_mutable(&c->img, &d);
   for (int dy = r[1]; dy < r[3]; dy++) {
      uint32_t *p = (uint32_t *)((uint8_t *)d.pixel_data + dy * d.stride);
      int j = dy / s - y;
      if (j < 0 || j >= h) continue;
      for (int dx = r[0]; dx < r[2]; dx++) {
         int i = dx / s - x;
         if (i < 0 || i >= w) continue;
         if (opaque && !opaque[j * w + i]) continue;
         p[dx] = OPAQUE | (px[j * w + i] & 0xFFFFFF);
      }
   }
   touch(c, r[0], r[1], r[2], r[3]);
}

static const char *ext(const char *path)
{
   const char *e = strrchr(path, '.');
   return e ? e + 1 : "";
}

int cv_save(void *cv, const char *path, int x, int y, int w, int h)
{
   Canvas *c = cv;
   const char *e = ext(path);
   BLImageCore out;
   BLImageData d;
   BLImageCodecCore codec;
   int ok;

   if (strcasecmp(e, "png") && strcasecmp(e, "jpg") && strcasecmp(e, "jpeg")
       && strcasecmp(e, "bmp") && strcasecmp(e, "qoi"))
      return -1;
   bl_image_init(&out);
   if (bl_image_create(&out, w, h, BL_FORMAT_XRGB32) != BL_SUCCESS)
      return 0;
   bl_image_make_mutable(&out, &d);
   for (int j = 0; j < h; j++)
      cv_read(c, x, y + j, w, 1, 0, (unsigned int *)((uint8_t *)d.pixel_data + j * d.stride));
   for (int j = 0; j < h; j++) {
      uint32_t *p = (uint32_t *)((uint8_t *)d.pixel_data + j * d.stride);
      for (int i = 0; i < w; i++)
         p[i] |= OPAQUE;
   }
   bl_image_codec_init(&codec);
   ok = bl_image_codec_find_by_extension(&codec, e, SIZE_MAX, NULL) == BL_SUCCESS
        && bl_image_write_to_file(&out, path, &codec) == BL_SUCCESS;
   bl_image_codec_destroy(&codec);
   bl_image_destroy(&out);
   return ok;
}

unsigned int *cv_load(const char *path, int *w, int *h)
{
   BLImageCore img;
   BLImageData d;
   unsigned int *px;

   bl_image_init(&img);
   if (bl_image_read_from_file(&img, path, NULL) != BL_SUCCESS
       || bl_image_convert(&img, BL_FORMAT_PRGB32) != BL_SUCCESS) {
      bl_image_destroy(&img);
      return NULL;
   }
   bl_image_get_data(&img, &d);
   *w = d.size.w;
   *h = d.size.h;
   px = malloc((size_t)*w * *h * 4);
   if (px)
      for (int j = 0; j < *h; j++)
         memcpy(px + (size_t)j * *w, (uint8_t *)d.pixel_data + j * d.stride, (size_t)*w * 4);
   bl_image_destroy(&img);
   return px;
}

void cv_pixels(void *cv, unsigned char **p, long *stride, int *pw, int *ph)
{
   Canvas *c = cv;
   BLImageData d;
   bl_image_get_data(&c->img, &d);
   *p = d.pixel_data;
   *stride = (long)d.stride;
   *pw = c->pw;
   *ph = c->ph;
}

void cv_dirty(void *cv, int x, int y, int w, int h)
{
   Canvas *c = cv;
   int r[4];
   if (region(c, NULL, x, y, x + w, y + h, 0, r))
      touch(c, r[0], r[1], r[2], r[3]);
}

int cv_take_dirty(void *cv, int r[4])
{
   Canvas *c = cv;
   if (!c->dirty)
      return 0;
   memcpy(r, c->d, sizeof c->d);
   c->dirty = 0;
   return 1;
}

/* ---------- text ---------- */

/* Icon strings are bytes; they are treated as Latin-1, as X's ISO 8859-1
 * fonts do (BL_TEXT_ENCODING_LATIN1). */

/* The cached shaping of single character ch. */
static Glyph1 *glyph1(Font *f, unsigned char ch)
{
   Glyph1 *g = &f->g1[ch];
   if (!f->have1[ch]) {
      BLGlyphBufferCore gb;
      BLTextMetrics tm;
      const BLGlyphRun *run;
      char c1 = (char)ch;
      bl_glyph_buffer_init(&gb);
      bl_glyph_buffer_set_text(&gb, &c1, 1, BL_TEXT_ENCODING_LATIN1);
      bl_font_shape(&f->font, &gb);
      bl_font_get_text_metrics(&f->font, &gb, &tm);
      run = bl_glyph_buffer_get_glyph_run(&gb);
      memset(g, 0, sizeof *g);
      if (run->size >= 1) {
         g->gid = *(const uint32_t *)run->glyph_data;
         if (run->placement_data)
            g->pl = *(const BLGlyphPlacement *)run->placement_data;
         g->ptype = run->placement_type;
         g->flags = run->flags;
      }
      g->adv = (int)lround(tm.advance.x);
      g->x0 = tm.bounding_box.x0;
      g->x1 = tm.bounding_box.x1;
      bl_glyph_buffer_destroy(&gb);
      f->have1[ch] = 1;
   }
   return g;
}

int ft_width(void *fp, const char *s, int n)
{
   Font *f = fp;
   BLGlyphBufferCore gb;
   BLTextMetrics tm;
   if (!f || n <= 0)
      return 0;
   if (n == 1)
      return glyph1(f, (unsigned char)s[0])->adv;
   bl_glyph_buffer_init(&gb);
   bl_glyph_buffer_set_text(&gb, s, (size_t)n, BL_TEXT_ENCODING_LATIN1);
   bl_font_shape(&f->font, &gb);
   bl_font_get_text_metrics(&f->font, &gb, &tm);
   bl_glyph_buffer_destroy(&gb);
   return (int)lround(tm.advance.x);
}

/* DrawString (image = 0) or the text output of write() (image = 1: the
 * text's cell is first filled with bg, in copy mode, as X's
 * XDrawImageString does). The string is shaped once, and the glyph run
 * drawn; returns the text's width. */
int cv_text(void *cv, const sdlgc *gc, void *fp, int x, int y,
   const char *s, int n, int image)
{
   Canvas *c = cv;
   Font *f = fp;
   BLGlyphBufferCore gb;
   BLTextMetrics tm;
   BLGlyphRun one;
   const BLGlyphRun *run;
   Glyph1 *g1 = NULL;
   BLPoint o;
   int r[4], w, plain;
   sdlgc g = *gc;

   if (!f || n <= 0)
      return 0;
   bl_glyph_buffer_init(&gb);
   if (n == 1) {				/* cached */
      g1 = glyph1(f, (unsigned char)s[0]);
      memset(&one, 0, sizeof one);
      one.glyph_data = &g1->gid;
      one.placement_data = &g1->pl;
      one.size = 1;
      one.placement_type = g1->ptype;
      one.glyph_advance = (int8_t)sizeof(uint32_t);
      one.placement_advance = (int8_t)sizeof(BLGlyphPlacement);
      one.flags = g1->flags;
      run = &one;
      w = g1->adv;
      tm.bounding_box.x0 = g1->x0;
      tm.bounding_box.x1 = g1->x1;
   }
   else {
      bl_glyph_buffer_set_text(&gb, s, (size_t)n, BL_TEXT_ENCODING_LATIN1);
      bl_font_shape(&f->font, &gb);
      bl_font_get_text_metrics(&f->font, &gb, &tm);
      run = bl_glyph_buffer_get_glyph_run(&gb);
      w = (int)lround(tm.advance.x);
   }
   if (image) {
      g.drawop = 3; g.reverse = 0; g.fillstyle = 0;
      cv_erase(c, &g, x, y - f->ascent, w, f->ascent + f->descent);
   }
   /* horizontally, the shaped text's ink (at least its advance);
    * vertically the font's ascent and descent; a pixel to spare */
   if (region(c, &g, x + fmin(0, tm.bounding_box.x0) - 1, y - f->ascent - 1,
              x + fmax(w, tm.bounding_box.x1) + 1, y + f->descent + 1, 0, r)) {
      mask_clear(c, r);
      o.x = x; o.y = y;
      bl_context_fill_glyph_run_d_rgba32(&c->mctx, &o, &f->font, run, 0xFFFFFFFFu);
      plain = g.drawop == 3 && !g.reverse && g.fillstyle == 0;
      apply(c, &g, r, 0, plain);
   }
   bl_glyph_buffer_destroy(&gb);
   return w;
}

/* Open a font; the face is found by fontlookup() (platform-specific). */
int font_lookup(const char *family, int bold, int italic,
   char *path, int pathlen, char *psname, int pslen);

void *ft_open(const char *family, int bold, int italic, int size)
{
   char path[1024], ps[256];
   BLFontDataCore fd;
   BLFontDesignMetrics dm;
   BLFontMetrics m;
   Font *f;
   int found = -1;

   if (!font_lookup(family, bold, italic, path, sizeof path, ps, sizeof ps))
      return NULL;
   bl_font_data_init(&fd);
   if (bl_font_data_create_from_file(&fd, path, 0) != BL_SUCCESS) {
      bl_font_data_destroy(&fd);
      return NULL;
   }
   f = calloc(1, sizeof *f);
   if (!f) {
      bl_font_data_destroy(&fd);
      return NULL;
   }
   bl_font_face_init(&f->face);
   /* a .ttc holds several faces: take the one the lookup named */
   for (uint32_t i = 0; i < bl_font_data_get_face_count(&fd) && found < 0; i++) {
      BLStringCore nm;
      bl_string_init(&nm);
      if (bl_font_face_create_from_data(&f->face, &fd, i) == BL_SUCCESS) {
         bl_font_face_get_post_script_name(&f->face, &nm);
         if (!strcmp(bl_string_get_data(&nm), ps))
            found = (int)i;
      }
      bl_string_destroy(&nm);
   }
   if (found < 0 && bl_font_face_create_from_data(&f->face, &fd, 0) != BL_SUCCESS) {
      bl_font_data_destroy(&fd);
      bl_font_face_destroy(&f->face);
      free(f);
      return NULL;
   }
   bl_font_data_destroy(&fd);
   bl_font_init(&f->font);
   bl_font_create_from_face(&f->font, &f->face, (float)size);
   bl_font_get_metrics(&f->font, &m);
   bl_font_face_get_design_metrics(&f->face, &dm);
   f->ascent = (int)ceil(m.ascent);
   f->descent = (int)ceil(m.descent);
   /*
    * The maximum character width, as X's max_bounds.width: over the
    * printable Latin-1 characters (an X core font's repertoire), not the
    * whole font, whose symbols can be far wider.
    */
   f->maxw = 0;
   for (int ch = 32; ch < 256; ch++) {
      char s1 = (char)ch;
      int cw;
      if (ch >= 127 && ch < 160)
         continue;
      cw = ft_width(f, &s1, 1);
      if (cw > f->maxw)
         f->maxw = cw;
   }
   if (f->maxw == 0)
      f->maxw = dm.units_per_em > 0
         ? (int)ceil((double)dm.h_max_advance * size / dm.units_per_em) : size;
   return f;
}

void ft_metrics(void *fp, int *ascent, int *descent, int *maxwidth)
{
   Font *f = fp;
   *ascent = f->ascent;
   *descent = f->descent;
   *maxwidth = f->maxw;
}
