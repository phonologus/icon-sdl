#  src/sdl: the SDL3 + Blend2D side of the SDL graphics backend
#  (icon-sdl fork). Builds libiconsdl.a, plus the vendored SDL3, Blend2D
#  and AsmJit from ../../deps, for linking into iconx. Does nothing
#  unless the configuration selects SDLGraphics.

all:
	@if grep -q 'define SDLGraphics' ../h/define.h; then \
	   sh build.sh; \
	else \
	   echo "src/sdl: not an SDL configuration; nothing to build"; \
	fi

Clean Pure:
	rm -rf build libiconsdl.a
