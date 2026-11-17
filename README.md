# ZEN ファームウェア

ZEN用のファームウェアです。このファームウェアは **ZEN Configurator** に対応しています。

- ZEN Configurator: https://e24-gh.github.io/zen/configurator/

Configuratorは、ZENをUSBでつないでブラウザから設定する画面です。キー配置とトラックボールの設定を、GitHubを使わずに変更できます。Chrome または Edge でお使いください。

## 2つの使い方

### かんたん: ファームウェアを書き込んで、Configuratorで設定する

GitHubの操作は必要ありません。

1. Releases から ZIP をダウンロードします
2. 右手と左手の対応コントローラへUF2ファイルを書き込みます
3. 右手をUSBでつなぎ、Configuratorを開いて設定します

### 応用: forkして、コンボやマクロも編集する

コンボとマクロはファームウェアに焼き込まれるため、Configuratorからは変更できません。これらを使いたい場合はこちらです。

1. このリポジトリを自分のGitHubアカウントへforkします
2. `config/keymap.keymap` を編集します
3. GitHub Actionsでファームウェアがbuildされるのを待ちます
4. `firmware` のダウンロードボタンからzipファイルをダウンロードします
5. UF2ファイルを書き込みます

キー配置そのものは、この方法で書き込んだあとでもConfiguratorから変更できます。

> **キー配置はConfigurator、コンボとマクロは `config/keymap.keymap`。**
> 同じキーを両方で変更すると、Configuratorで設定した内容が優先されます。
> `config/keymap.keymap` 側の変更を反映させたいときは、先にConfiguratorの
> 「出荷時の設定に戻す」を実行してください。

## 書き込むファイル

ZIPおよびGitHub Actionsで作成される `firmware` には、以下のUF2ファイルが含まれます。

- 設定リセット用UF2ファイル
- `zen_right_trackball_pmw3610_central.uf2`
- `zen_right_trackball_paw3222_central.uf2`
- `zen_right_trackpad_central.uf2`
- `zen_left_peripheral.uf2`
- `zen_left_trackpad_peripheral.uf2`

標準構成では以下を使用します。

- 右手: `zen_right_trackball_pmw3610_central.uf2`
- 左手: `zen_left_peripheral.uf2`

右手にPAW3222やTrackpadを使用する場合は、それぞれ名前に対応する右手用UF2ファイルを使用します。左手にTrackpadを付けている場合は `zen_left_trackpad_peripheral.uf2` を使用します。

## 設定を元に戻したいとき

Configuratorの「出荷時の設定に戻す」で、この画面から変更したキー配置とマウス操作をキーボードから消せます。ファームウェアに入っている元の設定に戻ります。無線の登録は消えません。

無線接続そのものがうまくいかない場合は、設定リセット用のUF2ファイルを使います。こちらは無線の登録も消えます。

1. PCやMacの接続設定から `ZEN` を削除します
2. 設定リセット用のUF2ファイルを書き込みます
3. USBを外します
4. ZENの電源を入れます
5. もう一度通常ファームウェアを書き込みます
6. 接続設定から `ZEN` を再接続します

## 以前のリポジトリをお使いの方へ

[zen-firmware](https://github.com/E24-GH/zen-firmware) で作成したファームウェアはConfiguratorに対応していません。こちらへ移行する場合は、`config/keymap.keymap` の内容をコピーして移してください。コンボとマクロも同じファイルに入っているため、ファイルごと移せばそのまま引き継がれます。

`config/keymap.keymap` 以外（`boards/shields/zen/*.conf` など）も変更している場合は、その差分もあわせてご確認ください。

## 対象

PMW3610トラックボール版を標準構成とし、左右それぞれでPAW3222およびTrackpadを使用する構成にも対応しています。

## ライセンス

MIT
