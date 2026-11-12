#pragma once
/* HTTPS downloads for the Windows start menu (win_http.c). */
#include <stddef.h>
#include <stdint.h>

/* GET url into the file path and/or into memory (*mem, NUL-terminated, free() it; *len its size);
   done / total (may be NULL) follow the transfer. 1 = done with status 200. Blocks. */
int win_http_get(const char *url, const char *path, char **mem, size_t *len, volatile int64_t *done,
                 volatile int64_t *total);
