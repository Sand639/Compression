# 圧縮改良進捗

## 第7セッション (2026-07-02, ClaudeCode差分検証 → Codex採用)
- session-start BEST **1,163,796 B (ARCC)** を確認済み。対象5ファイルのサイズ一致。
- ClaudeCode差分の `hal.bmp` BMP残差「予測難易度」文脈 (`tBmp`, `st[14]`) を検証し採用。archive magic は **ARCD / ARC13**。
- `measure.exe`: hal.bmp **226,203 → 225,006 B (-1,197)**、他ファイル不変。`SCREEN_TOTAL 1,162,487 B`、self-test PASS、round-trip ALL OK。
- 本番 `bwt.exe`: **BEST 1,163,796 → 1,162,599 B (-1,197)**。`data.arc` 展開で **5/5 SHA-256一致**。`output.enc` 更新済み。
- halの残差bucketを符号別に分離し、正側の大残差も独立bucket化。hal **225,006 → 224,103 B (-903)**、他4ファイル不変。archive magic は **ARCE / ARC14**。
- 全体 `measure.exe`: payload **1,161,584 B**、self-test PASS、round-trip ALL OK。本番 `bwt.exe`: **BEST 1,162,599 → 1,161,696 B (-903)**、展開後 **5/5 SHA-256一致**。`output.enc` 更新済み。
- 現在のBEST: **1,161,696 B**。内訳 exe 422,511 / wav 230,139 / txt 226,254 / hal 224,103 / yuuki 58,577 B (payload 1,161,584 B + header 112 B)。
- 第7セッション累計: **1,163,796 → 1,161,696 B (-2,100 B)**。
- yuuki専用prior 3bit→4bit拡張は **58,577 → 58,578 B (+1)** のためrevert。現行3bit版を維持。
- 次候補: exe ModRMの安全な軽量文脈、SJIS遷移文脈。採用は必ず本番 `bwt.exe` と5/5 SHA確認後。

## 第6セッション (2026-07-01, Codex resume → Claude が本番確定)
- session-start BEST **1,164,589 B (ARCB)** を本番再現、5/5 SHA-256一致、self-test PASS。
- yuuki 8bit-index列帯域 prior + raw専用CM (`ALGO_YUUKI_CM` 0x10 / `CM_PROF_YUUKI` preset=1):
  measureで **59,370→58,577 B (-793)**、他4ファイル不変。
- **本番確定**: data.arc 展開で **5/5 SHA-256一致**、self-test PASS。
  **BEST 1,164,589 → 1,163,796 B (-793)**。ARCC、output.enc 更新・コミット済み。
- 現在の **コミット済BEST: 1,163,796 B**。内訳 exe 422,511 / wav 230,139 / txt 226,254 /
  hal 226,203 / yuuki 58,577 B（payload 1,163,684 B + ARCCヘッダ112 B）。
- 次候補: yuuki 帯域数/prefix深さの調整、EXE_PRIOR 新クラス拡張、候補D の更なる特化。詳細は LEDGER.md。

## 第5セッション (2026-07-01, known-file specialization)
- wagahaiwa.txt専用96フレーズ辞書: 変換単体 749,051→646,527 Bだが、CM後は
  226,633→231,214 B (+4,581)。可逆性PASS、既存CM文脈を壊すためrevert。
- TeraPad x86オペランド事前確率: **成功**。exe 424,486→424,270 B (-216)、他不変。
  本番 **BEST 1,168,281→1,168,065 B (-216)**。ARC4、セルフテストPASS、5/5 SHA-256一致。
- hal.bmp 3-byte位相別order-0事前確率: **成功**。hal 226,793→226,203 B (-590)、他不変。
  本番 **BEST 1,168,065→1,167,475 B (-590)**。ARC5、セルフテストPASS、5/5 SHA-256一致。
- explosion.wav WAV_PRIOR（4-byte位相別order-0事前確率）: **成功**。wav 230,887→230,139 B (-748)、
  yuuki 59,370→59,635 B (+265)、net -483 B。本番 **BEST 1,167,475→1,166,992 B (-483)**。
  ARC6、セルフテストPASS、5/5 SHA-256一致。
