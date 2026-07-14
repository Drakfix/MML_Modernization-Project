"""Download the exact upstream license texts for the third-party components this mod
redistributes, into RE-notes/licenses/, and write THIRD-PARTY-NOTICES.txt.

We ship these binaries UNMODIFIED:
  dinput.dll        = Xidi           (BSD-3-Clause)   github.com/samuelgr/Xidi
  ddraw.dll         = DDrawCompat    (0BSD)           github.com/narzoul/DDrawCompat
  dsound.dll        = DSOAL          (LGPL-2.1)       github.com/kcat/dsoal
  dsoal-aldrv.dll   = OpenAL Soft    (LGPL-2.1)       github.com/kcat/openal-soft

Run once (or when a component is updated): python fetch_licenses.py
"""
import os
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(os.path.dirname(HERE), "licenses")   # RE-notes/licenses
os.makedirs(OUT, exist_ok=True)

SOURCES = {
    "Xidi-LICENSE.txt": "https://raw.githubusercontent.com/samuelgr/Xidi/master/LICENSE",
    "DDrawCompat-LICENSE.txt": "https://raw.githubusercontent.com/narzoul/DDrawCompat/master/LICENSE.txt",
    "DSOAL-LICENSE.txt": "https://raw.githubusercontent.com/kcat/dsoal/master/LICENSE",
    "OpenAL-Soft-COPYING.txt": "https://raw.githubusercontent.com/kcat/openal-soft/master/COPYING",
}

for fn, url in SOURCES.items():
    req = urllib.request.Request(url, headers={"User-Agent": "Mozilla/5.0"})
    data = urllib.request.urlopen(req, timeout=30).read()
    with open(os.path.join(OUT, fn), "wb") as f:
        f.write(data)
    print(f"  {fn:28s} {len(data):>7,} bytes  <- {url}")

NOTICES = """MEGA MAN LEGENDS - MODERNIZATION PROJECT
Third-Party Notices and Licenses
================================================================================

The Modernization Project itself (the launcher, the widescreen / camera / controls
/ HUD / audio-timing work, and the exe byte-patch) is this project's own work and
contains NO Capcom code or data. It requires your own copy of the Mega Man Legends
PC game.

To run the game on modern Windows it also installs the following THIRD-PARTY,
OPEN-SOURCE components, redistributed UNMODIFIED from their upstream releases. Each
is used under its own license (full texts in this "licenses" folder). You may
replace any of these DLLs with your own build from the linked source.

--------------------------------------------------------------------------------
Xidi                     file: dinput.dll
  XInput-to-DirectInput translation (controller support).
  (c) 2016-2026 Samuel Grossman.  License: BSD-3-Clause  ->  licenses/Xidi-LICENSE.txt
  Source: https://github.com/samuelgr/Xidi

DDrawCompat              file: ddraw.dll
  DirectDraw/Direct3D compatibility layer for modern Windows.
  (c) narzoul.  License: BSD Zero Clause (0BSD)  ->  licenses/DDrawCompat-LICENSE.txt
  Source: https://github.com/narzoul/DDrawCompat

DSOAL                    file: dsound.dll
  DirectSound3D-over-OpenAL wrapper (cutscene/positional audio).
  License: LGPL-2.1  ->  licenses/DSOAL-LICENSE.txt
  Source: https://github.com/kcat/dsoal

OpenAL Soft              file: dsoal-aldrv.dll
  OpenAL implementation used by DSOAL (an unmodified OpenAL Soft binary).
  License: LGPL-2.1  ->  licenses/OpenAL-Soft-COPYING.txt
  Source: https://github.com/kcat/openal-soft  /  https://openal-soft.org
--------------------------------------------------------------------------------

LGPL-2.1 note: DSOAL (dsound.dll) and OpenAL Soft (dsoal-aldrv.dll) are covered by
the GNU Lesser General Public License, version 2.1. They are redistributed here
unmodified; their complete corresponding source is available at the URLs above,
and because they are dynamically-loaded DLLs you may substitute your own build.

Credits / acknowledgements:
  * The controller setup is based on the Xidi configuration from the community
    "Mega Man Legends PC Fix" repack by KuroRaiden (@NC_KuroRaiden). Thank you.
  * A higher-quality music option for this game exists separately as "Mega Man
    Legends - Unfinished Business" (not redistributed by this project).
  * dxwrapper (github.com/elishacloud/dxwrapper) is NOT used or shipped by this
    project (our stack uses DDrawCompat + Xidi + DSOAL instead).
"""

with open(os.path.join(OUT, "THIRD-PARTY-NOTICES.txt"), "w", encoding="utf-8") as f:
    f.write(NOTICES)
print("  THIRD-PARTY-NOTICES.txt written")
print("licenses assembled in:", OUT)
