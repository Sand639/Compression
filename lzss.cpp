#include "compress.h"

// ==========================================================================
// LZSS (大窓 + LEB128 可変長, ハッシュチェーン探索)
//
//   出力フォーマット (バイト境界):
//     [8 byte] originalSize (uint64 LE)
//     以降、トークンを 8 個ごとにまとめる:
//        [1 byte] フラグ (MSB 側から各トークン: 0=リテラル, 1=一致)
//        各トークン:
//           リテラル: [1 byte] そのままのバイト
//           一致    : [LEB128] (length - MIN_MATCH) , [LEB128] (distance - 1)
//   近距離の一致ほど LEB128 が短くなり、固定長より小さく、後段ハフマンとも相性が良い。
//
//   ウィンドウ 1 MiB、最短一致 3、最長一致 258。3 バイトハッシュのチェーンを辿る。
// ==========================================================================
static const int LZSS_WINDOW    = 1 << 20;   // 1 MiB 探索窓
static const int LZSS_MIN_MATCH = 3;
static const int LZSS_MAX_MATCH = 258;
static const int LZSS_HASH_BITS = 17;
static const int LZSS_HASH_SIZE = 1 << LZSS_HASH_BITS;
static const int LZSS_MAX_CHAIN = 4096;      // 一致探索の最大チェーン長

static inline uint32_t LzssHash(const uint8_t* p) {
    uint32_t v = static_cast<uint32_t>(p[0])
               | (static_cast<uint32_t>(p[1]) << 8)
               | (static_cast<uint32_t>(p[2]) << 16);
    return (v * 2654435761u) >> (32 - LZSS_HASH_BITS);
}

struct LzTok { int len; int dist; };          // len==1 -> リテラル

void LzPutLEB(std::vector<uint8_t>& v, uint32_t x) {
    do { uint8_t b = static_cast<uint8_t>(x & 0x7F); x >>= 7; if (x) b |= 0x80; v.push_back(b); } while (x);
}
uint32_t LzGetLEB(const uint8_t* data, size_t& i) {
    uint32_t v = 0; int s = 0; uint8_t b;
    do { b = data[i++]; v |= static_cast<uint32_t>(b & 0x7F) << s; s += 7; } while (b & 0x80);
    return v;
}

// トークン列 -> LZSS バイト列 (8 トークンごとに 1 フラグバイト)
static std::vector<uint8_t> EmitLZSS(const uint8_t* d, int n, const std::vector<LzTok>& toks) {
    std::vector<uint8_t> out;
    PutU64(out, static_cast<uint64_t>(n));
    size_t ti = 0, pos = 0;
    while (ti < toks.size()) {
        size_t cnt = std::min<size_t>(8, toks.size() - ti);
        uint8_t flag = 0;
        for (size_t k = 0; k < cnt; ++k) if (toks[ti + k].len != 1) flag |= (1u << (7 - k));
        out.push_back(flag);
        for (size_t k = 0; k < cnt; ++k) {
            const LzTok& t = toks[ti + k];
            if (t.len == 1) { out.push_back(d[pos]); pos += 1; }
            else {
                LzPutLEB(out, static_cast<uint32_t>(t.len - LZSS_MIN_MATCH));
                LzPutLEB(out, static_cast<uint32_t>(t.dist - 1));
                pos += static_cast<size_t>(t.len);
            }
        }
        ti += cnt;
    }
    return out;
}

std::vector<uint8_t> Encode_LZSS_Greedy(const std::vector<uint8_t>& input) {
    const int n = static_cast<int>(input.size());
    const uint8_t* d = input.data();
    std::vector<LzTok> toks;

    if (n > 0) {
        std::vector<int> head(LZSS_HASH_SIZE, -1);
        std::vector<int> prev(n, -1);
        auto insert = [&](int p) {
            if (p + LZSS_MIN_MATCH <= n) { uint32_t h = LzssHash(d + p); prev[p] = head[h]; head[h] = p; }
        };
        int pos = 0;
        while (pos < n) {
            int bestLen = 0, bestDist = 0;
            int maxLen = std::min(LZSS_MAX_MATCH, n - pos);
            if (maxLen >= LZSS_MIN_MATCH) {
                int minPos = std::max(0, pos - LZSS_WINDOW);
                int cand = head[LzssHash(d + pos)];
                int chain = LZSS_MAX_CHAIN;
                while (cand >= minPos && chain-- > 0) {
                    if (d[cand + bestLen] == d[pos + bestLen]) {
                        int l = 0;
                        while (l < maxLen && d[cand + l] == d[pos + l]) ++l;
                        if (l > bestLen) { bestLen = l; bestDist = pos - cand; if (l >= maxLen) break; }
                    }
                    cand = prev[cand];
                }
            }
            if (bestLen >= LZSS_MIN_MATCH) {
                toks.push_back({bestLen, bestDist});
                int end = pos + bestLen;
                while (pos < end) { insert(pos); ++pos; }
            } else {
                toks.push_back({1, 0});
                insert(pos);
                ++pos;
            }
        }
    }
    return EmitLZSS(d, n, toks);
}

