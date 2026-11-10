"""Checks the Japanese voice files build_jp_voices.py wrote, without playing the game.

    python tools/voice/check_jp_voices.py US_GAME_DIR JP_GAME_DIR [LISTEN_DIR]

1. Files: every SPJ bank has the US bank's clip count, a valid offset table and valid WAV clips in
   the US clip's format; every DAJ file is the US file with only its sound banks changed.
2. Pairings: a US line and the Japanese line it became should be of similar length (for a
   joined line: the US clips of the message together). Pairs far apart are listed, and so are
   clips the scripts disagree about and voices the code plays that became empty entries.
3. With LISTEN_DIR: the US and Japanese audio of every listed pair as WAV files, to check by ear.
Exit status 1 if a file check fails."""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import build_jp_voices as B   # noqa: E402

RATIO = 1.8        # Japanese / US length beyond this (either way) is listed
MIN_DIFF = 1.5     # ... when they also differ by at least this many seconds


def seconds(w):
    ch, rate, bits, pcm = B.wav_parse(w)
    return len(pcm) / (rate * ch * bits // 8)


def check_files(m, us_dir):
    errors = []
    spj = os.path.join(us_dir, 'SPJ')
    for bank, name in enumerate(m.us_exe.banks):
        path = os.path.join(spj, name)
        if not os.path.isfile(path):
            errors.append('%s missing' % path)
            continue
        try:
            clips = B.wbk_read(path)
        except SystemExit as e:
            errors.append(str(e))
            continue
        us = m.us_banks[bank]
        if len(clips) != len(us):
            errors.append('%s: %d clips, the US bank has %d' % (name, len(clips), len(us)))
            continue
        for i, (c, u) in enumerate(zip(clips, us)):
            if not c:
                if bank >= m.nu:
                    errors.append('%s clip %d: empty cutscene track' % (name, i))
                continue
            try:
                fc, fu = B.wav_parse(c), B.wav_parse(u)
            except ValueError as e:
                errors.append('%s clip %d: %s' % (name, i, e))
                continue
            if fc[:3] != fu[:3]:
                errors.append('%s clip %d: format %s, the US clip has %s' % (name, i, fc[:3], fu[:3]))
            if not fc[3]:
                errors.append('%s clip %d: no audio' % (name, i))
    daj = os.path.join(us_dir, 'DAJ')
    for name in sorted(os.listdir(daj)) if os.path.isdir(daj) else []:
        a = B.dat_chunks(open(os.path.join(us_dir, 'DAT', name), 'rb').read())
        try:
            b = B.dat_chunks(open(os.path.join(daj, name), 'rb').read())
        except Exception as e:   # a broken offset table
            errors.append('DAJ\\%s: %s' % (name, e))
            continue
        if len(a) != len(b):
            errors.append('DAJ\\%s: %d chunks, the US file has %d' % (name, len(b), len(a)))
            continue
        for i, (x, y) in enumerate(zip(a, b)):
            if x[:2] != y[:2]:
                errors.append('DAJ\\%s chunk %d: %s, the US file has %s' % (name, i, y[:2], x[:2]))
            elif x[:2] != b'WB' and x != y:
                errors.append('DAJ\\%s chunk %d (%s) differs from the US file' % (name, i, x[:2]))
            elif x[:2] == b'WB' and x[:8] != y[:8]:
                errors.append('DAJ\\%s chunk %d: sound bank header differs' % (name, i))
    return errors


def pair_report(m):
    """(US clips of the line, Japanese clips, US seconds, Japanese seconds) for the pairs whose
    lengths are far apart"""
    flagged = []
    for bank in range(m.nu):
        n = len(m.us_banks[bank])
        for clip in range(n):
            action = m.actions.get((bank, clip))
            if not action:
                continue
            # a joined line: all the US clips of its message
            group = list(m.groups.get((bank, clip), ((bank, clip),))) if len(action) > 1 else [(bank, clip)]
            us_s = sum(seconds(m.us_banks[b][c]) for b, c in group)
            jp_s = seconds(m.clip_wav(bank, clip))
            r = max(us_s, jp_s) / max(min(us_s, jp_s), 0.05)
            if r > RATIO and abs(us_s - jp_s) >= MIN_DIFF:
                flagged.append((group, list(action), us_s, jp_s))
    return flagged


def main(us_dir, jp_dir, listen_dir=None):
    m = B.Mapping(us_dir, jp_dir)
    ub, jb = m.us_exe.banks, m.jp_exe.banks
    name = lambda bc, names=ub: '%s:%d' % (names[bc[0]][:-4], bc[1])   # noqa: E731

    errors = check_files(m, us_dir)
    print('1. files: %s' % ('OK' if not errors else '%d problems' % len(errors)))
    for e in errors:
        print('   ' + e)

    flagged = pair_report(m)
    total = sum(1 for k, v in m.actions.items() if v)
    print('2. pairings: %d of %d Japanese lines differ a lot in length from the US line' % (len(flagged), total))
    for us, jp, us_s, jp_s in sorted(flagged, key=lambda f: -abs(f[2] - f[3])):
        places = sorted(set('%s#%s' % p for p in m.where[us[0]]))
        print('   US %-22s %5.1fs  ->  JP %-28s %5.1fs   %s' % (
            ' + '.join(name(x) for x in us), us_s, ' + '.join(name(x, jb) for x in jp), jp_s,
            ', '.join(places[:3]) + (' ...' if len(places) > 3 else '')))
    split = [(k, c) for k, c in m.votes.items() if len(c) > 1]
    print('   scripts disagree about %d clips (the most frequent pairing is used):' % len(split))
    for k, c in sorted(split):
        print('     US %-12s %s' % (name(k), ', '.join('%s x%d' % ('+'.join(name(x, jb) for x in a) or 'empty', n)
                                                  for a, n in c.most_common())))
    code_empty = sorted(k for k, w in m.where.items() if any(p[0] == 'code' for p in w) and m.actions.get(k) == ())
    print('   voices the code plays that became empty entries: %d %s' % (
        len(code_empty), ' '.join(name(k) for k in code_empty)))
    print('   Japanese-only voiced messages (not played): %d' % len(m.jp_only))

    if listen_dir:
        os.makedirs(listen_dir, exist_ok=True)
        with open(os.path.join(listen_dir, 'index.txt'), 'w', encoding='utf-8') as idx:
            for n, (us, jp, us_s, jp_s) in enumerate(sorted(flagged, key=lambda f: -abs(f[2] - f[3])), 1):
                base = '%02d_%s' % (n, name(us[0]).replace(':', '_'))
                parts = [B.wav_parse(m.us_banks[b][c]) for b, c in us]
                pcm = (b'\x80' * int(B.GAP * B.RATE)).join(p[3] for p in parts)
                with open(os.path.join(listen_dir, base + '_US.wav'), 'wb') as f:
                    f.write(B.wav_make(parts[0][0], parts[0][1], parts[0][2], pcm))
                with open(os.path.join(listen_dir, base + '_JP.wav'), 'wb') as f:
                    f.write(m.clip_wav(*us[0]))
                idx.write('%s  US %s (%.1fs)  JP %s (%.1fs)  in %s\n' % (
                    base, ' + '.join(name(x) for x in us), us_s, ' + '.join(name(x, jb) for x in jp), jp_s,
                    ', '.join(sorted(set('%s#%s' % p for p in m.where[us[0]])))))
        print('3. listening pack: %d pairs in %s' % (len(flagged), listen_dir))
    return 1 if errors else 0


if __name__ == '__main__':
    if len(sys.argv) not in (3, 4):
        raise SystemExit(__doc__.split('\n\n')[1])
    sys.exit(main(*sys.argv[1:]))
