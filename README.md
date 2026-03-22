# MotionPngTuberPlayer

[日本語 README](./README.JA.MD)

`MotionPngTuberPlayer` is a derivative OBS plugin project for `MotionPNGTuber`.

This directory is intended to be managed as a **separate Git repository** from the main `MotionPNGTuber` project.

## Current scope

- Native OBS input source plugin
- In-process native MotionPNGTuber playback runtime on Windows
- Per-source audio input device selection
- Auto-fill of sibling mouth/track assets when the loop video is selected
- Output as a normal OBS video source so standard OBS filters can be applied

## Layout

- `src/` native OBS plugin code
- `data/locale/` plugin locale files
- `build-win-fallback-vs/` Windows fallback build tree for local builds without the full OBS SDK

## Build note

On Windows, this project can now be built locally even when the OBS SDK / `libobsConfig.cmake` package is not installed.

The current fallback build path uses:

- an import library generated from the installed `obs.dll`
- a local minimal OBS 32.0.4 ABI shim
- a local Win32 compatibility layer for the small `util/*` subset used by the plugin

The current verified local build artifact is:

- `build-win-fallback-vs\\Release\\MotionPngTuberPlayer.dll`

To deploy the current build into OBS on Windows, run:

- `powershell -ExecutionPolicy Bypass -File .\\install-to-obs.ps1`
- If you use a portable OBS folder, pass it explicitly: `powershell -ExecutionPolicy Bypass -File .\\install-to-obs.ps1 -ObsRoot .\\obs-portable-test`
- The install script now verifies that the target looks like a 64-bit OBS install before copying files.

To create a distributable folder/zip locally, run:

- `powershell -ExecutionPolicy Bypass -File .\\package-release.ps1 -PackageName MotionPngTuberPlayer-obs-plugin-windows-x64`
- To build, tag, and create/update a GitHub release from the current checkout, run: `powershell -ExecutionPolicy Bypass -File .\\release-windows.ps1 -Tag v0.1.0 -PreRelease`

GitHub Actions now includes a build workflow at `.github\\workflows\\windows-ci.yml` that builds the Windows release package, compiles the Linux and macOS packages, and publishes all three assets on tag pushes.

The release asset names are now more explicit:

- `MotionPngTuberPlayer-obs-plugin-windows-x64.zip`
- `MotionPngTuberPlayer-obs-plugin-linux-x64-stub.zip`
- `MotionPngTuberPlayer-obs-plugin-macos-arm64-stub.zip`
- `MotionPngTuberPlayer-release-checksums.txt`

The CI workflow still uploads extracted folders as temporary Actions artifacts so the downloadable release assets do not become a zip containing another zip.

The plugin now also has a built-in English/Japanese text fallback in the DLL. If OBS cannot load the external locale files, source and property labels fall back to readable built-in strings instead of raw keys such as `MotionPngTuberPlayer.SourceName`.

If OBS shows raw keys such as `MotionPngTuberPlayer.SourceName` or `MotionPngTuberPlayer.LoopVideo`, the DLL was copied without the plugin data folder. Make sure both of these exist in the OBS install:

- `obs-plugins\\64bit\\MotionPngTuberPlayer.dll`
- `data\\obs-plugins\\MotionPngTuberPlayer\\locale\\ja-JP.ini`

The native-only Windows runtime currently uses:

- Media Foundation for loop video decode
- Windows Imaging Component (WIC) for PNG sprite decode
- WinMM / `waveIn` for per-source audio input capture

The current non-Windows build path now shares the runtime core and uses:

- FFmpeg for loop video decode
- `libpng` for PNG sprite decode
- Windows-only audio capture for now, so Linux/macOS release assets remain labeled `-stub`

## macOS / Linux build note

The repository now contains a shared runtime core plus non-Windows media backends, with a shared helper script at `ci/build-nonwindows-stub.sh`.

Linux builds are expected to work with:

- `libobs-dev`
- `libpng-dev`
- `libavcodec-dev`
- `libavformat-dev`
- `libavutil-dev`
- `libswscale-dev`
- `pkg-config`
- `cmake`
- `ninja-build`

macOS builds now have a GitHub Actions path too. The hosted `macos-14` runner installs `OBS.app`, locates the bundled `libobs` framework, and installs Homebrew `ffmpeg` / `libpng` so the shared non-Windows media backends can compile.

For local macOS builds, install `OBS.app`, install `ffmpeg` and `libpng` via Homebrew, and point `MPT_MACOS_OBS_LIBRARY` at the bundled `libobs` binary, for example:

- `MPT_MACOS_OBS_LIBRARY=/Applications/OBS.app/Contents/Frameworks/libobs.framework/libobs`

The release assets remain named `-stub` until non-Windows audio capture/device selection and OBS smoke tests are complete.

## Track note

The native runtime accepts:

- `.json` track exports
- `.npz` track selections when a sibling `mouth_track.json` export exists next to the NPZ

JSON compatibility now covers both:

- legacy nested `quad`: `[[x, y], ...]`
- flattened `x0..x3`, `y0..y3`

`convert_npz_to_json.py` emits both JSON representations, and the OBS runtime can read either form.

## Verification status

Verified locally against portable OBS 32.0.4 on the native-only Windows path:

- the module loads and registers `motionpngtuber_player`
- source creation succeeds through obs-websocket
- standard OBS filters can be attached to the source (verified with `crop_filter`)
- source screenshots are produced successfully from the native runtime
- selecting only the loop video path auto-fills sibling `mouth`, `mouth_track.json`, and `mouth_track_calibrated.npz` when present
- selecting a `.npz` track path works when a sibling `mouth_track.json` exists
- legacy `quad` JSON and flat `x0..x3` / `y0..y3` JSON both render correctly
- repeated `SetInputSettings` updates with screenshot checks continue to succeed after the latest hardening pass
