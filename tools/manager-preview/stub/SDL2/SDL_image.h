/* The PC preview draws no screenshots: SDL2_image's three calls, empty */
#pragma once
#include <SDL2/SDL.h>
#define IMG_INIT_PNG 2
inline int IMG_Init(int flags) { return flags; }
inline void IMG_Quit() {}
inline SDL_Surface *IMG_Load(const char *) { return nullptr; }
