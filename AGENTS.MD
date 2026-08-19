# AGENTS.MD

このリポジトリでコーディングエージェントが作業するためのガイドです。人間向けの概要・使い方は [README.md](./README.md) / [README.JA.MD](./README.JA.MD) を参照してください。

## このプロジェクトの位置づけ

- `MotionPngTuberPlayer` は `MotionPNGTuber` の派生 OBS プラグインプロジェクトです。
- 現在の主対象は **Windows 64-bit の native-only OBS input source** です。
- 再生は **OBS プロセス内で直接動く native runtime** で行い、release 配布物は **DLL 単体** で自己完結します（Python 不要）。
- Linux / macOS 向けの build・smoke・release 導線は現行サポート対象外です。`CMakeLists.txt` は非 Windows ホストで `FATAL_ERROR` を出して停止します。

## プロジェクト構成 / エントリポイント

- `src/plugin-main.c`
  - OBS module エントリ (`obs_module_load` など) と source の登録
- `src/motionpngtuber-source.c`
  - OBS input source 本体 (`motionpngtuber_player`)
  - properties UI、sibling asset 自動補完、update / runtime rebuild 制御
  - property キーは `PROP_*` マクロで定義（`loop_video` / `mouth_dir` / `track_file` / `track_calibrated_file` / `render_fps` / `audio_sync_source_uuid` / `valid_policy` / `show_advanced` など）
- `src/motionpngtuber-native.cpp` / `.h`
  - native runtime core：track load、`.npz` パース、mouth 合成 (warp / bilinear blend)、Windows audio capture の統合
- `src/mpt-video-backend.cpp` / `.h`
  - Media Foundation による loop 動画 decode
- `src/mpt-image-backend.cpp` / `.h`
  - Windows Imaging Component (WIC) による口スプライト PNG decode
- `src/mpt-audio-backend.cpp` / `.h`
  - WinMM `waveIn` によるソースごとの音声入力 capture
- `src/mpt-text.cpp` / `.h`
  - locale 読み込み失敗時の英日埋め込み fallback
- `src/mpt-obs-util.c` / `.h`
  - fallback build (`MPT_FALLBACK_OBS`) 時にのみリンクされる OBS ユーティリティ補完
- `src/plugin-support.c.in` / `plugin-support.h`
  - `configure_file` で生成する plugin 名・バージョン情報
- `data/locale/en-US.ini` / `ja-JP.ini`
  - OBS locale。UI 文字列はここと `mpt-text.cpp` の fallback を一致させる
- `tests/`
  - `mpt-video-backend-reader-test.cpp` / `mpt-video-backend-loop-test.cpp`（C++）と `obs_websocket_smoke_test.py`（Python unittest）
- `ci/`
  - `install-obs.ps1`（OBS 導入）、`resolve-ffmpeg.ps1`、`generate-smoke-assets.py`（最小 loop video / mouth / track 生成）、`configure-obs-websocket.py`、`obs-websocket-smoke.py`（smoke test 本体）
- `cmake/generate_obs_import_lib.ps1`
  - fallback build で installed OBS の `obs.dll` から import library を生成
- `*.ps1`（リポジトリ直下）
  - `install-to-obs.ps1`（DLL / data を OBS へ配置）、`package-release.ps1`（配布フォルダ / zip 作成）、`release-windows.ps1`（build + package + tag push + GitHub release）
- `.github/workflows/build-release.yml`
  - Windows build / package / OBS smoke / release publish
- `python/`
  - **旧 Python worker 経路の legacy コード**。現行 native 実装の build / smoke / release path には含まれず、`CMakeLists.txt` からも参照されません。

## セットアップ

- OS：**Windows 64-bit** が必須（他 OS では configure が失敗）。
- 依存：
  - Visual Studio 2022 の C++ tools（`dumpbin.exe` / `lib.exe` を fallback build で使用）
  - CMake `>= 3.28`
  - 64-bit OBS Studio（`libobs` CMake package が無い場合は installed OBS の `obs.dll` から import lib を生成）
- `.npz` の deflate 圧縮メンバー展開には OBS 同梱の `zlib.dll` を実行時 load します。
- smoke / 一部テスト用に Python 3 と `ffmpeg` があると追加テストが有効化されます（無い場合は該当テストが skip されます）。
- pre-commit / husky 等の git hook 設定はありません。

## ビルド / テスト / lint / typecheck

### ビルド（Windows fallback build）

```powershell
cmake -S . -B build-win-fallback-vs -DMPT_OBS_ROOT="C:\Program Files\obs-studio"
cmake --build build-win-fallback-vs --config Release
```

- 生成物：`build-win-fallback-vs\Release\MotionPngTuberPlayer.dll`
- `libobs` package が見つかる環境では `-DMPT_OBS_ROOT` は不要です。

### テスト（CTest）

テストは `-DBUILD_TESTING=ON` で configure した場合に有効になります。

```powershell
cmake -S . -B build-win-fallback-vs -DMPT_OBS_ROOT="C:\Program Files\obs-studio" -DBUILD_TESTING=ON
cmake --build build-win-fallback-vs --config Release
ctest --test-dir build-win-fallback-vs -C Release --output-on-failure
```

