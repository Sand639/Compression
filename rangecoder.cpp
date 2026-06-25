#include "compress.h"

// ==========================================================================
// 適応型レンジコーダー (Subbotin 方式, 32bit, 0次適応モデル)
//
//   最終エントロピー段。ハフマンの「整数ビット境界」「256B の符号長表」の無駄を
//   なくし、出現確率を動的更新しながらバイト列を符号化する。
//   正規化: 上位バイトが確定 (low と low+range の最上位が一致) するたび 1 バイト
//   出力。range が小さくなりすぎたら (アンダーフロー) range=(-low)&(BOT-1) で
//   桁あふれ(キャリー)を吸収する標準処理。
//
//   出力: [8 byte] originalSize(uint64 LE) + レンジ符号化ストリーム。
// ==========================================================================
static const uint32_t RC_TOP = 1u << 24;
static const uint32_t RC_BOT = 1u << 16;

struct RangeEncoder {
    uint32_t low = 0, range = 0xFFFFFFFFu;
    std::vector<uint8_t>& out;
    explicit RangeEncoder(std::vector<uint8_t>& o) : out(o) {}
    void encode(uint32_t cum, uint32_t freq, uint32_t tot) {
        range /= tot;
        low   += cum * range;
        range *= freq;
        while ((low ^ (low + range)) < RC_TOP ||
               (range < RC_BOT && ((range = (0u - low) & (RC_BOT - 1)), true))) {
            out.push_back(static_cast<uint8_t>(low >> 24));
            low <<= 8; range <<= 8;
        }
    }
    void flush() { for (int i = 0; i < 4; ++i) { out.push_back(static_cast<uint8_t>(low >> 24)); low <<= 8; } }
};

struct RangeDecoder {
    uint32_t low = 0, range = 0xFFFFFFFFu, code = 0;
    const uint8_t* in; size_t pos = 0, size;
    RangeDecoder(const uint8_t* d, size_t s) : in(d), size(s) {
        for (int i = 0; i < 4; ++i) code = (code << 8) | rb();
    }
    uint8_t rb() { return pos < size ? in[pos++] : 0; }
    uint32_t getfreq(uint32_t tot) { range /= tot; return (code - low) / range; }
    void decode(uint32_t cum, uint32_t freq) {
        low   += cum * range;
        range *= freq;
        while ((low ^ (low + range)) < RC_TOP ||
               (range < RC_BOT && ((range = (0u - low) & (RC_BOT - 1)), true))) {
            code = (code << 8) | rb();
            low <<= 8; range <<= 8;
        }
    }
};

// 0 次適応モデルでバイト列をレンジ符号化 (最終段)
std::vector<uint8_t> Encode_RangeCoder(const std::vector<uint8_t>& input) {
    std::vector<uint8_t> out;
    PutU64(out, static_cast<uint64_t>(input.size()));

    RangeEncoder rc(out);
    uint32_t freq[256];
    for (int i = 0; i < 256; ++i) freq[i] = 1;
    uint32_t total = 256;
    const uint32_t INC = 32;

    for (uint8_t b : input) {
        uint32_t cum = 0;
        for (int i = 0; i < b; ++i) cum += freq[i];
        rc.encode(cum, freq[b], total);
        freq[b] += INC; total += INC;
        if (total >= RC_BOT) {                         // 再スケール (合計を BOT 未満に保つ)
            total = 0;
            for (int i = 0; i < 256; ++i) { freq[i] = (freq[i] >> 1) | 1; total += freq[i]; }
        }
    }
    rc.flush();
    return out;
}

