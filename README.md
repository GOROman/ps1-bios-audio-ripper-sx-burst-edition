# PS1 BIOS Audio Ripper SX — Burst Edition

![PS1 BIOS Audio Ripper SX Burst Edition ロゴ](docs/images/burst-edition-logo.png)

![PS1 BIOS Audio Ripper SX Burst Edition イメージキャラクター](docs/images/burst-edition-characters.png)

## このプロジェクトの目的

所有する実機PlayStationの512 KiB BIOSを、アナログオーディオ出力を使って転送し、**180秒以内にブラウザへダウンロードできるようにする**ことを目標とする高速実験版です。

PlayStation側でBIOSデータを音声信号へ変調し、ブラウザ側で受信・復調・誤り検出／訂正・BIOS再構成を行います。BIOSデータはサーバーへ送信せず、サイズおよび全体CRCの検証に成功した場合のみローカルへ保存します。

## 位置づけ

Burst Editionでは、安定動作を優先した通常版とは別に、実機の音響特性とキャプチャ環境に合わせた高速変調・復調方式を検討します。

- 目標時間: 180秒以内
- 対象データ: 初代PlayStation BIOS 512 KiB
- 送信経路: 実機のアナログ音声出力
- 受信環境: Web Audio対応ブラウザ
- 完了条件: 全ブロックとBIOS全体のCRC検証成功

ビルド成功、合成音声のループバック、エミュレータでの動作だけでは実機成功とは扱いません。最終的な達成判定は、物理PlayStationから録音した音声をブラウザで復元できた場合に行います。

## イメージキャラクター

上記ビジュアルは「PS1 BIOS Audio Ripper SX — Burst Edition」のイメージキャラクターです。

> RIP THE SOUND — PRESERVE THE LEGEND.
