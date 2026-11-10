"""Where the addresses the mod uses in the US MEGAMAN.EXE are in the Japanese rockman.exe
(Rockman DASH, the same 2001 build lineage: the same sections at the same addresses, the code
moved around by small insertions and edits).

    python tools/jp/map_exe.py US_EXE JP_EXE [--out map.txt] [--verbose]

The addresses come from the sources: constants in native/src, the hook sites in
tools/recomp/patch_sites.txt and the byte patches in src/patch_manifest.py.

How an address is found:
  code  the bytes around it, with relocated dwords masked (they hold addresses that move), are
        looked up in the Japanese code; the address maps when a window holding it is found exactly
        once and the bytes at the address itself are the same.
  data  the US instructions that refer to it (a relocated operand at or a little below it, a
        struct base plus a field) are found in the Japanese code through the code map; their
        Japanese operands say where it went. Votes from every reference.
  rest  between two mapped neighbours that moved by the same amount (data no code refers to
        directly: a table's later entries, bss next to referenced variables).
Each line of the output: US address, Japanese address (or ?), how it was found, the votes."""
import bisect
import collections
import os
import re
import struct
import sys

BASE = 0x400000
REPO = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..'))


class Pe:
    def __init__(self, path):
        b = open(path, 'rb').read()
        e = struct.unpack_from('<I', b, 0x3C)[0]
        ns = struct.unpack_from('<H', b, e + 6)[0]
        opt = struct.unpack_from('<H', b, e + 20)[0]
        self.secs = {}
        end = 0
        for i in range(ns):
            o = e + 24 + opt + i * 40
            nm = b[o:o + 8].rstrip(b'\0').decode()
            va, rs, ra = struct.unpack_from('<III', b, o + 12)
            self.secs[nm] = (BASE + va, rs, ra)
            end = max(end, va + rs)
        self.img = bytearray(end)
        for nm, (va, rs, ra) in self.secs.items():
            if ra:
                self.img[va - BASE:va - BASE + rs] = b[ra:ra + rs]
        self.file = b
        va, rs, ra = self.secs['.reloc']
        r = b[ra:ra + rs]
        self.rel = set()
        p = 0
        while p + 8 <= len(r):
            page, sz = struct.unpack_from('<II', r, p)
            if sz == 0:
                break
            for k in range(8, sz, 2):
                t = struct.unpack_from('<H', r, p + k)[0]
                if t >> 12 == 3:
                    self.rel.add(BASE + page + (t & 0xFFF))
            p += sz
        self.masked = bytearray(self.img)
        for a in self.rel:
            self.masked[a - BASE:a - BASE + 4] = b'\xAA\xAA\xAA\xAA'
        self.strict = bytes(self.masked)   # relocations masked only
        self.code =self.secs['AUTO'][0], self.secs['AUTO'][0] + self.secs['AUTO'][1]
        self.calls = {}   # call/jmp rel32 site -> target
        self.mask_branches()

    def mask_branches(self):
        """rel32 calls and jumps: their distance changes whenever the code between moved"""
        import capstone
        cs = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
        lo, hi = self.code
        code = bytes(self.img[lo - BASE:hi - BASE])
        pos = 0
        while pos < len(code):
            last = pos
            for ins in cs.disasm(code[pos:], lo + pos):
                last = ins.address - lo + ins.size
                b0 = ins.bytes[0]
                if ins.size == 5 and b0 in (0xE8, 0xE9) or ins.size == 6 and b0 == 0x0F and 0x80 <= ins.bytes[1] <= 0x8F:
                    at = ins.address + ins.size - 4
                    self.masked[at - BASE:at - BASE + 4] = b'\xBB\xBB\xBB\xBB'
                    self.calls[ins.address] = (ins.address + ins.size + struct.unpack_from('<i', ins.bytes, ins.size - 4)[0]) & 0xFFFFFFFF
            pos = last if last > pos else pos + 1

    def dword(self, va):
        return struct.unpack_from('<I', self.img, va - BASE)[0]

    def m(self, va, n):
        return bytes(self.masked[va - BASE:va - BASE + n])

    def s(self, va, n):
        return self.strict[va - BASE:va - BASE + n]

    def file_to_va(self, off):
        for nm, (va, rs, ra) in self.secs.items():
            if ra and ra <= off < ra + rs:
                return va + off - ra
        return None

    def va_to_file(self, va):
        for nm, (sva, rs, ra) in self.secs.items():
            if ra and sva <= va < sva + rs:
                return ra + va - sva
        return None


