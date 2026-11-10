"""Japanese voices for the US Windows PC version, built from the player's own Japanese PC copy.

    python tools/voice/build_jp_voices.py US_GAME_DIR JP_GAME_DIR
    python tools/voice/build_jp_voices.py --emit-c US_GAME_DIR JP_GAME_DIR   (native/src/voice_map.c)

US_GAME_DIR is the Mega Man Legends folder (MEGAMAN.EXE, DAT, SPH), JP_GAME_DIR the Rockman DASH
folder (rockman.exe, DAT, SPH). Writes US_GAME_DIR/SPJ: one bank per US bank, with the US bank's
file name and clip numbering, each clip holding the Japanese audio of that line. Also writes
US_GAME_DIR/DAJ: the few DAT files whose sound banks ("WB" chunks) hold voice samples (the title
screen, the shared bank), each the US file with the Japanese sound banks. mml.dll uses these
instead of SPH and DAT when [general] voices=japanese.

Both versions keep their voices in SPH/*.WBK: a table of uint32 offsets (the first one is the
table's size, the last one the file size) followed by plain RIFF WAV files. Cutscene tracks
(3700E/3701E, 37S00/37S01) match slot for slot. In-game voices are grouped differently (US: 36
banks, JP: 14), so the mapping comes from the scripts: message text in DAT/*.BIN ("BD" chunks of
subtype 0x1F, a uint16 offset table then the messages) plays a voice with control code 0x8B
followed by a uint32 ID = bank << 16 | clip. Both versions have the same messages in the same
files, so the n-th voice of a message in one is the n-th voice of that message in the other.
Where a message has a different number of voices (the US dub often splits a Japanese line in
two), the message's first US voice plays all of its Japanese clips one after the other and its
other US voices become empty entries: mml.dll ignores those, so the Japanese line keeps playing.
Voices the game's code plays directly (mov eax, ID; call play_voice) are paired by call site.

Nothing of the game is stored in this script: the mapping is derived from the two copies."""
import collections
import difflib
import os
import re
import struct
import sys

GAP = 0.12          # seconds of silence between joined clips
RATE = 22050


def wbk_read(path):
    b = open(path, 'rb').read()
    n = struct.unpack_from('<I', b, 0)[0] // 4
    offs = struct.unpack_from('<%dI' % n, b, 0)
    if offs[-1] != len(b):
        raise SystemExit('%s: not a voice bank' % path)
    return [b[offs[i]:offs[i + 1]] for i in range(n - 1)]


def wav_parse(w):
    """(channels, rate, bits, pcm) of a RIFF WAV clip"""
    if w[:4] != b'RIFF' or w[8:12] != b'WAVE':
        raise ValueError('not a WAV')
    p, fmt, data = 12, None, None
    while p + 8 <= len(w):
        tag, size = w[p:p + 4], struct.unpack_from('<I', w, p + 4)[0]
        if tag == b'fmt ':
            fmt = struct.unpack_from('<HHIIHH', w, p + 8)
        elif tag == b'data':
            data = w[p + 8:p + 8 + size]
            break
        p += 8 + size + (size & 1)
    if fmt is None or data is None:
        raise ValueError('WAV without fmt or data')
    return fmt[1], fmt[2], fmt[5], data


def wav_make(ch, rate, bits, pcm):
    align = ch * bits // 8
    fmt = struct.pack('<HHIIHH', 1, ch, rate, rate * align, align, bits)
    return (b'RIFF' + struct.pack('<I', 4 + 8 + len(fmt) + 8 + len(pcm)) + b'WAVE' +
            b'fmt ' + struct.pack('<I', len(fmt)) + fmt + b'data' + struct.pack('<I', len(pcm)) + pcm)


def wbk_write(path, clips):
    n = len(clips) + 1
    offs, pos = [], 4 * n
    for c in clips:
        offs.append(pos)
        pos += len(c)
    offs.append(pos)
    with open(path, 'wb') as f:
        f.write(struct.pack('<%dI' % n, *offs))
        for c in clips:
            f.write(c)


