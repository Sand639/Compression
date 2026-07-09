#include "compress.h"

// ==========================================================================
// 繧ｳ繝ｳ繝・く繧ｹ繝医Α繧ｭ繧ｷ繝ｳ繧ｰ (CM) 窶・莠悟､邂苓｡鍋ｬｦ蜿ｷ蛹・+ 隍・焚譁・ц繝｢繝・Ν + 繝ｭ繧ｸ繧ｹ繝・ぅ繝・け豺ｷ蜷・
//   lpaq 邉ｻ縺ｮ霆ｽ驥冗沿縲ゅΔ繝・Ν: order-1/2/3 譁・ц + 繝槭ャ繝√Δ繝・Ν縲ょ推繝｢繝・Ν縺ｮ莠域ｸｬ繧・
//   stretch 鬆伜沺縺ｧ驥阪∩莉倥″蜉邂・(mixer) 縺・squash 縺ｧ譛邨ら｢ｺ邇・↓縲・ 繝薙ャ繝医＃縺ｨ縺ｫ
//   螳溘ン繝・ヨ縺ｧ蜷・㍾縺ｿ縺ｨ繝｢繝・Ν遒ｺ邇・ｒ譖ｴ譁ｰ縲ゅお繝ｳ繧ｳ繝ｼ繝・繝・さ繝ｼ繝峨・蜷御ｸ繝｢繝・Ν繧帝ｧ・虚縺・
//   隨ｦ蜿ｷ蝎ｨ縺縺代′逡ｰ縺ｪ繧九・縺ｧ螳悟・蜿ｯ騾・ょ・蜉・ [u64 size][莠悟､邂苓｡薙せ繝医Μ繝ｼ繝]縲・
// ==========================================================================
static int CM_squash(int d) {
    static const int t[33] = {1,2,3,6,10,16,27,45,73,120,194,310,488,747,1101,1546,2047,
                              2549,2994,3348,3607,3785,3901,3975,4022,4050,4068,4079,4085,4089,4092,4093,4094};
    if (d >  2047) return 4095;
    if (d < -2047) return 0;
    int w = d & 127; d = (d >> 7) + 16;
    return (t[d] * (128 - w) + t[d + 1] * w + 64) >> 7;
}
struct CM_Stretch {
    int v[4096];
    CM_Stretch() {
        int pi = 0;
        for (int x = -2047; x <= 2047; ++x) { int val = CM_squash(x); for (int j = pi; j <= val; ++j) v[j] = x; pi = val + 1; }
        for (int j = pi; j < 4096; ++j) v[j] = 2047;
    }
};
static const CM_Stretch CM_STR;


struct BinaryRangeEncoder {
    uint32_t x1 = 0, x2 = 0xFFFFFFFFu;
    std::vector<uint8_t>& out;
    explicit BinaryRangeEncoder(std::vector<uint8_t>& o) : out(o) {}
    void encode(int bit, int p) {                 // p = P(bit==1), 12bit (1..4095)
        if (p < 1) p = 1; else if (p > 4095) p = 4095;
        uint32_t xmid = x1 + static_cast<uint32_t>((static_cast<uint64_t>(x2 - x1) * p) >> 12);
        if (bit) x2 = xmid; else x1 = xmid + 1;
        while (((x1 ^ x2) & 0xFF000000u) == 0) { out.push_back(static_cast<uint8_t>(x2 >> 24)); x1 <<= 8; x2 = (x2 << 8) | 0xFF; }
    }
    void flush() { for (int i = 0; i < 4; ++i) { out.push_back(static_cast<uint8_t>(x1 >> 24)); x1 <<= 8; } }
};
struct BinaryRangeDecoder {
    uint32_t x1 = 0, x2 = 0xFFFFFFFFu, x = 0;
    const uint8_t* in; size_t pos = 0, size;
    BinaryRangeDecoder(const uint8_t* d, size_t s) : in(d), size(s) { for (int i = 0; i < 4; ++i) x = (x << 8) | rb(); }
    uint8_t rb() { return pos < size ? in[pos++] : 0; }
    int decode(int p) {
        if (p < 1) p = 1; else if (p > 4095) p = 4095;
        uint32_t xmid = x1 + static_cast<uint32_t>((static_cast<uint64_t>(x2 - x1) * p) >> 12);
        int bit = (x <= xmid) ? 1 : 0;
        if (bit) x2 = xmid; else x1 = xmid + 1;
        while (((x1 ^ x2) & 0xFF000000u) == 0) { x1 <<= 8; x2 = (x2 << 8) | 0xFF; x = (x << 8) | rb(); }
        return bit;
    }
};

// 驕ｩ蠢懊き繧ｦ繝ｳ繧ｿ縺ｮ蟄ｦ鄙偵Ξ繝ｼ繝・(16.16 蝗ｺ螳壼ｰ乗焚, 1/(n+ﾎｱ)逶ｸ蠖・縲ゅヵ繧｡繧､繝ｫ遞ｮ蛻･縺斐→縺ｫ繝励Ο繝輔ぃ繧､繝ｫ繧帝∈縺ｶ:
//   SLOW = 菴弱＞蠎・~1/16): 螳壼ｸｸ逧・↑繝・く繧ｹ繝・逕ｻ蜒丞髄縺・(CM, BMP_CM)縲・
//   FAST = 鬮倥＞蠎・~1/5):  髱槫ｮ壼ｸｸ縺ｪ exe/髻ｳ螢ｰ谿句ｷｮ蜷代￠ (BCJ_CM, WAV_CM)縲・
// algo 繝舌う繝医・繧｢繝ｼ繧ｫ繧､繝悶↓菫晏ｭ倥＆繧悟ｾｩ蜿ｷ繧ょ酔縺・algo 繧定ｦ九ｋ縺溘ａ縲√・繝ｭ繝輔ぃ繧､繝ｫ驕ｸ謚槭・螳悟・蜿ｯ騾・・


