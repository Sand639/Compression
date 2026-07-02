# 改良台帳

## 第7セッション (2026-07-02, ClaudeCode差分の検証・Codex採用)

### イテレーション3: yuuki専用priorを先頭3bit→4bitへ拡張 → **失敗 +1 B・revert**
- 9領域別priorの既存prefix 1..7を保持し、固定ファイルから算出したprefix 8..15を追加。単体round-tripはOKだが、yuuki **58,577 → 58,578 B (+1)** と僅かに悪化した。
- 下位側は適応CMへ任せる現行3bit版が最良。4bit版コードはrevertし、BEST **1,161,696 B (ARCE)** を維持。

### イテレーション2: hal.bmp BMP残差bucketの符号分離 → **成功 -903 B**
- 直前残差の大きさだけを使っていた `tBmp` 文脈を、正負別のbucketへ分離。正側の大残差 (`d > 96`) も独立bucketにして、予測器や元データは変更せず確率文脈だけを細分化した。
- 単体スクリーニング: hal.bmp **225,006 → 224,103 B (-903)**。全体 `measure.exe`: exe 422,511 / wav 230,139 / txt 226,254 / hal 224,103 / yuuki 58,577 B、payload **1,161,584 B**。self-test PASS、round-trip ALL OK。
- CMストリーム非互換のため archive magic を **ARCD → ARCE (ARC14)** に更新。
- 本番 `bwt.exe`: **data.arc = 1,161,696 B**。旧BEST **1,162,599 → 1,161,696 B (-903)**。展開後 **5/5 SHA-256一致**。
- `output.enc` を新BESTへ更新済み。内訳 payload 1,161,584 B + header 112 B。

### イテレーション1: hal.bmp BMP残差「予測難易度」文脈(tBmp) → **成功 -1,197 B**
- ClaudeCodeの未コミット差分として残っていた `cm.cpp` / `compress.h` を検証。内容は BMP_CM 専用で、直前残差の大きさ bucket と RGB phase と bit-prefix を組み合わせた `tBmp` 文脈を `st[14]` に入れるもの。
- 既存の `NIN=15` と `st[14]` の枠を使い、text 専用文脈と排他的に BMP 専用文脈を有効化。CMストリーム非互換のため archive magic を **ARCC → ARCD (ARC13)** に更新。
- `measure.exe`: hal.bmp **226,203 → 225,006 B (-1,197)**。他ファイルは不変: exe 422,511 / wav 230,139 / txt 226,254 / yuuki 58,577 B。`SCREEN_TOTAL 1,162,487 B`、self-test PASS、round-trip ALL OK。
- 本番 `bwt.exe`: **data.arc = 1,162,599 B**。旧BEST **1,163,796 → 1,162,599 B (-1,197)**。展開後 **5/5 SHA-256一致**。
- `output.enc` を新BESTへ更新済み。内訳: exe 422,511 / wav 230,139 / txt 226,254 / hal 225,006 / yuuki 58,577 B (payload 1,162,487 B + header 112 B)。

## 🆕 未着手の採用候補（ChatGPT案を現状と照合して精査・2026-07-01追加 / 上から優先）

> 現BEST = 1,166,348 B (ARC8, 2a3fdc0→8e94422 時点)。**採用の最終条件（絶対に守る）**:
> ① 全5ファイルの round-trip 5/5 SHA-256一致 + self-test PASS（**完全可逆は絶対**）。
> ② 全5ファイル合計 output.enc が現BESTより **厳密に小さい**。
> ③ CMストリーム非互換なら ARC magic を更新。

#### 決め打ち（ハードコード）の許可範囲 ← このプロジェクトの前提
対象5ファイルは `data/` で固定。よって以下を積極的に使ってよい:
- **ファイル特化のハードコード可**: 指定ファイルに対し、マジックナンバー・専用の定数/変数構成・
  事前確率テーブル等を決め打ちしてよい（例: このファイルの最適 subShift/床/prior をコードに直書きする）。
- **最適が判明したファイルは方式を固定してよい**: トーナメントで全方式を試さず「このファイルはこの方式のみ」
  と決め打ちしてテスト時間を短縮してよい。**ただしトーナメントのコード自体は残す**（他の実験・再確認のため）。
- **1ファイルずつテスト可**: 毎回5ファイル一括で測らず、対象ファイルだけで素早く検証して回してよい。
  ただし **採用前の最終確認は必ず全5ファイル** で round-trip + self-test + 合計payload を通すこと。
- **改善と改悪が両立する場合は revert せず「コード分岐」で解決してよい**:
  あるコード変更が1ファイルを改善し別ファイルを悪化させる場合、全体を revert するのではなく、
  **ほぼ同一でも別 .cpp / 別関数にコピーしてファイルごとに特化した版**を作り、各ファイルが自分の最良コードを
  使うようにしてよい（復号側が algo ID / ファイル種別で正しい版を選べること）。結果として合計が縮めば採用。
  ※ 実例: iter6 の `ALGO_WAV_CM_LEGACY`(prior版と非prior版を候補分離) がこの方針。

#### 決め打ちでも越えてはいけない線（禁止）
- **生データ・巨大辞書を復号器へ埋め込むのは禁止**。それは圧縮でなく格納（静的フレーズ辞書 +4,581 が実例）。
  許されるのは小型の統計事前分布・定数・方式選択まで。**データそのものをコードや別ファイルに退避しないこと**。
- 可逆性を少しでも損なう決め打ちは不可。エンコーダとデコーダで必ず同一の処理を再現すること。

### A. WAV_PRIOR を候補分離（LEGACY / PRIOR） ← ✅ **採用済 -265 B (iter6, ARC7)**
- 何を: 現在 explosion.wav 用の WAV_PRIOR が WAV+CM に固定適用され、同じ WAV+CM を選ぶ
  yuuki_256.bmp に副作用で **+265 B** 出ている。WAV候補を `ALGO_WAV_CM_LEGACY`(priorなし) /
  `ALGO_WAV_CM`(priorあり) に分け、トーナメントで小さい方を選ばせる。
