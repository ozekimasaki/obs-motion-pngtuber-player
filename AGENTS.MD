# AGENTS.MD

## このプロジェクトの位置づけ

- `MotionPngTuberPlayer` は `MotionPNGTuber` の派生 OBS プラグインプロジェクトです。
- このディレクトリは **別 Git 管理前提** で進めています。
- 現在の主対象は **Windows-first の native-only OBS source** です。

## 実装状況

2026-03-22 時点で、OBS プラグインは Python worker 経路から離れ、**OBS プロセス内で直接動く native runtime** を使う構成に切り替わっています。

### 実装済み

- OBS ネイティブ入力ソース `motionpngtuber_player`
- Python なしの in-process 再生ランタイム
- 口パク用入力デバイスの選択
- 通常の OBS ソースとしての出力
  - OBS 標準フィルターをそのまま適用可能
- `loop_video` 選択時の sibling asset 自動補完
  - `mouth`
  - `mouth_track.json` / `mouth_track.npz`
  - `mouth_track_calibrated.json` / `mouth_track_calibrated.npz`
- track 互換
  - `.json`
  - `.npz` 選択時の sibling JSON 解決
  - legacy nested `quad`
  - flat `x0..x3` / `y0..y3`
- 設定保存まわりの安定化
  - property status 表示の race 修正
  - `waveIn` teardown 競合修正
- 動画ループ境界の改善
  - 出力 timestamp を単調増加に変更
- 軽量化の一部実施
  - `render_fps` のみの変更では runtime を再構築しない
  - track 読み込み時に warp 用の bounding box / inverse homography を前計算し、描画 hot path の毎フレーム計算を削減
  - bilinear sampling を 4ch まとめて処理するようにして mouth warp 時の重複計算を削減
- 日本語 UI 改善
  - `ja-JP.ini` 整備
  - DLL 内蔵の英日 fallback を追加し、locale 読み込み失敗時も raw key 表示を避けやすくした
- release hardening
  - `waveIn` handle を atomic 化し、audio start / buffer requeue の失敗時を少し硬くした
  - frame buffer resize 失敗時に properties / log へ error を出すようにした
  - `install-to-obs.ps1` で 64-bit OBS 配置先チェックを追加
- packaging / CI
  - `package-release.ps1` を CMake install ベースに寄せ、Windows / Linux / macOS の package 導線を共有化
  - `release-windows.ps1` で Windows package の build / tag / GitHub release 作成をローカル自動化
  - `.github\workflows\build-release.yml` で Windows / Linux / macOS build + package + OBS smoke test + release publish を統合
  - tag push ベースの GitHub Actions release publish は Windows / Linux / macOS の smoke test 成功後のみ動く
  - Linux / macOS release asset 名から `-stub` を外した
  - macOS build は repo 内 fallback OBS header + `OBS.app` 同梱 `libobs` で通す形を維持
  - 日本語向け `README.JA.MD` を追加し、README 導線も追加
- macOS / Linux 互換化の下地
  - audio device の列挙 / identity 解決を `mpt-audio-backend.*` に切り出し始めた
  - PNG sprite load を `mpt-image-backend.*` に切り出し始めた
  - loop video decode を `mpt-video-backend.*` に切り出し始めた
  - `ci\build-nonwindows.sh` と `pkg-config libobs` fallback で Linux / macOS build 導線を追加
  - `motionpngtuber-native.cpp` の track / sprite / render 側を共通 runtime として non-Windows でも compile できる形へ寄せた
  - non-Windows audio backend に PortAudio 実装を追加
  - non-Windows image backend に `libpng` 実装を追加
  - non-Windows video backend に FFmpeg 実装を追加
  - GitHub Actions の Linux / macOS build に `libpng` / FFmpeg / PortAudio 依存を追加
  - `ci\generate-smoke-assets.py` / `ci\detect-obs-layout.py` / `ci\obs-websocket-smoke.py` で cross-platform OBS smoke test を追加

### 現在の backend 構成

- Media Foundation
  - Windows の loop 動画 decode
- Windows Imaging Component (WIC)
  - Windows の口スプライト PNG decode
