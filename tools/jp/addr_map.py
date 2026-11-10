"""US MEGAMAN.EXE addresses -> the Japanese rockman.exe, from the same table mml.dll uses
(native/src/addr_jp.c, written by tools/jp/map_exe.py --emit-c). For tools that work on the
Japanese program with US addresses in them (the Ghidra fix-ups, the recompiler).

    from addr_map import jp          # jp(0x4EED08) -> 0x4EEF28; KeyError when it has none
"""
import bisect
import os
import re

TABLE = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'native', 'src', 'addr_jp.c')
_SEGS = sorted((int(a, 16), int(b, 16), int(d)) for a, b, d in re.findall(
    r'\{\s*0x([0-9A-Fa-f]+)u,\s*0x([0-9A-Fa-f]+)u,\s*(-?\d+)\s*\}', open(TABLE).read()))
_STARTS = [s[0] for s in _SEGS]


def jp(us):
    i = bisect.bisect_right(_STARTS, us) - 1
    if i >= 0 and _SEGS[i][0] <= us < _SEGS[i][1]:
        return us + _SEGS[i][2]
    raise KeyError('US address %06X has no counterpart in the Japanese program' % us)
