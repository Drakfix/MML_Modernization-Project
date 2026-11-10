"""English subtitles for the cutscenes, from the Rockman DASH PSP English project's script.

    python tools/voice/build_subtitles.py SUBTITLES_CSV US_GAME_DIR
    python tools/voice/build_subtitles.py --download US_GAME_DIR

SUBTITLES_CSV is script/subtitles_en.csv of https://github.com/JamesAZ0/RockmanDASH-PSP-English-Patcher
(columns scene, n, start_s, end_s, text, en_final, status, time_override): the lines of the US
voice acting, transcribed and timed to it by that project. --download fetches it from that
repository at the commit this script was written for (SOURCE_COMMIT) and checks it
(SOURCE_SHA256); the setup program does the same. Writes US_GAME_DIR/Modernization/subtitles_en.txt,
one line per subtitle:

    <US voice bank> <clip> <start seconds> <end seconds> <text>

keyed by the clip the US game plays (mml.dll shows a clip's lines while it plays, timed from its
start, with the English voices or the Japanese ones). Scenes: A n is clip n of the first cutscene
track (3700E), B n is clip n + 2 of the second (3701E: the PC's has two more clips at its start
than the PSP's), C 0-4 are the news reports the game's code plays (NEWS: the US clips; with the
Japanese voices, tools/voice/build_jp_voices.py puts the Japanese reports, Japanese bank 18M06
clips 46, 18, 19, 20 and 21, in those same clips). The script is Capcom's game text as that project
transcribed it: it is read from the player's download of the file, never stored in this repository.

Credit: the subtitles are the work of the Rockman DASH PSP English project (JamesAZ0 and its
contributors), who transcribed the US voice acting (with Whisper, checked against Estil's GameFAQs
script) and timed the lines to it. That work is under the MIT licence (Copyright (c) 2026 The
Rockman DASH PSP English project contributors); the lines belong to Capcom. The file this writes
says so too."""
import csv
import hashlib
import io
import os
import sys
import urllib.request

SOURCE_REPO = 'https://github.com/JamesAZ0/RockmanDASH-PSP-English-Patcher'
SOURCE_COMMIT = '7d49ceb334aae2b2381bdde3d03448cbd90320da'
SOURCE_PATH = 'script/subtitles_en.csv'
SOURCE_URL = ('https://raw.githubusercontent.com/JamesAZ0/RockmanDASH-PSP-English-Patcher/%s/%s'
              % (SOURCE_COMMIT, SOURCE_PATH))
SOURCE_SHA256 = '0822f5228b3248dd62a358611abe1a7a6182c6a15de9e210896f6b753579697c'
CREDIT = (
    'English subtitles for the cutscenes: the work of the Rockman DASH PSP English project',
    '(JamesAZ0 and its contributors), %s,' % SOURCE_REPO,
    'from its %s at commit %s, downloaded from there during setup.' % (SOURCE_PATH, SOURCE_COMMIT[:7]),
    'That project transcribed the US voice acting and timed the lines to it, under the MIT licence:',
    'Copyright (c) 2026 The Rockman DASH PSP English project contributors.',
    'The lines belong to Capcom.',
)

TRACKS = ('3700E', '3701E')   # the US cutscene tracks (A, B scenes)
NEWS = {0: ('1821E', 10), 1: ('1821E', 4), 2: ('1821E', 5), 3: ('1821E', 7), 4: ('1821E', 6)}   # C scenes
ASCII = {'‘': "'", '’': "'", '“': '"', '”': '"', '…': '...', '–': '-',
         '—': '--', ' ': ' ', 'é': 'e', 'è': 'e', 'à': 'a', '™': ''}


def ascii_text(s):
    s = ''.join(ASCII.get(c, c) for c in s.strip())
    return ''.join(c if 32 <= ord(c) < 127 else '?' for c in ' '.join(s.split()))


def download():
    """the project's script file, as text, checked against SOURCE_SHA256"""
    req = urllib.request.Request(SOURCE_URL, headers={'User-Agent': 'mml-modernization-setup'})
    with urllib.request.urlopen(req, timeout=30) as r:
        data = r.read()
    if hashlib.sha256(data).hexdigest() != SOURCE_SHA256:
        raise SystemExit('the file from %s is not the expected one, so nothing was written '
                         '(the repository may have changed).' % SOURCE_REPO)
    return data.decode('utf-8')


def main(csv_path, us_dir):
    """csv_path: the script file, or None to download it; returns the subtitle file written"""
    out, skipped = [], set()
    text_in = download() if csv_path is None else open(csv_path, encoding='utf-8').read()
    for row in csv.DictReader(io.StringIO(text_in)):
        if row['status'].strip() == 'drop':
            continue
        scene, n = row['scene'].strip(), int(row['scene'].strip()[1:])
        if scene[0] == 'A':
            bank, clip = TRACKS[0], n
        elif scene[0] == 'B':
            bank, clip = TRACKS[1], n + 2
        elif scene[0] == 'C' and n in NEWS:
            bank, clip = NEWS[n]
        else:
            skipped.add(scene)
            continue
        start, end = float(row['start_s']), float(row['end_s'])
        if row.get('time_override', '').strip():
            start, end = (float(x) for x in row['time_override'].split('-'))
        text = ascii_text(row['en_final'] or row['text'])
        if text:
            out.append((bank, clip, start, end, text))
    path = os.path.join(us_dir, 'Modernization', 'subtitles_en.txt')
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, 'w', encoding='ascii', newline='\n') as f:
        for line in CREDIT:
            f.write('# %s\n' % line)
        f.write('# bank clip start end text: built by tools/voice/build_subtitles.py\n')
        for bank, clip, start, end, text in out:
            f.write('%s %d %.2f %.2f %s\n' % (bank, clip, start, end, text))
    print('%d subtitles for %d clips written to %s' % (len(out), len({(b, c) for b, c, *_ in out}), path))
    if skipped:
        print('scenes without a clip: %s' % ' '.join(sorted(skipped)))
    return path


if __name__ == '__main__':
    if len(sys.argv) != 3:
        raise SystemExit(__doc__.split('\n\n')[1])
    main(None if sys.argv[1] == '--download' else sys.argv[1], sys.argv[2])
