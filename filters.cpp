#include "compress.h"

// ==========================================================================
// デルタフィルタ (stride バイト前との差分)
//
//   stride を変えることで様々なデータ並びに対応する:
//     stride=1: 隣接バイト          (汎用)
//     stride=2: 16bit サンプル/ピクセル
//     stride=3: 24bit RGB ピクセル
//     stride=4: 32bit                (RGBA / 16bit ステレオ)
//   uint8_t の自然なラップアラウンドで完全可逆。
//     encode: out[i] = in[i] - in[i-stride]   (i<stride では in[i-stride]=0)
//     decode: out[i] = in[i] + out[i-stride]
// ==========================================================================
std::vector<uint8_t> Encode_Delta(const std::vector<uint8_t>& in, int stride) {
    const size_t n = in.size();
    std::vector<uint8_t> out(n);
    for (size_t i = 0; i < n; ++i) {
        uint8_t prev = (i >= static_cast<size_t>(stride)) ? in[i - stride] : 0;
        out[i] = static_cast<uint8_t>(in[i] - prev);
    }
    return out;
}
std::vector<uint8_t> Decode_Delta(const std::vector<uint8_t>& in, int stride) {
    const size_t n = in.size();
    std::vector<uint8_t> out(n);
    for (size_t i = 0; i < n; ++i) {
        uint8_t prev = (i >= static_cast<size_t>(stride)) ? out[i - stride] : 0;
        out[i] = static_cast<uint8_t>(in[i] + prev);
    }
    return out;
}

// ==========================================================================
// BCJ (x86 Branch/Call/Jump) フィルタ
//
//   E8 (CALL) / E9 (JMP) 命令直後の 4 バイト相対変位 (little-endian) を
//   「絶対アドレス = 相対 + 次命令位置(i+5)」に変換する。同じ関数を呼ぶ
//   CALL が同一バイト列になり、後段 LZSS の一致が激増する。
//   オペコードバイト自体は書き換えず、両側で同じ走査・スキップを行うため完全可逆。
// ==========================================================================
static inline bool Test86MSByte(uint8_t b) { return b == 0x00 || b == 0xFF; }

// LZMA SDK x86 (BCJ) フィルタの忠実な移植。E8/E9 直後 4 バイト相対変位を
// 絶対アドレスへ (encode) / 相対へ (decode) 変換。上位バイトが 00/FF の
// 「近接分岐らしい」場合のみ変換し、mask 状態機械で連続 E8/E9 を正しく扱う。
// data 内のランダムな E8/E9 を誤変換しないため、素朴版より残差が綺麗になる。
static std::vector<uint8_t> BcjTransform(const std::vector<uint8_t>& in, bool encode) {
    std::vector<uint8_t> data(in);
    const size_t size = data.size();
    if (size < 5) return data;
    const size_t lim = size - 4;
    const uint32_t ip = 5;          // 次命令位置オフセット (元コードの ip+=5 相当)
    uint32_t mask = 0;
    size_t pos = 0;
    for (;;) {
        size_t pp = pos;
        while (pp < lim && (data[pp] & 0xFE) != 0xE8) pp++;
        size_t d = pp - pos;
        pos = pp;
        if (pp >= lim) break;
        if (d > 2) {
            mask = 0;
        } else {
            mask >>= static_cast<unsigned>(d);
            if (mask != 0 && (mask > 4 || mask == 3 ||
                              Test86MSByte(data[pos + (mask >> 1) + 1]))) {
                mask = (mask >> 1) | 4; pos++; continue;
            }
        }
        if (Test86MSByte(data[pos + 4])) {
            size_t ipos = pos;
            uint32_t v = (static_cast<uint32_t>(data[ipos + 4]) << 24)
                       | (static_cast<uint32_t>(data[ipos + 3]) << 16)
                       | (static_cast<uint32_t>(data[ipos + 2]) << 8)
                       |  static_cast<uint32_t>(data[ipos + 1]);
            uint32_t cur = ip + static_cast<uint32_t>(pos);
            pos += 5;
            if (encode) v += cur; else v -= cur;
            if (mask != 0) {
                unsigned sh = (mask & 6) << 2;
                if (Test86MSByte(static_cast<uint8_t>(v >> sh))) {
                    v ^= ((static_cast<uint32_t>(0x100) << sh) - 1);
                    if (encode) v += cur; else v -= cur;
                }
                mask = 0;
            }
            data[ipos + 1] = static_cast<uint8_t>(v);
            data[ipos + 2] = static_cast<uint8_t>(v >> 8);
            data[ipos + 3] = static_cast<uint8_t>(v >> 16);
            data[ipos + 4] = static_cast<uint8_t>(0 - ((v >> 24) & 1));
        } else {
            mask = (mask >> 1) | 4; pos++;
        }
    }
    return data;
}
std::vector<uint8_t> Encode_BCJ(const std::vector<uint8_t>& in) { return BcjTransform(in, true); }
std::vector<uint8_t> Decode_BCJ(const std::vector<uint8_t>& in) { return BcjTransform(in, false); }