class Exe:
    """the bank names and the voices the code plays directly"""

    def __init__(self, path):
        b = self.b = open(path, 'rb').read()
        e = struct.unpack_from('<I', b, 0x3C)[0]
        ns, opt = struct.unpack_from('<H', b, e + 6)[0], struct.unpack_from('<H', b, e + 20)[0]
        self.secs = []
        for i in range(ns):
            o = e + 24 + opt + i * 40
            vs, va, rs, ra = struct.unpack_from('<IIII', b, o + 8)
            self.secs.append((0x400000 + va, rs, ra))
        s = b.find(b'SPH\\')
        if s < 0:
            raise SystemExit('%s: no SPH file names' % path)
        self.banks = []
        while b[s:s + 4] == b'SPH\\':
            z = b.index(b'\0', s)
            self.banks.append(b[s + 4:z].decode())
            s = z + 1
        code_va, code_size, code_raw = self.secs[0]
        code = b[code_raw:code_raw + code_size]
        # the voice loader: mov eax,[esi*4 + names]; xor ebx,ebx; xor edx,edx; call open, where
        # names is the pointer table whose first entry is the first "SPH\" string
        first = self.va(b.find(b'SPH\\'))
        table = self.va(b.find(struct.pack('<I', first), self.secs[1][2]))
        m = re.search(rb'\x8b\x04\xb5' + re.escape(struct.pack('<I', table)) + rb'\x31\xdb\x31\xd2\xe8', code)
        if not m:
            raise SystemExit('%s: voice loader not found' % path)
        loader = code_va + m.start() - 0xF
        play_site = [code_va + i for i in self.calls(code, code_va, loader)]
        if len(play_site) != 1:
            raise SystemExit('%s: voice loader has %d callers' % (path, len(play_site)))
        p = play_site[0] - code_va
        while not (code[p - 1] == 0xC3 and code[p] == 0x53):   # ret, then the function's push ebx
            p -= 1
        self.play = code_va + p
        # per call site of play_voice: ('id', ID) for mov eax, imm32 before it, ('table', address)
        # for mov eax, [reg*4 + table], else None
        self.direct = []
        for i in self.calls(code, code_va, self.play):
            if code[i - 5] == 0xB8:
                self.direct.append(('id', struct.unpack_from('<I', code, i - 4)[0]))
            elif code[i - 7] == 0x8B and code[i - 6] == 0x04 and code[i - 5] & 0xC7 == 0x85:
                self.direct.append(('table', struct.unpack_from('<I', code, i - 4)[0]))
            else:
                self.direct.append(None)

    def table_ids(self, addr, counts):
        """voice IDs of a table in the data, up to the first entry that isn't one"""
        out = []
        for sva, size, sraw in self.secs:
            if sva <= addr < sva + size:
                p = sraw + addr - sva
                while p + 4 <= sraw + size:
                    v = struct.unpack_from('<I', self.b, p)[0]
                    if (v >> 16) >= len(counts) or (v & 0xFFFF) >= counts[v >> 16]:
                        break
                    out.append(v)
                    p += 4
        return out

    def va(self, raw):
        """virtual address of a file offset"""
        for sva, size, sraw in self.secs:
            if sraw <= raw < sraw + size:
                return sva + raw - sraw
        raise SystemExit('file offset %X is outside the sections' % raw)

    @staticmethod
    def calls(code, code_va, target):
        out = []
        for m in re.finditer(rb'\xe8', code):
            i = m.start()
            if i + 5 <= len(code) and code_va + i + 5 + struct.unpack_from('<i', code, i + 1)[0] == target:
                out.append(i)
        return out


def message_chunks(path):
    b = open(path, 'rb').read()
    n = struct.unpack_from('<I', b, 0)[0] // 4
    offs = struct.unpack_from('<%dI' % n, b, 0)
    out = []
    for i in range(n - 1):
        o, e = offs[i], offs[i + 1]
        if b[o:o + 2] != b'BD' or struct.unpack_from('<H', b, o + 2)[0] not in (0x1F, 0x20):
            continue   # 0x1F: a stage's messages, 0x20: the support car's (SUPPORT.BIN)
        base = o + 8
        k = struct.unpack_from('<H', b, base)[0] // 2
        mo = list(struct.unpack_from('<%dH' % k, b, base)) + [e - base]
        out.append([b[base + mo[j]:base + mo[j + 1]] for j in range(k)])
    return out


def message_voices(msg, counts):
    out = []
    for m in re.finditer(b'\x8b(.)(.)(.)\x00', msg, re.S):
        clip, bank = m.group(1)[0] | m.group(2)[0] << 8, m.group(3)[0]
        if bank < len(counts) and clip < counts[bank]:
            out.append((bank, clip))
    return out