std::vector<uint8_t> Decode_RangeCoder(const std::vector<uint8_t>& input) {
    if (input.size() < 8) return {};
    uint64_t n = GetU64(input.data());
    std::vector<uint8_t> out;
    if (n == 0) return out;
    out.reserve(static_cast<size_t>(n));

    RangeDecoder rc(input.data() + 8, input.size() - 8);
    uint32_t freq[256];
    for (int i = 0; i < 256; ++i) freq[i] = 1;
    uint32_t total = 256;
    const uint32_t INC = 32;

    for (uint64_t k = 0; k < n; ++k) {
        uint32_t value = rc.getfreq(total);
        if (value >= total) value = total - 1;
        uint32_t cum = 0; int s = 0;
        while (cum + freq[s] <= value) { cum += freq[s]; ++s; }
        rc.decode(cum, freq[s]);
        out.push_back(static_cast<uint8_t>(s));
        freq[s] += INC; total += INC;
        if (total >= RC_BOT) {
            total = 0;
            for (int i = 0; i < 256; ++i) { freq[i] = (freq[i] >> 1) | 1; total += freq[i]; }
        }
    }
    return out;
}

// ==========================================================================
// Order-1 (1次) 適応レンジコーダー
//
//   コンテキスト = 直前に処理した 1 バイト (初期コンテキスト 0)。
//   freq[ctx][sym] (256x256) と total[ctx] (256) を持ち、現在のコンテキスト専用の
//   表だけを使って符号化し、更新・再スケールもその表に対してのみ独立に行う。
//   英文/和文や実行ファイルのように「直前の文字で次の分布が大きく変わる」データで
//   0 次より大幅に縮む。
// ==========================================================================
std::vector<uint8_t> Encode_RangeCoderO1(const std::vector<uint8_t>& input) {
    std::vector<uint8_t> out;
    PutU64(out, static_cast<uint64_t>(input.size()));

    RangeEncoder rc(out);
    std::vector<uint32_t> freq(256 * 256, 1);
    std::vector<uint32_t> total(256, 256);
    const uint32_t INC = 24;
    int ctx = 0;

    for (uint8_t b : input) {
        uint32_t* f = &freq[static_cast<size_t>(ctx) * 256];
        uint32_t cum = 0;
        for (int i = 0; i < b; ++i) cum += f[i];
        rc.encode(cum, f[b], total[ctx]);
        f[b] += INC; total[ctx] += INC;
        if (total[ctx] >= RC_BOT) {                    // このコンテキストだけ再スケール
            uint32_t t = 0;
            for (int i = 0; i < 256; ++i) { f[i] = (f[i] >> 1) | 1; t += f[i]; }
            total[ctx] = t;
        }
        ctx = b;
    }
    rc.flush();
    return out;
}

std::vector<uint8_t> Decode_RangeCoderO1(const std::vector<uint8_t>& input) {
    if (input.size() < 8) return {};
    uint64_t n = GetU64(input.data());
    std::vector<uint8_t> out;
    if (n == 0) return out;
    out.reserve(static_cast<size_t>(n));

    RangeDecoder rc(input.data() + 8, input.size() - 8);
    std::vector<uint32_t> freq(256 * 256, 1);
    std::vector<uint32_t> total(256, 256);
    const uint32_t INC = 24;
    int ctx = 0;

    for (uint64_t k = 0; k < n; ++k) {
        uint32_t* f = &freq[static_cast<size_t>(ctx) * 256];
        uint32_t value = rc.getfreq(total[ctx]);
        if (value >= total[ctx]) value = total[ctx] - 1;
        uint32_t cum = 0; int s = 0;
        while (cum + f[s] <= value) { cum += f[s]; ++s; }
        rc.decode(cum, f[s]);
        out.push_back(static_cast<uint8_t>(s));
        f[s] += INC; total[ctx] += INC;
        if (total[ctx] >= RC_BOT) {
            uint32_t t = 0;
            for (int i = 0; i < 256; ++i) { f[i] = (f[i] >> 1) | 1; t += f[i]; }
            total[ctx] = t;
        }
        ctx = s;
    }
    return out;
}

