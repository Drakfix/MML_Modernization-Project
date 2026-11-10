"""The static patches of the US MEGAMAN.EXE, made for the Japanese rockman.exe.

    python tools/jp/patch_jp_exe.py US_VANILLA US_PATCHED JP_VANILLA JP_OUT

US_PATCHED is the maintainer's patched exe (the static patches; the import of mml.dll is added
here, as make_patcher.py does for the US one). Every run of changed bytes is carried over to the
same instructions in the Japanese program (tools/jp/map_exe.py finds them): the instructions
around it have to be the same there (relocated addresses and call distances aside), and in the
patched instructions every call or jump distance and every address of code or initialized data
is translated. Anything that doesn't fit stops the tool."""
import os
import struct
import sys

import capstone
from capstone import x86

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
from map_exe import BASE, CodeMap, Pe, code_segments, data_segments, xlat   # noqa: E402
from patch_add_import import add_import, has_import_section                   # noqa: E402

LO, HI = 0x401000, 0x559000   # code and initialized data: what moved


def runs(van, pat):
    out, i, n = [], 0, min(len(van), len(pat))
    while i < n:
        if van[i] != pat[i]:
            j = i
            while j < n and van[j] != pat[j]:
                j += 1
            out.append((i, j))
            i = j
        else:
            i += 1
    return out


class Translator:
    def __init__(self, us, jp):
        self.us, self.jp = us, jp
        cm = CodeMap(us, jp)
        c = code_segments(us, jp, cm)
        self.segs = sorted(c + data_segments(us, jp, c) + [[us.secs['.bss'][0], us.secs['.bss'][0] + us.secs['.bss'][1], 0]])
        self.cs = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
        self.cs.detail = True
        lo, hi = us.code
        self.starts = set()   # instruction starts of the US code (linear sweep, as map_exe)
        code = bytes(us.img[lo - BASE:hi - BASE])
        pos = 0
        while pos < len(code):
            last = pos
            for ins in capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32).disasm(code[pos:], lo + pos):
                self.starts.add(ins.address)
                last = ins.address - lo + ins.size
            pos = last if last > pos else pos + 1

    def addr(self, a):
        if not LO <= a < HI:
            return a   # .bss, our own memory past the image: the same in both
        t = xlat(self.segs, a)
        if t is None:
            raise SystemExit('no Japanese counterpart for %06X' % a)
        return t

    def code_run(self, img_pat, s, e):
        """US patched bytes [s, e) of code -> (Japanese address, bytes)"""
        us, jp = self.us, self.jp
        i0 = s
        while i0 not in self.starts:   # whole instructions: back to the one the run starts in
            i0 -= 1
        # forward over the PATCHED instructions until past the run (it may change boundaries)
        pat = bytes(img_pat[i0 - BASE:i0 - BASE + (e - i0) + 16])
        ins_list, end = [], i0
        for ins in self.cs.disasm(pat, i0):
            ins_list.append(ins)
            end = ins.address + ins.size
            if end >= e:
                break
        while end not in self.starts:   # and to a vanilla boundary
            end += 1
        j0 = self.addr(i0)
        if us.m(i0, end - i0) != jp.m(j0, end - i0):
            raise SystemExit('code at %06X (Japanese %06X) differs: %s vs %s' % (
                i0, j0, us.m(i0, end - i0).hex(), jp.m(j0, end - i0).hex()))
        out = bytearray(img_pat[i0 - BASE:end - BASE])
        for ins in ins_list:
            k = ins.address - i0
            b0 = ins.bytes[0]
            if ins.size == 5 and b0 in (0xE8, 0xE9) or ins.size == 6 and b0 == 0x0F and 0x80 <= ins.bytes[1] <= 0x8F:
                rel = struct.unpack_from('<i', ins.bytes, ins.size - 4)[0]
                t = self.addr((ins.address + ins.size + rel) & 0xFFFFFFFF)
                struct.pack_into('<i', out, k + ins.size - 4, t - (j0 + k + ins.size))
                continue
            for off in (ins.disp_offset, ins.imm_offset):   # absolute addresses in operands
                if off and off + 4 <= ins.size:
                    v = struct.unpack_from('<I', ins.bytes, off)[0]
                    if LO <= v < HI:
                        struct.pack_into('<I', out, k + off, self.addr(v))
        # bytes past the patched instructions up to the vanilla boundary are the Japanese ones
        tail = ins_list[-1].address + ins_list[-1].size - i0
        out[tail:] = jp.img[j0 + tail - BASE:j0 + (end - i0) - BASE]
        return j0, bytes(out), i0, end

    def data_run(self, img_pat, s, e):
        us, jp = self.us, self.jp
        j0 = self.addr(s)
        a0, a1 = s & ~3, (e + 3) & ~3
        if bytes(us.img[a0 - BASE:a1 - BASE]) != bytes(jp.img[a0 + (j0 - s) - BASE:a1 + (j0 - s) - BASE]):
            raise SystemExit('data at %06X (Japanese %06X) differs' % (s, j0))
        out = bytearray(img_pat[s - BASE:e - BASE])
        for k in range(a0, a1, 4):   # pointers into code or initialized data
            if s <= k and k + 4 <= e:
                v = struct.unpack_from('<I', img_pat, k - BASE)[0]
                if LO <= v < HI:
                    struct.pack_into('<I', out, k - s, self.addr(v))
        return j0, bytes(out)


