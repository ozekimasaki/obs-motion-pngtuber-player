# AGENTS.MD

## このプロジェクトの位置づけ

- `MotionPngTuberPlayer` は `MotionPNGTuber` の派生 OBS プラグインプロジェクトです。
- このディレクトリは **別 Git 管理前提** で進めています。
- 現在の主対象は **Windows native OBS source** と **Ubuntu build 対応 OBS source** です。
- Windows release 導線は維持しますが、Ubuntu では source build をサポート対象に含めます。

## 実装状況

2026-03-22 時点で、OBS プラグインは Python worker 経路から離れ、**OBS プロセス内で直接動く native runtime** を使う構成です。

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
  - `package-release.ps1` は Windows package 作成用の CMake install ラッパー
  - `release-windows.ps1` で Windows package の build / tag / GitHub release 作成をローカル自動化
  - `.github\workflows\build-release.yml` で Windows build + package + OBS smoke test + release publish を回す
  - tag push ベースの GitHub Actions release publish は Windows smoke test 成功後のみ動く
  - 日本語向け `README.JA.MD` を追加し、README 導線も維持
  - Ubuntu 向けに OBS per-user plugin layout (`~/.config/obs-studio/plugins/<plugin>/...`) へ install できる CMake option を追加

### 現在の backend 構成

- Media Foundation
  - Windows の loop 動画 decode
- Windows Imaging Component (WIC)
  - Windows の口スプライト PNG decode
- WinMM `waveIn`
  - Windows のソースごとの音声入力 capture
- FFmpeg
  - Ubuntu の loop 動画 decode
  - Ubuntu の口スプライト PNG decode
- OBS audio source follow
  - Ubuntu の口パク音声入力の主経路

## 検証済み

portable OBS 32.0.4 と GitHub Actions Windows smoke で次を確認済みです。

- module load 成功
- source create 成功
- source screenshot 取得成功
- `crop_filter` の attach 成功
- OBS properties 経由の audio device list 取得成功
- `.npz` 選択時の sibling JSON 解決で描画継続
- loop video だけ指定した場合の auto-fill
- `SetInputSettings` を繰り返す save-stress test 通過
- OBS restart 後も source / filter / settings が残ること

ビルド済み DLL:

- `build-win-fallback-vs\Release\MotionPngTuberPlayer.dll`

## 既知の制約

- Windows release / smoke / packaging は引き続き Windows 中心
- Ubuntu は **source build 対応** で、配布 zip / release automation は未整備
- Ubuntu で `.npz` track を使う build には `zlib1g-dev` 相当の zlib 開発ヘッダーが必要
- `.npz` は直接パースではなく、**sibling JSON がある前提の互換対応**
- 通常インストール版 OBS (`C:\Program Files\obs-studio`) へ配置する場合は管理者権限が必要
- DLL に英日 fallback text を内蔵しているため、release 配布物は DLL 単体で自己完結する
- direct input device capture は現状 Windows backend のみで、Ubuntu では OBS 音声ソース追従を使う

## 重要ファイル

- `src\motionpngtuber-source.c`
  - OBS source 実装本体
  - properties UI
  - auto-fill
  - update / runtime rebuild 制御
- `src\motionpngtuber-native.cpp`
  - native runtime core
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
- `release-windows.ps1`
  - Windows build + package + tag push + GitHub release 作成スクリプト
- `ci\configure-obs-websocket.py`
  - OBS websocket 設定ファイルの生成
- `ci\generate-smoke-assets.py`
  - CI で使う最小 loop video / mouth / track asset 生成
- `ci\obs-websocket-smoke.py`
  - OBS websocket ベースの smoke test 本体
- `.github\workflows\build-release.yml`
  - Windows CI build / package / OBS smoke / release

## ビルドと配置

Windows fallback build:

```powershell
& 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe' -S . -B build-win-fallback-vs
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

Ubuntu build:

```bash
sudo apt install build-essential cmake pkg-config ffmpeg libobs-dev libavformat-dev libavcodec-dev libavutil-dev libswscale-dev zlib1g-dev
cmake -S . -B build-linux -DMPT_BUILD_PLUGIN=ON -DMPT_INSTALL_UBUNTU_PER_USER_PLUGIN=ON
cmake --build build-linux
cmake --install build-linux --prefix "$HOME/.config/obs-studio/plugins"
```

`libobs` 開発ファイルがない環境で backend test だけ回す場合:

```bash
cmake -S . -B build-linux -DMPT_BUILD_PLUGIN=OFF -DBUILD_TESTING=ON
cmake --build build-linux --target mpt-video-backend-loop-test
ctest --test-dir build-linux --output-on-failure
```

## 次に着手しやすい項目

- Windows release の signing
- より広い playback / seek / long-run QA
- 追加の OBS filter coverage
- 追加の軽量化レビュー
  - warp / blend
  - decode cadence
  - audio capture cadence
- runtime 再構築をさらに減らす設定更新経路の細分化

## 補足

- 旧 Python worker / IPC 前提の履歴は session plan に残っていますが、**現行 Windows 実装の主経路ではありません**。
- tree 内に non-Windows 向けの痕跡が残っていても、現行の supported build / smoke / release path には含めません。
