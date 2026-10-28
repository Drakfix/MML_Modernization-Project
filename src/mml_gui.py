"""Graphical launcher front-end.

A single-window launcher over the key art: pick resolution, aspect ratio, controls,
texture filtering, antialiasing and intro behaviour, then Play. Selections are written
to mml.ini (read by mml.dll, which the patched MEGAMAN.EXE loads itself) and to
DDrawCompat.ini, then the game is started directly. The launcher is only a settings
screen: running MEGAMAN.EXE on its own uses the last saved settings.

It can also create a Desktop shortcut that starts the game with the saved settings
without showing this window.
"""
import configparser
import ctypes
import ctypes.wintypes
import hashlib
import json
import os
import subprocess
import sys
import threading
import time

import patch_manifest as M

VERSION = "0.5.0-beta"

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
import tkinter.font as tkfont  # noqa: E402

# ---------- theme ----------
# The in-game settings menu's look (native/src/ui_style.c): the game's HUD navy and
# Mega Man blue, the lock-on yellow for whatever is selected, cyan edges.
BG = "#0b1628"              # the options panel (tk frames can't be see-through, so it's solid)
CARD = "#112139"
CARD_EDGE = "#22456c"
CARD_HI = "#16294a"
SEG_OFF = "#183052"
SEG_HOVER = "#22406a"
ACCENT = "#5cc8ff"          # cyan: edges, the subtitle
BLUE = "#1f66da"
BLUE_DARK = "#0c2a6a"
YELLOW = "#ffd23a"
YELLOW_LO = "#ffa81c"
NAVY = "#0a1830"
TEXT = "#e6f0fa"
DIM = "#8ea4bc"
GREEN = "#62f06e"
AMBER = "#ffb347"
RED = "#ff6a5a"
SHADOW = "#03070d"
HEAD = "Segoe UI"           # display font, set to Bahnschrift in App() when Windows has it


def mix(a, b, t):
    """Blend two #rrggbb colours."""
    pa = [int(a[i:i + 2], 16) for i in (1, 3, 5)]
    pb = [int(b[i:i + 2], 16) for i in (1, 3, 5)]
    return "#" + "".join(f"{round(x + (y - x) * t):02x}" for x, y in zip(pa, pb))


def chamfer(x0, y0, x1, y1, big, small):
    """Outline points of a box with the top-right and bottom-left corners cut by `big`
    and the other two bevelled by `small` (the in-game panels' shape)."""
    return [x0 + small, y0, x1 - big, y0, x1, y0 + big, x1, y1 - small,
            x1 - small, y1, x0 + big, y1, x0, y1 - big, x0, y0 + small]


def gradient(c, y0, y1, left, right, top, bottom, tags=(), steps=24):
    """A vertical gradient as horizontal slices; left(y)/right(y) give the shape's edges.
    Returns [(item, t)] so the colours can be changed later (hover)."""
    out = []
    for i in range(steps):
        ya, yb = y0 + (y1 - y0) * i / steps, y0 + (y1 - y0) * (i + 1) / steps
        t = i / max(1, steps - 1)
        item = c.create_polygon(left(ya), ya, right(ya), ya, right(yb), yb, left(yb), yb,
                                fill=mix(top, bottom, t), outline="", tags=tags)
        out.append((item, t))
    return out


def outlined_text(c, x, y, text, font, fill="white", outline=NAVY, o=2, tags=(), anchor="nw"):
    """Text with a dark outline and drop shadow, like the game's title lettering."""
    c.create_text(x + o + 1, y + o + 1, text=text, font=font, fill=SHADOW, anchor=anchor, tags=tags)
    for dx, dy in ((-o, 0), (o, 0), (0, -o), (0, o), (-o, -o), (o, -o), (-o, o), (o, o)):
        c.create_text(x + dx, y + dy, text=text, font=font, fill=outline, anchor=anchor, tags=tags)
    return c.create_text(x, y, text=text, font=font, fill=fill, anchor=anchor, tags=tags)


def brackets(c, x0, y0, x1, y1, size=26, width=3):
    """Yellow corner brackets on the top-left and bottom-right."""
    g = 4
    c.create_line(x0 - g, y0 + size, x0 - g, y0 - g, x0 + size, y0 - g, fill=YELLOW, width=width)
    c.create_line(x1 + g, y1 - size, x1 + g, y1 + g, x1 - size, y1 + g, fill=YELLOW, width=width)


def panel_frame(c, x0, y0, x1, y1, big, small, fill=None):
    """An in-game style panel: drop shadow, optional fill, cyan double edge, brackets."""
    c.create_polygon(chamfer(x0 + 6, y0 + 8, x1 + 6, y1 + 8, big, small), fill=SHADOW, outline="")
    if fill:
        c.create_polygon(chamfer(x0, y0, x1, y1, big, small), fill=fill, outline="")
    c.create_polygon(chamfer(x0, y0, x1, y1, big, small), fill="", outline=ACCENT, width=2)
    c.create_polygon(chamfer(x0 + 5, y0 + 5, x1 - 5, y1 - 5, big - 2, max(0, small - 2)), fill="",
                     outline="#1f4a72", width=1)
    brackets(c, x0, y0, x1, y1)

W, H = 1010, 930
ART_X, ART_Y = 28, 86
COL_X, COL_W = 512, 468
PLAY_X, PLAY_Y = 512, 830
TITLE_BAR = 40    # title bar + borders
COMPACT_MIN_H = 600


def work_area():
    """(left, top, right, bottom) of the screen minus the taskbar, or None.
    MML_GUI_WORKAREA_H pretends the screen is that tall (to preview the compact layout)."""
    try:
        r = ctypes.wintypes.RECT()
        ctypes.windll.user32.SystemParametersInfoW(0x30, 0, ctypes.byref(r), 0)   # SPI_GETWORKAREA
        area = [r.left, r.top, r.right, r.bottom]
    except Exception:
        return None
    fake = os.environ.get("MML_GUI_WORKAREA_H")
    if fake and fake.isdigit():
        area[3] = area[1] + int(fake)
    return area

# DDrawCompat ResolutionScale: "display" = your monitor's resolution, app(N) = N times
# the game's own mode (640x480 in 4:3, e.g. 854x480 in widescreen).
RES_CHOICES = [("Display", "display"), ("1x", "app(1)"), ("2x", "app(2)"),
               ("3x", "app(3)"), ("4x", "app(4)"), ("5x", "app(5)")]
LEGACY_RES = {"640x480": "app(1)", "1280x960": "app(2)", "1920x1440": "app(3)",
              "2560x1920": "app(4)", "3200x2400": "app(5)"}   # 0.1.x settings.json values