WINDOWS = (24, 48)   # window lengths; every position in the Japanese code is indexed


class CodeMap:
    """first the bytes as they are (relocations masked), then with call/jump distances masked
    too; a longer window when the short one is not unique"""
    def __init__(self, us, jp):
        self.us, self.jp = us, jp
        self.idx = {}
        lo, hi = jp.code
        for kind in ('s', 'm'):
            get = getattr(jp, kind)
            for w in WINDOWS:
                idx = {}
                for a in range(lo, hi - w):
                    k = get(a, w)
                    idx[k] = None if k in idx else a
                self.idx[kind, w] = idx
        self.cache = {}

    def map(self, a):
        """the Japanese address of US code address a, or None"""
        if a in self.cache:
            return self.cache[a]
        us, res = self.us, None
        for kind in ('s', 'm'):
            get, jget = getattr(us, kind), getattr(self.jp, kind)
            for w in WINDOWS:
                votes = collections.Counter()
                for s in range(a - w + 1, a + 1):   # windows that hold a
                    if s < us.code[0] or s + w > us.code[1]:
                        continue
                    j = self.idx[kind, w].get(get(s, w))
                    if j:
                        votes[j - s] += 1
                if votes:
                    d, n = votes.most_common(1)[0]
                    if us.m(a, 4) == self.jp.m(a + d, 4):
                        res = a + d
                        break
            if res is not None:
                break
        self.cache[a] = res
        return res


def data_map(us, jp, cm, addrs):
    """US data address -> Counter of Japanese candidates, from the code that refers to it"""
    refs = collections.defaultdict(list)   # value -> relocated operand positions in US code
    for p in us.rel:
        if us.code[0] <= p < us.code[1]:
            refs[us.dword(p)].append(p)
    vals = sorted(refs)
    out = {}
    for a in addrs:
        votes = collections.Counter()
        i = bisect.bisect_right(vals, a)
        for v in vals[max(0, i - 12):i]:   # an operand at a or a struct base up to 0x200 below it
            if a - v > 0x200:
                continue
            for p in refs[v]:
                q = cm.map(p)
                if q and q in jp.rel:
                    votes[jp.dword(q) + (a - v)] += 3 if v == a else 1
        out[a] = votes
    return out


def merge(anchors, cover):
    """sorted (address, delta) anchors -> [start, end, delta] runs of one delta; an anchor whose
    delta neither neighbour shares is dropped (a chance match)"""
    keep = [x for i, x in enumerate(anchors)
            if (i > 0 and anchors[i - 1][1] == x[1]) or (i + 1 < len(anchors) and anchors[i + 1][1] == x[1])]
    runs = []
    for a, d in keep:
        if runs and runs[-1][2] == d:
            runs[-1][1] = a + cover
        else:
            runs.append([a, a + cover, d])
    for i in range(len(runs) - 1):   # (a window can reach past the next run's first anchor)
        runs[i][1] = min(runs[i][1], runs[i + 1][0])
    # a short run that moved far from both neighbours is repeated bytes matched to another copy
    keep = []
    for i, r in enumerate(runs):
        near = [runs[k][2] for k in (i - 1, i + 1) if 0 <= k < len(runs)]
        if r[1] - r[0] < 0x100 and near and all(abs(r[2] - d) > 0x400 for d in near):
            continue
        keep.append(r)
    runs = keep
    # where the Japanese code got shorter, the last window of a run can reach into the next run's
    # Japanese bytes: no byte may be claimed twice
    for i in range(len(runs) - 1):
        over = (runs[i][1] + runs[i][2]) - (runs[i + 1][0] + runs[i + 1][2])
        if over > 0:
            runs[i][1] -= over
    return [r for r in runs if r[1] > r[0]]


def code_segments(us, jp, cm):
    """runs of US code that moved by one amount: windows at every 4 bytes found once in Japanese"""
    w = WINDOWS[0]
    anchors = []
    lo, hi = us.code
    for a in range(lo, hi - w, 4):
        j = cm.idx['s', w].get(us.s(a, w)) or cm.idx['m', w].get(us.m(a, w))
        if j:
            anchors.append((a, j - a))
    return merge(anchors, w)


def xlat(segs, a):
    i = bisect.bisect_right(segs, [a, 0xFFFFFFFF, 0]) - 1
    if i >= 0 and segs[i][0] <= a < segs[i][1]:
        return a + segs[i][2]
    return None


