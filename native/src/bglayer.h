#pragma once

/* Smooth background layers (the sky): sub-pixel scrolling + interpolation. */
int bglayer_install(void);
void bglayer_frame_end(void);   /* call once per frame, right before the display list is walked */
/* For a tile packet of a background layer in the frame being drawn: offset to
 * add to its vertices (ox, oy, 640x480 space), and where it was one tick
 * earlier relative to that (px, py). Returns 0 if the packet isn't a layer
 * tile, 1 with offsets only, 2 with the previous position too. */
int bglayer_offsets(const void *pkt, float *ox, float *oy, float *px, float *py);