- WinMM `waveIn`
  - Windows のソースごとの音声入力 capture
- FFmpeg
  - 現在の non-Windows loop 動画 decode
- `libpng`
  - 現在の non-Windows 口スプライト PNG decode
- PortAudio
  - 現在の non-Windows 音声入力 device 列挙 / capture

## 検証済み

portable OBS 32.0.4 で次を確認済みです。

- module load 成功
- source create 成功
- source screenshot 取得成功
- `crop_filter` の attach 成功
- OBS properties 経由の audio device list 取得成功
- `.npz` 選択時の sibling JSON 解決で描画継続
- loop video だけ指定した場合の auto-fill
- `SetInputSettings` を繰り返す save-stress test 通過
- 最新の release hardening 後も portable OBS 32.0.4 で source create / screenshot / 複数回の `SetInputSettings` + screenshot を再確認済み

GitHub Actions の OBS smoke test では Windows / Linux / macOS で次を確認します。

- `motionpngtuber_player` source create
- audio device property list の取得
- `crop_filter` 適用後に source screenshot の出力サイズが変わること
- `SetInputSettings` 更新後の readback
- OBS restart 後も source / filter / settings が残ること

ビルド済み DLL:

- `build-win-fallback-vs\Release\MotionPngTuberPlayer.dll`

## 既知の制約

- 手元での長時間検証は **Windows が中心**
- Linux / macOS は GitHub Actions の OBS smoke test までは入ったが、手元実機での長時間運用検証量はまだ少ない
- `.npz` は直接パースではなく、**sibling JSON がある前提の互換対応**
- 通常インストール版 OBS (`C:\Program Files\obs-studio`) へ配置する場合は管理者権限が必要
- DLL 内 fallback はあるが、配布物としては `data\obs-plugins\MotionPngTuberPlayer\locale\` 同梱を推奨

## 重要ファイル

- `src\motionpngtuber-source.c`
  - OBS source 実装本体
  - properties UI
  - auto-fill
  - update / runtime rebuild 制御
- `src\motionpngtuber-native.cpp`
  - 共通 runtime core
  - track load
  - mouth 合成
  - Windows audio capture
- `src\mpt-text.cpp`
  - locale 読み込み失敗時の英日埋め込み fallback
- `data\locale\ja-JP.ini`
  - 日本語 locale
- `install-to-obs.ps1`
  - DLL と data をまとめて OBS へ配置するスクリプト
- `package-release.ps1`
  - 配布用フォルダ / zip を組み立てるスクリプト
- `ci\build-nonwindows.sh`
  - Linux / macOS build helper
- `ci\obs-websocket-smoke.py`
  - OBS websocket ベースの smoke test 本体
- `ci\generate-smoke-assets.py`
  - CI で使う最小 loop video / mouth / track asset 生成
- `ci\detect-obs-layout.py`
  - Linux / macOS の OBS executable / plugin path 検出
- `release-windows.ps1`
  - Windows build + package + tag push + GitHub release 作成スクリプト
- `.github\workflows\build-release.yml`
  - Windows / Linux / macOS CI build / package / OBS smoke / release

## ビルドと配置

Windows fallback build:

```powershell
& 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe' --build 'build-win-fallback-vs' --config Release
```

OBS への配置:

```powershell
powershell -ExecutionPolicy Bypass -File .\install-to-obs.ps1
```

portable OBS へ配置する場合:

```powershell
powershell -ExecutionPolicy Bypass -File .\install-to-obs.ps1 -ObsRoot .\obs-portable-test
```

## 次に着手しやすい項目

- signing / notarization
- より広い playback / seek / long-run QA
- 追加の OBS filter coverage
- 追加の軽量化レビュー
  - warp / blend
  - decode cadence
  - audio capture cadence
- runtime 再構築をさらに減らす設定更新経路の細分化

## 補足

- 旧 Python worker / IPC 前提の履歴は session plan に残っていますが、**現行 Windows 実装の主経路ではありません**。
- 現在の優先対象はこの `MotionPngTuberPlayer` プロジェクトの完成度向上です。
