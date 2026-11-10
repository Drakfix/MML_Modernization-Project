#pragma once
/* The app's updater, where there is no installer to run again (Android): looks up the newest
 * release on GitHub, downloads its package and hands it to the system's installer, which asks
 * the player to confirm. The work is MmlActivity's / Updater.java (threads of its own); this
 * starts it and polls it. The start menu's APP page is the front end (menu.c). Elsewhere
 * update_can() is 0 and the rest does nothing. See updater.c. */
#include <stddef.h>

enum { UPD_IDLE, UPD_CHECKING, UPD_UPTODATE, UPD_AVAILABLE, UPD_DOWNLOADING, UPD_READY, UPD_INSTALLING,
       UPD_FAILED };

int update_can(void);                      /* this platform has one */
const char *update_current(void);          /* this app's version, e.g. "0.6.4-beta" */
void update_check_begin(void);             /* look up the newest release */
void update_install_begin(void);           /* download it (when one is available), then install */
/* its state now; msg: a line for the screen; ver: the newest release's version; progress 0..1
   while downloading */
int update_poll(char *msg, size_t n, char *ver, size_t vn, float *progress);