// CM 莠域ｸｬ繝｢繝・Ν (encode/decode 蜈ｱ騾・
//   譁・ц繝｢繝・Ν: order 0,1,2,3,4,5,6 + 繝槭ャ繝・= 8 蜈･蜉帙Ｎixer + APM(莠梧ｬ｡謗ｨ螳・縲・
//   蜷・ユ繝ｼ繝悶Ν隕∫ｴ uint16 = (prob<<4)|count : prob 縺ｯ 12bit, count(0..15) 縺ｧ蟄ｦ鄙堤紫繧貞宛蠕｡縲・
struct CMModel {
    static const int NIN = 15;                     // o0..o8,stride3,match x3,x86 operand,SJIS text
    const int TBITS, TSIZE, TMASK;                  // t2..t9 縺ｮ繧ｵ繧､繧ｺ (繝励Ο繝輔ぃ繧､繝ｫ萓晏ｭ・
    const int SM;                                   // マッチテーブルサイズ (プロファイル依存: 画像系26, 他24)
    const int APM2SHIFT, APM2N;                     // APM2 文脈シフト/文脈数 (プロファイル依存: text 19, 他23)
    const size_t WN;                                // sub-mixer 文脈数 = 2^(32-subShift)*8 (subShift 連動)
    static const int EXE_BITS = 24, EXE_SIZE = 1 << EXE_BITS, EXE_MASK = EXE_SIZE - 1;
    static const int TEXT_BITS = 28, TEXT_SIZE = 1 << TEXT_BITS, TEXT_MASK = TEXT_SIZE - 1;  // 26→28: tText衝突減 (-30B)。29は-1でメモリ増に見合わず
    std::vector<uint16_t> t0, t1, t2, t3, t4, t5, t6, t7, t8, t9;  // 繝薙ャ繝育｢ｺ邇・(12bit, 蛻晄悄 2048)
    std::vector<uint16_t> tExe;                    // x86 opcode + operand byte position (FAST蟆ら畑)
    std::vector<uint16_t> tText;                   // Shift-JIS讒矩繝ｻ譁・ｭ励け繝ｩ繧ｹ譁・ц (SLOW蟆ら畑)
    std::vector<uint16_t> tBmp;                    // BMP残差 予測難易度文脈 (BMP_CM専用, st[14]兼用)
    std::vector<uint16_t> tYuuki;                  // yuuki 縦order-1: 上の行の同位置index (st[14]兼用)
    std::vector<uint16_t> tWav;                    // wav 同位相order-1: 1サンプル前 buf[p-4] (st[14]兼用)
    std::vector<uint32_t> matchTab, matchTab2, matchTab3;
    std::vector<uint8_t> buf;
    std::vector<int> w;                            // mixer 驥阪∩ (mixCtx 譁・ц x NIN)
    std::vector<int> w2;                           // 隨ｬ2 mixer 驥阪∩ (order-2 譁・ц x NIN)
    std::vector<int> w3;                           // 隨ｬ3 mixer 驥阪∩ (order-3 譁・ц x NIN)
    std::vector<int> w4;                           // 隨ｬ4 mixer 驥阪∩ (order-4 譁・ц x NIN)
    std::vector<int> wf;                           // 譛邨・mixer (sub-mixer 繧鍛itpos豈弱↓蟄ｦ鄙貞粋謌・
    std::vector<int> w5;                           // 第5 mixer 重み (order-5 文脈 x NIN, exe専用)
    bool useW5 = false;                            // exe のみ (subShift 14 の細かい文脈でのみ有効。他は +36〜+171 悪化)
    static const int NMIX = 5;
    int mix2Ctx = 0, mix3Ctx = 0, mix4Ctx = 0, mix5Ctx = 0, fmCtx = 0, fmLogit[NMIX] = {0,0,0,0,0};
    std::vector<uint16_t> apm;                     // 荳谺｡謗ｨ螳・(8192 譁・ц x 65 轤ｹ縲［atch蠑ｷ蠎ｦ莉倥″)
    std::vector<uint16_t> apm2;                    // 莠梧ｬ｡謗ｨ螳・(2048 譁・ц x 65 轤ｹ縲｜itpos莉倥″)
    std::vector<uint16_t> apm3;                    // 荳画ｬ｡謗ｨ螳・(1024 譁・ц x 65 轤ｹ縲…0ﾃ洋atch蠑ｷ蠎ｦ)
    std::vector<uint16_t> apm4;                    // 蝗帶ｬ｡謗ｨ螳・(2048 譁・ц x 65 轤ｹ縲…x[3]繝上ャ繧ｷ繝･+bitpos)
    uint32_t matchPtr = 0; int matchLen = 0;
    uint32_t matchPtr2 = 0; int matchLen2 = 0;     // 隨ｬ2繝槭ャ繝√Δ繝・Ν (6繝舌う繝医ワ繝・す繝･)
    uint32_t matchPtr3 = 0; int matchLen3 = 0;     // 隨ｬ3繝槭ャ繝√Δ繝・Ν (8繝舌う繝医ワ繝・す繝･)
    uint32_t cx[9] = {0,0,0,0,0,0,0,0,0};           // cx[k] = 逶ｴ霑・k 繝舌う繝医・繝上ャ繧ｷ繝･
    bool isExe = false, isBmp = false, isWav = false, isYuuki = false, exeActive = false;
    bool applyPrior = true;                          // false: legacy(prior/位相なし)
    int exeRemain = 0, exeClass = 0, exeOpcode = 0;
    bool exePrefix0F = false;                        // 直前が 0x0F (2バイトopcode prefix) か
    bool isText = false, sjisTrail = false;
    int sjisLead = 0, textIdx = 0;
    int bmpIdx = 0, prevResMag = 0;                // BMP残差文脈(st[14]兼用): 直前残差の大きさbucket
    // BMP残差フィルタ出力レイアウトの動的パース (Encode_Bmp_2DPredict のヘッダから取得)
    uint32_t bmpResidOff = 0, bmpStride = 0;       // 残差開始位置 / 行ストライド
    bool bmpParsed = false;
    int yuukiIdx = 0;                              // yuuki 縦order-1文脈(st[14]兼用) の現在index
    // インデックスBMPのヘッダ動的パース結果 (エンコード/デコードとも buf から同一手順で得る)
    uint32_t idxOff = 0, idxStride = 0, idxEnd = 0; // データ開始 / 行ストライド / データ終端
    bool idxParsed = false;
    int wavIdx = 0;                                // wav 同位相order-1文脈(st[14]兼用) の現在index
    uint16_t textPrevChar = 0;
    uint32_t textClasses = 0;                      // 逶ｴ霑・繝医・繧ｯ繝ｳ縺ｮ4bit譁・ｭ励け繝ｩ繧ｹ
    int c0 = 1, bitpos = 0, mc = 0, mc_ext = 0;    // mc_ext = mc*8+bitpos (APM1逕ｨ)
    int mixCtx = 0;                                // 繝溘く繧ｵ繝ｼ譁・ц = mc_ext*2 + match-active
    int ms_apm = 0;                                // 8谿ｵ髫・match strength (APM3蟆ら畑)
    int ms_apm16 = 0;                              // 16谿ｵ髫・match strength (APM1蟆ら畑)
    int st[NIN], idx[NIN], pr0 = 2048, prf = 2048, apmIdx = 0;
    int apm2Ctx = 0, apm2Idx = 0, apm2Wt = 0;
    int apm3Idx = 0, apm3Wt = 0;
    int apm4Ctx = 0, apm4Idx = 0, apm4Wt = 0;
    const int* rate = CM_RATE_SLOW;                // 驕ｩ蠢懊き繧ｦ繝ｳ繧ｿ蟄ｦ鄙偵Ξ繝ｼ繝医・繝励Ο繝輔ぃ繧､繝ｫ
    int mixShift = 12;                             // 繝溘く繧ｵ繝ｼ蟄ｦ鄙偵Ξ繝ｼ繝・(繝励Ο繝輔ぃ繧､繝ｫ萓晏ｭ・
    int apmShift = 7;                              // APM 譖ｴ譁ｰ繝ｬ繝ｼ繝・(繝励Ο繝輔ぃ繧､繝ｫ萓晏ｭ・
    int subShift = 24;                             // sub-mixer 譁・ц縺ｮ邏ｰ縺九＆ (繝励Ο繝輔ぃ繧､繝ｫ萓晏ｭ・
    int strideLen = 3;                             // 繧ｹ繝代・繧ｹ譁・ц縺ｮ蛻ｻ縺ｿ (繝励Ο繝輔ぃ繧､繝ｫ萓晏ｭ・

