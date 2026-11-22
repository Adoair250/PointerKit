# PointerKit

マウス・トラックボール・タッチパッド・キーボードの操作を便利にする、Windows 用の常駐ソフトです。
ボタンの割り当てとトラックボールでのスクロールを 1 本でこなし、インストールは不要です。

**[ダウンロード（最新版）](../../releases/latest)**　｜　Windows 10 / 11（64ビット）　｜　無料

<!-- スクリーンショット：docs/screenshot.png を置いたら下の行を有効にしてください -->
<!-- ![設定画面](docs/screenshot.png) -->

## できること

| 機能 | 内容 |
|---|---|
| ボタン割り当て | 各ボタンを、キー入力（Ctrl+W など）、戻る／進む、ウィンドウ操作、ホイール操作などに変更 |
| 長押し・2回押し | 同じボタンでも、長押し・すばやい2回押しで別の動作に |
| 同時押し | 「左を押しながら進む」のような組み合わせに動作を割り当て |
| ボールスクロール | ボタンを押しながらボールを動かして縦・横にスクロール（速さ・加速・慣性を調整可能） |
| ジェスチャー | ボタンを押したままマウスを動かして、戻る・進む・タブを閉じるなど |
| 画面の四隅 | カーソルを角に当てて、タスクビューやデスクトップ表示などを実行 |
| タッチパッドの辺 | ノートPCのタッチパッドの辺をなぞって、音量・明るさ・スクロールなどを操作 |
| US配列キーボード | Windows は日本語配列のまま、US配列キーボードで刻印どおりに入力（接続中だけ自動で切り替え） |
| そのほか | ボタン確認表示、ダークモード、設定の書き出し・読み込み |

X-Mouse Button Control と W10Wheel、USkey2JP の役割を 1 本にまとめたものです。
マウス・トラックボールの機種は問いません（左・右・中・戻る・進むの5ボタンに対応）。

## 使い方

1. [Releases](../../releases/latest) から zip をダウンロードして展開します。
2. `PointerKit.exe` を好きなフォルダに置いて起動します。
3. タスクトレイの青いアイコンをクリックすると設定画面が開きます。

初回起動時に「Windows によって PC が保護されました」と表示された場合は、「詳細情報」→「実行」を押してください（デジタル署名がないために表示されます）。

PointerKit はインターネットに接続せず、ファイルを外部へ送ることもありません。

## ビルド

Linux / WSL の mingw-w64 でビルドできます。

```sh
cd src
sh build.sh build
```

## 不具合の報告・ご要望

[Issues](../../issues) へお願いします。

## ライセンス

[MIT License](LICENSE)

---

### English

PointerKit is a portable Windows utility for mice, trackballs, touchpads and keyboards: button remapping, long-press / double-click actions, chords, trackball scrolling with inertia, mouse gestures, hot corners, touchpad edge controls and US-keyboard-on-JIS-layout conversion. The UI is Japanese only. Download the zip from [Releases](../../releases/latest).
