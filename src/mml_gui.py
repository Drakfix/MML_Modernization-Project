"""Graphical launcher front-end.

A single-window launcher over the key art: pick resolution, aspect ratio, controls,
texture filtering, antialiasing and intro behaviour, then Play. Selections are written to
DDrawCompat.ini and passed to launch_widescreen_freecam.py through the environment.

When run as the packaged executable it re-invokes itself with --run-game and executes the
runner inside its own interpreter (no console; output goes to launcher.log). It can also
create a Desktop shortcut that launches straight into the game with the saved settings.
"""
import ctypes
import hashlib
import json
import os
import subprocess
import sys
import threading
import time

VANILLA_SHA256 = "cb0228dfcb3416ed48b6ec8c4b5cdb659c05e104a8e2e7c2b1720cc97d203324"
PATCHED_SHA256 = "4b583cd622524997b5431fcda5b26cd775b98a489f85984338d293eb32c69406"
VERSION = "0.1.3"

HERE = os.path.dirname(os.path.abspath(__file__))
FROZEN = getattr(sys, "frozen", False)
BASE = os.path.dirname(os.path.abspath(sys.executable)) if FROZEN else HERE


def find_game_dir():
    env = os.environ.get("MML_GAME_DIR")
    if env and os.path.exists(os.path.join(env, "MEGAMAN.EXE")):
        return os.path.abspath(env)
    p = BASE
    for _ in range(4):
        if os.path.exists(os.path.join(p, "MEGAMAN.EXE")):
            return p
        p = os.path.dirname(p)
    if os.path.exists(os.path.join(os.getcwd(), "MEGAMAN.EXE")):
        return os.getcwd()
    return None


GAME_DIR = find_game_dir()


def _find_runner():
    for cand in (os.path.join(BASE, "launch_widescreen_freecam.py"),
                 os.path.join(BASE, "Modernization", "launch_widescreen_freecam.py"),
                 os.path.join(GAME_DIR or ".", "Modernization", "launch_widescreen_freecam.py")):
        if os.path.exists(cand):
            return cand
    return os.path.join(BASE, "launch_widescreen_freecam.py")


RUNNER = _find_runner()
SETTINGS = os.path.join(os.path.dirname(RUNNER), "settings.json")

def strip_pyinstaller_from_path():
    """CRITICAL: PyInstaller prepends its onefile temp-extraction dir (full of 64-bit
    support DLLs like VCRUNTIME140.dll) to PATH. The 32-bit MEGAMAN.EXE we spawn inherits
    that PATH and loads the WRONG-architecture DLLs -> 'Machine Type Mismatch' + DSOUND
    entry-point errors + game death (which frida then reports as a failed injection).
    Remove every _MEI* extraction dir from PATH so the game finds its own/system DLLs -
    exactly the clean environment the Play-MML.bat (system python) path already had."""
    parts = os.environ.get("PATH", "").split(os.pathsep)
    clean = [p for p in parts
             if not os.path.basename(p.rstrip("\\/ ")).upper().startswith("_MEI")]
    os.environ["PATH"] = os.pathsep.join(clean)


# ---------- --run-game mode (frozen exe hosts the runner in-process) ----------
if "--run-game" in sys.argv:
    import runpy
    strip_pyinstaller_from_path()
    sys.argv = [RUNNER]           # the runner parses argv[1..] as floats - hide our flag
    log = open(os.path.join(os.path.dirname(RUNNER), "launcher.log"), "a",
               buffering=1, encoding="utf-8")
    sys.stdout = sys.stderr = log
    print(f"\n--- session {time.strftime('%Y-%m-%d %H:%M:%S')} ---")
    runpy.run_path(RUNNER, run_name="__main__")
    sys.exit(0)

import tkinter as tk  # noqa: E402

# ---------- theme ----------
BG = "#0c141d"
CARD = "#16202dcc"[:7]      # solid card color (tk has no alpha)
CARD_HI = "#1b2836"
SEG_OFF = "#22303f"
SEG_HOVER = "#2b3d50"
ACCENT = "#3b96ff"
TEXT = "#e8eef5"
DIM = "#93a5b7"
GREEN = "#4ad073"
AMBER = "#e0af45"
RED = "#e06555"

