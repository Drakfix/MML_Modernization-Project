#pragma once
#include <windows.h>

/* Point module `mod`'s import of dll!func at `repl`; the previous target goes
 * to *orig. Returns 1 on success. */
int iat_patch(HMODULE mod, const char *dll, const char *func, void *repl, void **orig);
