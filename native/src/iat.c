#include <windows.h>
#include <string.h>

#include "hook.h"
#include "iat.h"

int iat_patch(HMODULE mod, const char *dll, const char *func, void *repl, void **orig)
{
    BYTE *base = (BYTE *)mod;
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)base;
    IMAGE_NT_HEADERS32 *nt;
    IMAGE_DATA_DIRECTORY dir;
    IMAGE_IMPORT_DESCRIPTOR *desc;

    if (!mod || dos->e_magic != IMAGE_DOS_SIGNATURE)
        return 0;
    nt = (IMAGE_NT_HEADERS32 *)(base + dos->e_lfanew);
    dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress)
        return 0;
    for (desc = (IMAGE_IMPORT_DESCRIPTOR *)(base + dir.VirtualAddress); desc->Name; desc++) {
        IMAGE_THUNK_DATA32 *names, *slots;
        if (_stricmp((const char *)(base + desc->Name), dll) != 0)
            continue;
        names = (IMAGE_THUNK_DATA32 *)(base + (desc->OriginalFirstThunk ? desc->OriginalFirstThunk
                                                                       : desc->FirstThunk));
        slots = (IMAGE_THUNK_DATA32 *)(base + desc->FirstThunk);
        for (; names->u1.AddressOfData; names++, slots++) {
            IMAGE_IMPORT_BY_NAME *ibn;
            if (IMAGE_SNAP_BY_ORDINAL32(names->u1.Ordinal))
                continue;
            ibn = (IMAGE_IMPORT_BY_NAME *)(base + names->u1.AddressOfData);
            if (strcmp((const char *)ibn->Name, func) != 0)
                continue;
            if (orig)
                *orig = (void *)(uintptr_t)slots->u1.Function;
            return mem_write((uintptr_t)&slots->u1.Function, &repl, sizeof repl);
        }
    }
    return 0;
}
