/*
 * plat.c -- the platform layer of Icon's SDL graphics backend: SDL3
 * windows and events. (icon-sdl fork; MIT licence, see LICENSE.md.)
 *
 * Threading. macOS requires window and event calls on the process's main
 * thread, but Icon 9.5 runs each co-expression on its own pthread
 * (src/common/rswitch.c), and any of them may open windows or wait for
 * events. So pl_main() runs the interpreter on a new thread and keeps the
 * main thread for SDL:
 *
 *   - until the first window opens, the main thread just waits on a
 *     condition variable for requests: programs that use no graphics
 *     never start SDL;
 *   - the first request initialises SDL; from then on the main thread
 *     waits in SDL_WaitEvent, and requests arrive as SDL user events
 *     (SDL_RunOnMainThread does not wake SDL_WaitEvent on macOS);
 *   - SDL events are translated here (characters, X keysyms, X modifier
 *     masks, millisecond times) and queued; the interpreter drains the
 *     queue into Icon's event lists (rsdlwin.ri), and redraws on expose
 *     happen here without involving it.
 *
 * Icon threads strictly alternate, so at most one request is ever in
 * flight, and a canvas is never touched by two threads at once: while
 * the main thread presents a canvas, the Icon thread waits for it.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <pthread.h>
#include <sys/resource.h>
#include <SDL3/SDL.h>
#include "sdlgfx.h"

/* X modifier masks, as in sdlwin.h */
#define X_SHIFT   (1L << 0)
#define X_CONTROL (1L << 2)
#define X_MOD1    (1L << 3)

typedef struct Win {
   SDL_Window *sw;
   SDL_Renderer *r;
   SDL_Texture *tex;
   SDL_WindowID id;
   int tw, th;				/* texture size (device pixels) */
   int vw, vh;				/* shown part (device pixels) */
   struct Win *next;
   } Win;

static Win *wins;			/* main thread only */

/* ---------- requests to the main thread ---------- */

static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
static void (*req_fn)(void *);		/* pending request (before SDL) */
static void *req_arg;
static int req_done;
static int sdl_up;			/* SDL initialised (main thread loop) */
static Uint32 ev_call;

static void call_main(void (*fn)(void *), void *arg)
{
   pthread_mutex_lock(&mu);
   req_done = 0;
   if (!sdl_up) {
      req_fn = fn;
      req_arg = arg;
      pthread_cond_broadcast(&cv);
   }
   else {
      SDL_Event e;
      memset(&e, 0, sizeof e);
      e.type = ev_call;
      e.user.data1 = (void *)fn;
      e.user.data2 = arg;
      SDL_PushEvent(&e);
   }
   while (!req_done)
      pthread_cond_wait(&cv, &mu);
   pthread_mutex_unlock(&mu);
}

static void finish_call(void)
{
   pthread_mutex_lock(&mu);
   req_done = 1;
   pthread_cond_broadcast(&cv);
   pthread_mutex_unlock(&mu);
}

/* ---------- the event queue (main thread -> interpreter) ---------- */

#define QSIZE 1024
static pl_event q[QSIZE];
static int qhead, qn;
static pthread_cond_t qcv = PTHREAD_COND_INITIALIZER;

static void enqueue(pl_event *e)
{
   pthread_mutex_lock(&mu);
   if (qn < QSIZE) {
      q[(qhead + qn) % QSIZE] = *e;
      qn++;
   }
   pthread_cond_broadcast(&qcv);
   pthread_mutex_unlock(&mu);
}

int pl_poll(pl_event *e)
{
   int ok = 0;
   pthread_mutex_lock(&mu);
   if (qn > 0) {
      *e = q[qhead];
      qhead = (qhead + 1) % QSIZE;
      qn--;
      ok = 1;
   }
   pthread_mutex_unlock(&mu);
   return ok;
}

