#pragma once

/* Our intro player (Modernization\intro.mpg via pl_mpeg + the D3D11 renderer +
 * SDL3 audio) in place of the game's MCI one. Needs the modern renderer. */
int intro_video_install(const char *game_dir);