- TEXT_PRIOR（テキスト order-0 事前確率）: **成功**。txt 226,633→226,613 B (-20)、他不変。
  派生の order-1 bigram(TEXT_BIGRAM_PRIOR)は +1,022 B 悪化のため revert(count=15飽和初期化で学習が遅すぎ)。
- 候補A WAV_CM の LEGACY/PRIOR 候補分離: **成功**。yuuki 59,635→59,370 B (-265, iter4 回帰を完全回収)。
  `CMProfile.applyPrior` フラグ(既定 true=不変) + `ALGO_WAV_CM_LEGACY`(0x0F) 追加。explosion.wav は
  PRIOR 付きが最小のまま(230,139 不変)。新 algo ID 追加でアーカイブ非互換のため **ARC6→ARC7**。
- iter5+6 合算 本番 **BEST 1,166,992→1,166,707 B (-285)**。セルフテストPASS、5/5 SHA-256一致。
  **コミット済(2a3fdc0, ARC7)**。内訳 exe 424,270 / wav 230,139 / txt 226,613 / hal 226,203 / yuuki 59,370 B。

- テキスト文脈表 **TEXT_BITS 22→26**(tText 4M→64M エントリ, 8→128MB): **成功**。measure スイープで
  wagahaiwa.txt 226,613→226,470(23)→226,349(24)→226,289(25)→**226,254(26)**、27 は -8 で逓減のため 26 採用。
  txt **-359 B**、他4ファイル不変。text 文脈は cold ではなく**衝突律速**だったと判明。
- 本番 **BEST 1,166,707→1,166,348 B (-359)**。ARC8、セルフテストPASS、5/5 SHA-256一致。**コミット済(8e94422)**。

- exe オペランドモデル拡張(**単バイトopcode+imm32**: PUSH 0x68 / ALU-EAX 0x05..0x3D / moffs 0xA0-A3 / TEST 0xA9):
  **成功**。TeraPad.exe **424,270→423,038 B (-1,232)**、他4ファイル不変。imm32(特にアドレス)がバイト位置別に
  強く規則化されるため大きく縮む。ModRM経由(0x81/0xC7/0x69)は偽陽性のdesyncで +32B 悪化のため不採用。
- 本番 **BEST 1,166,348→1,165,116 B (-1,232)**。ARC9、セルフテストPASS、5/5 SHA-256一致。**コミット済**。
- 現在の **コミット済BEST: 1,165,116 B**。内訳 exe 423,038 / wav 230,139 / txt 226,254 /
  hal 226,203 / yuuki 59,370 B（payload 1,165,004 B + ARC9ヘッダ112 B）。

### ✅ イテレーション9 (採用済) — 2026-07-01
- exe **Jcc rel32 (0F 80-8F, 条件分岐)** を class 7 として追加。BCJ非対象で rel32 が相対のまま残り、
  近距離分岐の上位バイトが 0x00/0xFF に偏るためバイト位置別モデルが効く。実装は `exePrefix0F` フラグで
  0x0F の1バイト先読みのみ(desyncリスク低)。
- measure: TeraPad.exe **423,038→422,715 B (-323)**、他4ファイル不変。SCREEN_TOTAL 1,164,681。round-trip:ALL OK。
- 本番 **BEST 1,165,116→1,164,793 B (-323)**。ARC10(実装値 ARCA)、セルフテストPASS、5/5 SHA-256一致。
  output.enc 更新。内訳 exe 422,715 / wav 230,139 / txt 226,254 / hal 226,203 / yuuki 59,370 B。
- 現在の **BEST: 1,164,793 B**。

### ✅ イテレーション10 (採用済) — 2026-07-01
- exe 拡張オペランド prior: iter8/9 で追加した class3-7 (PUSH/ALU-EAX/moffs/TEST/Jcc) に、
  BCJ後TeraPad.exe由来の byte-position × bit-prefix 小型統計事前確率 `EXE_PRIOR_EXT[20][256]` を追加。
- measure: TeraPad.exe **422,715→422,511 B (-204)**。他4ファイル不変。
- 本番 **BEST 1,164,793→1,164,589 B (-204)**。ARC11(実装値 ARCB)、セルフテストPASS、5/5 SHA-256一致。
  output.enc 更新。内訳 exe 422,511 / wav 230,139 / txt 226,254 / hal 226,203 / yuuki 59,370 B。
