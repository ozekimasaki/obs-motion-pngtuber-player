# MotionPngTuberPlayer 日本語 README

[English README](./README.md)

`MotionPngTuberPlayer` は、`MotionPNGTuber` 派生の OBS プラグインプロジェクトです。

このディレクトリは、親の `MotionPNGTuber` 本体とは **別 Git リポジトリ** として運用する前提です。

## いま何ができるか

- OBS の通常ソースとして MotionPNGTuber を再生
- ソースごとに口パク用の音声入力デバイスを選択
- OBS 標準フィルターをそのまま適用
- `loop_video` を選ぶと、周辺の `mouth` / `mouth_track.*` / `mouth_track_calibrated.*` を自動補完
- `.json` トラック対応
- `.npz` を選んだ場合も sibling JSON があれば互換動作

## 現在の対応状況

- **Windows**
  - 実装済み
  - 実機検証済み
  - 配布 asset あり
- **Linux**
  - stub build 導線あり
  - GitHub Actions で stub build / package / release 可能
  - ただし現時点では **stub package** 扱い
- **macOS**
  - stub build 導線あり
  - backend 分離の土台あり
  - GitHub Actions でも `workflow_dispatch` で build conduit を見せた
  - ただし GitHub-hosted runner 上の release packaging はまだ未接続

## Windows でのビルド

Windows では OBS SDK / `libobsConfig.cmake` がなくても、インストール済み OBS から import lib を生成して fallback build できます。

ローカル build:

```powershell
& 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe' -S . -B build-win-fallback-vs
& 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe' --build build-win-fallback-vs --config Release
```

OBS へ配置:

```powershell
powershell -ExecutionPolicy Bypass -File .\install-to-obs.ps1
```

## Linux / macOS の stub build

共通 helper:

```bash
bash ./ci/build-nonwindows-stub.sh build-linux-stub
```

Linux では次があれば流せます。

- `libobs-dev`
- `pkg-config`
- `cmake`
- `ninja-build`

Ubuntu 24.04 なら例えば:

```bash
sudo apt-get update
sudo apt-get install -y cmake ninja-build pkg-config libobs-dev
bash ./ci/build-nonwindows-stub.sh build-linux-stub
```

macOS は現在、**OBS / libobs の開発用パスを自前で見せる前提** です。次のいずれかを設定してください。

- `libobs_DIR`
- `PKG_CONFIG_PATH`
- `OBS_PREFIX`

補足:

- Homebrew cask の `obs` は runtime 導線としては有用ですが、plugin build には開発用 header / metadata が別途必要です。

## パッケージ化

配布レイアウト作成:

```powershell
.\package-release.ps1 -Configuration Release
```

Linux package 例:

```powershell
pwsh ./package-release.ps1 -BuildDir build-linux-stub -Platform linux
```

## GitHub Actions

`.github/workflows/windows-ci.yml` で次を回しています。

- Windows release package build
- Linux stub build
- tag push 時の GitHub release asset 更新
- `workflow_dispatch` 時の macOS stub build conduit

現時点の release asset 方針:

- Windows: 実利用向け package
- Linux: stub package
- macOS: build conduit はあるが、まだ自動 release asset なし

## GitHub release

Windows からローカルで release まで一気に進める場合:

```powershell
.\release-windows.ps1 -Tag v0.1.0 -PreRelease
```

このスクリプトは次をまとめて実行します。

- build
- package
- tag push
- GitHub release 作成 / asset upload

## 既知の制約

- 現状の完成度は **Windows が最優先**
- Linux / macOS は backend 分離と build 導線の段階
- `.npz` は直接 parse ではなく、sibling JSON 前提の互換対応
- locale fallback はあるが、配布物では `data/obs-plugins/MotionPngTuberPlayer/locale/` 同梱推奨