DEFAULTS = {"renderer": "mml", "window": "fullscreen",
            "res": "display", "aspect": "16:9", "controls": "modern", "intro": "play",
            "scalefilter": "bilinear", "texfilter": "af16x", "aa": "off", "fog": "original",
            "lod": "full", "fps": "60", "reticle": "FFD840",
            "input": "sdl", "orbit_start": "off", "cam_speed": "100", "invert_x": "off",
            "invert_y": "off", "deadzone": "24", "roll": "fast", "walk": "on", "tab": "display",
            "mouse": "100", "shader": "", "shader_res": "game", "texpack": "on", "texdump": "off"}

# button remapping (modern controls): physical input -> action, mml.ini [controls] btn_*
BUTTONS = [("btn_a", "A", "Cross"), ("btn_b", "B", "Circle"), ("btn_x", "X", "Square"),
           ("btn_y", "Y", "Triangle"), ("btn_lb", "LB", "L1"), ("btn_rb", "RB", "R1"),
           ("btn_lt", "LT", "L2"), ("btn_rt", "RT", "R2"), ("btn_l3", "L3", "stick"),
           ("btn_r3", "R3", "stick"), ("btn_back", "Back", "Share / −"),
           ("btn_start", "Start", "Options / +")]
ACTIONS = [("jump", "Jump"), ("shoot", "Shoot"), ("special", "Special"), ("talk", "Talk / check"),
           ("roll", "Roll"), ("aim", "Aim / lock-on"), ("orbit", "Modern on/off"),
           ("menu", "Menu"), ("select", "Select"), ("none", "Nothing")]
BUTTON_DEFAULTS = {"btn_a": "jump", "btn_b": "special", "btn_x": "roll", "btn_y": "talk", "btn_lb": "aim",
                   "btn_rb": "none", "btn_lt": "aim", "btn_rt": "shoot", "btn_l3": "orbit", "btn_r3": "orbit",
                   "btn_back": "select", "btn_start": "menu"}
DEFAULTS.update(BUTTON_DEFAULTS)

# keyboard + mouse bindings (modern controls): slot -> "KEY" or "KEY,KEY", mml.ini [controls] key_*
KEY_SLOTS = [("key_up", "Forward"), ("key_down", "Back"), ("key_left", "Left"), ("key_right", "Right"),
             ("key_walk", "Walk (hold)"), ("key_jump", "Jump"), ("key_shoot", "Shoot"), ("key_aim", "Aim / lock"),
             ("key_roll", "Roll"), ("key_talk", "Talk / check"), ("key_special", "Special"), ("key_menu", "Menu"),
             ("key_select", "Select"), ("key_orbit", "Modern on/off")]
KEY_DEFAULTS = {"key_up": "W,UP", "key_down": "S,DOWN", "key_left": "A,LEFT", "key_right": "D,RIGHT",
                "key_walk": "LCTRL", "key_jump": "SPACE", "key_shoot": "MOUSE1", "key_aim": "MOUSE2",
                "key_roll": "LSHIFT", "key_talk": "E", "key_special": "Q", "key_menu": "TAB",
                "key_select": "M", "key_orbit": "F5"}
DEFAULTS.update(KEY_DEFAULTS)
DEFAULTS["buttons_view"] = "pad"
# Tk key names -> mml.ini key names (anything else: letters, digits, F1-F12 as they are)
TK_KEYS = {"space": "SPACE", "Tab": "TAB", "Return": "ENTER", "BackSpace": "BACKSPACE", "Caps_Lock": "CAPSLOCK",
           "Shift_L": "LSHIFT", "Shift_R": "RSHIFT", "Control_L": "LCTRL", "Control_R": "RCTRL",
           "Alt_L": "LALT", "Alt_R": "RALT", "Up": "UP", "Down": "DOWN", "Left": "LEFT", "Right": "RIGHT",
           "Insert": "INSERT", "Delete": "DELETE", "Home": "HOME", "End": "END", "Prior": "PGUP", "Next": "PGDN",
           "comma": "COMMA", "period": "PERIOD", "slash": "SLASH", "semicolon": "SEMICOLON",
           "apostrophe": "QUOTE", "bracketleft": "LBRACKET", "bracketright": "RBRACKET", "minus": "MINUS",
           "equal": "EQUALS", "grave": "BACKQUOTE", "backslash": "BACKSLASH"}
KEY_LABELS = {"MOUSE1": "L click", "MOUSE2": "R click", "MOUSE3": "M click", "MOUSE4": "Mouse 4",
              "MOUSE5": "Mouse 5", "LSHIFT": "L Shift", "RSHIFT": "R Shift", "LCTRL": "L Ctrl", "RCTRL": "R Ctrl",
              "LALT": "L Alt", "RALT": "R Alt", "SPACE": "Space", "TAB": "Tab", "ENTER": "Enter",
              "BACKSPACE": "Bksp", "CAPSLOCK": "Caps", "UP": "Up", "DOWN": "Down", "LEFT": "Left",
              "RIGHT": "Right", "PGUP": "PgUp", "PGDN": "PgDn", "INSERT": "Ins", "DELETE": "Del"}


def tk_key_name(keysym):
    """Tk keysym -> mml.ini key name, or None if it can't be bound."""
    if keysym in TK_KEYS:
        return TK_KEYS[keysym]
    if len(keysym) == 1 and keysym.isalnum():
        return keysym.upper()
    if keysym.startswith("KP_") and keysym[3:].isdigit():
        return "NUM" + keysym[3:]
    if keysym[:1] == "F" and keysym[1:].isdigit() and 1 <= int(keysym[1:]) <= 12 and keysym not in ("F9", "F10"):
        return keysym
    return None


def key_label(name):
    return KEY_LABELS.get(name, name.title() if name.startswith("NUM") else name) if name else "-"

# roll style -> mml.ini roll_speed (stock cartwheel 512 = running speed), roll_anim (% faster)
ROLL_STYLES = {"classic": (512, 0), "fast": (768, 50), "snappy": (896, 80)}

# lock-on reticle colours offered in the launcher (any RRGGBB works in mml.ini)
RETICLE_PRESETS = [("Yellow", "FFD840"), ("Orange", "FF9A30"), ("Red", "FF4848"), ("Pink", "FF70C8"),
                   ("Purple", "B47CFF"), ("Blue", "4FA2FF"), ("Cyan", "40E0E8"), ("Green", "5EE070"),
                   ("White", "FFFFFF")]


def load_settings():
    s = DEFAULTS.copy()
    try:
        data = json.load(open(SETTINGS, encoding="utf-8"))
        s.update({k: v for k, v in data.items() if k in DEFAULTS})
    except Exception:
        pass
    s["res"] = LEGACY_RES.get(s["res"], s["res"])
    import_mml_ini(s)
    return s


