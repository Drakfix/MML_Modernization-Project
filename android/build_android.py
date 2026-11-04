"""Builds the Android app (android/build/mml.apk) with the Android SDK's own tools, no Gradle:
the native code with the NDK's clang (arm64-v8a), SDL3's Java glue and ours with javac and d8,
the manifest and resources with aapt2, then zipalign and apksigner (a debug key made on first use).

    python android/build_android.py [--install]

Needs (defaults in parentheses; environment variables override them):
  ANDROID_SDK   the Android SDK (%LOCALAPPDATA%\\Android\\Sdk), with build-tools, platforms and ndk
  JAVA_HOME     a JDK 17 or 21 (Android Studio's own, jbr)
  SDL3_SRC      SDL3's source (mml-deps\\SDL3-3.4.18 next to the repo): its Java glue and headers
  SDL3_ANDROID  SDL3 built for Android (mml-deps\\SDL3-android): <abi>\\libSDL3.so
  MML_KEYSTORE  the signing key (mml-deps\\android\\debug.keystore, made if missing)
--install puts the app on the connected device with adb.
"""
import os
import shutil
import subprocess
import sys
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
DEPS = os.path.join(os.path.dirname(REPO), "mml-deps")
ABI = "arm64-v8a"
MIN_SDK = 28   # Android 9: Vulkan 1.1
TARGET_SDK = 35


def env(name, default):
    return os.environ.get(name) or default


SDK = env("ANDROID_SDK", os.path.join(os.environ.get("LOCALAPPDATA", ""), "Android", "Sdk"))
JDK = env("JAVA_HOME", r"C:\Program Files\Android\Android Studio\jbr")
SDL_SRC = env("SDL3_SRC", os.path.join(DEPS, "SDL3-3.4.18"))
SDL_LIB = env("SDL3_ANDROID", os.path.join(DEPS, "SDL3-android"))
KEYSTORE = env("MML_KEYSTORE", os.path.join(DEPS, "android", "debug.keystore"))
OUT = os.path.join(HERE, "build")


def newest(path):
    return os.path.join(path, sorted(os.listdir(path))[-1])


BUILD_TOOLS = newest(os.path.join(SDK, "build-tools"))
NDK = newest(os.path.join(SDK, "ndk"))
ANDROID_JAR = os.path.join(SDK, "platforms", "android-%d" % TARGET_SDK, "android.jar")
CLANG = os.path.join(NDK, "toolchains", "llvm", "prebuilt", "windows-x86_64", "bin",
                     "aarch64-linux-android%d-clang.cmd" % MIN_SDK)


def run(*cmd):
    print(os.path.basename(cmd[0]), *[os.path.basename(c) if os.path.isabs(c) else c for c in cmd[1:8]],
          "..." if len(cmd) > 8 else "")
    subprocess.run(list(cmd), check=True)


def native():
    """libmain.so: our native code (for now the step-A test, android/hello)"""
    lib = os.path.join(OUT, "lib", ABI)
    os.makedirs(lib, exist_ok=True)
    run(CLANG, "-shared", "-fPIC", "-O2", "-Wall",
        "-I", os.path.join(SDL_SRC, "include"),
        os.path.join(HERE, "hello", "hello.c"),
        "-L", os.path.join(SDL_LIB, ABI), "-lSDL3", "-llog", "-landroid", "-lm",
        "-o", os.path.join(lib, "libmain.so"))
    shutil.copy(os.path.join(SDL_LIB, ABI, "libSDL3.so"), lib)


def java():
    """classes.dex: SDL3's activity (from its source) and ours"""
    classes = os.path.join(OUT, "classes")
    shutil.rmtree(classes, ignore_errors=True)
    os.makedirs(classes)
    sources = []
    for root in (os.path.join(SDL_SRC, "android-project", "app", "src", "main", "java"), os.path.join(HERE, "java")):
        for d, _, files in os.walk(root):
            sources += [os.path.join(d, f) for f in files if f.endswith(".java")]
    run(os.path.join(JDK, "bin", "javac.exe"), "-nowarn", "-encoding", "UTF-8", "--release", "11", "-classpath",
        ANDROID_JAR, "-d", classes, *sources)
    class_files = [os.path.join(d, f) for d, _, fs in os.walk(classes) for f in fs if f.endswith(".class")]
    run(os.path.join(BUILD_TOOLS, "d8.bat"), "--release", "--min-api", str(MIN_SDK), "--lib", ANDROID_JAR,
        "--output", OUT, *class_files)


def package():
    """the manifest and resources (aapt2), then the dex and native libraries added"""
    res_flat = os.path.join(OUT, "res.zip")
    base = os.path.join(OUT, "base.apk")
    run(os.path.join(BUILD_TOOLS, "aapt2.exe"), "compile", "--dir", os.path.join(HERE, "res"), "-o", res_flat)
    run(os.path.join(BUILD_TOOLS, "aapt2.exe"), "link", "-o", base, "-I", ANDROID_JAR,
        "--manifest", os.path.join(HERE, "AndroidManifest.xml"), "--min-sdk-version", str(MIN_SDK),
        "--target-sdk-version", str(TARGET_SDK), res_flat)
    with zipfile.ZipFile(base, "a", zipfile.ZIP_DEFLATED) as z:
        z.write(os.path.join(OUT, "classes.dex"), "classes.dex")
        lib = os.path.join(OUT, "lib", ABI)
        for f in sorted(os.listdir(lib)):
            z.write(os.path.join(lib, f), "lib/%s/%s" % (ABI, f))
    return base


def sign(base):
    aligned = os.path.join(OUT, "aligned.apk")
    apk = os.path.join(OUT, "mml.apk")
    if not os.path.exists(KEYSTORE):   # a debug key, as Android Studio makes one
        os.makedirs(os.path.dirname(KEYSTORE), exist_ok=True)
        run(os.path.join(JDK, "bin", "keytool.exe"), "-genkeypair", "-keystore", KEYSTORE, "-storepass", "android",
            "-keypass", "android", "-alias", "androiddebugkey", "-keyalg", "RSA", "-keysize", "2048",
            "-validity", "10000", "-dname", "CN=Android Debug,O=Android,C=US")
    run(os.path.join(BUILD_TOOLS, "zipalign.exe"), "-f", "-p", "4", base, aligned)
    run(os.path.join(BUILD_TOOLS, "apksigner.bat"), "sign", "--ks", KEYSTORE, "--ks-pass", "pass:android",
        "--key-pass", "pass:android", "--out", apk, aligned)
    return apk


def main():
    os.makedirs(OUT, exist_ok=True)
    os.environ["JAVA_HOME"] = JDK   # (d8 and apksigner run on it too)
    native()
    java()
    apk = sign(package())
    print("built", apk)
    if "--install" in sys.argv:
        run(os.path.join(SDK, "platform-tools", "adb.exe"), "install", "-r", apk)


if __name__ == "__main__":
    main()
