#include "compress.h"

// ==========================================================================
// コンテキストミキシング (CM) — 二値算術符号化 + 複数文脈モデル + ロジスティック混合
//   lpaq 系の軽量版。モデル: order-1/2/3 文脈 + マッチモデル。各モデルの予測を
//   stretch 領域で重み付き加算 (mixer) し squash で最終確率に。1 ビットごとに
//   実ビットで各重みとモデル確率を更新。エンコード/デコードは同一モデルを駆動し
//   符号器だけが異なるので完全可逆。出力: [u64 size][二値算術ストリーム]。
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

// 適応カウンタの学習レート (16.16 固定小数, 1/(n+α)相当)。ファイル種別ごとにプロファイルを選ぶ:
//   SLOW = 低い床(~1/16): 定常的なテキスト/画像向け (CM, BMP_CM)。
//   FAST = 高い床(~1/5):  非定常な exe/音声残差向け (BCJ_CM, WAV_CM)。
// algo バイトはアーカイブに保存され復号も同じ algo を見るため、プロファイル選択は完全可逆。

// CM 予測モデル (encode/decode 共通)
//   文脈モデル: order 0,1,2,3,4,5,6 + マッチ = 8 入力。mixer + APM(二次推定)。
//   各テーブル要素 uint16 = (prob<<4)|count : prob は 12bit, count(0..15) で学習率を制御。
struct CMModel {
    static const int NIN = 13;                     // o0..o8,stride3,match,match2(6B),match3(8B)
    const int TBITS, TSIZE, TMASK;                  // t2..t9 のサイズ (プロファイル依存)
    static const int SM = 1 << 24;
    std::vector<uint16_t> t0, t1, t2, t3, t4, t5, t6, t7, t8, t9;  // ビット確率 (12bit, 初期 2048)
    std::vector<uint32_t> matchTab, matchTab2, matchTab3;
    std::vector<uint8_t> buf;
    std::vector<int> w;                            // mixer 重み (mixCtx 文脈 x NIN)
    std::vector<int> w2;                           // 第2 mixer 重み (order-2 文脈 x NIN)
    std::vector<int> w3;                           // 第3 mixer 重み (order-3 文脈 x NIN)
    std::vector<int> w4;                           // 第4 mixer 重み (order-4 文脈 x NIN)
    std::vector<int> wf;                           // 最終 mixer (sub-mixer をbitpos毎に学習合成)
    static const int NMIX = 4;
    int mix2Ctx = 0, mix3Ctx = 0, mix4Ctx = 0, fmCtx = 0, fmLogit[NMIX] = {0,0,0,0};
    std::vector<uint16_t> apm;                     // 一次推定 (8192 文脈 x 65 点、match強度付き)
    std::vector<uint16_t> apm2;                    // 二次推定 (2048 文脈 x 65 点、bitpos付き)
    std::vector<uint16_t> apm3;                    // 三次推定 (1024 文脈 x 65 点、c0×match強度)
    std::vector<uint16_t> apm4;                    // 四次推定 (2048 文脈 x 65 点、cx[3]ハッシュ+bitpos)
    uint32_t matchPtr = 0; int matchLen = 0;
    uint32_t matchPtr2 = 0; int matchLen2 = 0;     // 第2マッチモデル (6バイトハッシュ)
    uint32_t matchPtr3 = 0; int matchLen3 = 0;     // 第3マッチモデル (8バイトハッシュ)
    uint32_t cx[9] = {0,0,0,0,0,0,0,0,0};           // cx[k] = 直近 k バイトのハッシュ
    int c0 = 1, bitpos = 0, mc = 0, mc_ext = 0;    // mc_ext = mc*8+bitpos (APM1用)
    int mixCtx = 0;                                // ミキサー文脈 = mc_ext*2 + match-active
    int ms_apm = 0;                                // 8段階 match strength (APM3専用)
    int ms_apm16 = 0;                              // 16段階 match strength (APM1専用)
    int st[NIN], idx[NIN], pr0 = 2048, prf = 2048, apmIdx = 0;
    int apm2Ctx = 0, apm2Idx = 0, apm2Wt = 0;
    int apm3Idx = 0, apm3Wt = 0;
    int apm4Ctx = 0, apm4Idx = 0, apm4Wt = 0;
    const int* rate = CM_RATE_SLOW;                // 適応カウンタ学習レートのプロファイル
    int mixShift = 12;                             // ミキサー学習レート (プロファイル依存)
    int apmShift = 7;                              // APM 更新レート (プロファイル依存)
    int subShift = 24;                             // sub-mixer 文脈の細かさ (プロファイル依存)
    int strideLen = 3;                             // スパース文脈の刻み (プロファイル依存)

