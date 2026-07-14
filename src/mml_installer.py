"""Graphical setup wizard.

A self-contained installer (welcome -> choose the game folder -> install -> done). Built to
a single executable with PyInstaller that bundles everything needed, so the end user does
not have to install anything separately.

The install itself is do_install() - a plain function, independent of the GUI, so it can be
tested directly. Patch data comes from patch_manifest.py, the same source the command-line
applier uses.
"""
import ctypes
import hashlib
import os
import shutil
import subprocess
import sys

import patch_manifest as M

APP_NAME = "Mega Man Legends - Modernization Project"
VERSION = "0.1.1"


def _res_dir():
    """Where the bundled payload lives: sys._MEIPASS when frozen, else RE-notes/release."""
    if getattr(sys, "frozen", False):
        return sys._MEIPASS
    return os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "release")


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def is_game_folder(path):
    return bool(path) and os.path.isfile(os.path.join(path, "MEGAMAN.EXE"))


def detect_game_folder():
    """Best-effort auto-detect of a Mega Man Legends install."""
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
                    if is_game_folder(full):
                        return full
        except OSError:
            continue
    return ""


HOOK_DLL_NAMES = [n.lower() for n in M.HOOK_DLL_NAMES]


def do_install(game_dir, add_desktop_shortcut, log):
    """Patch + install into game_dir. `log(msg, frac)` reports progress (frac 0..1).
    Returns (ok, message)."""
    res = _res_dir()
    exe = os.path.join(game_dir, "MEGAMAN.EXE")
    if not os.path.isfile(exe):
        return False, "MEGAMAN.EXE was not found in that folder."

    # 1. exe hash-check + backup + patch -----------------------------------------
    log("Checking your game version...", 0.05)
    digest = sha256(exe)
    if digest == M.PATCHED_SHA256:
        log("Game already patched - keeping it.", 0.30)
    elif os.path.getsize(exe) != M.EXE_SIZE or digest != M.VANILLA_SHA256:
        return False, ("This MEGAMAN.EXE is a version the patch doesn't recognize, so nothing\n"
                       "was changed. Please report this to the project:\n  " + digest)
    else:
        backup = os.path.join(game_dir, "MEGAMAN.EXE.vanilla-backup")
        if not os.path.exists(backup):
            shutil.copyfile(exe, backup)
        log("Backing up and patching the game...", 0.12)
        data = bytearray(open(exe, "rb").read())
        for off, hexbytes in M.PATCHES:
            blob = bytes.fromhex(hexbytes)
            data[off:off + len(blob)] = blob
        open(exe, "wb").write(data)
        if sha256(exe) != M.PATCHED_SHA256:
            shutil.copyfile(backup, exe)
            return False, "The patch did not verify - your original game was restored."
        log("Game patched and verified.", 0.30)

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
    log("Installing the widescreen + controls files...", 0.55)
    gf = os.path.join(res, "game-files")
    copied = 0
    for root, dirs, files in os.walk(gf):
        rel = os.path.relpath(root, gf)
        tgt = game_dir if rel == "." else os.path.join(game_dir, rel)
        os.makedirs(tgt, exist_ok=True)
        for fn in files:
            shutil.copyfile(os.path.join(root, fn), os.path.join(tgt, fn))
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
        log("Adding the Desktop shortcut...", 0.94)
        try:
            make_desktop_shortcut(game_dir)
        except Exception as e:
            log(f"(couldn't add the shortcut: {e})", 0.95)

    log("Done!", 1.0)
    return True, "Installation complete."


