"""Add an import of mml.dll!mml_entry to MEGAMAN.EXE so Windows loads our DLL
before the game starts (no injector, no Frida).

The original import descriptors are copied into a new section (.mmli) with
one more descriptor appended, and the import data directory is pointed at the
copy. Every existing import thunk stays where it was, so code that calls
through the old IAT slots (e.g. GetTickCount at 0x508328) is unaffected.

Usage: python patch_add_import.py IN.EXE OUT.EXE [DLL] [SYMBOL]
Also importable: add_import(data, dll, symbol) -> bytes (used by make_patcher.py).
"""
import struct
import sys

SECTION_NAME = b".mmli"


def align(v, a):
    return (v + a - 1) // a * a


def has_import_section(data):
    """True if the exe already carries our .mmli import section."""
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    nsec = struct.unpack_from("<H", data, pe + 6)[0]
    tab = pe + 24 + struct.unpack_from("<H", data, pe + 20)[0]
    return any(data[tab + 40 * i:tab + 40 * i + 8].rstrip(b"\0") == SECTION_NAME for i in range(nsec))


def add_import(data, dll="mml.dll", symbol="mml_entry"):
    """Return a copy of exe `data` that also imports dll!symbol."""
    d = bytearray(data)
    dll = dll.encode()
    sym = symbol.encode()
    pe = struct.unpack_from("<I", d, 0x3C)[0]
    nsec = struct.unpack_from("<H", d, pe + 6)[0]
    optsz = struct.unpack_from("<H", d, pe + 20)[0]
    opt = pe + 24
    sect_align, file_align = struct.unpack_from("<II", d, opt + 32)
    size_of_image = struct.unpack_from("<I", d, opt + 56)[0]
    size_of_headers = struct.unpack_from("<I", d, opt + 60)[0]
    dirs = opt + 96
    sect_tab = opt + optsz

    secs = [struct.unpack_from("<8sIIIIIIHHI", d, sect_tab + 40 * i) for i in range(nsec)]
    if any(s[0].rstrip(b"\0") == SECTION_NAME for s in secs):
        sys.exit("already has a %s section" % SECTION_NAME.decode())
    if sect_tab + 40 * (nsec + 1) > size_of_headers:
        sys.exit("no room for another section header")

    def rva2off(rva):
        for s in secs:
            vsz = max(s[1], s[3])
            if s[2] <= rva < s[2] + vsz and s[4]:
                return rva - s[2] + s[4]
        raise ValueError("rva %x not in file" % rva)

    imp_rva = struct.unpack_from("<I", d, dirs + 8)[0]
    o = rva2off(imp_rva)
    descs = []
    while True:
        desc = bytes(d[o:o + 20])
        if desc == b"\0" * 20:
            break
        descs.append(desc)
        o += 20
    names = [d[rva2off(struct.unpack_from("<I", x, 12)[0]):].split(b"\0")[0] for x in descs]
    if dll.lower() in (n.lower() for n in names):
        raise ValueError("%s is already imported" % dll.decode())

    # New section layout (offsets relative to section start).
    sec_rva = align(size_of_image, sect_align)
    n_desc = len(descs) + 2                      # old + ours + terminator
    off_desc = 0
    off_int = align(off_desc + 20 * n_desc, 8)
    off_iat = off_int + 8
    off_hint = off_iat + 8
    off_dll = align(off_hint + 2 + len(sym) + 1, 2)
    used = off_dll + len(dll) + 1
    raw_size = align(used, file_align)
    body = bytearray(raw_size)
    for i, desc in enumerate(descs):
        body[off_desc + 20 * i:off_desc + 20 * (i + 1)] = desc
    struct.pack_into("<IIIII", body, off_desc + 20 * len(descs),
                     sec_rva + off_int, 0, 0, sec_rva + off_dll, sec_rva + off_iat)
    struct.pack_into("<I", body, off_int, sec_rva + off_hint)
    struct.pack_into("<I", body, off_iat, sec_rva + off_hint)
    body[off_hint + 2:off_hint + 2 + len(sym)] = sym
    body[off_dll:off_dll + len(dll)] = dll

    raw_ptr = align(len(d), file_align)
    d += b"\0" * (raw_ptr - len(d))
    d += body

    struct.pack_into("<8sIIIIIIHHI", d, sect_tab + 40 * nsec, SECTION_NAME, used, sec_rva,
                     raw_size, raw_ptr, 0, 0, 0, 0, 0xC0000040)   # initialized, read, write
    struct.pack_into("<H", d, pe + 6, nsec + 1)
    struct.pack_into("<I", d, opt + 56, align(sec_rva + used, sect_align))
    struct.pack_into("<II", d, dirs + 8, sec_rva + off_desc, 20 * n_desc)
    struct.pack_into("<II", d, dirs + 8 * 11, 0, 0)                # no stale bound imports
    struct.pack_into("<I", d, opt + 64, 0)                         # checksum unused for exes

    return bytes(d)


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    src, dst = sys.argv[1], sys.argv[2]
    dll = sys.argv[3] if len(sys.argv) > 3 else "mml.dll"
    sym = sys.argv[4] if len(sys.argv) > 4 else "mml_entry"
    try:
        out = add_import(open(src, "rb").read(), dll, sym)
    except ValueError as e:
        sys.exit(str(e))
    open(dst, "wb").write(out)
    print("wrote %s with an import of %s!%s" % (dst, dll, sym))


if __name__ == "__main__":
    main()
