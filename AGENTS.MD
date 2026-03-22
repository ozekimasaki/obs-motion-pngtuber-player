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
  - `package-release.ps1` で OBS 配布レイアウトと zip をローカル生成できるようにした
  - `.github\workflows\windows-ci.yml` で Windows CI build + package artifact upload を追加
- macOS / Linux 互換化の下地
  - audio device の列挙 / identity 解決を `mpt-audio-backend.*` に切り出し始めた
  - PNG sprite load を `mpt-image-backend.*` に切り出し始めた
  - 今後の共通 core + OS backend 分離の最初の足場

### 現在の Windows 実装で使っているもの

- Media Foundation
  - loop 動画 decode
- Windows Imaging Component (WIC)
  - 口スプライト PNG decode
- WinMM `waveIn`
  - ソースごとの音声入力 capture

## 検証済み

portable OBS 32.0.4 で次を確認済みです。

- module load 成功
- source create 成功
- source screenshot 取得成功
- `crop_filter` の attach 成功
- `.npz` 選択時の sibling JSON 解決で描画継続
- loop video だけ指定した場合の auto-fill
- `SetInputSettings` を繰り返す save-stress test 通過
- 最新の release hardening 後も portable OBS 32.0.4 で source create / screenshot / 複数回の `SetInputSettings` + screenshot を再確認済み

ビルド済み DLL:

- `build-win-fallback-vs\Release\MotionPngTuberPlayer.dll`

## 既知の制約

- 実機検証は **Windows が中心**
- macOS / Linux は未ビルド・未検証
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
  - Windows native runtime 本体
  - 動画 decode
  - track load
  - 音声入力
  - mouth 合成
- `src\mpt-text.cpp`
  - locale 読み込み失敗時の英日埋め込み fallback
- `data\locale\ja-JP.ini`
  - 日本語 locale
- `install-to-obs.ps1`
  - DLL と data をまとめて OBS へ配置するスクリプト
- `package-release.ps1`
  - 配布用フォルダ / zip を組み立てるスクリプト
- `.github\workflows\windows-ci.yml`
  - Windows CI build と package artifact upload

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

- macOS / Linux の実ビルド確認
- signing / release automation
- 追加の軽量化レビュー
  - warp / blend
  - decode cadence
  - audio capture cadence
- runtime 再構築をさらに減らす設定更新経路の細分化

## 補足

- 旧 Python worker / IPC 前提の履歴は session plan に残っていますが、**現行 Windows 実装の主経路ではありません**。
- 現在の優先対象はこの `MotionPngTuberPlayer` プロジェクトの完成度向上です。