// ==========================================================================
// Order-2 (2次) 適応レンジコーダー
//   コンテキスト = 直前 2 バイト (ctx1=2つ前, ctx2=1つ前; 初期 0,0)。
//   freq[ctx1*256+ctx2][sym] (256x256x256) はヒープに確保 (スタック溢れ回避)。
//   大きく文脈相関の強いリテラル列/生データで 0次/1次を上回りうる。
// ==========================================================================
std::vector<uint8_t> Encode_RangeCoderO2(const std::vector<uint8_t>& input) {
    std::vector<uint8_t> out;
    PutU64(out, static_cast<uint64_t>(input.size()));

    RangeEncoder rc(out);
    std::vector<uint16_t> freq(static_cast<size_t>(256) * 256 * 256, 1);  // ~33MB
    std::vector<uint32_t> total(static_cast<size_t>(256) * 256, 256);
    const uint32_t INC = 24;
    int ctx = 0;                                       // (ctx1<<8)|ctx2

    for (uint8_t b : input) {
        uint16_t* f = &freq[static_cast<size_t>(ctx) * 256];
        uint32_t cum = 0;
        for (int i = 0; i < b; ++i) cum += f[i];
        rc.encode(cum, f[b], total[ctx]);
        f[b] += static_cast<uint16_t>(INC); total[ctx] += INC;
        if (total[ctx] >= RC_BOT) {
            uint32_t t = 0;
            for (int i = 0; i < 256; ++i) { f[i] = static_cast<uint16_t>((f[i] >> 1) | 1); t += f[i]; }
            total[ctx] = t;
        }
        ctx = ((ctx << 8) | b) & 0xFFFF;               // ctx1=ctx2, ctx2=b
    }
    rc.flush();
    return out;
}

std::vector<uint8_t> Decode_RangeCoderO2(const std::vector<uint8_t>& input) {
    if (input.size() < 8) return {};
    uint64_t n = GetU64(input.data());
    std::vector<uint8_t> out;
    if (n == 0) return out;
    out.reserve(static_cast<size_t>(n));

    RangeDecoder rc(input.data() + 8, input.size() - 8);
    std::vector<uint16_t> freq(static_cast<size_t>(256) * 256 * 256, 1);
    std::vector<uint32_t> total(static_cast<size_t>(256) * 256, 256);
    const uint32_t INC = 24;
    int ctx = 0;

    for (uint64_t k = 0; k < n; ++k) {
        uint16_t* f = &freq[static_cast<size_t>(ctx) * 256];
        uint32_t value = rc.getfreq(total[ctx]);
        if (value >= total[ctx]) value = total[ctx] - 1;
        uint32_t cum = 0; int s = 0;
        while (cum + f[s] <= value) { cum += f[s]; ++s; }
        rc.decode(cum, f[s]);
        out.push_back(static_cast<uint8_t>(s));
        f[s] += static_cast<uint16_t>(INC); total[ctx] += INC;
        if (total[ctx] >= RC_BOT) {
            uint32_t t = 0;
            for (int i = 0; i < 256; ++i) { f[i] = static_cast<uint16_t>((f[i] >> 1) | 1); t += f[i]; }
            total[ctx] = t;
        }
        ctx = ((ctx << 8) | s) & 0xFFFF;
    }
    return out;
}

// ==========================================================================
// 最終エントロピー段: 0次/1次(/大きい入力は2次) を試し最小を 1 バイトのタグ付きで採用。
//   tag: 0=order0, 1=order1, 2=order2
// ==========================================================================
std::vector<uint8_t> Encode_Entropy(const std::vector<uint8_t>& input) {
    std::vector<uint8_t> best = Encode_RangeCoder(input);
    uint8_t tag = 0;
    {
        std::vector<uint8_t> e1 = Encode_RangeCoderO1(input);
        if (e1.size() < best.size()) { best = std::move(e1); tag = 1; }
    }
    if (input.size() >= 65536) {                        // 2次は十分大きい入力でのみ試す
        std::vector<uint8_t> e2 = Encode_RangeCoderO2(input);
        if (e2.size() < best.size()) { best = std::move(e2); tag = 2; }
    }
    std::vector<uint8_t> out;
    out.push_back(tag);
    out.insert(out.end(), best.begin(), best.end());
    return out;
}
std::vector<uint8_t> Decode_Entropy(const std::vector<uint8_t>& input) {
    if (input.empty()) return {};
    uint8_t tag = input[0];
    std::vector<uint8_t> rest(input.begin() + 1, input.end());
    if (tag == 2) return Decode_RangeCoderO2(rest);
    if (tag == 1) return Decode_RangeCoderO1(rest);
    return Decode_RangeCoder(rest);
}