// ==========================================================================
// WAV (FLAC 方式: Mid/Side + 16bit Delta + バイトプレーン分離)
//
//   16bit/2ch ステレオ PCM 前提。L/R の相関を Mid/Side で除去し、各チャンネルに
//   16bit 差分をかけ、上位/下位バイトを別プレーンにまとめる。相関の強い音源では
//   Side と上位バイトがほぼゼロになり、後段 LZSS+Huffman が劇的に効く。
//
//   可逆 Mid/Side (リフティング, すべて uint16):
//     enc: side = L - R ;  mid = R + (side>>1)
//     dec: R = mid - (side>>1) ;  L = side + R
//   16bit Delta も uint16 のラップアラウンドで完全可逆。
//
//   出力フォーマット:
//     [u32 headerLen][header(verbatim)]
//     [u32 frames]
//     [u32 trailerLen][trailer(verbatim)]
//     [frames] midLow | [frames] midHigh | [frames] sideLow | [frames] sideHigh
//   WAV として解析できない入力は headerLen=全体, frames=0 のフォールバック(完全可逆)。
// ==========================================================================
// FLAC 固定予測子 (uint16 ラップ)。p1..p4 = 直前〜4 つ前の値。
static inline uint16_t WavPredict(int order, uint16_t p1, uint16_t p2, uint16_t p3, uint16_t p4) {
    switch (order) {
        case 1:  return p1;
        case 2:  return static_cast<uint16_t>(2 * p1 - p2);
        case 3:  return static_cast<uint16_t>(3 * p1 - 3 * p2 + p3);
        case 4:  return static_cast<uint16_t>(4 * p1 - 6 * p2 + 4 * p3 - p4);
        default: return 0;   // order 0 (予測なし)
    }
}

// ---- 真の LPC (線形予測符号化) ----
static const int LPC_ORDER = 8;        // 予測次数 (LMS適応フィルタ追加で 16->8 が最良)
static const uint8_t WAV_METHOD_LPC = 5;   // ブロック method 値 (0-4=固定, 5=LPC)

// 整数 LPC 予測 (連続履歴, uint16 を int16 とみなす, シフトはブロック毎)。enc/dec で同一。
static inline int WavLpcPredict(const uint16_t* v, size_t i, const int16_t* q, int shift) {
    long long sum = 0;
    for (int j = 0; j < LPC_ORDER; ++j) {
        int sv = (i >= static_cast<size_t>(j + 1)) ? static_cast<int>(static_cast<int16_t>(v[i - 1 - j])) : 0;
        sum += static_cast<long long>(q[j]) * sv;
    }
    return static_cast<int>(sum >> shift);     // 算術右シフト
}

// [lo,hi) の窓付き自己相関 -> Levinson-Durbin -> ブロック毎シフトで量子化。残差絶対値和を返す。
static long WavLpcAnalyze(const std::vector<uint16_t>& v, size_t lo, size_t hi, int16_t* q, int& outShift) {
    const long BIGCOST = 0x7fffffffL;
    size_t len = hi - lo;
    if (len < static_cast<size_t>(LPC_ORDER + 1)) return BIGCOST;

    // Welch 窓を掛けてから自己相関 (係数推定の安定化)
    std::vector<double> s(len);
    double half = (len - 1) / 2.0;
    for (size_t i = 0; i < len; ++i) {
        double t = (static_cast<double>(i) - half) / (half > 0 ? half : 1.0);
        double w = 1.0 - t * t;
        s[i] = static_cast<double>(static_cast<int16_t>(v[lo + i])) * w;
    }
    double R[LPC_ORDER + 1];
    for (int j = 0; j <= LPC_ORDER; ++j) { double acc = 0; for (size_t i = j; i < len; ++i) acc += s[i] * s[i - j]; R[j] = acc; }
    if (R[0] <= 0) return BIGCOST;

    double a[LPC_ORDER] = {0}, err = R[0];
    for (int i = 0; i < LPC_ORDER; ++i) {
        double acc = R[i + 1];
        for (int j = 0; j < i; ++j) acc -= a[j] * R[i - j];
        double k = acc / err;
        a[i] = k;
        for (int j = 0; j < i / 2; ++j) { double t = a[j]; a[j] = t - k * a[i - 1 - j]; a[i - 1 - j] -= k * t; }
        if (i & 1) a[i / 2] -= k * a[i / 2];
        err *= (1 - k * k);
        if (err <= 0) break;
    }

    // 係数の最大値からブロック毎の量子化シフトを決定 (精度 ~14bit, int16 に収める)
    double cmax = 0;
    for (int j = 0; j < LPC_ORDER; ++j) { double c = a[j] < 0 ? -a[j] : a[j]; if (c > cmax) cmax = c; }
    if (cmax <= 0) return BIGCOST;
    int shift = 13 - static_cast<int>(std::floor(std::log2(cmax)));
    if (shift > 15) shift = 15;
    if (shift < 1)  shift = 1;
    outShift = shift;
    for (int j = 0; j < LPC_ORDER; ++j) {
        long qq = std::lround(a[j] * static_cast<double>(1 << shift));
        if (qq > 32767) qq = 32767;
        if (qq < -32768) qq = -32768;
        q[j] = static_cast<int16_t>(qq);
    }

    long cost = 0;
    for (size_t i = lo; i < hi; ++i) {
        int pred = WavLpcPredict(v.data(), i, q, shift);
        uint16_t r = static_cast<uint16_t>(v[i] - static_cast<uint16_t>(pred));
        int sv = (r < 32768) ? r : static_cast<int>(r) - 65536;
        cost += (sv < 0) ? -sv : sv;
    }
    return cost;
}