- 結果: yuuki が LEGACY を選び **-265 B 回収**。explosion.wav は PRIOR 付きが最小のまま。詳細は iter6。
- 実装: `CMProfile.applyPrior` フラグ(既定 true) + `CM_PROF_WAV_LEGACY` + `ALGO_WAV_CM_LEGACY`(0x0F)。
- 残り(案5): 他の prior(exe/bmp/text)も同様に ON/OFF を候補化すると副作用回避で更に -100〜-500 B。
  `applyPrior` 設計は流用可能。**未着手**。

### B. exe ModRM/SIB 軽量 x86 状態文脈 ← 最優先（PROGRESS の「次の試み」と一致）
- 何を: 完全 x86 デコーダ不要。軽量ヒューリスティックで OPCODE/MODRM/SIB/DISP/IMM/REL 状態を推定し、
  x86_state・opcode_class・modrm(mod/reg/rm)・operand_byte_pos 等を FAST(exe)専用の小型文脈へ追加。
- 期待: TeraPad.exe 限定で **-100〜-800 B**。
- 注意: 変換ではなく**確率モデル補助に留める**（Jcc 拡張BCJ の偽陽性失敗 +2,389 とは別物）。
  表は小さく、失敗時に他4ファイルへ影響しないよう exe 専用にする。

### C. SJIS 2-gram / 文字クラス遷移文脈 ← 高優先（PROGRESS の「次の試み」と一致）
- 何を: 既存 Shift-JIS 文字クラスモデル(-739 成功)の延長。prev_class→cur_class、prev2+prev、
  lead/trail phase、句読点後/鉤括弧内/改行直後フラグを SLOW(text)専用の小型文脈へ追加。
- 期待: wagahaiwa.txt 限定で **-100〜-700 B**。
- 注意: **生フレーズ置換は禁止**（静的辞書 +4,581 の失敗）。巨大2-gram表を避け、全入力で初期化・更新漏れを出さない。

### D. yuuki_256.bmp 専用インデックス画像 codec ← 中〜高優先（不一致方式の是正）
- 何を: yuuki は8bitインデックス画像なのに現在 WAV+CM を選んでいる(59,635)。palette別保存 +
  index map の2D予測(left/up/MED) + 同色ラン長RLE + tile 単位モード選択 の独立候補を追加。
- 期待: **-100〜-2,000 B**（本来不一致な方式なので伸びしろあり）。
- 注意: 独立候補として追加し、既存に勝てなければ不採用。現状59KB台なので幅は読みにくい。

### E. ブロック単位トーナメント圧縮器 ← 中優先・新系統で当たれば全ファイルに効く
- 何を: ファイル単位トーナメントは残し、**追加候補**として 256KiB 前後のブロック単位で
  既存CM候補 / RAW 等を試し最小を採用。ブロックヘッダに方式ID・サイズを保存。
- 期待: 合計 **-200〜-2,000 B**（特に exe / bmp）。
- 注意: ブロックを小さくしすぎると CM の長文脈が切れて悪化する。まず **256KiB 以上**で。
  ヘッダオーバーヘッドに注意。全体CMより悪化するブロックは使わない。

### F. hal.bmp MED残差の軽量確率文脈 ← 中優先（予測値は変えない）
- 何を: 予測器は触らず、MED後残差の確率だけを RGB phase / bitpos / 直前残差の符号・大きさbucket /
  x mod 3 / 行フィルタ mode の小型文脈で補助。
- 期待: hal.bmp 限定で **-50〜-400 B**。
- 注意: CALIC 勾配バイアス(+174 失敗)と違い**予測値を変えない**。既存3位相prior(-590済)と重複しない情報に絞る。

### G. text 専用 PPM 独立候補（byte → SJIS文字単位） ← 中優先・新系統
- 何を: wagahaiwa.txt 専用候補として order0-5 の byte単位 PPM + range coder を追加。
  余裕があれば SJIS 2バイトを1文字シンボル化した PPM も。既存CMは触らず別候補。
- 期待: **-100〜-1,500 B**（ただし強い既存CMに負ける可能性も高い）。
- 注意: 最大order・ノード数に上限。辞書のように入力を壊さない（完全に別候補として実装）。

### H. exe 命令ストリーム分離 codec ← 中優先・高コスト（B が当たってから拡張）
- 何を: 軽量 x86 パーサで opcode/ModRM/SIB/disp/imm/rel/raw に分離し、各ストリームを個別圧縮
  (range/PPM)。パース不能箇所は raw へ逃がす。exe は最大ファイルなので上振れが大きい。
- 期待: TeraPad.exe で **-300〜-3,000 B**。
- 注意: 完全可逆必須・raw escape 必須。実装コスト高。まず B の軽量文脈で当たりを確認してから着手。

### 低優先（大コスト・既存CMに負ける可能性が高い。上が尽きてから）
- ROLZ+RangeCoder / LZMA風 Optimal Parse LZ / FLAC・Monkey's風 WAV専用codec /
  BWT+MTF+RLE 独立候補（※既に BWT パイプラインがあり CM が上回っている）/ 小型PAQ風 別CM。
- いずれも「独立候補として追加し、勝てなければ不採用」を厳守。実装コストの割に期待薄。

---

## 第6セッション (2026-07-01, Codex resume)

### イテレーション1: yuuki 8bit-index列帯域 prior + 専用CM → **measure成功 -793 B**
- 既存BMP_CMは78,451 BでWAV+CM(legacy) 59,370 Bに大敗。800×800 index面を100列×8帯域に分け、
  header/paletteを含む9領域×bit-prefixの小型事前確率を学習したraw専用CM候補を追加する。
