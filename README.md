# MotionPngTuberPlayer

[日本語 README](./README.JA.MD)

`MotionPngTuberPlayer` is a **Windows 64-bit OBS Studio plugin** for playing a MotionPNGTuber avatar directly inside OBS.

It renders a mouthless loop video, overlays mouth sprites based on a lip-sync track, and drives the mouth in real time from an OBS audio source. Playback runs as a **native in-process runtime**, so the release ships as a **single DLL** and does not require Python.

## Overview

The plugin registers a native OBS input source (`motionpngtuber_player`). Once added to a scene it behaves like any other OBS source, so standard OBS filters and transforms apply. It reads a loop video, a folder of mouth sprites, and a lip-sync track, then warps and blends the mouth onto the video each frame while following the audio level of a selected OBS audio source.

## Features

- Native OBS input source, no Python required at runtime
- Mouthless loop video playback via Media Foundation
- Mouth sprite compositing (warp + bilinear blend) from a mouth image folder
- Lip sync driven by a selectable OBS audio source, with an optional direct Windows input device (advanced)
- Track file support for `.json` and `.npz`
  - `.npz` archives are parsed natively (raw deflate members via OBS `zlib.dll`, streamed ZIP layouts, big-endian and Fortran-ordered NumPy arrays)
  - Track shapes: legacy nested `quad` and flat `x0..x3` / `y0..y3`
- Auto-fill of sibling assets (`mouth`, `mouth_track`, `mouth_track_calibrated`) when only `loop_video` is selected
- Configurable render FPS (1–120, default 30) and a mouth validity policy (`hold` / `strict`)
- Japanese and English UI, with an English/Japanese fallback embedded in the DLL for when locale files fail to load
- Works as a normal OBS source, so standard OBS filters can be applied

## Requirements

- Windows 64-bit
- 64-bit OBS Studio (verified against portable OBS 32.0.4)