- `mpt-video-backend-reader`：Media Foundation reader の単体テスト
- `mpt-video-backend-loop`：loop 境界テスト（`ffmpeg` が PATH にある場合のみ追加）
- `obs-websocket-smoke-unit`：`obs_websocket_smoke_test.py` の Python unittest（Python 3 が見つかる場合のみ追加）

### lint / typecheck

- 専用の lint / typecheck / formatter 設定（clang-format・.editorconfig 等）は現状ありません。
- 変更時は周辺コードのスタイルに合わせてください（下記「コーディング規約」）。

### OBS への配置 / パッケージング

```powershell
powershell -ExecutionPolicy Bypass -File .\install-to-obs.ps1
powershell -ExecutionPolicy Bypass -File .\install-to-obs.ps1 -ObsRoot .\obs-portable-test
powershell -ExecutionPolicy Bypass -File .\package-release.ps1
```

## コーディング規約

- インデントは **タブ**。既存ファイルの整形（改行・波括弧位置）に合わせる。
- 言語は C（`.c`）と C++17（`.cpp`）の混在。OBS と接する source 層は C、runtime / backend は C++。
- プラグイン固有のシンボルは `mpt_` / `MPT_` / `PROP_` などの接頭辞を踏襲する。
- UI 文字列は `mpt_text("MotionPngTuberPlayer.*")` 経由で参照し、`data/locale/en-US.ini`・`ja-JP.ini`・`mpt-text.cpp` の fallback を同時に更新して齟齬を出さない。
- Windows API / OBS API のリソースは既存の解放パターン（`obs_data_release` / `bfree` / handle の close 等）に従う。
- スレッド共有状態は `context->mutex` 等の既存ロック規約を守る（properties 表示や `waveIn` teardown に race 修正が入っている）。

## 実装状況（実装済み）

- OBS ネイティブ入力ソース `motionpngtuber_player`
- Python なしの in-process 再生ランタイム
- 口パク用音声ソースの選択（OBS audio source、advanced で直接入力デバイスも選択可）
- 通常の OBS ソースとしての出力（OBS 標準フィルターをそのまま適用可能）
- `loop_video` 選択時の sibling asset 自動補完（`mouth` / `mouth_track.*` / `mouth_track_calibrated.*`、`.json` / `.npz`）
- track 互換
  - `.json`
  - `.npz` の**ネイティブ直接パース**（OBS `zlib.dll` による raw deflate 展開、streamed ZIP レイアウト、big-endian 配列、Fortran-order 配列に対応）
  - legacy nested `quad` / flat `x0..x3` / `y0..y3`
- 設定保存まわりの安定化（property status 表示の race 修正、`waveIn` teardown 競合修正）
- 動画ループ境界の改善（出力 timestamp を単調増加に）
- 描画 hot path の軽量化（warp 用 bounding box / inverse homography 前計算、bilinear sampling の 4ch まとめ処理、`render_fps` のみ変更時は runtime を再構築しない）
- 日本語 UI 整備（`ja-JP.ini` と DLL 内蔵の英日 fallback）
- release hardening（`waveIn` handle の atomic 化、frame buffer resize 失敗時の error 出力、`install-to-obs.ps1` の 64-bit 配置先チェック）
- packaging / CI（`package-release.ps1` / `release-windows.ps1` / `.github/workflows/build-release.yml`）

### backend 構成

- Media Foundation：loop 動画 decode
- Windows Imaging Component (WIC)：口スプライト PNG decode
- WinMM `waveIn`：ソースごとの音声入力 capture
- OBS `zlib.dll`：`.npz` の deflate メンバー展開（実行時 load）

## 検証済み

portable OBS 32.0.4 と GitHub Actions Windows smoke で確認済み：

- module load / source create / source screenshot 取得
- `crop_filter` の attach
- OBS properties 経由の audio device list 取得
- `.npz` track のネイティブ読み込みで描画継続
- loop video だけ指定した場合の auto-fill
- `SetInputSettings` を繰り返す save-stress test 通過
- OBS restart 後も source / filter / settings が残ること

ビルド済み DLL：`build-win-fallback-vs\Release\MotionPngTuberPlayer.dll`

## 注意点 / 既知の制約

- サポート対象は **Windows 64-bit OBS のみ**。Linux / macOS の build・smoke・release は現行方針では対象外。
- 通常インストール版 OBS (`C:\Program Files\obs-studio`) へ配置する場合は管理者権限が必要。
- release 配布物は DLL 単体で自己完結（英日 fallback text を DLL に内蔵）。
- `python/` は legacy であり、現行 native 実装の主経路ではない。tree 内に non-Windows / Python worker の痕跡が残っていても supported path には含めない。
- tag push (`v*`) ベースの GitHub Actions release publish は Windows smoke test 成功後のみ動く。

## 次に着手しやすい項目

- Windows release の signing
- より広い playback / seek / long-run QA
- 追加の OBS filter coverage
- warp / blend / decode cadence / audio capture cadence の軽量化レビュー
- runtime 再構築をさらに減らす設定更新経路の細分化
