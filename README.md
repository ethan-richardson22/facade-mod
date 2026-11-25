# Façade modernization mod

A drop-in add-on for the Windows version of Façade (2005) that makes it run well on modern PCs: native-resolution rendering in a resizable window, borderless fullscreen, widescreen, high-DPI support, anti-aliasing and a few quality-of-life fixes.

**No game files are included.** You need your own copy of Façade.

## Features

**Display**
- Renders at your window's real resolution instead of a fixed 1024×768 (the engine's 1016×741 canvas is scaled and centred)
- Normal resizable window with configurable start size; **F11** or **Alt+Enter** toggles borderless fullscreen
- Widescreen: wide windows show more of the scene instead of black side bars, with no stretching
- DPI-aware manifest, so the game isn't drawn at half resolution and stretched on 150–200% scaled displays
- MSAA anti-aliasing (2–16×, falls back to the best the GPU supports)
- Lines the engine draws are thickened to match the display scale; no flicker while resizing

**Mouse & cursor**
- Mouse picking, cursor warping and cursor confinement are mapped to the scaled window (including the widescreen side areas)
- Action cursors (hand, kiss, hug, …) scale with Windows display scaling

**Gameplay / QoL**
- Startup wait cut from 35 s to 3 s (Trip's hard-coded "let everything load" delay)
- Faster character reactions: the Java game logic's queries to the engine are answered within ~1 ms instead of waiting for the next frame (~18 ms each)
- Subtitles on by default (the in-game toggle still works)
- Optional automatic skip of the title sequence
- No error beep on Alt+Enter

**Java & diagnostics**
- Java output captured to `fxglu_java_out.log` (no console window); Java path and arguments can be overridden
- Optional debug log and engine/Java timing statistics

## How it works

- `animEngineDLL.dll` imports `GLU32.dll`; the patcher renames that import to `FXGLU.dll` (a 3-byte change, same length). `FXGLU.dll` forwards the three GLU functions to the real `glu32.dll` and, when loaded, hooks the engine's OpenGL / window / mouse imports (IAT patching) plus one internal jump thunk for the intro skip.
- The engine keeps believing it draws to its original 1016×741 canvas; only the real viewport, projection width and mouse coordinates are translated. Matrix and viewport read-backs return the original values, so the engine's own picking maths is unchanged.
- The startup wait is a one-byte bytecode change in `Trip_ArgumentStepExecute.class`.
- A side-by-side `animEngineStarter.exe.manifest` marks the process DPI-aware.

Everything is reversible: originals are backed up as `*.orig` and `facade_patch.py restore` puts them back.

## Requirements

- Façade for Windows (the original 2005 release, `animEngineDLL.dll` with the stock file layout)
- Python 3.6+ (for the patcher)
- To build the DLL: Visual Studio 2017 or newer, or the Build Tools, with the *Desktop development with C++* workload

## Install

1. **Copy your Façade install** (the folder containing `util\`, e.g. `C:\Program Files (x86)\Facade`) to `game\Facade` next to this README.
   Working on a copy keeps your original install untouched. To patch a different folder instead, set the `FACADE_DIR` environment variable.
2. **Build the add-on:**
   ```
   fxglu\build.bat
   ```
3. **Patch the game copy:**
   ```
   python facade_patch.py install        # optional: install N  = startup wait in seconds (default 3, original 35)
   ```
4. Run `game\Facade\util\sources\facade\animEngineStarter.exe`.

To undo everything: `python facade_patch.py restore`.

## Configuration

Settings live in `fxglu.ini` next to the game's `animEngineStarter.exe` (copied there on first install and never overwritten afterwards). The defaults:

```ini
[display]
width=1920          ; client size at startup
height=1080
fullscreen=0        ; start in borderless fullscreen
widescreen=1        ; show more scene in wide windows instead of bars
msaa=8              ; 0 / 2 / 4 / 8 / 16
anisotropy=16       ; 0..16
cursor_scale=0      ; percent; 0 = follow Windows display scaling

[game]
skip_intro=0        ; skip the title sequence (it contains Trip's voicemail setting up the story)
subtitles=1
fast_answers=1      ; answer Java's engine queries during the frame sleep

[java]
path=               ; empty = bundled ..\..\j2re1.4.2_06\bin\java.exe
args=               ; empty = the engine's built-in argument list

[debug]
log=1               ; fxglu_<process>.log next to the DLL
perfstats=0         ; timing statistics every 10 s
```

## Known limitations

- Windows only.
- Engine addresses are hard-coded for the stock 2005 `animEngineDLL.dll`; the patcher checks the bytes it changes and refuses to patch an unexpected file.
- The engine uploads textures without mipmaps, so the `anisotropy` setting currently has little visible effect.

## Additional Credits
- Claude Code, for writing all of the programming
- CowBoyMeatBall, the creator of the [3ds Façade demake](https://archive.org/details/Facade-for-3DS), whose decompiled code helped guide Claude on fixing issues

## Disclaimer

Unofficial fan project, not affiliated with or endorsed by the creators of Façade (Michael Mateas and Andrew Stern / Procedural Arts). Façade and its assets belong to their respective owners; this repository contains only original code for the add-on and patcher.
