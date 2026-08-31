# PS1 BIOS Audio Ripper SX Burst — compression sweep

測定日: 2026-08-31

## 結論

現行の独立ブロック + fixed-Deflate 構成では、32 KiB ブロック / match-chain 192 が最小だった。

- SX表示サイズ: **292,489 bytes**
- 入力: 524,288 bytes
- 200,000 bytes目標との差: **+92,489 bytes**（約31.6%）
- したがって、ブロックサイズと探索深さの調整だけでは200 KBには届かない

16 KiBの現行基準値は297,729 bytesで、32 KiB / chain 192への改善は5,240 bytes（約1.8%）だった。

## ベンチマーク結果

`chain` は fixed-Deflate の一致検索深さ。`SX` は各PS1実行ファイルをDuckStationで動かし、画面に表示されたコンテナ格納サイズ。`DFL/LZS/RAW` はブロックごとのcodec選択数。

| コンテナブロック | chain | SX | ブロック数 | DFL/LZS/RAW | 画面表示率 |
|---:|---:|---:|---:|---:|---:|
| 4 KiB | 96 | 314,551 | 128 | 127/0/1 | 59% |
| 8 KiB | 96 | 304,380 | 64 | 64/0/0 | 58% |
| 16 KiB | 24 | 298,752 | 32 | 32/0/0 | 56% |
| 16 KiB | 96 | 297,729 | 32 | 32/0/0 | 56% |
| 16 KiB | 192 | 297,555 | 32 | 32/0/0 | 56% |
| 32 KiB | 24 | 294,015 | 16 | 16/0/0 | 56% |
| 32 KiB | 96 | 292,768 | 16 | 16/0/0 | 55% |
| **32 KiB** | **192** | **292,489** | **16** | **16/0/0** | **55%** |

## 実行条件

- Mac Studio上でDocker版PSn00bSDKツールチェーンを使用
- `luksamuk/psxtoolchain:latest`
- 24 physical / 32 logical CPU
- ブロックサイズ4条件を並列投入。chain sweepは6条件を並列投入し、各ビルドは`-j24`
- 16 KiB / chain 24だけGCC内部エラーが出たため、同じ設定を`-j1`で再ビルドして測定
- 生成した各CUEを、ローカルMacのDuckStation実行ファイルへ直接渡して確認
- 圧縮・コンテナ比較中はwire V6のステレオOFDM、16QAM、96 carriers、FEC 16+6、FSKなしを維持

## 解釈

4 KiBはブロック境界のリセットが多く、8 KiBも16 KiBより大きかった。16 KiB / chain 96から32 KiB / chain 192にすると、実測で5,240 bytes縮む。一方、chainを96から192に増やした効果は16 KiBで174 bytes、32 KiBで279 bytesにとどまる。

ヘッダだけの差は、16 KiBの32ブロックから32 KiBの16ブロックにして256 bytes減るだけ。主な差は、ブロックごとの圧縮履歴リセットとfixed Huffmanの表現力であり、探索深さを増やすだけでは大幅な短縮にならない。

なお、画面に表示される`BLOCK SIZE 32 KiB`はOFDM送信側のtransport chunkであり、表の「コンテナブロック」とは別の設定である。

## 検証範囲

- **確認済み:** Mac StudioでのPS1ビルド、各バリアントのDuckStation実行、画面上のcodec選択数・SX格納サイズ
- **エミュレーターのみ:** DuckStation上の結果であり、実機のSPU音声出力・CD-R・録音機器を通した受信ではない
- **未検証:** 物理PlayStationからの音声キャプチャ、実機での転送時間、実機受信成功率
- BIOS本体、復元ダンプ、鍵、CRCは保存・公開していない

## 次の候補

200 KBを狙うには、32 KiB境界の微調整ではなく、dynamic Huffmanと、独立ブロック復旧要件を見直した連続Deflate履歴の比較が必要。これは復旧単位とwire/container versionに影響するため、現行decoder互換を保った別バージョンの実験として扱う。

