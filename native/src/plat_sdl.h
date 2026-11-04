#pragma once
/* plat_sdl.c's own: the game's window, for the renderer (a Vulkan surface on it) */
#include <SDL3/SDL.h>

SDL_Window *plat_sdl_window(void);
