#pragma once
/* after this, DEFINE_GUID defines its GUID instead of declaring it (windows.h) */
#include <windows.h>
#define INITGUID
#undef DEFINE_GUID
#define DEFINE_GUID(n, l, w1, w2, b1, b2, b3, b4, b5, b6, b7, b8) \
    const GUID n = { l, w1, w2, { b1, b2, b3, b4, b5, b6, b7, b8 } }
