# MotionPngTuberPlayer

[English README](./README.md)

`MotionPngTuberPlayer` は、OBS で MotionPNGTuber を使うための **Windows 64-bit 専用プラグイン** です。

いまの release は **DLL だけで動作** します。Python の追加インストールは不要です。

## まずはこれだけ

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

## 対応しているファイル

- トラックは `.json` と `.npz` に対応
- 標準的な `.npz` トラックはネイティブで直接読み込みます
- もし `.npz` が未対応の ZIP 形式なら、同じ場所に `mouth_track.json` を置くとフォールバックできます

## うまくいかないとき

### `MotionPngTuberPlayer` が OBS に出てこない

次を確認してください。

- OBS をいったん終了してから入れ直したか
- `MotionPngTuberPlayer.dll` が `obs-plugins\64bit\` に入っているか
- 32-bit ではなく **64-bit OBS** を使っているか

### コピーで拒否される

通常版 OBS を `C:\Program Files\obs-studio\` に入れている場合は、管理者権限が必要です。

権限が難しい場合は、portable OBS を使うと入れやすいです。

### 口が動かない

次を確認してください。

- `track_file` が正しいか
- `mouth_dir` が正しいか
- 正しい OBS 音声ソースが選ばれているか

## 現在の対応範囲

- 対応環境は **Windows 64-bit OBS**
- release は DLL-only
- OBS の通常ソースとして使えるので、標準フィルターもそのまま使えます

## 開発者向けメモ

通常の利用ではここは読まなくて大丈夫です。

ローカル build:

```powershell
& 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe' -S . -B build-win-fallback-vs
& 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe' --build build-win-fallback-vs --config Release
```

ローカル package:

```powershell
powershell -ExecutionPolicy Bypass -File .\package-release.ps1
```