int pl_wait(int timeout_ms)
{
   int ok;
   pthread_mutex_lock(&mu);
   if (timeout_ms < 0)
      while (qn == 0)
         pthread_cond_wait(&qcv, &mu);
   else if (qn == 0) {
      struct timespec ts;
      clock_gettime(CLOCK_REALTIME, &ts);
      ts.tv_sec += timeout_ms / 1000;
      ts.tv_nsec += (long)(timeout_ms % 1000) * 1000000;
      if (ts.tv_nsec >= 1000000000) { ts.tv_sec++; ts.tv_nsec -= 1000000000; }
      pthread_cond_timedwait(&qcv, &mu, &ts);
   }
   ok = qn > 0;
   pthread_mutex_unlock(&mu);
   return ok;
}

/* ---------- translating SDL events ---------- */

static Win *find(SDL_WindowID id)
{
   for (Win *w = wins; w; w = w->next)
      if (w->id == id)
         return w;
   return NULL;
}

static int meta_is_command = -1;	/* &meta: Option (default) or Command */

static long xmods(SDL_Keymod m)
{
   long x = 0;
   if (meta_is_command < 0) {
      const char *s = getenv("ICONSDL_META");
      meta_is_command = s && !SDL_strcasecmp(s, "command");
   }
   if (m & SDL_KMOD_SHIFT) x |= X_SHIFT;
   if (m & SDL_KMOD_CTRL) x |= X_CONTROL;
   if (m & (meta_is_command ? SDL_KMOD_GUI : SDL_KMOD_ALT)) x |= X_MOD1;
   return x;
}

/* X keysyms for keys that produce no character (X11/keysymdef.h). */
static long keysym(SDL_Keycode k)
{
   switch (k) {
   case SDLK_LEFT: return 0xFF51;
   case SDLK_UP: return 0xFF52;
   case SDLK_RIGHT: return 0xFF53;
   case SDLK_DOWN: return 0xFF54;
   case SDLK_PAGEUP: return 0xFF55;
   case SDLK_PAGEDOWN: return 0xFF56;
   case SDLK_END: return 0xFF57;
   case SDLK_HOME: return 0xFF50;
   case SDLK_INSERT: return 0xFF63;
   case SDLK_PRINTSCREEN: return 0xFF61;
   case SDLK_PAUSE: return 0xFF13;
   case SDLK_SCROLLLOCK: return 0xFF14;
   case SDLK_HELP: return 0xFF6A;
   case SDLK_MENU: return 0xFF67;
   case SDLK_F1: return 0xFFBE;  case SDLK_F2: return 0xFFBF;
   case SDLK_F3: return 0xFFC0;  case SDLK_F4: return 0xFFC1;
   case SDLK_F5: return 0xFFC2;  case SDLK_F6: return 0xFFC3;
   case SDLK_F7: return 0xFFC4;  case SDLK_F8: return 0xFFC5;
   case SDLK_F9: return 0xFFC6;  case SDLK_F10: return 0xFFC7;
   case SDLK_F11: return 0xFFC8; case SDLK_F12: return 0xFFC9;
   default: return 0;
   }
}

/*
 * A key press as X's XLookupString would report it: a character (with
 * shift and control applied) or, for other keys, a keysym. Modifier keys
 * alone report nothing.
 */