For building from source, see [Development](#development).

## Quick install

### 1. Download the release zip

Download this file from GitHub Releases:

- `MotionPngTuberPlayer-obs-plugin-windows-x64-<version>.zip`

### 2. Close OBS

If OBS is still running, Windows may block the DLL from being replaced.

### 3. Copy the `obs-plugins` folder from the zip into your OBS folder

Use one of these destinations:

- Standard OBS install  
  `C:\Program Files\obs-studio\`
- Portable OBS  
  the root of your portable OBS folder

After copying, this file should exist:

```text
obs-plugins\64bit\MotionPngTuberPlayer.dll
```

> Installing into `C:\Program Files\obs-studio\` requires Administrator permission.

### 4. Start OBS

If `MotionPngTuberPlayer` appears in the source list, the plugin is installed correctly.

## First use in OBS

### 1. Add the source

In OBS:

1. Click `+` in the `Sources` panel
2. Choose `Input`
3. Choose `MotionPngTuberPlayer`

### 2. Set the required files

You normally need these three things:

- a mouthless loop video
- a mouth image folder
- a lip-sync track file

In the source properties, set:

- `loop_video`
- `mouth_dir`
- `track_file`

### 3. Auto-fill may handle the rest

If related files are next to the selected `loop_video`, the plugin can auto-fill the remaining paths.

Typical sibling files are:

- `mouth`
- `mouth_track.json`
- `mouth_track_calibrated.json`

### 4. Select the OBS audio source for lip sync

Choose the OBS audio source that should drive lip sync in `Audio Sync Source`.

For most users, this should be the microphone/input source they already use in OBS.

### Advanced settings

Enable `Show Advanced Settings` in the source properties to access:

- `Track Calibrated File` — an optional calibrated track (`.json` or `.npz`)
- `Render FPS` — output frame rate (1–120, default 30)
- `Valid Policy` — how invalid mouth frames are handled (`Hold` keeps the last valid mouth, `Strict` is stricter)
- Direct Windows input device selection, for lip sync without an OBS audio source

## Supported files

- `.json` track files
- `.npz` track files

NumPy-generated `.npz` track archives are read directly, including regular deflate-compressed members, streamed ZIP layouts, big-endian arrays, and Fortran-ordered arrays.

## Troubleshooting

### The source does not appear in OBS

Check these first:

- OBS was restarted after copying the plugin
- `MotionPngTuberPlayer.dll` is in `obs-plugins\64bit\`
- you are using **64-bit OBS**

### Windows says the file cannot be copied

If OBS is installed in `C:\Program Files\obs-studio\`, you need Administrator permission.

If that is inconvenient, use a portable OBS folder instead.

### The mouth does not move

Check:

- `track_file` is correct
- `mouth_dir` is correct
- the correct OBS audio source is selected

## Current scope

- Windows 64-bit OBS only
- DLL-only release package
- Works as a normal OBS source, so standard OBS filters can be used

## Development

Most users can ignore this section.

The plugin is a C/C++ (C++17) OBS module built with CMake (>= 3.28). It is **Windows-only**; CMake fails fast on non-Windows hosts.

The build looks for a `libobs` CMake package first. When it is not found, it falls back to an installed OBS runtime: it locates `obs.dll` (default `C:\Program Files\obs-studio`, override with `-DMPT_OBS_ROOT=...`) and generates an import library from it using the Visual Studio `dumpbin.exe` / `lib.exe` tools.

### Build

```powershell
cmake -S . -B build-win-fallback-vs -DMPT_OBS_ROOT="C:\Program Files\obs-studio"
cmake --build build-win-fallback-vs --config Release
```

The built plugin is `build-win-fallback-vs\Release\MotionPngTuberPlayer.dll`.

### Test

Tests are wired through CTest and are built when configuring with `-DBUILD_TESTING=ON`:

```powershell
cmake -S . -B build-win-fallback-vs -DMPT_OBS_ROOT="C:\Program Files\obs-studio" -DBUILD_TESTING=ON
cmake --build build-win-fallback-vs --config Release
ctest --test-dir build-win-fallback-vs -C Release --output-on-failure
```

Available tests:

- `mpt-video-backend-reader` — Media Foundation reader unit test
- `mpt-video-backend-loop` — loop-boundary test (only added when `ffmpeg` is on `PATH`)
- `obs-websocket-smoke-unit` — Python `unittest` for the smoke helper (only added when Python 3 is found)

### Install into OBS locally

```powershell
powershell -ExecutionPolicy Bypass -File .\install-to-obs.ps1
```

Install into a portable OBS folder:

```powershell
powershell -ExecutionPolicy Bypass -File .\install-to-obs.ps1 -ObsRoot .\obs-portable-test
```

### Package a release

```powershell
powershell -ExecutionPolicy Bypass -File .\package-release.ps1
```

`release-windows.ps1 -Tag v<version>` automates build, packaging, tag push, and GitHub release creation.

### Continuous integration

`.github/workflows/build-release.yml` builds the Windows plugin, runs an OBS WebSocket smoke test (module load, source create, screenshot, crop filter attach, audio device enumeration, settings persistence across restart), and publishes a GitHub release on `v*` tags after the smoke test passes.

## Project structure

```text
src/                     Native plugin sources (OBS source, runtime, backends)
  plugin-main.c            OBS module entry / source registration
  motionpngtuber-source.c  OBS source, properties UI, auto-fill, runtime control
  motionpngtuber-native.cpp Native runtime: track load, mouth compositing, audio capture
  mpt-video-backend.*      Media Foundation loop-video decode
  mpt-image-backend.*      WIC mouth-sprite PNG decode
  mpt-audio-backend.*      WinMM waveIn audio capture
  mpt-text.*               Embedded English/Japanese locale fallback
data/locale/             OBS locale files (en-US.ini, ja-JP.ini)
tests/                   C++ and Python tests wired through CTest
ci/                      OBS install / smoke-asset / WebSocket smoke helpers
cmake/                   Import-library generation for fallback builds
python/                  Legacy Python worker code (not part of the native build)
*.ps1                    Local build, install, package, and release scripts
CMakeLists.txt           Build configuration
```

## License

Released under the [MIT License](./LICENSE).
