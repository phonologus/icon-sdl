/*
 * sdlgfx.h -- interface between Icon's runtime (rsdlwin.ri, processed by
 * rtt) and the SDL3 + Blend2D code in src/sdl (plain C). Only plain C
 * types may appear here: rtt never sees SDL or Blend2D headers.
 */

/* Check that SDL3 and Blend2D are linked and working; returns the SDL
 * version as major*10000 + minor*100 + patch, or 0 on failure. */
int sdlgfx_probe(void);