W, H = 1010, 878
ART_X, ART_Y = 28, 86
COL_X, COL_W = 512, 468
PLAY_X, PLAY_Y = 512, 784

RES_CHOICES = [("Display", "display"), ("1x", "640x480"), ("2x", "1280x960"),
               ("3x", "1920x1440"), ("4x", "2560x1920"), ("5x", "3200x2400")]
DEFAULTS = {"res": "display", "aspect": "16:9", "controls": "modern", "intro": "play",
            "scalefilter": "bilinear", "texfilter": "af16x", "aa": "off"}


def load_settings():
    s = DEFAULTS.copy()
    try:
        data = json.load(open(SETTINGS, encoding="utf-8"))
        s.update({k: v for k, v in data.items() if k in DEFAULTS})
    except Exception:
        pass
    return s


def save_settings(s):
    try:
        json.dump(s, open(SETTINGS, "w", encoding="utf-8"), indent=1)
    except Exception:
        pass


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def start_game(s):
    """Write the DDrawCompat config from settings s and spawn the runtime launcher
    (the frozen exe re-invokes itself with --run-game; dev uses python). Returns the
    Popen handle, or None if the game folder can't be found. Shared by the GUI PLAY
    button and the --play direct-launch mode."""
    if not GAME_DIR:
        return None
    write_ini(s)
    env = dict(os.environ)
    # Let the spawned launcher exe do its OWN PyInstaller onefile extraction. Without this,
    # the child inherits our bootloader vars (_PYI_* / _MEIPASS2) and REUSES this process's
    # _MEI temp dir - which gets deleted the instant we exit, so the child crashes with
    # "base_library.zip not found" (verified: scratchpad/probe3). That is why the --play
    # shortcut - whose parent exits immediately - failed, while the GUI PLAY (window stays
    # open) survived by luck. Stripping these forces the child to extract its own dir.
    for k in list(env):
        if k.startswith("_PYI") or k == "_MEIPASS2":
            env.pop(k, None)
    env["MML_GAME_DIR"] = GAME_DIR
    env["MML_ASPECT"] = s["aspect"]
    env["MML_CONTROLS"] = s["controls"]
    env["MML_INTRO"] = s["intro"]
    logf = open(os.path.join(os.path.dirname(RUNNER), "launcher.log"), "a",
                buffering=1, encoding="utf-8")
    logf.write(f"\n--- session {time.strftime('%Y-%m-%d %H:%M:%S')} {json.dumps(s)} ---\n")
    cmd = [sys.executable, "--run-game"] if FROZEN else [sys.executable, RUNNER]
    flags = subprocess.CREATE_NO_WINDOW if FROZEN else 0
    return subprocess.Popen(cmd, cwd=GAME_DIR, env=env, stdout=logf, stderr=logf,
                            creationflags=flags)


def make_play_shortcut():
    """Create a 'Play Mega Man Legends.lnk' on the Desktop that runs this launcher with
    --play (skips the GUI, boots straight into the game with the saved settings). Returns
    the shortcut path or raises. Frozen exe only (needs a real .exe target)."""
    target = sys.executable
    desktop = os.path.join(os.path.expanduser("~"), "Desktop")
    lnk = os.path.join(desktop, "Play Mega Man Legends.lnk")
    icon = target
    ps = (
        "$w = New-Object -ComObject WScript.Shell; "
        f"$s = $w.CreateShortcut('{lnk}'); "
        f"$s.TargetPath = '{target}'; "
        "$s.Arguments = '--play'; "
        f"$s.WorkingDirectory = '{os.path.dirname(target)}'; "
        f"$s.IconLocation = '{icon}'; "
        "$s.Description = 'Play Mega Man Legends with your saved settings'; "
        "$s.Save()"
    )
    subprocess.run(["powershell", "-NoProfile", "-Command", ps], check=True,
                   creationflags=subprocess.CREATE_NO_WINDOW)
    return lnk


