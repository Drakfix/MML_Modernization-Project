#pragma once
#include <stdint.h>

/* HD texture replacement and dumping ([textures] replace / dump):
 * Modernization\textures\<hash>.png replaces a game texture, dump=1 writes
 * every new one to Modernization\textures\dump. */
void texreplace_init(const char *game_dir, int replace, int dump);
int texreplace_active(void);
/* the replacement for this converted texture (RGBA8), or NULL; dumps it if asked */
const uint32_t *texreplace_filter(const uint32_t *rgba, int w, int h, int *out_w, int *out_h);
void texreplace_set_replace(int on);   /* in-game change; then ddshim_reupload_textures() */
