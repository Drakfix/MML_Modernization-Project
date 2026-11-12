/* kbm.h's key names, on every platform: the names mml.ini's key_* values use (the launcher and the
 * start menu use the same ones), as Windows virtual keys. kbm.c (Windows) and kbm_sdl.c (SDL) read
 * the keys by those codes. */
#include <windows.h>   /* (native/compat elsewhere) */
#include <stdlib.h>
#include <string.h>

#include "kbm.h"

static const struct { const char *name; int vk; } g_key_names[] = {
    { "SPACE", VK_SPACE }, { "TAB", VK_TAB }, { "ENTER", VK_RETURN }, { "BACKSPACE", VK_BACK },
    { "CAPSLOCK", VK_CAPITAL }, { "LSHIFT", VK_LSHIFT }, { "RSHIFT", VK_RSHIFT },
    { "LCTRL", VK_LCONTROL }, { "RCTRL", VK_RCONTROL }, { "LALT", VK_LMENU }, { "RALT", VK_RMENU },
    { "UP", VK_UP }, { "DOWN", VK_DOWN }, { "LEFT", VK_LEFT }, { "RIGHT", VK_RIGHT },
    { "INSERT", VK_INSERT }, { "DELETE", VK_DELETE }, { "HOME", VK_HOME }, { "END", VK_END },
    { "PGUP", VK_PRIOR }, { "PGDN", VK_NEXT },
    { "MOUSE1", VK_LBUTTON }, { "MOUSE2", VK_RBUTTON }, { "MOUSE3", VK_MBUTTON },
    { "MOUSE4", VK_XBUTTON1 }, { "MOUSE5", VK_XBUTTON2 },
    { "NUM0", VK_NUMPAD0 }, { "NUM1", VK_NUMPAD1 }, { "NUM2", VK_NUMPAD2 }, { "NUM3", VK_NUMPAD3 },
    { "NUM4", VK_NUMPAD4 }, { "NUM5", VK_NUMPAD5 }, { "NUM6", VK_NUMPAD6 }, { "NUM7", VK_NUMPAD7 },
    { "NUM8", VK_NUMPAD8 }, { "NUM9", VK_NUMPAD9 },
    { "COMMA", VK_OEM_COMMA }, { "PERIOD", VK_OEM_PERIOD }, { "SLASH", VK_OEM_2 },
    { "SEMICOLON", VK_OEM_1 }, { "QUOTE", VK_OEM_7 }, { "LBRACKET", VK_OEM_4 }, { "RBRACKET", VK_OEM_6 },
    { "MINUS", VK_OEM_MINUS }, { "EQUALS", VK_OEM_PLUS }, { "BACKQUOTE", VK_OEM_3 }, { "BACKSLASH", VK_OEM_5 },
};

int kbm_vk_from_name(const char *name)
{
    size_t i;
    if (!name || !name[0])
        return 0;
    if (!name[1] && ((name[0] >= 'A' && name[0] <= 'Z') || (name[0] >= '0' && name[0] <= '9')))
        return name[0];
    if (!name[1] && name[0] >= 'a' && name[0] <= 'z')
        return name[0] - 'a' + 'A';
    if ((name[0] == 'F' || name[0] == 'f') && name[1] >= '1' && name[1] <= '9') {
        int n = atoi(name + 1);
        if (n >= 1 && n <= 12)
            return VK_F1 + n - 1;
    }
    for (i = 0; i < sizeof g_key_names / sizeof g_key_names[0]; i++)
        if (_stricmp(name, g_key_names[i].name) == 0)
            return g_key_names[i].vk;
    return 0;
}