std::vector<uint8_t> Decode_LZSS(const std::vector<uint8_t>& input) {
    if (input.size() < 8) return {};
    uint64_t n = GetU64(input.data());

    std::vector<uint8_t> out;
    out.reserve(static_cast<size_t>(n));
    const uint8_t* p = input.data();
    size_t i = 8;
    uint8_t flags = 0;
    int flagsLeft = 0;

    while (out.size() < n) {
        if (flagsLeft == 0) { flags = p[i++]; flagsLeft = 8; }
        bool isMatch = (flags & 0x80) != 0;
        flags <<= 1; --flagsLeft;
        if (!isMatch) {
            out.push_back(p[i++]);
        } else {
            uint32_t len  = LzGetLEB(p, i) + LZSS_MIN_MATCH;
            uint32_t dist = LzGetLEB(p, i) + 1;
            size_t start = out.size() - dist;
            for (uint32_t k = 0; k < len; ++k) out.push_back(out[start + k]);  // 重なり対応
        }
    }
    return out;
}

// ==========================================================================
// LZSS 最適構文解析 (Optimal Parsing)
//
//   出力フォーマットは Encode_LZSS_Greedy と同一 (LEB128) なので Decode_LZSS で
//   そのまま復元できる。エンコード側の「賢さ」だけを引き上げる。
//
//   手法: 各位置の最長一致を前計算し、前方 DP (最短経路) で
//         「リテラル / 長さ L の一致」の全分岐から推定ビット最小の経路を選ぶ。
//   コスト模型: 後段ハフマンが見る「バイト値の推定ビット数」(bytePrice) で各
//   トークンが出力するバイト (リテラル / LEB128 の各バイト) を価格付けする。
//   zopfli 風に反復: iter0 は一律 8bit、以降は直前の出力バイト分布から再計算。
//   各反復で実際に Encode_Huffman を通した実サイズを測り最小を採用
//   (greedy も候補に含めるので貪欲法より悪化しない)。
// ==========================================================================
// 最適パースを行い、選ばれたトークン列を返す (単一ストリーム版とスプリット版で共用)
static std::vector<LzTok> LzssParse(const std::vector<uint8_t>& input) {
    const int n = static_cast<int>(input.size());
    const uint8_t* d = input.data();
    if (n == 0) return {};

    // ---- 1. 各位置の最長一致 (長さ・距離) を前計算 ----
    std::vector<int> mlen(n, 0), mdist(n, 0);
    {
        std::vector<int> head(LZSS_HASH_SIZE, -1), prev(n, -1);
        for (int i = 0; i < n; ++i) {
            int maxLen = std::min(LZSS_MAX_MATCH, n - i);
            int bestLen = 0, bestDist = 0;
            if (maxLen >= LZSS_MIN_MATCH) {
                int minPos = std::max(0, i - LZSS_WINDOW);
                int cand = head[LzssHash(d + i)];
                int chain = LZSS_MAX_CHAIN;
                while (cand >= minPos && chain-- > 0) {
                    if (d[cand + bestLen] == d[i + bestLen]) {
                        int l = 0;
                        while (l < maxLen && d[cand + l] == d[i + l]) ++l;
                        if (l > bestLen) { bestLen = l; bestDist = i - cand; if (l >= maxLen) break; }
                    }
                    cand = prev[cand];
                }
            }
            mlen[i] = bestLen; mdist[i] = bestDist;
            if (i + LZSS_MIN_MATCH <= n) { uint32_t h = LzssHash(d + i); prev[i] = head[h]; head[h] = i; }
        }
    }

    // ---- 2. 価格モデル: 出力バイト値の推定ビット数 ----
    std::vector<double> bytePrice(256, 8.0);   // iter0 は一律 8bit
    const double flagBit = 1.0;                 // フラグは ~1bit/トークン
    auto lebPrice = [&](uint32_t x) -> double {
        double s = 0; do { uint8_t b = static_cast<uint8_t>(x & 0x7F); x >>= 7; if (x) b |= 0x80; s += bytePrice[b]; } while (x); return s;
    };
    auto litPrice   = [&](uint8_t c) { return flagBit + bytePrice[c]; };
    auto matchLenPrice = [&](int L) { return lebPrice(static_cast<uint32_t>(L - LZSS_MIN_MATCH)); };

    const double INF = 1e18;
    std::vector<double> cost(n + 1, INF);
    std::vector<int> pLen(n + 1, 0), pDist(n + 1, 0);
    // 各位置の最良経路における rep 履歴 (rep0..rep3)
    std::vector<int> rA0(n + 1, 0), rA1(n + 1, 0), rA2(n + 1, 0), rA3(n + 1, 0);

    auto runDP = [&]() -> std::vector<LzTok> {
        std::fill(cost.begin(), cost.end(), INF);
        std::fill(rA0.begin(), rA0.end(), 0); std::fill(rA1.begin(), rA1.end(), 0);
        std::fill(rA2.begin(), rA2.end(), 0); std::fill(rA3.begin(), rA3.end(), 0);
        cost[0] = 0.0;
        // dest を相対づけして緩和 (rep 履歴 nr0..nr3 も記録)
        auto relax = [&](int dest, double c, int L, int dist, int nr0, int nr1, int nr2, int nr3) {
            if (c < cost[dest]) {
                cost[dest] = c; pLen[dest] = L; pDist[dest] = dist;
                rA0[dest] = nr0; rA1[dest] = nr1; rA2[dest] = nr2; rA3[dest] = nr3;
            }
        };
        for (int i = 0; i < n; ++i) {
            if (cost[i] >= INF) continue;
            double base = cost[i];
            int r0 = rA0[i], r1 = rA1[i], r2 = rA2[i], r3 = rA3[i];

            // リテラル (rep 履歴は不変)
            relax(i + 1, base + litPrice(d[i]), 1, 0, r0, r1, r2, r3);

            // 通常一致 (距離コストあり, 履歴をシフト rep0=D)
            int Lmax = mlen[i];
            if (Lmax >= LZSS_MIN_MATCH) {
                int D = mdist[i];
                double pre = base + flagBit + lebPrice(static_cast<uint32_t>(D - 1));
                for (int L = LZSS_MIN_MATCH; L <= Lmax; ++L)
                    relax(i + L, pre + matchLenPrice(L), L, D, D, r0, r1, r2);
            }

            // rep0..rep3 一致 (距離コスト 0。使った rep を rep0 へ昇格)
            int reps[4] = { r0, r1, r2, r3 };
            for (int k = 0; k < 4; ++k) {
                int rk = reps[k];
                if (rk <= 0 || rk > i) continue;
                int maxRep = std::min(LZSS_MAX_MATCH, n - i);
                int s = i - rk;
                int rl = 0;
                while (rl < maxRep && d[s + rl] == d[i + rl]) ++rl;
                if (rl < LZSS_MIN_MATCH) continue;
                // 昇格後の履歴
                int n0, n1, n2, n3;
                if      (k == 0) { n0 = r0; n1 = r1; n2 = r2; n3 = r3; }
                else if (k == 1) { n0 = r1; n1 = r0; n2 = r2; n3 = r3; }
                else if (k == 2) { n0 = r2; n1 = r0; n2 = r1; n3 = r3; }
                else             { n0 = r3; n1 = r0; n2 = r1; n3 = r2; }
                // rep0 は無料、rep1..3 は稀なので軽いペナルティ (過剰選択を抑制)
                double pre = base + flagBit + k * 3.0;        // distPrice 無し
                for (int L = LZSS_MIN_MATCH; L <= rl; ++L)
                    relax(i + L, pre + matchLenPrice(L), L, rk, n0, n1, n2, n3);
            }
        }
        std::vector<LzTok> toks;
        int j = n;
        while (j > 0) {
            int L = pLen[j];
            if (L == 1) { toks.push_back({1, 0}); j -= 1; }
            else        { toks.push_back({L, pDist[j]}); j -= L; }
        }
        std::reverse(toks.begin(), toks.end());
        return toks;
    };

    // 出力バイト分布から bytePrice を再計算 (サイズヘッダ 8B は除く)
    auto rebuildPrices = [&](const std::vector<uint8_t>& bytes) {
        std::vector<double> f(256, 0); double tot = 0;
        for (size_t i = 8; i < bytes.size(); ++i) { f[bytes[i]] += 1; tot += 1; }
        for (int c = 0; c < 256; ++c) bytePrice[c] = -std::log2((f[c] + 0.5) / (tot + 128.0));
    };

    // 貪欲法の候補 (最低保証)
    std::vector<LzTok> greedy;
    { int pos = 0; while (pos < n) {
        if (mlen[pos] >= LZSS_MIN_MATCH) { greedy.push_back({mlen[pos], mdist[pos]}); pos += mlen[pos]; }
        else { greedy.push_back({1, 0}); pos += 1; } } }

    std::vector<LzTok> bestToks = greedy;
    size_t bestSize = Encode_Huffman(EmitLZSS(d, n, greedy)).size();

    const int ITERS = 4;
    for (int it = 0; it < ITERS; ++it) {
        std::vector<LzTok> toks = runDP();
        std::vector<uint8_t> bytes = EmitLZSS(d, n, toks);
        size_t hs = Encode_Huffman(bytes).size();      // 後段サイズの近似で評価
        rebuildPrices(bytes);                           // 次反復用のコスト
        if (hs < bestSize) { bestSize = hs; bestToks = std::move(toks); }
    }
    return bestToks;
}