static void key(SDL_KeyboardEvent *ke)
{
   pl_event e;
   SDL_Keymod m = ke->mod;
   SDL_Keycode k;
   float fx, fy;
   Win *w = find(ke->windowID);
   long ch = -1, ks = 0;

   if (!w)
      return;
   switch (ke->key) {
   case SDLK_RETURN: case SDLK_KP_ENTER: ch = '\r'; break;
   case SDLK_TAB: ch = '\t'; break;
   case SDLK_BACKSPACE: ch = '\b'; break;
   case SDLK_ESCAPE: ch = 033; break;
   case SDLK_DELETE: ch = 0177; break;
   default:
      if ((ks = keysym(ke->key)) != 0)
         break;
      /* the character this layout gives with shift (and caps lock) */
      k = SDL_GetKeyFromScancode(ke->scancode, m & (SDL_KMOD_SHIFT | SDL_KMOD_CAPS), false);
      if (k >= 32 && k < 256 && k != 127)
         ch = (long)k;
      else
         return;				/* modifier or unknown key */
      if (m & SDL_KMOD_CTRL) {
         if (ch >= '@' && ch <= '_') ch -= '@';
         else if (ch >= 'a' && ch <= 'z') ch -= 'a' - 1;
         else if (ch == ' ') ch = 0;
      }
   }
   memset(&e, 0, sizeof e);
   e.win = w;
   e.kind = ks ? PL_KEYSYM : PL_KEYCHAR;
   e.code = ks ? ks : ch;
   SDL_GetMouseState(&fx, &fy);
   e.x = (int)fx;
   e.y = (int)fy;
   e.mods = xmods(m);
   e.ms = (unsigned long)(ke->timestamp / 1000000);
   enqueue(&e);
}

static void render(Win *w)
{
   SDL_FRect src;
   if (!w->tex)
      return;
   src.x = 0; src.y = 0; src.w = (float)w->vw; src.h = (float)w->vh;
   SDL_RenderClear(w->r);
   SDL_RenderTexture(w->r, w->tex, &src, NULL);
   SDL_RenderPresent(w->r);
}

static void handle(SDL_Event *se)
{
   pl_event e;
   Win *w;

   if (se->type == ev_call) {
      ((void (*)(void *))se->user.data1)(se->user.data2);
      finish_call();
      return;
   }
   memset(&e, 0, sizeof e);
   switch (se->type) {
   case SDL_EVENT_KEY_DOWN:
      key(&se->key);
      return;
   case SDL_EVENT_MOUSE_BUTTON_DOWN:
   case SDL_EVENT_MOUSE_BUTTON_UP:
      if (!(w = find(se->button.windowID))) return;
      if (se->button.button > 3) return;
      e.kind = se->type == SDL_EVENT_MOUSE_BUTTON_DOWN ? PL_PRESS : PL_RELEASE;
      e.code = se->button.button == SDL_BUTTON_LEFT ? 1 :
               se->button.button == SDL_BUTTON_MIDDLE ? 2 : 3;
      e.x = (int)se->button.x; e.y = (int)se->button.y;
      e.ms = (unsigned long)(se->button.timestamp / 1000000);
      break;
   case SDL_EVENT_MOUSE_MOTION:
      if (!(w = find(se->motion.windowID))) return;
      e.kind = PL_MOTION;
      e.code = (int)se->motion.state;
      e.x = (int)se->motion.x; e.y = (int)se->motion.y;
      e.ms = (unsigned long)(se->motion.timestamp / 1000000);
      break;
   case SDL_EVENT_WINDOW_RESIZED:
      if (!(w = find(se->window.windowID))) return;
      e.kind = PL_RESIZE;
      e.x = se->window.data1; e.y = se->window.data2;
      break;
   case SDL_EVENT_WINDOW_MOVED:
      if (!(w = find(se->window.windowID))) return;
      e.kind = PL_MOVE;
      e.x = se->window.data1; e.y = se->window.data2;
      break;
   case SDL_EVENT_WINDOW_EXPOSED:
      if (!(w = find(se->window.windowID))) return;
      render(w);
      e.kind = PL_EXPOSE;
      break;
   case SDL_EVENT_WINDOW_MINIMIZED:
   case SDL_EVENT_WINDOW_MAXIMIZED:
   case SDL_EVENT_WINDOW_RESTORED:
      if (!(w = find(se->window.windowID))) return;
      e.kind = PL_STATE;
      e.code = se->type == SDL_EVENT_WINDOW_MINIMIZED ? PL_ICONIC :
               se->type == SDL_EVENT_WINDOW_MAXIMIZED ? PL_MAXIMAL : PL_NORMAL;
      break;
   case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
      if (!(w = find(se->window.windowID))) return;
      e.kind = PL_CLOSE;
      break;
   case SDL_EVENT_QUIT:			/* Command-Q: like closing a window */
      if (!(w = wins)) return;
      e.kind = PL_CLOSE;
      break;
   default:
      return;
   }
   e.win = w;
   e.mods = xmods(SDL_GetModState());
   enqueue(&e);
}

