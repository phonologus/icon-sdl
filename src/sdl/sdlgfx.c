/*
 * sdlgfx.c -- SDL3 + Blend2D side of Icon's SDL graphics backend.
 * (icon-sdl fork; MIT licence, see LICENSE.md.)
 */
#include <SDL3/SDL.h>
#include <blend2d/blend2d.h>
#include "sdlgfx.h"

int sdlgfx_probe(void)
{
   BLImageCore img;
   int ok, v = SDL_GetVersion();

   bl_image_init(&img);
   ok = bl_image_create(&img, 1, 1, BL_FORMAT_PRGB32) == BL_SUCCESS;
   bl_image_destroy(&img);
   if (!ok)
      return 0;
   return SDL_VERSIONNUM_MAJOR(v) * 10000 + SDL_VERSIONNUM_MINOR(v) * 100
      + SDL_VERSIONNUM_MICRO(v);
}