- 実装は高寄与の先頭3bit(prefix 1..7)だけを9領域別に事前学習し、細部は適応CMへ委ねる。
- measure: yuuki **59,370 → 58,577 B (-793)**、他4ファイル不変。payload
  **1,164,477 → 1,163,684 B**。self-test PASS、round-trip 5/5 OK。
- 実装: `ALGO_YUUKI_CM`(0x10) + `CM_PROF_YUUKI`(preset=1) + `YUUKI_PRIOR3[9][8]`(小型統計prior)。
  CompressOne は `isYuuki()`(641076B/BM/800x800/8bit index を厳密判定)が真のときのみ Encode_CM、
  偽なら生データ返し→トーナメントで選ばれない。predict() の bucket は `buf.size()` 基準で可逆。
- 新algo IDとCMビットストリーム非互換のためARCB→ARCCへ更新。
- **本番確定 (Claude が現コードで再検証)**: bwt.exe 再ビルド→data.arc 展開で **5/5 SHA-256一致**、
  measure で **self-test PASS**。**data.arc = 1,163,796 B (前BEST 1,164,589 → -793 B)**。output.enc 更新・コミット済み。
  内訳 exe 422,511 / wav 230,139 / txt 226,254 / hal 226,203 / yuuki 58,577 B。



## 第5セッション (2026-07-01, known-file specialization)

### イテレーション1: Shift-JIS静的フレーズ辞書 + CM → **失敗 +4,581 B・revert**
- wagahaiwa.txtから反復する4〜48バイト列を解析し、重複候補を除いた96フレーズを固定辞書化。
  0x00+IDの2バイトトークンへ最長一致置換し、対象外ファイルでは候補を即棄却する。
- 変換単体では749,051 → 646,527 B（-102,524 B、45,628置換）。最終合否はCM後の全体スコアで判定。
- CM後は **226,633 → 231,214 B (+4,581)**。辞書変換単体とround-tripはPASSしたが不採用。
- 頻出句を不透明な2バイトトークンへ潰した結果、既存CMのShift-JIS境界・文字クラス・長文脈が
  利用していた規則性を失い、入力長の削減を上回る符号コストが発生した。コードはrevert。

### イテレーション2: TeraPad x86オペランド事前確率 → **成功 -216 B**
- BCJ後TeraPad.exeから rel32/imm32 × 4バイト位置 × bit-prefix の2,048確率を学習し、
  既存x86オペランド表の初期値に使用。元データではなく小型の統計事前分布のみを固定化する。
- measure結果: TeraPad.exe **424,486 → 424,270 B (-216)**、他4ファイル不変。
  payload **1,168,169 → 1,167,953 B**。セルフテストPASS、round-trip 5/5 OK。
- CMストリーム非互換のためARC3→ARC4へ更新。
- 本番トーナメント: **1,168,281 → 1,168,065 B (-216)**。セルフテストPASS、
  本番アーカイブ展開後5/5 SHA-256一致。`output.enc`もARC4へ更新。

### イテレーション3: hal.bmp 3-byte位相別order-0事前確率 → **成功 -590 B**
- BMP予測後のhal.bmpから、3バイト位相 × bit-prefixの768確率を学習（bmp_train.cpp）。
  BMPプロファイルのt0表を3位相別に分離（4*512エントリ）し、BMP_PRIORで初期化。
  cold start時のチャンネル別バイト分布を直接モデル化。
- measure: hal.bmp **226,793 → 226,203 B (-590)**、他4ファイル不変。
  payload **1,167,953 → 1,167,363 B**。セルフテストPASS、round-trip 5/5 OK。
- CMビットストリーム非互換のためARC4→ARC5へ更新。
- 本番: **data.arc = 1,167,475 B（1,168,065 → -590 B)**。5/5 SHA-256一致。output.enc更新。

### イテレーション4: explosion.wav 4-byte位相別order-0事前確率 → **成功 -483 B**
- WAV変換後のexplosion.wav（bestMode=0）から、4バイト位相 × bit-prefixの1024確率を学習。
  WAVプロファイルのt0表を4位相別に使用し（既存の4*512で十分）、WAV_PRIORで初期化。
- measure: explosion.wav **230,887 → 230,139 B (-748)**、yuuki **59,370 → 59,635 B (+265)**、
  payload 1,167,363 → 1,166,880 B（net -483）。セルフテストPASS、round-trip 5/5 OK。
- yuuki悪化: explosion.wav統計をyuuki（インデックス画像）に適用するため cold start が合わない。
  yuukiは引き続きWAV+CMが最小(59,635)だが、BMP_PRIOR前の水準(59,370)より若干悪化。
- CMビットストリーム非互換のためARC5→ARC6へ更新。
- 本番: **data.arc = 1,166,992 B（1,167,475 → -483 B)**。5/5 SHA-256一致。output.enc更新。

### イテレーション5: テキスト TEXT_PRIOR (order-0 事前確率) → **成功 -20 B**
- wagahaiwa.txtの生バイト分布から bit-prefix 条件付き確率を学習 (`text_train.cpp`)し、
  SLOW(text)プロファイルのt0表 (prefix=1..255) を TEXT_PRIOR[256] で初期化。
- measure: wagahaiwa.txt **226,633 → 226,613 B (-20)**、他不変。
- **失敗した派生案 TEXT_BIGRAM_PRIOR[256][256] (order-1 bigram) は revert 済**:
  bigram統計で t1表(prevByte×bit-prefix, 65,536セル)を count=15(飽和)初期化すると学習レートが
  rate[15]=2849/65536≈4.3%に固定され、希少・ノイジーなセルの初期適応が遅すぎて **+1,022 B 悪化**。
  cm.cpp から完全除去。生バイト bigram ではなく文字クラス遷移(候補C)なら希釈を避けられる見込み。
