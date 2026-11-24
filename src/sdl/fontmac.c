/*
 * fontmac.c -- font discovery on macOS via CoreText (plain C API).
 * (icon-sdl fork; MIT licence, see LICENSE.md.)
 * The Linux equivalent would use fontconfig (FcFontMatch -> FC_FILE, FC_INDEX).
 */
#include <CoreText/CoreText.h>
#include "sdlgfx.h"

int font_lookup(const char *family, int bold, int italic,
   char *path, int pathlen, char *psname, int pslen);

int font_lookup(const char *family, int bold, int italic,
                char *path, int pathlen, char *psname, int pslen)
{
   CFStringRef name;
   CTFontRef f, g;
   CFURLRef url;
   CFStringRef ps;
   CTFontSymbolicTraits want = 0;
   int ok = 0;

   name = CFStringCreateWithCString(NULL, family, kCFStringEncodingUTF8);
   f = CTFontCreateWithName(name, 12.0, NULL);
   CFRelease(name);
   if (!f)
      return 0;
   if (bold) want |= kCTFontBoldTrait;
   if (italic) want |= kCTFontItalicTrait;
   if (want) {
      g = CTFontCreateCopyWithSymbolicTraits(f, 0.0, NULL, want, want);
      if (g) { CFRelease(f); f = g; }
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