## 追加: MIPS／リソース別アルゴリズム sweep

Gistの静的解析結果を使い、次のヒューリスティックな領域分割を追加した。

- MIPS-ish: `0x000000–0x044C60`（常駐BIOSとシェルコード側）
- resource-ish: `0x044C60–0x080000`（UI TIM、起動ロゴTIM、フォント／ビットマップ／テーブル側）
- 別ジョブでは、UI TIM `0x044C60–0x053D38` とロゴTIM `0x05FA40–0x062E30` も境界として分割

ホスト専用の `tools/codec_sweep.py` で、LZSS、custom fixed-Deflate、zlib dynamic/fixed/RLE/Huffman、BZip2、LZMA2、4-byte byte-plane、word XOR/delta、bit-plane、MIPS opcode/rest field splitを比較した。

- **176ジョブ / 24ワーカー / 176 PASS**（全候補で復元一致）
- 最良のarea候補: **198,407 bytes**
- MIPS area: **LZMA2、`lp=2`／`lc=2`、dict 256 KiB**（4-byte alignmentを反映）
- resource area: **通常のLZMA2、dict 256 KiB**（512 KiB以上と同じサイズ）
- 2 areaのpayloadは198,347 bytes、area header見積りは60 bytes
- 全体を1本にしたLZMA2 streamは199,109 bytesだが、独立復旧containerではないlower bound

単純なMIPS byte-plane／field分離はこのBIOSでは悪化した。MIPS-ish領域は、命令を並べ替えるより、4-byte位置モデルを持つLZMAの方が有効だった。リソース側をdynamic DeflateやBZip2に替える組み合わせは、LZMA2 resourceより大きくなった。

この結果は**ホストcodec選別の候補**であり、現行PS1／ブラウザdecoderではLZMA2をまだ受け付けない。wire V6、現行container version、PS1実機処理は変更していない。Mac Studio上のBIOS一時コピーは測定後に削除済みで、公開物にはBIOSデータを含めていない。


## V2 C++基準実装の実測

tools/sx_v2_lzma.cppをMac StudioのHomebrew liblzmaで実行し、MIPS／リソースの2領域をV2ヘッダ付きで実際にパッケージした。

- 生成データの圧縮・復元・領域CRC・全体CRC: **PASS**
- 実BIOSのV2コンテナ: **197,887 bytes**
- V2実ヘッダ構成: 32-byte header + 2 × 28-byte area header
- これはホスト基準実装の値で、PS1実行時エンコーダとブラウザLZMA2復号が未実装のため、現行wireへはまだ切り替えていない

## 全176条件の結果一覧

以下はMac Studio上のPython sweep（24 worker、176/176 PASS）の全結果。total_bytesは、independent／regions／areasでは現行SXの28-byte headerと16-byte segment headerを加えた比較値、streamは独立復旧ヘッダを含まないlower-boundである。

