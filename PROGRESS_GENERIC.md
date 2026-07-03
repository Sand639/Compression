# 汎用モデルブランチ (generic-model) 進捗

## 方針
対象ファイルを実際にエンコード/解析した結果 (prior 焼き込み) を一切含まない汎用構成。
残しているのは「ファイル形式の一般知識に基づく予測方式・アルゴリズム選択・ハイパーパラメータ」のみ。

## prior 除去内容 (2026-07-03)
- prior_data1〜3.cpp (86配列, 約108MB) をリポジトリから削除。
- cm.cpp 内の旧 prior 7テーブル (EXE_PRIOR / EXE_PRIOR_SHORT / EXE_PRIOR_EXT / WAV_PRIOR /
  TEXT_PRIOR / BMP_PRIOR / YUUKI_PRIOR3) と全焼き込みコードを削除。
- train_*.cpp 群 / wav_mode_check.cpp / Encode_CM_DumpState (prior 生成用の採取口) を削除。

## ファイル固有リテラルの動的化 (ファイル固有情報 → 一般アルゴリズム化)
- yuuki 系 (作業1, A ブランチから継承): BMPヘッダ動的パース (bfOffBits/biWidth/biHeight/
  biBitCount、stride=((w*bc+31)/32)*4)。8bit インデックスBMP 全般に適用可。
- exe 系: PE ヘッダ動的パース (e_lfanew → セクションテーブル → PointerToRawData/
  SizeOfRawData/Characteristics)。領域分類はコードフラグ (IMAGE_SCN_CNT_CODE) と
  標準セクション名 (.reloc/.rsrc) による PE 一般知識ベース。
- hal 系: BMP残差フィルタ出力の自己記述ヘッダ (hdrLen/stride/rows/trailerLen) から
  残差開始位置と行ストライドを動的計算。

## スコア (measure, round-trip ALL OK / self-test PASS)

| ファイル | 元サイズ | 汎用モデル (B) | 特化モデル (A) | 差 |
|---|---|---|---|---|
| TeraPad.exe | 1,462,272 | 422,821 | 327,850 | +94,971 |
| explosion.wav | 599,084 | 229,841 | 217,241 | +12,600 |
| wagahaiwa_nekodearu.txt | 749,051 | 226,276 | 201,309 | +24,967 |
| hal.bmp | 712,922 | 220,593 | 206,291 | +14,302 |
| yuuki_256.bmp | 641,076 | 50,394 | 37,052 | +13,342 |
| **payload 合計** | 4,164,405 | **1,149,925** | 989,743 | +160,182 |

- output.enc (bwt.exe フルゲート): 下記「正式確定」参照。
- 7z (1,640,836 B) 比でも約 30% 小さい。

## 正式確定 (bwt.exe フルゲート)
- **data.arc = 1,150,037 B、5/5 SHA-256 一致、self-test PASS (2026-07-03 確定)**。
- 7z (1,640,836 B) 比 -29.9%。特化モデルA (989,855 B) との差 +160,182 B。