def import_mml_ini(s):
    """Pick up what the in-game settings menu (F1) saved to mml.ini, so the next PLAY
    doesn't put the launcher's older values back."""
    if not GAME_DIR:
        return
    ini = configparser.ConfigParser(strict=False, interpolation=None)
    try:
        ini.read(os.path.join(GAME_DIR, "mml.ini"), encoding="utf-8")
    except (configparser.Error, OSError):
        return

    def get(sec, key):
        try:
            return ini.get(sec, key).strip()
        except (configparser.Error, ValueError):
            return None

    def num(sec, key):
        v = get(sec, key)
        return int(v) if v and v.lstrip("-").isdigit() else None

    v = num("video", "interp")
    if v is not None:
        s["fps"] = "60" if v else "30"
    v = num("video", "render_scale")
    if v is not None:
        s["res"] = "display" if v <= 0 else f"app({v})"
    v = num("video", "msaa")
    if v is not None:
        s["aa"] = {4: "msaa4x", 8: "msaa8x"}.get(v, "off")
    v = num("video", "anisotropy")
    if v is not None:
        if v >= 16:
            s["texfilter"] = "af16x"
        elif s["texfilter"] == "af16x":
            s["texfilter"] = "trilinear"
    v = num("video", "nearest_upscale")
    if v is not None:
        s["scalefilter"] = "nearest" if v else "bilinear"
    v = get("video", "fog")
    if v in ("original", "light", "off"):
        s["fog"] = v
    v = get("video", "lod")
    if v in ("full", "original"):
        s["lod"] = v
    v = get("video", "shader")
    if v is not None:
        s["shader"] = v
    v = num("video", "shader_scale")
    if v is not None:
        s["shader_res"] = "game" if v else "display"
    v = num("textures", "replace")
    if v is not None:
        s["texpack"] = "on" if v else "off"
    v = num("controls", "camera_speed_x")
    if v is not None:
        s["cam_speed"] = str(v)
    for key in ("invert_x", "invert_y"):
        v = num("controls", key)
        if v is not None:
            s[key] = "on" if v else "off"
    v = num("controls", "mouse_keyboard")
    if v == 0:
        s["mouse"] = "off"
    elif v is not None:
        sp = num("controls", "mouse_speed")
        s["mouse"] = str(sp) if sp else "100"
    v = get("controls", "reticle_color")
    if v:
        s["reticle"] = v.lstrip("#").upper()


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
    exe = os.environ.get("MML_EXE", "MEGAMAN.EXE")   # dev trees test other builds
    return subprocess.Popen([os.path.join(GAME_DIR, exe)], cwd=GAME_DIR,
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


def reticle_hex(s):
    v = str(s.get("reticle", "")).lstrip("#").upper()
    return v if len(v) == 6 and all(c in "0123456789ABCDEF" for c in v) else DEFAULTS["reticle"]


def controls_ini(s):
    roll_speed, roll_anim = ROLL_STYLES.get(s["roll"], ROLL_STYLES["fast"])
    speed = int(s["cam_speed"]) if str(s["cam_speed"]).isdigit() else 100
    return {
        "mode": s["controls"],
        "input": s["input"],
        "orbit_start": 1 if s["orbit_start"] == "on" else 0,
        "camera_speed_x": speed,
        "camera_speed_y": speed,
        "invert_x": 1 if s["invert_x"] == "on" else 0,
        "invert_y": 1 if s["invert_y"] == "on" else 0,
        "deadzone": int(s["deadzone"]) if str(s["deadzone"]).isdigit() else 24,
        "roll_speed": roll_speed,
        "roll_anim": roll_anim,
        "walk_tilt": 55 if s["walk"] == "on" else 0,
        "reticle_color": reticle_hex(s),
        "mouse_keyboard": 0 if s["mouse"] == "off" else 1,
        "mouse_speed": int(s["mouse"]) if str(s["mouse"]).isdigit() else 100,
        "mouse_invert_y": 1 if s["invert_y"] == "on" else 0,
        **{k: (s.get(k) if s.get(k) in dict(ACTIONS) else v) for k, v in BUTTON_DEFAULTS.items()},
        **{k: (str(s.get(k)) if s.get(k) is not None else v) for k, v in KEY_DEFAULTS.items()},
    }


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
            "exclusive": 1 if s["window"] == "exclusive" else 0,
            "render_scale": scale,
            "nearest_upscale": 1 if s["scalefilter"] == "nearest" else 0,
            "msaa": aa,
            "anisotropy": aniso,
            "fog": s["fog"],
            "lod": s["lod"],
            "interp": 1 if s["fps"] == "60" else 0,
            "shader": s.get("shader", ""),
            "shader_scale": 1 if s.get("shader_res", "game") == "game" else 0,
        },
        "controls": controls_ini(s),
        "textures": {"replace": 1 if s.get("texpack", "on") == "on" else 0,
                     "dump": 1 if s.get("texdump", "off") == "on" else 0},
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
                b.configure(bg=YELLOW, fg=NAVY, font=("Segoe UI", 10, "bold"))
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
        # Short screens (the Steam Deck's 800 lines, small laptops): PLAY moves under a
        # trimmed artwork in the left column so the whole window fits.
        area = work_area()
        self.h = H
        if area and area[3] - area[1] - TITLE_BAR < H:
            self.h = max(COMPACT_MIN_H, area[3] - area[1] - TITLE_BAR)
        compact = self.h < H
        if compact:
            play_x, play_y = ART_X - 8, self.h - 62 - 40
            foot_x = play_x + 8
            art_bottom = play_y - (56 if FROZEN else 16)
        else:
            play_x, play_y, foot_x, art_bottom = PLAY_X, PLAY_Y, COL_X, ART_Y + 622
        root.geometry(f"{W}x{self.h}+{self.centered_pos(self.h)}")
        try:
            root.iconbitmap(self.res_path("mml.ico"))
        except Exception:
            pass

        global HEAD
        if "Bahnschrift" in tkfont.families(root):
            HEAD = "Bahnschrift"
        c = self.canvas = tk.Canvas(root, width=W, height=self.h, highlightthickness=0, bd=0, bg=BG)
        c.pack()
        self.img_bg = tk.PhotoImage(file=self.res_path("gui_bg.png"))
        c.create_image(0, 0, image=self.img_bg, anchor="nw")

        # the cover art in an in-game style frame: its old border cropped off, the corners cut
        # (on short screens only the top is kept: faces, the Flutter)
        src = tk.PhotoImage(file=self.res_path("gui_art_round.png"))
        aw, ah = src.width() - 6, min(src.height() - 6, art_bottom - ART_Y - 6)
        self.img_art = tk.PhotoImage(width=aw, height=ah)
        self.img_art.tk.call(self.img_art, "copy", src, "-from", 3, 3, 3 + aw, 3 + ah)
        big, small = 40, 20
        for y in range(big):
            for x in range(big - y):
                for px, py in ((aw - 1 - x, y), (x, ah - 1 - y)):   # top right, bottom left
                    self.img_art.tk.call(self.img_art, "transparency", "set", px, py, 1)
                if x < small - y and y < small:   # top left, bottom right
                    self.img_art.tk.call(self.img_art, "transparency", "set", x, y, 1)
                    self.img_art.tk.call(self.img_art, "transparency", "set", aw - 1 - x, ah - 1 - y, 1)
        panel_frame(c, ART_X, ART_Y, ART_X + aw + 6, ART_Y + ah + 6, big + 2, small + 2, fill=NAVY)
        c.create_image(ART_X + 3, ART_Y + 3, image=self.img_art, anchor="nw")

        # title block: a Mega Man blue band with the name, hazard stripes, the subtitle;
        # stacked by real line heights so it doesn't overlap at higher Windows scaling
        f_title = (HEAD, 24, "bold italic")
        ls_title = tkfont.Font(family=HEAD, size=24, weight="bold", slant="italic").metrics("linespace")
        ls_small = tkfont.Font(family="Segoe UI", size=10, weight="bold").metrics("linespace")
        band_y, band_h = 16, ls_title + 8
        band_x0 = COL_X - 12
        band_x1 = band_x0 + tkfont.Font(family=HEAD, size=24, weight="bold", slant="italic").measure(
            "MEGA MAN LEGENDS") + 60
        lean = band_h * 0.5
        gradient(c, band_y, band_y + band_h, lambda y: band_x0,
                 lambda y: band_x1 - lean * (y - band_y) / band_h, BLUE, BLUE_DARK)
        c.create_rectangle(band_x0, band_y + band_h - 3, band_x1 - lean, band_y + band_h, fill=YELLOW, width=0)
        outlined_text(c, COL_X + 2, band_y + 4, "MEGA MAN LEGENDS", f_title, fill="white", outline="#061226")
        sx, sy, sh = band_x1 + 6, band_y + band_h - 12, 12   # hazard stripes after the band
        while sx + 22 <= COL_X + COL_W + 12:
            c.create_polygon(sx + sh, sy, sx + sh + 11, sy, sx + 11, sy + sh, sx, sy + sh, fill=YELLOW, outline="")
            sx += 20
        c.create_text(COL_X + COL_W + 12, band_y + 2, anchor="ne", fill=DIM, font=("Segoe UI", 9),
                      text=f"v{VERSION}")
        sub_y = band_y + band_h + 8
        c.create_text(COL_X, sub_y, text="M O D E R N I Z A T I O N    P R O J E C T", anchor="nw",
                      font=(HEAD, 11, "bold italic"), fill=ACCENT)
        status_y = sub_y + ls_small + 12
        col_y = status_y + 2 * ls_small + 26
        c.create_text(ART_X + 4, 30, anchor="nw", fill=DIM, font=("Segoe UI", 10),
                      text="1997 Capcom PC port · widescreen · modern controls · fixed audio")

        # status lines (canvas texts, colored dynamically)
        self.exe_item = c.create_text(COL_X, status_y, anchor="nw", font=("Segoe UI", 10), fill=DIM, text="…")
        self.pad_item = c.create_text(COL_X, status_y + ls_small + 2, anchor="nw", font=("Segoe UI", 10),
                                      fill=DIM, text="…")

        # options column (frame hosted on the canvas) on an in-game style panel: a tab bar
        # over four pages
        panel_bottom = self.h - 8 if compact else play_y - 16 - (46 if FROZEN else 0) - 4
        panel_frame(c, COL_X - 14, col_y - 14, COL_X + COL_W + 14, panel_bottom, 26, 12, fill=BG)
        col = tk.Frame(root, bg=BG)
        c.create_window(COL_X, col_y, window=col, anchor="nw", width=COL_W)
        tabs = tk.Frame(col, bg=BG)
        tabs.pack(fill="x", pady=(0, 6))
        self.pages = {}
        for key in ("display", "graphics", "controls", "buttons"):
            self.pages[key] = tk.Frame(col, bg=BG)
        page = [self.pages["display"]]

        def option(label, key, choices, hint=None, row=None, side=None):
            card = tk.Frame(row or page[0], bg=CARD, highlightthickness=1, highlightbackground=CARD_EDGE)
            if row:   # half-width card sharing a row with another
                card.pack(side="left", fill="both", expand=True,
                          padx=(0, 2) if side == "left" else (2, 0))
            else:
                card.pack(fill="x", pady=2)
            top = tk.Frame(card, bg=CARD)
            top.pack(fill="x", padx=10, pady=(5, 0))
            tk.Label(top, text=label, bg=CARD, fg=TEXT,
                     font=(HEAD, 11, "bold")).pack(side="left")
            if hint:
                tk.Label(top, text="  " + hint, bg=CARD, fg=DIM,
                         font=("Segoe UI", 9)).pack(side="left")
            Segmented(card, self.settings, key, choices,
                      on_change=lambda: save_settings(self.settings)).pack(
                anchor="w", padx=10, pady=(1, 6))
            return card

        def pair():
            r = tk.Frame(page[0], bg=BG)
            r.pack(fill="x", pady=2)
            return r

        # ---- Display ----
        page[0] = self.pages["display"]
        option("Renderer", "renderer",
               [("Modern (Direct3D 11)", "mml"), ("Compatibility", "ddrawcompat")],
               "Modern = 60 fps, alt-tab, screenshots, windowed")
        option("Display", "window",
               [("Borderless", "fullscreen"), ("Exclusive", "exclusive"), ("Windowed", "windowed")],
               "Exclusive = lowest GPU use, limited Alt+Tab and capture")
        option("Resolution", "res", RES_CHOICES, "Display = your monitor (recommended)")
        r = pair()
        option("Aspect ratio", "aspect", [("16:9 Wide", "16:9"), ("4:3 Classic", "4:3")], row=r, side="left")
        option("Frame rate", "fps", [("60 fps", "60"), ("30 fps", "30")], row=r, side="right")
        option("Intro video", "intro",
               [("Play (any button skips)", "play"), ("Skip entirely", "skip")])

        # ---- Graphics ----
        page[0] = self.pages["graphics"]
        option("Texture filter", "texfilter",
               [("Game default", "app"), ("Trilinear", "trilinear"), ("Anisotropic 16x", "af16x")])
        r = pair()
        option("Upscaling", "scalefilter",
               [("Bilinear", "bilinear"), ("Nearest", "nearest")], row=r, side="left")
        option("Antialiasing", "aa",
               [("Off", "off"), ("4x", "msaa4x"), ("8x", "msaa8x")], row=r, side="right")
        r = pair()
        option("Distance fog", "fog",
               [("Original", "original"), ("Light", "light"), ("Off", "off")], row=r, side="left")
        option("Model detail", "lod",
               [("Full detail", "full"), ("Original", "original")], row=r, side="right")
        self.shader_card(self.pages["graphics"])
        self.texture_card(self.pages["graphics"])

        # ---- Controls ----
        page[0] = self.pages["controls"]
        option("Controls", "controls",
               [("Modern", "modern"), ("Vanilla (1997)", "vanilla")],
               "LT lock-on · RT shoot · X roll · L3/R3 modern on/off")
        r = pair()
        option("Modern controls at start", "orbit_start", [("Off", "off"), ("On", "on")], row=r, side="left")
        option("Controller", "input", [("Any (SDL)", "sdl"), ("Legacy", "legacy")], row=r, side="right")
        option("Camera speed", "cam_speed",
               [("50%", "50"), ("75%", "75"), ("100%", "100"), ("125%", "125"), ("150%", "150"), ("200%", "200")])
        r = pair()
        option("Invert camera X", "invert_x", [("Off", "off"), ("On", "on")], row=r, side="left")
        option("Invert camera Y", "invert_y", [("Off", "off"), ("On", "on")], row=r, side="right")
        r = pair()
        option("Stick deadzone", "deadzone", [("Small", "12"), ("Default", "24"), ("Large", "32")],
               row=r, side="left")
        option("Light tilt", "walk", [("Walks", "on"), ("Runs", "off")], row=r, side="right")
        option("Roll", "roll", [("Classic", "classic"), ("Fast", "fast"), ("Snappy", "snappy")],
               "Classic = the original cartwheel speed")
        option("Mouse & keyboard", "mouse",
               [("Off", "off"), ("50%", "50"), ("75%", "75"), ("100%", "100"), ("150%", "150"), ("200%", "200")],
               "mouse look speed")
        self.reticle_card(page[0])

        # ---- Buttons ----
        self.buttons_page(self.pages["buttons"])

        # tab bar: slanted tabs like the in-game menu's, the open one in yellow
        f_tab = (HEAD, 11, "bold italic")
        th = tkfont.Font(family=HEAD, size=11, weight="bold").metrics("linespace") + 14
        tc = tk.Canvas(tabs, width=COL_W, height=th, bg=BG, highlightthickness=0, bd=0)
        tc.pack(fill="x")
        names = (("Display", "display"), ("Graphics", "graphics"), ("Controls", "controls"), ("Buttons", "buttons"))
        slant, gap = th * 0.4, 6
        tw = (COL_W - slant - gap * (len(names) - 1)) / len(names)
        self.tab_canvas, self.tab_items = tc, {}
        for i, (label, key) in enumerate(names):
            x = i * (tw + gap)
            poly = tc.create_polygon(x + slant, 0, x + tw + slant, 0, x + tw, th, x, th, outline="", tags=key)
            txt = tc.create_text(x + (tw + slant) / 2, th / 2, text=label.upper(), font=f_tab, tags=key)
            tc.tag_bind(key, "<Button-1>", lambda e, k=key: self.show_tab(k))
            tc.tag_bind(key, "<Enter>", lambda e, k=key: self.hover_tab(k, True))
            tc.tag_bind(key, "<Leave>", lambda e, k=key: self.hover_tab(k, False))
            self.tab_items[key] = (poly, txt)
        tc.configure(cursor="hand2")
        self.show_tab(self.settings.get("tab", "display"))

        # PLAY: a slanted Mega Man blue plate with a yellow arrow, brighter on hover
        pw, ph, pl = 468, 62, 22
        self.play_slices = gradient(c, play_y, play_y + ph, lambda y: play_x + pl * (1 - (y - play_y) / ph),
                                    lambda y: play_x + pw - pl * ((y - play_y) / ph), BLUE, BLUE_DARK, tags="play")
        self.play_edge = c.create_polygon(play_x + pl, play_y, play_x + pw, play_y, play_x + pw - pl, play_y + ph,
                                          play_x, play_y + ph, fill="", outline=ACCENT, width=2, tags="play")
        cx, cy = play_x + pw / 2, play_y + ph / 2
        f_play = (HEAD, 20, "bold italic")
        label_w = tkfont.Font(family=HEAD, size=20, weight="bold", slant="italic").measure("P L A Y")
        ax = cx - label_w / 2 - 22
        c.create_polygon(ax - 9, cy - 12, ax + 11, cy, ax - 9, cy + 12, fill=YELLOW, outline=NAVY, width=2,
                         tags="play")
        outlined_text(c, cx + 12, cy, "P L A Y", f_play, fill="white", outline="#061226", tags="play",
                      anchor="center")
        brackets(c, play_x + pl, play_y, play_x + pw - pl, play_y + ph, size=18)

        def play_hover(on):
            top, bottom = ("#3a86f0", "#1546a8") if on else (BLUE, BLUE_DARK)
            for item, t in self.play_slices:
                c.itemconfigure(item, fill=mix(top, bottom, t))
            c.itemconfigure(self.play_edge, outline=YELLOW if on else ACCENT)
        c.tag_bind("play", "<Button-1>", lambda e: self.launch())
        c.tag_bind("play", "<Enter>", lambda e: play_hover(True))
        c.tag_bind("play", "<Leave>", lambda e: play_hover(False))
        # one-click direct-play shortcut, as a proper secondary button (frozen exe only -
        # a shortcut to python.exe --play wouldn't work in the dev tree).
        if FROZEN:
            self.sc_btn = tk.Label(root, text="＋   Create Desktop Shortcut",
                                   bg=SEG_OFF, fg=TEXT, font=("Segoe UI", 10, "bold"),
                                   padx=16, pady=8, cursor="hand2", bd=0)
            c.create_window(foot_x if compact else COL_X, play_y - 16, window=self.sc_btn, anchor="sw")
            self.sc_btn.bind("<Button-1>", lambda e: self.make_shortcut())
            self.sc_btn.bind("<Enter>", lambda e: self.sc_btn.configure(bg=SEG_HOVER, fg="white"))
            self.sc_btn.bind("<Leave>", lambda e: self.sc_btn.configure(bg=SEG_OFF, fg=TEXT))

        self.foot_item = c.create_text(foot_x, play_y + 70, anchor="nw",
                                       font=("Segoe UI", 9), fill=DIM, text="")

        # Does the tallest tab fit above PLAY (or the window's bottom in the compact
        # layout)? If not, main() builds the window again with the text a bit smaller.
        root.update_idletasks()
        tallest = max(p.winfo_reqheight() for p in self.pages.values())
        bottom = self.h - 8 if compact else play_y - 16 - (46 if FROZEN else 0) - 8
        room = bottom - col_y - tabs.winfo_reqheight() - 4
        self.fit_scaling = None
        if tallest > room > 0:
            # padding doesn't shrink with the text: aim a little lower so one rebuild does it
            self.fit_scaling = float(root.tk.call("tk", "scaling")) * room / tallest * 0.98
            return   # main() rebuilds; no status polling for this one

        self.refresh_status()

    def show_tab(self, key):
        if key not in self.pages:
            key = "display"
        for p in self.pages.values():
            p.pack_forget()
        self.pages[key].pack(fill="x")
        self.tab = key
        for k in self.tab_items:
            self.hover_tab(k, False)
        if self.settings.get("tab") != key:
            self.settings["tab"] = key
            save_settings(self.settings)

    def hover_tab(self, key, on):
        poly, txt = self.tab_items[key]
        if key == getattr(self, "tab", None):
            fill, fg = YELLOW, NAVY
        else:
            fill, fg = (SEG_HOVER, TEXT) if on else (SEG_OFF, DIM)
        self.tab_canvas.itemconfigure(poly, fill=fill)
        self.tab_canvas.itemconfigure(txt, fill=fg)

    def buttons_page(self, parent):
        """Controller and keyboard remapping (modern controls), one view at a time."""
        switch = tk.Frame(parent, bg=BG)
        switch.pack(fill="x", pady=(0, 2))
        self.btn_views = {"pad": tk.Frame(parent, bg=BG), "keys": tk.Frame(parent, bg=BG)}
        Segmented(switch, self.settings, "buttons_view", [("Controller", "pad"), ("Keyboard & mouse", "keys")],
                  on_change=self.show_buttons_view).pack(anchor="w")
        self.pad_buttons(self.btn_views["pad"])
        self.key_buttons(self.btn_views["keys"])
        self.show_buttons_view()

    def show_buttons_view(self):
        view = self.settings.get("buttons_view", "pad")
        for k, f in self.btn_views.items():
            f.pack_forget()
        self.btn_views["keys" if view == "keys" else "pad"].pack(fill="x")
        save_settings(self.settings)

    def pad_buttons(self, parent):
        head = tk.Frame(parent, bg=CARD, highlightthickness=1, highlightbackground=CARD_EDGE)
        head.pack(fill="x", pady=2)
        top = tk.Frame(head, bg=CARD)
        top.pack(fill="x", padx=10, pady=(6, 0))
        tk.Label(top, text="Buttons", bg=CARD, fg=TEXT, font=(HEAD, 11, "bold")).pack(side="left")
        reset = tk.Label(top, text="Reset to default", bg=SEG_OFF, fg=TEXT, font=("Segoe UI", 9), padx=10,
                         pady=2, cursor="hand2")
        reset.pack(side="right")
        tk.Label(head, text="Used while modern controls are on. Menus and classic controls keep the "
                            "original buttons. Back + Start opens the settings, Back + RB quick saves.", bg=CARD, fg=DIM, font=("Segoe UI", 9), wraplength=440,
                 justify="left").pack(anchor="w", padx=10, pady=(2, 7))
        reset.bind("<Button-1>", lambda e: self.reset_buttons())
        reset.bind("<Enter>", lambda e: reset.configure(bg=SEG_HOVER))
        reset.bind("<Leave>", lambda e: reset.configure(bg=SEG_OFF))
        self.btn_vars = {}
        names = dict(ACTIONS)
        by_name = {v: k for k, v in ACTIONS}
        grid = tk.Frame(parent, bg=BG)
        grid.pack(fill="x")
        for i, (key, pad, ps) in enumerate(BUTTONS):
            card = tk.Frame(grid, bg=CARD, highlightthickness=1, highlightbackground=CARD_EDGE)
            card.grid(row=i // 2, column=i % 2, sticky="nsew", padx=(0, 2) if i % 2 == 0 else (2, 0), pady=2)
            name = tk.Frame(card, bg=CARD)
            name.pack(side="left", padx=(10, 6), pady=4)
            tk.Label(name, text=pad, bg=CARD, fg=TEXT, font=("Segoe UI", 10, "bold"), width=5,
                     anchor="w").pack(anchor="w")
            tk.Label(name, text=ps.split()[0], bg=CARD, fg=DIM, font=("Segoe UI", 8), anchor="w").pack(
                anchor="w", pady=(0, 2))
            var = tk.StringVar(value=names.get(self.settings.get(key), names[BUTTON_DEFAULTS[key]]))
            self.btn_vars[key] = var
            self.dropdown(card, var, [n for _, n in ACTIONS],
                          lambda v, k=key: self.set_button(k, by_name[v])).pack(
                side="left", fill="x", expand=True, padx=(0, 8), pady=8)
        grid.columnconfigure(0, weight=1, uniform="b")
        grid.columnconfigure(1, weight=1, uniform="b")

    def key_buttons(self, parent):
        """Keyboard + mouse bindings: two keys per action, click one and press the new key."""
        head = tk.Frame(parent, bg=CARD, highlightthickness=1, highlightbackground=CARD_EDGE)
        head.pack(fill="x", pady=2)
        top = tk.Frame(head, bg=CARD)
        top.pack(fill="x", padx=10, pady=(6, 0))
        tk.Label(top, text="Keyboard & mouse", bg=CARD, fg=TEXT, font=(HEAD, 11, "bold")).pack(side="left")
        reset = tk.Label(top, text="Reset to default", bg=SEG_OFF, fg=TEXT, font=("Segoe UI", 9), padx=10,
                         pady=2, cursor="hand2")
        reset.pack(side="right")
        reset.bind("<Button-1>", lambda e: self.reset_keys())
        reset.bind("<Enter>", lambda e: reset.configure(bg=SEG_HOVER))
        reset.bind("<Leave>", lambda e: reset.configure(bg=SEG_OFF))
        tk.Label(head, text="Click a key, then press the new key or mouse button (Esc cancels, Delete clears). "
                            "Mouse look and the wheel (next target) are fixed. While these are used the "
                            "game's own keyboard keys rest; menus keep them. F1 opens the settings, F6 quick saves.",
                 bg=CARD, fg=DIM, font=("Segoe UI", 9), wraplength=440, justify="left").pack(
            anchor="w", padx=10, pady=(2, 7))
        grid = tk.Frame(parent, bg=BG)
        grid.pack(fill="x")
        self.key_btns = {}
        for i, (key, label) in enumerate(KEY_SLOTS):
            card = tk.Frame(grid, bg=CARD, highlightthickness=1, highlightbackground=CARD_EDGE)
            card.grid(row=i // 2, column=i % 2, sticky="nsew", padx=(0, 2) if i % 2 == 0 else (2, 0), pady=2)
            tk.Label(card, text=label, bg=CARD, fg=TEXT, font=("Segoe UI", 9, "bold"), width=12,
                     anchor="w").pack(side="left", padx=(10, 2), pady=7)
            for slot in (0, 1):
                b = tk.Label(card, text="", bg=SEG_OFF, fg=TEXT, font=("Segoe UI", 9), width=7 if slot == 0 else 6,
                             pady=3, cursor="hand2")
                b.pack(side="left", padx=(0, 4 if slot == 0 else 8))
                b.bind("<Button-1>", lambda e, k=key, sl=slot: self.capture_key(k, sl))
                self.key_btns[(key, slot)] = b
        grid.columnconfigure(0, weight=1, uniform="k")
        grid.columnconfigure(1, weight=1, uniform="k")
        self.refresh_keys()

    def key_pair(self, key):
        parts = [p.strip().upper() for p in str(self.settings.get(key, KEY_DEFAULTS[key])).split(",")]
        return (parts + ["", ""])[:2]

    def refresh_keys(self):
        for (key, slot), b in self.key_btns.items():
            b.configure(text=key_label(self.key_pair(key)[slot]), bg=SEG_OFF, fg=TEXT)

    def set_key(self, key, slot, name):
        pair = self.key_pair(key)
        pair[slot] = name
        if pair[0] == "" and pair[1]:
            pair = [pair[1], ""]
        self.settings[key] = ",".join(p for p in pair if p)
        save_settings(self.settings)

    def capture_key(self, key, slot):
        b = self.key_btns[(key, slot)]
        b.configure(text="press…", bg=YELLOW, fg=NAVY)
        root = self.root

        def done():
            root.unbind("<KeyPress>")
            for n in (1, 2, 3):
                root.unbind(f"<ButtonPress-{n}>")
            self.refresh_keys()

        def on_key(e):
            if e.keysym == "Escape":
                return done()
            if e.keysym == "Delete":
                self.set_key(key, slot, "")
                return done()
            name = tk_key_name(e.keysym)
            if name:
                self.set_key(key, slot, name)
                done()

        def on_mouse(e):
            self.set_key(key, slot, {1: "MOUSE1", 3: "MOUSE2", 2: "MOUSE3"}.get(e.num, "MOUSE1"))
            done()
            return "break"
        # the click that started the capture must not count: arm the mouse a moment later
        root.bind("<KeyPress>", on_key)
        root.after(250, lambda: [root.bind(f"<ButtonPress-{n}>", on_mouse) for n in (1, 2, 3)])

    def reset_keys(self):
        for key, v in KEY_DEFAULTS.items():
            self.settings[key] = v
        save_settings(self.settings)
        self.refresh_keys()

    def dropdown(self, parent, var, choices, on_pick):
        """Flat dropdown in the launcher's style: a label showing var, a menu on click."""
        box = tk.Frame(parent, bg=SEG_OFF, cursor="hand2")
        arrow = tk.Label(box, text="▾", bg=SEG_OFF, fg=DIM, font=("Segoe UI", 10), padx=8, cursor="hand2")
        arrow.pack(side="right")   # first, so the text can't squeeze it out
        text = tk.Label(box, textvariable=var, bg=SEG_OFF, fg=TEXT, font=("Segoe UI", 10), anchor="w",
                        padx=9, pady=4, cursor="hand2", width=12)
        text.pack(side="left", fill="x", expand=True)
        menu = tk.Menu(box, tearoff=0, bg=CARD_HI, fg=TEXT, activebackground=BLUE, activeforeground="white",
                       bd=0, font=("Segoe UI", 10))
        for c in choices:
            menu.add_command(label=c, command=lambda c=c: (var.set(c), on_pick(c)))

        def show(e):
            menu.tk_popup(box.winfo_rootx(), box.winfo_rooty() + box.winfo_height())

        def hover(on):
            for w in (box, text, arrow):
                w.configure(bg=SEG_HOVER if on else SEG_OFF)
        for w in (box, text, arrow):
            w.bind("<Button-1>", show)
            w.bind("<Enter>", lambda e: hover(True))
            w.bind("<Leave>", lambda e: hover(False))
        return box

    def set_button(self, key, action):
        self.settings[key] = action
        save_settings(self.settings)

    def reset_buttons(self):
        names = dict(ACTIONS)
        for key, action in BUTTON_DEFAULTS.items():
            self.settings[key] = action
            self.btn_vars[key].set(names[action])
        save_settings(self.settings)

    def reticle_card(self, parent):
        """Lock-on reticle colour: preset swatches plus the Windows colour dialog."""
        card = tk.Frame(parent, bg=CARD, highlightthickness=1, highlightbackground=CARD_EDGE)
        card.pack(fill="x", pady=2)
        top = tk.Frame(card, bg=CARD)
        top.pack(fill="x", padx=10, pady=(5, 0))
        tk.Label(top, text="Lock-on reticle", bg=CARD, fg=TEXT, font=(HEAD, 11, "bold")).pack(side="left")
        tk.Label(top, text="  marker on the locked enemy", bg=CARD, fg=DIM,
                 font=("Segoe UI", 9)).pack(side="left")
        row = tk.Frame(card, bg=CARD)
        row.pack(anchor="w", padx=10, pady=(3, 7))
        self.ret_swatches = {}
        for name, hexcol in RETICLE_PRESETS:
            sw = tk.Label(row, width=3, bg="#" + hexcol, cursor="hand2", bd=0, highlightthickness=2)
            sw.pack(side="left", padx=(0, 5), ipady=3)
            sw.bind("<Button-1>", lambda e, h=hexcol: self.set_reticle(h))
            self.ret_swatches[hexcol] = sw
        self.ret_custom = tk.Label(row, text="Custom…", bg=SEG_OFF, fg=TEXT, font=("Segoe UI", 9),
                                   padx=9, pady=3, cursor="hand2", highlightthickness=2)
        self.ret_custom.pack(side="left", padx=(4, 0))
        self.ret_custom.bind("<Button-1>", lambda e: self.custom_reticle())
        self.refresh_reticle()

    def shader_card(self, parent):
        """RetroArch shader preset (.slangp) for the final picture: CRT, scanlines, NTSC..."""
        card = tk.Frame(parent, bg=CARD, highlightthickness=1, highlightbackground=CARD_EDGE)
        card.pack(fill="x", pady=2)
        top = tk.Frame(card, bg=CARD)
        top.pack(fill="x", padx=10, pady=(5, 0))
        tk.Label(top, text="Shader", bg=CARD, fg=TEXT, font=(HEAD, 11, "bold")).pack(side="left")
        tk.Label(top, text="  RetroArch presets (.slangp): CRT, scanlines, NTSC...", bg=CARD, fg=DIM,
                 font=("Segoe UI", 9)).pack(side="left")
        row = tk.Frame(card, bg=CARD)
        row.pack(fill="x", padx=10, pady=(3, 2))

        def button(text, action):
            b = tk.Label(row, text=text, bg=SEG_OFF, fg=TEXT, font=("Segoe UI", 9), padx=9, pady=3, cursor="hand2")
            b.pack(side="left", padx=(0, 4))
            b.bind("<Button-1>", lambda e: action())
            return b
        self.shader_off = button("Off", lambda: self.set_shader(""))
        self.shader_pick = button("Choose preset…", self.choose_shader)
        button("Open folder", self.open_shader_folder)
        Segmented(card, self.settings, "shader_res", [("Game resolution", "game"), ("Display", "display")],
                  on_change=lambda: save_settings(self.settings)).pack(anchor="w", padx=10, pady=(2, 2))
        tk.Label(card, text="Game resolution suits CRT presets. Presets aren't included: get RetroArch's "
                            "slang-shaders and put them in Modernization\\shaders.",
                 bg=CARD, fg=DIM, font=("Segoe UI", 8), wraplength=440, justify="left").pack(
            anchor="w", padx=10, pady=(0, 6))
        self.refresh_shader()

    def texture_card(self, parent):
        """HD texture packs (Modernization/textures) and dumping the originals for pack makers."""
        card = tk.Frame(parent, bg=CARD, highlightthickness=1, highlightbackground=CARD_EDGE)
        card.pack(fill="x", pady=2)
        top = tk.Frame(card, bg=CARD)
        top.pack(fill="x", padx=10, pady=(5, 0))
        tk.Label(top, text="Textures", bg=CARD, fg=TEXT, font=(HEAD, 11, "bold")).pack(side="left")
        tk.Label(top, text="  HD packs: PNGs in Modernization\\textures", bg=CARD, fg=DIM,
                 font=("Segoe UI", 9)).pack(side="left")
        row = tk.Frame(card, bg=CARD)
        row.pack(fill="x", padx=10, pady=(3, 7))
        tk.Label(row, text="HD pack", bg=CARD, fg=DIM, font=("Segoe UI", 9)).pack(side="left", padx=(0, 4))
        Segmented(row, self.settings, "texpack", [("On", "on"), ("Off", "off")],
                  on_change=lambda: save_settings(self.settings)).pack(side="left")
        tk.Label(row, text="   Dump originals", bg=CARD, fg=DIM, font=("Segoe UI", 9)).pack(side="left", padx=(0, 4))
        Segmented(row, self.settings, "texdump", [("Off", "off"), ("On", "on")],
                  on_change=lambda: save_settings(self.settings)).pack(side="left")
        b = tk.Label(row, text="Open folder", bg=SEG_OFF, fg=TEXT, font=("Segoe UI", 9), padx=9, pady=3,
                     cursor="hand2")
        b.pack(side="right")
        b.bind("<Button-1>", lambda e: os.startfile(self.texture_dir()))

    def texture_dir(self):
        d = os.path.join(GAME_DIR or BASE, "Modernization", "textures")
        os.makedirs(d, exist_ok=True)
        return d

    def shader_dir(self):
        d = os.path.join(GAME_DIR or BASE, "Modernization", "shaders")
        os.makedirs(d, exist_ok=True)
        return d

    def refresh_shader(self):
        cur = self.settings.get("shader", "")
        self.shader_off.configure(bg=YELLOW if not cur else SEG_OFF, fg=NAVY if not cur else TEXT)
        name = os.path.splitext(os.path.basename(cur))[0] if cur else ""
        if len(name) > 30:
            name = name[:29] + "…"
        self.shader_pick.configure(text=name or "Choose preset…", bg=YELLOW if cur else SEG_OFF,
                                   fg="white" if cur else TEXT)

    def set_shader(self, path):
        if path and GAME_DIR:   # inside the game folder: keep it relative, so the folder can move
            try:
                rel = os.path.relpath(path, GAME_DIR)
                if not rel.startswith(".."):
                    path = rel
            except ValueError:
                pass
        self.settings["shader"] = path
        save_settings(self.settings)
        self.refresh_shader()

    def choose_shader(self):
        from tkinter import filedialog
        path = filedialog.askopenfilename(parent=self.root, title="RetroArch shader preset",
                                          initialdir=self.shader_dir(),
                                          filetypes=[("Shader presets", "*.slangp"), ("All files", "*.*")])
        if path:
            self.set_shader(os.path.normpath(path))

    def open_shader_folder(self):
        os.startfile(self.shader_dir())

    def refresh_reticle(self):
        cur = reticle_hex(self.settings)
        for hexcol, sw in self.ret_swatches.items():
            sw.configure(highlightbackground=YELLOW if hexcol == cur else CARD)
        custom = cur not in self.ret_swatches
        self.ret_custom.configure(highlightbackground=YELLOW if custom else CARD,
                                  bg=("#" + cur) if custom else SEG_OFF,
                                  fg="black" if custom else TEXT)

    def set_reticle(self, hexcol):
        self.settings["reticle"] = hexcol.lstrip("#").upper()
        save_settings(self.settings)
        self.refresh_reticle()

    def custom_reticle(self):
        from tkinter import colorchooser
        rgb, hexcol = colorchooser.askcolor(color="#" + reticle_hex(self.settings), parent=self.root,
                                            title="Lock-on reticle colour")
        if hexcol:
            self.set_reticle(hexcol)

    @staticmethod
    def centered_pos(h):
        """Centre in the screen's work area (above the taskbar), never above its top."""
        area = work_area()
        if not area:
            return "100+40"
        left, top, right, bottom = area
        x = left + max(0, (right - left - W) // 2)
        y = top + max(0, (bottom - top - h - TITLE_BAR) // 2)
        return f"{x}+{y}"

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
            text="Launched!  F5 or L3/R3 = modern controls on/off · any button skips the intro")
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
    app = App(root)
    for _ in range(3):   # small screen at high Windows scaling: shrink the text until it fits
        if not app.fit_scaling:
            break
        scaling = app.fit_scaling
        root.destroy()
        root = tk.Tk()
        root.tk.call("tk", "scaling", scaling)
        app = App(root)
    if app.fit_scaling:   # still a hair too tall: keep this one anyway
        app.refresh_status()
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