static bool ParseWav(const std::vector<uint8_t>& in, size_t& dataStart, size_t& frames) {
    const size_t n = in.size();
    if (n < 12) return false;
    if (std::memcmp(in.data(), "RIFF", 4) != 0) return false;
    if (std::memcmp(in.data() + 8, "WAVE", 4) != 0) return false;

    size_t pos = 12;
    bool fmtOk = false;
    uint16_t fmt = 0, ch = 0, bits = 0;
    while (pos + 8 <= n) {
        const uint8_t* p = in.data() + pos;
        uint32_t csize = GetU32(p + 4);
        size_t payload = pos + 8;
        if (std::memcmp(p, "fmt ", 4) == 0 && csize >= 16 && payload + 16 <= n) {
            fmt  = static_cast<uint16_t>(in[payload]      | (in[payload + 1]  << 8));
            ch   = static_cast<uint16_t>(in[payload + 2]  | (in[payload + 3]  << 8));
            bits = static_cast<uint16_t>(in[payload + 14] | (in[payload + 15] << 8));
            fmtOk = true;
        } else if (std::memcmp(p, "data", 4) == 0) {
            if (!fmtOk || fmt != 1 || ch != 2 || bits != 16) return false;
            size_t dsize = std::min(static_cast<size_t>(csize), n - payload);
            dataStart = payload;
            frames = dsize / 4;                 // 4 bytes/frame (16bit x 2ch)
            return frames > 0;
        }
        pos = payload + csize + (csize & 1);     // 偶数パディング
    }
    return false;
}

// 符号-符号 LMS 適応フィルタ (Monkey's Audio 系)。ブロックLPC残差の、ブロック内非定常な
// 短期相関を捉える。全演算を16bit(mod 2^16)に丸めるので enc/dec で状態が完全同期=可逆。
struct WavLMS {
    static const int K = 64, SH = 11;
    int w[K] = {0}, h[K] = {0};
    int16_t predict() const {
        long long s = 0;
        for (int k = 0; k < K; ++k) s += static_cast<long long>(w[k]) * h[k];
        return static_cast<int16_t>(s >> SH);
    }
    void adapt(int16_t e) {
        int se = e > 0 ? 1 : (e < 0 ? -1 : 0);
        for (int k = 0; k < K; ++k) {
            w[k] += se * (h[k] > 0 ? 1 : (h[k] < 0 ? -1 : 0));
            if (w[k] < -(1 << 18)) w[k] = -(1 << 18); else if (w[k] > (1 << 18)) w[k] = (1 << 18);
        }
    }
    void push(int16_t x) { for (int k = K - 1; k > 0; --k) h[k] = h[k - 1]; h[0] = x; }
    int16_t enc(int16_t x) { int16_t p = predict(); int16_t e = static_cast<int16_t>(x - p); adapt(e); push(x); return e; }
    int16_t dec(int16_t e) { int16_t p = predict(); int16_t x = static_cast<int16_t>(e + p); adapt(e); push(x); return x; }
};