def skeleton(msg):   # control codes of a message, for lining up message lists that differ
    return bytes(x for x in msg if x >= 0x80 and x != 0x82)[:24]


def dat_chunks(b):
    n = struct.unpack_from('<I', b, 0)[0] // 4
    offs = struct.unpack_from('<%dI' % n, b, 0)
    return [b[offs[i]:offs[i + 1]] for i in range(n - 1)]


def dat_with_jp_sounds(us_path, jp_path):
    """the US file with the Japanese file's sound banks ("WB" chunks), or None if they're the same.
    A few banks hold voice samples (title screen, shared bank); the rest are identical."""
    uc, jc = dat_chunks(open(us_path, 'rb').read()), dat_chunks(open(jp_path, 'rb').read())
    jwb = [c for c in jc if c[:2] == b'WB']   # the n-th sound bank of one is the n-th of the other
    if len(jwb) != sum(c[:2] == b'WB' for c in uc):
        return None                           # (other chunks can differ: DEMO_DAT's title logo)
    out, k = [], 0
    for c in uc:
        if c[:2] == b'WB':
            c, k = jwb[k], k + 1
        out.append(c)
    if out == uc:
        return None
    n = len(out) + 1
    offs, pos = [], 4 * n
    for c in out:
        offs.append(pos)
        pos += len(c)
    offs.append(pos)
    return struct.pack('<%dI' % n, *offs) + b''.join(out)


def find_exe(d, names):
    for n in names:
        if os.path.isfile(os.path.join(d, n)):
            return os.path.join(d, n)
    raise SystemExit('%s: none of %s' % (d, ', '.join(names)))