    CMModel(const CMProfile& prof)
              : TBITS(prof.tbits), TSIZE(1 << prof.tbits), TMASK((1 << prof.tbits) - 1),
                t0(512, 32768), t1(256 * 512, 32768), t2(TSIZE, 32768), t3(TSIZE, 32768),
                t4(TSIZE, 32768), t5(TSIZE, 32768), t6(TSIZE, 32768), t7(TSIZE, 32768),
                t8(TSIZE, 32768), t9(TSIZE, 32768), matchTab(SM, 0), matchTab2(SM, 0), matchTab3(SM, 0), w(8192 * NIN, 1 << 14), w2(2097152 * NIN, 1 << 14), w3(2097152 * NIN, 1 << 14), w4(2097152 * NIN, 1 << 14), wf(64 * NMIX, 16384),
                apm(32768 * 65), apm2(4096 * 65), apm3(32768 * 65), apm4(524288 * 65) {
        rate = prof.rate; mixShift = prof.mixShift; apmShift = prof.apmShift; subShift = prof.subShift; strideLen = prof.strideLen;
        uint16_t initv[65];
        for (int j = 0; j < 65; ++j) initv[j] = static_cast<uint16_t>(CM_squash((j - 32) * 64) * 16);
        for (int i = 0; i < 32768; ++i)
            for (int j = 0; j < 65; ++j) apm[i * 65 + j] = initv[j];
        for (int i = 0; i < 4096; ++i)
            for (int j = 0; j < 65; ++j) apm2[i * 65 + j] = initv[j];
        for (int i = 0; i < 524288; ++i)
            for (int j = 0; j < 65; ++j) apm4[i * 65 + j] = initv[j];
        for (int i = 0; i < 16384; ++i)
            for (int j = 0; j < 65; ++j)
                apm3[i * 65 + j] = initv[j];
    }