// stereoMode: 0=Mid/Side, 1=Left/Side, 2=Right/Side, 3=Left/Right(独立)。先頭1バイトに保存。
std::vector<uint8_t> Encode_Wav_MidSide_Delta(const std::vector<uint8_t>& in, int stereoMode) {
    std::vector<uint8_t> out;
    out.push_back(static_cast<uint8_t>(stereoMode));     // 先頭にステレオモード
    size_t dataStart = 0, frames = 0;

    if (!ParseWav(in, dataStart, frames)) {
        // フォールバック: 全体を header として格納 (完全可逆)
        PutU32(out, static_cast<uint32_t>(in.size()));
        out.insert(out.end(), in.begin(), in.end());
        PutU32(out, 0);                          // frames
        PutU32(out, 0);                          // blockSize (未使用)
        PutU32(out, 0);                          // trailerLen
        return out;
    }

    const size_t pcmEnd = dataStart + frames * 4;

    // Mid/Side を構築
    std::vector<uint16_t> mid(frames), side(frames);
    for (size_t i = 0; i < frames; ++i) {
        const uint8_t* s = in.data() + dataStart + i * 4;
        uint16_t L = static_cast<uint16_t>(s[0] | (s[1] << 8));
        uint16_t R = static_cast<uint16_t>(s[2] | (s[3] << 8));
        uint16_t sd = static_cast<uint16_t>(L - R);
        switch (stereoMode) {
            case 1:  side[i] = sd; mid[i] = L; break;                                  // L/S
            case 2:  side[i] = sd; mid[i] = R; break;                                  // R/S
            case 3:  side[i] = R;  mid[i] = L; break;                                  // L/R 独立
            default: side[i] = sd; mid[i] = static_cast<uint16_t>(R + (sd >> 1)); break; // M/S
        }
    }

    // ブロック別 (連続履歴) の FLAC 固定予測 (0〜4 次)。すべて uint16 ラップで可逆。
    const size_t BS = 4096;                              // フレーム/ブロック (インターリーブ後は4096が最良)
    const size_t numBlocks = (frames + BS - 1) / BS;

    // [lo,hi) ブロックで order の残差絶対値和を計算 (履歴は連続 = 全域参照)
    auto blockCost = [&](const std::vector<uint16_t>& v, size_t lo, size_t hi, int order) {
        long c = 0;
        for (size_t i = lo; i < hi; ++i) {
            uint16_t p1 = (i >= 1) ? v[i - 1] : 0, p2 = (i >= 2) ? v[i - 2] : 0;
            uint16_t p3 = (i >= 3) ? v[i - 3] : 0, p4 = (i >= 4) ? v[i - 4] : 0;
            uint16_t r = static_cast<uint16_t>(v[i] - WavPredict(order, p1, p2, p3, p4));
            int sv = (r < 32768) ? r : static_cast<int>(r) - 65536;
            c += (sv < 0) ? -sv : sv;
        }
        return c;
    };
    // 固定予測の最良次数とそのコストを返す
    auto pickFixed = [&](const std::vector<uint16_t>& v, size_t lo, size_t hi, long& outCost) {
        int bo = 0; long bc = -1;
        for (int o = 0; o <= 4; ++o) { long c = blockCost(v, lo, hi, o); if (bc < 0 || c < bc) { bc = c; bo = o; } }
        outCost = bc; return bo;
    };

    // ブロックごとに mid/side の手法を決定: 固定予測(0-4) か LPC(5)。LPC 採用時は係数+シフトを保持。
    std::vector<uint8_t> midMethod(numBlocks), sideMethod(numBlocks);
    std::vector<uint8_t> midShift(numBlocks, 0), sideShift(numBlocks, 0);
    std::vector<int16_t> midCoef(numBlocks * LPC_ORDER, 0), sideCoef(numBlocks * LPC_ORDER, 0);
    auto chooseMethod = [&](const std::vector<uint16_t>& v, size_t lo, size_t hi,
                            uint8_t& method, uint8_t& shiftOut, int16_t* coef) {
        long fc; int fo = pickFixed(v, lo, hi, fc);
        int16_t q[LPC_ORDER]; int sh = 0; long lc = WavLpcAnalyze(v, lo, hi, q, sh);
        if (lc < fc) { method = WAV_METHOD_LPC; shiftOut = static_cast<uint8_t>(sh); for (int j = 0; j < LPC_ORDER; ++j) coef[j] = q[j]; }
        else         { method = static_cast<uint8_t>(fo); }
    };
    for (size_t b = 0; b < numBlocks; ++b) {
        size_t lo = b * BS, hi = std::min(frames, (b + 1) * BS);
        chooseMethod(mid,  lo, hi, midMethod[b],  midShift[b],  &midCoef[b * LPC_ORDER]);
        chooseMethod(side, lo, hi, sideMethod[b], sideShift[b], &sideCoef[b * LPC_ORDER]);
    }

    // ヘッダ出力
    PutU32(out, static_cast<uint32_t>(dataStart));
    out.insert(out.end(), in.begin(), in.begin() + dataStart);          // header
    PutU32(out, static_cast<uint32_t>(frames));
    PutU32(out, static_cast<uint32_t>(BS));                             // blockSize
    PutU32(out, static_cast<uint32_t>(in.size() - pcmEnd));
    out.insert(out.end(), in.begin() + pcmEnd, in.end());               // trailer

    // ブロック手法配列 (mid, side 各 1 バイト) と、LPC ブロックの [shift][係数]
    auto putI16 = [&](int16_t x) { uint16_t u = static_cast<uint16_t>(x); out.push_back(static_cast<uint8_t>(u)); out.push_back(static_cast<uint8_t>(u >> 8)); };
    for (size_t b = 0; b < numBlocks; ++b) { out.push_back(midMethod[b]); out.push_back(sideMethod[b]); }
    for (size_t b = 0; b < numBlocks; ++b) {
        if (midMethod[b]  == WAV_METHOD_LPC) { out.push_back(midShift[b]);  for (int j = 0; j < LPC_ORDER; ++j) putI16(midCoef[b * LPC_ORDER + j]); }
        if (sideMethod[b] == WAV_METHOD_LPC) { out.push_back(sideShift[b]); for (int j = 0; j < LPC_ORDER; ++j) putI16(sideCoef[b * LPC_ORDER + j]); }
    }

    // 各フレームを所属ブロックの手法で予測 -> LMS適応フィルタ -> バイトプレーン分離 (履歴は連続)
    std::vector<uint8_t> midLo(frames), midHi(frames), sideLo(frames), sideHi(frames);
    WavLMS lmsM, lmsS;
    for (size_t i = 0; i < frames; ++i) {
        size_t b = i / BS;
        uint16_t mpred, spred;
        if (midMethod[b] == WAV_METHOD_LPC) mpred = static_cast<uint16_t>(WavLpcPredict(mid.data(), i, &midCoef[b * LPC_ORDER], midShift[b]));
        else { uint16_t p1=(i>=1)?mid[i-1]:0,p2=(i>=2)?mid[i-2]:0,p3=(i>=3)?mid[i-3]:0,p4=(i>=4)?mid[i-4]:0; mpred = WavPredict(midMethod[b],p1,p2,p3,p4); }
        if (sideMethod[b] == WAV_METHOD_LPC) spred = static_cast<uint16_t>(WavLpcPredict(side.data(), i, &sideCoef[b * LPC_ORDER], sideShift[b]));
        else { uint16_t p1=(i>=1)?side[i-1]:0,p2=(i>=2)?side[i-2]:0,p3=(i>=3)?side[i-3]:0,p4=(i>=4)?side[i-4]:0; spred = WavPredict(sideMethod[b],p1,p2,p3,p4); }
        uint16_t mr = static_cast<uint16_t>(lmsM.enc(static_cast<int16_t>(mid[i]  - mpred)));
        uint16_t sr = static_cast<uint16_t>(lmsS.enc(static_cast<int16_t>(side[i] - spred)));
        midLo[i]  = static_cast<uint8_t>(mr);       midHi[i]  = static_cast<uint8_t>(mr >> 8);
        sideLo[i] = static_cast<uint8_t>(sr);       sideHi[i] = static_cast<uint8_t>(sr >> 8);
    }
    // フレームインターリーブ: フレーム毎に mid,side の16bit残差を隣接配置 (mLo,mHi,sLo,sHi)。
    // 各16bit値の lo,hi が連続し、CM の多バイト文脈が16bit残差を強力にモデル化 (プレーン分離より大幅縮小)。
    for (size_t i = 0; i < frames; ++i) { out.push_back(midLo[i]); out.push_back(midHi[i]); out.push_back(sideLo[i]); out.push_back(sideHi[i]); }
    return out;
}