def copy_sections(us_van, us_pat, jp):
    """the empty sections the patched exe appends (memory of ours past the image, the same
    addresses in both programs): their entries, the section count and the image size"""
    def hdr(b):
        e = struct.unpack_from('<I', b, 0x3C)[0]
        return e, struct.unpack_from('<H', b, e + 6)[0], e + 24 + struct.unpack_from('<H', b, e + 20)[0]
    ue, un, ut = hdr(us_van)
    pe_, pn, pt = hdr(us_pat)
    je, jn, jt = hdr(jp)
    if un != jn or any(us_van[ut + 40 * i + 12:ut + 40 * i + 16] != jp[jt + 40 * i + 12:jt + 40 * i + 16] for i in range(un)):
        raise SystemExit('the Japanese sections are not where the US ones are')
    out = bytearray(jp)
    for i in range(un, pn):
        ent = us_pat[pt + 40 * i:pt + 40 * i + 40]
        if struct.unpack_from('<I', ent, 16)[0]:
            raise SystemExit('an added section with file data: not handled')
        if jt + 40 * i + 40 > struct.unpack_from('<I', jp, je + 24 + 60)[0]:
            raise SystemExit('no room for another section entry')
        out[jt + 40 * i:jt + 40 * i + 40] = ent
    struct.pack_into('<H', out, je + 6, pn)
    struct.pack_into('<I', out, je + 24 + 56, struct.unpack_from('<I', us_pat, pe_ + 24 + 56)[0])
    return out, un, pn


def build(us_van_p, us_pat_p, jp_van_p):
    """-> (the patched Japanese exe with the mml.dll import, report lines)"""
    us, jp = Pe(us_van_p), Pe(jp_van_p)
    us_pat = open(us_pat_p, 'rb').read()
    if has_import_section(us_pat):
        raise SystemExit('give the patched exe without the mml.dll import')
    van_file = us.file
    img_pat = bytearray(us.img)   # the patched US image, by address
    for nm, (va, rs, ra) in us.secs.items():
        if ra:
            img_pat[va - BASE:va - BASE + rs] = us_pat[ra:ra + rs]
    t = Translator(us, jp)
    jp_img, n0, n1 = copy_sections(van_file, us_pat, jp.file)
    done = []
    pending = runs(van_file, us_pat)
    covered = 0
    first_raw = min(ra for va, rs, ra in us.secs.values() if ra)
    for s_off, e_off in pending:
        if s_off < first_raw:   # the header: copy_sections did it
            continue
        if s_off < covered:
            continue
        s = us.file_to_va(s_off)
        if s is None:
            raise SystemExit('patch outside the sections at file offset %X' % s_off)
        e = s + (e_off - s_off)
        if us.code[0] <= s < us.code[1]:
            j, b, i0, end = t.code_run(img_pat, s, e)
            covered = us.va_to_file(end)
            kind = 'code'
        else:
            j, b = t.data_run(img_pat, s, e)
            i0, end, kind = s, e, 'data'
        jo = jp.va_to_file(j)
        jp_img[jo:jo + len(b)] = b
        done.append((kind, i0, j, b))
    report = ['header: %d sections -> %d (empty ones past the image)' % (n0, n1)]
    report += ['%s %06X -> %06X  %s' % (kind, i0, j, b.hex()) for kind, i0, j, b in done]
    report.append('%d patches carried over' % len(done))
    return add_import(bytes(jp_img)), report


def main(us_van_p, us_pat_p, jp_van_p, out_p):
    out, report = build(us_van_p, us_pat_p, jp_van_p)
    open(out_p, 'wb').write(out)
    print('\n'.join(report))
    print('wrote', out_p)


if __name__ == '__main__':
    if len(sys.argv) != 5:
        raise SystemExit(__doc__.split('\n\n')[1])
    main(*sys.argv[1:])
