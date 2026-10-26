#pragma once

/* Dev tool: count and log every DirectDraw/Direct3D call ([debug] api_census=1).
 * Install from DllMain (before the game creates DirectDraw). */
void apicensus_install(void);
void apicensus_frame(void);   /* once per frame */
