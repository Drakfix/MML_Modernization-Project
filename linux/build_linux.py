"""Builds the Linux version (experimental): linux/build/mml-linux, a folder with the program, SDL3 and
the bundled files, and linux/build/mml-linux-<version>-x86_64.tar.gz of it. Run on Linux (or WSL):

    python3 linux/build_linux.py

The native code is compiled with clang for x86_64: the runtime, our mods and the recompiled game
(native/build/gen, made on Windows by tools/recomp). Fonts come from the system (fontconfig and
FreeType), Vulkan from the system's driver, through SDL.

Needs (environment variables override the defaults):
  SDL3_LINUX  SDL3 built for Linux, installed (~/mml-linux-deps/SDL3): include/ and lib/libSDL3.so.0
  SDL3_SRC    SDL3's source (mml-deps/SDL3-3.4.18 next to the repo): the Vulkan headers it carries
  MML_INTRO   our intro video to bundle (else MML_GAME_DIR/Modernization/intro.mpg)
  MML_LINUX_OUT  where to build (linux/build)
and clang, glslangValidator and spirv-opt (glslang-tools, spirv-tools), and the development files
of FreeType and fontconfig (libfreetype-dev, libfontconfig-dev).
"""
import os
import re
import shutil
import subprocess
import sys
import tarfile
from concurrent.futures import ThreadPoolExecutor

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
DEPS = os.path.join(os.path.dirname(REPO), "mml-deps")
NATIVE = os.path.join(REPO, "native")
sys.path.insert(0, NATIVE)
from patch_data import patch_data  # noqa: E402

SDL_LIB = os.environ.get("SDL3_LINUX") or os.path.expanduser("~/mml-linux-deps/SDL3")
SDL_SRC = os.environ.get("SDL3_SRC") or os.path.join(DEPS, "SDL3-3.4.18")
OUT = os.environ.get("MML_LINUX_OUT") or os.path.join(HERE, "build")
GEN = os.path.join(NATIVE, "build", "gen")        # the recompiled game (never committed)
GEN_JP = os.path.join(NATIVE, "build", "gen_jp")  # the Japanese program's, when it was made
CLANG = os.environ.get("CC") or "clang"
# Windows-only sources (as for Android): Direct3D 11 and its shader runtime, GDI fonts, the Windows
# platform and window placement, debugging tools; recomp_native.c compiles to nothing here
WINDOWS_ONLY = {"gfx_d3d11.c", "rshader.c", "ui_font_gdi.c", "plat_win32.c", "gfx_win.c", "iat.c",
                "apicensus.c", "sampler.c", "watch.c", "kbm.c", "recomp_native.c"}
FLAGS = ["-O2", "-D_GNU_SOURCE", "-fms-extensions", "-DMML_RUNNER", "-DMML_VULKAN", "-DMML_PATCH_DATA"]


def run(*cmd):
    print(os.path.basename(cmd[0]), *[os.path.basename(c) if os.path.isabs(c) else c for c in cmd[1:8]],
          "..." if len(cmd) > 8 else "")
    subprocess.run(list(cmd), check=True)


def pkg(*args):
    return subprocess.run(["pkg-config", *args], check=True, capture_output=True, text=True).stdout.split()


def version():
    """the version in mml.h (MML_NATIVE_BUILD "0.7.2-beta ..."), for the archive's name"""
    text = open(os.path.join(NATIVE, "src", "mml.h"), encoding="utf-8").read()
    m = re.search(r'#define\s+MML_NATIVE_BUILD\s+"([0-9][^" ]*)', text)
    return m.group(1) if m else "dev"


def newer(src, obj):
    return not os.path.exists(obj) or os.path.getmtime(src) > os.path.getmtime(obj)


def compile_many(jobs):
    """(source, object, extra flags) in parallel; stops at the first failure"""
    def one(job):
        src, obj, extra = job
        return src, subprocess.run([CLANG, "-c", *FLAGS, *extra, src, "-o", obj], capture_output=True, text=True)

    failed = []
    with ThreadPoolExecutor(max_workers=os.cpu_count() or 4) as pool:
        for src, r in pool.map(one, jobs):
            if r.returncode:
                failed.append(src)
            if r.stderr.strip():
                print(r.stderr)
    if failed:
        sys.exit("failed: " + ", ".join(os.path.basename(f) for f in failed))


def expand(path):
    """a GLSL file with its #include "file" lines replaced by the files (relative to it)"""
    out = []
    for line in open(path, encoding="utf-8"):
        m = re.match(r'\s*#include\s+"([^"]+)"', line)
        out.append(expand(os.path.join(os.path.dirname(path), m.group(1))) if m else line)
    return "".join(out)


def shaders(inc):
    """the Vulkan backend's GLSL to SPIR-V, as C initializers (glslc -mfmt=c does it on Windows)"""
    d = os.path.join(inc, "shaders")
    os.makedirs(d, exist_ok=True)
    for name in ("game", "blit", "ui"):
        for stage in ("vert", "frag"):
            spv = os.path.join(d, "%s_%s.spv" % (name, stage))
            src = os.path.join(d, "%s.%s" % (name, stage))   # (its #includes put in: glslc does that itself)
            with open(src, "w") as f:
                f.write(expand(os.path.join(NATIVE, "shaders", "vk", "%s.%s" % (name, stage))))
            run("glslangValidator", "-V", "--target-env", "vulkan1.1", "-o", spv, src)
            run("spirv-opt", "-O", spv, "-o", spv)
            data = open(spv, "rb").read()
            words = ["0x%08x" % int.from_bytes(data[i:i + 4], "little") for i in range(0, len(data), 4)]
            with open(os.path.join(d, "vk_%s_%s.inc" % (name, stage)), "w") as f:
                f.write("{" + ",\n".join(",".join(words[i:i + 8]) for i in range(0, len(words), 8)) + "}\n")


