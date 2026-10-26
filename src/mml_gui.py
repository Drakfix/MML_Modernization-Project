"""Graphical launcher front-end.

A single-window launcher over the key art: pick resolution, aspect ratio, controls,
texture filtering, antialiasing and intro behaviour, then Play. Selections are written
to mml.ini (read by mml.dll, which the patched MEGAMAN.EXE loads itself) and to
DDrawCompat.ini, then the game is started directly. The launcher is only a settings
screen: running MEGAMAN.EXE on its own uses the last saved settings.

It can also create a Desktop shortcut that starts the game with the saved settings
without showing this window.
"""
import ctypes
import hashlib
import json
import os
import subprocess
import sys
import threading
import time

import patch_manifest as M

VERSION = "0.2.0-dev"

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
SETTINGS = os.path.join(GAME_DIR or BASE, "Modernization", "settings.json")


def clean_child_env():
    """Environment for the game process. A frozen (PyInstaller) launcher puts its temp
    extraction dir (64-bit runtime DLLs) on PATH and in its own DLL search path; the
    32-bit game must not see either, or it loads the wrong-architecture VCRUNTIME and
    dies with 'Machine Type Mismatch'."""
    try:
        ctypes.windll.kernel32.SetDllDirectoryW(None)
    except Exception:
        pass
    env = dict(os.environ)
    for k in list(env):
        if k.startswith("_PYI") or k in ("_MEIPASS", "_MEIPASS2"):
            env.pop(k, None)
    env["PATH"] = os.pathsep.join(
        p for p in env.get("PATH", "").split(os.pathsep)
        if p and not os.path.basename(p.rstrip("\\/ ")).upper().startswith("_MEI"))
    return env


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

W, H = 1010, 930
ART_X, ART_Y = 28, 86
COL_X, COL_W = 512, 468
PLAY_X, PLAY_Y = 512, 830

# DDrawCompat ResolutionScale: "display" = your monitor's resolution, app(N) = N times
# the game's own mode (640x480 in 4:3, e.g. 854x480 in widescreen).
RES_CHOICES = [("Display", "display"), ("1x", "app(1)"), ("2x", "app(2)"),
               ("3x", "app(3)"), ("4x", "app(4)"), ("5x", "app(5)")]
LEGACY_RES = {"640x480": "app(1)", "1280x960": "app(2)", "1920x1440": "app(3)",
              "2560x1920": "app(4)", "3200x2400": "app(5)"}   # 0.1.x settings.json values
DEFAULTS = {"renderer": "mml", "window": "fullscreen",
            "res": "display", "aspect": "16:9", "controls": "modern", "intro": "play",
            "scalefilter": "bilinear", "texfilter": "af16x", "aa": "off", "fog": "original"}


def load_settings():
    s = DEFAULTS.copy()
    try:
        data = json.load(open(SETTINGS, encoding="utf-8"))
        s.update({k: v for k, v in data.items() if k in DEFAULTS})
    except Exception:
        pass
    s["res"] = LEGACY_RES.get(s["res"], s["res"])
    return s


def save_settings(s):
    try:
        os.makedirs(os.path.dirname(SETTINGS), exist_ok=True)
        json.dump(s, open(SETTINGS, "w", encoding="utf-8"), indent=1)
    except Exception:
        pass


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def native_width():
    """Width of the native widescreen mode: 480 lines at the primary monitor's aspect,
    rounded to an even number. Written to both mml.ini and DDrawCompat.ini so the game
    asks for exactly the mode DDrawCompat allows."""
    u = ctypes.windll.user32
    sw, sh = u.GetSystemMetrics(0), u.GetSystemMetrics(1)
    if sw <= 0 or sh <= 0:
        return 854
    return (int(480.0 * sw / sh + 0.5) + 1) & ~1


def start_game(s):
    """Write mml.ini + DDrawCompat.ini from settings s and start the game. Returns the
    Popen handle, or None if the game folder can't be found. Shared by the GUI PLAY
    button and the --play direct-launch mode."""
    if not GAME_DIR:
        return None
    width = native_width()
    write_ini(s, width)
    write_mml_ini(s, width)
    log = os.path.join(GAME_DIR, "Modernization", "launcher.log")
    os.makedirs(os.path.dirname(log), exist_ok=True)
    with open(log, "a", encoding="utf-8") as f:
        f.write(f"--- {time.strftime('%Y-%m-%d %H:%M:%S')} v{VERSION} width {width} {json.dumps(s)}\n")
    return subprocess.Popen([os.path.join(GAME_DIR, "MEGAMAN.EXE")], cwd=GAME_DIR,
                            env=clean_child_env())


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