    int predict() {
        idx[0] = c0;                                                       // order0
        idx[1] = static_cast<int>((cx[1] & 0xFF) * 512 + c0);             // order1
        idx[2] = static_cast<int>(((cx[2] * 0x9E3779B1u) + c0) & TMASK);  // order2
        idx[3] = static_cast<int>(((cx[3] * 0x9E3779B1u) + c0) & TMASK);  // order3
        idx[4] = static_cast<int>(((cx[4] * 0x9E3779B1u) + c0) & TMASK);  // order4
        idx[5] = static_cast<int>(((cx[5] * 0x9E3779B1u) + c0) & TMASK);  // order5
        idx[6] = static_cast<int>(((cx[6] * 0x9E3779B1u) + c0) & TMASK);  // order6
        idx[7] = static_cast<int>(((cx[7] * 0x9E3779B1u) + c0) & TMASK);  // order7
        idx[8] = static_cast<int>(((cx[8] * 0x9E3779B1u) + c0) & TMASK);  // order8
        // スパース文脈: -s, -2s, -3s バイト (s=strideLen; txt=3 UTF-8整列, exe=4 dword整列)
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
        st[11] = 0;                                 // 第2マッチモデル (6バイトハッシュ)
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
        st[12] = 0;                                 // 第3マッチモデル (8バイトハッシュ)
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
        mc = static_cast<int>(cx[1] & 0xFF);
        mc_ext = mc * 8 + bitpos;
        int ms = matchLen == 0 ? 0 : (matchLen < 8 ? 1 : (matchLen < 32 ? 2 : 3));
        ms_apm = matchLen == 0 ? 0 : (matchLen < 4 ? 1 : (matchLen < 8 ? 2 : (matchLen < 16 ? 3 : (matchLen < 32 ? 4 : (matchLen < 64 ? 5 : (matchLen < 128 ? 6 : 7))))));  // 8段階 (APM3用)
        ms_apm16 = matchLen == 0 ? 0 : (matchLen < 2 ? 1 : (matchLen < 3 ? 2 : (matchLen < 4 ? 3 : (matchLen < 6 ? 4 : (matchLen < 8 ? 5 : (matchLen < 12 ? 6 : (matchLen < 16 ? 7 : (matchLen < 24 ? 8 : (matchLen < 32 ? 9 : (matchLen < 48 ? 10 : (matchLen < 64 ? 11 : (matchLen < 96 ? 12 : (matchLen < 128 ? 13 : (matchLen < 192 ? 14 : 15))))))))))))));  // 16段階 (APM1用)
        mixCtx = mc_ext * 4 + ms;                       // match 強度 (2bit) で別重み集合
        mix2Ctx = static_cast<int>(((cx[2] * 0x9E3779B1u) >> subShift) * 8 + bitpos);  // order-2 文脈
        mix3Ctx = static_cast<int>(((cx[3] * 0x9E3779B1u) >> subShift) * 8 + bitpos);  // order-3 文脈
        mix4Ctx = static_cast<int>(((cx[4] * 0x9E3779B1u) >> subShift) * 8 + bitpos);  // order-4 文脈
        long long dot = 0, dot2 = 0, dot3 = 0, dot4 = 0;
        for (int i = 0; i < NIN; ++i) {
            dot  += static_cast<long long>(w [mixCtx  * NIN + i]) * st[i];
            dot2 += static_cast<long long>(w2[mix2Ctx * NIN + i]) * st[i];
            dot3 += static_cast<long long>(w3[mix3Ctx * NIN + i]) * st[i];
            dot4 += static_cast<long long>(w4[mix4Ctx * NIN + i]) * st[i];
        }
        // sub-mixer 出力を最終 mixer が bitpos 毎に学習合成 (2層 mixer)
        fmLogit[0] = static_cast<int>(dot >> 16);
        fmLogit[1] = static_cast<int>(dot2 >> 16);
        fmLogit[2] = static_cast<int>(dot3 >> 16);
        fmLogit[3] = static_cast<int>(dot4 >> 16);
        fmCtx = bitpos * 8 + ms_apm;                    // bitpos + match強度8段階 で sub-mixer 配分を変える
        long long dotF = 0;
        for (int k = 0; k < NMIX; ++k) dotF += static_cast<long long>(wf[fmCtx * NMIX + k]) * fmLogit[k];
        pr0 = CM_squash(static_cast<int>(dotF >> 16));
        if (pr0 < 1) pr0 = 1; else if (pr0 > 4094) pr0 = 4094;
        // APM1: mixer 出力を文脈 (直前バイト*8+ビット位置+match強度8段階) で補正 (65点補間)
        int s = CM_STR.v[pr0] + 2048;               // 0..4095
        int wt = s & 63, j = s >> 6;
        apmIdx = (mc_ext * 16 + ms_apm16) * 65 + j;  // 32768文脈 (mc_ext=2048, ms_apm16=16)
        int ap = (apm[apmIdx] * (64 - wt) + apm[apmIdx + 1] * wt) >> 10;    // 12bit
        prf = (pr0 + 3 * ap) >> 2;
        if (prf < 1) prf = 1; else if (prf > 4094) prf = 4094;
        // APM2: prf を 2次文脈 (cx[2]のハッシュ上位9bit+bitpos) でさらに補正 (65点補間)
        apm2Ctx = static_cast<int>(((cx[2] * 0x9E3779B1u) >> 23) * 8 + bitpos);  // 4096文脈 (9bit+3bit)
        int s2 = CM_STR.v[prf] + 2048;
        apm2Wt = s2 & 63; int j2 = s2 >> 6;
        apm2Idx = apm2Ctx * 65 + j2;
        int ap2 = (apm2[apm2Idx] * (64 - apm2Wt) + apm2[apm2Idx + 1] * apm2Wt) >> 10;
        prf = (prf + ap2) >> 1;
        if (prf < 1) prf = 1; else if (prf > 4094) prf = 4094;
        // APM3: prf を c0 (バイト内部分ビット列) でさらに補正 (65点補間)
        {
            int s3 = CM_STR.v[prf] + 2048;
            apm3Wt = s3 & 63; int j3 = s3 >> 6;
            apm3Idx = (((mc >> 5) & 7) * 2048 + c0 * 8 + ms_apm) * 65 + j3;  // prev3bits*2048+c0*8+ms_apm → 16384文脈
            int ap3 = (apm3[apm3Idx] * (64 - apm3Wt) + apm3[apm3Idx + 1] * apm3Wt) >> 10;
            prf = (prf + ap3) >> 1;
            if (prf < 1) prf = 1; else if (prf > 4094) prf = 4094;
        }
        // APM4: prf を cx[4]ハッシュ上位15bit+match有無+bitpos でさらに補正 (65点補間)
        apm4Ctx = static_cast<int>(((cx[4] * 0x9E3779B1u) >> 17) * 16 + (matchLen > 0 ? 8 : 0) + bitpos);  // 524288文脈
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
        int err = (bit << 12) - pr0;                // 両 mixer は最終出力誤差で学習
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
        for (int k = 0; k < NMIX; ++k) {            // 最終 mixer 更新
            int& wfk = wf[fmCtx * NMIX + k];
            wfk += (fmLogit[k] * err) >> 14;
            if (wfk < -(1 << 18)) wfk = -(1 << 18); else if (wfk > (1 << 18)) wfk = (1 << 18);
        }
        int g = bit << 16;                          // APM1 更新
        apm[apmIdx]     = static_cast<uint16_t>(apm[apmIdx]     + ((g - apm[apmIdx])     >> apmShift));
        apm[apmIdx + 1] = static_cast<uint16_t>(apm[apmIdx + 1] + ((g - apm[apmIdx + 1]) >> apmShift));
        // APM2 更新
        apm2[apm2Idx]     = static_cast<uint16_t>(apm2[apm2Idx]     + ((g - apm2[apm2Idx])     >> apmShift));
        apm2[apm2Idx + 1] = static_cast<uint16_t>(apm2[apm2Idx + 1] + ((g - apm2[apm2Idx + 1]) >> apmShift));
        // APM3 更新
        apm3[apm3Idx]     = static_cast<uint16_t>(apm3[apm3Idx]     + ((g - apm3[apm3Idx])     >> apmShift));
        apm3[apm3Idx + 1] = static_cast<uint16_t>(apm3[apm3Idx + 1] + ((g - apm3[apm3Idx + 1]) >> apmShift));
        // APM4 更新
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
        upd(t0, idx[0]); upd(t1, idx[1]); upd(t2, idx[2]); upd(t3, idx[3]);
        upd(t4, idx[4]); upd(t5, idx[5]); upd(t6, idx[6]); upd(t7, idx[7]); upd(t8, idx[8]); upd(t9, idx[9]);
        c0 = (c0 << 1) | bit; ++bitpos;
        if (bitpos == 8) {
            int B = c0 & 0xFF;
            buf.push_back(static_cast<uint8_t>(B));
            if (matchPtr > 0 && matchPtr < buf.size() - 1 && buf[matchPtr] == B) { ++matchPtr; ++matchLen; }
            else { matchPtr = 0; matchLen = 0; }
            if (matchPtr2 > 0 && matchPtr2 < buf.size() - 1 && buf[matchPtr2] == B) { ++matchPtr2; ++matchLen2; }
            else { matchPtr2 = 0; matchLen2 = 0; }
            if (matchPtr3 > 0 && matchPtr3 < buf.size() - 1 && buf[matchPtr3] == B) { ++matchPtr3; ++matchLen3; }
            else { matchPtr3 = 0; matchLen3 = 0; }
            size_t p = buf.size();
            uint32_t hsh = 0;                        // cx[k] = 直近 k バイトの累積ハッシュ
            for (int k = 1; k <= 8; ++k) { if (p >= static_cast<size_t>(k)) hsh = hsh * 0x9E3779B1u + buf[p - k] + 1u; cx[k] = hsh; }
            if (p >= 4) {
                uint32_t hh = (static_cast<uint32_t>(buf[p - 1]) | (static_cast<uint32_t>(buf[p - 2]) << 8)
                             | (static_cast<uint32_t>(buf[p - 3]) << 16) | (static_cast<uint32_t>(buf[p - 4]) << 24));
                hh = (hh * 2654435761u) & (SM - 1);
                if (matchPtr == 0) { uint32_t cand = matchTab[hh]; if (cand > 0 && cand < p) { matchPtr = cand; matchLen = 1; } }
                matchTab[hh] = static_cast<uint32_t>(p);
            }
            if (p >= 6) {                            // 第2マッチ: 直近6バイトハッシュ
                uint32_t h2 = 0;
                for (int k = 1; k <= 6; ++k) h2 = h2 * 0x9E3779B1u + buf[p - k] + 1u;
                h2 = (h2 * 2654435761u) & (SM - 1);
                if (matchPtr2 == 0) { uint32_t cand = matchTab2[h2]; if (cand > 0 && cand < p) { matchPtr2 = cand; matchLen2 = 1; } }
                matchTab2[h2] = static_cast<uint32_t>(p);
            }
            if (p >= 8) {                            // 第3マッチ: 直近8バイトハッシュ
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