std::vector<uint8_t> Decode_Wav_MidSide_Delta(const std::vector<uint8_t>& in) {
    size_t pos = 0;
    int stereoMode = in.empty() ? 0 : in[pos++];        // 先頭のステレオモード
    auto rdU32 = [&]() { uint32_t v = GetU32(in.data() + pos); pos += 4; return v; };

    uint32_t headerLen = rdU32();
    std::vector<uint8_t> out(in.begin() + pos, in.begin() + pos + headerLen);  // header
    pos += headerLen;
    uint32_t frames     = rdU32();
    uint32_t blockSize  = rdU32();               // フレーム/ブロック
    uint32_t trailerLen = rdU32();
    std::vector<uint8_t> trailer(in.begin() + pos, in.begin() + pos + trailerLen);
    pos += trailerLen;

    if (frames == 0) return out;                 // フォールバック (out == 元データ全体)

    const size_t numBlocks = (frames + blockSize - 1) / blockSize;

    // ブロック手法配列 (mid, side 各 1 バイト)
    const uint8_t* methods = in.data() + pos; pos += 2 * numBlocks;
    // LPC ブロックの [shift][量子化係数] を読み出し
    std::vector<int16_t> midCoef(numBlocks * LPC_ORDER, 0), sideCoef(numBlocks * LPC_ORDER, 0);
    std::vector<uint8_t> midShift(numBlocks, 0), sideShift(numBlocks, 0);
    auto getI16 = [&]() { uint16_t u = static_cast<uint16_t>(in[pos] | (in[pos + 1] << 8)); pos += 2; return static_cast<int16_t>(u); };
    for (size_t b = 0; b < numBlocks; ++b) {
        if (methods[2 * b]     == WAV_METHOD_LPC) { midShift[b]  = in[pos++]; for (int j = 0; j < LPC_ORDER; ++j) midCoef[b * LPC_ORDER + j]  = getI16(); }
        if (methods[2 * b + 1] == WAV_METHOD_LPC) { sideShift[b] = in[pos++]; for (int j = 0; j < LPC_ORDER; ++j) sideCoef[b * LPC_ORDER + j] = getI16(); }
    }

    // エンコーダー出力順: midLo, sideLo, midHi, sideHi (Lo系→Hi系)
    const uint8_t* fp = in.data() + pos;                  // フレーム毎 4B: mLo,mHi,sLo,sHi

    // 残差から各チャンネル値を逆予測で復元 (ブロックごとに手法切替, 履歴は連続)
    std::vector<uint16_t> mid(frames), side(frames);
    WavLMS lmsM, lmsS;
    for (uint32_t i = 0; i < frames; ++i) {
        size_t b = i / blockSize;
        uint8_t mMethod = methods[2 * b], sMethod = methods[2 * b + 1];
        uint16_t me = static_cast<uint16_t>(fp[4 * i]     | (fp[4 * i + 1] << 8));   // LMS残差
        uint16_t se = static_cast<uint16_t>(fp[4 * i + 2] | (fp[4 * i + 3] << 8));
        uint16_t mr = static_cast<uint16_t>(lmsM.dec(static_cast<int16_t>(me)));     // LPC残差へ復元
        uint16_t sr = static_cast<uint16_t>(lmsS.dec(static_cast<int16_t>(se)));
        uint16_t mpred, spred;
        if (mMethod == WAV_METHOD_LPC) mpred = static_cast<uint16_t>(WavLpcPredict(mid.data(), i, &midCoef[b * LPC_ORDER], midShift[b]));
        else { uint16_t p1=(i>=1)?mid[i-1]:0,p2=(i>=2)?mid[i-2]:0,p3=(i>=3)?mid[i-3]:0,p4=(i>=4)?mid[i-4]:0; mpred = WavPredict(mMethod,p1,p2,p3,p4); }
        if (sMethod == WAV_METHOD_LPC) spred = static_cast<uint16_t>(WavLpcPredict(side.data(), i, &sideCoef[b * LPC_ORDER], sideShift[b]));
        else { uint16_t p1=(i>=1)?side[i-1]:0,p2=(i>=2)?side[i-2]:0,p3=(i>=3)?side[i-3]:0,p4=(i>=4)?side[i-4]:0; spred = WavPredict(sMethod,p1,p2,p3,p4); }
        mid[i]  = static_cast<uint16_t>(mr + mpred);
        side[i] = static_cast<uint16_t>(sr + spred);
    }

    out.reserve(static_cast<size_t>(headerLen) + frames * 4 + trailerLen);
    for (uint32_t i = 0; i < frames; ++i) {
        uint16_t L, R;
        switch (stereoMode) {
            case 1:  L = mid[i]; R = static_cast<uint16_t>(mid[i] - side[i]); break;        // L/S
            case 2:  R = mid[i]; L = static_cast<uint16_t>(mid[i] + side[i]); break;        // R/S
            case 3:  L = mid[i]; R = side[i]; break;                                        // L/R
            default: R = static_cast<uint16_t>(mid[i] - (side[i] >> 1)); L = static_cast<uint16_t>(side[i] + R); break; // M/S
        }
        out.push_back(static_cast<uint8_t>(L));
        out.push_back(static_cast<uint8_t>(L >> 8));
        out.push_back(static_cast<uint8_t>(R));
        out.push_back(static_cast<uint8_t>(R >> 8));
    }
    out.insert(out.end(), trailer.begin(), trailer.end());
    return out;
}

