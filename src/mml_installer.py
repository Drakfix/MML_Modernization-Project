"""Graphical setup wizard.

A self-contained installer (welcome -> choose the install folder -> install -> done). Built to
a single executable with PyInstaller that bundles everything needed, so the end user does
not have to install anything separately.

Setup installs the app (do_app_install): the start menu, both game engines, the runtime and the
intro, into a folder the player chooses. The games are added in the start menu (its APP page):
the player picks their Mega Man Legends and/or Rockman DASH folder and it is copied into the app's
folder, patched there. A game folder an earlier Setup (0.5 to 0.7) patched in place is still
updated where it is (do_install). Both are plain functions, independent of the GUI, so they can
be tested directly. Patch data comes from patch_manifest.py.
"""
import ctypes
import hashlib
import os
import shutil
import subprocess
import sys

import patch_manifest as M

APP_NAME = "Mega Man Legends - Modernization Project"
VERSION = "0.8.1-beta"


def _res_dir():
    """Where the bundled payload lives: sys._MEIPASS when frozen, else dist/release (make_patcher.py)."""
    if getattr(sys, "frozen", False):
        return sys._MEIPASS
    return os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "dist", "release")


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


class Edition:
    """one release of the game the patch knows: its exe and the patch data for it"""
    def __init__(self, exe, name, vanilla, patched, previous, size, patches, tail):
        self.exe, self.name, self.vanilla, self.patched = exe, name, vanilla, patched
        self.previous, self.size, self.patches, self.tail = previous, size, patches, tail


def editions():
    out = [Edition("MEGAMAN.EXE", "US", M.VANILLA_SHA256, M.PATCHED_SHA256, M.PREVIOUS_PATCHED_SHA256,
                   M.EXE_SIZE, M.PATCHES, M.EXE_TAIL)]
    if getattr(M, "JP_EXE", None):   # the Japanese version (Rockman DASH)
        out.append(Edition(M.JP_EXE, "Japanese", M.JP_VANILLA_SHA256, M.JP_PATCHED_SHA256, [],
                           M.JP_EXE_SIZE, M.JP_PATCHES, M.JP_EXE_TAIL))
    return out


GAME_EXES = ("MEGAMAN.EXE", "ROCKMAN.EXE")   # US, Japanese (Rockman DASH)


def game_exe(path):
    """the game's exe name in path (the US one first), or None"""
    for exe in GAME_EXES:
        if path and os.path.isfile(os.path.join(path, exe)):
            return exe
    return None


def is_game_folder(path):
    return game_exe(path) is not None


def detect_game_folder(exe=None):
    """Best-effort auto-detect of a game install (exe: only a folder with that game exe)."""
    cands = []
    for root in [os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)"),
                 os.environ.get("ProgramFiles", r"C:\Program Files"),
                 r"C:\Games", r"C:\GOG Games", r"C:\\", os.path.expanduser("~\\Desktop")]:
        if root and os.path.isdir(root):
            cands.append(root)
    names = ("megaman legends", "mega man legends", "rockman dash", "mml")
    for base in cands:
        try:
            for entry in os.listdir(base):
                full = os.path.join(base, entry)
                if os.path.isdir(full) and any(n in entry.lower() for n in names):
                    if is_game_folder(full) and (exe is None or game_exe(full) == exe):
                        return full
        except OSError:
            continue
    return ""


# ---- English subtitles for the cutscenes, Japanese voices (both optional, for the US version) ----
# The subtitles are the Rockman DASH PSP English project's work (the English voice acting,
# transcribed and timed): tools/voice/build_subtitles.py downloads that project's file from its
# GitHub repository during setup (never shipped with the patch) and credits it. They show with
# either voices. The Japanese voices are built from the player's own Rockman DASH PC copy by
# tools/voice/build_jp_voices.py (bundled): nothing of either game is in the patch.
_VOICE_TOOLS = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "tools", "voice")
if not getattr(sys, "frozen", False) and _VOICE_TOOLS not in sys.path:
    sys.path.insert(0, _VOICE_TOOLS)
import build_jp_voices   # noqa: E402
import build_subtitles   # noqa: E402

