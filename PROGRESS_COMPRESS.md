# 圧縮改良進捗

## ビルド・実行コマンド (Windows / MSVC)
VsDevCmd 経由でビルドする (UTF-8 BOM 必須なので `/utf-8` を必ず付ける):
```
cmd /c "\"C:\Program Files\Microsoft Visual Studio\18\Community\Common7\Tools\VsDevCmd.bat\" -arch=x64 -no_logo && cd /d D:\GitHub\Compression && cl /nologo /std:c++20 /O2 /EHsc /utf-8 bwt.cpp"
.\bwt.exe        # data/ を output.enc に圧縮 -> data_restored/ へ復元 -> 5/5 バイト一致検証
```
- 本物 5 ファイルでの完全実行は約 **400 秒**。
- 高速スクリーニング: `measure.cpp` (gitignore 済) を同じ手順でビルドし `.\measure.exe`。
  5 ファイルを各々の勝ち方式で **並列**圧縮し payload 合計を出す。約 **3 秒**。
  payload 合計 + 180 B(アーカイブヘッダ) = output.enc サイズ。**最終合否は必ず bwt.exe で確認**。

## 現在の BEST スコア (本物 5 ファイル)
**1,229,265 bytes** (output.enc) — round-trip 5/5 完全一致, self-test PASS, 7z(1,640,836) を 411,571 B 上回る (7zより25.1%小)。
(local-baseline 1,306,118 から -76,853)
内訳: exe 435,775 [BCJ+CM] / wav 269,707 [WAV+CM] / hal 235,054 [BMP+CM] / txt 229,049 [CM] / yuuki 59,500 [WAV+CM]
注: CM 強化で yuuki の勝者が CM->WAV+CM にフリップ。measure.exe は yuuki=min(CM,WAV)で実機一致。

> 注: 旧 PROGRESS の 543,360 / 624,073 は **壊れたデータ(explosion.wav 118B, TeraPad.exe 欠落)**
> 上の無効値。本物 5 ファイルで測り直したのが上記。

## データセット (data/ 内の本物 5 ファイル)
| ファイル | 元サイズ | 圧縮後 | 採用アルゴリズム | 削減率 |
|---|---|---|---|---|
| TeraPad.exe | 1,462,272 | 492,658 | BCJ+CM | 66.3% |
| explosion.wav | 599,084 | 270,125 | WAV+CM | 54.9% |
| wagahaiwa_nekodearu.txt | 749,051 | 244,659 | CM | 67.3% |
| hal.bmp | 712,922 | 236,441 | BMP+CM | 66.8% |
| yuuki_256.bmp | 641,076 | 62,055 | CM | 90.3% |

**全 5 ファイルが CM をバックエンドに採用** → 共有 CM モデルの改良が最大レバレッジ。

## CMModel 現構成 (iteration 13)
- NIN = 12: order0..8, stride3, match(4B), match2(6B)
- 各テーブル要素 = (prob<<4)|count, 学習レートは count と CMProfile で可変
- TBITS = 27 (128M entries) for t2..t9 / SM = 1<<24 for matchTab
- **2層ミキサー**: sub-mixer 4本 (order-1[+match強度]/2/3/4文脈) を、bitpos*4+match強度
  文脈の学習最終合成器(>>14)で合成
- CMProfile (algo由来で可逆): SLOW(txt/bmp 床~1/18,>>12) / FAST(exe 床1/5,>>11) / WAV(床1/8,>>11)
- APM1-4 カスケード (saturated)

## 次に試す候補
- hal.bmp 2D予測器の改善 / BMP 専用 CM プロファイル
- text 向け word モデル (ただし単純な文脈追加は希釈で失敗済)
- 最終合成器の文脈チューニング継続 (prevByteは希釈で失敗)

## スコア履歴 (本セッション = local-baseline 以降)
| # | スコア | 手法 | 変化量 |
|---|---|---|---|
| local-baseline | 1,306,118 | 本物5ファイル再計測 | - |
| +1 | 1,305,634 | ミキサー文脈に match-active ビット (2048->4096) | -484 |
| +2 | 1,302,964 | 第2マッチモデル (6バイトハッシュ) | -2,670 |
| +3 | 1,282,271 | 適応カウンタ学習レート (prob<<4)\|count | -20,693 |
| +4 | 1,270,252 | CM学習レートをファイル種別で2プロファイル化 | -12,019 |
| +5 | 1,268,951 | ミキサー学習レートをプロファイル化 (CMProfile) | -1,301 |
| +6 | 1,254,309 | コンテキストテーブル TBITS 23->27 | -14,642 |

| +7 | 1,254,082 | SLOW床緩下げ + ミキサーmatch文脈2bit強度化 | -227 |
| +8 | 1,240,331 | BCJ を LZMA SDK x86 フィルタへ置換 | -13,751 |
| +9 | 1,240,027 | WAV ブロックサイズ 4096->8192 | -304 |
| +10 | 1,239,662 | exe/wav の CM プロファイル分離 + 床再調整 | -365 |
| +11 | 1,234,661 | 第2ミキサー(order-2, ロジット平均) | -5,001 |
| +12 | 1,230,425 | 2層ミキサー(第3+学習最終合成器) | -4,236 |
| +13 | 1,229,265* | 第4ミキサー+最終文脈にmatch強度 (*実機, yuukiフリップ込) | -1,160 |

失敗(revert): order-9文脈(+2,004希釈) / match StateMap(+1,167) / SM=1<<26(+367) /
mixerバイアス入力(+592) / LPC次数24・32(係数増) / WAV BS 2048/16384 / 最終mixer文脈にprevByte(+1,561希釈)