// ==========================================================================
// BMP 2D 予測フィルタ (PNG 方式: 行ごとに最適フィルタを適応選択)
//
//   横方向だけでなく縦方向の相関も使う。各バイトを近傍 a(左) b(上) c(左上) から
//   予測し残差を出力。フィルタは None/Sub/Up/Average/Paeth から行ごとに最良を選び、
//   行フィルタ種別 (1 byte/行) を保存する。近傍は復元済みバイト = 原データなので可逆。
//
//   出力フォーマット:
//     [u32 headerLen][header(verbatim, = dataOffset まで)]
//     [u32 bytesPerPixel][u32 stride][u32 rows]
//     [u32 trailerLen][trailer(verbatim)]
//     [rows] filterType  | [rows*stride] residual
//   BMP として解析できない入力は rows=0 のフォールバック(完全可逆)。
// ==========================================================================
static int PngPredict(int f, int a, int b, int c) {
    switch (f) {
        case 0: return 0;            // None
        case 1: return a;            // Sub  (左)
        case 2: return b;            // Up   (上)
        case 3: return (a + b) >> 1; // Average
        default: {                   // Paeth
            int p = a + b - c;
            int pa = p - a; if (pa < 0) pa = -pa;
            int pb = p - b; if (pb < 0) pb = -pb;
            int pc = p - c; if (pc < 0) pc = -pc;
            if (pa <= pb && pa <= pc) return a;
            if (pb <= pc) return b;
            return c;
        }
    }
}

// MED (JPEG-LS / LOCO-I の中央値エッジ検出予測): a(左) b(上) c(左上) からエッジを推定。
static int MedPredict(int a, int b, int c) {
    int mx = a > b ? a : b, mn = a < b ? a : b;
    if (c >= mx) return mn;
    if (c <= mn) return mx;
    return a + b - c;
}

// GAP (Gradient Adaptive Predictor, CALIC 系)。周辺画素 (同一チャンネル, bpp ストライド)
// W/WW/N/NW/NE/NN の勾配から適応的に予測。近傍は復元済み = 原データなので可逆。
// 返り値は int (mod 256 で残差化されるので範囲制限は不要)。
static int GapPredict(const uint8_t* row, const uint8_t* prow, const uint8_t* prow2,
                      size_t x, size_t stride, int bpp) {
    auto iabs = [](int v) { return v < 0 ? -v : v; };
    int W  = (x >= static_cast<size_t>(bpp)) ? row[x - bpp] : 0;
    int WW = (x >= static_cast<size_t>(2 * bpp)) ? row[x - 2 * bpp] : W;
    int N  = prow ? prow[x] : 0;
    int NW = (prow && x >= static_cast<size_t>(bpp)) ? prow[x - bpp] : N;
    int NE = (prow && x + bpp < stride) ? prow[x + bpp] : N;
    int NN = prow2 ? prow2[x] : N;
    int dh = iabs(W - WW) + iabs(N - NW) + iabs(N - NE);   // 水平勾配
    int dv = iabs(W - NW) + iabs(N - NN);                  // 垂直勾配
    int pred;
    if (dv - dh > 80) pred = W;
    else if (dh - dv > 80) pred = N;
    else {
        pred = (W + N) / 2 + (NE - NW) / 4;
        if      (dv - dh > 32) pred = (pred + W) / 2;
        else if (dh - dv > 32) pred = (pred + N) / 2;
        else if (dv - dh > 8)  pred = (3 * pred + W) / 4;
        else if (dh - dv > 8)  pred = (3 * pred + N) / 4;
    }
    return pred;
}

static bool ParseBmp(const std::vector<uint8_t>& in, size_t& dataOffset,
                     int& bppBytes, size_t& stride, size_t& rows, size_t& width) {
    const size_t n = in.size();
    if (n < 54) return false;
    if (in[0] != 'B' || in[1] != 'M') return false;
    uint32_t off    = GetU32(in.data() + 10);
    uint32_t ihsize = GetU32(in.data() + 14);
    if (ihsize < 40) return false;                          // BITMAPINFOHEADER 以上のみ
    int32_t  w   = static_cast<int32_t>(GetU32(in.data() + 18));
    int32_t  h   = static_cast<int32_t>(GetU32(in.data() + 22));
    uint16_t bpp = static_cast<uint16_t>(in[28] | (in[29] << 8));
    uint32_t comp = GetU32(in.data() + 30);
    if (comp != 0) return false;                            // 非圧縮 (BI_RGB) のみ
    if (!(bpp == 8 || bpp == 24 || bpp == 32)) return false;
    if (w <= 0 || h == 0) return false;
    size_t height = (h < 0) ? static_cast<size_t>(-(int64_t)h) : static_cast<size_t>(h);
    size_t st = ((static_cast<size_t>(w) * bpp + 31) / 32) * 4;
    if (off >= n || st == 0) return false;
    size_t maxRows = (n - off) / st;
    size_t r = std::min(height, maxRows);
    if (r == 0) return false;
    dataOffset = off; bppBytes = bpp / 8; stride = st; rows = r; width = static_cast<size_t>(w);
    return true;
}

