# MotionPngTuberPlayer

[日本語 README](./README.JA.MD)

`MotionPngTuberPlayer` is an OBS plugin for using MotionPNGTuber inside OBS on **Windows 64-bit** and **Ubuntu**.

The current Windows release is **DLL-only**. You do not need to install Python.

## Quick install

### Windows release zip

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

### Ubuntu build

Ubuntu support is currently **source-build based**. The repository CI now builds, tests, install-verifies, and Linux-runtime-validates the Ubuntu plugin on `ubuntu-24.04`.

The Ubuntu validation job now generates synthetic Linux fixtures and checks the Linux-only runtime paths directly: FFmpeg PNG sprite decode, native `.npz` inflate/track load, native runtime startup/render, and libobs module/source loading of the built `.so`.

On Ubuntu, install the required build dependencies first. `zlib1g-dev` is required for native `.npz` track loading on Linux. The current CMake minimum is **3.22**.

```bash
sudo apt install build-essential cmake pkg-config ffmpeg libobs-dev libavformat-dev libavcodec-dev libavutil-dev libswscale-dev zlib1g-dev
```

Then build and install into OBS's per-user plugin directory:

```bash
cmake -S . -B build-linux -DMPT_BUILD_PLUGIN=ON -DMPT_INSTALL_UBUNTU_PER_USER_PLUGIN=ON
cmake --build build-linux
cmake --install build-linux --prefix "$HOME/.config/obs-studio/plugins"
```

That install step copies the plugin into:

```text
~/.config/obs-studio/plugins/MotionPngTuberPlayer/bin/64bit/MotionPngTuberPlayer.so
~/.config/obs-studio/plugins/MotionPngTuberPlayer/data/locale/...
```

Restart OBS after the install/copy step. If you want a system-style package layout instead, configure with `-DMPT_INSTALL_UBUNTU_PER_USER_PLUGIN=OFF`.

If you only want to run the backend tests on Ubuntu without `libobs` headers installed, you still need the FFmpeg and zlib development headers:

```bash
cmake -S . -B build-linux -DMPT_BUILD_PLUGIN=OFF -DBUILD_TESTING=ON
cmake --build build-linux --target mpt-video-backend-loop-test
ctest --test-dir build-linux --output-on-failure
```

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

On Ubuntu, this OBS audio-source path is the supported lip-sync input path.

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

On Ubuntu, also confirm these paths exist:

- `~/.config/obs-studio/plugins/MotionPngTuberPlayer/bin/64bit/MotionPngTuberPlayer.so`
- `~/.config/obs-studio/plugins/MotionPngTuberPlayer/data/locale/`

### Windows says the file cannot be copied

If OBS is installed in `C:\Program Files\obs-studio\`, you need Administrator permission.

If that is inconvenient, use a portable OBS folder instead.

### The mouth does not move

Check:

- `track_file` is correct
- `mouth_dir` is correct
- the correct OBS audio source is selected

## Current scope

- Windows 64-bit OBS
- Ubuntu source builds
- Windows release package
- Works as a normal OBS source, so standard OBS filters can be used
- Direct input device capture remains Windows-only; on Ubuntu use an OBS audio source for lip sync

## Developer notes

Most users can ignore this section.

Local Windows build:

```powershell
& 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe' -S . -B build-win-fallback-vs
& 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe' --build build-win-fallback-vs --config Release
```

Local package:

```powershell
powershell -ExecutionPolicy Bypass -File .\package-release.ps1
```

Local Ubuntu build:

```bash
cmake -S . -B build-linux -DMPT_BUILD_PLUGIN=ON -DMPT_INSTALL_UBUNTU_PER_USER_PLUGIN=ON
cmake --build build-linux
cmake --install build-linux --prefix "$HOME/.config/obs-studio/plugins"
```