class Mapping:
    """What each US voice clip becomes, from the two copies' scripts and code.
    actions[(bank, clip)]: a tuple of Japanese (bank, clip), empty for an empty entry; no key if
    nothing plays the clip. where[(bank, clip)]: the places that decided it, (DAT file, message
    number) or ('code', call site number)."""

    def __init__(self, us_dir, jp_dir):
        self.us_exe = us_exe = Exe(find_exe(us_dir, ['MEGAMAN.EXE', 'MEGAMAN_NATIVE.EXE', 'MEGAMAN.EXE.orig']))
        self.jp_exe = jp_exe = Exe(find_exe(jp_dir, ['rockman.exe', 'ROCKMAN.EXE']))
        self.us_banks = [wbk_read(os.path.join(us_dir, 'SPH', n)) for n in us_exe.banks]
        self.jp_banks = [wbk_read(os.path.join(jp_dir, 'SPH', n)) for n in jp_exe.banks]
        uc, jc = [len(x) for x in self.us_banks], [len(x) for x in self.jp_banks]
        self.nu, self.nj = nu, nj = len(uc) - 2, len(jc) - 2   # the last two banks: cutscene tracks
        if uc[nu:] != jc[nj:]:
            raise SystemExit('the cutscene tracks have different clip counts: %s / %s' % (uc[nu:], jc[nj:]))
        votes = collections.defaultdict(collections.Counter)
        where = collections.defaultdict(list)
        self.stats = stats = collections.Counter()
        self.jp_only = []   # (DAT file, message number, Japanese clips): voiced only in Japanese
        self.groups = {}    # first US clip of a joined line -> all the US clips of its message
        for name in sorted(os.listdir(os.path.join(us_dir, 'DAT'))):
            jp_path = os.path.join(jp_dir, 'DAT', name)
            if not name.upper().endswith('.BIN') or not os.path.isfile(jp_path):
                continue
            for um, jm in zip(message_chunks(os.path.join(us_dir, 'DAT', name)), message_chunks(jp_path)):
                if len(um) == len(jm):
                    pairs = list(enumerate(zip(um, jm)))
                else:
                    sm = difflib.SequenceMatcher(None, [skeleton(x) for x in um], [skeleton(x) for x in jm],
                                                 autojunk=False)
                    pairs = []
                    for tag, a0, a1, b0, b1 in sm.get_opcodes():
                        if tag == 'equal' or (tag == 'replace' and a1 - a0 == b1 - b0):
                            pairs += list(zip(range(a0, a1), zip(um[a0:a1], jm[b0:b1])))
                for k, (a, b) in pairs:
                    va, vb = message_voices(a, uc), message_voices(b, jc)
                    if not va:
                        if vb:
                            stats['Japanese-only voiced messages'] += 1
                            self.jp_only.append((name, k, vb))
                        continue
                    if len(va) == len(vb):
                        stats['messages paired line by line'] += 1
                        for x, y in zip(va, vb):
                            votes[x][(y,)] += 1
                    else:
                        stats['messages joined' if vb else 'US-only voiced messages'] += 1
                        if vb:
                            self.groups[va[0]] = tuple(va)
                        votes[va[0]][tuple(vb)] += 1
                        for x in va[1:]:
                            votes[x][()] += 1
                    for x in va:
                        where[x].append((name, k))
        if len(us_exe.direct) == len(jp_exe.direct):
            for site, (x, y) in enumerate(zip(us_exe.direct, jp_exe.direct)):
                if x is None or y is None or x[0] != y[0]:
                    continue
                if x[0] == 'id':
                    xs, ys = [x[1]], [y[1]]
                else:
                    xs, ys = us_exe.table_ids(x[1], uc), jp_exe.table_ids(y[1], jc)
                stats['voices the code plays directly'] += min(len(xs), len(ys))
                for a, c in zip(xs, ys):
                    votes[(a >> 16, a & 0xFFFF)][((c >> 16, c & 0xFFFF),)] += 1
                    where[(a >> 16, a & 0xFFFF)].append(('code', site))
        else:
            print('note: the code plays voices from %d places in one version, %d in the other; skipped'
                  % (len(us_exe.direct), len(jp_exe.direct)))
        # most votes; ties: a single clip before a joined line before an empty entry
        self.actions = {k: max(c.items(), key=lambda kv: (kv[1], len(kv[0]) == 1, len(kv[0]) > 0))[0]
                        for k, c in votes.items()}
        self.votes, self.where = votes, where

    def clip_wav(self, bank, clip):
        """the WAV that SPJ holds for this US clip (b'' for an empty entry)"""
        action = self.actions.get((bank, clip))
        if action is None:                  # no script plays it: keep the US clip
            return self.us_banks[bank][clip]
        if not action:
            return b''
        parts = [wav_parse(self.jp_banks[b][c]) for b, c in action]
        if any(p[:3] != parts[0][:3] for p in parts) or parts[0][2] != 8:
            raise SystemExit('unexpected clip format in %s' % (action,))
        gap = b'\x80' * int(GAP * RATE)
        pcm = gap.join(p[3] for p in parts) if parts[0][0] == 1 else parts[0][3]
        return wav_make(parts[0][0], parts[0][1], parts[0][2], pcm)


def main(us_dir, jp_dir):
    m = Mapping(us_dir, jp_dir)
    out_dir = os.path.join(us_dir, 'SPJ')
    os.makedirs(out_dir, exist_ok=True)
    for bank in range(m.nu):
        wbk_write(os.path.join(out_dir, m.us_exe.banks[bank]),
                  [m.clip_wav(bank, clip) for clip in range(len(m.us_banks[bank]))])
    for k in range(2):   # cutscene tracks: the Japanese bank as it is
        with open(os.path.join(out_dir, m.us_exe.banks[m.nu + k]), 'wb') as f, \
                open(os.path.join(jp_dir, 'SPH', m.jp_exe.banks[m.nj + k]), 'rb') as g:
            f.write(g.read())

    dat_dir = os.path.join(us_dir, 'DAJ')
    os.makedirs(dat_dir, exist_ok=True)
    sound_files = []
    for name in sorted(os.listdir(os.path.join(us_dir, 'DAT'))):
        jp_path = os.path.join(jp_dir, 'DAT', name)
        if not name.upper().endswith('.BIN') or not os.path.isfile(jp_path):
            continue
        data = dat_with_jp_sounds(os.path.join(us_dir, 'DAT', name), jp_path)
        target = os.path.join(dat_dir, name)
        if data is None:
            if os.path.exists(target):
                os.remove(target)
            continue
        with open(target, 'wb') as f:
            f.write(data)
        sound_files.append(name)

    clips = [(b, c) for b in range(m.nu) for c in range(len(m.us_banks[b]))]
    kept = sum(1 for x in clips if x not in m.actions)
    empty = sum(1 for x in clips if x in m.actions and not m.actions[x])
    joined = sum(1 for x in clips if len(m.actions.get(x, ())) > 1)
    print('in-game voice clips: %d' % len(clips))
    print('  Japanese: %d (%d of them several Japanese clips joined)' % (len(clips) - kept - empty, joined))
    print('  empty (the Japanese line plays on through them): %d' % empty)
    print('  kept in English (no script plays them): %d' % kept)
    for k, v in sorted(m.stats.items()):
        print('  %s: %d' % (k, v))
    print('cutscene tracks: %s, %s' % tuple(m.us_exe.banks[m.nu:]))
    print('sound banks with Japanese samples: %s' % (', '.join(sound_files) or 'none'))
    print('written to %s and %s' % (out_dir, dat_dir))