def program(pkgdir):
    """the program: the runtime, our mods and the recompiled game"""
    inc = os.path.join(OUT, "include")
    obj = os.path.join(OUT, "obj")
    gen_obj = os.path.join(OUT, "gen_obj")
    gen_obj_jp = os.path.join(OUT, "gen_obj_jp")
    for d in (inc, obj, gen_obj, gen_obj_jp):
        os.makedirs(d, exist_ok=True)
    if not os.path.exists(os.path.join(GEN, "recomp_table.c")):
        sys.exit("no recompiled code in native/build/gen (tools/recomp/README.md)")
    shaders(inc)
    patch_data(inc)
    src_dir = os.path.join(NATIVE, "src")
    flags = ["-Wall", "-Wno-unused-parameter", "-Wno-unused-function", "-Wno-missing-braces",
             "-I", os.path.join(NATIVE, "compat"), "-I", inc, "-I", src_dir,
             "-I", os.path.join(SDL_LIB, "include"), "-I", os.path.join(SDL_SRC, "src", "video", "khronos"),
             *pkg("--cflags", "freetype2", "fontconfig")]
    have_jp = os.path.exists(os.path.join(GEN_JP, "recomp_table.c"))
    if have_jp:   # the runtime picks the code for the game it was given
        flags.append("-DRECOMP_HAVE_JP")
        print("with the Japanese program's recompiled code (native/build/gen_jp)")
    sources = sorted(f for f in os.listdir(src_dir) if f.endswith(".c") and f not in WINDOWS_ONLY)
    jobs = [(os.path.join(src_dir, f), os.path.join(obj, f[:-2] + ".o"), flags) for f in sources]
    print("compiling %d sources" % len(jobs))
    compile_many(jobs)
    gen_jobs = []
    for gdir, odir in ((GEN, gen_obj), (GEN_JP, gen_obj_jp)) if have_jp else ((GEN, gen_obj),):
        for f in sorted(os.listdir(gdir)):
            if f.endswith(".c"):
                o = os.path.join(odir, f[:-2] + ".o")
                if newer(os.path.join(gdir, f), o):
                    gen_jobs.append((os.path.join(gdir, f), o, ["-w", "-I", src_dir]))
    if gen_jobs:
        print("compiling %d generated files (the recompiled game)" % len(gen_jobs))
        compile_many(gen_jobs)
    objs = [j[1] for j in jobs] + [os.path.join(gen_obj, f) for f in sorted(os.listdir(gen_obj))]
    if have_jp:
        objs += [os.path.join(gen_obj_jp, f) for f in sorted(os.listdir(gen_obj_jp))]
    rsp = os.path.join(OUT, "objects.rsp")   # (too many for one command line)
    with open(rsp, "w") as f:
        f.write("\n".join('"%s"' % o for o in objs))
    libdir = os.path.join(pkgdir, "lib")
    os.makedirs(libdir, exist_ok=True)
    # SDL3 next to the program (lib/), found there through the rpath; the system's own libraries
    # (fontconfig, FreeType, libm) as every desktop has them
    run(CLANG, "-o", os.path.join(pkgdir, "mml"), "@" + rsp, "-L", os.path.join(SDL_LIB, "lib"), "-lSDL3",
        *pkg("--libs", "freetype2", "fontconfig"), "-lm", "-ldl", "-lpthread", "-Wl,-rpath,$ORIGIN/lib",
        "-Wl,--as-needed")
    shutil.copy(os.path.join(SDL_LIB, "lib", "libSDL3.so.0"), libdir)


def assets(pkgdir):
    """the bundled files: our default mml.ini, our intro, the licences (none of the game's files)"""
    d = os.path.join(pkgdir, "assets")
    os.makedirs(os.path.join(d, "licenses"), exist_ok=True)
    shutil.copyfile(os.path.join(REPO, "config", "mml.ini"), os.path.join(d, "mml.ini"))
    shutil.copyfile(os.path.join(REPO, "LICENSE"), os.path.join(d, "licenses", "LICENSE.txt"))
    shutil.copyfile(os.path.join(REPO, "THIRD-PARTY-NOTICES.txt"), os.path.join(d, "licenses", "THIRD-PARTY-NOTICES.txt"))
    for f in ("SDL3-LICENSE.txt", "pl_mpeg-LICENSE.txt"):
        shutil.copyfile(os.path.join(REPO, "licenses", f), os.path.join(d, "licenses", f))
    intro = os.environ.get("MML_INTRO") or os.path.join(os.environ.get("MML_GAME_DIR", ""), "Modernization", "intro.mpg")
    if os.path.isfile(intro):
        shutil.copyfile(intro, os.path.join(d, "intro.mpg"))
    else:
        print("WARNING: no intro.mpg (MML_INTRO or MML_GAME_DIR): the game shows its own intro")


def main():
    pkgdir = os.path.join(OUT, "mml-linux")
    shutil.rmtree(pkgdir, ignore_errors=True)
    os.makedirs(pkgdir)
    program(pkgdir)
    assets(pkgdir)
    archive = os.path.join(OUT, "mml-linux-%s-x86_64.tar.gz" % version())
    with tarfile.open(archive, "w:gz") as t:
        t.add(pkgdir, arcname="mml-linux")
    print("built", pkgdir)
    print("archive", archive)


if __name__ == "__main__":
    main()