    CMModel(const CMProfile& prof)
              : TBITS(prof.tbits), TSIZE(1 << prof.tbits), TMASK((1 << prof.tbits) - 1),
                SM(1 << prof.mbits),
                APM2SHIFT(prof.apm2Shift), APM2N(1 << (35 - prof.apm2Shift)),
                WN((1ull << (32 - prof.subShift)) * 8),
                t0(9 * 512, 32768), t1(256 * 512, 32768), t2(TSIZE, 32768), t3(TSIZE, 32768),
                t4(TSIZE, 32768), t5(TSIZE, 32768), t6(TSIZE, 32768), t7(TSIZE, 32768),
                t8(TSIZE, 32768), t9(TSIZE, 32768), tExe(prof.fileKind == CMK_EXE ? EXE_SIZE : 1, 32768),
                tText(prof.fileKind == CMK_TEXT ? TEXT_SIZE : 1, 32768),
                tBmp(prof.fileKind == CMK_HAL ? (3 * 16 * 16 * 512) : 1, 32768),
                tYuuki(prof.fileKind == CMK_YUUKI ? (256 * 2 * 2 * 2 * 512) : 1, 32768),
                tWav(prof.fileKind == CMK_WAV ? (4 * 256 * 16 * 512) : 1, 32768),
                matchTab(SM, 0), matchTab2(SM, 0), matchTab3(SM, 0), w(8192 * NIN, 1 << 14), w2(WN * NIN, 1 << 14), w3(WN * NIN, 1 << 14), w4(WN * NIN, 1 << 14), w5(prof.fileKind == CMK_EXE ? WN * NIN : 1, 1 << 14), wf(64 * NMIX, 16384),
                apm(32768 * 65), apm2(static_cast<size_t>(APM2N) * 65), apm3(32768 * 65), apm4(2097152 * 65) {
        rate = prof.rate; mixShift = prof.mixShift; apmShift = prof.apmShift; subShift = prof.subShift; strideLen = prof.strideLen;
        useW5 = prof.fileKind == CMK_EXE;
        applyPrior = prof.applyPrior;
        isYuuki = prof.fileKind == CMK_YUUKI;
        isExe = prof.fileKind == CMK_EXE;
        isBmp = prof.fileKind == CMK_HAL;
        // isWav は CMK_WAV のみ (YUUKI の旧互換 true は実効なしのため 2026-07-03 に整理)。
        isWav = prof.fileKind == CMK_WAV;
        isText = prof.fileKind == CMK_TEXT;
        uint16_t initv[65];
        for (int j = 0; j < 65; ++j) initv[j] = static_cast<uint16_t>(CM_squash((j - 32) * 64) * 16);
        for (int i = 0; i < 32768; ++i)
            for (int j = 0; j < 65; ++j) apm[i * 65 + j] = initv[j];
        for (int i = 0; i < APM2N; ++i)
            for (int j = 0; j < 65; ++j) apm2[i * 65 + j] = initv[j];
        for (int i = 0; i < 2097152; ++i)
            for (int j = 0; j < 65; ++j) apm4[i * 65 + j] = initv[j];
        for (int i = 0; i < 16384; ++i)
            for (int j = 0; j < 65; ++j)
                apm3[i * 65 + j] = initv[j];
    }