| No. | total bytes | payload bytes | mode | condition | verify |
|---:|---:|---:|---|---|---|
| 1 | 198407 | 198347 | areas | areas-mips-lzma-mips1024-resource-lzma1024-plain | PASS |
| 2 | 198407 | 198347 | areas | areas-mips-lzma-mips1024-resource-lzma256-plain | PASS |
| 3 | 198407 | 198347 | areas | areas-mips-lzma-mips1024-resource-lzma512-plain | PASS |
| 4 | 198407 | 198347 | areas | areas-mips-lzma-mips256-resource-lzma1024-plain | PASS |
| 5 | 198407 | 198347 | areas | areas-mips-lzma-mips256-resource-lzma256-plain | PASS |
| 6 | 198407 | 198347 | areas | areas-mips-lzma-mips256-resource-lzma512-plain | PASS |
| 7 | 198407 | 198347 | areas | areas-mips-lzma-mips512-resource-lzma1024-plain | PASS |
| 8 | 198407 | 198347 | areas | areas-mips-lzma-mips512-resource-lzma256-plain | PASS |
| 9 | 198407 | 198347 | areas | areas-mips-lzma-mips512-resource-lzma512-plain | PASS |
| 10 | 198428 | 198368 | areas | areas-mips-lzma-mips1024-resource-lzma128-plain | PASS |
| 11 | 198428 | 198368 | areas | areas-mips-lzma-mips256-resource-lzma128-plain | PASS |
| 12 | 198428 | 198368 | areas | areas-mips-lzma-mips512-resource-lzma128-plain | PASS |
| 13 | 199095 | 199035 | areas | areas-mips-lzma-mips1024-resource-lzma64-plain | PASS |
| 14 | 199095 | 199035 | areas | areas-mips-lzma-mips256-resource-lzma64-plain | PASS |
| 15 | 199095 | 199035 | areas | areas-mips-lzma-mips512-resource-lzma64-plain | PASS |
| 16 | 199109 | 199081 | stream | lzma256-stream-l9 | PASS |
| 17 | 199518 | 199458 | areas | areas-mips-lzma-mips128-resource-lzma1024-plain | PASS |
| 18 | 199518 | 199458 | areas | areas-mips-lzma-mips128-resource-lzma256-plain | PASS |
| 19 | 199518 | 199458 | areas | areas-mips-lzma-mips128-resource-lzma512-plain | PASS |
| 20 | 199539 | 199479 | areas | areas-mips-lzma-mips128-resource-lzma128-plain | PASS |
| 21 | 199825 | 199765 | areas | areas-mips-lzma256-resource-lzma1024-plain | PASS |
| 22 | 199825 | 199765 | areas | areas-mips-lzma256-resource-lzma256-plain | PASS |
| 23 | 199825 | 199765 | areas | areas-mips-lzma256-resource-lzma512-plain | PASS |
| 24 | 199836 | 199776 | areas | areas-mips-lzma1024-resource-lzma1024-plain | PASS |
| 25 | 199836 | 199776 | areas | areas-mips-lzma1024-resource-lzma256-plain | PASS |
| 26 | 199836 | 199776 | areas | areas-mips-lzma1024-resource-lzma512-plain | PASS |
| 27 | 199836 | 199776 | areas | areas-mips-lzma512-resource-lzma1024-plain | PASS |
| 28 | 199836 | 199776 | areas | areas-mips-lzma512-resource-lzma256-plain | PASS |
| 29 | 199836 | 199776 | areas | areas-mips-lzma512-resource-lzma512-plain | PASS |
| 30 | 199846 | 199786 | areas | areas-mips-lzma256-resource-lzma128-plain | PASS |
| 31 | 199857 | 199797 | areas | areas-mips-lzma1024-resource-lzma128-plain | PASS |
| 32 | 199857 | 199797 | areas | areas-mips-lzma512-resource-lzma128-plain | PASS |
| 33 | 200076 | 200048 | stream | lzma1024-stream-l9 | PASS |
| 34 | 200076 | 200048 | stream | lzma512-stream-l9 | PASS |
| 35 | 200206 | 200146 | areas | areas-mips-lzma-mips128-resource-lzma64-plain | PASS |
| 36 | 200454 | 200394 | areas | areas-mips-lzma-mips64-resource-lzma1024-plain | PASS |
| 37 | 200454 | 200394 | areas | areas-mips-lzma-mips64-resource-lzma256-plain | PASS |
| 38 | 200454 | 200394 | areas | areas-mips-lzma-mips64-resource-lzma512-plain | PASS |
| 39 | 200475 | 200415 | areas | areas-mips-lzma-mips64-resource-lzma128-plain | PASS |
| 40 | 200513 | 200453 | areas | areas-mips-lzma256-resource-lzma64-plain | PASS |
| 41 | 200524 | 200464 | areas | areas-mips-lzma1024-resource-lzma64-plain | PASS |
| 42 | 200524 | 200464 | areas | areas-mips-lzma512-resource-lzma64-plain | PASS |
| 43 | 201067 | 201007 | areas | areas-mips-lzma128-resource-lzma1024-plain | PASS |
| 44 | 201067 | 201007 | areas | areas-mips-lzma128-resource-lzma256-plain | PASS |
| 45 | 201067 | 201007 | areas | areas-mips-lzma128-resource-lzma512-plain | PASS |
| 46 | 201088 | 201028 | areas | areas-mips-lzma128-resource-lzma128-plain | PASS |
| 47 | 201142 | 201082 | areas | areas-mips-lzma-mips64-resource-lzma64-plain | PASS |
| 48 | 201285 | 201257 | stream | lzma64-stream-l6 | PASS |
| 49 | 201565 | 201537 | stream | lzma128-stream-l9 | PASS |
| 50 | 201755 | 201695 | areas | areas-mips-lzma128-resource-lzma64-plain | PASS |
| 51 | 202026 | 201966 | areas | areas-mips-lzma64-resource-lzma1024-plain | PASS |
| 52 | 202026 | 201966 | areas | areas-mips-lzma64-resource-lzma256-plain | PASS |
| 53 | 202026 | 201966 | areas | areas-mips-lzma64-resource-lzma512-plain | PASS |
| 54 | 202047 | 201987 | areas | areas-mips-lzma64-resource-lzma128-plain | PASS |
| 55 | 202690 | 202662 | stream | lzma64-stream-l9 | PASS |
| 56 | 202714 | 202654 | areas | areas-mips-lzma64-resource-lzma64-plain | PASS |
| 57 | 213177 | 213117 | areas | areas-mips-lzma-mips1024-resource-zlib-dynamic-plain | PASS |
| 58 | 213177 | 213117 | areas | areas-mips-lzma-mips256-resource-zlib-dynamic-plain | PASS |
| 59 | 213177 | 213117 | areas | areas-mips-lzma-mips512-resource-zlib-dynamic-plain | PASS |
| 60 | 214288 | 214228 | areas | areas-mips-lzma-mips128-resource-zlib-dynamic-plain | PASS |
| 61 | 214595 | 214535 | areas | areas-mips-lzma256-resource-zlib-dynamic-plain | PASS |
| 62 | 214606 | 214546 | areas | areas-mips-lzma1024-resource-zlib-dynamic-plain | PASS |
| 63 | 214606 | 214546 | areas | areas-mips-lzma512-resource-zlib-dynamic-plain | PASS |
| 64 | 215224 | 215164 | areas | areas-mips-lzma-mips64-resource-zlib-dynamic-plain | PASS |
| 65 | 215837 | 215777 | areas | areas-mips-lzma128-resource-zlib-dynamic-plain | PASS |
| 66 | 216796 | 216736 | areas | areas-mips-lzma64-resource-zlib-dynamic-plain | PASS |
| 67 | 218928 | 218868 | areas | areas-mips-lzma-mips1024-resource-bz2-plain | PASS |
| 68 | 218928 | 218868 | areas | areas-mips-lzma-mips256-resource-bz2-plain | PASS |
| 69 | 218928 | 218868 | areas | areas-mips-lzma-mips512-resource-bz2-plain | PASS |
| 70 | 220039 | 219979 | areas | areas-mips-lzma-mips128-resource-bz2-plain | PASS |
| 71 | 220346 | 220286 | areas | areas-mips-lzma256-resource-bz2-plain | PASS |
| 72 | 220357 | 220297 | areas | areas-mips-lzma1024-resource-bz2-plain | PASS |
| 73 | 220357 | 220297 | areas | areas-mips-lzma512-resource-bz2-plain | PASS |
| 74 | 220975 | 220915 | areas | areas-mips-lzma-mips64-resource-bz2-plain | PASS |
| 75 | 221588 | 221528 | areas | areas-mips-lzma128-resource-bz2-plain | PASS |
| 76 | 222547 | 222487 | areas | areas-mips-lzma64-resource-bz2-plain | PASS |
| 77 | 227664 | 227604 | areas | areas-mips-bz2-resource-lzma1024-plain | PASS |
| 78 | 227664 | 227604 | areas | areas-mips-bz2-resource-lzma256-plain | PASS |
| 79 | 227664 | 227604 | areas | areas-mips-bz2-resource-lzma512-plain | PASS |
| 80 | 227685 | 227625 | areas | areas-mips-bz2-resource-lzma128-plain | PASS |
| 81 | 228075 | 228015 | areas | areas-mips-lzma256-mips-byteplane-resource-lzma256 | PASS |
| 82 | 228146 | 228086 | areas | areas-mips-lzma64-mips-byteplane-resource-lzma256 | PASS |
| 83 | 228352 | 228292 | areas | areas-mips-bz2-resource-lzma64-plain | PASS |
| 84 | 230228 | 230168 | areas | areas-mips-zlib-dynamic-resource-lzma1024-plain | PASS |
| 85 | 230228 | 230168 | areas | areas-mips-zlib-dynamic-resource-lzma256-plain | PASS |
| 86 | 230228 | 230168 | areas | areas-mips-zlib-dynamic-resource-lzma512-plain | PASS |
| 87 | 230249 | 230189 | areas | areas-mips-zlib-dynamic-resource-lzma128-plain | PASS |
| 88 | 230916 | 230856 | areas | areas-mips-zlib-dynamic-resource-lzma64-plain | PASS |
| 89 | 231034 | 230926 | regions | regions-mips-plain-resource-lzma64-l9 | PASS |
| 90 | 239745 | 239685 | areas | areas-mips-zlib-dynamic-mips-byteplane-resource-lzma256 | PASS |
| 91 | 240551 | 240443 | regions | regions-mips-mips-byteplane-resource-lzma64-l9 | PASS |
| 92 | 242434 | 242374 | areas | areas-mips-bz2-resource-zlib-dynamic-plain | PASS |
| 93 | 244187 | 244079 | regions | regions-mips-plain-resource-zlib-dynamic-l9 | PASS |
| 94 | 244617 | 244509 | regions | regions-mips-plain-resource-bz2-l9 | PASS |
| 95 | 244745 | 244717 | stream | zlib-dynamic-stream-plain-l9 | PASS |
| 96 | 244790 | 244682 | regions | regions-mips-plain-resource-zlib-dynamic-l6 | PASS |
| 97 | 244998 | 244938 | areas | areas-mips-zlib-dynamic-resource-zlib-dynamic-plain | PASS |
| 98 | 245177 | 245149 | stream | zlib-dynamic-stream-plain-l6 | PASS |
| 99 | 248185 | 248125 | areas | areas-mips-bz2-resource-bz2-plain | PASS |
| 100 | 250169 | 249885 | independent | zlib-dynamic-plain-32K-l9 | PASS |
| 101 | 250562 | 250278 | independent | zlib-dynamic-plain-32K-l6 | PASS |
| 102 | 250749 | 250689 | areas | areas-mips-zlib-dynamic-resource-bz2-plain | PASS |
| 103 | 253617 | 253077 | independent | zlib-dynamic-plain-16K-l9 | PASS |
| 104 | 253704 | 253596 | regions | regions-mips-mips-byteplane-resource-zlib-dynamic-l9 | PASS |
| 105 | 253938 | 253398 | independent | zlib-dynamic-plain-16K-l6 | PASS |
| 106 | 254134 | 254026 | regions | regions-mips-mips-byteplane-resource-bz2-l9 | PASS |
| 107 | 254265 | 254157 | regions | regions-mips-mips-byteplane-resource-zlib-dynamic-l6 | PASS |
| 108 | 256031 | 255971 | areas | areas-mips-lzma256-mips-fields-resource-lzma256 | PASS |
| 109 | 256445 | 256385 | areas | areas-mips-lzma256-mips-xor-byteplane-resource-lzma256 | PASS |
| 110 | 256489 | 256429 | areas | areas-mips-lzma64-mips-xor-byteplane-resource-lzma256 | PASS |
| 111 | 257167 | 257107 | areas | areas-mips-lzma64-mips-fields-resource-lzma256 | PASS |
| 112 | 257527 | 257499 | stream | bz2-stream-l9 | PASS |
| 113 | 265600 | 265316 | independent | zlib-dynamic-plain-32K-l1 | PASS |
| 114 | 266500 | 266440 | areas | areas-mips-zlib-dynamic-mips-xor-byteplane-resource-lzma256 | PASS |
| 115 | 266962 | 266422 | independent | zlib-dynamic-plain-16K-l1 | PASS |
| 116 | 267306 | 267198 | regions | regions-mips-mips-xor-byteplane-resource-lzma64-l9 | PASS |
| 117 | 272632 | 272604 | stream | zlib-dynamic-stream-mips-byteplane-l9 | PASS |
| 118 | 273011 | 272983 | stream | zlib-dynamic-stream-mips-byteplane-l6 | PASS |
| 119 | 273414 | 273354 | areas | areas-mips-zlib-dynamic-mips-fields-resource-lzma256 | PASS |
| 120 | 273505 | 273397 | regions | regions-mips-plain-resource-zlib-rle-l9 | PASS |
| 121 | 274220 | 274112 | regions | regions-mips-mips-fields-resource-lzma64-l9 | PASS |
| 122 | 277231 | 276947 | independent | zlib-dynamic-mips-byteplane-32K-l9 | PASS |
| 123 | 278871 | 278331 | independent | zlib-dynamic-mips-byteplane-16K-l9 | PASS |
| 124 | 280459 | 280351 | regions | regions-mips-mips-xor-byteplane-resource-zlib-dynamic-l9 | PASS |
| 125 | 280889 | 280781 | regions | regions-mips-mips-xor-byteplane-resource-bz2-l9 | PASS |
| 126 | 281041 | 280933 | regions | regions-mips-mips-xor-byteplane-resource-zlib-dynamic-l6 | PASS |
| 127 | 283022 | 282914 | regions | regions-mips-mips-byteplane-resource-zlib-rle-l9 | PASS |
| 128 | 283643 | 283359 | independent | zlib-fixed-plain-32K-l9 | PASS |
| 129 | 287373 | 287265 | regions | regions-mips-mips-fields-resource-zlib-dynamic-l9 | PASS |
| 130 | 287803 | 287695 | regions | regions-mips-mips-fields-resource-bz2-l9 | PASS |
| 131 | 287990 | 287882 | regions | regions-mips-mips-fields-resource-zlib-dynamic-l6 | PASS |
| 132 | 290376 | 289836 | independent | zlib-fixed-plain-16K-l9 | PASS |
| 133 | 292485 | 292201 | independent | fixed-custom-32K-c192 | PASS |
| 134 | 292704 | 292420 | independent | current-mixed-32K | PASS |
| 135 | 292704 | 292420 | independent | fixed-custom-32K-c96 | PASS |
| 136 | 294010 | 293726 | independent | fixed-custom-32K-c24 | PASS |
| 137 | 297550 | 297010 | independent | fixed-custom-16K-c192 | PASS |
| 138 | 297725 | 297185 | independent | current-mixed-16K | PASS |
| 139 | 297725 | 297185 | independent | fixed-custom-16K-c96 | PASS |
| 140 | 298747 | 298207 | independent | fixed-custom-16K-c24 | PASS |
| 141 | 300263 | 300235 | stream | zlib-dynamic-stream-byte-delta-l9 | PASS |
| 142 | 300740 | 300712 | stream | zlib-dynamic-stream-byte-delta-l6 | PASS |
| 143 | 304832 | 303780 | independent | current-mixed-8K | PASS |
| 144 | 308069 | 307785 | independent | zlib-dynamic-byte-delta-32K-l9 | PASS |
| 145 | 309777 | 309669 | regions | regions-mips-mips-xor-byteplane-resource-zlib-rle-l9 | PASS |
| 146 | 313656 | 313116 | independent | zlib-dynamic-byte-delta-16K-l9 | PASS |
| 147 | 314545 | 312469 | independent | current-mixed-4K | PASS |
| 148 | 316691 | 316583 | regions | regions-mips-mips-fields-resource-zlib-rle-l9 | PASS |
| 149 | 320276 | 320248 | stream | zlib-dynamic-stream-mips-fields-l9 | PASS |
| 150 | 320377 | 320349 | stream | zlib-dynamic-stream-mips-xor-byteplane-l9 | PASS |
| 151 | 320788 | 320760 | stream | zlib-dynamic-stream-mips-fields-l6 | PASS |
| 152 | 320879 | 320851 | stream | zlib-dynamic-stream-mips-xor-byteplane-l6 | PASS |
| 153 | 325432 | 325148 | independent | zlib-dynamic-mips-xor-byteplane-32K-l9 | PASS |
| 154 | 326014 | 325986 | stream | zlib-dynamic-stream-word-xor-l9 | PASS |
| 155 | 326432 | 326404 | stream | zlib-dynamic-stream-word-xor-l6 | PASS |
| 156 | 328479 | 327939 | independent | zlib-dynamic-mips-xor-byteplane-16K-l9 | PASS |
| 157 | 328563 | 328279 | independent | zlib-dynamic-mips-fields-32K-l9 | PASS |
| 158 | 330259 | 329975 | independent | zlib-dynamic-word-xor-32K-l9 | PASS |
| 159 | 332029 | 331489 | independent | zlib-dynamic-mips-fields-16K-l9 | PASS |
| 160 | 332166 | 331626 | independent | zlib-dynamic-word-xor-16K-l9 | PASS |
| 161 | 338631 | 338603 | stream | zlib-dynamic-stream-mips-delta-byteplane-l9 | PASS |
| 162 | 338831 | 338803 | stream | zlib-dynamic-stream-mips-delta-byteplane-l6 | PASS |
| 163 | 341438 | 341410 | stream | zlib-dynamic-stream-word-delta-l9 | PASS |
| 164 | 341790 | 341762 | stream | zlib-dynamic-stream-word-delta-l6 | PASS |
| 165 | 342410 | 341870 | independent | zlib-rle-plain-16K-l9 | PASS |
| 166 | 343131 | 342847 | independent | zlib-rle-plain-32K-l9 | PASS |
| 167 | 343796 | 343512 | independent | zlib-dynamic-mips-delta-byteplane-32K-l9 | PASS |
| 168 | 345428 | 345144 | independent | zlib-dynamic-word-delta-32K-l9 | PASS |
| 169 | 346321 | 345781 | independent | zlib-dynamic-mips-delta-byteplane-16K-l9 | PASS |
| 170 | 347764 | 347224 | independent | zlib-dynamic-word-delta-16K-l9 | PASS |
| 171 | 351277 | 350993 | independent | zlib-huffman-plain-32K-l9 | PASS |
| 172 | 351566 | 351026 | independent | zlib-huffman-plain-16K-l9 | PASS |
| 173 | 366974 | 366434 | independent | zlib-dynamic-mips-bitplane-16K-l9 | PASS |
| 174 | 368235 | 367951 | independent | zlib-dynamic-mips-bitplane-32K-l9 | PASS |
| 175 | 382444 | 382416 | stream | zlib-dynamic-stream-mips-bitplane-l9 | PASS |
| 176 | 383505 | 383477 | stream | zlib-dynamic-stream-mips-bitplane-l6 | PASS |