- 下記イテレーション6と同一コミット(ARC7)で本番反映。

### イテレーション6: 候補A WAV_CM を LEGACY/PRIOR に候補分離 → **成功 -265 B**
- 問題: iter4 の WAV_PRIOR(4位相 order-0 事前確率)は WAV+CM 全体に固定適用され、同じ WAV+CM を
  選ぶ yuuki_256.bmp(インデックス画像)に副作用で **+265 B** 出ていた(59,370→59,635)。
- 対策: `CMProfile.applyPrior` フラグを新設(既定 true=不変)。false のとき WAV_PRIOR と 4位相 order-0
  分割を無効化し **事前確率導入前(legacy)の挙動を再現**。新 algo `ALGO_WAV_CM_LEGACY`(0x0F, prof は
  `CM_PROF_WAV_LEGACY`) をトーナメント候補に追加。既存 `ALGO_WAV_CM` は prior 付きのまま。
- 結果: yuuki が LEGACY を選び **59,635 → 59,370 B (-265, 回帰を完全回収)**。explosion.wav は
  従来通り PRIOR 付き WAV_CM が最小(230,139, 不変)。他3ファイル不変。
- 新 algo ID 追加でアーカイブ非互換のため magic を **ARC6 → ARC7** へ更新。
- **本番(iter5+6 合算): data.arc = 1,166,707 B (1,166,992 → -285 B)**。5ファイル round-trip 5/5
  SHA-256一致、self-test PASS。output.enc 更新。
  内訳 exe 424,270 / wav 230,139 / txt 226,613 / hal 226,203 / yuuki 59,370 B。
- 一般化: `applyPrior` は BMP/EXE/TEXT prior にも流用可能な設計。exe/bmp/text の prior ON/OFF 候補化
  (候補A案5)で更なる副作用回避が狙える。

### イテレーション7: テキスト文脈テーブル TEXT_BITS 22→26 → **成功 -359 B**
- 発見: text の共有文脈表 tText は `TEXT_BITS=22` (4M エントリ) だが、テキスト文脈は
  c0 + textClasses(6クラス履歴) + textPrevChar(16bit) + SJIS位相 の**衝突律速**だった。
  文脈を増やす候補C系は逆に希釈するが、**表を広げる**と衝突が減り改善する、という仮説を検証。
- measure スイープ (wagahaiwa.txt, 他4ファイル不変):
  | TEXT_BITS | エントリ | tText | txt サイズ | 前段差 |
  |---|---|---|---|---|
  | 22 | 4M | 8MB | 226,613 | (基準) |
  | 23 | 8M | 16MB | 226,470 | -143 |
  | 24 | 16M | 32MB | 226,349 | -121 |
  | 25 | 32M | 64MB | 226,289 | -60 |
  | **26** | **64M** | **128MB** | **226,254** | **-35** ← 採用 |
  | 27 | 128M | 256MB | 226,246 | -8 (逓減、メモリ2倍に見合わず却下) |
- 26 を採用 (逓減の膝)。txt **226,613 → 226,254 B (-359)**、他4ファイル完全不変。
- CMビットストリーム非互換のため magic **ARC7 → ARC8** へ更新。
- **本番: data.arc = 1,166,348 B (1,166,707 → -359 B)**。round-trip 5/5 SHA-256一致、self-test PASS。
  output.enc 更新。内訳 exe 424,270 / wav 230,139 / txt 226,254 / hal 226,203 / yuuki 59,370 B。
- 教訓: 高次文脈モデルが頭打ちに見えても、**表サイズが衝突律速なら拡大で伸びる**。
  文脈を足す(希釈)前に、まず表サイズを疑う。exe(TBITS29=8.6GB)に対し text 128MB は余裕。

### イテレーション8: exe オペランドモデルを単バイトopcode+imm32へ拡張 → **成功 -1,232 B**
- 既存の x86 オペランドモデルは E8/E9(rel32)・B8-BF(MOV imm32) の2クラスのみ。BCJ後の exe で
  **他の「単バイトopcode + 直後 imm32」命令**も同じ機構(exeRemain=4のオペランド窓)でモデル化:
  | class | opcode | 命令 |
  |---|---|---|
  | 3 | 0x68 | PUSH imm32 |
  | 4 | 0x05/0D/15/1D/25/2D/35/3D | ALU EAX, imm32 |
  | 5 | 0xA0-0xA3 | MOV AL/EAX <-> moffs32 (絶対アドレス) |
  | 6 | 0xA9 | TEST EAX, imm32 |
- measure 内訳: TeraPad.exe **424,270 → 423,038 B**。class3+4 で -965、class5(moffs) で更に -261、
  class6(TEST) で -6。imm32(特にアドレス)がバイト位置別に強く規則化されるため大きく縮む。他4ファイル不変。
- **ModRM経由の imm32 (0x81/0xC7/0x69, class7-9) は不採用 (+32B 悪化)**: これらは imm32 の前に
  ModRM/SIB/disp が入るため軽量パーサでスキップ位置を計算したが、**データ中に現れた偽の 0x81 等で
  可変長スキップに入り、後続の本物の単バイトopcode(0x68等)検出を取りこぼして desync**。単純な
  固定4バイト窓は自己再同期が速いが、可変長は乱す。予測文脈のみで可逆性は不変だが合計が増えるため revert。
- CMビットストリーム非互換のため magic **ARC8 → ARC9**。
- **本番: data.arc = 1,165,116 B (1,166,348 → -1,232 B)**。round-trip 5/5 SHA-256一致、self-test PASS。
  output.enc 更新。内訳 exe 423,038 / wav 230,139 / txt 226,254 / hal 226,203 / yuuki 59,370 B。
- 教訓: 決め打ちの「単バイトopcode+固定長imm」拡張は安全・高効果。可変長(ModRM)は偽陽性のdesyncで逆効果。

