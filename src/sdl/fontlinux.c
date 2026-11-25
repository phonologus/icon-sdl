/*
 * fontlinux.c -- font lookup on Linux (and other non-Apple Unix) via
 * fontconfig. (icon-sdl fork; MIT licence, see LICENSE.md.)
 *
 * Icon's portable families map to fontconfig's generic families. Any
 * other name must be one of the matched font's family names or its full
 * name (ignoring case): fontconfig always returns something, where X
 * fails a request for a font it doesn't have. Only outline fonts (TrueType
 * and OpenType) are used; Blend2D cannot draw bitmap fonts.
 */
#include <string.h>
#include <fontconfig/fontconfig.h>
#include "sdlgfx.h"

int font_lookup(const char *family, int bold, int italic,
   char *path, int pathlen, char *psname, int pslen);

static const char *generic(const char *f)
{
   if (!strcmp(f, "mono") || !strcmp(f, "fixed")) return "monospace";
   if (!strcmp(f, "typewriter"))
      return "Nimbus Mono PS,Liberation Mono,Courier New,Courier,monospace";
   if (!strcmp(f, "sans")) return "sans-serif";
   if (!strcmp(f, "serif")) return "serif";
   return NULL;
}

static int named(FcPattern *m, const char *object, const char *want)
{
   FcChar8 *s;
   for (int i = 0; FcPatternGetString(m, object, i, &s) == FcResultMatch; i++)
      if (FcStrCmpIgnoreCase(s, (const FcChar8 *)want) == 0)
         return 1;
   return 0;
}

int font_lookup(const char *family, int bold, int italic,
   char *path, int pathlen, char *psname, int pslen)
{
   const char *g = generic(family);
   FcPattern *p, *m;
   FcResult res;
   FcChar8 *file, *ps;
   int ok = 0;

   if (!FcInit())
      return 0;
   p = FcPatternCreate();
   if (!p)
      return 0;
   if (g) {
      FcPattern *q = FcNameParse((const FcChar8 *)g);	/* family list */
      FcChar8 *fam;
      for (int i = 0; q && FcPatternGetString(q, FC_FAMILY, i, &fam) == FcResultMatch; i++)
         FcPatternAddString(p, FC_FAMILY, fam);
      if (q) FcPatternDestroy(q);
   }
   else
      FcPatternAddString(p, FC_FAMILY, (const FcChar8 *)family);
   FcPatternAddInteger(p, FC_WEIGHT, bold ? FC_WEIGHT_BOLD : FC_WEIGHT_REGULAR);
   FcPatternAddInteger(p, FC_SLANT, italic ? FC_SLANT_ITALIC : FC_SLANT_ROMAN);
   FcPatternAddBool(p, FC_OUTLINE, FcTrue);
   FcConfigSubstitute(NULL, p, FcMatchPattern);
   FcDefaultSubstitute(p);
   m = FcFontMatch(NULL, p, &res);
   FcPatternDestroy(p);
   if (!m)
      return 0;
   if ((g || named(m, FC_FAMILY, family) || named(m, FC_FULLNAME, family))
       && FcPatternGetString(m, FC_FILE, 0, &file) == FcResultMatch
       && strlen((char *)file) < (size_t)pathlen) {
      strcpy(path, (char *)file);
      if (FcPatternGetString(m, FC_POSTSCRIPT_NAME, 0, &ps) == FcResultMatch
          && strlen((char *)ps) < (size_t)pslen)
         strcpy(psname, (char *)ps);
      else
         psname[0] = '\0';
      ok = 1;
   }
   FcPatternDestroy(m);
   return ok;
}