// 単一ストリーム版 (従来どおり。Decode_LZSS で復元)
std::vector<uint8_t> Encode_LZSS_Optimal(const std::vector<uint8_t>& input) {
    return EmitLZSS(input.data(), static_cast<int>(input.size()), LzssParse(input));
}

// ==========================================================================
// LZSS ストリーム分離 (Split Stream)
//   Stream A: リテラル (生バイト) のみ      -> Order-1 で圧縮 (文脈が強い)
//   Stream B: フラグ + LEB128(長さ) + LEB128(距離) -> Order-0 で圧縮
//   出力: [u64 n][u32 compA サイズ][compA][compB]
// ==========================================================================
static std::vector<uint8_t> EmitLZSS_Split(const uint8_t* d, int n, const std::vector<LzTok>& toks) {
    std::vector<uint8_t> A, B;                          // A=リテラル, B=フラグ+メタ
    size_t ti = 0, pos = 0;
    while (ti < toks.size()) {
        size_t cnt = std::min<size_t>(8, toks.size() - ti);
        uint8_t flag = 0;
        for (size_t k = 0; k < cnt; ++k) if (toks[ti + k].len != 1) flag |= (1u << (7 - k));
        B.push_back(flag);
        for (size_t k = 0; k < cnt; ++k) {
            const LzTok& t = toks[ti + k];
            if (t.len == 1) { A.push_back(d[pos]); pos += 1; }
            else {
                LzPutLEB(B, static_cast<uint32_t>(t.len - LZSS_MIN_MATCH));
                LzPutLEB(B, static_cast<uint32_t>(t.dist - 1));
                pos += static_cast<size_t>(t.len);
            }
        }
        ti += cnt;
    }
    std::vector<uint8_t> compA = Encode_RangeCoderO1(A);   // リテラルは 1 次
    std::vector<uint8_t> compB = Encode_RangeCoder(B);     // メタは 0 次

    std::vector<uint8_t> out;
    PutU64(out, static_cast<uint64_t>(n));
    PutU32(out, static_cast<uint32_t>(compA.size()));
    out.insert(out.end(), compA.begin(), compA.end());
    out.insert(out.end(), compB.begin(), compB.end());
    return out;
}