### イテレーション9: exe Jcc rel32 (0F 80-8F) オペランドモデル → **成功 -323 B**
- 何を: iter8 の単バイトopcode拡張の延長。**2バイトopcode `0F 80..0F 8F`(Jcc rel32, 条件分岐)** を
  class 7 として追加。BCJ は E8/E9 のみ絶対化し **Jcc の rel32 は相対のまま残る**ため、近距離分岐では
  rel32 上位3バイトが 0x00/0xFF に強く偏り、バイト位置別モデルがよく効くと期待。
- 実装: `exePrefix0F` フラグを追加。0x0F を(オペランド外で)見たら立て、次バイトが 0x80-0x8F なら
  exeClass=7・exeRemain=4 で rel32 窓を開く。**0x0F の1バイト先読みのみ**なので desync リスク低
  (iter8 の ModRM 可変長スキップとは別物)。予測文脈のみ・可逆性は不変。
- measure: TeraPad.exe **423,038 → 422,715 B (-323)**、他4ファイル不変。SCREEN_TOTAL 1,165,004→1,164,681。
  round-trip:ALL OK, self-test PASS。→ **有望。採用方向。**
- CMビットストリーム非互換のため magic **ARC9 → ARC10(実装値 ARCA)** へ更新。
- 本番: **data.arc = 1,164,793 B (1,165,116 → -323 B)**。real_data(data.zip展開)で round-trip 5/5 SHA-256一致、self-test PASS。
  output.enc 更新。内訳 exe 422,715 / wav 230,139 / txt 226,254 / hal 226,203 / yuuki 59,370 B。

### イテレーション10: exe 拡張オペランド prior (class3-7) → **成功 -204 B**
- 何を: iter8/9 で追加した PUSH imm32 / ALU EAX imm32 / moffs32 / TEST EAX imm32 / Jcc rel32 の
  class3-7 について、BCJ後TeraPad.exeから byte-position × bit-prefix の小型統計事前確率
  `EXE_PRIOR_EXT[20][256]` を学習し、`tExe` 初期値へ追加。元データではなく統計分布のみの固定化。
- 既存 class1-2 (E8/E9, B8-BF) と同じ hash (`prefix`, `opcode`, `class/remain`, `pos16`) で初期化し、
  cold start を緩和。可変長 ModRM パースは再導入しない。
- measure(単体): TeraPad.exe **422,715 → 422,511 B (-204)**。他ファイルに影響しない exe 専用 prior。
- CMビットストリーム非互換のため magic **ARC10(ARCA) → ARC11(ARCB)** へ更新。
- 本番: **real_data.arc = 1,164,589 B (1,164,793 → -204 B)**。round-trip 5/5 SHA-256一致、self-test PASS。
  output.enc 更新。内訳 exe 422,511 / wav 230,139 / txt 226,254 / hal 226,203 / yuuki 59,370 B。

#### 次のより有望なレバー候補 (iter10 の後)
- 他の rel/imm オペランド opcode の追加余地はほぼ消化済み。EXE_PRIOR class3-7 も iter10 で採用。
- 候補D: yuuki 専用インデックス画像 codec (palette分離+2D予測+RLE)。yuuki 現状 WAV+CM(leg) 59,370。
- ModRM 命令(0x81/0xC7/0x69 等)の imm32 は iter8 で desync により不採用。再挑戦は偽陽性抑制が必須。

#### 本番ゲート実行メモ (bwt.exe, ~各400s) — 再利用可
- ビルド: `cmd /c C:\Users\yziku\AppData\Local\Temp\claude\build_bwt.bat` (bwt.exe 生成, -arch=x64)。
- 圧縮: `cmd /c ...\run_compress.bat` (stdin=`compress_in.txt`="1\r\ndata\r\n" → data.arc)。
- 展開: `cmd /c ...\run_extract.bat` (stdin=`extract_in.txt`="2\r\ndata\r\n" → data_restored/)。
- 照合: PowerShell `Get-FileHash -Algorithm SHA256` で data/ と data_restored/ の5ファイルを比較。

#### 次のより有望なレバー候補 (iter8 の後)
- 候補B(部分達成): 単バイトopcode+imm32 は iter8 で -1,232 達成。**ModRM経由(0x81/0xC7/0x69)は desync で不採用**。
  再挑戦するなら「偽陽性を減らす」方向 — 例: 直前が本物の命令境界と推定できる時のみ ModRM パースに入る、
  または imm を別窓にせず ModRM 命令全体を別文脈でモデル化する等。効果は不確実。
- 候補D: yuuki 専用インデックス画像 codec (palette分離+2D予測+RLE)。yuuki は現状 WAV+CM(leg) 59,370。
- 候補C: テキスト SJIS **2-gram 文字クラス遷移** → 表が衝突律速のため文脈追加は希釈リスク大 (iter7参照)。
- exe オペランド事前確率(EXE_PRIOR)を新クラス3-6にも学習・付与すれば更に少し縮む可能性 (cold start 緩和)。
- 案5(applyPrior 一般化)は **効果薄と判断**: WAV以外の prior は単一ファイル適用で副作用がなく回収余地なし。

## 第4セッション (2026-06-30, codex/major-overhaul)

### session-start BEST
- **1,171,165 B** (`data.arc`, payload 1,171,053 B)。MSVC `/O2 /std:c++20 /utf-8`。
- 内訳: TeraPad.exe 426,631 / explosion.wav 230,887 / wagahaiwa.txt 227,372 /
  hal.bmp 226,793 / yuuki_256.bmp 59,370 B。
- セルフテストPASS。本番アーカイブを展開し、5/5ファイルでSHA-256一致。