SUBS_PROJECT = "Rockman DASH PSP English project"
SUBS_REPO = build_subtitles.SOURCE_REPO


def is_jp_folder(path):
    """a Rockman DASH (Japanese PC) folder with what the voices are built from"""
    return bool(path) and game_exe(path) == "ROCKMAN.EXE" and all(
        os.path.isdir(os.path.join(path, d)) for d in ("SPH", "DAT"))


def detect_jp_folder():
    for root in (os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)"),
                 os.environ.get("ProgramFiles", r"C:\Program Files")):
        d = os.path.join(root or "", "Capcom", "Rockman Dash")
        if is_jp_folder(d):
            return d
    d = detect_game_folder("ROCKMAN.EXE")
    return d if is_jp_folder(d) else ""


def is_writable(path):
    """Setup can change files in path (not so in Program Files without administrator rights)"""
    test = os.path.join(path, ".mml-setup-write-test")
    try:
        with open(test, "w") as f:
            f.write("ok")
        os.remove(test)
        return True
    except OSError:
        return False


NOT_WRITABLE_APP = ("Windows doesn't let Setup write to this folder (Program Files is protected), and the\n"
                    "game couldn't save its settings there either. Pick another folder, for example in\n"
                    "C:\\Games.")
NOT_WRITABLE = ("Windows doesn't let Setup change this folder (Program Files is protected), and the\n"
                "game couldn't save its settings there either. Copy the game folder somewhere else,\n"
                "for example C:\\Games, and pick that copy.")


def set_ini(path, section, values):
    """[section] key=value in an ini file, everything else in it kept as it is"""
    try:
        lines = open(path, encoding="utf-8", errors="replace").read().splitlines()
    except OSError:
        lines = []
    todo, out, cur, done_sec = dict(values), [], None, False

    def rest():
        for k, v in todo.items():
            out.append(f"{k}={v}")
        todo.clear()

    for line in lines:
        st = line.strip()
        if st.startswith("[") and st.endswith("]"):
            if cur == section.lower():
                while out and not out[-1].strip():
                    out.pop()
                rest()
                out.append("")
                done_sec = True
            cur = st[1:-1].strip().lower()
        elif cur == section.lower() and "=" in st and not st.startswith(";"):
            key = st.split("=", 1)[0].strip()
            hit = next((k for k in todo if k.lower() == key.lower()), None)
            if hit is not None:
                out.append(f"{key}={todo.pop(hit)}")
                continue
        out.append(line)
    if cur == section.lower():
        rest()
    elif not done_sec and todo:
        out.extend(["", f"[{section}]"])
        rest()
    with open(path, "w", encoding="utf-8") as f:
        f.write("\n".join(out).rstrip("\n") + "\n")


def install_subtitles(game_dir, log):
    """The English subtitles for the cutscenes; returns a line for the finish page."""
    import contextlib
    import io
    log(f"Downloading the English subtitles from the {SUBS_PROJECT}...", 0.92)
    try:
        with contextlib.redirect_stdout(io.StringIO()):
            build_subtitles.main(None, game_dir)
    except BaseException as e:   # (the builders stop with SystemExit)
        set_ini(os.path.join(game_dir, "mml.ini"), "general", {"subtitles": 0})
        return (f"The subtitles couldn't be downloaded: {e}\n"
                "Everything else is installed; run Setup again later to add the subtitles.")
    set_ini(os.path.join(game_dir, "mml.ini"), "general", {"subtitles": 1})
    return f"English subtitles: the work of the {SUBS_PROJECT},\n{SUBS_REPO}"


def install_jp_voices(game_dir, jp_dir, log):
    """The Japanese voices for the US version in game_dir; returns a line for the finish page."""
    import contextlib
    import io
    log("Building the Japanese voices from your Rockman DASH copy...", 0.96)
    try:
        with contextlib.redirect_stdout(io.StringIO()):
            build_jp_voices.main(game_dir, jp_dir)
    except BaseException as e:
        return f"The Japanese voices couldn't be built: {e}"
    set_ini(os.path.join(game_dir, "mml.ini"), "general", {"voices": "japanese"})
    return "Japanese voices installed (the launcher switches between them and the English ones)."