    // TeraPad.exe の PEセクション境界 (決め打ち。BCJ は長さ保存なので BCJ 後も同一オフセット)。
    // .reloc/.rsrc(計328KB) は命令列と統計が全く違うため order-0 を領域別に分離する。
    // BMPヘッダの動的パース (インデックスBMP汎用化)。エンコード・デコードとも復元済みの buf
    // から同じ手順で読むため対称性が保証される。bfOffBits(10,4LE)=データ開始、biWidth(18,4LE)、
    // biHeight(22,4LE,符号あり: 負=top-down)、biBitCount(28,2LE)。
    // 行ストライド = ((width*bitCount+31)/32)*4 (4バイト境界パディング込み)。
    static bool ParseBmpHeaderForCM(const std::vector<uint8_t>& b,
                                    uint32_t& off, uint32_t& stride, uint32_t& end) {
        if (b.size() < 30) return false;
        if (b[0] != 'B' || b[1] != 'M') return false;
        uint32_t bfOffBits = GetU32(b.data() + 10);
        int32_t w = static_cast<int32_t>(GetU32(b.data() + 18));
        int32_t h = static_cast<int32_t>(GetU32(b.data() + 22));
        int bitCount = b[28] | (b[29] << 8);
        if (w <= 0 || h == 0 || bitCount != 8) return false;   // 8bit インデックスカラーのみ対象
        uint32_t hh = h < 0 ? static_cast<uint32_t>(-h) : static_cast<uint32_t>(h);
        uint32_t st = ((static_cast<uint32_t>(w) * static_cast<uint32_t>(bitCount) + 31u) / 32u) * 4u;
        if (st == 0) return false;
        off = bfOffBits; stride = st; end = bfOffBits + st * hh;
        return true;
    }
    // PE 実行ファイル一般のセクション境界認識 (動的パース)。エンコード・デコードとも
    // 復元済みの buf から同じ手順で読むため対称性が保証される。BCJ は E8/E9 直後の
    // rel32 のみを書き換えるため、パースは BCJ 変換後のストリームに対して行い両者で一致する。
    // 領域分類 (PE 一般知識): 0=ヘッダ, 1=コード (IMAGE_SCN_CNT_CODE),
    // 2=データ (既定), 3=.reloc, 4=.rsrc (いずれも標準セクション名)。
    struct PeSec { uint32_t s, e; uint8_t r; };
    PeSec peSec[32];
    int peSecN = 0;
    uint32_t peHdrEnd = 0;
    bool peParsed = false;
    void tryParsePeHeader() {
        const std::vector<uint8_t>& b = buf;
        if (b.size() < 0x40 || b[0] != 'M' || b[1] != 'Z') { peParsed = (b.size() >= 2 && !(b[0] == 'M' && b[1] == 'Z')); return; }
        uint32_t pe = GetU32(b.data() + 0x3C);
        if (b.size() < pe + 24) return;                       // まだ足りない (次バイトで再試行)
        if (pe > (1u << 20)) { peParsed = true; return; }     // 異常値は放棄 (領域0のみ)
        if (b[pe] != 'P' || b[pe + 1] != 'E' || b[pe + 2] != 0 || b[pe + 3] != 0) { peParsed = true; return; }
        uint32_t nsec = b[pe + 6] | (b[pe + 7] << 8);
        uint32_t optHdr = b[pe + 20] | (b[pe + 21] << 8);
        uint32_t secTab = pe + 24 + optHdr;
        if (nsec == 0 || nsec > 32) { peParsed = true; return; }
        if (b.size() < secTab + nsec * 40) return;            // セクションテーブル未到達
        uint32_t hdrEnd = 0xFFFFFFFFu;
        int n = 0;
        for (uint32_t i = 0; i < nsec; ++i) {
            uint32_t o = secTab + i * 40;
            uint32_t rsz = GetU32(b.data() + o + 16);
            uint32_t raw = GetU32(b.data() + o + 20);
            uint32_t chr = GetU32(b.data() + o + 36);
            if (rsz == 0) continue;                            // .bss 等 raw なしは飛ばす
            uint8_t r = 2;                                     // 既定: 初期化データ
            if (std::memcmp(b.data() + o, ".reloc\0\0", 8) == 0) r = 3;
            else if (std::memcmp(b.data() + o, ".rsrc\0\0\0", 8) == 0) r = 4;
            else if (chr & 0x20u) r = 1;                       // IMAGE_SCN_CNT_CODE
            peSec[n++] = { raw, raw + rsz, r };
            if (raw < hdrEnd) hdrEnd = raw;
        }
        peSecN = n;
        peHdrEnd = (hdrEnd == 0xFFFFFFFFu) ? 0 : hdrEnd;
        peParsed = true;
    }
    int peRegion(size_t p) const {
        if (!peParsed || p < peHdrEnd) return 0;
        for (int i = 0; i < peSecN; ++i)
            if (p >= peSec[i].s && p < peSec[i].e) return peSec[i].r;
        return 0;                                              // セクション外 (末尾ギャップ等)
    }
    static int bmpResMag(int v) {                  // 残差の符号 + 0/255からの距離を量子化 (0..15)
        if (v == 0) return 0;
        int neg = v >= 128;
        int d = neg ? 256 - v : v;
        int m;
        if (d <= 2) m = d;
        else if (d <= 4) m = 3;
        else if (d <= 8) m = 4;
        else if (d <= 16) m = 5;
        else if (d <= 32) m = 6;
        else if (!neg && d > 96) m = 15;
        else m = 7;
        return neg ? (7 + m) : m;
    }
    int predict() {
        int o0base = 0;
        if (isYuuki) {
            size_t p = buf.size(); int bucket = 0;
            // 列位置を8帯域に分割 (帯域幅 = stride/8。yuuki: 800/8=100 で従来と同値)
            if (idxParsed && p >= idxOff && p < idxEnd && idxStride >= 8) {
                bucket = 1 + static_cast<int>(((p - idxOff) % idxStride) / (idxStride / 8));
                if (bucket > 8) bucket = 8;        // stride が8で割り切れない場合の端数ガード
            }
            o0base = bucket * 512;
        } else if (isBmp) o0base = static_cast<int>(buf.size() % 3) * 512;
        else if (isWav && applyPrior) o0base = static_cast<int>(buf.size() % 4) * 512;
        else if (isExe) o0base = peRegion(buf.size()) * 512;
        idx[0] = o0base + c0; // order0 (BMP/WAV/Yuuki/exeは位相・領域別)
        idx[1] = static_cast<int>((cx[1] & 0xFF) * 512 + c0);             // order1
        idx[2] = static_cast<int>(((cx[2] * 0x9E3779B1u) + c0) & TMASK);  // order2
        idx[3] = static_cast<int>(((cx[3] * 0x9E3779B1u) + c0) & TMASK);  // order3
        idx[4] = static_cast<int>(((cx[4] * 0x9E3779B1u) + c0) & TMASK);  // order4
        idx[5] = static_cast<int>(((cx[5] * 0x9E3779B1u) + c0) & TMASK);  // order5
        idx[6] = static_cast<int>(((cx[6] * 0x9E3779B1u) + c0) & TMASK);  // order6
        idx[7] = static_cast<int>(((cx[7] * 0x9E3779B1u) + c0) & TMASK);  // order7
        idx[8] = static_cast<int>(((cx[8] * 0x9E3779B1u) + c0) & TMASK);  // order8
        // 繧ｹ繝代・繧ｹ譁・ц: -s, -2s, -3s 繝舌う繝・(s=strideLen; txt=3 UTF-8謨ｴ蛻・ exe=4 dword謨ｴ蛻・
        {
            size_t p = buf.size();
            int s = strideLen;
            uint32_t sh3 = static_cast<uint32_t>(c0);
            if (p >= static_cast<size_t>(s))     sh3 = sh3 * 0x9E3779B1u + buf[p - s] + 1u;
            if (p >= static_cast<size_t>(2 * s)) sh3 = sh3 * 0x9E3779B1u + buf[p - 2 * s] + 1u;
            if (p >= static_cast<size_t>(3 * s)) sh3 = sh3 * 0x9E3779B1u + buf[p - 3 * s] + 1u;
            idx[9] = static_cast<int>(sh3 & TMASK);
        }
        st[0] = CM_STR.v[t0[idx[0]] >> 4];
        st[1] = CM_STR.v[t1[idx[1]] >> 4];
        st[2] = CM_STR.v[t2[idx[2]] >> 4];
        st[3] = CM_STR.v[t3[idx[3]] >> 4];
        st[4] = CM_STR.v[t4[idx[4]] >> 4];
        st[5] = CM_STR.v[t5[idx[5]] >> 4];
        st[6] = CM_STR.v[t6[idx[6]] >> 4];
        st[7] = CM_STR.v[t7[idx[7]] >> 4];
        st[8] = CM_STR.v[t8[idx[8]] >> 4];
        st[9] = CM_STR.v[t9[idx[9]] >> 4];
        st[10] = 0;                                 // match model
        if (matchPtr > 0 && matchPtr < buf.size()) {
            int predByte = buf[matchPtr];
            int bitsSoFar = c0 - (1 << bitpos);
            int expected = predByte >> (8 - bitpos);
            if (bitsSoFar == expected) {
                int predBit = (predByte >> (7 - bitpos)) & 1;
                int conf = (matchLen < 28 ? matchLen : 28) * 72;
                st[10] = predBit ? conf : -conf;
            }
        }
        st[11] = 0;                                 // 隨ｬ2繝槭ャ繝√Δ繝・Ν (6繝舌う繝医ワ繝・す繝･)
        if (matchPtr2 > 0 && matchPtr2 < buf.size()) {
            int predByte = buf[matchPtr2];
            int bitsSoFar = c0 - (1 << bitpos);
            int expected = predByte >> (8 - bitpos);
            if (bitsSoFar == expected) {
                int predBit = (predByte >> (7 - bitpos)) & 1;
                int conf = (matchLen2 < 28 ? matchLen2 : 28) * 72;
                st[11] = predBit ? conf : -conf;
            }
        }
        st[12] = 0;                                 // 隨ｬ3繝槭ャ繝√Δ繝・Ν (8繝舌う繝医ワ繝・す繝･)
        if (matchPtr3 > 0 && matchPtr3 < buf.size()) {
            int predByte = buf[matchPtr3];
            int bitsSoFar = c0 - (1 << bitpos);
            int expected = predByte >> (8 - bitpos);
            if (bitsSoFar == expected) {
                int predBit = (predByte >> (7 - bitpos)) & 1;
                int conf = (matchLen3 < 28 ? matchLen3 : 28) * 72;
                st[12] = predBit ? conf : -conf;
            }
        }
        // x86 operand model: opcode 縺ｨ immediate/relative operand 蜀・・繝舌う繝井ｽ咲ｽｮ繧貞・譛画枚閼亥喧縲・
        // BCJ蠕後・rel32縺ｯ蜷・ヰ繧､繝井ｽ咲ｽｮ縺ｧ蛻・ｸ・′螟ｧ縺阪￥逡ｰ縺ｪ繧九◆繧√・壼ｸｸ縺ｮbyte-order譁・ц縺ｨ蛻・屬縺吶ｋ縲・
        st[13] = 0;
        exeActive = isExe && exeRemain > 0;
        if (exeActive) {
            uint32_t eh = static_cast<uint32_t>(c0);
            eh = eh * 0x9E3779B1u + static_cast<uint32_t>(exeOpcode + 1);
            eh = eh * 0x9E3779B1u + static_cast<uint32_t>((exeClass << 3) | exeRemain);
            eh = eh * 0x9E3779B1u + static_cast<uint32_t>(buf.size() & 15);
            idx[13] = static_cast<int>(eh & EXE_MASK);
            st[13] = CM_STR.v[tExe[idx[13]] >> 4];
        }
        // Shift-JIS text model: byte-order譁・ц縺ｨ縺ｯ蛻･縺ｫ譁・ｭ怜｢・阜縺ｨ邊励＞譁・ｭ礼ｨｮ繧貞・譛峨☆繧九・
        st[14] = 0;
        if (isText) {
            uint32_t th = static_cast<uint32_t>(c0);
            th = th * 0x9E3779B1u + textClasses;
            th = th * 0x9E3779B1u + static_cast<uint32_t>(textPrevChar + 1);
            th = th * 0x9E3779B1u + static_cast<uint32_t>(sjisTrail ? (0x100 | sjisLead) : 0);
            textIdx = static_cast<int>(th & TEXT_MASK);
            st[14] = CM_STR.v[tText[textIdx] >> 4];
        } else if (isBmp) {                          // BMP残差 予測難易度文脈 (st[14]兼用)
            size_t p = buf.size();
            int phase = static_cast<int>(p % 3);
            // 1行上の同位置の残差bucket (残差開始位置と行ストライドはフィルタ出力ヘッダから
            // 動的取得)。24bit BMP では stride%3==0 で同チャンネル・同x。エッジ/テクスチャの
            // 難易度は縦にも連続する。
            int upMag = (bmpParsed && bmpStride > 0 && p >= bmpResidOff + bmpStride)
                            ? bmpResMag(buf[p - bmpStride]) : 0;
            bmpIdx = ((phase * 16 + prevResMag) * 16 + upMag) * 512 + c0;
            st[14] = CM_STR.v[tBmp[bmpIdx] >> 4];
        } else if (isYuuki) {                        // インデックスBMP 縦order-1 + 面/エッジbit (st[14]兼用)
            // 上の行の同位置 index (行ストライドは BMP ヘッダから動的取得。yuuki では従来の
            // 800B と同値)。left はフル直積だと密度不足 (+1,596) なので「left==up か」の1bitに
            // 量子化して足す (面の内部 vs エッジで up の予測力が大きく変わる)。
            size_t p = buf.size();
            const size_t so = idxOff, ss = idxStride;
            int up = (idxParsed && p >= so + ss) ? buf[p - ss] : 0;
            int flat = (p >= 1 && buf[p - 1] == up) ? 1 : 0;
            int vflat = (idxParsed && p >= so + 2 * ss && buf[p - 2 * ss] == up) ? 1 : 0;  // 縦に同色が続くか
            int dflat = (idxParsed && p >= so + ss - 1 && ss >= 1 && buf[p - (ss - 1)] == up) ? 1 : 0;  // 右上==上
            yuukiIdx = (((up * 2 + flat) * 2 + vflat) * 2 + dflat) * 512 + c0;
            st[14] = CM_STR.v[tYuuki[yuukiIdx] >> 4];
        } else if (isWav) {                          // wav 同位相order-1.5 (st[14]兼用, YUUKIは上で除外)
            // 1サンプル前の同位相バイト (4B周期) + 2サンプル前の上位nibble (符号+主要振幅)。
            // stride文脈 idx[9] は p-4,-8,-12 の3タップ合成ハッシュなので、直積は新情報。
            size_t p = buf.size();
            int prev = (p >= 4) ? buf[p - 4] : 0;
            int prev2hi = (p >= 8) ? (buf[p - 8] >> 4) : 0;  // フルバイトは+10悪化、nibbleが頂点
            int phase = static_cast<int>(p % 4);   // M下位/M上位/S下位/S上位 で prev の意味が違う
            wavIdx = ((phase * 256 + prev) * 16 + prev2hi) * 512 + c0;
            st[14] = CM_STR.v[tWav[wavIdx] >> 4];
        }
        mc = static_cast<int>(cx[1] & 0xFF);
        mc_ext = mc * 8 + bitpos;
        int ms = matchLen == 0 ? 0 : (matchLen < 8 ? 1 : (matchLen < 32 ? 2 : 3));
        ms_apm = matchLen == 0 ? 0 : (matchLen < 4 ? 1 : (matchLen < 8 ? 2 : (matchLen < 16 ? 3 : (matchLen < 32 ? 4 : (matchLen < 64 ? 5 : (matchLen < 128 ? 6 : 7))))));  // 8谿ｵ髫・(APM3逕ｨ)
        ms_apm16 = matchLen == 0 ? 0 : (matchLen < 2 ? 1 : (matchLen < 3 ? 2 : (matchLen < 4 ? 3 : (matchLen < 6 ? 4 : (matchLen < 8 ? 5 : (matchLen < 12 ? 6 : (matchLen < 16 ? 7 : (matchLen < 24 ? 8 : (matchLen < 32 ? 9 : (matchLen < 48 ? 10 : (matchLen < 64 ? 11 : (matchLen < 96 ? 12 : (matchLen < 128 ? 13 : (matchLen < 192 ? 14 : 15))))))))))))));  // 16谿ｵ髫・(APM1逕ｨ)
        mixCtx = mc_ext * 4 + ms;                       // match 蠑ｷ蠎ｦ (2bit) 縺ｧ蛻･驥阪∩髮・粋
        mix2Ctx = static_cast<int>(((cx[2] * 0x9E3779B1u) >> subShift) * 8 + bitpos);  // order-2 譁・ц
        mix3Ctx = static_cast<int>(((cx[3] * 0x9E3779B1u) >> subShift) * 8 + bitpos);  // order-3 譁・ц
        mix4Ctx = static_cast<int>(((cx[4] * 0x9E3779B1u) >> subShift) * 8 + bitpos);  // order-4 譁・ц
        mix5Ctx = useW5 ? static_cast<int>(((cx[5] * 0x9E3779B1u) >> subShift) * 8 + bitpos) : 0;  // order-5 文脈 (exe専用)
        long long dot = 0, dot2 = 0, dot3 = 0, dot4 = 0, dot5 = 0;
        for (int i = 0; i < NIN; ++i) {
            dot  += static_cast<long long>(w [mixCtx  * NIN + i]) * st[i];
            dot2 += static_cast<long long>(w2[mix2Ctx * NIN + i]) * st[i];
            dot3 += static_cast<long long>(w3[mix3Ctx * NIN + i]) * st[i];
            dot4 += static_cast<long long>(w4[mix4Ctx * NIN + i]) * st[i];
        }
        if (useW5) for (int i = 0; i < NIN; ++i) dot5 += static_cast<long long>(w5[mix5Ctx * NIN + i]) * st[i];
        // sub-mixer 蜃ｺ蜉帙ｒ譛邨・mixer 縺・bitpos 豈弱↓蟄ｦ鄙貞粋謌・(2螻､ mixer)
        fmLogit[0] = static_cast<int>(dot >> 16);
        fmLogit[1] = static_cast<int>(dot2 >> 16);
        fmLogit[2] = static_cast<int>(dot3 >> 16);
        fmLogit[3] = static_cast<int>(dot4 >> 16);
        fmLogit[4] = static_cast<int>(dot5 >> 16);
        fmCtx = bitpos * 8 + ms_apm;                    // bitpos + match蠑ｷ蠎ｦ8谿ｵ髫・縺ｧ sub-mixer 驟榊・繧貞､峨∴繧・
        long long dotF = 0;
        for (int k = 0; k < NMIX; ++k) dotF += static_cast<long long>(wf[fmCtx * NMIX + k]) * fmLogit[k];
        pr0 = CM_squash(static_cast<int>(dotF >> 16));
        if (pr0 < 1) pr0 = 1; else if (pr0 > 4094) pr0 = 4094;
        // APM1: mixer 蜃ｺ蜉帙ｒ譁・ц (逶ｴ蜑阪ヰ繧､繝・8+繝薙ャ繝井ｽ咲ｽｮ+match蠑ｷ蠎ｦ8谿ｵ髫・ 縺ｧ陬懈ｭ｣ (65轤ｹ陬憺俣)
        int s = CM_STR.v[pr0] + 2048;               // 0..4095
        int wt = s & 63, j = s >> 6;
        apmIdx = (mc_ext * 16 + ms_apm16) * 65 + j;  // 32768譁・ц (mc_ext=2048, ms_apm16=16)
        int ap = (apm[apmIdx] * (64 - wt) + apm[apmIdx + 1] * wt) >> 10;    // 12bit
        prf = (pr0 + 3 * ap) >> 2;
        if (prf < 1) prf = 1; else if (prf > 4094) prf = 4094;
        // APM2: prf 繧・2谺｡譁・ц (cx[2]縺ｮ繝上ャ繧ｷ繝･荳贋ｽ・bit+bitpos) 縺ｧ縺輔ｉ縺ｫ陬懈ｭ｣ (65轤ｹ陬憺俣)
        apm2Ctx = static_cast<int>(((cx[2] * 0x9E3779B1u) >> APM2SHIFT) * 8 + bitpos);  // プロファイル依存文脈数
        int s2 = CM_STR.v[prf] + 2048;
        apm2Wt = s2 & 63; int j2 = s2 >> 6;
        apm2Idx = apm2Ctx * 65 + j2;
        int ap2 = (apm2[apm2Idx] * (64 - apm2Wt) + apm2[apm2Idx + 1] * apm2Wt) >> 10;
        prf = (prf + ap2) >> 1;
        if (prf < 1) prf = 1; else if (prf > 4094) prf = 4094;
        // APM3: prf 繧・c0 (繝舌う繝亥・驛ｨ蛻・ン繝・ヨ蛻・ 縺ｧ縺輔ｉ縺ｫ陬懈ｭ｣ (65轤ｹ陬憺俣)
        {
            int s3 = CM_STR.v[prf] + 2048;
            apm3Wt = s3 & 63; int j3 = s3 >> 6;
            apm3Idx = (((mc >> 5) & 7) * 2048 + c0 * 8 + ms_apm) * 65 + j3;  // prev3bits*2048+c0*8+ms_apm 竊・16384譁・ц
            int ap3 = (apm3[apm3Idx] * (64 - apm3Wt) + apm3[apm3Idx + 1] * apm3Wt) >> 10;
            prf = (prf + ap3) >> 1;
            if (prf < 1) prf = 1; else if (prf > 4094) prf = 4094;
        }
        // APM4: prf 繧・cx[4]繝上ャ繧ｷ繝･荳贋ｽ・5bit+match譛臥┌+bitpos 縺ｧ縺輔ｉ縺ｫ陬懈ｭ｣ (65轤ｹ陬憺俣)
        apm4Ctx = static_cast<int>(((cx[4] * 0x9E3779B1u) >> 15) * 16 + (matchLen > 0 ? 8 : 0) + bitpos);  // 2M文脈 (実験: >>17 から拡大)
        {
            int s4 = CM_STR.v[prf] + 2048;
            apm4Wt = s4 & 63; int j4 = s4 >> 6;
            apm4Idx = apm4Ctx * 65 + j4;
            int ap4 = (apm4[apm4Idx] * (64 - apm4Wt) + apm4[apm4Idx + 1] * apm4Wt) >> 10;
            prf = (prf + ap4) >> 1;
            if (prf < 1) prf = 1; else if (prf > 4094) prf = 4094;
        }
        return prf;
    }
    void update(int bit) {
        int err = (bit << 12) - pr0;                // 荳｡ mixer 縺ｯ譛邨ょ・蜉幄ｪ､蟾ｮ縺ｧ蟄ｦ鄙・
        for (int i = 0; i < NIN; ++i) {
            int& wi = w[mixCtx * NIN + i];
            wi += (st[i] * err) >> mixShift;
            if (wi < -(1 << 20)) wi = -(1 << 20); else if (wi > (1 << 20)) wi = (1 << 20);
            int& wi2 = w2[mix2Ctx * NIN + i];
            wi2 += (st[i] * err) >> mixShift;
            if (wi2 < -(1 << 20)) wi2 = -(1 << 20); else if (wi2 > (1 << 20)) wi2 = (1 << 20);
            int& wi3 = w3[mix3Ctx * NIN + i];
            wi3 += (st[i] * err) >> mixShift;
            if (wi3 < -(1 << 20)) wi3 = -(1 << 20); else if (wi3 > (1 << 20)) wi3 = (1 << 20);
            int& wi4 = w4[mix4Ctx * NIN + i];
            wi4 += (st[i] * err) >> mixShift;
            if (wi4 < -(1 << 20)) wi4 = -(1 << 20); else if (wi4 > (1 << 20)) wi4 = (1 << 20);
        }
        if (useW5) for (int i = 0; i < NIN; ++i) {
            int& wi5 = w5[mix5Ctx * NIN + i];
            wi5 += (st[i] * err) >> mixShift;
            if (wi5 < -(1 << 20)) wi5 = -(1 << 20); else if (wi5 > (1 << 20)) wi5 = (1 << 20);
        }
        for (int k = 0; k < NMIX; ++k) {            // 譛邨・mixer 譖ｴ譁ｰ
            int& wfk = wf[fmCtx * NMIX + k];
            wfk += (fmLogit[k] * err) >> 14;
            if (wfk < -(1 << 18)) wfk = -(1 << 18); else if (wfk > (1 << 18)) wfk = (1 << 18);
        }
        int g = bit << 16;                          // APM1 譖ｴ譁ｰ
        apm[apmIdx]     = static_cast<uint16_t>(apm[apmIdx]     + ((g - apm[apmIdx])     >> apmShift));
        apm[apmIdx + 1] = static_cast<uint16_t>(apm[apmIdx + 1] + ((g - apm[apmIdx + 1]) >> apmShift));
        // APM2 譖ｴ譁ｰ
        apm2[apm2Idx]     = static_cast<uint16_t>(apm2[apm2Idx]     + ((g - apm2[apm2Idx])     >> apmShift));
        apm2[apm2Idx + 1] = static_cast<uint16_t>(apm2[apm2Idx + 1] + ((g - apm2[apm2Idx + 1]) >> apmShift));
        // APM3 譖ｴ譁ｰ
        apm3[apm3Idx]     = static_cast<uint16_t>(apm3[apm3Idx]     + ((g - apm3[apm3Idx])     >> apmShift));
        apm3[apm3Idx + 1] = static_cast<uint16_t>(apm3[apm3Idx + 1] + ((g - apm3[apm3Idx + 1]) >> apmShift));
        // APM4 譖ｴ譁ｰ
        apm4[apm4Idx]     = static_cast<uint16_t>(apm4[apm4Idx]     + ((g - apm4[apm4Idx])     >> apmShift));
        apm4[apm4Idx + 1] = static_cast<uint16_t>(apm4[apm4Idx + 1] + ((g - apm4[apm4Idx + 1]) >> apmShift));
        int tgt = bit << 12;
        auto upd  = [&](std::vector<uint16_t>& t, int ix) {
            uint16_t e = t[ix];
            int n = e & 15, pr = e >> 4;
            pr += (((tgt - pr) * rate[n]) >> 16);
            if (pr < 0) pr = 0; else if (pr > 4095) pr = 4095;
            if (n < 15) ++n;
            t[ix] = static_cast<uint16_t>((pr << 4) | n);
        };
        // 専用テーブルを個別の rate で更新する版 (tYuuki は WAV 床4096 より深い学習が合う)
        auto updR = [&](std::vector<uint16_t>& t, int ix, const int* rt) {
            uint16_t e = t[ix];
            int n = e & 15, pr = e >> 4;
            pr += (((tgt - pr) * rt[n]) >> 16);
            if (pr < 0) pr = 0; else if (pr > 4095) pr = 4095;
            if (n < 15) ++n;
            t[ix] = static_cast<uint16_t>((pr << 4) | n);
        };
        upd(t0, idx[0]); upd(t1, idx[1]); upd(t2, idx[2]); upd(t3, idx[3]);
        upd(t4, idx[4]); upd(t5, idx[5]); upd(t6, idx[6]); upd(t7, idx[7]); upd(t8, idx[8]); upd(t9, idx[9]);
        if (exeActive) upd(tExe, idx[13]);
        if (isText) upd(tText, textIdx);
        else if (isBmp) upd(tBmp, bmpIdx);
        else if (isYuuki) updR(tYuuki, yuukiIdx, CM_RATE_YUUKI_T);
        else if (isWav) upd(tWav, wavIdx);
        c0 = (c0 << 1) | bit; ++bitpos;
        if (bitpos == 8) {
            int B = c0 & 0xFF;
            buf.push_back(static_cast<uint8_t>(B));
            // インデックスBMPのヘッダを動的パース (30バイト溜まった時点で確定)。
            // yuuki_256.bmp では off=1074/stride=800/end=641074 が得られ、従来の決め打ちと同値。
            if (isYuuki && !idxParsed)
                idxParsed = ParseBmpHeaderForCM(buf, idxOff, idxStride, idxEnd);
            // PE ヘッダの動的パース (exe 領域認識用。セクションテーブル到達まで再試行)
            if (isExe && !peParsed) tryParsePeHeader();
            // BMP残差フィルタ出力レイアウトのパース:
            // [4B hdrLen][hdr][4B bpp][4B stride][4B rows][4B width][4B trailerLen][trailer][ftypes rows][resid]
            if (isBmp && !bmpParsed && buf.size() >= 24) {
                uint32_t hdrLen = GetU32(buf.data());
                size_t meta = 4 + static_cast<size_t>(hdrLen);
                if (buf.size() >= meta + 20) {
                    uint32_t stride = GetU32(buf.data() + meta + 4);
                    uint32_t rows = GetU32(buf.data() + meta + 8);
                    uint32_t trailerLen = GetU32(buf.data() + meta + 16);
                    bmpStride = stride;
                    bmpResidOff = static_cast<uint32_t>(meta + 20 + trailerLen + rows);
                    bmpParsed = true;                          // rows==0 (フォールバック) でも確定
                }
            }
            if (isExe) {
                if (exeRemain > 0) {
                    --exeRemain;
                    if (exeRemain == 0) { exeClass = 0; exeOpcode = 0; }
                } else if (exePrefix0F) {                                // 直前が 0x0F の 2バイトopcode
                    exePrefix0F = false;
                    if (B >= 0x80 && B <= 0x8F) { exeClass = 7; exeOpcode = B; exeRemain = 4; } // Jcc rel32 (相対分岐, BCJ非対象)
                } else if (B == 0x0F) {
                    exePrefix0F = true;                                  // 2バイトopcode prefix (次バイトで判定)
                } else if (B == 0xE8 || B == 0xE9) {
                    exeClass = 1; exeOpcode = B; exeRemain = 4;          // CALL/JMP rel32 (BCJ蟇ｾ雎｡)
                } else if (B >= 0xB8 && B <= 0xBF) {
                    exeClass = 2; exeOpcode = B; exeRemain = 4;          // MOV reg, imm32
                } else if (B == 0x68) {
                    exeClass = 3; exeOpcode = B; exeRemain = 4;          // PUSH imm32
                } else if ((B & 0xC7) == 0x05 && B < 0x40) {
                    exeClass = 4; exeOpcode = B; exeRemain = 4;          // ALU EAX, imm32 (05/0D/.../3D)
                } else if (B >= 0xA0 && B <= 0xA3) {
                    exeClass = 5; exeOpcode = B; exeRemain = 4;          // MOV AL/EAX <-> moffs32 (絶対アドレス)
                } else if (B == 0xA9) {
                    exeClass = 6; exeOpcode = B; exeRemain = 4;          // TEST EAX, imm32
                } else if (B >= 0x70 && B <= 0x7F) {
                    exeClass = 8; exeOpcode = B; exeRemain = 1;          // Jcc rel8 (短い条件分岐)
                } else if (B == 0xEB || (B >= 0xE0 && B <= 0xE3)) {
                    exeClass = 9; exeOpcode = B; exeRemain = 1;          // JMP/LOOP/JECXZ rel8
                }
                // ※ ModRM経由の imm32 (0x81/0xC7/0x69) は誤検出時の可変長スキップで単純opcode検出を
                //    乱し +32B 悪化したため不採用 (iter8 で検証)。単純な単バイトopcode+imm32 に限定する。
            }
            if (isText) {
                auto isLead = [](int x) { return (x >= 0x81 && x <= 0x9F) || (x >= 0xE0 && x <= 0xFC); };
                int cls = 0;
                if (sjisTrail) {
                    uint16_t ch = static_cast<uint16_t>((sjisLead << 8) | B);
                    if (sjisLead == 0x82 && B >= 0x9F && B <= 0xF1) cls = 6;      // ひらがな
                    else if (sjisLead == 0x83) cls = 7;                           // カタカナ
                    else if (sjisLead == 0x81 && (B == 0x41 || B == 0x42)) cls = 10; // 句読点「、」「。」(文境界マーカー)
                    else if (sjisLead == 0x81 && (B == 0x75 || B == 0x76)) cls = 11; // 鍵括弧「」(会話境界マーカー)
                    else if (sjisLead == 0x81) cls = 8;                           // 全角記号
                    else cls = 9;                                                 // 漢字ほか
                    textPrevChar = ch;
                    sjisTrail = false; sjisLead = 0;
                } else if (isLead(B)) {
                    sjisLead = B; sjisTrail = true;
                } else {
                    if (B == '\r' || B == '\n') cls = 1;
                    else if (B == ' ' || B == '\t') cls = 2;
                    else if (B >= '0' && B <= '9') cls = 3;
                    else if ((B >= 'A' && B <= 'Z') || (B >= 'a' && B <= 'z')) cls = 4;
                    else cls = 5;
                    textPrevChar = static_cast<uint16_t>(B);
                }
                if (cls != 0) textClasses = ((textClasses << 4) | static_cast<uint32_t>(cls)) & 0xFFFFFFu;
            }
            if (isBmp) prevResMag = bmpResMag(B);   // BMP残差の大きさbucketを更新 (次バイトの文脈)
            if (matchPtr > 0 && matchPtr < buf.size() - 1 && buf[matchPtr] == B) { ++matchPtr; ++matchLen; }
            else { matchPtr = 0; matchLen = 0; }
            if (matchPtr2 > 0 && matchPtr2 < buf.size() - 1 && buf[matchPtr2] == B) { ++matchPtr2; ++matchLen2; }
            else { matchPtr2 = 0; matchLen2 = 0; }
            if (matchPtr3 > 0 && matchPtr3 < buf.size() - 1 && buf[matchPtr3] == B) { ++matchPtr3; ++matchLen3; }
            else { matchPtr3 = 0; matchLen3 = 0; }
            size_t p = buf.size();
            uint32_t hsh = 0;                        // cx[k] = 逶ｴ霑・k 繝舌う繝医・邏ｯ遨阪ワ繝・す繝･
            for (int k = 1; k <= 8; ++k) { if (p >= static_cast<size_t>(k)) hsh = hsh * 0x9E3779B1u + buf[p - k] + 1u; cx[k] = hsh; }
            if (p >= 4) {
                uint32_t hh = (static_cast<uint32_t>(buf[p - 1]) | (static_cast<uint32_t>(buf[p - 2]) << 8)
                             | (static_cast<uint32_t>(buf[p - 3]) << 16) | (static_cast<uint32_t>(buf[p - 4]) << 24));
                hh = (hh * 2654435761u) & (SM - 1);
                if (matchPtr == 0) { uint32_t cand = matchTab[hh]; if (cand > 0 && cand < p) { matchPtr = cand; matchLen = 1; } }
                matchTab[hh] = static_cast<uint32_t>(p);
            }
            if (p >= 6) {                            // 隨ｬ2繝槭ャ繝・ 逶ｴ霑・繝舌う繝医ワ繝・す繝･
                uint32_t h2 = 0;
                for (int k = 1; k <= 6; ++k) h2 = h2 * 0x9E3779B1u + buf[p - k] + 1u;
                h2 = (h2 * 2654435761u) & (SM - 1);
                if (matchPtr2 == 0) { uint32_t cand = matchTab2[h2]; if (cand > 0 && cand < p) { matchPtr2 = cand; matchLen2 = 1; } }
                matchTab2[h2] = static_cast<uint32_t>(p);
            }
            if (p >= 8) {                            // 隨ｬ3繝槭ャ繝・ 逶ｴ霑・繝舌う繝医ワ繝・す繝･
                uint32_t h3 = 0;
                for (int k = 1; k <= 8; ++k) h3 = h3 * 0x9E3779B1u + buf[p - k] + 1u;
                h3 = (h3 * 2654435761u) & (SM - 1);
                if (matchPtr3 == 0) { uint32_t cand = matchTab3[h3]; if (cand > 0 && cand < p) { matchPtr3 = cand; matchLen3 = 1; } }
                matchTab3[h3] = static_cast<uint32_t>(p);
            }
            c0 = 1; bitpos = 0;
        }
    }
};

