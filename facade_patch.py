"""Facade display patcher: native-resolution rendering + a proper resizable window.

Operates on the game copy in ./game/Facade next to this script:
    python facade_patch.py install [N]  # (re)install everything; N = startup wait in seconds (default 3,
                                        #   original 35)
    python facade_patch.py restore      # put the original files back, remove FXGLU files and the manifest

What it changes in game/Facade/util/sources/facade:
  * animEngineDLL.dll: the import name "GLU32.dll" becomes "FXGLU.dll" (same length, 3 bytes),
    so the engine loads our add-on. FXGLU.dll forwards the 3 GLU functions to the real glu32
    and hooks the engine's window / viewport / mouse calls (source: fxglu/fxglu.c).
    The DLL is always rebuilt from the pristine backup animEngineDLL.dll.orig.
  * FXGLU.dll copied in; fxglu.ini copied in only if not already there (it's the live config).
  * util/classes/.../Trip_ArgumentStepExecute.class: Trip's fixed 35-second "give everyone a chance
    to load" wait before the title sequence (real loading takes ~3 s on a modern PC). Backup: .orig
  * animEngineStarter.exe.manifest declaring the process DPI-aware, so Windows doesn't render
    at half resolution and stretch 2x on a 200%-scaled display.
"""
import os, shutil, sys

HERE = os.path.dirname(os.path.abspath(__file__))
# FACADE_DIR overrides which game copy is patched (default: ./game/Facade next to this script)
GAME_ROOT = os.environ.get("FACADE_DIR") or os.path.join(HERE, "game", "Facade")
GAME = os.path.join(GAME_ROOT, "util", "sources", "facade")
SRC = os.path.join(HERE, "fxglu")
DLL = os.path.join(GAME, "animEngineDLL.dll")
BAK = DLL + ".orig"
EXE = os.path.join(GAME, "animEngineStarter.exe")
MANIFEST = EXE + ".manifest"
ADDON_FILES = ("FXGLU.dll", "fxglu.ini")
ADDON_LOG_PREFIX = "fxglu_"     # fxglu_<process>.log

OFF_GLU_NAME = 0x1FB89E          # file offset of the "GLU32.dll" import-name string
OLD_NAME, NEW_NAME = b"GLU32.dll\0", b"FXGLU.dll\0"

CLASSES = os.path.join(GAME_ROOT, "util", "classes")
TRIP_ARGS = os.path.join(CLASSES, "facade", "characters", "trip", "java", "Trip_ArgumentStepExecute.class")
TRIP_ARGS_BAK = TRIP_ARGS + ".orig"
# Trip's startup behaviour InitialWait_1 does `subgoal WaitFor(35)` ("waiting to give a chance for
# everyone to load") before starting the title sequence. Its argument is a single `bipush 35` in
# argumentExecute81, case 33367. Pattern = the surrounding bytecode; the byte after 0x10 is the value.
WAIT_PATTERN = bytes.fromhex("04bd00034e2d03bb003d5910")
WAIT_SUFFIX = bytes.fromhex("b70041")
DEFAULT_INTRO_WAIT = 3

MANIFEST_XML = """<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<assembly xmlns="urn:schemas-microsoft-com:asm.v1" manifestVersion="1.0">
  <application xmlns="urn:schemas-microsoft-com:asm.v3">
    <windowsSettings>
      <dpiAware xmlns="http://schemas.microsoft.com/SMI/2005/WindowsSettings">true</dpiAware>
    </windowsSettings>
  </application>
</assembly>
"""


def touch_exe():
    # Windows caches manifest lookups by exe timestamp; bump it so changes are seen
    os.utime(EXE, None)


def patch_intro_wait(seconds):
    if not 0 <= seconds <= 127:
        sys.exit("intro wait must be 0..127 seconds")
    if not os.path.exists(TRIP_ARGS_BAK):
        shutil.copy2(TRIP_ARGS, TRIP_ARGS_BAK)
    data = bytearray(open(TRIP_ARGS_BAK, "rb").read())
    full = WAIT_PATTERN + bytes([35]) + WAIT_SUFFIX     # unique only with the value included
    pos = data.find(full)
    if pos < 0 or data.count(full) != 1:
        sys.exit("Trip_ArgumentStepExecute.class doesn't look like the original")
    data[pos + len(WAIT_PATTERN)] = seconds
    open(TRIP_ARGS, "wb").write(data)
    print(f"Trip's startup WaitFor(35) -> WaitFor({seconds})")


def install(intro_wait=DEFAULT_INTRO_WAIT):
    patch_intro_wait(intro_wait)
    built = os.path.join(SRC, "FXGLU.dll")
    if not os.path.exists(built):
        sys.exit(f"{built} not found - run fxglu\\build.bat first")
    if not os.path.exists(BAK):
        shutil.copy2(DLL, BAK)
        print("backup ->", BAK)
    data = bytearray(open(BAK, "rb").read())
    if data[OFF_GLU_NAME:OFF_GLU_NAME + len(OLD_NAME)] != OLD_NAME:
        sys.exit("backup DLL doesn't look like the original animEngineDLL.dll")
    data[OFF_GLU_NAME:OFF_GLU_NAME + len(NEW_NAME)] = NEW_NAME
    open(DLL, "wb").write(data)
    print("animEngineDLL.dll: GLU32.dll import -> FXGLU.dll")

    shutil.copy2(built, os.path.join(GAME, "FXGLU.dll"))
    print("copied FXGLU.dll")
    ini = os.path.join(GAME, "fxglu.ini")
    if not os.path.exists(ini):
        shutil.copy2(os.path.join(SRC, "fxglu.ini"), ini)
        print("copied fxglu.ini")
    else:
        print("kept existing", ini)

    with open(MANIFEST, "w", encoding="utf-8") as f:
        f.write(MANIFEST_XML)
    touch_exe()
    print("DPI-aware manifest written")


def restore():
    if os.path.exists(TRIP_ARGS_BAK):
        shutil.copy2(TRIP_ARGS_BAK, TRIP_ARGS)
        print("original Trip_ArgumentStepExecute.class restored")
    if os.path.exists(BAK):
        shutil.copy2(BAK, DLL)
        print("original animEngineDLL.dll restored")
    logs = tuple(n for n in os.listdir(GAME) if n.startswith(ADDON_LOG_PREFIX) and n.endswith(".log"))
    for name in ADDON_FILES + logs + (os.path.basename(MANIFEST),):
        p = os.path.join(GAME, name)
        if os.path.exists(p):
            os.remove(p)
            print("removed", name)
    touch_exe()


if __name__ == "__main__":
    mode = sys.argv[1] if len(sys.argv) > 1 else "install"
    if mode == "install":
        install(int(sys.argv[2]) if len(sys.argv) > 2 else DEFAULT_INTRO_WAIT)
    elif mode == "restore":
        restore()
    else:
        sys.exit(__doc__)