def data_segments(us, jp, csegs):
    """runs of US data (imports, initialized data and the zeroed tail) that moved by one amount:
    from every relocated operand in the code; a run reaches as far as the bytes stay the same"""
    lo, hi = us.secs['.idata'][0], us.secs['.bss'][0]
    votes = collections.defaultdict(collections.Counter)
    for p in us.rel:
        if us.code[0] <= p < us.code[1]:
            v = us.dword(p)
            q = xlat(csegs, p)
            if lo <= v < hi and q is not None and q in jp.rel and us.m(p, 4) == jp.m(q, 4):
                votes[v][jp.dword(q) - v] += 1
    anchors = sorted((v, c.most_common(1)[0][0]) for v, c in votes.items())
    runs = merge(anchors, 4)
    dva, drs, _ = us.secs['DGROUP']
    for i, r in enumerate(runs):   # grow each run while the initialized bytes match
        stop = runs[i + 1][0] if i + 1 < len(runs) else hi
        e = r[1]
        while e < stop and (e >= dva + drs or us.img[e - BASE] == jp.img[e + r[2] - BASE]):
            e += 1
        r[1] = e
    for i in range(len(runs) - 1):   # no Japanese byte claimed twice (as in merge)
        over = (runs[i][1] + runs[i][2]) - (runs[i + 1][0] + runs[i + 1][2])
        if over > 0:
            runs[i][1] -= over
    return [r for r in runs if r[1] > r[0]]


def emit_c(path, segs, us_sha, jp_sha):
    with open(path, 'w', newline='\n') as f:
        f.write('/* GENERATED by tools/jp/map_exe.py: do not edit.\n'
                ' * Where runs of the US MEGAMAN.EXE (sha256 %s...) are in the Japanese\n'
                ' * rockman.exe (sha256 %s...): US start, US end, how far it moved. */\n'
                '#include "addr.h"\n\n'
                'const addr_seg addr_jp_segs[] = {\n' % (us_sha[:16], jp_sha[:16]))
        for s, e, d in segs:
            f.write('    { 0x%06Xu, 0x%06Xu, %d },\n' % (s, e, d))
        f.write('};\nconst int addr_jp_nsegs = (int)(sizeof addr_jp_segs / sizeof addr_jp_segs[0]);\n')


def source_addresses():
    """(address, where) from the mod's sources"""
    found = collections.defaultdict(set)
    pat = re.compile(r'0x0{0,2}([4-8][0-9A-Fa-f]{5})u?\b')
    src = os.path.join(REPO, 'native', 'src')
    for f in sorted(os.listdir(src)):
        if not f.endswith(('.c', '.h')) or f.startswith('recomp_') and f.endswith('_gen.c'):
            continue
        if f in ('addr_jp.c', 'addr.c'):   # this tool's own output, and the probe of both programs
            continue
        for ln, line in enumerate(open(os.path.join(src, f), encoding='latin-1'), 1):
            for m in pat.finditer(line):
                found[int(m.group(1), 16)].add('%s:%d' % (f, ln))
    sites = os.path.join(REPO, 'tools', 'recomp', 'patch_sites.txt')
    for line in open(sites):
        p = line.split()
        if p and not line.startswith('#'):
            try:
                found[int(p[0], 16)].add('patch_sites')
            except ValueError:
                pass
    return found