def write_ini(s):
    base = os.path.join(GAME_DIR, "DDrawCompat.ini.16to9")
    out_path = os.path.join(GAME_DIR, "DDrawCompat.ini")
    overrides = {
        "resolutionscale": f"ResolutionScale         = {s['res']}\n",
        "displayaspectratio": f"DisplayAspectRatio      = {'display' if s['aspect'] == '16:9' else '4:3'}\n",
        "resolutionscalefilter": f"ResolutionScaleFilter   = {'bilinear' if s['scalefilter'] == 'bilinear' else 'point'}\n",
        "texturefilter": f"TextureFilter           = {s['texfilter']}\n",
        "antialiasing": f"Antialiasing            = {s['aa']}\n",
    }
    lines, seen = [], set()
    with open(base, encoding="utf-8", errors="replace") as f:
        for line in f:
            key = line.split("=", 1)[0].strip().lower() if "=" in line else ""
            if key in overrides:
                lines.append(overrides[key])
                seen.add(key)
            else:
                lines.append(line)
    for key, text in overrides.items():
        if key not in seen:
            lines.append(text)
    with open(out_path, "w", encoding="utf-8") as f:
        f.writelines(lines)


def pad_connected():
    class GP(ctypes.Structure):
        _fields_ = [("a", ctypes.c_uint32), ("b", ctypes.c_uint8 * 12)]
    for name in ("xinput1_4", "xinput1_3", "xinput9_1_0"):
        try:
            dll = ctypes.windll.LoadLibrary(name)
            return dll.XInputGetState(0, ctypes.byref(GP())) == 0
        except OSError:
            continue
    return False


class Segmented(tk.Frame):
    def __init__(self, parent, store, key, choices, on_change=None):
        super().__init__(parent, bg=CARD)
        self.store, self.key, self.on_change = store, key, on_change
        self.buttons = {}
        for i, (label, value) in enumerate(choices):
            b = tk.Label(self, text=label, padx=11, pady=4, cursor="hand2",
                         font=("Segoe UI", 10), bd=0)
            b.grid(row=0, column=i, padx=(0 if i == 0 else 3, 0))
            b.bind("<Button-1>", lambda e, v=value: self.set(v))
            b.bind("<Enter>", lambda e, v=value, w=b: self._hover(w, v, True))
            b.bind("<Leave>", lambda e, v=value, w=b: self._hover(w, v, False))
            self.buttons[value] = b
        self.refresh()

    def _hover(self, widget, value, on):
        if self.store[self.key] != value:
            widget.configure(bg=SEG_HOVER if on else SEG_OFF)

    def set(self, value):
        self.store[self.key] = value
        self.refresh()
        if self.on_change:
            self.on_change()

    def refresh(self):
        cur = self.store[self.key]
        for value, b in self.buttons.items():
            if value == cur:
                b.configure(bg=ACCENT, fg="white", font=("Segoe UI", 10, "bold"))
            else:
                b.configure(bg=SEG_OFF, fg=DIM, font=("Segoe UI", 10))