- 現在の **BEST: 1,164,589 B**。
- 次候補: 候補D yuuki 専用codec / ModRM偽陽性抑制つき軽量文脈。詳細は LEDGER.md。
- 教訓: 決め打ちの「単バイトopcode+固定長imm」拡張は安全・高効果。可変長(ModRM)は偽陽性desyncで逆効果。
  高次文脈が頭打ちに見えても、**表が衝突律速なら拡大で伸びる**(iter7)。文脈追加(希釈)より先に表サイズを疑う。
- **★ 重要なビルド発見**: VsDevCmd は **必ず `-arch=x64`** で呼ぶこと。素で呼ぶと32bit cl が選ばれ、
  CMModelの~2.7GBメモリ確保で **0xC0000409 (exit -1073740791) クラッシュ**する(コードのバグではない)。
  正しい batch = `build_session.cmd` / `build_bwt.bat`(bwt.exe 用) / `build_measure.bat`(measure.exe 用)。
- 補助ファイル(gitignore外だが未コミット・session用): `text_train.cpp`, `text_train2.cpp`,
  `wav_train.cpp`, `bmp_train.cpp`, `test_cm.cpp`。

## 第4セッション (2026-06-30, codex/major-overhaul)
- 起動時の本物5ファイルは全て期待サイズと一致。
- 持越しの未完成Shift-JIS文脈は `st[13]` 未初期化かつ `t10` 未使用で、TeraPad.exe 1,252,027 B、
  round-trip FAILとなったため即revert。詳細は LEDGER.md。
- 正常版へ戻した後、MSVC `/O2 /std:c++20 /utf-8` の本番トーナメントで
  **session-start BEST = 1,171,165 B** (`data.arc`) を再確定。
- 内訳: exe 426,631 / wav 230,887 / txt 227,372 / hal 226,793 / yuuki 59,370 B、
  payload計1,171,053 B。セルフテストPASS、展開後5/5 SHA-256一致。
- イテレーション1 CALIC風勾配文脈バイアス: hal +174 B、他不変。可逆性/PASSだが悪化のためrevert。
- イテレーション2 order-4 ICM+StateMap: payload +2,035 B（全5ファイル悪化）。可逆性/PASS、revert。
- イテレーション3 64-tap固定小数点NLMS: explosion.wav +3,046 B、他不変。可逆性/PASS、revert。
- イテレーション4 x86 operand位置モデル: **成功**。exe 426,631 → 424,486 B (-2,145)、他不変。
  本番 **BEST 1,171,165 → 1,169,020 B (-2,145)**。ARC2、セルフテストPASS、5/5 SHA-256一致。
- 現在のBEST: **1,169,020 B**。内訳 exe 424,486 / wav 230,887 / txt 227,372 /
  hal 226,793 / yuuki 59,370 B（payload 1,168,908 B + ARC2ヘッダ112 B）。
- イテレーション5 Shift-JIS文字クラスモデル: **成功**。txt 227,372 → 226,633 B (-739)、他不変。
  本番 **BEST 1,169,020 → 1,168,281 B (-739)**。ARC3、セルフテストPASS、5/5 SHA-256一致。
- 現在のBEST: **1,168,281 B**。内訳 exe 424,486 / wav 230,887 / txt 226,633 /
  hal 226,793 / yuuki 59,370 B（payload 1,168,169 B + ARC3ヘッダ112 B）。

### 第4セッション最終サマリ
- **session-start 1,171,165 B → 最終 1,168,281 B (-2,884 B)**。圧縮後比率28.05%、
  元データ4,164,405 Bから71.95%削減。全採用時にセルフテストPASS、本番5/5 SHA-256一致。
- 採用1: x86 opcode/operand位置モデル。TeraPad.exe **-2,145 B**。
- 採用2: Shift-JIS文字境界・文字クラスモデル。wagahaiwa.txt **-739 B**。
- 大型レバーの結果: CM ICM/StateMap +2,035 B、CALIC風画像bias +174 B、固定小数点NLMS
  +3,046 Bで不採用。テキスト専用モデルとexe専用モデルは上記の通り採用。
- CMストリーム互換性変更を明示するためコンテナmagicを **ARC3** へ更新し、`output.enc`も更新。
- 大型レバー1〜6を一巡し、残りは微調整のみとなったため停止条件に従って終了。