HOOK_DLL_NAMES = [n.lower() for n in M.HOOK_DLL_NAMES]


def is_reshade(path):
    """ReShade installed for DirectX 10/11/12 hooks our own Direct3D 11 renderer and is
    supported; set up for the old DirectDraw/Direct3D 9 chain it would still conflict."""
    try:
        with open(path, "rb") as f:
            return b"ReShadeRegisterAddon" in f.read()
    except OSError:
        return False

# Files from the 0.1.x (Frida) runtime. Left in place, an old "Play - MML.bat" or the
# old launcher would attach Frida on top of mml.dll and both would patch the game, so
# an upgrade moves them to Modernization/legacy-0.1 (moved, never deleted).
LEGACY_FILES = [
    "Play - MML.bat",
    os.path.join("Modernization", "launch_widescreen_freecam.py"),
    os.path.join("Modernization", "mml_launcher.py"),
    os.path.join("Modernization", "mml_gui.py"),
]
# Config the user may have edited by hand; installed only if missing.
KEEP_IF_PRESENT = {"mml.ini"}


# The game opens CAPCOM.AVI through MCI, which can't handle a full path of 128 characters or
# more: from a deeper folder the intro silently doesn't play (everything else works; measured).
MAX_INTRO_DIR = 116


def do_install(game_dir, add_desktop_shortcut, log, jp_dir=None, subtitles=False):
    """Patch + install into game_dir. `log(msg, frac)` reports progress (frac 0..1).
    US version only: subtitles downloads the English subtitles for the cutscenes; jp_dir, a
    Rockman DASH folder, has the Japanese voices built from it. Returns (ok, message)."""
    res = _res_dir()
    name = game_exe(game_dir)
    if not name:
        return False, "MEGAMAN.EXE (or ROCKMAN.EXE, the Japanese version) was not found in that folder."
    ed = next((e for e in editions() if e.exe == name), None)
    if ed is None:
        return False, ("This is the Japanese version (ROCKMAN.EXE), which this build of the patch\n"
                       "doesn't support. Nothing was changed.")
    exe = os.path.join(game_dir, name)
    if not is_writable(game_dir):
        return False, NOT_WRITABLE + "\nNothing was changed."

    # 1. exe hash-check + backup + patch -----------------------------------------
    log("Checking your game version...", 0.05)
    digest = sha256(exe)
    if digest == ed.patched:
        log("Game already patched - keeping it.", 0.30)
    elif digest != ed.vanilla and digest not in ed.previous:
        return False, (f"This {name} is a version the patch doesn't recognize, so nothing\n"
                       "was changed. Please report this to the project:\n  " + digest)
    else:
        # Vanilla, or patched by an earlier release: the current runs overwrite every
        # byte an earlier release touched, so both land on the same verified result.
        vanilla = digest == ed.vanilla
        backup = os.path.join(game_dir, name + ".vanilla-backup")
        if vanilla and not os.path.exists(backup):
            shutil.copyfile(exe, backup)
        log("Backing up and patching the game..." if vanilla
            else "Upgrading the game patch from an earlier version...", 0.12)
        before = open(exe, "rb").read()
        data = bytearray(before[:ed.size])
        for off, hexbytes in ed.patches:
            blob = bytes.fromhex(hexbytes)
            data[off:off + len(blob)] = blob
        data += bytes.fromhex(ed.tail)
        open(exe, "wb").write(data)
        if sha256(exe) != ed.patched:
            open(exe, "wb").write(before)
            return False, "The patch did not verify - your game was put back the way it was."
        log("Game patched and verified." if ed.name == "US"
            else f"Game patched and verified ({ed.name} version).", 0.30)

    # 2. quarantine foreign wrapper DLLs -----------------------------------------
    log("Checking for conflicting mods...", 0.40)
    qdir = os.path.join(game_dir, "Modernization", "quarantined-wrappers")
    for fn in list(os.listdir(game_dir)):
        low = fn.lower()
        if low not in HOOK_DLL_NAMES:
            continue
        p = os.path.join(game_dir, fn)
        if not os.path.isfile(p):
            continue
        if low in M.OUR_DLLS and sha256(p) == M.OUR_DLLS[low]:
            continue
        if low in ("dxgi.dll", "d3d11.dll") and is_reshade(p):
            log(f"Kept ReShade ('{fn}'): it works with the Modern renderer.", 0.42)
            continue
        os.makedirs(qdir, exist_ok=True)
        dst = os.path.join(qdir, fn)
        i = 2
        while os.path.exists(dst):
            b, e = os.path.splitext(fn)
            dst = os.path.join(qdir, f"{b}.{i}{e}")
            i += 1
        shutil.move(p, dst)
        log(f"Set aside a conflicting '{fn}'.", 0.42)

    # 3. runtime stack ------------------------------------------------------------
    for rel in LEGACY_FILES:
        old = os.path.join(game_dir, rel)
        if os.path.isfile(old):
            ldir = os.path.join(game_dir, "Modernization", "legacy-0.1")
            os.makedirs(ldir, exist_ok=True)
            shutil.move(old, os.path.join(ldir, os.path.basename(rel)))
            log(f"Retired the old launcher file '{os.path.basename(rel)}'.", 0.50)
    log("Installing the widescreen + controls files...", 0.55)
    gf = os.path.join(res, "game-files")
    copied = 0
    for root, dirs, files in os.walk(gf):
        rel = os.path.relpath(root, gf)
        tgt = game_dir if rel == "." else os.path.join(game_dir, rel)
        os.makedirs(tgt, exist_ok=True)
        for fn in files:
            dst = os.path.join(tgt, fn)
            if fn.lower() in KEEP_IF_PRESENT and os.path.exists(dst):
                continue
            shutil.copyfile(os.path.join(root, fn), dst)
            copied += 1
    log(f"Installed {copied} files.", 0.80)

    # 4. intro video --------------------------------------------------------------
    src_avi = os.path.join(res, "CAPCOM.AVI")
    dst_avi = os.path.join(game_dir, "CAPCOM.AVI")
    if os.path.isfile(src_avi):
        log("Installing the intro video...", 0.88)
        if not (os.path.isfile(dst_avi) and sha256(dst_avi) == M.INTRO_SHA256):
            keep = os.path.join(game_dir, "CAPCOM.AVI.original")
            if os.path.isfile(dst_avi) and not os.path.exists(keep):
                shutil.move(dst_avi, keep)
            shutil.copyfile(src_avi, dst_avi)

    # 5. desktop shortcut ---------------------------------------------------------
    if add_desktop_shortcut:
        log("Adding the Desktop shortcut...", 0.89)
        try:
            make_desktop_shortcut(game_dir, "Mega Man Legends" if ed.name == "US" else "Rockman DASH")
        except Exception as e:
            log(f"(couldn't add the shortcut: {e})", 0.89)

    # 6. subtitles, Japanese voices (optional) --------------------------------------------
    notes = []
    if subtitles and ed.name == "US":
        notes.append(install_subtitles(game_dir, log))
    if jp_dir and ed.name == "US":
        notes.append(install_jp_voices(game_dir, jp_dir, log))
    note = "\n\n".join(notes)

    log("Done!", 1.0)
    return True, "Installation complete." + ("\n\n" + note if note else "")


