/*
 * fontmac.c -- font lookup on macOS via CoreText (plain C API).
 * (icon-sdl fork; MIT licence, see LICENSE.md.)
 *
 * Icon's portable families map to macOS fonts. Any other name must be
 * the found font's family or full name (ignoring case): CoreText would
 * otherwise substitute a fallback silently, where X fails the request.
 */
#include <string.h>
#include <CoreText/CoreText.h>
#include "sdlgfx.h"

int font_lookup(const char *family, int bold, int italic,
   char *path, int pathlen, char *psname, int pslen);

static const char *generic(const char *f)
{
   if (!strcmp(f, "mono") || !strcmp(f, "fixed")) return "Menlo";
   if (!strcmp(f, "typewriter")) return "Courier";
   if (!strcmp(f, "sans")) return "Helvetica";
   if (!strcmp(f, "serif")) return "Times";
   return NULL;
}

static int same(CFStringRef a, CFStringRef b)
{
   return a && b && CFStringCompare(a, b, kCFCompareCaseInsensitive) == kCFCompareEqualTo;
}

int font_lookup(const char *family, int bold, int italic,
   char *path, int pathlen, char *psname, int pslen)
{
   const char *g = generic(family);
   CFStringRef name, want, fam, full;
   CTFontRef f, t;
   CFURLRef url;
   CFStringRef ps;
   CTFontSymbolicTraits traits = 0;
   int ok = 0;

   name = CFStringCreateWithCString(NULL, g ? g : family, kCFStringEncodingUTF8);
   f = CTFontCreateWithName(name, 12.0, NULL);
   CFRelease(name);
   if (!f)
      return 0;
   if (!g) {				/* must be the font asked for */
      want = CFStringCreateWithCString(NULL, family, kCFStringEncodingUTF8);
      fam = CTFontCopyFamilyName(f);
      full = CTFontCopyFullName(f);
      ok = same(want, fam) || same(want, full);
      if (want) CFRelease(want);
      if (fam) CFRelease(fam);
      if (full) CFRelease(full);
      if (!ok) {
         CFRelease(f);
         return 0;
      }
      ok = 0;
   }
   if (bold) traits |= kCTFontBoldTrait;
   if (italic) traits |= kCTFontItalicTrait;
   if (traits) {
      t = CTFontCreateCopyWithSymbolicTraits(f, 0.0, NULL, traits, traits);
      if (t) { CFRelease(f); f = t; }
   }
   url = CTFontCopyAttribute(f, kCTFontURLAttribute);
   ps = CTFontCopyPostScriptName(f);
   if (url && ps
       && CFURLGetFileSystemRepresentation(url, true, (UInt8 *)path, pathlen)
       && CFStringGetCString(ps, psname, pslen, kCFStringEncodingUTF8))
      ok = 1;
   if (url) CFRelease(url);
   if (ps) CFRelease(ps);
   CFRelease(f);
   return ok;
}