/* ---------- the main thread ---------- */

typedef struct { int (*f)(int, char **); int argc; char **argv; } Start;

static void *interp(void *p)
{
   Start *s = p;
   exit(s->f(s->argc, s->argv));		/* Icon exits from here */
   return NULL;
}

static void init_sdl(void *unused)
{
   (void)unused;
   SDL_SetHint(SDL_HINT_MAC_BACKGROUND_APP, "0");
   if (SDL_Init(SDL_INIT_VIDEO)) {
      ev_call = SDL_RegisterEvents(1);
      sdl_up = 1;
   }
}

/*
 * pl_main() - run f(argc, argv) (the interpreter) on a new thread with a
 * stack as large as the main thread's, and serve it from this thread.
 */
int pl_main(int (*f)(int, char **), int argc, char **argv)
{
   static Start s;
   pthread_t t;
   pthread_attr_t at;
   struct rlimit rl;
   size_t stack = 8 << 20;

   if (getrlimit(RLIMIT_STACK, &rl) == 0) {
      if (rl.rlim_cur == RLIM_INFINITY) stack = 64 << 20;
      else if (rl.rlim_cur > stack) stack = rl.rlim_cur;
   }
   s.f = f; s.argc = argc; s.argv = argv;
   pthread_attr_init(&at);
   pthread_attr_setstacksize(&at, stack);
   if (pthread_create(&t, &at, interp, &s) != 0)
      return f(argc, argv);			/* no thread: no windows either */

   /* phase 1: no SDL until the first request */
   pthread_mutex_lock(&mu);
   while (!sdl_up) {
      while (!req_fn)
         pthread_cond_wait(&cv, &mu);
      void (*fn)(void *) = req_fn;
      void *arg = req_arg;
      req_fn = NULL;
      pthread_mutex_unlock(&mu);
      if (!sdl_up && fn != init_sdl)
         init_sdl(NULL);
      fn(arg);
      pthread_mutex_lock(&mu);
      req_done = 1;
      pthread_cond_broadcast(&cv);
   }
   pthread_mutex_unlock(&mu);

   /* phase 2: SDL's event loop, forever (the interpreter exits) */
   for (;;) {
      SDL_Event e;
      if (SDL_WaitEvent(&e))
         handle(&e);
   }
}

/* ---------- scripted input, for tests ---------- */

/*
 * If ICONSDL_TEST_EVENTS is set when the first window opens, its
 * space-separated items are pushed into SDL's event queue as if typed or
 * clicked in that window, and so go through the same translation:
 *
 *   key:NAME          a key press; NAME as SDL_GetKeyFromName knows it
 *                     ("a", "Return", "Left", "F1"), optionally prefixed
 *                     by shift+, ctrl+, alt+ or cmd+
 *   down:B:X:Y        mouse button B (1 left, 2 middle, 3 right) pressed
 *   up:B:X:Y          and released
 *   move:B:X:Y        pointer moved with button B held (0: none)
 *   close             the window's close box
 */