# The app's folder (do_app_install) has this file: Setup and its updates tell it from a game folder
# an earlier Setup patched in place (the app's folder holds games too once they are imported).
APP_MARKER = os.path.join("Modernization", "app-folder.txt")
DEFAULT_APP_DIR = r"C:\Games\Mega Man Legends Modernization"
RUNNER64 = "mml_run64.exe"


def is_app_folder(path):
    return bool(path) and os.path.isfile(os.path.join(path, APP_MARKER))


def install_kind(path):
    """'update': a game folder an earlier Setup patched in place (updated there, do_install);
    'app': the app (new, or one to update, do_app_install)"""
    if path and not is_app_folder(path) and game_exe(path):
        return "update"
    return "app"


def do_app_install(app_dir, add_desktop_shortcut, log):
    """The app into app_dir (made if needed): the start menu and both engines (mml_run64.exe), the
    runtime the Classic engine loads beside the game, the intro, mml.ini (kept if there) and the
    licenses. Updating keeps the imported games, settings and saves. Returns (ok, message)."""
    res = _res_dir()
    try:
        os.makedirs(app_dir, exist_ok=True)
    except OSError as e:
        return False, f"The folder couldn't be made ({e}).\nNothing was changed."
    if not is_writable(app_dir):
        return False, NOT_WRITABLE_APP + "\nNothing was changed."
    log("Installing the app...", 0.10)
    gf = os.path.join(res, "game-files")
    copied = 0
    for root, dirs, files in os.walk(gf):
        rel = os.path.relpath(root, gf)
        tgt = app_dir if rel == "." else os.path.join(app_dir, rel)
        os.makedirs(tgt, exist_ok=True)
        for fn in files:
            if fn.lower() == "mml launcher.exe":
                continue   # (the start menu is the app's launcher)
            dst = os.path.join(tgt, fn)
            if fn.lower() in KEEP_IF_PRESENT and os.path.exists(dst):
                continue
            shutil.copyfile(os.path.join(root, fn), dst)
            copied += 1
    log(f"Installed {copied} files.", 0.80)
    # our intro as CAPCOM.AVI for the Compatibility renderer (the start menu's import keeps it)
    src_avi = os.path.join(res, "CAPCOM.AVI")
    dst_avi = os.path.join(app_dir, "CAPCOM.AVI")
    if os.path.isfile(src_avi) and not (os.path.isfile(dst_avi) and sha256(dst_avi) == M.INTRO_SHA256):
        shutil.copyfile(src_avi, dst_avi)
    with open(os.path.join(app_dir, APP_MARKER), "w", encoding="utf-8") as f:
        f.write("This folder holds the Mega Man Legends Modernization Project app. Its start menu imports\n"
                "your games into it. Setup and the app's updates look for this file: leave it here.\n")
    if add_desktop_shortcut:
        log("Adding the Desktop shortcut...", 0.90)
        try:
            make_desktop_shortcut(app_dir, "Mega Man Legends", app=True)
        except Exception as e:
            log(f"(couldn't add the shortcut: {e})", 0.90)
    log("Done!", 1.0)
    return True, "Installation complete."


