#pragma once

/* Dev tool: hardware write watchpoints ([debug] watch=ADDR,SIZE;...). */
void watch_configure(const char *spec);
void watch_frame(void);   /* call once per frame on the game thread */
