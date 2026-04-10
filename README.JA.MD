# MotionPngTuberPlayer

[English README](./README.md)

`MotionPngTuberPlayer` は、OBS で MotionPNGTuber を使うための **Windows 64-bit / Ubuntu 対応プラグイン** です。

現在の Windows release は **DLL だけで動作** します。Python の追加インストールは不要です。

## まずはこれだけ

### Windows release

### 1. release をダウンロードする

GitHub Releases から次のファイルをダウンロードしてください。

- `MotionPngTuberPlayer-obs-plugin-windows-x64-<version>.zip`

### 2. OBS を閉じる

OBS が起動したままだと、DLL を上書きできないことがあります。

### 3. zip の中の `obs-plugins` フォルダを OBS にコピーする

コピー先は、使っている OBS によって違います。

- 通常インストール版 OBS  
  `C:\Program Files\obs-studio\`
- portable OBS  
  portable OBS フォルダの直下

コピー後に、次の場所へ DLL が入っていれば OK です。

```text
obs-plugins\64bit\MotionPngTuberPlayer.dll
```

> `C:\Program Files\obs-studio\` に入れる場合は、Windows の管理者権限が必要です。

### 4. OBS を起動する

OBS のソース追加で `MotionPngTuberPlayer` が出てくれば、インストール成功です。

### Ubuntu build

Ubuntu は現在 **ソースから build する導線** です。repository の CI でも `ubuntu-24.04` 上で plugin の build / test / install layout 検証に加えて Linux runtime 検証まで回すようにしています。

Ubuntu 向け validation では synthetic fixture を生成し、Linux 専用経路の FFmpeg PNG decode、native `.npz` inflate / track load、native runtime の起動と描画、そして build した `.so` を libobs が load して source 作成できることまで確認します。

まず Ubuntu の build 依存を入れてください。Linux のネイティブ `.npz` 読み込みには `zlib1g-dev` が必要です。現在の CMake minimum は **3.22** です。

```bash
sudo apt install build-essential cmake pkg-config ffmpeg libobs-dev libavformat-dev libavcodec-dev libavutil-dev libswscale-dev zlib1g-dev
```

そのうえで、OBS の per-user plugin ディレクトリへ install する形で build してください。

```bash
cmake -S . -B build-linux -DMPT_BUILD_PLUGIN=ON -DMPT_INSTALL_UBUNTU_PER_USER_PLUGIN=ON
cmake --build build-linux
cmake --install build-linux --prefix "$HOME/.config/obs-studio/plugins"
```

この install 手順で、次の場所に plugin が入ります。

```text
~/.config/obs-studio/plugins/MotionPngTuberPlayer/bin/64bit/MotionPngTuberPlayer.so
~/.config/obs-studio/plugins/MotionPngTuberPlayer/data/locale/...
```

install / copy のあとに OBS を再起動してください。system 向けレイアウトで install したい場合は `-DMPT_INSTALL_UBUNTU_PER_USER_PLUGIN=OFF` を使ってください。

`libobs` ヘッダーなしで backend test だけ回したい場合も、FFmpeg と zlib の開発ヘッダーは必要です:

```bash
cmake -S . -B build-linux -DMPT_BUILD_PLUGIN=OFF -DBUILD_TESTING=ON
cmake --build build-linux --target mpt-video-backend-loop-test
ctest --test-dir build-linux --output-on-failure
```

## OBS での使い方

### 1. ソースを追加する

OBS で次の順に開きます。

1. `ソース` の `+`
2. `入力`
3. `MotionPngTuberPlayer`

### 2. 必要なファイルを指定する

基本的に必要なのは次の 3 つです。

- 口なし動画
- 口画像フォルダー
- 口パクトラック

プロパティでは次の項目を設定します。

- `loop_video`
- `mouth_dir`
- `track_file`

### 3. `loop_video` だけで自動入力されることがある

次のような関連ファイルが同じ場所にそろっている場合、`loop_video` を選ぶだけで残りが自動入力されます。

- `mouth`
- `mouth_track.json`
- `mouth_track_calibrated.json`

### 4. `Audio Sync Source` で OBS 音声ソースを選ぶ

`Audio Sync Source` で、口パクを同期させたい OBS 音声ソースを選んでください。

多くの場合は、OBS で使っているマイク入力ソースを選べば大丈夫です。

Ubuntu では、この OBS 音声ソース追従経路がサポート対象です。

## 対応しているファイル

- トラックは `.json` と `.npz` に対応
- NumPy 生成の `.npz` トラックは、通常の deflate 圧縮メンバー、streamed ZIP レイアウト、big-endian 配列、Fortran-order 配列を含めてネイティブで直接読み込みます

## うまくいかないとき

### `MotionPngTuberPlayer` が OBS に出てこない

次を確認してください。

- OBS をいったん終了してから入れ直したか
- `MotionPngTuberPlayer.dll` が `obs-plugins\64bit\` に入っているか
- 32-bit ではなく **64-bit OBS** を使っているか

Ubuntu では、次のパスも確認してください。

- `~/.config/obs-studio/plugins/MotionPngTuberPlayer/bin/64bit/MotionPngTuberPlayer.so`
- `~/.config/obs-studio/plugins/MotionPngTuberPlayer/data/locale/`

### コピーで拒否される

通常版 OBS を `C:\Program Files\obs-studio\` に入れている場合は、管理者権限が必要です。

権限が難しい場合は、portable OBS を使うと入れやすいです。

### 口が動かない

次を確認してください。

- `track_file` が正しいか
- `mouth_dir` が正しいか
- 正しい OBS 音声ソースが選ばれているか

## 現在の対応範囲

- 対応環境は **Windows 64-bit OBS** と **Ubuntu build**
- release 配布は現状 Windows 向け
- OBS の通常ソースとして使えるので、標準フィルターもそのまま使えます
- 直接入力デバイス capture はまだ Windows 向けで、Ubuntu では OBS 音声ソースを使ってください

## 開発者向けメモ

通常の利用ではここは読まなくて大丈夫です。

Windows ローカル build:

```powershell
& 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe' -S . -B build-win-fallback-vs
& 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe' --build build-win-fallback-vs --config Release
```

Windows ローカル package:

```powershell
powershell -ExecutionPolicy Bypass -File .\package-release.ps1
```

Ubuntu ローカル build:

```bash
cmake -S . -B build-linux -DMPT_BUILD_PLUGIN=ON -DMPT_INSTALL_UBUNTU_PER_USER_PLUGIN=ON
cmake --build build-linux
cmake --install build-linux --prefix "$HOME/.config/obs-studio/plugins"
```