def write_ini(s, width):
    base = os.path.join(GAME_DIR, "DDrawCompat.ini.16to9")
    out_path = os.path.join(GAME_DIR, "DDrawCompat.ini")
    overrides = {
        "resolutionscale": f"ResolutionScale         = {s['res']}\n",
        "displayaspectratio": f"DisplayAspectRatio      = {'display' if s['aspect'] == '16:9' else '4:3'}\n",
        "resolutionscalefilter": f"ResolutionScaleFilter   = {'bilinear' if s['scalefilter'] == 'bilinear' else 'point'}\n",
        "texturefilter": f"TextureFilter           = {s['texfilter']}\n",
        "antialiasing": f"Antialiasing            = {s['aa']}\n",
        # native widescreen asks for {width}x480, and DDrawCompat only allows listed modes
        "supportedresolutions": f"SupportedResolutions    = native, 640x480, 800x600, 1024x768, {width}x480\n",
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


def merge_ini(path, values, header=""):
    """Set [section] key=value pairs in an ini file, keeping everything else in it
    (comments, keys we don't manage, [debug] and so on) exactly as it was."""
    try:
        lines = open(path, encoding="utf-8", errors="replace").read().splitlines()
    except OSError:
        lines = header.splitlines() if header else []
    pending = {sec.lower(): dict(kv) for sec, kv in values.items()}
    out, section = [], None

    def flush(sec):
        # keys of this section we haven't seen yet go at its end
        for k, v in pending.get(sec, {}).items():
            out.append(f"{k}={v}")
        pending.pop(sec, None)

    for line in lines:
        st = line.strip()
        if st.startswith("[") and st.endswith("]"):
            if section is not None:
                while out and not out[-1].strip():
                    out.pop()
                flush(section)
                out.append("")
            section = st[1:-1].strip().lower()
            out.append(line)
            continue
        if section in pending and "=" in st and not st.startswith(";"):
            key = st.split("=", 1)[0].strip()
            match = next((k for k in pending[section] if k.lower() == key.lower()), None)
            if match is not None:
                out.append(f"{key}={pending[section].pop(match)}")
                continue
        out.append(line)
    if section is not None:
        flush(section)
    for sec in list(pending):
        out.extend(["", f"[{sec}]"])
        flush(sec)
    with open(path, "w", encoding="utf-8") as f:
        f.write("\n".join(out).rstrip("\n") + "\n")


def write_mml_ini(s, width):
    """Settings for mml.dll, merged into the existing file so hand-edited keys survive."""
    aa = {"off": 1, "msaa4x": 4, "msaa8x": 8}.get(s["aa"], 1)
    aniso = 16 if s["texfilter"] == "af16x" else 0
    scale = 0 if s["res"] == "display" else int("".join(c for c in s["res"] if c.isdigit()) or 0)
    merge_ini(os.path.join(GAME_DIR, "mml.ini"), {
        "video": {
            "aspect": "16:9" if s["aspect"] == "16:9" else "4:3",
            "mode": "native",
            "native_width": width,
            "renderer": s["renderer"],
            "windowed": 1 if s["window"] == "windowed" else 0,
            "render_scale": scale,
            "nearest_upscale": 1 if s["scalefilter"] == "nearest" else 0,
            "msaa": aa,
            "anisotropy": aniso,
            "fog": s["fog"],
        },
        "controls": {"mode": s["controls"]},
        "general": {"intro": s["intro"]},
    }, header="; Mega Man Legends Modernization - settings, read by mml.dll when the game starts.\n"
              "; The launcher updates the keys it manages and leaves everything else alone.\n")


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

        def option(label, key, choices, hint=None, row=None, side=None):
            card = tk.Frame(row or col, bg=CARD)
            if row:   # half-width card sharing a row with another
                card.pack(side="left", fill="both", expand=True,
                          padx=(0, 2) if side == "left" else (2, 0))
            else:
                card.pack(fill="x", pady=2)
            top = tk.Frame(card, bg=CARD)
            top.pack(fill="x", padx=10, pady=(5, 0))
            tk.Label(top, text=label, bg=CARD, fg=TEXT,
                     font=("Segoe UI", 10, "bold")).pack(side="left")
            if hint:
                tk.Label(top, text="  " + hint, bg=CARD, fg=DIM,
                         font=("Segoe UI", 9)).pack(side="left")
            Segmented(card, self.settings, key, choices,
                      on_change=lambda: save_settings(self.settings)).pack(
                anchor="w", padx=10, pady=(1, 6))

        option("Renderer", "renderer",
               [("Modern (Direct3D 11)", "mml"), ("Compatibility", "ddrawcompat")],
               "Modern = alt-tab, screenshots + windowed work")
        pair = tk.Frame(col, bg=BG)
        pair.pack(fill="x", pady=2)
        option("Display", "window",
               [("Fullscreen", "fullscreen"), ("Windowed", "windowed")], row=pair, side="left")
        option("Upscaling", "scalefilter",
               [("Bilinear", "bilinear"), ("Nearest", "nearest")], row=pair, side="right")
        option("Resolution", "res", RES_CHOICES, "Display = your monitor (recommended)")
        option("Aspect ratio", "aspect", [("16:9 Widescreen", "16:9"), ("4:3 Classic", "4:3")])
        option("Controls", "controls",
               [("Modern twin-stick", "modern"), ("Vanilla (1997)", "vanilla")],
               "LT aim · RT shoot · X dodge · F5 orbit")
        option("Intro video", "intro",
               [("Play (any button skips)", "play"), ("Skip entirely", "skip")])
        option("Texture filter", "texfilter",
               [("Game default", "app"), ("Trilinear", "trilinear"), ("Anisotropic 16x", "af16x")])
        option("Antialiasing", "aa",
               [("Off", "off"), ("MSAA 4x", "msaa4x"), ("MSAA 8x", "msaa8x")])
        option("Distance fog", "fog",
               [("Original", "original"), ("Light", "light"), ("Off", "off")], "Light = twice as far away")

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
            if digest == M.PATCHED_SHA256:
                c.itemconfigure(self.exe_item, text="●  Game: Modernized  ✓", fill=GREEN)
            elif digest in M.PREVIOUS_PATCHED_SHA256:
                c.itemconfigure(self.exe_item, text="●  Game: older Modernization version - run the new installer", fill=AMBER)
            elif digest == M.VANILLA_SHA256:
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
            text="Launched!  F5 or L3/R3 = orbit camera · any button skips the intro")
        # the game runs on its own now (mml.dll loads with it); nothing left to do here
        self.root.after(1500, self.root.destroy)

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