static void inject(SDL_WindowID id, const char *script)
{
   char *s = SDL_strdup(script), *save = NULL;
   for (char *t = SDL_strtok_r(s, " ", &save); t; t = SDL_strtok_r(NULL, " ", &save)) {
      SDL_Event e;
      int b = 0, x = 0, y = 0;
      memset(&e, 0, sizeof e);
      e.common.timestamp = SDL_GetTicksNS();
      if (!strncmp(t, "key:", 4)) {
         char *name = t + 4;
         SDL_Keymod m = 0;
         for (;;) {
            if (!strncmp(name, "shift+", 6)) { m |= SDL_KMOD_LSHIFT; name += 6; }
            else if (!strncmp(name, "ctrl+", 5)) { m |= SDL_KMOD_LCTRL; name += 5; }
            else if (!strncmp(name, "alt+", 4)) { m |= SDL_KMOD_LALT; name += 4; }
            else if (!strncmp(name, "cmd+", 4)) { m |= SDL_KMOD_LGUI; name += 4; }
            else break;
         }
         e.type = SDL_EVENT_KEY_DOWN;
         e.key.windowID = id;
         e.key.key = SDL_GetKeyFromName(name);
         e.key.scancode = SDL_GetScancodeFromKey(e.key.key, NULL);
         e.key.mod = m;
         e.key.down = true;
      }
      else if (sscanf(t, "down:%d:%d:%d", &b, &x, &y) == 3 ||
               sscanf(t, "up:%d:%d:%d", &b, &x, &y) == 3) {
         e.type = t[0] == 'd' ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
         e.button.windowID = id;
         e.button.button = b == 1 ? SDL_BUTTON_LEFT : b == 2 ? SDL_BUTTON_MIDDLE : SDL_BUTTON_RIGHT;
         e.button.down = t[0] == 'd';
         e.button.x = (float)x; e.button.y = (float)y;
      }
      else if (sscanf(t, "move:%d:%d:%d", &b, &x, &y) == 3) {
         e.type = SDL_EVENT_MOUSE_MOTION;
         e.motion.windowID = id;
         e.motion.state = b ? SDL_BUTTON_MASK(b == 1 ? SDL_BUTTON_LEFT :
            b == 2 ? SDL_BUTTON_MIDDLE : SDL_BUTTON_RIGHT) : 0;
         e.motion.x = (float)x; e.motion.y = (float)y;
      }
      else if (!strcmp(t, "close")) {
         e.type = SDL_EVENT_WINDOW_CLOSE_REQUESTED;
         e.window.windowID = id;
      }
      else
         continue;
      SDL_PushEvent(&e);
   }
   SDL_free(s);
}

/* ---------- window requests (run on the main thread) ---------- */

typedef struct {
   const char *title;
   int x, y, w, h, state, resizable;
   int scale;
   Win *win;
   } OpenReq;

static void open_cb(void *p)
{
   OpenReq *o = p;
   Win *w;
   SDL_WindowFlags fl = SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_HIDDEN;
   float d;

   if (!sdl_up)
      return;
   if (o->resizable) fl |= SDL_WINDOW_RESIZABLE;
   w = calloc(1, sizeof *w);
   if (!w) return;
   w->sw = SDL_CreateWindow(o->title, o->w, o->h, fl);
   if (!w->sw) { free(w); return; }
   w->r = SDL_CreateRenderer(w->sw, NULL);
   if (!w->r) { SDL_DestroyWindow(w->sw); free(w); return; }
   w->id = SDL_GetWindowID(w->sw);
   if (o->x != INT_MIN && o->y != INT_MIN)
      SDL_SetWindowPosition(w->sw, o->x, o->y);
   d = SDL_GetWindowPixelDensity(w->sw);
   o->scale = d >= 1.5f ? (int)(d + 0.5f) : 1;
   if (getenv("ICONSDL_SCALE"))
      o->scale = atoi(getenv("ICONSDL_SCALE")) > 0 ? atoi(getenv("ICONSDL_SCALE")) : 1;
   w->next = wins;
   wins = w;
   o->win = w;
   {
      static int injected = 0;
      const char *script = getenv("ICONSDL_TEST_EVENTS");
      if (script && !injected) {
         injected = 1;
         inject(w->id, script);
      }
   }
}

void *pl_open(const char *title, int x, int y, int w, int h, int state,
   int resizable, int *scale)
{
   OpenReq o;
   o.title = title; o.x = x; o.y = y; o.w = w; o.h = h;
   o.state = state; o.resizable = resizable; o.scale = 1; o.win = NULL;
   call_main(open_cb, &o);
   *scale = o.scale;
   return o.win;
}