// 単体テスト用ラッパ (入力 -> スプリット圧縮)
std::vector<uint8_t> Encode_LZSS_Split(const std::vector<uint8_t>& input) {
    return EmitLZSS_Split(input.data(), static_cast<int>(input.size()), LzssParse(input));
}

std::vector<uint8_t> Decode_LZSS_Split(const std::vector<uint8_t>& input) {
    if (input.size() < 12) return {};
    size_t pos = 0;
    uint64_t n = GetU64(input.data() + pos); pos += 8;
    uint32_t compAsize = GetU32(input.data() + pos); pos += 4;
    if (pos + compAsize > input.size()) return {};
    std::vector<uint8_t> compA(input.begin() + pos, input.begin() + pos + compAsize); pos += compAsize;
    std::vector<uint8_t> compB(input.begin() + pos, input.end());

    std::vector<uint8_t> A = Decode_RangeCoderO1(compA);   // リテラル
    std::vector<uint8_t> B = Decode_RangeCoder(compB);     // フラグ+メタ

    std::vector<uint8_t> out;
    if (n == 0) return out;
    out.reserve(static_cast<size_t>(n));
    size_t ai = 0, bi = 0;
    uint8_t flags = 0; int flagsLeft = 0;
    while (out.size() < n) {
        if (flagsLeft == 0) { flags = B[bi++]; flagsLeft = 8; }
        bool isMatch = (flags & 0x80) != 0;
        flags <<= 1; --flagsLeft;
        if (!isMatch) {
            out.push_back(A[ai++]);
        } else {
            uint32_t len  = LzGetLEB(B.data(), bi) + LZSS_MIN_MATCH;
            uint32_t dist = LzGetLEB(B.data(), bi) + 1;
            size_t start = out.size() - dist;
            for (uint32_t k = 0; k < len; ++k) out.push_back(out[start + k]);
        }
    }
    return out;
}

