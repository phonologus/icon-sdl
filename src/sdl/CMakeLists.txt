# libiconsdl: the C code of Icon's SDL graphics backend that uses SDL3
# and Blend2D directly. It is plain C, compiled outside rtt, and exposes
# only plain-C types to the runtime (see sdlgfx.h).
cmake_minimum_required(VERSION 3.24)
project(iconsdl C CXX)

set(DEPS "${CMAKE_CURRENT_SOURCE_DIR}/build/src")

# SDL3: static, video and events only.
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_TESTS OFF CACHE BOOL "" FORCE)
set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SDL_INSTALL OFF CACHE BOOL "" FORCE)
foreach(sub AUDIO JOYSTICK HAPTIC HIDAPI CAMERA SENSOR POWER GPU)
   set(SDL_${sub} OFF CACHE BOOL "" FORCE)
endforeach()
add_subdirectory("${DEPS}/sdl3" sdl3)

# Blend2D with its JIT; AsmJit sits next to it.
set(BLEND2D_STATIC ON CACHE BOOL "" FORCE)
set(BLEND2D_NO_INSTALL ON CACHE BOOL "" FORCE)
set(ASMJIT_DIR "${DEPS}/asmjit" CACHE PATH "" FORCE)
add_subdirectory("${DEPS}/blend2d" blend2d)

add_library(iconsdl STATIC sdlgfx.c)
set_property(TARGET iconsdl PROPERTY C_STANDARD 11)
target_compile_options(iconsdl PRIVATE -Wall -Wextra)
target_link_libraries(iconsdl PRIVATE SDL3::SDL3-static blend2d::blend2d)