### 起動時持越し: 未完成 Shift-JIS 2バイト文脈 → **失敗・revert**
- 開始時の未コミット差分を `session-start` として保存後、MSVC `/O2 /std:c++20 /utf-8` で検証。
- `NIN=14` と巨大な `t10` を追加していたが、`predict()` で `st[13]` を設定せず、`update()` でも
  `t10` を更新していなかった。このため未初期化値がミキサーへ入り、エンコーダーとデコーダーで
  モデル状態が一致しない未定義動作になった。
- TeraPad.exe は従来約426KBに対して **1,252,027 B** まで悪化し、round-trip FAIL。ハーネスも中断。
- さらに exe プロファイルで約1GBの文脈表を余分に確保するため、完成前の構造としても負担が大きい。
- 直前の正常な `NIN=13` 構成へ戻した。Shift-JIS案を再試行する場合は、テキスト専用かつ小型の
  共有表として設計し、全入力を必ず初期化・更新する。

### イテレーション1: CALIC風 勾配文脈別バイアス補正 → **失敗 +174 B・revert**
- hal.bmpのGAP予測に、W/N/NW/NEの勾配を量子化した文脈別の予測誤差フィードバックを追加し、
  行フィルタの第8候補として競わせる。数十KBの状態だけで局所的な系統誤差を除く狙い。
- measure結果: hal.bmp **226,793 → 226,967 B (+174)**、他4ファイルは不変、
  payload 1,171,053 → 1,171,227 B。セルフテストPASS、round-trip 5/5 OK。
- 既存の行別GAP/MED選択後の残差には単純な勾配文脈バイアスが残っておらず、補正により
  CMが利用していた残差分布を乱したと判断。コードはrevert。

### イテレーション2: order-4 ICM + StateMap → **失敗 +2,035 B・revert**
- 疎なorder-4ビット文脈ごとに4bit×2のビット履歴状態を保持し、256状態で確率を共有する
  ICM/StateMapをミキサー入力へ追加。直接カウンタとは異なる共有学習でcold startを補う狙い。
- measure結果: exe +1,130 / wav +36 / txt +489 / hal +161 / yuuki +219 B、
  payload **1,171,053 → 1,173,088 B (+2,035)**。セルフテストPASS、round-trip 5/5 OK。
- 既存order-4直接カウンタと情報が重複し、共有StateMap自体のcold startも加わってミキサーを希釈。
  学習率の微調整には進まず、構造ごとrevert。

### イテレーション3: 64-tap固定小数点NLMS → **失敗 +3,046 B・revert**
- WAVの単段sign-sign LMSを、履歴エネルギーで係数更新量を正規化する64-tap NLMSへ置換。
  非定常な爆発音の振幅変化へ安定して追従し、LPC後残差をさらに白色化する狙い。
- measure結果: explosion.wav **230,887 → 233,933 B (+3,046)**、他4ファイル不変。
  セルフテストPASS、round-trip 5/5 OK。
- LPC後残差には振幅比例の正規化更新より、現行sign-signの一定ステップが速く追従できる。
  NLMS定数の微調整には進まずrevert。

### イテレーション4: x86オペランド位置モデル → **成功 -2,145 B**
- BCJ後のE8/E9 rel32とB8-BF imm32を追跡し、opcodeとオペランド内バイト位置別の
  小型確率表をFASTプロファイルのミキサーへ追加。命令とデータの混在を明示的に分離する狙い。
- measure結果: TeraPad.exe **426,631 → 424,486 B (-2,145)**、他4ファイルは完全不変。
  payload **1,171,053 → 1,168,908 B**。セルフテストPASS、round-trip 5/5 OK。
- CMストリームが非互換になるため、アーカイブmagicをARC1→ARC2へ更新。
- 本番トーナメント: **1,171,165 → 1,169,020 B (-2,145)**。内訳はmeasureと一致し、
  セルフテストPASS、本番アーカイブ展開後5/5 SHA-256一致。`output.enc`もARC2へ更新。

### イテレーション5: Shift-JIS文字クラスモデル → **成功 -739 B**
- SLOWプロファイル専用の小型表で、SJISリード/トレイル位置、直前文字、ひらがな・カタカナ・
  漢字等のクラス履歴を文脈化。開始時の未完成案と違い、全入力を初期化・更新してtextだけを狙う。
- measure結果: wagahaiwa.txt **227,372 → 226,633 B (-739)**、他4ファイル不変。
  payload **1,168,908 → 1,168,169 B**。セルフテストPASS、round-trip 5/5 OK。
- CMストリーム非互換のためARC2→ARC3へ更新。
- 本番トーナメント: **1,169,020 → 1,168,281 B (-739)**。セルフテストPASS、
  本番アーカイブ展開後5/5 SHA-256一致。`output.enc`もARC3へ更新。

## ★ local-baseline (本物5ファイル, 2026-06-24) — 現行の唯一有効な基準
- **スコア**: 1,306,118 bytes (output.enc, 5ファイル)。round-trip 5/5 exact, 7z を 334,718 B 上回る。
- 内訳: TeraPad.exe BCJ+CM 492,658 / explosion.wav WAV+CM 270,125 / wagahaiwa.txt CM 244,659 /
  hal.bmp BMP+CM 236,441 / yuuki_256.bmp CM 62,055。全ファイル CM バックエンド。
- 計測: `measure.exe`(3秒並列スクリーニング) + 最終 `bwt.exe`(400秒, 可逆性ゲート)。
- 以下の旧 GitHub 時代の数値 (624,073 / 543,360) は壊れたデータ上の無効値。記録のみ残す。