// ==========================================================================
// LZSS 4 ストリーム分離 (Zstd 風)
//   A: リテラル (生バイト)      -> Order-1
//   B: 一致長さ (len-MIN, LEB128) -> Order-0
//   C: 一致距離 (dist-1, LEB128)  -> Order-0
//   D: フラグ (1 byte/トークン, 0=リテラル/1=一致) -> Order-0
//   出力: [u32 sizeA][u32 sizeB][u32 sizeC][u32 sizeD][compA][compB][compC][compD]
//   復号は D (= トークン数) を辿って再構成するので n ヘッダは不要。
// ==========================================================================
static std::vector<uint8_t> EmitLZSS_4Stream(const uint8_t* d, int /*n*/, const std::vector<LzTok>& toks) {
    std::vector<uint8_t> A, B, C, D;
    size_t pos = 0;
    int rep[4] = { 0, 0, 0, 0 };                   // rep0..rep3 (emit/decode 共通の権威状態)
    for (const LzTok& t : toks) {
        if (t.len == 1) {                          // リテラル (flag 0)
            D.push_back(0); A.push_back(d[pos]); pos += 1;
            continue;
        }
        LzPutLEB(B, static_cast<uint32_t>(t.len - LZSS_MIN_MATCH));   // 長さは共通
        // 距離が rep0..rep3 のいずれかに一致するか (低い index 優先)
        int k = -1;
        for (int j = 0; j < 4; ++j) if (rep[j] == t.dist) { k = j; break; }
        if (k >= 0) {                              // rep 一致 (flag 2+k): 距離は出さない
            D.push_back(static_cast<uint8_t>(2 + k));
            int tmp = rep[k];                      // rep[k] を rep0 へ昇格、間をシフト
            for (int j = k; j > 0; --j) rep[j] = rep[j - 1];
            rep[0] = tmp;
        } else {                                   // 通常一致 (flag 1): 距離を C へ, 履歴シフト
            D.push_back(1);
            LzPutLEB(C, static_cast<uint32_t>(t.dist - 1));
            rep[3] = rep[2]; rep[2] = rep[1]; rep[1] = rep[0]; rep[0] = t.dist;
        }
        pos += static_cast<size_t>(t.len);
    }
    // 各ストリーム独立に 0次/1次 の小さい方を採用 (Encode_Entropy が tag 付きで選択)
    std::vector<uint8_t> cA = Encode_Entropy(A);   // リテラル
    std::vector<uint8_t> cB = Encode_Entropy(B);   // 長さ
    std::vector<uint8_t> cC = Encode_Entropy(C);   // 距離
    std::vector<uint8_t> cD = Encode_Entropy(D);   // フラグ

    std::vector<uint8_t> out;
    PutU32(out, static_cast<uint32_t>(cA.size()));
    PutU32(out, static_cast<uint32_t>(cB.size()));
    PutU32(out, static_cast<uint32_t>(cC.size()));
    PutU32(out, static_cast<uint32_t>(cD.size()));
    out.insert(out.end(), cA.begin(), cA.end());
    out.insert(out.end(), cB.begin(), cB.end());
    out.insert(out.end(), cC.begin(), cC.end());
    out.insert(out.end(), cD.begin(), cD.end());
    return out;
}

