#pragma once
#include <stdint.h>

/* Which display-list packet is being drawn right now, and which ordering-table
 * bucket it came from. Bucket = index into the current gfx_ctx's 8-entry root
 * OT (ctx+0x70): 2 = HUD (ctx+0x78), 3 = dialogue/minimap (ctx+0x7c),
 * 4 = the world (big depth OT merges in at ctx+0x80). The walker draws
 * bucket 7 first, down to 0. */
extern volatile const uint8_t *g_cur_pkt;
extern volatile int g_cur_bucket;

int packets_install(void);

/* Which screen edge a HUD sprite belongs to (gauges built by hud_emit_sprite
 * below the top row), or HUD_SIDE_NONE for everything else. */
enum { HUD_SIDE_NONE, HUD_SIDE_LEFT, HUD_SIDE_RIGHT };
int hud_side_of(const void *pkt);
void packets_frame_start(void);   /* call right before the walker runs */

/* Census: per bucket+opcode stats of what reaches DrawPrimitive, dumped to
 * mml.log every few seconds. */
void census_enable(void);
void census_draw(const float *xyzrhw, unsigned stride, unsigned count);
void census_frame(void);