def make_desktop_shortcut(game_dir, title="Mega Man Legends", app=False):
    """app: the start menu (mml_run64.exe --menu); else the launcher of a game folder patched in place"""
    target = os.path.join(game_dir, RUNNER64 if app else "MML Launcher.exe")
    args = "--menu" if app else ""
    desktop = os.path.join(os.path.expanduser("~"), "Desktop")
    lnk = os.path.join(desktop, title + ".lnk")
    ps = (
        "$w = New-Object -ComObject WScript.Shell; "
        f"$s = $w.CreateShortcut('{lnk}'); "
        f"$s.TargetPath = '{target}'; "
        f"$s.Arguments = '{args}'; "
        f"$s.WorkingDirectory = '{game_dir}'; "
        f"$s.IconLocation = '{target}'; "
        f"$s.Description = 'Play {title} (Modernization Project)'; "
        "$s.Save()"
    )
    subprocess.run(["powershell", "-NoProfile", "-Command", ps], check=True,
                   creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
    return lnk


# ============================ GUI wizard ============================
def run_gui():
    import tkinter as tk
    from tkinter import filedialog
    import threading

    try:
        ctypes.windll.shcore.SetProcessDpiAwareness(1)
    except Exception:
        pass

    BG = "#0c141d"; CARD = "#16202d"; ACCENT = "#3b96ff"; ACCENT_DK = "#2f7fd8"
    TEXT = "#e8eef5"; DIM = "#93a5b7"; GREEN = "#4ad073"; RED = "#e06555"; FIELD = "#22303f"
    AMBER = "#e0af45"

    root = tk.Tk()
    root.title(f"{APP_NAME} - Setup")
    root.configure(bg=BG)
    root.resizable(False, False)
    sc = max(1.0, root.winfo_fpixels("1i") / 96.0)   # (the display's scaling: fonts grow with it)

    def px(v):
        return int(v * sc)

    def fit():   # the window as wide as designed, as tall as the page needs
        root.update_idletasks()
        root.geometry(f"{px(640)}x{max(px(460), root.winfo_reqheight())}")
    try:
        root.iconbitmap(os.path.join(_res_dir(), "mml.ico"))
    except Exception:
        pass

    # --dir FOLDER: the app's (or an earlier launcher's) update starts this installer on its folder
    given = sys.argv[sys.argv.index("--dir") + 1] if "--dir" in sys.argv[:-1] else ""
    state = {"shortcut": tk.BooleanVar(value=True)}

    header = tk.Frame(root, bg=CARD, height=px(88))
    header.pack(fill="x"); header.pack_propagate(False)
    tk.Label(header, text="MEGA MAN LEGENDS", bg=CARD, fg=TEXT,
             font=("Segoe UI", 17, "bold")).pack(anchor="w", padx=22, pady=(16, 0))
    tk.Label(header, text=f"Modernization Project   ·   Setup v{VERSION}", bg=CARD, fg=ACCENT,
             font=("Segoe UI", 10, "bold")).pack(anchor="w", padx=22)

    body = tk.Frame(root, bg=BG)
    body.pack(fill="both", expand=True, padx=22, pady=16)

    pages = {}

    def show(name):
        for p in pages.values():
            p.pack_forget()
        pages[name].pack(fill="both", expand=True)

    # ---- page: welcome / the install folder ----
    p1 = tk.Frame(body, bg=BG); pages["welcome"] = p1
    tk.Label(p1, text="This installs widescreen, modern controls, an audio fix and more\n"
                      "for the Mega Man Legends PC game.",
             bg=BG, fg=TEXT, font=("Segoe UI", 11), justify="left").pack(anchor="w")
    tk.Label(p1, text="Your game is added afterwards, in the app's start menu (its APP page): pick your "
                      "Mega Man Legends and/or Rockman DASH folder there, and it is copied into the app's "
                      "folder. Your own game folders aren't changed.",
             bg=BG, fg=DIM, font=("Segoe UI", 9), justify="left", wraplength=px(580)).pack(anchor="w", pady=(4, 14))

    def check(parent, text, var, command=None, bold=False):
        return tk.Checkbutton(parent, text="  " + text, variable=var, command=command, bg=BG, fg=TEXT,
                              selectcolor=FIELD, activebackground=BG, activeforeground=TEXT,
                              disabledforeground=DIM, font=("Segoe UI", 10, "bold" if bold else "normal"),
                              bd=0, highlightthickness=0, cursor="hand2")

    def folder_row(parent, var, title):
        row = tk.Frame(parent, bg=BG)
        row.pack(fill="x", pady=(4, 2))
        e = tk.Entry(row, textvariable=var, bg=FIELD, fg=TEXT, insertbackground=TEXT, relief="flat",
                     font=("Segoe UI", 10), disabledbackground=CARD)
        e.pack(side="left", fill="x", expand=True, ipady=5, padx=(0, 8))

        def pick():
            d = filedialog.askdirectory(title=title)
            if d:
                var.set(os.path.normpath(d))
        b = tk.Button(row, text="Browse…", command=pick, bg=FIELD, fg=TEXT, relief="flat",
                      font=("Segoe UI", 10), activebackground="#2b3d50", cursor="hand2", padx=14)
        b.pack(side="left", ipady=2)
        st = tk.Label(parent, text="", bg=BG, fg=DIM, font=("Segoe UI", 9), justify="left", wraplength=px(560))
        st.pack(anchor="w")
        return (e, b), st

    tk.Label(p1, text="Install folder", bg=BG, fg=TEXT, font=("Segoe UI", 10, "bold")).pack(anchor="w")
    dir_var = tk.StringVar(value=given or DEFAULT_APP_DIR)
    _, dir_status = folder_row(p1, dir_var, "Select the folder to install into")
    tk.Checkbutton(p1, text="  Add a shortcut on my Desktop", variable=state["shortcut"],
                   bg=BG, fg=TEXT, selectcolor=FIELD, activebackground=BG, activeforeground=TEXT,
                   font=("Segoe UI", 10), bd=0, highlightthickness=0,
                   cursor="hand2").pack(anchor="w", pady=(16, 0))

    def folder(var):
        return var.get().strip('"').strip()

    def validate(*_):
        d = folder(dir_var)
        ok, col = True, GREEN
        if not d or not os.path.isabs(d):
            ok, col, text = False, DIM, "Pick the folder to install into (a full path, like " + DEFAULT_APP_DIR + ")."
        elif install_kind(d) == "update":
            name = "Rockman DASH" if game_exe(d) == "ROCKMAN.EXE" else "Mega Man Legends"
            text = (f"✓  This is your {name} folder from an earlier version of the patch: Setup updates it "
                    "where it is. Your settings and saves stay.")
            if not is_writable(d):
                ok, col, text = False, AMBER, NOT_WRITABLE.replace("\n", " ")
        elif is_app_folder(d):
            text = "✓  The app is already here: Setup updates it. Your imported games, settings and saves stay."
        elif os.path.isdir(d) and os.listdir(d):
            col = AMBER
            text = "This folder isn't empty: the app is added to what's in it. An empty or new folder is tidier."
        else:
            text = ("✓  The app goes into this folder (made if needed): about 60 MB, then about 2.5 GB for each "
                    "version of the game you import.")
        if ok and os.path.isdir(d) and install_kind(d) == "app" and not is_writable(d):
            ok, col, text = False, AMBER, NOT_WRITABLE_APP.replace("\n", " ")
        if ok and len(os.path.abspath(d)) > MAX_INTRO_DIR:
            col = AMBER
            text += (f" Heads up: this folder path is long ({len(os.path.abspath(d))} characters); the intro "
                     f"video can't play from folders longer than {MAX_INTRO_DIR}.")
        dir_status.configure(text=text, fg=col)
        install_btn.configure(state="normal" if ok else "disabled", bg=ACCENT_DK if ok else FIELD)
        fit()

    dir_var.trace_add("write", validate)

    # ---- page: installing ----
    p2 = tk.Frame(body, bg=BG); pages["installing"] = p2
    tk.Label(p2, text="Installing…", bg=BG, fg=TEXT, font=("Segoe UI", 13, "bold")).pack(anchor="w", pady=(30, 14))
    barbg = tk.Frame(p2, bg=FIELD, height=10); barbg.pack(fill="x"); barbg.pack_propagate(False)
    barfg = tk.Frame(barbg, bg=ACCENT, width=0); barfg.place(x=0, y=0, relheight=1.0)
    prog_lbl = tk.Label(p2, text="", bg=BG, fg=DIM, font=("Segoe UI", 9)); prog_lbl.pack(anchor="w", pady=8)

    def set_prog(msg, frac):
        prog_lbl.configure(text=msg)
        barfg.configure(width=int(barbg.winfo_width() * max(0.0, min(1.0, frac))))
        root.update_idletasks()

    # ---- page: done ----
    p3 = tk.Frame(body, bg=BG); pages["done"] = p3
    done_icon = tk.Label(p3, text="✓", bg=BG, fg=GREEN, font=("Segoe UI", 34, "bold"))
    done_icon.pack(anchor="w", pady=(24, 0))
    done_lbl = tk.Label(p3, text="", bg=BG, fg=TEXT, font=("Segoe UI", 13, "bold"), justify="left")
    done_lbl.pack(anchor="w")
    done_sub = tk.Label(p3, text="", bg=BG, fg=DIM, font=("Segoe UI", 10), justify="left")
    done_sub.pack(anchor="w", pady=(4, 0))

    # ---- footer buttons ----
    footer = tk.Frame(root, bg=BG); footer.pack(fill="x", side="bottom", padx=22, pady=(0, 16))

    def do_quit():
        root.destroy()

    quit_btn = tk.Button(footer, text="Cancel", command=do_quit, bg=FIELD, fg=TEXT, relief="flat",
                         font=("Segoe UI", 10), activebackground="#2b3d50", cursor="hand2", padx=16)
    quit_btn.pack(side="left", ipady=4)

    def run_install():
        show("installing")
        install_btn.pack_forget(); quit_btn.configure(state="disabled")
        root.update_idletasks()
        d = os.path.abspath(folder(dir_var))
        kind = install_kind(d)
        state["played"] = (d, kind)

        def worker():
            if kind == "update":
                result = do_install(d, state["shortcut"].get(), set_prog)
            else:
                result = do_app_install(d, state["shortcut"].get(), set_prog)
            root.after(0, lambda: finish(kind, result))
        threading.Thread(target=worker, daemon=True).start()

    def finish(kind, result):
        good, msg = result
        quit_btn.configure(state="normal", text="Close")
        show("done")
        if good:
            done_icon.configure(text="✓", fg=GREEN)
            done_lbl.configure(text="All set!")
            note = msg.split("\n\n", 1)[1] if "\n\n" in msg else ""
            shortcut = "A shortcut was added to your Desktop.\n" if state["shortcut"].get() else ""
            if kind == "app":
                text = ("The app is installed. Play now opens its start menu: on its APP page, import your "
                        "Mega Man Legends (and/or Rockman DASH) folder, then press Play.\n" + shortcut +
                        f"You can also start it any time with {RUNNER64} --menu in the install folder.")
            else:
                text = ("Your game folder is updated.\n" + shortcut +
                        "You can start it any time from MML Launcher.exe in the game folder.")
            done_sub.configure(text=text + ("\n\n" + note if note else ""), wraplength=px(580))
            play_btn.pack(side="right", ipady=4)
        else:
            done_icon.configure(text="✕", fg=RED)
            done_lbl.configure(text="Something went wrong")
            done_sub.configure(text=msg, wraplength=px(580))

    def play_now():
        try:
            d, kind = state["played"]
            if kind == "app" or not os.path.isfile(os.path.join(d, "MML Launcher.exe")):
                subprocess.Popen([os.path.join(d, RUNNER64), "--menu"], cwd=d)
            else:
                subprocess.Popen([os.path.join(d, "MML Launcher.exe")], cwd=d)
        except Exception:
            pass
        root.destroy()

    play_btn = tk.Button(footer, text="▶  Play now", command=play_now, bg=ACCENT_DK, fg="white",
                         relief="flat", font=("Segoe UI", 10, "bold"), activebackground=ACCENT,
                         cursor="hand2", padx=18)
    install_btn = tk.Button(footer, text="Install", command=run_install, bg=FIELD, fg="white",
                            relief="flat", font=("Segoe UI", 11, "bold"),
                            activebackground=ACCENT, cursor="hand2", padx=26, state="disabled")
    install_btn.pack(side="right", ipady=5)

    show("welcome")
    validate()
    fit()
    shot = os.environ.get("MML_SETUP_SHOT")
    if shot:
        def grab():
            from PIL import ImageGrab
            root.update()
            x, y = root.winfo_rootx(), root.winfo_rooty()
            ImageGrab.grab((x, y, x + root.winfo_width(), y + root.winfo_height())).save(shot)
            root.destroy()
        root.after(1300, grab)
    root.mainloop()


if __name__ == "__main__":
    if "--selftest" in sys.argv:   # headless: verify the frozen bundle installs correctly
        d = sys.argv[sys.argv.index("--selftest") + 1]
        ok, msg = do_install(d, False, lambda m, f: print(f"  {f:.2f}  {m}"))
        print("RESULT:", ok, "|", msg)
        sys.exit(0 if ok else 1)
    if "--selftest-app" in sys.argv:   # headless: the app install into a folder
        d = sys.argv[sys.argv.index("--selftest-app") + 1]
        ok, msg = do_app_install(d, False, lambda m, f: print(f"  {f:.2f}  {m}"))
        print("RESULT:", ok, "|", msg, "| kind now:", install_kind(d))
        sys.exit(0 if ok else 1)
    run_gui()