class App:
    def __init__(self, root):
        self.root = root
        self.settings = load_settings()
        self.proc = None
        root.title("Mega Man Legends - Modernization Project")
        root.configure(bg=BG)
        root.resizable(False, False)
        root.geometry(f"{W}x{H}")
        try:
            root.iconbitmap(self.res_path("mml.ico"))
        except Exception:
            pass

        c = self.canvas = tk.Canvas(root, width=W, height=H, highlightthickness=0, bd=0, bg=BG)
        c.pack()
        self.img_bg = tk.PhotoImage(file=self.res_path("gui_bg.png"))
        self.img_art = tk.PhotoImage(file=self.res_path("gui_art_round.png"))
        self.img_play = tk.PhotoImage(file=self.res_path("gui_play.png"))
        self.img_play_hot = tk.PhotoImage(file=self.res_path("gui_play_hot.png"))
        c.create_image(0, 0, image=self.img_bg, anchor="nw")
        c.create_image(ART_X, ART_Y, image=self.img_art, anchor="nw")

        # title block (drawn on the blur, with soft shadow)
        c.create_text(COL_X + 2, 24 + 2, text="MEGA MAN LEGENDS", anchor="nw",
                      font=("Segoe UI", 23, "bold"), fill="#04080d")
        c.create_text(COL_X, 24, text="MEGA MAN LEGENDS", anchor="nw",
                      font=("Segoe UI", 23, "bold"), fill=TEXT)
        c.create_text(COL_X + 2, 62, text=f"M O D E R N I Z A T I O N   P R O J E C T    v{VERSION}",
                      anchor="nw", font=("Segoe UI", 10, "bold"), fill=ACCENT)
        c.create_text(ART_X + 4, 30, anchor="nw", fill=DIM, font=("Segoe UI", 10),
                      text="1997 Capcom PC port · widescreen · twin-stick · fixed audio")

        # status lines (canvas texts, colored dynamically)
        self.exe_item = c.create_text(COL_X, 94, anchor="nw", font=("Segoe UI", 10), fill=DIM, text="…")
        self.pad_item = c.create_text(COL_X, 114, anchor="nw", font=("Segoe UI", 10), fill=DIM, text="…")

        # options column (frame hosted on the canvas)
        col = tk.Frame(root, bg=BG)
        c.create_window(COL_X, 140, window=col, anchor="nw", width=COL_W)

        def option(label, key, choices, hint=None):
            card = tk.Frame(col, bg=CARD)
            card.pack(fill="x", pady=3)
            top = tk.Frame(card, bg=CARD)
            top.pack(fill="x", padx=10, pady=(6, 1))
            tk.Label(top, text=label, bg=CARD, fg=TEXT,
                     font=("Segoe UI", 10, "bold")).pack(side="left")
            if hint:
                tk.Label(top, text="  " + hint, bg=CARD, fg=DIM,
                         font=("Segoe UI", 9)).pack(side="left")
            Segmented(card, self.settings, key, choices,
                      on_change=lambda: save_settings(self.settings)).pack(
                anchor="w", padx=10, pady=(0, 7))

        option("Resolution", "res", RES_CHOICES, "Display = your monitor (recommended)")
        option("Aspect ratio", "aspect", [("16:9 Widescreen", "16:9"), ("4:3 Classic", "4:3")])
        option("Controls", "controls",
               [("Modern twin-stick", "modern"), ("Vanilla (1997)", "vanilla")],
               "LT aim · RT shoot · X dodge · F5 orbit")
        option("Intro video", "intro",
               [("Play (any button skips)", "play"), ("Skip entirely", "skip")])
        option("Upscaling", "scalefilter",
               [("Bilinear", "bilinear"), ("Nearest", "nearest")], "image scaling filter")
        option("Texture filter", "texfilter",
               [("Game default", "app"), ("Trilinear", "trilinear"), ("Anisotropic 16x", "af16x")])
        option("Antialiasing", "aa",
               [("Off", "off"), ("MSAA 4x", "msaa4x"), ("MSAA 8x", "msaa8x")])

        # PLAY pill (canvas image + text, hover swap)
        self.play_img = c.create_image(PLAY_X, PLAY_Y, image=self.img_play, anchor="nw")
        self.play_txt = c.create_text(PLAY_X + 234, PLAY_Y + 30, text="▶   P L A Y",
                                      font=("Segoe UI", 15, "bold"), fill="white")
        for item in (self.play_img, self.play_txt):
            c.tag_bind(item, "<Button-1>", lambda e: self.launch())
            c.tag_bind(item, "<Enter>", lambda e: c.itemconfigure(self.play_img, image=self.img_play_hot))
            c.tag_bind(item, "<Leave>", lambda e: c.itemconfigure(self.play_img, image=self.img_play))
        # one-click direct-play shortcut, as a proper secondary button (frozen exe only -
        # a shortcut to python.exe --play wouldn't work in the dev tree).
        if FROZEN:
            self.sc_btn = tk.Label(root, text="＋   Create Desktop Shortcut",
                                   bg=SEG_OFF, fg=TEXT, font=("Segoe UI", 10, "bold"),
                                   padx=16, pady=8, cursor="hand2", bd=0)
            c.create_window(COL_X, PLAY_Y - 16, window=self.sc_btn, anchor="sw")
            self.sc_btn.bind("<Button-1>", lambda e: self.make_shortcut())
            self.sc_btn.bind("<Enter>", lambda e: self.sc_btn.configure(bg=SEG_HOVER, fg="white"))
            self.sc_btn.bind("<Leave>", lambda e: self.sc_btn.configure(bg=SEG_OFF, fg=TEXT))

        self.foot_item = c.create_text(COL_X, PLAY_Y + 70, anchor="nw",
                                       font=("Segoe UI", 9), fill=DIM, text="")

        self.refresh_status()

    @staticmethod
    def res_path(name):
        if FROZEN:
            return os.path.join(sys._MEIPASS, "art", name)
        return os.path.join(os.path.dirname(HERE), "art", name)

    def refresh_status(self):
        c = self.canvas
        if not GAME_DIR:
            c.itemconfigure(self.exe_item, text="●  Game not found - put this launcher in the game folder", fill=RED)
        else:
            digest = sha256(os.path.join(GAME_DIR, "MEGAMAN.EXE"))
            if digest == PATCHED_SHA256:
                c.itemconfigure(self.exe_item, text="●  Game: Modernized  ✓", fill=GREEN)
            elif digest == VANILLA_SHA256:
                c.itemconfigure(self.exe_item, text="●  Game: vanilla exe - run the installer first", fill=AMBER)
            else:
                c.itemconfigure(self.exe_item, text="●  Game: unknown exe build - see README", fill=AMBER)
        self.update_pad()

    def update_pad(self):
        def poll():
            ok = pad_connected()
            self.root.after(0, lambda: self.canvas.itemconfigure(
                self.pad_item,
                text="●  Controller detected" if ok else "●  No controller (keyboard works too)",
                fill=GREEN if ok else DIM))
            self.root.after(3000, self.update_pad)
        threading.Thread(target=poll, daemon=True).start()

    def launch(self):
        if not GAME_DIR:
            return
        if self.proc and self.proc.poll() is None:
            self.canvas.itemconfigure(self.foot_item, text="Already running.")
            return
        save_settings(self.settings)
        self.proc = start_game(self.settings)
        self.canvas.itemconfigure(
            self.foot_item,
            text="Launched!  F5 or L3/R3 = orbit camera · any button skips the intro\n"
                 "Closing this window removes the mods (the game keeps running, vanilla).")
        self.root.iconify()

    def make_shortcut(self):
        save_settings(self.settings)
        try:
            lnk = make_play_shortcut()
            self.canvas.itemconfigure(
                self.foot_item,
                text=f"Created '{os.path.basename(lnk)}' on your Desktop.\n"
                     "Double-click it to boot straight into the game with these settings.")
        except Exception as e:
            self.canvas.itemconfigure(self.foot_item, text=f"Could not create shortcut: {e}")


def main():
    # Direct-play mode (from the Desktop shortcut): boot straight into the game with the
    # saved settings, no launcher window.
    if "--play" in sys.argv:
        start_game(load_settings())
        return
    try:
        ctypes.windll.shcore.SetProcessDpiAwareness(1)
    except Exception:
        pass
    root = tk.Tk()
    App(root)
    shot = os.environ.get("MML_GUI_SHOT")
    if shot:
        def grab():
            from PIL import ImageGrab
            root.update()
            x, y = root.winfo_rootx(), root.winfo_rooty()
            ImageGrab.grab((x, y, x + root.winfo_width(), y + root.winfo_height())).save(shot)
            root.destroy()
        root.after(1400, grab)
    root.mainloop()


if __name__ == "__main__":
    main()