static void close_cb(void *p)
{
   Win *w = p, **pp;
   for (pp = &wins; *pp; pp = &(*pp)->next)
      if (*pp == w) { *pp = w->next; break; }
   if (w->tex) SDL_DestroyTexture(w->tex);
   SDL_DestroyRenderer(w->r);
   SDL_DestroyWindow(w->sw);
   free(w);
}

void pl_close(void *win)
{
   if (win)
      call_main(close_cb, win);
}

typedef struct { Win *w; void *canvas; int lw, lh; int r[4]; int full; } PresentReq;

static void present_cb(void *p)
{
   PresentReq *pr = p;
   Win *w = pr->w;
   unsigned char *px;
   long stride;
   int pw, ph, s = cv_scale(pr->canvas);
   SDL_Rect rect;

   cv_pixels(pr->canvas, &px, &stride, &pw, &ph);
   if (w->tex && (w->tw != pw || w->th != ph)) {
      SDL_DestroyTexture(w->tex);
      w->tex = NULL;
   }
   if (!w->tex) {
      w->tex = SDL_CreateTexture(w->r, SDL_PIXELFORMAT_ARGB8888,
                                 SDL_TEXTUREACCESS_STREAMING, pw, ph);
      if (!w->tex) return;
      SDL_SetTextureScaleMode(w->tex, SDL_SCALEMODE_NEAREST);
      w->tw = pw; w->th = ph;
      pr->full = 1;
   }
   if (pr->full) {
      pr->r[0] = 0; pr->r[1] = 0; pr->r[2] = pw; pr->r[3] = ph;
   }
   if (pr->r[2] > pr->r[0] && pr->r[3] > pr->r[1]) {
      rect.x = pr->r[0]; rect.y = pr->r[1];
      rect.w = pr->r[2] - pr->r[0]; rect.h = pr->r[3] - pr->r[1];
      SDL_UpdateTexture(w->tex, &rect, px + pr->r[1] * stride + pr->r[0] * 4, (int)stride);
   }
   w->vw = pr->lw * s < pw ? pr->lw * s : pw;
   w->vh = pr->lh * s < ph ? pr->lh * s : ph;
   render(w);
}

/* Show the canvas's changes in the window, whose logical size is lw x lh
 * (the canvas may be larger). */
void pl_present(void *win, void *canvas, int lw, int lh, int full)
{
   PresentReq pr;
   if (!win || !canvas)
      return;
   pr.w = win; pr.canvas = canvas; pr.lw = lw; pr.lh = lh;
   pr.full = full;
   if (!cv_take_dirty(canvas, pr.r) && !full)
      return;
   call_main(present_cb, &pr);
}

typedef struct { Win *w; int op; int a, b, c, d; const char *s; } WinReq;

enum { R_SHOW, R_TITLE, R_GEOM, R_GETPOS, R_STATE, R_RESIZABLE, R_RAISE,
   R_CURSOR, R_WARP, R_DISPLAY, R_BEEP, R_MOUSE };

static SDL_Cursor *cursors[SDL_SYSTEM_CURSOR_COUNT];