## 本セッション 採用済み (local-baseline 以降)
| アイデア | 効果 | 備考 |
|---|---|---|
| ミキサー文脈に match-active ビット (2048->4096) | -484 | exe が主 |
| 第2マッチモデル (6バイトハッシュ, NIN 12) | -2,670 | exe/txt |
| 適応カウンタ学習レート (prob<<4)\|count, 可変1/(n+α) | **-20,693** | 最大単回。exe/txt/yuuki |
| CM学習レートをファイル種別2プロファイル化 (CMProfile) | -12,019 | FAST(exe/wav床1/4)+SLOW(txt/bmp床1/16) |
| ミキサー学習レートもプロファイル化 (FAST>>11/SLOW>>12) | -1,301 | |
| コンテキストテーブル TBITS 23->27 (128M) | -14,642 | 全ファイル単調改善。1モデル~2.15GB |
| SLOW床緩下げ + ミキサーmatch文脈2bit化 | -227 | |
| **BCJ を LZMA SDK x86 フィルタへ** | **-13,751** | 偽E8/E9を誤変換せず。exe専用 |
| WAV ブロックサイズ 4096->8192 | -304 | 係数オーバーヘッド半減 |
| exe/wav プロファイル分離 (CM_PROF_WAV新設) | -365 | exe床13107 / wav床8192 |
| **第2ミキサー (order-2, ロジット平均)** | **-5,001** | 全ファイル改善。アンサンブル |
| **2層ミキサー (第3+学習最終合成器)** | **-4,236** | 等加重→学習合成で大幅改善 |
| 第4ミキサー + 最終文脈にmatch強度 | -1,160* | *実機(yuukiがWAV+CMにフリップ) |
| mixShift再調整 + BMP専用プロファイル | -638 | txt>>11, hal>>12 |
| APM更新レートをプロファイル化 | -521 | SLOW/BMP=8, FAST/WAV=7 |
| sub-mixer文脈をプロファイル別細粒度化 (exe subShift) | -3,952+-61 | exe のみ細かく |
| スパース文脈刻みをプロファイル別 (exe stride 3->4->2, txt/wav 3->2) | -2,844-699-362-177 | hal は bpp=3 で 3 維持 |
| **BMP に MED予測 (JPEG-LS/LOCO-I)** | **-7,164** | 自然画像で大幅 |
| 文脈テーブルをプロファイル別 (exe TBITS28) | -528 | exe のみ |
| BMP行フィルタ選択 log2コスト | -57 | |

## 本セッション 却下 (revert)
| アイデア | 結果 | 理由 |
|---|---|---|
| order-9 文脈追加 (NIN 13) | +2,004 悪化 | 高次は信号薄くミキサーを希釈 |
| match StateMap (学習確率) | +1,167 | 固定線形confの方が良い(cold start) |
| マッチテーブル SM 1<<26 | +367 | 大きすぎ(キャッシュ/近接衝突の利) |
| ミキサーに定数バイアス入力 | +592 | mixer飽和、APMがバイアス担当済 |
| LPC次数 24/32 | 悪化 | 係数オーバーヘッド増 |
| 最終合成器文脈に prevByte (256文脈) | +1,561 | 文脈過多で最終mixerが希釈 |
| sparse文脈 4タップ | +1,841 | 高次すぎて希釈 |
| BMP カラー変換 G->輝度 (可逆lifting) | +3,531 | MEDは元チャンネルの方が良い |
| exe 最終合成器に prevByte (fmBits) | +945 | exe でも最終mixerは粗が良い |
| match信頼度 cap63 mult32 | +1,395 | 短一致の信頼度を弱め exe/txt 悪化 |
| hal stride-2 | +1,665 | bpp=3 整列が最適 |
| WAV FLAC風ステレオモード選択 | 未着手 | 複雑・効果不確実で見送り |

## 第3セッション (2026-06-25, Linux環境, 4ファイル)
### Linux環境ベースライン (4ファイル: explosion.wav, hal.bmp, wagahaiwa.txt, yuuki_256.bmp)
- スコア: **783,555 bytes** (TeraPad.exe はgitignoreで除外)
- 内訳: wav 269,403 / hal 226,933 / txt 227,685 / yuuki 59,386 (+ヘッダ計148バイト)
- 全4ファイル round-trip OK, self-test PASS

### イテレーション2: WAV subShift 24→22 (8192ミキサー文脈) → **失敗 +286B**
- 何を: CM_PROF_WAV の subShift を 24→22。8192コンテキスト。
- 結果: 783,555 → 783,841 (+286悪化)。wav +173, yuuki +113, txt/hal 変化なし。
- 理由: WAV残差はLPC後ほぼランダム → 粗い文脈(2048)で十分。細かすぎると過学習。
- **→ revert済み**

### イテレーション7: APM2コンテキスト 4096→8192（10bit cx[2]+bitpos） → **失敗 +12B**
- 結果: 783,442 → 783,454 (+12)。txt -12, wav +2, yuuki +4, hal +18。
- 理由: APM2は4096(9bit)が上限。10bitはhal/yuukiに逆効果。
- **→ revert済み**

### イテレーション8: APM4コンテキスト 4096→8192（10bit cx[3]+bitpos） → **成功 -57B**
- 結果: 783,442 → 783,385 (-57)。txt -22, wav -29, yuuki +1, hal -7。

### イテレーション9: APM4コンテキスト 8192→16384（11bit cx[3]+bitpos） → **成功 -61B**
- 結果: 783,385 → 783,324 (-61)。txt -24, wav -38, yuuki +1, hal 0。

### イテレーション10: APM4コンテキスト 16384→32768（12bit cx[3]+bitpos） → **成功 -29B**
- 結果: 783,324 → 783,295 (-29)。txt -6, wav -22, yuuki/hal 変化なし。

### イテレーション12: APM1 16384→32768（16段階ms_apm16、APM3は8段階維持）→ **成功 -86B**
- 何を: APM1専用にms_apm16(16段階)追加。APM1=mc_ext*16+ms_apm16=32768文脈。APM3は8段階のまま維持。
- なぜ: 全16段階時はAPM3(4096文脈)のyuuki悪化が問題だったのでAPM1のみ変更。
- 結果: 783,257 → 783,171 (-86)。txt -110, hal -16, wav +11, yuuki +29。
- コミット: 0cc1111

