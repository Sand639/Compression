# 改良台帳

## ベースライン
- **スコア**: 624,073 bytes
- **日時**: 初期計測
- 各ファイル: wagahaiwa(CM:264745) + explosion(Range:118) + yuuki_256(CM:66664) + hal.bmp(BMP+LZSS:292398) + オーバーヘッド148

## アイデア管理

### 未試行
- [ ] **BMP+CM** (ALGO_BMP_CM): BMP 2D予測 -> LZSS の代わりに CM。hal.bmpで292,398->? 狙い。最優先。
- [ ] **WAV+CM** (ALGO_WAV_CM): WAV Mid/Side+LPC -> CM。explosion.wavが131バイトと微小なので効果は限定的。
- [ ] **BCJ+CM** (ALGO_BCJ_CM): TeraPad.exeがないので今は意味なし。
- [ ] **CMモデル強化**: APM多段化、スパース文脈追加、バイト状態機械。
- [ ] **CM内order強化**: order-5を追加。
- [ ] **varint化**: ヘッダのuint32/uint64をLEB128化。数バイト程度の削減。

### 試行中
(なし)

### 採用済み (合格)
(なし)

### 却下済み (不合格)
(なし)