def make_desktop_shortcut(game_dir):
    target = os.path.join(game_dir, "MML Launcher.exe")
    desktop = os.path.join(os.path.expanduser("~"), "Desktop")
    lnk = os.path.join(desktop, "Mega Man Legends.lnk")
    ps = (
        "$w = New-Object -ComObject WScript.Shell; "
        f"$s = $w.CreateShortcut('{lnk}'); "
        f"$s.TargetPath = '{target}'; "
        f"$s.WorkingDirectory = '{game_dir}'; "
        f"$s.IconLocation = '{target}'; "
        "$s.Description = 'Play Mega Man Legends (Modernization Project)'; "
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

    root = tk.Tk()
    root.title(f"{APP_NAME} - Setup")
    root.configure(bg=BG)
    root.resizable(False, False)
    root.geometry("640x460")
    try:
        root.iconbitmap(os.path.join(_res_dir(), "mml.ico"))
    except Exception:
        pass

    state = {"dir": detect_game_folder(), "shortcut": tk.BooleanVar(value=True), "busy": False}

    header = tk.Frame(root, bg=CARD, height=88)
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

    # ---- page: welcome / choose folder ----
    p1 = tk.Frame(body, bg=BG); pages["welcome"] = p1
    tk.Label(p1, text="This will add widescreen, modern controls, an audio fix and more\n"
                      "to your own copy of the Mega Man Legends PC game.",
             bg=BG, fg=TEXT, font=("Segoe UI", 11), justify="left").pack(anchor="w")
    tk.Label(p1, text="Nothing else needs to be installed. Your original files are backed up.",
             bg=BG, fg=DIM, font=("Segoe UI", 9), justify="left").pack(anchor="w", pady=(4, 18))
    tk.Label(p1, text="Your Mega Man Legends folder (the one with MEGAMAN.EXE):",
             bg=BG, fg=TEXT, font=("Segoe UI", 10, "bold")).pack(anchor="w")
    rowf = tk.Frame(p1, bg=BG); rowf.pack(fill="x", pady=(6, 4))
    path_var = tk.StringVar(value=state["dir"])
    entry = tk.Entry(rowf, textvariable=path_var, bg=FIELD, fg=TEXT, insertbackground=TEXT,
                     relief="flat", font=("Segoe UI", 10))
    entry.pack(side="left", fill="x", expand=True, ipady=6, padx=(0, 8))
    status = tk.Label(p1, text="", bg=BG, fg=DIM, font=("Segoe UI", 9))
    status.pack(anchor="w")

    def validate(*_):
        d = path_var.get().strip('"').strip()
        state["dir"] = d
        if is_game_folder(d):
            status.configure(text="✓  MEGAMAN.EXE found - ready to install.", fg=GREEN)
            install_btn.configure(state="normal", bg=ACCENT_DK)
        else:
            status.configure(text="Pick the folder that contains MEGAMAN.EXE.", fg=RED if d else DIM)
            install_btn.configure(state="disabled", bg=FIELD)

    def browse():
        d = filedialog.askdirectory(title="Select your Mega Man Legends folder")
        if d:
            path_var.set(os.path.normpath(d))

    tk.Button(rowf, text="Browse…", command=browse, bg=FIELD, fg=TEXT, relief="flat",
              font=("Segoe UI", 10), activebackground="#2b3d50", cursor="hand2",
              padx=14).pack(side="left", ipady=3)
    path_var.trace_add("write", validate)

    tk.Checkbutton(p1, text="  Add a Launcher Shortcut on my Desktop", variable=state["shortcut"],
                   bg=BG, fg=TEXT, selectcolor=FIELD, activebackground=BG, activeforeground=TEXT,
                   font=("Segoe UI", 10), bd=0, highlightthickness=0,
                   cursor="hand2").pack(anchor="w", pady=(22, 0))

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

        def worker():
            ok, msg = do_install(state["dir"], state["shortcut"].get(), set_prog)
            root.after(0, lambda: finish(ok, msg))
        threading.Thread(target=worker, daemon=True).start()

    def finish(ok, msg):
        quit_btn.configure(state="normal", text="Close")
        show("done")
        if ok:
            done_icon.configure(text="✓", fg=GREEN)
            done_lbl.configure(text="All set!")
            done_sub.configure(text="Mega Man Legends is ready to play.\n"
                                    + ("A shortcut was added to your Desktop.\n" if state["shortcut"].get() else "")
                                    + "You can also start it any time from MML Launcher.exe in the game folder.")
            play_btn.pack(side="right", ipady=4)
        else:
            done_icon.configure(text="✕", fg=RED)
            done_lbl.configure(text="Something went wrong")
            done_sub.configure(text=msg)

    def play_now():
        try:
            subprocess.Popen([os.path.join(state["dir"], "MML Launcher.exe")], cwd=state["dir"])
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
    run_gui()