def manifest_patches():
    ns = {}
    exec(open(os.path.join(REPO, 'src', 'patch_manifest.py')).read(), ns)
    return ns['PATCHES']


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    verbose = '--verbose' in sys.argv
    out = sys.argv[sys.argv.index('--out') + 1] if '--out' in sys.argv else None
    us, jp = Pe(args[0]), Pe(args[1])
    cm = CodeMap(us, jp)
    found = source_addresses()
    for off, hx in manifest_patches():
        va = us.file_to_va(off)
        if va and va >= us.code[0]:
            found[va].add('manifest')
    limit = max(nm[0] + nm[1] for nm in us.secs.values())
    addrs = sorted(a for a in found if BASE < a < limit)

    res = {}
    for a in addrs:
        if us.code[0] <= a < us.code[1]:
            j = cm.map(a)
            if j is not None:
                res[a] = (j, 'code', '')
    dm = data_map(us, jp, cm, [a for a in addrs if a not in res and not (us.code[0] <= a < us.code[1])])
    dva, drs, _ = us.secs['DGROUP']
    for a, votes in dm.items():
        if not votes:
            continue
        total = sum(votes.values())
        if dva <= a < dva + drs:   # initialized data: the candidate has to hold the same bytes
            raw = lambda x, n: bytes(x.img[n - BASE:n - BASE + 4])   # real bytes first: masked pointers all look alike
            same = collections.Counter({j: n for j, n in votes.items() if raw(us, a) == raw(jp, j)}) or                 collections.Counter({j: n for j, n in votes.items() if us.m(a, 4) == jp.m(j, 4)})
            if same:
                (j, n), = same.most_common(1)
                res[a] = (j, 'data', '%d/%d same bytes' % (n, total))
                continue
        (j, n), = votes.most_common(1)
        res[a] = (j, 'data', '%d/%d' % (n, total) + (' split' if n * 2 <= total else ''))
    # the rest: between two mapped neighbours that moved by the same amount
    known = sorted((a, j - a) for a, (j, how, _) in res.items() if how in ('code', 'data'))
    ka = [k[0] for k in known]
    for a in addrs:
        if a in res:
            continue
        i = bisect.bisect_left(ka, a)
        if 0 < i < len(known) and known[i - 1][1] == known[i][1]:
            lo, hi = known[i - 1][0], known[i][0]
            same_kind = (us.code[0] <= lo < us.code[1]) == (us.code[0] <= a < us.code[1]) == (us.code[0] <= hi < us.code[1])
            if same_kind and hi - lo < 0x4000:
                res[a] = (a + known[i][1], 'between', '%06X..%06X' % (lo, hi))

    if '--emit-c' in sys.argv:   # the runs mml.dll translates with
        import hashlib
        csegs = code_segments(us, jp, cm)
        segs = sorted(csegs + data_segments(us, jp, csegs) +
                      [[us.secs['.bss'][0], us.secs['.bss'][0] + us.secs['.bss'][1], 0]])
        bad = 0
        for a in addrs:   # every address the mod uses: the runs have to agree with the point map
            j = res.get(a, (None,))[0]
            if j is None or res[a][1] == 'between':
                continue
            if xlat(segs, a) is not None and xlat(segs, a) != j:
                print('runs and point map disagree: %06X -> %06X (runs, kept), point map %06X' % (a, xlat(segs, a), j))
                continue
            if xlat(segs, a) is None:
                print('not in a run: %06X -> %s, point map %06X; added' % (a, '%06X' % xlat(segs, a) if xlat(segs, a) else '?', j))
                bad += 1
                n = 8 if us.code[0] <= a < us.code[1] else 4
                cut = [s for s in segs if s[1] <= a or s[0] >= a + n]
                for s in segs:   # split a run that held a wrongly
                    if s[0] < a < s[1]:
                        cut.append([s[0], a, s[2]])
                    if s[0] < a + n < s[1]:
                        cut.append([a + n, s[1], s[2]])
                segs = sorted(cut + [[a, a + n, j - a]])
        sha = lambda p: hashlib.sha256(open(p, 'rb').read()).hexdigest()
        emit_c(sys.argv[sys.argv.index('--emit-c') + 1], segs, sha(args[0]), sha(args[1]))
        cover = collections.Counter()
        for s, e, d in segs:
            cover['code' if s < us.code[1] else 'bss' if s >= us.secs['.bss'][0] else 'data'] += e - s
        print('%d runs (%d fixed); covered: code %d of %d bytes, data %d of %d' % (
            len(segs), bad, cover['code'], us.code[1] - us.code[0], cover['data'],
            us.secs['.bss'][0] - us.secs['.idata'][0]))

    lines = []
    kinds = collections.Counter()
    for a in addrs:
        j, how, why = res.get(a, (None, 'unmapped', ''))
        kinds[how] += 1
        lines.append('%06X %-7s %-9s %-14s %s' % (a, '%06X' % j if j is not None else '?', how, why,
                                              ' '.join(sorted(found[a]))[:120]))
    if out:
        with open(out, 'w') as f:
            f.write('\n'.join(lines) + '\n')
    for line in lines:
        if verbose or ' unmapped ' in line or ' split' in line:
            print(line)
    print('%d addresses: %s' % (len(addrs), ', '.join('%s %d' % kv for kv in kinds.most_common())))


if __name__ == '__main__':
    if len(sys.argv) < 3:
        raise SystemExit(__doc__.split('\n\n')[1])
    main()
