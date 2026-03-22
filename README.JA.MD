# MotionPngTuberPlayer 日本語 README

[English README](./README.md)

`MotionPngTuberPlayer` は、`MotionPNGTuber` 派生の **Windows 専用** OBS プラグインプロジェクトです。

このディレクトリは、親の `MotionPNGTuber` 本体とは **別 Git リポジトリ** として運用する前提です。

## いま何ができるか

- Windows 上の OBS 通常ソースとして MotionPNGTuber を再生
- ソースごとに口パク用の音声入力デバイスを選択
- `loop_video` を選ぶと、周辺の `mouth` / `mouth_track.*` / `mouth_track_calibrated.*` を自動補完
- OBS 標準フィルターをそのまま適用
- Python なしの in-process native runtime で再生

## 現在の対応状況

- **Windows**
  - 実装済み
  - 実機検証済み
  - GitHub Actions の build / OBS smoke / release publish 対応済み
- **Linux / macOS**
  - 現行方針ではサポート対象外
  - build / smoke / release 導線は維持しない

## Windows でのビルド

Windows では OBS SDK / `libobsConfig.cmake` がなくても、インストール済み OBS から import lib を生成して fallback build できます。

ローカル build:

```powershell
& 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe' -S . -B build-win-fallback-vs
& 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe' --build build-win-fallback-vs --config Release
```

確認済み DLL:

- `build-win-fallback-vs\Release\MotionPngTuberPlayer.dll`

OBS へ配置:

```powershell
powershell -ExecutionPolicy Bypass -File .\install-to-obs.ps1
```

portable OBS へ配置する場合:

```powershell
powershell -ExecutionPolicy Bypass -File .\install-to-obs.ps1 -ObsRoot .\obs-portable-test
```

## パッケージ化

配布レイアウト作成:

```powershell
.\package-release.ps1 -Configuration Release -PackageName MotionPngTuberPlayer-obs-plugin-windows-x64
```

Windows からローカルで build / tag / release まで進める場合:

```powershell
.\release-windows.ps1 -Tag v0.1.0 -PreRelease
```

## GitHub Actions

`.github\workflows\build-release.yml` では、現在次を回しています。

- Windows release package build
- Windows OBS smoke test
- tag push 時の GitHub release asset 更新

現時点の release asset は次です。

- `MotionPngTuberPlayer-obs-plugin-windows-x64.zip`
- `MotionPngTuberPlayer-release-checksums.txt`

## 現在の backend 構成

- Media Foundation
  - loop 動画 decode
- Windows Imaging Component (WIC)
  - 口スプライト PNG decode
- WinMM `waveIn`
  - ソースごとの音声入力 capture

## 検証済み

portable OBS 32.0.4 と GitHub Actions Windows smoke で次を確認済みです。

- module load 成功
- `motionpngtuber_player` source create 成功
- source screenshot 取得成功
- `crop_filter` の attach 成功
- `crop_filter` 適用後に出力サイズが変わること
- OBS properties 経由の audio device list 取得成功
- `SetInputSettings` 更新後も OBS restart をまたいで設定が残ること
- `.npz` 選択時の sibling JSON 解決で描画継続
- loop video だけ指定した場合の auto-fill
- `SetInputSettings` を繰り返す save-stress 相当の更新で描画継続

## 既知の制約

- 現在のサポート対象は **Windows 64-bit OBS のみ**
- Linux / macOS build・smoke・release は現行方針では対象外
- `.npz` は直接 parse ではなく、**sibling JSON がある前提の互換対応**
- DLL 内 fallback はあるが、配布物としては `data\obs-plugins\MotionPngTuberPlayer\locale\` 同梱を推奨
- 通常インストール版 OBS (`C:\Program Files\obs-studio`) へ配置する場合は管理者権限が必要
