# MotionPngTuberPlayer

[日本語 README](./README.JA.MD)

`MotionPngTuberPlayer` is a Windows-only OBS plugin derived from `MotionPNGTuber`.

This directory is intended to be managed as a **separate Git repository** from the main `MotionPNGTuber` project.

## Current scope

- Native OBS input source plugin on Windows
- In-process native MotionPNGTuber playback runtime on Windows
- Per-source audio input device selection
- Auto-fill of sibling mouth/track assets when the loop video is selected
- Output as a normal OBS video source so standard OBS filters can be applied

## Layout

- `src/` native OBS plugin code
- `data/locale/` plugin locale files
- `build-win-fallback-vs/` Windows fallback build tree for local builds without the full OBS SDK

## Windows build

On Windows, this project can be built locally even when the OBS SDK / `libobsConfig.cmake` package is not installed.

The fallback build path uses:

- an import library generated from the installed `obs.dll`
- a local minimal OBS 32.0.4 ABI shim
- a local Win32 compatibility layer for the small `util/*` subset used by the plugin

Local build:

```powershell
& 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe' -S . -B build-win-fallback-vs
& 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe' --build build-win-fallback-vs --config Release
```

The currently verified local build artifact is:

- `build-win-fallback-vs\Release\MotionPngTuberPlayer.dll`

To deploy the current build into OBS on Windows, run:

- `powershell -ExecutionPolicy Bypass -File .\install-to-obs.ps1`
- If you use a portable OBS folder, pass it explicitly: `powershell -ExecutionPolicy Bypass -File .\install-to-obs.ps1 -ObsRoot .\obs-portable-test`

To create a distributable folder or zip locally, run:

- `powershell -ExecutionPolicy Bypass -File .\package-release.ps1 -PackageName MotionPngTuberPlayer-obs-plugin-windows-x64`

To build, tag, and create or update a GitHub release from the current checkout, run:

- `powershell -ExecutionPolicy Bypass -File .\release-windows.ps1 -Tag v0.1.0 -PreRelease`

## GitHub Actions

`.github\workflows\build-release.yml` now runs a Windows-only release pipeline:

- Windows package build
- Windows OBS smoke test
- Tag-based GitHub release publication after the Windows smoke test succeeds

The published release assets are:

- `MotionPngTuberPlayer-obs-plugin-windows-x64.zip`
- `MotionPngTuberPlayer-release-checksums.txt`

## Native runtime

The current supported runtime uses Windows-native backends:

- Media Foundation for loop video decode
- Windows Imaging Component (WIC) for PNG sprite decode
- WinMM / `waveIn` for per-source audio input capture

## Track compatibility

The native runtime accepts:

- `.json` track exports
- `.npz` track selections when a sibling `mouth_track.json` export exists next to the NPZ
- legacy nested `quad`: `[[x, y], ...]`
- flattened `x0..x3`, `y0..y3`

## Verification status

Verified locally against portable OBS 32.0.4 and in GitHub Actions Windows smoke:

- the module loads and registers `motionpngtuber_player`
- source creation succeeds through obs-websocket
- the audio-device property list is populated through the OBS properties API
- standard OBS filters can be attached to the source and `crop_filter` changes the rendered source size
- source screenshots are produced successfully from the native runtime
- `SetInputSettings` updates survive an OBS restart
- selecting only the loop video path auto-fills sibling `mouth`, `mouth_track.json`, and `mouth_track_calibrated.npz` when present
- selecting a `.npz` track path works when a sibling `mouth_track.json` exists
- repeated `SetInputSettings` updates with screenshot checks continue to succeed after the latest hardening pass

## Known constraints

- Windows 64-bit OBS is the supported target
- Linux/macOS build, smoke, and release paths are no longer supported
- `.npz` compatibility still depends on a sibling JSON export
- The DLL has a built-in English/Japanese locale fallback, but shipping `data\obs-plugins\MotionPngTuberPlayer\locale\` is still recommended
- Installing into `C:\Program Files\obs-studio` requires Administrator PowerShell
