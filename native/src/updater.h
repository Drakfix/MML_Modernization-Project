#pragma once
/* The app's updater: looks up the newest release on GitHub, downloads this platform's package,
 * checks it and installs it. Android (updater.c): MmlActivity / Updater.java do the work and hand
 * the APK to the system's installer. Windows and Linux (updater_net.c): the start menu runs
 * MML-Setup.exe, or unpacks the Linux archive over the app; the game itself only checks (the
 * title screen's notice, hints.c). The start menu's GAME page is the front end (menu.c). */
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