static void win_cb(void *p)
{
   WinReq *q = p;
   Win *w = q->w;
   switch (q->op) {
   case R_SHOW:
      SDL_ShowWindow(w->sw);
      SDL_SyncWindow(w->sw);
      break;
   case R_TITLE:
      SDL_SetWindowTitle(w->sw, q->s);
      break;
   case R_GEOM:
      if (q->a != INT_MIN && q->b != INT_MIN)
         SDL_SetWindowPosition(w->sw, q->a, q->b);
      if (q->c > 0 && q->d > 0)
         SDL_SetWindowSize(w->sw, q->c, q->d);
      SDL_SyncWindow(w->sw);
      break;
   case R_GETPOS:
      SDL_GetWindowPosition(w->sw, &q->a, &q->b);
      break;
   case R_STATE:
      if (q->a == PL_HIDDEN) SDL_HideWindow(w->sw);
      else {
         SDL_ShowWindow(w->sw);
         if (q->a == PL_ICONIC) SDL_MinimizeWindow(w->sw);
         else if (q->a == PL_MAXIMAL) SDL_MaximizeWindow(w->sw);
         else SDL_RestoreWindow(w->sw);
      }
      SDL_SyncWindow(w->sw);
      break;
   case R_RESIZABLE:
      SDL_SetWindowResizable(w->sw, q->a != 0);
      break;
   case R_RAISE:
      SDL_RaiseWindow(w->sw);
      break;
   case R_CURSOR:
      if (q->a >= 0 && q->a < SDL_SYSTEM_CURSOR_COUNT) {
         if (!cursors[q->a])
            cursors[q->a] = SDL_CreateSystemCursor((SDL_SystemCursor)q->a);
         if (cursors[q->a])
            SDL_SetCursor(cursors[q->a]);
      }
      break;
   case R_WARP:
      SDL_WarpMouseInWindow(w->sw, (float)q->a, (float)q->b);
      break;
   case R_DISPLAY: {
      SDL_Rect r;
      SDL_DisplayID id = w ? SDL_GetDisplayForWindow(w->sw) : SDL_GetPrimaryDisplay();
      if (SDL_GetDisplayBounds(id, &r)) { q->a = r.w; q->b = r.h; }
      break;
      }
   case R_BEEP:
      /* SDL has no beep; nothing portable to do yet */
      break;
   case R_MOUSE: {
      float fx, fy;
      SDL_GetGlobalMouseState(&fx, &fy);
      q->a = (int)fx; q->b = (int)fy;
      break;
      }
   }
}

static void winreq(void *win, int op, int a, int b, int c, int d, const char *s, WinReq *out)
{
   WinReq q;
   q.w = win; q.op = op; q.a = a; q.b = b; q.c = c; q.d = d; q.s = s;
   call_main(win_cb, &q);
   if (out) *out = q;
}

void pl_show(void *win) { if (win) winreq(win, R_SHOW, 0, 0, 0, 0, NULL, NULL); }
void pl_title(void *win, const char *s) { if (win) winreq(win, R_TITLE, 0, 0, 0, 0, s, NULL); }
void pl_geometry(void *win, int x, int y, int w, int h)
{
   if (win) winreq(win, R_GEOM, x, y, w, h, NULL, NULL);
}
int pl_position(void *win, int *x, int *y)
{
   WinReq q;
   if (!win) return 0;
   winreq(win, R_GETPOS, 0, 0, 0, 0, NULL, &q);
   *x = q.a; *y = q.b;
   return 1;
}
void pl_state(void *win, int state) { if (win) winreq(win, R_STATE, state, 0, 0, 0, NULL, NULL); }
void pl_resizable(void *win, int on) { if (win) winreq(win, R_RESIZABLE, on, 0, 0, 0, NULL, NULL); }
void pl_raise(void *win) { if (win) winreq(win, R_RAISE, 0, 0, 0, 0, NULL, NULL); }
void pl_cursor(void *win, int cursor) { if (win) winreq(win, R_CURSOR, cursor, 0, 0, 0, NULL, NULL); }
void pl_warp(void *win, int x, int y) { if (win) winreq(win, R_WARP, x, y, 0, 0, NULL, NULL); }
void pl_beep(void) { if (sdl_up) winreq(NULL, R_BEEP, 0, 0, 0, 0, NULL, NULL); }
void pl_display_size(void *win, int *w, int *h)
{
   WinReq q;
   winreq(win, R_DISPLAY, 0, 0, 0, 0, NULL, &q);
   *w = q.a; *h = q.b;
}
int pl_global_mouse(int *x, int *y)
{
   WinReq q;
   if (!sdl_up) return 0;
   winreq(NULL, R_MOUSE, 0, 0, 0, 0, NULL, &q);
   *x = q.a; *y = q.b;
   return 1;
}
