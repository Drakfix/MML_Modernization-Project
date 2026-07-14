"""
Command-line launcher front-end (fallback for the graphical launcher).

Prompts for render resolution, aspect ratio and control scheme, writes the matching
DDrawCompat.ini, and runs launch_widescreen_freecam.py with the corresponding environment.
Every choice is applied at runtime; the game files are not modified.
"""
import os
import sys
import subprocess

HERE = os.path.dirname(os.path.abspath(__file__))


def _find_game_dir():
    """Game folder = MML_GAME_DIR env override, else walk up from this script (dev:
    RE-notes/tools/, release: Modernization/), else the current directory."""
    env = os.environ.get("MML_GAME_DIR")
    if env and os.path.exists(os.path.join(env, "MEGAMAN.EXE")):
        return os.path.abspath(env)
    p = HERE
    for _ in range(4):
        if os.path.exists(os.path.join(p, "MEGAMAN.EXE")):
            return p
        p = os.path.dirname(p)
    if os.path.exists(os.path.join(os.getcwd(), "MEGAMAN.EXE")):
        return os.getcwd()
    print("ERROR: MEGAMAN.EXE not found - run from the game folder or set MML_GAME_DIR.")
    sys.exit(1)


GAME_DIR = _find_game_dir()
LAUNCHER = os.path.join(HERE, "launch_widescreen_freecam.py")
INI_BASE = os.path.join(GAME_DIR, "DDrawCompat.ini.16to9")   # template we edit from
INI_OUT = os.path.join(GAME_DIR, "DDrawCompat.ini")

# ---- option tables: key -> (menu label, value) --------------------------------
# Render resolution -> DDrawCompat ResolutionScale. Multipliers are of the game's
# 640x480 logical surface; higher-than-display = supersampling (crisper, heavier).
RES_OPTIONS = [
    ("d", "Display  - match your monitor (recommended)", "display"),
    ("1", "1x       - 640x480  (native, sharp pixels)",   "640x480"),
    ("2", "2x       - 1280x960",                          "1280x960"),
    ("3", "3x       - 1920x1440",                         "1920x1440"),
    ("4", "4x       - 2560x1920",                         "2560x1920"),
    ("5", "5x       - 3200x2400  (heavy supersampling)",  "3200x2400"),
]
ASPECT_OPTIONS = [
    ("1", "16:9 widescreen (anamorphic)", ("display", "16:9")),
    ("2", "4:3 classic (pillarboxed)",    ("4:3", "4:3")),
]
CONTROL_OPTIONS = [
    ("1", "Modernized - twin-stick camera, camera-relative move, LT-aim/RT-shoot/X-dodge", "modern"),
    ("2", "Vanilla    - stock controls (widescreen + timing fix still apply)",             "vanilla"),
]
INTRO_OPTIONS = [
    ("1", "Play the intro video (any pad button / key / click skips it)", "play"),
    ("2", "Skip it entirely (straight to the title screen)",              "skip"),
]


def ask(title, options, default_key):
    print("\n" + title)
    for key, label, _ in options:
        tag = " (default)" if key == default_key else ""
        print(f"   [{key}] {label}{tag}")
    while True:
        choice = input(f"Select [{default_key}]: ").strip().lower()
        if choice == "":
            choice = default_key
        for key, _, value in options:
            if choice == key:
                return value
        print("   ! not a valid option, try again.")


def write_ini(resolution_scale, display_aspect):
    if not os.path.exists(INI_BASE):
        print(f"ERROR: template not found: {INI_BASE}")
        sys.exit(1)
    out = []
    saw_res = saw_asp = False
    with open(INI_BASE, "r", encoding="utf-8", errors="replace") as f:
        for line in f:
            # key = text before '=', so we match ResolutionScale exactly (NOT
            # ResolutionScaleFilter) and DisplayAspectRatio exactly.
            key = line.split("=", 1)[0].strip().lower() if "=" in line else ""
            if key == "resolutionscale":
                out.append(f"ResolutionScale         = {resolution_scale}\n")
                saw_res = True
            elif key == "displayaspectratio":
                out.append(f"DisplayAspectRatio      = {display_aspect}\n")
                saw_asp = True
            else:
                out.append(line)
    if not saw_res:
        out.append(f"ResolutionScale         = {resolution_scale}\n")
    if not saw_asp:
        out.append(f"DisplayAspectRatio      = {display_aspect}\n")
    with open(INI_OUT, "w", encoding="utf-8") as f:
        f.writelines(out)


def main():
    print("=" * 66)
    print("  MEGA MAN LEGENDS - Modernization Project  (launch options)")
    print("=" * 66)

    res_scale = ask("Render resolution:", RES_OPTIONS, "d")
    disp_aspect, aspect_tag = ask("Aspect ratio:", ASPECT_OPTIONS, "1")
    controls = ask("Controls:", CONTROL_OPTIONS, "1")
    intro = ask("Intro video:", INTRO_OPTIONS, "1")

    write_ini(res_scale, disp_aspect)

    print("\n" + "-" * 66)
    print(f"  Resolution : {res_scale}")
    print(f"  Aspect     : {aspect_tag}")
    print(f"  Controls   : {controls}")
    print(f"  Intro      : {'plays (any input skips)' if intro == 'play' else 'bypassed'}")
    print("-" * 66)
    if controls == "modern":
        print("  F5 or click L3/R3 toggles the orbit camera (starts OFF).")
        print("  LT=aim  RT=shoot  X=dodge roll  A=jump  left stick=move  right stick=camera")
    print("  Screenshots: Win+Alt+PrtScn (Xbox Game Bar) -> Videos\\Captures")
    print("  Keep this window open while playing. Closing it (or Ctrl+C) removes the")
    print("  mods; the game keeps running as vanilla - quit from the game itself.")
    print("-" * 66 + "\n")

    env = dict(os.environ)
    env["MML_ASPECT"] = aspect_tag        # "16:9" or "4:3"
    env["MML_CONTROLS"] = controls        # "modern" or "vanilla"
    env["MML_INTRO"] = intro              # "play" (skippable) or "skip" (bypass)
    env.setdefault("MML_WINDOW", "fullscreen")

    try:
        subprocess.run([sys.executable, LAUNCHER], cwd=GAME_DIR, env=env)
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
