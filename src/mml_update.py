"""Update check and download for the Windows launcher.

Looks up the newest release on GitHub, and downloads its MML-Setup.exe into the temp folder,
checked against the SHA-256 GitHub lists for the file. Plain functions with no GUI, so they can be
tested directly (python mml_update.py [version] prints what a check finds). The launcher
(mml_gui.py) runs them on a thread and, once the download is checked, starts the installer.
"""
import hashlib
import json
import os
import re
import sys
import tempfile
import urllib.request

REPO = "Drakfix/MML_Modernization-Project"
API = f"https://api.github.com/repos/{REPO}/releases/latest"
ASSET = "MML-Setup.exe"


def numbers(version):
    """'v0.6.5-beta' -> (0, 6, 5)"""
    n = [int(x) for x in re.findall(r"\d+", version.lstrip("vV"))][:3]
    return tuple(n + [0] * (3 - len(n)))


def newer(latest, mine):
    return numbers(latest) > numbers(mine)


def _open(url, timeout=15):
    req = urllib.request.Request(url, headers={"User-Agent": "MML-Modernization-Updater",
                                               "Accept": "application/vnd.github+json, application/octet-stream"})
    return urllib.request.urlopen(req, timeout=timeout)


def check(mine):
    """The newest release if it is newer than mine, as {version, url, size, digest}; None if mine is
    current. Raises when GitHub can't be reached or has no installer in the release."""
    with _open(API) as r:
        rel = json.load(r)
    tag = rel["tag_name"]
    if not newer(tag, mine):
        return None
    for a in rel.get("assets", []):
        if a["name"].lower() == ASSET.lower():
            return {"version": tag.lstrip("vV"), "url": a["browser_download_url"], "size": a.get("size", 0),
                    "digest": a.get("digest") or ""}
    raise RuntimeError(f"{tag} has no {ASSET} yet")


def download(rel, progress=None):
    """The release's installer into a temp folder of its own; returns its path. progress(done, total)
    is called as it arrives. Raises if the file isn't the one GitHub lists."""
    folder = os.path.join(tempfile.gettempdir(), "MML-update")
    os.makedirs(folder, exist_ok=True)
    dest, part = os.path.join(folder, ASSET), os.path.join(folder, ASSET + ".part")
    h = hashlib.sha256()
    done = 0
    with _open(rel["url"], timeout=30) as r, open(part, "wb") as f:
        total = int(r.headers.get("Content-Length") or rel.get("size") or 0)
        while True:
            chunk = r.read(1 << 16)
            if not chunk:
                break
            f.write(chunk)
            h.update(chunk)
            done += len(chunk)
            if progress:
                progress(done, total)
    if rel.get("size") and done != rel["size"]:
        os.remove(part)
        raise RuntimeError("the download is incomplete")
    digest = rel.get("digest", "")
    if digest.startswith("sha256:") and digest[7:].lower() != h.hexdigest():
        os.remove(part)
        raise RuntimeError("the download doesn't match its checksum")
    if os.path.exists(dest):
        os.remove(dest)
    os.replace(part, dest)
    return dest


if __name__ == "__main__":
    mine = sys.argv[1] if len(sys.argv) > 1 else "0.0.0"
    found = check(mine)
    print("up to date" if not found else found)
