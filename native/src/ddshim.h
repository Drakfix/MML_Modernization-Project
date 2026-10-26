#pragma once
#include "gfx.h"

/* Point the exe's DirectDrawCreate / DirectDrawEnumerateA imports at our own
 * DirectDraw 4 / Direct3D 3 implementation (renderer=mml). Call from DllMain. */
int ddshim_install(const gfx_options *opt);
