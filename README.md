# MotionPngTuberPlayer

[日本語 README](./README.JA.MD)

`MotionPngTuberPlayer` is a **Windows 64-bit OBS plugin** for using MotionPNGTuber inside OBS.

The current release is **DLL-only**. You do not need to install Python.

## Quick install

### 1. Download the release zip

Download this file from GitHub Releases:

- `MotionPngTuberPlayer-obs-plugin-windows-x64.zip`

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

### 4. Leave audio sync on the default setting first

`Audio Sync Source` defaults to **Auto**.

- If a suitable OBS audio source exists, lip sync follows it
- If not, the plugin falls back to the direct input device

For most users, the default setting is the best place to start.

## Supported files

- `.json` track files
- `.npz` track files

Standard `.npz` track archives are read directly.

If your `.npz` uses an unsupported ZIP variant, keep a sibling `mouth_track.json` in the same folder as a fallback.

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
- your microphone or OBS audio source is selected correctly

## Current scope

- Windows 64-bit OBS only
- DLL-only release package
- Works as a normal OBS source, so standard OBS filters can be used

## Developer notes

Most users can ignore this section.

Local build:

```powershell
& 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe' -S . -B build-win-fallback-vs
& 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe' --build build-win-fallback-vs --config Release
```

Local package:

```powershell
powershell -ExecutionPolicy Bypass -File .\package-release.ps1 -PackageName MotionPngTuberPlayer-obs-plugin-windows-x64
```