def emit_c(us_dir, jp_dir, path):
    """native/src/voice_map.c: the mapping as tables, for builders without Python (voice_build.c,
    on Android). It holds bank file names, clip counts and voice IDs only: nothing of the game."""
    m = Mapping(us_dir, jp_dir)
    first, count, src = [], [], []
    for bank in range(m.nu):
        for clip in range(len(m.us_banks[bank])):
            action = m.actions.get((bank, clip))
            first.append(len(src))
            if action is None:
                count.append(255)    # VM_KEEP: the US clip
                continue
            count.append(len(action))
            src += [b << 16 | c for b, c in action]

    def table(values, fmt, per):
        rows = [', '.join(fmt % v for v in values[i:i + per]) for i in range(0, len(values), per)]
        return ',\n'.join('    ' + r for r in rows)

    us_names, jp_names = m.us_exe.banks, m.jp_exe.banks
    out = [
        '/* GENERATED by tools/voice/build_jp_voices.py --emit-c US_GAME_DIR JP_GAME_DIR: do not edit.',
        ' * The Japanese voices\' mapping (see that script) as tables for voice_build.c: bank file names,',
        ' * clip counts and voice IDs (bank << 16 | clip), derived from the two versions\' scripts. */',
        '#include "voice_build.h"',
        '',
        'const char *const VM_US_BANK[VM_US_BANKS] = {',
        table(us_names, '"%s"', 8), '};',
        'const uint16_t VM_US_CLIPS[VM_US_BANKS] = {',
        table([len(x) for x in m.us_banks], '%d', 16), '};',
        'const char *const VM_JP_BANK[VM_JP_BANKS] = {',
        table(jp_names, '"%s"', 8), '};',
        'const uint16_t VM_JP_CLIPS[VM_JP_BANKS] = {',
        table([len(x) for x in m.jp_banks], '%d', 16), '};',
        '/* per in-game US clip, bank by bank: where its Japanese clips start in VM_SRC and how many',
        '   (0: an empty entry, VM_KEEP: the US clip stays) */',
        'const uint16_t VM_FIRST[VM_CLIPS] = {',
        table(first, '%d', 16), '};',
        'const uint8_t VM_COUNT[VM_CLIPS] = {',
        table(count, '%d', 24), '};',
        'const uint32_t VM_SRC[VM_SRCS] = {',
        table(src, '0x%06X', 10), '};',
        '',
    ]
    with open(path, 'w', encoding='ascii', newline='\n') as f:
        f.write('\n'.join(out))
    with open(os.path.splitext(path)[0] + '_sizes.h', 'w', encoding='ascii', newline='\n') as f:
        f.write('/* GENERATED by tools/voice/build_jp_voices.py --emit-c (with voice_map.c) */\n'
                '#define VM_US_BANKS %d\n#define VM_JP_BANKS %d\n#define VM_CLIPS %d\n#define VM_SRCS %d\n'
                % (len(us_names), len(jp_names), len(first), len(src)))
    print('%s: %d US banks, %d Japanese banks, %d clips, %d Japanese clips used'
          % (path, len(us_names), len(jp_names), len(first), len(src)))


if __name__ == '__main__':
    if len(sys.argv) == 4 and sys.argv[1] == '--emit-c':
        emit_c(sys.argv[2], sys.argv[3], os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..',
                                                       'native', 'src', 'voice_map.c'))
        raise SystemExit
    if len(sys.argv) != 3:
        raise SystemExit(__doc__.split('\n\n')[1])
    main(sys.argv[1], sys.argv[2])