std::vector<uint8_t> Encode_CM(const std::vector<uint8_t>& input, const CMProfile& prof) {
    std::vector<uint8_t> out;
    PutU64(out, static_cast<uint64_t>(input.size()));
    if (input.empty()) return out;
    CMModel cm(prof);
    BinaryRangeEncoder enc(out);
    for (uint8_t B : input) {
        for (int k = 7; k >= 0; --k) {
            int bit = (B >> k) & 1;
            int p = cm.predict();
            enc.encode(bit, p);
            cm.update(bit);
        }
    }
    enc.flush();
    return out;
}
std::vector<uint8_t> Decode_CM(const std::vector<uint8_t>& input, const CMProfile& prof) {
    if (input.size() < 8) return {};
    uint64_t n = GetU64(input.data());
    std::vector<uint8_t> out;
    if (n == 0) return out;
    out.reserve(static_cast<size_t>(n));
    CMModel cm(prof);
    BinaryRangeDecoder dec(input.data() + 8, input.size() - 8);
    for (uint64_t i = 0; i < n; ++i) {
        int B = 0;
        for (int k = 0; k < 8; ++k) {
            int p = cm.predict();
            int bit = dec.decode(p);
            cm.update(bit);
            B = (B << 1) | bit;
        }
        out.push_back(static_cast<uint8_t>(B));
    }
    return out;
}
