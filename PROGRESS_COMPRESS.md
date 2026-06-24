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

## 現在の BEST スコア (本物 5 ファイル, local-baseline 2026-06-24)
**1,306,118 bytes** (output.enc) — round-trip 5/5 完全一致, self-test PASS, 7z(1,640,836) を 334,718 B 上回る。

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

## CMModel 現構成
- NIN = 11: order0,1,2,3,4,5,6,7,8, stride3, match
- TBITS = 23 (8M entries) for t2..t9
- SM = 1<<24 (16M) for matchTab
- ミキサー: 2048 文脈 (prevByte*8+bitpos), 学習レート >>12
- APM1: 2048 文脈, 65点, >>7 / APM2: 256(cx2) / APM3: 512(c0) / APM4: 256(cx3)

## 次に試す候補 (3秒スクリーニングで回す)
- 適応カウンタ (count ベース可変レート, 現状は固定 >>3)  ← 最有望
- ミキサー文脈に match-active ビット追加 (2048→4096)
- order-9 以上 / 第2マッチモデル(長ハッシュ)
- WAV/BMP 特化 CM パラメータ

## スコア履歴 (本セッション = local-baseline 以降)
| # | スコア | 手法 | 変化量 |
|---|---|---|---|
| local-baseline | 1,306,118 | 本物5ファイル再計測 | - |