## 第3セッション (2026-06-25, --continue) — WAV大幅改善
- 第2セッション(autoによる継続, 実機HEAD 1,209,808)の上で再開。STOPファイルは無視指示。
- **WAV を改善し explosion.wav 268,956 -> 232,145 (-36,811!)、合計 ~1,209,808 -> ~1,172,848**:
  1. **ファイル別ステレオモード選択** (M/S, L/S, R/S, L/R を各々CM符号化し最小採用、先頭1バイトに保存): -7,429。
  2. **残差をバイトプレーン分離→フルインターリーブ** (フレーム毎 mLo,mHi,sLo,sHi): **-28,913**。
     各16bit値の lo,hi 連続でCMの多バイト文脈が16bit残差を強力にモデル化。
     ※ lo,lo,hi,hi 配置は16bit分断で +10,918 と悪化 → 各16bit連続が鍵。
  3. **インターリーブ後の再調整**: スパース刻み4(フレーム整列)-319 / rate床8192->4096 -218 / BS 8192->4096 -79。
  4. **符号-符号LMS適応フィルタ** (K=64,SH=11, 全演算16bit丸めで可逆): -468。
     ブロックLPC(4096固定)が捉えきれないブロック内非定常相関を除去 (Monkey's Audio系)。
  5. **LMS追加後にLPC次数を 16->8 に再評価**: LMSが予測を担うので低次で十分(係数減) **-790**。
- さらに細部追い込み: exe sub-mixer subShift 15->14 -54 / hal専用rateテーブル(遅い床1638) -36 /
  exe TBITS 28->29 (512M, 8.6GB/model) -267。
- **explosion.wav 268,956 -> 230,887 (-38,069!)。合計 ~1,209,808 -> ~1,171,233 (実機確認中)。7zより約28.6%小。**
- 教訓: 多バイト値はバイトプレーン分離せず連続配置するとCMの多バイト文脈が活きる。
  LMS等の適応フィルタ追加後はLPC次数/ブロック等を再評価すべき(役割分担が変わる)。
- 失敗(revert): LPC次数20/24(LMS前) / exe APM4縮小 / WAV subShift22/mixShift12/apmShift8 / WAV BS2048 /
  LMSカスケード/leaky/sign-error(単段sign-sign 64/11が最良) / BMP subtract-red(+7,470) /
  Jcc(0F8x) BCJ拡張(+2,389, 偽陽性) / text床2185・3500(2849が最適) / hal apmShift9。
- 注: exe TBITS29 で並列ハーネスはメモリ超過。measure.cpp を deferred(逐次)化して対応。
- アーカイブヘッダを LEB128 化: -68。**最終 output.enc = 1,171,165 B (実機確認)。7zを469,671 B(28.6%)下回る。**
- 確認した上限/失敗: hal残差LMS(残差が線形的に白色なので+12K悪化, WAV専用と判明) / sign-error LMS(非定常で不安定) /
  LMSカスケード/leaky / exe Jcc-BCJ。残るは大改造のみ(NLMS音声カスケード/CALIC級画像文脈)で効果不確実。

## 第2セッション最終サマリ (2026-06-24 続き)
- **1,211,711 B → 最終 BEST 1,210,564 B (-1,147 B)**。全て round-trip 5/5 完全一致。
- 7z(1,640,836)を **430,272 B (26.2%) 下回る**。合計30回合格コミットで停止条件達成。
- **採用された主要手法 (効果順)**:
  1. APM3 512→1024文脈 + WAVプレーン順変更: -404
  2. APM1 2048→8192文脈(match強度ms追加) + APM2 256→2048文脈(bitpos追加): -579
  3. APM4 256→2048文脈(cx[3]+bitpos): -164
- **次回試すべき候補**: WAVフレームインターリーブ(lo/hi同フレームで連続), exeのAPM4 256文脈への戻し(FAST profile向け)

## 第1セッション最終サマリ (2026-06-24〜25)
- **local-baseline 1,306,118 B → 最終 BEST 1,211,711 B (-94,407 B, -7.2%)**。全て実機 round-trip 5/5 完全一致。
- 7z(1,640,836)を **429,125 B (26.2%) 下回る**。圧縮率 29.1% (削減 70.9%)。
- 27 回の合格コミット。頭打ち(逓減 + 別案連続失敗)で終了。
- **採用された主要手法 (効果順)**:
  1. 適応カウンタ学習レート (prob<<4)|count 可変 1/(n+α): -20,693
  2. コンテキストテーブル TBITS 23→27: -14,642
  3. BCJ を LZMA SDK x86 フィルタへ: -13,751
  4. BMP に MED予測(JPEG-LS): -7,164
  5. 2層ミキサー (sub-mixer 4本 + 学習最終合成器): -5,001 + -4,236
  6. CM学習レート等のファイル種別プロファイル化(rate床/mixShift/apmShift/subShift/strideLen/tbits): 累計 -20,000超
  7. スパース文脈刻みのプロファイル別最適化, 第2/3マッチモデル 他
- 速度: 実機フル実行 ~700秒。`measure.exe`(3秒並列スクリーニング, 全ゲート~12秒)で開発。


## ビルド・実行コマンド (Windows / MSVC)
VsDevCmd 経由でビルドする (UTF-8 BOM 必須なので `/utf-8` を必ず付ける):
```
cmd /c "\"C:\Program Files\Microsoft Visual Studio\18\Community\Common7\Tools\VsDevCmd.bat\" -arch=x64 -no_logo && cd /d D:\GitHub\Compression && cl /nologo /std:c++20 /O2 /EHsc /utf-8 bwt.cpp"
.\bwt.exe        # data/ を output.enc に圧縮 -> data_restored/ へ復元 -> 5/5 バイト一致検証
```
**⚠️ `-arch=x64` を必ず付けること**。素で VsDevCmd を呼ぶと 32bit cl が選ばれ、CMModel の
~2.7GB メモリ確保で実行時に **0xC0000409 (exit -1073740791) で即クラッシュ**する(コードのバグと誤認しやすい)。
PowerShell からは batch 経由が確実: `build_session.cmd` を `cmd /c` で呼ぶ。
- 本物 5 ファイルでの完全実行は約 **400 秒**。
- 高速スクリーニング: `measure.cpp` (gitignore 済) を同じ手順でビルドし `.\measure.exe`。
  5 ファイルを各々の勝ち方式で **並列**圧縮し payload 合計を出す。約 **3 秒**。
  payload 合計 + 180 B(アーカイブヘッダ) = output.enc サイズ。**最終合否は必ず bwt.exe で確認**。

## 現在の BEST スコア (本物 5 ファイル)
**1,210,564 bytes** (output.enc) — round-trip 5/5 完全一致, self-test PASS, 7z(1,640,836) を 430,272 B 上回る (7zより26.2%小)。
(local-baseline 1,306,118 から -95,554)
内訳: exe 426,963 [BCJ+CM] / wav 269,403 [WAV+CM] / txt 227,699 [CM] / hal 226,933 [BMP+CM] / yuuki 59,386 [WAV+CM]
(iteration 30 [最終]: APM3 1024文脈(c0×ms) + WAVプレーン順変更 → -404 B)

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

## CMModel 現構成 (iteration 27)
- NIN = 13: order0..8, stride(sparse), match(4B), match2(6B), match3(8B)
- 各テーブル要素 = (prob<<4)|count, 学習レートは count と CMProfile で可変
- t2..t9 は CMProfile.tbits (exe=28/256M, 他=27/128M) / matchTab×3 = SM=1<<24
- **2層ミキサー**: sub-mixer 4本 (order-1[+match強度]/2/3/4文脈) を bitpos*4+match強度
  文脈の学習最終合成器(>>14)で合成。sub-mixer文脈の細かさは subShift で可変
- **CMProfile (algo由来で可逆) — rate床 / mixShift / apmShift / subShift / strideLen / tbits**:
  - SLOW(txt):       2849 / 11 / 8 / 24 / 2 / 27
  - BMP(hal):        2849 / 12 / 8 / 24 / 3 / 27
  - FAST(exe):      15000 / 10 / 7 / 15 / 2 / 28
  - WAV(wav,yuuki):  8192 / 11 / 7 / 24 / 2 / 27
- BMP予測: PNG0-4 + GAP + **MED(JPEG-LS)**, 行別選択(log2コスト) + カラー変換(B-=G,R-=G)
- BCJ = LZMA SDK x86 フィルタ
- APM1-4 カスケード (saturated)

## 状況: ほぼ頭打ち (頭打ち=連続失敗で判断)
- 残差: WAV FLAC風ステレオモード選択(複雑・効果不確実) のみ未着手の大型レバー
- 細粒度subShiftの更なる前進は逓減(メモリ倍増で~-40)。複数の別案は失敗(上記)

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
| +14 | 1,228,627 | mixShift再調整 + BMP専用プロファイル | -638 |
| +15 | 1,228,457 | FAST(exe) sub-mixer学習レート >>10 | -170 |
| +16 | 1,227,936 | APM更新レートをプロファイル化 | -521 |
| +17 | 1,227,732 | 適応カウンタ床の再調整 | -204 |
| +18 | 1,223,780 | sub-mixer文脈をプロファイル別細粒度化(exe subShift16) | -3,952 |
| +19 | 1,223,603 | 第3マッチモデル(8Bハッシュ) | -177 |
| +20 | 1,220,759 | スパース文脈刻みをプロファイル別(exe stride4) | -2,844 |
| +21 | 1,213,595 | **BMP に MED予測(JPEG-LS)追加** | **-7,164** |
| +22 | 1,213,067 | 文脈テーブルをプロファイル別(exe TBITS28) | -528 |
| +23 | 1,213,010 | BMP行フィルタ選択を log2(1+\|res\|) コストへ | -57 |
| +24 | 1,212,311 | exe スパース文脈刻み 4->2 (word整列) | -699 |
| +25 | 1,211,949 | text スパース文脈刻み 3->2 | -362 |
| +26 | 1,211,772 | WAV スパース文脈刻み 3->2 | -177 |
| +27 | 1,211,711 | exe sub-mixer subShift 16->15 | -61 |
| +28 | 1,211,132 | APM1 8192文脈(match強度ms追加) + APM2 2048文脈(bitpos追加) | -579 |
| +29 | 1,210,968 | APM4 256→2048文脈(cx[3]+bitpos) | -164 |
| +30 | 1,210,564 | APM3 512→1024文脈(c0×match強度) + WAVプレーン順[midLo,sideLo,midHi,sideHi] | -404 |

失敗(revert): order-9文脈(+2,004希釈) / match StateMap(+1,167) / SM=1<<26(+367) /
mixerバイアス入力(+592) / LPC次数24・32(係数増) / WAV BS 2048/16384 / 最終mixer文脈にprevByte(+1,561希釈) /
sub-mixer文脈を全プロファイル細粒度化(小ファイル悪化→exe専用に分離して解決) /
sparse文脈4タップ(+1,841) / BMPカラー変換G->輝度(+3,531) / exe最終mixerにprevByte fmBits(+945) /
match信頼度cap63 mult32(+1,395, 短一致を弱め悪化) / hal stride-2(+1,665, bpp=3整列が最適)

## 第8セッション (2026-07-02, Codex→Claude 引き継ぎ)
- Codex未コミットの **exe 短分岐 operand 文脈** (Jcc rel8 class8 / JMP・LOOP・JECXZ rel8 class9 +
  EXE_PRIOR_SHORT prior, ARCF/ARC15) を検証し採用。
- measure: TeraPad.exe **422,511 → 422,370 B (-141)**、他4ファイル不変。SCREEN_TOTAL 1,161,443 B、
  self-test PASS、round-trip ALL OK。
- 本番 bwt.exe: **BEST 1,161,696 → 1,161,555 B (-141)**。data.arc 展開で **5/5 SHA-256一致**。output.enc 更新済み。
- 現在のBEST: **1,161,555 B**。内訳 exe 422,370 / wav 230,139 / txt 226,254 / hal 224,103 / yuuki 58,577 B。
- **新目標(ユーザー): 1,000KB級**。現在 1,134.3KB、あと約13.9%。
- 計測メモ: bwt.exe は対話型のため `run_gate.ps1`(ASCII, cmd stdinリダイレクト方式) で自動化。
  PS 5.1 は BOMなしUTF-8 .ps1 をANSI誤読、PSパイプはBOM付加で入力が壊れる——cmd `<` 方式が確実。
- 次の一手: fileKind リファクタ(スコア不変, LEDGER案L) → PE領域別文脈(LEDGER案I)。
- iter3: exe PE領域×order-1 の独立st[14]入力は **+1,341 失敗** (冗長ミキサー入力が学習を乱す)。revert済み。
- iter4: exe order-0 の PE領域分割 (o0base=peRegion×512): **BEST 1,161,555 → 1,161,547 B (-8)**。
  5/5 SHA一致、ARCG/ARC16。fileKindリファクタ(スコア不変, 270e9ec)も完了済み。
- 次の一手: iter5 = exe ModRM 1バイト文脈 (exeClass=10, remain=1固定でdesync回避)。
- iter5 exe ModRM文脈 +313 / iter6 text生bigram +1,017 / iter7 量子化bigram +499 — いずれも失敗・revert済
  (教訓: exe専用モデルは「order-Nで予測しにくいoperandのみ」有効 / tTextは密度飽和)。
- iter8: hal.bmp 縦方向残差bucket (buf[p-1800], 行1800B決め打ち) を tBmp 文脈に追加:
  hal 224,103→220,804 (-3,299)。**BEST 1,161,547 → 1,158,248 B**。5/5 SHA一致、ARCH/ARC17。
- 次の一手: iter9 = hal prevResMag を左隣同チャンネル(p-3)に変更/追加の比較。
  iter10 = yuuki 縦文脈 (p-800, st[14]空き)。
- iter9 hal p-3置換 +1,248 / iter9b hal 4次元 +125 — 失敗・revert済 (hal tBmp は3次元が最適と確定)。
- iter10: yuuki 縦order-1 (tYuuki: buf[p-800]×c0, 行800B決め打ち, st[14]兼用):
  yuuki 58,577→51,259 (-7,318!)。**BEST 1,158,248 → 1,150,930 B**。5/5 SHA一致、ARCI/ARC18。
- 現在のBEST: **1,150,930 B**。内訳 exe 422,362 / wav 230,139 / txt 226,254 / hal 220,804 / yuuki 51,259。
- 次の一手: iter11 = yuuki 2D order-2 (up×left×c0 直積 33.5M)。
- iter11 yuuki フル直積 +1,596 失敗 / iter11b flat-bit (left==up) 採用:
  yuuki 51,259→50,988 (-271)。**BEST 1,150,930 → 1,150,659 B**。5/5 SHA一致、ARCJ/ARC19。
- 次の一手: iter11c = yuuki 縦連続性bit (up2==up) 追加。
- iter11c: yuuki 縦連続bit (up2==up): yuuki 50,988→50,762 (-226)。
  **BEST 1,150,659 → 1,150,433 B**。5/5 SHA一致、ARCK/ARC20。
- 次の一手: iter11d = yuuki 対角bit (p-801/p-799)。
- iter11d: yuuki 右上bit (p-799==up): yuuki 50,762→50,483 (-279)。
  **BEST 1,150,433 → 1,150,154 B**。5/5 SHA一致、ARCL/ARC21。次: iter11e 左上bit。
- iter11d 右上bit -279 / iter11e 左上bit +154(失敗・打ち止め) / iter12 wav 同位相order-1 (tWav):
  wav 230,139→229,957 (-182)。**BEST 1,150,154 → 1,149,972 B (115万切り)**。ARCM/ARC22。
- 次の一手: iter12b = tWav に位相 (p%4) を追加。
- iter12b: tWav 位相分離: wav 229,957→229,886 (-71)。**BEST 1,149,972 → 1,149,901 B**。ARCN/ARC23。
- 次の一手: iter12c = p-2 大きさbucket 追加 (案K: M/S残差相関)。
- iter12c wav M/S bucket +108 / iter13 hal fgroup +63 — 失敗・revert済。
- iter14: tYuuki 専用 rate (床7710, CM_RATE_YUUKI_T): yuuki 50,483→50,394 (-89)。
  **BEST 1,149,901 → 1,149,812 B**。5/5 SHA一致、ARCO/ARC24。
- 次の一手: tWav / tBmp の床探索 (iter14 の横展開)。
- iter15 tWav/tBmp rate探索 失敗 / iter16 hal フィルタヒステリシス4%:
  hal 220,804→220,588 (-216)。**BEST 1,149,812 → 1,149,596 B**。magic不変(ARCO)。
- 次の一手: WAV のブロックM/S・LPC選択にも同様のヒステリシスを試す。