// 可逆カラー変換 (BMP の画素並びは B,G,R)。行内の実画素のみ処理 (パディング除外)。
//   forward : B-=G ; R-=G        (G は不変, 32bit の A も不変)
//   inverse : B+=G ; R+=G
static void BmpColorTransform(std::vector<uint8_t>& pix, size_t stride, size_t rows,
                              size_t width, int bpp, bool forward) {
    if (bpp < 3) return;                                    // 8bit パレットは対象外
    for (size_t r = 0; r < rows; ++r) {
        uint8_t* row = pix.data() + r * stride;
        for (size_t x = 0; x < width; ++x) {
            uint8_t* px = row + x * bpp;
            uint8_t g = px[1];
            if (forward) { px[0] = static_cast<uint8_t>(px[0] - g); px[2] = static_cast<uint8_t>(px[2] - g); }
            else         { px[0] = static_cast<uint8_t>(px[0] + g); px[2] = static_cast<uint8_t>(px[2] + g); }
        }
    }
}

std::vector<uint8_t> Encode_Bmp_2DPredict(const std::vector<uint8_t>& in) {
    std::vector<uint8_t> out;
    size_t off = 0, stride = 0, rows = 0, width = 0; int bpp = 0;

    if (!ParseBmp(in, off, bpp, stride, rows, width)) {
        PutU32(out, static_cast<uint32_t>(in.size()));
        out.insert(out.end(), in.begin(), in.end());
        PutU32(out, 0); PutU32(out, 0); PutU32(out, 0); PutU32(out, 0); PutU32(out, 0);  // bpp,stride,rows,width,trailer
        return out;
    }

    const size_t pixEnd = off + stride * rows;
    PutU32(out, static_cast<uint32_t>(off));
    out.insert(out.end(), in.begin(), in.begin() + off);                 // header
    PutU32(out, static_cast<uint32_t>(bpp));
    PutU32(out, static_cast<uint32_t>(stride));
    PutU32(out, static_cast<uint32_t>(rows));
    PutU32(out, static_cast<uint32_t>(width));
    PutU32(out, static_cast<uint32_t>(in.size() - pixEnd));
    out.insert(out.end(), in.begin() + pixEnd, in.end());                // trailer

    // 画素領域をコピーし、2D 予測の直前に可逆カラー変換 (24/32bit のみ)
    std::vector<uint8_t> pixBuf(in.begin() + off, in.begin() + pixEnd);
    BmpColorTransform(pixBuf, stride, rows, width, bpp, true);
    const uint8_t* pix = pixBuf.data();

    std::vector<uint8_t> ftypes(rows);
    std::vector<uint8_t> resid(rows * stride);
    const int NUM_FILT = 7;                          // 0-4: PNG, 5: GAP, 6: MED
    std::vector<uint8_t> tmp[NUM_FILT];
    for (int f = 0; f < NUM_FILT; ++f) tmp[f].resize(stride);

    // フィルタ選択コスト: L1 ではなく log2(1+|残差|) (エントロピー符号化後のビット数に近い)。
    int bitCost[256];
    for (int v = 0; v < 256; ++v) { int sv = (v < 128) ? v : v - 256; int a = sv < 0 ? -sv : sv; bitCost[v] = static_cast<int>(std::log2(1.0 + a) * 256.0 + 0.5); }

    for (size_t r = 0; r < rows; ++r) {
        const uint8_t* row   = pix + r * stride;
        const uint8_t* prow  = (r > 0) ? pix + (r - 1) * stride : nullptr;
        const uint8_t* prow2 = (r > 1) ? pix + (r - 2) * stride : nullptr;
        long bestCost = -1; int bestF = 0;
        for (int f = 0; f < NUM_FILT; ++f) {
            long cost = 0;
            for (size_t x = 0; x < stride; ++x) {
                int pred;
                if (f == 5) {
                    pred = GapPredict(row, prow, prow2, x, stride, bpp);
                } else {
                    int a = (x >= static_cast<size_t>(bpp)) ? row[x - bpp] : 0;
                    int b = prow ? prow[x] : 0;
                    int c = (prow && x >= static_cast<size_t>(bpp)) ? prow[x - bpp] : 0;
                    pred = (f < 5) ? PngPredict(f, a, b, c) : MedPredict(a, b, c);
                }
                uint8_t res = static_cast<uint8_t>(row[x] - pred);
                tmp[f][x] = res;
                cost += bitCost[res];                         // log2(1+|残差|) コスト
            }
            if (bestCost < 0 || cost < bestCost) { bestCost = cost; bestF = f; }
        }
        ftypes[r] = static_cast<uint8_t>(bestF);
        std::copy(tmp[bestF].begin(), tmp[bestF].end(), resid.begin() + r * stride);
    }
    out.insert(out.end(), ftypes.begin(), ftypes.end());
    out.insert(out.end(), resid.begin(), resid.end());
    return out;
}