std::vector<uint8_t> Encode_LZSS_4Stream(const std::vector<uint8_t>& input) {
    return EmitLZSS_4Stream(input.data(), static_cast<int>(input.size()), LzssParse(input));
}

std::vector<uint8_t> Decode_LZSS_4Stream(const std::vector<uint8_t>& in) {
    if (in.size() < 16) return {};
    size_t pos = 0;
    uint32_t sA = GetU32(in.data() + pos); pos += 4;
    uint32_t sB = GetU32(in.data() + pos); pos += 4;
    uint32_t sC = GetU32(in.data() + pos); pos += 4;
    uint32_t sD = GetU32(in.data() + pos); pos += 4;
    if (pos + (size_t)sA + sB + sC + sD > in.size()) return {};

    auto slice = [&](uint32_t len) {
        std::vector<uint8_t> v(in.begin() + pos, in.begin() + pos + len); pos += len; return v;
    };
    std::vector<uint8_t> A = Decode_Entropy(slice(sA));
    std::vector<uint8_t> B = Decode_Entropy(slice(sB));
    std::vector<uint8_t> C = Decode_Entropy(slice(sC));
    std::vector<uint8_t> D = Decode_Entropy(slice(sD));

    std::vector<uint8_t> out;
    size_t ai = 0, bi = 0, ci = 0;
    int rep[4] = { 0, 0, 0, 0 };                   // emit と同一規則で更新
    for (uint8_t f : D) {
        if (f == 0) {                              // リテラル
            out.push_back(A[ai++]);
            continue;
        }
        uint32_t len = LzGetLEB(B.data(), bi) + LZSS_MIN_MATCH;
        int dist;
        if (f == 1) {                              // 通常一致: 距離を C から, 履歴シフト
            dist = static_cast<int>(LzGetLEB(C.data(), ci) + 1);
            rep[3] = rep[2]; rep[2] = rep[1]; rep[1] = rep[0]; rep[0] = dist;
        } else {                                   // rep 一致 (f=2+k): 距離 = rep[k], 昇格
            int k = f - 2;
            dist = rep[k];
            int tmp = rep[k];
            for (int j = k; j > 0; --j) rep[j] = rep[j - 1];
            rep[0] = tmp;
        }
        size_t start = out.size() - dist;
        for (uint32_t kk = 0; kk < len; ++kk) out.push_back(out[start + kk]);
    }
    return out;
}

// LZSS 系の最終圧縮: 単一(0/1次) / 2分割 / 4分割 を試し最小をタグ付きで採用。
//   出力: [1 byte tag] (1=単一, 2=2分割, 3=4分割) + データ
std::vector<uint8_t> CompressLZSS(const std::vector<uint8_t>& filtered) {
    const int n = static_cast<int>(filtered.size());
    std::vector<LzTok> toks = LzssParse(filtered);
    std::vector<uint8_t> single = Encode_Entropy(EmitLZSS(filtered.data(), n, toks));
    std::vector<uint8_t> split  = EmitLZSS_Split(filtered.data(), n, toks);
    std::vector<uint8_t> quad   = EmitLZSS_4Stream(filtered.data(), n, toks);

    uint8_t tag = 1;
    std::vector<uint8_t>* best = &single;
    if (split.size() < best->size()) { best = &split; tag = 2; }
    if (quad.size()  < best->size()) { best = &quad;  tag = 3; }

    std::vector<uint8_t> out;
    out.push_back(tag);
    out.insert(out.end(), best->begin(), best->end());
    return out;
}
std::vector<uint8_t> DecompressLZSS(const std::vector<uint8_t>& in) {
    if (in.empty()) return {};
    uint8_t tag = in[0];
    std::vector<uint8_t> rest(in.begin() + 1, in.end());
    if (tag == 3) return Decode_LZSS_4Stream(rest);
    if (tag == 2) return Decode_LZSS_Split(rest);
    return Decode_LZSS(Decode_Entropy(rest));            // tag==1
}