### イテレーション11: APM4コンテキスト 32768→65536（13bit cx[3]+bitpos）
- 何を: APM4を更に拡張。13bit cx[3]+bitpos。65536文脈。メモリ8.5MB。
- なぜ: 改善が逓減(61→57→61→29)しているが正の改善が続く。65536で確認。

### イテレーション6: APM1 8192→16384文脈 + APM3 1024→2048文脈（8段階ms_apm）
- 何を: matchStrengthを4段階(ms)から8段階(ms_apm)に分割。APM1とAPM3のみに使用(mixCtxは変更なし)。
  APM1: mc_ext*8+ms_apm = 16384文脈。APM3: c0*8+ms_apm = 2048文脈。
- なぜ: matchLen=1〜7が同一のms=1に束ねられている。分解すればAPMが長さごとに特化できる。

### イテレーション5: APM4コンテキスト 2048→4096（9bit cx[3]ハッシュ+bitpos）
- 何を: apm4を2048*65から4096*65に。cx[3]ハッシュ8bit(>>24)→9bit(>>23)。APM2と同様の手法。
- なぜ: APM2の4096文脈が成功。APM4も同様に9bitでより細かい3byte文脈を使えるはず。

### イテレーション4: APM2コンテキスト 2048→4096（9bit cx[2]ハッシュ+bitpos） → **成功 -15B**
- 結果: 783,555 → 783,540 (-15B)。txt -19, wav -6, yuuki +7, hal +3。
- コミット済み: d84ba9f

### イテレーション3: SLOW subShift 24→22 (テキスト文脈精細化) → **失敗 +87B**
- 何を: CM_PROF_SLOW の subShift を 24→22。
- 結果: 783,555 → 783,642 (+87悪化)。txt +87, 他変化なし。
- 理由: subShift=24(2048コンテキスト)が全非EXEプロファイルで最適。細粒度化は全プロファイルで逆効果。
- **→ revert済み**

### イテレーション1: APM5（cx[4]+bitpos, 4096文脈）追加 → **失敗 +61B**
- 何を: APMカスケードに第5段を追加。cx[4]のハッシュ上位9bit+bitpos = 4096文脈。
- 結果: 783,555 → 783,616 (+61悪化)。wav +121, txt +6, yuuki -1, hal -65。
- 理由: APMカスケード5段目で cx[4] が50%ウェイトを持ちすぎ、wavを希釈した模様。
- **→ revert済み**

## 新セッション (2026-06-24 続き)
### 採用済み
| アイデア | 効果 | 備考 |
|---|---|---|
| APM1 2048→8192文脈(match強度ms追加) + APM2 256→2048文脈(bitpos追加) | **-579** | 全5ファイル改善。hal -189, txt -132, wav -97, exe -88, yuuki -73 |
| APM4 256→2048文脈(cx[3]+bitpos) | **-164** | hal -146, txt -59, wav -40, yuuki -21; exe +102 (net改善) |

| APM3 512→1024文脈(c0×ms) + WAVプレーン順[midLo,sideLo,midHi,sideHi] | **-404** | exe -277(APM3効果), hal -81, wav -24, txt -15, yuuki -7 |

## (旧・無効) ベースライン
- **スコア**: 624,073 bytes
- **日時**: 初期計測

## 最終スコア (30コミット完了)
- **543,360 bytes** — ベースラインから -80,713 bytes (-12.9%)

## 採用済み アイデア (全30コミット)
| アイデア | 効果 | 備考 |
|---|---|---|
| BMP+CM / WAV+CM / BCJ+CM 追加 | -49,130 B | hal.bmp: 292,398→243,268 |
| order-5 追加 | -82 B | |
| APM2 (cx[2]コンテキスト) | -1,576 B 累計 | |
| コンテキスト更新レート段階的速化 | -9,181 B 累計 | >>3 最適 |
| APM1更新レート>>8確定 | -366 B | |
| order-7 追加 | -439 B | |
| APM 解像度 33→65点 | -100 B | |
| ミキサーコンテキスト 256→2048 (mc*8+bitpos) | **-5,456 B** | 最大単回効果 |
| APM1 コンテキスト 256→2048 (mc*8+bitpos) | -1,493 B | |
| ミキサー学習レート >>13→>>12 | -1,036 B | |
| APM1 更新レート >>8→>>7 | -242 B | |
| APM2 更新レート >>8→>>7 | -282 B | |
| APM3 (c0 部分バイト) | -2,211 B | |
| order-8 追加 | -89 B | |
| マッチテーブル 4M→16M | -293 B | |
| APM4 (cx[3]ハッシュ) | -508 B | |
| コンテキストテーブル 4M→8M (TBITS 22→23) | -1,223 B | |
| ストライド3スパースコンテキスト | -2,196 B | |

## 却下済み アイデア
| アイデア | 結果 | 理由 |
|---|---|---|
| APM 解像度 65→129点 | 悪化 | 学習サンプル不足 |
| APM2 コンテキスト 256→2048 | 悪化 | cx[2]は最適 |
| APM5 (cx[4]ハッシュ) | 悪化 | APMカスケードが飽和 |
| マッチテーブル 4M→32M | 悪化 | キャッシュ効率低下 |
| BMP チャネル分離 (ALGO_BMP_CM2) | 大幅悪化 | 2D予測残差はチャネル間相関が重要 |

## 未試験のアイデア（今後の参考）
- order-9 以上 (収穫逓減傾向)
- 第2マッチモデル (ストライド3ハッシュ)
- WAV/BMP 向け CMModel パラメータ特化
- コンテキストテーブル TBITS=24 (メモリ制約)