std::vector<uint8_t> Decode_Bmp_2DPredict(const std::vector<uint8_t>& in) {
    size_t pos = 0;
    auto rd = [&]() { uint32_t v = GetU32(in.data() + pos); pos += 4; return v; };

    uint32_t headerLen = rd();
    std::vector<uint8_t> out(in.begin() + pos, in.begin() + pos + headerLen);
    pos += headerLen;
    int bpp = static_cast<int>(rd());
    size_t stride = rd();
    size_t rows = rd();
    size_t width = rd();
    uint32_t trailerLen = rd();
    std::vector<uint8_t> trailer(in.begin() + pos, in.begin() + pos + trailerLen);
    pos += trailerLen;

    if (rows == 0) return out;                                // フォールバック

    const uint8_t* ftypes = in.data() + pos; pos += rows;
    const uint8_t* resid  = in.data() + pos; pos += rows * stride;

    std::vector<uint8_t> pix(rows * stride);
    for (size_t r = 0; r < rows; ++r) {
        int f = ftypes[r];
        uint8_t* row = pix.data() + r * stride;
        const uint8_t* prow  = (r > 0) ? pix.data() + (r - 1) * stride : nullptr;
        const uint8_t* prow2 = (r > 1) ? pix.data() + (r - 2) * stride : nullptr;
        for (size_t x = 0; x < stride; ++x) {
            int pred;
            if (f == 5) {
                pred = GapPredict(row, prow, prow2, x, stride, bpp);
            } else {
                int a = (x >= static_cast<size_t>(bpp)) ? row[x - bpp] : 0;
                int b = prow ? prow[x] : 0;
                int c = (prow && x >= static_cast<size_t>(bpp)) ? prow[x - bpp] : 0;
                pred = (f < 5) ? PngPredict(f, a, b, c) : MedPredict(a, b, c);
            }
            row[x] = static_cast<uint8_t>(resid[r * stride + x] + pred);
        }
    }
    BmpColorTransform(pix, stride, rows, width, bpp, false);  // 2D 復元の直後に逆カラー変換
    out.insert(out.end(), pix.begin(), pix.end());
    out.insert(out.end(), trailer.begin(), trailer.end());
    return out;
}

// BMP フィルタ出力のチャンネル分離: 残差バイトをチャンネル毎にまとめる
// (B,G,R),(B,G,R)... -> BBB...GGG...RRR...  bpp<3 や rows=0 では恒等変換
std::vector<uint8_t> BmpSeparateChannels(const std::vector<uint8_t>& in) {
    size_t pos = 0;
    uint32_t headerLen  = GetU32(in.data() + pos); pos += 4 + headerLen;
    uint32_t bpp        = GetU32(in.data() + pos); pos += 4;
    uint32_t stride     = GetU32(in.data() + pos); pos += 4;
    uint32_t rows       = GetU32(in.data() + pos); pos += 4;
    uint32_t width      = GetU32(in.data() + pos); pos += 4;
    uint32_t trailerLen = GetU32(in.data() + pos); pos += 4 + trailerLen;
    pos += rows;   // filter types

    if (bpp < 3 || rows == 0 || pos > in.size()) return in;

    uint32_t pixW = width * bpp;
    uint32_t pad  = stride - pixW;
    std::vector<uint8_t> out(in.begin(), in.begin() + pos);   // metadata unchanged

    const uint8_t* resid = in.data() + pos;
    for (uint32_t ch = 0; ch < bpp; ++ch)
        for (uint32_t r = 0; r < rows; ++r)
            for (uint32_t x = ch; x < pixW; x += bpp)
                out.push_back(resid[r * stride + x]);
    for (uint32_t r = 0; r < rows; ++r)
        for (uint32_t x = 0; x < pad; ++x)
            out.push_back(resid[r * stride + pixW + x]);
    return out;
}

// BmpSeparateChannels の逆: チャンネル毎まとめ -> インターリーブに戻す
std::vector<uint8_t> BmpJoinChannels(const std::vector<uint8_t>& in) {
    size_t pos = 0;
    uint32_t headerLen  = GetU32(in.data() + pos); pos += 4 + headerLen;
    uint32_t bpp        = GetU32(in.data() + pos); pos += 4;
    uint32_t stride     = GetU32(in.data() + pos); pos += 4;
    uint32_t rows       = GetU32(in.data() + pos); pos += 4;
    uint32_t width      = GetU32(in.data() + pos); pos += 4;
    uint32_t trailerLen = GetU32(in.data() + pos); pos += 4 + trailerLen;
    pos += rows;   // filter types

    if (bpp < 3 || rows == 0 || pos > in.size()) return in;

    uint32_t pixW = width * bpp;
    uint32_t pad  = stride - pixW;
    std::vector<uint8_t> out(in.begin(), in.begin() + pos);
    out.resize(pos + rows * stride, 0);
    uint8_t* oResid   = out.data() + pos;
    const uint8_t* sp = in.data() + pos;

    for (uint32_t ch = 0; ch < bpp; ++ch)
        for (uint32_t r = 0; r < rows; ++r)
            for (uint32_t x = ch; x < pixW; x += bpp)
                oResid[r * stride + x] = *sp++;
    for (uint32_t r = 0; r < rows; ++r)
        for (uint32_t x = 0; x < pad; ++x)
            oResid[r * stride + pixW + x] = *sp++;
    return out;
}

// ---- 方式に応じた 1 ファイルの圧縮 / 復元 ----
//   圧縮方式は拡張子ではなく「トーナメント」(全方式を試して最小を採用) で決める。
