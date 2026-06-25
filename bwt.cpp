// bwt.cpp
// 第1段: Burrows-Wheeler Transform (BWT / ブロックソート)
//
// 方式:
//   - 巡回回転を全てソートし、その末尾列 L を出力する。
//   - 元データに相当する行番号 (primary index) を 4 バイトで先頭に付与し可逆性を担保。
//   - 接尾辞配列は prefix-doubling (O(n log^2 n)) で構築し、同一バイト連続でも破綻しない。
//
// 出力フォーマット:
//   [primary index : uint32 little-endian][L 列 : n バイト]
//
// ※このファイルは UTF-8 (BOM付き) で保存しています。MSVC の IDE ビルドでも
//   /utf-8 を付けずに日本語コメントを正しく解釈させるためです。

#include <cstdint>
#include <cstring>
#include <cctype>
#include <cstdio>
#include <cmath>
#include <vector>
#include <algorithm>
#include <numeric>
#include <queue>
#include <string>
#include <random>
#include <iostream>
#include <fstream>
#include <filesystem>
#include <cassert>

// Windows コンソールを UTF-8 に切り替える (日本語の文字化け対策)。
// windows.h を取り込むと min/max マクロ等が衝突するため、API だけ前方宣言する。
#ifdef _WIN32
extern "C" __declspec(dllimport) int __stdcall SetConsoleOutputCP(unsigned int wCodePageID);
#endif

// ==========================================================================
// BWT 本体
// ==========================================================================

// --------------------------------------------------------------------------
// 巡回回転のソート順 (= 巡回接尾辞配列) を prefix-doubling で構築する。
// 戻り値 sa[i] = i 番目に小さい巡回回転の開始位置。
// --------------------------------------------------------------------------
static std::vector<int> BuildCyclicSuffixArray(const std::vector<uint8_t>& s) {
    const int n = static_cast<int>(s.size());
    std::vector<int> sa(n), rank(n), tmp(n);

    for (int i = 0; i < n; ++i) {
        sa[i] = i;
        rank[i] = s[i];          // 初期ランクはバイト値そのもの
    }

    for (int k = 1; k < n; k <<= 1) {
        // (rank[i], rank[(i+k)%n]) の辞書順で sa をソート
        auto cmp = [&](int a, int b) {
            if (rank[a] != rank[b]) return rank[a] < rank[b];
            int ra = rank[(a + k) % n];
            int rb = rank[(b + k) % n];
            return ra < rb;
        };
        std::sort(sa.begin(), sa.end(), cmp);

        // 新しいランクを再計算
        tmp[sa[0]] = 0;
        for (int i = 1; i < n; ++i) {
            tmp[sa[i]] = tmp[sa[i - 1]] + (cmp(sa[i - 1], sa[i]) ? 1 : 0);
        }
        rank = tmp;

        if (rank[sa[n - 1]] == n - 1) break;  // 全て一意になったら終了
    }
    return sa;
}

// --------------------------------------------------------------------------
// BWT エンコード
// --------------------------------------------------------------------------
std::vector<uint8_t> Encode_BWT(const std::vector<uint8_t>& input) {
    const int n = static_cast<int>(input.size());
    std::vector<uint8_t> output;
    output.reserve(static_cast<size_t>(n) + 4);

    // primary index を格納する 4 バイトを先に確保 (後で書き込む)
    output.resize(4, 0);

    if (n == 0) {
        // 空入力: primary index = 0、L は空
        return output;
    }

    std::vector<int> sa = BuildCyclicSuffixArray(input);

    uint32_t primary = 0;
    for (int i = 0; i < n; ++i) {
        if (sa[i] == 0) primary = static_cast<uint32_t>(i);
        // L[i] = input[(sa[i] - 1 + n) % n]
        int prev = (sa[i] - 1 + n) % n;
        output.push_back(input[prev]);
    }

    // primary index を little-endian で書き込み
    output[0] = static_cast<uint8_t>(primary & 0xFF);
    output[1] = static_cast<uint8_t>((primary >> 8) & 0xFF);
    output[2] = static_cast<uint8_t>((primary >> 16) & 0xFF);
    output[3] = static_cast<uint8_t>((primary >> 24) & 0xFF);

    return output;
}

// --------------------------------------------------------------------------
// BWT デコード (LF-mapping による逆変換, O(n))
// --------------------------------------------------------------------------
std::vector<uint8_t> Decode_BWT(const std::vector<uint8_t>& input) {
    if (input.size() < 4) return {};  // ヘッダ未満は空とみなす

    uint32_t primary = static_cast<uint32_t>(input[0])
                     | (static_cast<uint32_t>(input[1]) << 8)
                     | (static_cast<uint32_t>(input[2]) << 16)
                     | (static_cast<uint32_t>(input[3]) << 24);

    const int n = static_cast<int>(input.size()) - 4;
    const uint8_t* L = input.data() + 4;  // 末尾列

    std::vector<uint8_t> output;
    if (n == 0) return output;

    // C[c] = L 中で c より小さいバイトの総数
    int count[256] = {0};
    for (int i = 0; i < n; ++i) count[L[i]]++;
    int C[256];
    int sum = 0;
    for (int c = 0; c < 256; ++c) { C[c] = sum; sum += count[c]; }

    // LF[i] = C[L[i]] + (L[0..i-1] における L[i] の出現回数)
    std::vector<int> LF(n);
    int occ[256] = {0};
    for (int i = 0; i < n; ++i) {
        LF[i] = C[L[i]] + occ[L[i]];
        occ[L[i]]++;
    }

    // primary から LF を辿り、末尾から復元
    output.resize(n);
    int p = static_cast<int>(primary);
    for (int j = n - 1; j >= 0; --j) {
        output[j] = L[p];
        p = LF[p];
    }
    return output;
}

// ==========================================================================
// 第2段: Move-To-Front (MTF)
//
//   0..255 の記号テーブルを持ち、入力バイトの「現在の順位」を出力する。
//   出力した記号はテーブル先頭へ移動する。これにより、直近に出た記号ほど
//   小さな値で表現され、BWT 後のように同記号が固まった列は 0 付近に偏る。
//   サイズは不変 (1 バイト -> 1 インデックス)。
// ==========================================================================
std::vector<uint8_t> Encode_MTF(const std::vector<uint8_t>& input) {
    uint8_t table[256];
    for (int i = 0; i < 256; ++i) table[i] = static_cast<uint8_t>(i);

    std::vector<uint8_t> output;
    output.reserve(input.size());

    for (uint8_t b : input) {
        // b の現在位置を探す
        int idx = 0;
        while (table[idx] != b) ++idx;
        output.push_back(static_cast<uint8_t>(idx));
        // b を先頭へ移動 (idx より前を 1 つずつ後ろへずらす)
        for (int j = idx; j > 0; --j) table[j] = table[j - 1];
        table[0] = b;
    }
    return output;
}

std::vector<uint8_t> Decode_MTF(const std::vector<uint8_t>& input) {
    uint8_t table[256];
    for (int i = 0; i < 256; ++i) table[i] = static_cast<uint8_t>(i);

    std::vector<uint8_t> output;
    output.reserve(input.size());

    for (uint8_t idx : input) {
        uint8_t b = table[idx];
        output.push_back(b);
        // b を先頭へ移動
        for (int j = idx; j > 0; --j) table[j] = table[j - 1];
        table[0] = b;
    }
    return output;
}

// ==========================================================================
// 第3段: Zero-Run 特化 RLE
//
//   MTF 後のデータは 0x00 が大量に連続する。これに特化し、
//     - 0x00 を「ゼロ連 marker」として予約 (MTF 後はリテラルに 0 が出ないため衝突しない)
//     - marker 直後に連長 N を LEB128 (可変長整数) で格納
//     - 非ゼロバイトはそのまま通過
//   とする。長いゼロ連ほど対数オーダーに圧縮される (例: 0x00 が 1000 個 -> 3 バイト)。
//   バイト列で完全可逆。
//
//   注: 単体テストでは「リテラルの 0x00」も連長 1 のゼロ連として扱われるため、
//       任意のバイト列に対して可逆性が保たれる (短い孤立ゼロは一時的に膨らむが、
//       実データである BWT+MTF 出力では長いゼロ連が支配的で大きく縮む)。
// ==========================================================================
std::vector<uint8_t> Encode_RLE(const std::vector<uint8_t>& input) {
    std::vector<uint8_t> output;
    output.reserve(input.size());

    const size_t n = input.size();
    size_t i = 0;
    while (i < n) {
        if (input[i] == 0x00) {
            // ゼロ連の長さを数える
            size_t j = i;
            while (j < n && input[j] == 0x00) ++j;
            uint64_t run = static_cast<uint64_t>(j - i);   // >= 1

            output.push_back(0x00);                        // ゼロ連 marker
            // 連長を LEB128 で格納
            while (true) {
                uint8_t b = static_cast<uint8_t>(run & 0x7F);
                run >>= 7;
                if (run) b |= 0x80;                        // 継続ビット
                output.push_back(b);
                if (!run) break;
            }
            i = j;
        } else {
            output.push_back(input[i]);
            ++i;
        }
    }
    return output;
}

std::vector<uint8_t> Decode_RLE(const std::vector<uint8_t>& input) {
    std::vector<uint8_t> output;
    output.reserve(input.size());

    const size_t n = input.size();
    size_t i = 0;
    while (i < n) {
        uint8_t b = input[i++];
        if (b == 0x00) {
            // LEB128 で連長を復元
            uint64_t run = 0;
            int shift = 0;
            uint8_t x;
            do {
                x = input[i++];
                run |= static_cast<uint64_t>(x & 0x7F) << shift;
                shift += 7;
            } while (x & 0x80);
            output.insert(output.end(), static_cast<size_t>(run), 0x00);
        } else {
            output.push_back(b);
        }
    }
    return output;
}

// ==========================================================================
// 第4段: 静的・正準ハフマン符号化 (Canonical Huffman)
//
//   ヘッダ:
//     [8 byte] 元データのバイト数 n (uint64 little-endian)
//     [256 byte] 各記号 (0..255) の符号長 (0 = 未使用、最大 15)
//     [bitstream] 各入力バイトを正準符号で MSB-first にパックしたもの
//
//   - 木は保存せず符号長配列のみ保存し、復号側で正準的に再構築する。
//   - 符号長は JPEG/zlib 式アルゴリズムで 15bit 以下に制限 (病的分布対策)。
//   - パディング対策: 復号側は n 記号だけ読むため末尾の余りビットは無視され、
//     ゴミデータは混入しない。
// ==========================================================================

static const int HUFF_MAXBITS = 15;

// ---- ビット I/O (MSB-first) ----
struct BitWriter {
    std::vector<uint8_t>& out;
    uint8_t cur = 0;
    int nbits = 0;
    explicit BitWriter(std::vector<uint8_t>& o) : out(o) {}
    void PutBit(int b) {
        cur = static_cast<uint8_t>((cur << 1) | (b & 1));
        if (++nbits == 8) { out.push_back(cur); cur = 0; nbits = 0; }
    }
    void PutBits(uint32_t value, int len) {        // 上位ビットから
        for (int i = len - 1; i >= 0; --i) PutBit((value >> i) & 1);
    }
    void Flush() {                                  // 端数を 0 埋めして書き出す
        if (nbits) { cur = static_cast<uint8_t>(cur << (8 - nbits)); out.push_back(cur); cur = 0; nbits = 0; }
    }
};
struct BitReader {
    const uint8_t* data;
    size_t size;
    size_t bytepos = 0;
    int bitpos = 0;                                 // 0..7 (MSB から)
    BitReader(const uint8_t* d, size_t s) : data(d), size(s) {}
    int GetBit() {
        if (bytepos >= size) return 0;              // 末尾超過は 0 (健全データでは到達しない)
        int b = (data[bytepos] >> (7 - bitpos)) & 1;
        if (++bitpos == 8) { bitpos = 0; ++bytepos; }
        return b;
    }
    uint32_t GetBits(int len) {                     // 上位ビットから len ビット読む
        uint32_t v = 0;
        for (int i = 0; i < len; ++i) v = (v << 1) | static_cast<uint32_t>(GetBit());
        return v;
    }
};

// ---- 符号長を 15bit 以下に制限する (JPEG Annex K / zlib 方式) ----
//   cnt[l] = 長さ l の符号数。Kraft 等式を保ったまま長さを再分配する。
static void LimitCodeLengths(std::vector<int>& cnt, int maxlen) {
    for (int i = maxlen; i > HUFF_MAXBITS; --i) {
        while (cnt[i] > 0) {
            int j = i - 2;
            while (cnt[j] == 0) --j;
            cnt[i]     -= 2;
            cnt[i - 1] += 1;
            cnt[j + 1] += 2;
            cnt[j]     -= 1;
        }
    }
}

// ---- 各記号の符号長を計算する (Huffman 構築 + 15bit 制限) ----
static std::vector<uint8_t> BuildCodeLengths(const std::vector<uint64_t>& freq) {
    std::vector<uint8_t> len(256, 0);

    std::vector<int> used;
    for (int s = 0; s < 256; ++s) if (freq[s] > 0) used.push_back(s);
    const int m = static_cast<int>(used.size());

    if (m == 0) return len;                 // 記号なし
    if (m == 1) { len[used[0]] = 1; return len; }   // 1 記号は 1bit

    // Huffman 木を構築して各葉の深さ (= 符号長) を求める
    struct HNode { uint64_t f; int left, right, sym; };
    std::vector<HNode> nodes;
    nodes.reserve(2 * m);
    using PQItem = std::pair<uint64_t, int>;        // (freq, node index)
    std::priority_queue<PQItem, std::vector<PQItem>, std::greater<PQItem>> pq;
    for (int s : used) {
        nodes.push_back({freq[s], -1, -1, s});
        pq.push({freq[s], static_cast<int>(nodes.size()) - 1});
    }
    while (pq.size() > 1) {
        auto a = pq.top(); pq.pop();
        auto b = pq.top(); pq.pop();
        nodes.push_back({a.first + b.first, a.second, b.second, -1});
        pq.push({a.first + b.first, static_cast<int>(nodes.size()) - 1});
    }
    int root = pq.top().second;

    // 反復 DFS で深さを計算
    std::vector<std::pair<int, int>> stk;           // (node, depth)
    stk.push_back({root, 0});
    int maxlen = 0;
    while (!stk.empty()) {
        auto [idx, d] = stk.back(); stk.pop_back();
        const HNode& nd = nodes[idx];
        if (nd.sym >= 0) {
            int dd = (d == 0 ? 1 : d);              // 念のため (m>=2 なら d>=1)
            len[nd.sym] = static_cast<uint8_t>(dd);
            maxlen = std::max(maxlen, dd);
        } else {
            stk.push_back({nd.left,  d + 1});
            stk.push_back({nd.right, d + 1});
        }
    }

    if (maxlen > HUFF_MAXBITS) {
        // 長さ別カウントを作り、15bit 以下へ制限
        std::vector<int> cnt(maxlen + 1, 0);
        for (int s : used) cnt[len[s]]++;
        LimitCodeLengths(cnt, maxlen);

        // 頻度降順 (同頻度は記号昇順) に並べ、短い符号を高頻度記号へ割り当てる
        std::sort(used.begin(), used.end(), [&](int a, int b) {
            if (freq[a] != freq[b]) return freq[a] > freq[b];
            return a < b;
        });
        int k = 0;
        for (int l = 1; l <= HUFF_MAXBITS; ++l)
            for (int c = 0; c < cnt[l]; ++c)
                len[used[k++]] = static_cast<uint8_t>(l);
    }
    return len;
}

// ---- 符号長配列から正準符号 (Deflate 方式) を構築 ----
static void BuildCanonicalCodes(const std::vector<uint8_t>& len,
                                std::vector<uint32_t>& code) {
    code.assign(256, 0);
    std::vector<int> bl(HUFF_MAXBITS + 1, 0);
    for (int s = 0; s < 256; ++s) if (len[s]) bl[len[s]]++;

    std::vector<uint32_t> next(HUFF_MAXBITS + 1, 0);
    uint32_t c = 0;
    for (int l = 1; l <= HUFF_MAXBITS; ++l) {
        c = (c + bl[l - 1]) << 1;
        next[l] = c;
    }
    for (int s = 0; s < 256; ++s)
        if (len[s]) code[s] = next[len[s]]++;
}

std::vector<uint8_t> Encode_Huffman(const std::vector<uint8_t>& input) {
    const uint64_t n = input.size();

    // 頻度集計
    std::vector<uint64_t> freq(256, 0);
    for (uint8_t b : input) freq[b]++;

    std::vector<uint8_t> len = BuildCodeLengths(freq);
    std::vector<uint32_t> code;
    BuildCanonicalCodes(len, code);

    std::vector<uint8_t> output;
    output.reserve(8 + 256 + input.size());

    // ヘッダ: n (8 byte LE)
    for (int i = 0; i < 8; ++i)
        output.push_back(static_cast<uint8_t>((n >> (8 * i)) & 0xFF));
    // ヘッダ: 符号長 256 byte
    output.insert(output.end(), len.begin(), len.end());

    // 本体ビットストリーム
    BitWriter bw(output);
    for (uint8_t b : input) bw.PutBits(code[b], len[b]);
    bw.Flush();

    return output;
}

std::vector<uint8_t> Decode_Huffman(const std::vector<uint8_t>& input) {
    if (input.size() < 8 + 256) return {};          // ヘッダ未満

    uint64_t n = 0;
    for (int i = 0; i < 8; ++i)
        n |= static_cast<uint64_t>(input[i]) << (8 * i);

    std::vector<uint8_t> len(input.begin() + 8, input.begin() + 8 + 256);

    std::vector<uint8_t> output;
    if (n == 0) return output;
    output.reserve(static_cast<size_t>(n));

    // 復号テーブル: 長さ別カウントと、(長さ, 記号) 順に並べた記号列
    std::vector<int> counts(HUFF_MAXBITS + 1, 0);
    for (int s = 0; s < 256; ++s) if (len[s]) counts[len[s]]++;
    std::vector<int> sorted_syms;
    sorted_syms.reserve(256);
    for (int l = 1; l <= HUFF_MAXBITS; ++l)
        for (int s = 0; s < 256; ++s)
            if (len[s] == l) sorted_syms.push_back(s);

    BitReader br(input.data() + 8 + 256, input.size() - (8 + 256));

    for (uint64_t i = 0; i < n; ++i) {
        // 正準復号 (zlib puff 方式)
        int c = 0, first = 0, index = 0, sym = -1;
        for (int l = 1; l <= HUFF_MAXBITS; ++l) {
            c |= br.GetBit();
            int cnt = counts[l];
            if (c - first < cnt) { sym = sorted_syms[index + (c - first)]; break; }
            index += cnt;
            first += cnt;
            first <<= 1;
            c <<= 1;
        }
        output.push_back(static_cast<uint8_t>(sym));
    }
    return output;
}

// 最終エントロピー段 (定義は後方、ここでは前方宣言。PutU64/GetU64 依存のため)
//   Encode_Entropy は 0次/1次レンジコーダーの小さい方を 1 バイトのタグ付きで選ぶ。
std::vector<uint8_t> Encode_RangeCoder(const std::vector<uint8_t>& input);
std::vector<uint8_t> Decode_RangeCoder(const std::vector<uint8_t>& input);
std::vector<uint8_t> Encode_Entropy(const std::vector<uint8_t>& input);
std::vector<uint8_t> Decode_Entropy(const std::vector<uint8_t>& input);

// ==========================================================================
// 1 ブロック分の 4 段パイプライン
//   encode:  block ->[BWT]->[MTF]->[RLE]->[RangeCoder]-> chunk
//   decode:  chunk ->[RangeCoder^-1]->[RLE^-1]->[MTF^-1]->[BWT^-1]-> block
// 最終段はハフマンからレンジコーダーに換装 (旧ハフマン版は下にコメントで保持)。
// ==========================================================================
static std::vector<uint8_t> EncodeBlock(const std::vector<uint8_t>& block) {
    std::vector<uint8_t> x = Encode_BWT(block);
    x = Encode_MTF(x);
    x = Encode_RLE(x);
    x = Encode_Entropy(x);        // 0次/1次の小さい方 (旧: Encode_Huffman)
    return x;
}
static std::vector<uint8_t> DecodeBlock(const std::vector<uint8_t>& chunk) {
    std::vector<uint8_t> x = Decode_Entropy(chunk);      // 旧: Decode_Huffman(chunk);
    x = Decode_RLE(x);
    x = Decode_MTF(x);
    x = Decode_BWT(x);
    return x;
}

// ==========================================================================
// ブロック分割パイプライン (最大 900KiB / ブロック)
//
//   コンテナ形式:
//     [uint32 LE] blockCount
//     各ブロック: [uint32 LE] chunkLen, 続けて chunkLen バイトの chunk
//
//   各ブロックは完全に独立して圧縮・復元される。メモリ使用量と速度
//   (BWT の O(n log^2 n)) をブロック単位に抑え、大容量ファイルでも安定する。
// ==========================================================================
static const size_t kBlockSize = 900 * 1024;     // 921600 bytes

static void PutU32(std::vector<uint8_t>& v, uint32_t x) {
    v.push_back(static_cast<uint8_t>(x & 0xFF));
    v.push_back(static_cast<uint8_t>((x >> 8) & 0xFF));
    v.push_back(static_cast<uint8_t>((x >> 16) & 0xFF));
    v.push_back(static_cast<uint8_t>((x >> 24) & 0xFF));
}
static uint32_t GetU32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0])
         | (static_cast<uint32_t>(p[1]) << 8)
         | (static_cast<uint32_t>(p[2]) << 16)
         | (static_cast<uint32_t>(p[3]) << 24);
}

std::vector<uint8_t> Pipeline_Encode(const std::vector<uint8_t>& input) {
    std::vector<uint8_t> out;
    const size_t n = input.size();
    uint32_t blockCount = static_cast<uint32_t>((n + kBlockSize - 1) / kBlockSize);

    PutU32(out, blockCount);
    for (size_t off = 0; off < n; off += kBlockSize) {
        size_t len = std::min(kBlockSize, n - off);
        std::vector<uint8_t> block(input.begin() + off, input.begin() + off + len);
        std::vector<uint8_t> chunk = EncodeBlock(block);
        PutU32(out, static_cast<uint32_t>(chunk.size()));
        out.insert(out.end(), chunk.begin(), chunk.end());
    }
    return out;
}

std::vector<uint8_t> Pipeline_Decode(const std::vector<uint8_t>& input) {
    std::vector<uint8_t> out;
    if (input.size() < 4) return out;              // ヘッダ未満

    size_t pos = 0;
    uint32_t blockCount = GetU32(&input[pos]); pos += 4;

    for (uint32_t b = 0; b < blockCount; ++b) {
        if (pos + 4 > input.size()) break;         // 防御的境界チェック
        uint32_t chunkLen = GetU32(&input[pos]); pos += 4;
        if (pos + chunkLen > input.size()) break;
        std::vector<uint8_t> chunk(input.begin() + pos, input.begin() + pos + chunkLen);
        pos += chunkLen;
        std::vector<uint8_t> block = DecodeBlock(chunk);
        out.insert(out.end(), block.begin(), block.end());
    }
    return out;
}

// ==========================================================================
// ファイル入出力ヘルパ
// ==========================================================================
static bool ReadFile(const std::string& path, std::vector<uint8_t>& out) {
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs) return false;
    ifs.seekg(0, std::ios::end);
    std::streamoff size = ifs.tellg();
    ifs.seekg(0, std::ios::beg);
    out.resize(static_cast<size_t>(size));
    if (size > 0) ifs.read(reinterpret_cast<char*>(out.data()), size);
    return static_cast<bool>(ifs);
}

static bool WriteFile(const std::string& path, const std::vector<uint8_t>& data) {
    std::ofstream ofs(path, std::ios::binary | std::ios::trunc);
    if (!ofs) return false;
    if (!data.empty())
        ofs.write(reinterpret_cast<const char*>(data.data()), data.size());
    return static_cast<bool>(ofs);
}

// std::filesystem::path 版 (Unicode ファイル名でも安全に開ける)
static bool ReadFileFs(const std::filesystem::path& path, std::vector<uint8_t>& out) {
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs) return false;
    ifs.seekg(0, std::ios::end);
    std::streamoff size = ifs.tellg();
    ifs.seekg(0, std::ios::beg);
    out.resize(static_cast<size_t>(size));
    if (size > 0) ifs.read(reinterpret_cast<char*>(out.data()), size);
    return static_cast<bool>(ifs);
}
static bool WriteFileFs(const std::filesystem::path& path, const std::vector<uint8_t>& data) {
    if (path.has_parent_path()) {
        std::error_code ec;
        std::filesystem::create_directories(path.parent_path(), ec);
    }
    std::ofstream ofs(path, std::ios::binary | std::ios::trunc);
    if (!ofs) return false;
    if (!data.empty())
        ofs.write(reinterpret_cast<const char*>(data.data()), data.size());
    return static_cast<bool>(ofs);
}

// ==========================================================================
// 最前段: アーカイブ層 (複数ファイル <-> 1 バッファ)
//
//   新フォーマット 'ARC1': ファイルごとに圧縮方式を選び、個別に圧縮して格納する。
//   (旧方式の「アーカイブ全体を 1 回パイプライン」から、ファイル単位ルーティングへ)
//
//   [4 byte ] magic "ARC1"
//   [uint32 ] fileCount
//   fileCount 回:
//     [uint32] filenameLength
//     [bytes ] filename (UTF-8)
//     [uint8 ] algoFlag         圧縮方式 (下記)
//     [uint64] originalSize     非圧縮サイズ
//     [uint64] compressedSize   格納している圧縮データのサイズ
//     [bytes ] compressed data  (compressedSize バイト)
//
//   compress  : 各ファイル -> RouteByExtension -> CompressOne -> 格納
//   decompress: 各エントリ -> DecompressOne(algo) -> 元ファイル
// ==========================================================================

// ---- 圧縮方式フラグ ----
static const uint8_t ALGO_BWT    = 0x00;   // BWT パイプライン
static const uint8_t ALGO_LZSS   = 0x01;   // LZSS 最適構文解析 -> Huffman
static const uint8_t ALGO_DELTA1 = 0x02;   // Delta(stride=1) -> LZSS -> Huffman
static const uint8_t ALGO_DELTA2 = 0x03;   // Delta(stride=2) -> LZSS -> Huffman
static const uint8_t ALGO_DELTA3 = 0x04;   // Delta(stride=3) -> LZSS -> Huffman
static const uint8_t ALGO_DELTA4 = 0x05;   // Delta(stride=4) -> LZSS -> Huffman
static const uint8_t ALGO_BCJ    = 0x06;   // BCJ(x86) -> LZSS -> Huffman (.exe 向け)
static const uint8_t ALGO_WAV    = 0x07;   // WAV(Mid/Side+Delta) -> LZSS -> Huffman
static const uint8_t ALGO_BMP    = 0x08;   // BMP(2D 予測フィルタ) -> LZSS -> Huffman
static const uint8_t ALGO_RAW    = 0x09;   // 生データを直接 Entropy(0次/1次) で符号化
static const uint8_t ALGO_CM     = 0x0A;   // コンテキストミキシング (生データ直接, テキスト/exe 向け)
static const uint8_t ALGO_BCJ_CM = 0x0B;   // BCJ(x86) -> CM (.exe 向け)
static const uint8_t ALGO_WAV_CM = 0x0C;   // WAV(Mid/Side+LPC) 残差 -> CM (音声向け)
static const uint8_t ALGO_BMP_CM  = 0x0D;   // BMP(2D 予測フィルタ) 残差 -> CM (画像向け)
static const uint8_t ALGO_BMP_CM2 = 0x0E;  // BMP(2D 予測) 残差 + チャンネル分離 -> CM
static const uint8_t ALGO_STORE  = 0xFE;   // 無圧縮で格納
// 0x02..0x05 の stride は (algo - ALGO_LZSS) で求まる (0x02->1 ... 0x05->4)

// アーカイブに格納する 1 ファイル分 (data は「圧縮後」のバイト列)
struct StoredFile {
    std::string name;                  // UTF-8 の相対パス
    uint8_t algo = ALGO_BWT;           // 使用した圧縮方式
    uint64_t originalSize = 0;         // 非圧縮サイズ
    std::vector<uint8_t> data;         // 圧縮後データ
};

static const char ARCHIVE_MAGIC[4] = {'A', 'R', 'C', '1'};

static void PutU64(std::vector<uint8_t>& v, uint64_t x) {
    for (int i = 0; i < 8; ++i) v.push_back(static_cast<uint8_t>((x >> (8 * i)) & 0xFF));
}
static uint64_t GetU64(const uint8_t* p) {
    uint64_t x = 0;
    for (int i = 0; i < 8; ++i) x |= static_cast<uint64_t>(p[i]) << (8 * i);
    return x;
}

// fs::path <-> UTF-8 文字列 (日本語ファイル名も正しく往復する)
static std::string PathToUtf8(const std::filesystem::path& p) {
    std::u8string u8 = p.generic_u8string();
    return std::string(u8.begin(), u8.end());
}
static std::filesystem::path Utf8ToPath(const std::string& s) {
    return std::filesystem::path(std::u8string(s.begin(), s.end()));
}

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
static const int CM_RATE_SLOW[16] = {
    43690, 26214, 18724, 14563, 11915, 10082, 8738, 7710,
     6898,  6241,  5461,  4681,  4096,  3500, 3100, 2849
};
static const int CM_RATE_FAST[16] = {   // exe (BCJ_CM) 用: 速い床
    43690, 26214, 18724, 15000, 15000, 15000, 15000, 15000,
    15000, 15000, 15000, 15000, 15000, 15000, 15000, 15000
};
static const int CM_RATE_WAV[16] = {    // 音声 (WAV_CM) 用: やや遅い床 (残差は exe より定常)
    43690, 26214, 18724, 14563, 11915, 10082, 9362, 8192,
     8192,  8192,  8192,  8192,  8192,  8192, 8192, 8192
};
// ファイル種別ごとの CM パラメータ束 (rate プロファイル + ミキサー学習シフト)。
// algo バイト由来で決まるので encode/decode で一致し完全可逆。
// subShift: order-2/3/4 sub-mixer 文脈のハッシュ右シフト。小さいほど文脈が細かい
// (>>21=16384文脈, >>24=2048文脈)。データ量の多い exe は細かい方が良いが小ファイルは粗い方が良い。
// strideLen: スパース文脈の刻み幅。テキスト UTF-8 は 3、x86 exe は dword 整列の 4。
// tbits: 文脈テーブル t2..t9 のサイズ指数 (1<<tbits)。データ豊富な exe のみ 28 (4.3GB)。
struct CMProfile { const int* rate; int mixShift; int apmShift; int subShift; int strideLen; int tbits; };
static const CMProfile CM_PROF_SLOW { CM_RATE_SLOW, 11, 8, 24, 2, 27 };   // テキスト (CM)
static const CMProfile CM_PROF_BMP  { CM_RATE_SLOW, 12, 8, 24, 3, 27 };   // 画像 (BMP_CM)
static const CMProfile CM_PROF_FAST { CM_RATE_FAST, 10, 7, 15, 2, 28 };   // exe (BCJ_CM)
static const CMProfile CM_PROF_WAV  { CM_RATE_WAV,  11, 7, 24, 2, 27 };   // 音声 (WAV_CM)

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

    CMModel(const CMProfile& prof = CM_PROF_SLOW)
              : TBITS(prof.tbits), TSIZE(1 << prof.tbits), TMASK((1 << prof.tbits) - 1),
                t0(512, 32768), t1(256 * 512, 32768), t2(TSIZE, 32768), t3(TSIZE, 32768),
                t4(TSIZE, 32768), t5(TSIZE, 32768), t6(TSIZE, 32768), t7(TSIZE, 32768),
                t8(TSIZE, 32768), t9(TSIZE, 32768), matchTab(SM, 0), matchTab2(SM, 0), matchTab3(SM, 0), w(8192 * NIN, 1 << 14), w2(1048576 * NIN, 1 << 14), w3(1048576 * NIN, 1 << 14), w4(1048576 * NIN, 1 << 14), wf(64 * NMIX, 16384),
                apm(32768 * 65), apm2(4096 * 65), apm3(16384 * 65), apm4(524288 * 65) {
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

std::vector<uint8_t> Encode_CM(const std::vector<uint8_t>& input, const CMProfile& prof = CM_PROF_SLOW) {
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
std::vector<uint8_t> Decode_CM(const std::vector<uint8_t>& input, const CMProfile& prof = CM_PROF_SLOW) {
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

static void LzPutLEB(std::vector<uint8_t>& v, uint32_t x) {
    do { uint8_t b = static_cast<uint8_t>(x & 0x7F); x >>= 7; if (x) b |= 0x80; v.push_back(b); } while (x);
}
static uint32_t LzGetLEB(const uint8_t* data, size_t& i) {
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
static const int LPC_ORDER = 16;       // 予測次数 (8->16)
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

std::vector<uint8_t> Encode_Wav_MidSide_Delta(const std::vector<uint8_t>& in) {
    std::vector<uint8_t> out;
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
        side[i] = sd;
        mid[i]  = static_cast<uint16_t>(R + (sd >> 1));
    }

    // ブロック別 (連続履歴) の FLAC 固定予測 (0〜4 次)。すべて uint16 ラップで可逆。
    const size_t BS = 8192;                              // フレーム/ブロック
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

    // 各フレームを所属ブロックの手法で予測 -> バイトプレーン分離 (履歴は連続)
    std::vector<uint8_t> midLo(frames), midHi(frames), sideLo(frames), sideHi(frames);
    for (size_t i = 0; i < frames; ++i) {
        size_t b = i / BS;
        uint16_t mpred, spred;
        if (midMethod[b] == WAV_METHOD_LPC) mpred = static_cast<uint16_t>(WavLpcPredict(mid.data(), i, &midCoef[b * LPC_ORDER], midShift[b]));
        else { uint16_t p1=(i>=1)?mid[i-1]:0,p2=(i>=2)?mid[i-2]:0,p3=(i>=3)?mid[i-3]:0,p4=(i>=4)?mid[i-4]:0; mpred = WavPredict(midMethod[b],p1,p2,p3,p4); }
        if (sideMethod[b] == WAV_METHOD_LPC) spred = static_cast<uint16_t>(WavLpcPredict(side.data(), i, &sideCoef[b * LPC_ORDER], sideShift[b]));
        else { uint16_t p1=(i>=1)?side[i-1]:0,p2=(i>=2)?side[i-2]:0,p3=(i>=3)?side[i-3]:0,p4=(i>=4)?side[i-4]:0; spred = WavPredict(sideMethod[b],p1,p2,p3,p4); }
        uint16_t mr = static_cast<uint16_t>(mid[i]  - mpred);
        uint16_t sr = static_cast<uint16_t>(side[i] - spred);
        midLo[i]  = static_cast<uint8_t>(mr);       midHi[i]  = static_cast<uint8_t>(mr >> 8);
        sideLo[i] = static_cast<uint8_t>(sr);       sideHi[i] = static_cast<uint8_t>(sr >> 8);
    }
    // Lo系→Hi系の順: sideLo が midLo の文脈を活用, sideHi が midHi の文脈を活用
    out.insert(out.end(), midLo.begin(),  midLo.end());
    out.insert(out.end(), sideLo.begin(), sideLo.end());
    out.insert(out.end(), midHi.begin(),  midHi.end());
    out.insert(out.end(), sideHi.begin(), sideHi.end());
    return out;
}

std::vector<uint8_t> Decode_Wav_MidSide_Delta(const std::vector<uint8_t>& in) {
    size_t pos = 0;
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
    const uint8_t* midLo  = in.data() + pos;
    const uint8_t* sideLo = midLo  + frames;
    const uint8_t* midHi  = sideLo + frames;
    const uint8_t* sideHi = midHi  + frames;

    // 残差から各チャンネル値を逆予測で復元 (ブロックごとに手法切替, 履歴は連続)
    std::vector<uint16_t> mid(frames), side(frames);
    for (uint32_t i = 0; i < frames; ++i) {
        size_t b = i / blockSize;
        uint8_t mMethod = methods[2 * b], sMethod = methods[2 * b + 1];
        uint16_t mr = static_cast<uint16_t>(midLo[i]  | (midHi[i]  << 8));
        uint16_t sr = static_cast<uint16_t>(sideLo[i] | (sideHi[i] << 8));
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
        uint16_t R = static_cast<uint16_t>(mid[i] - (side[i] >> 1));
        uint16_t L = static_cast<uint16_t>(side[i] + R);
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
static std::vector<uint8_t> BmpSeparateChannels(const std::vector<uint8_t>& in) {
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
static std::vector<uint8_t> BmpJoinChannels(const std::vector<uint8_t>& in) {
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
static std::vector<uint8_t> CompressOne(uint8_t algo, const std::vector<uint8_t>& in) {
    switch (algo) {
        // LZSS 系は CompressLZSS (単一ストリーム/スプリットの小さい方)
        case ALGO_STORE: return in;                        // そのまま
        case ALGO_BWT:   return Pipeline_Encode(in);       // BWT 4 段 (内部で Entropy)
        case ALGO_LZSS:  return CompressLZSS(in);
        case ALGO_DELTA1: case ALGO_DELTA2:
        case ALGO_DELTA3: case ALGO_DELTA4:                 // Delta(stride) -> LZSS(split可)
            return CompressLZSS(Encode_Delta(in, algo - ALGO_LZSS));
        case ALGO_BCJ:                                       // BCJ -> LZSS(split可)
            return CompressLZSS(Encode_BCJ(in));
        case ALGO_WAV:                                        // WAV(Mid/Side+Delta) -> LZSS(split可)
            return CompressLZSS(Encode_Wav_MidSide_Delta(in));
        case ALGO_BMP:                                        // BMP(2D 予測) -> LZSS(split可)
            return CompressLZSS(Encode_Bmp_2DPredict(in));
        case ALGO_RAW:                                         // 生データ -> Entropy(0次/1次) 直掛け
            return Encode_Entropy(in);
        case ALGO_CM:                                          // 生データ -> コンテキストミキシング
            return Encode_CM(in);
        case ALGO_BCJ_CM:                                      // BCJ -> CM (非定常: FAST プロファイル)
            return Encode_CM(Encode_BCJ(in), CM_PROF_FAST);
        case ALGO_WAV_CM:                                      // WAV 残差 -> CM (WAV プロファイル)
            return Encode_CM(Encode_Wav_MidSide_Delta(in), CM_PROF_WAV);
        case ALGO_BMP_CM:                                      // BMP 残差 -> CM (BMP プロファイル)
            return Encode_CM(Encode_Bmp_2DPredict(in), CM_PROF_BMP);
        case ALGO_BMP_CM2:                                     // BMP 残差 + チャンネル分離 -> CM
            return Encode_CM(BmpSeparateChannels(Encode_Bmp_2DPredict(in)));
        default:         return in;
    }
}
static std::vector<uint8_t> DecompressOne(uint8_t algo, const std::vector<uint8_t>& in,
                                          uint64_t /*originalSize*/) {
    switch (algo) {
        // LZSS 系は DecompressLZSS (タグで単一/スプリットを判別)
        case ALGO_STORE: return in;
        case ALGO_BWT:   return Pipeline_Decode(in);
        case ALGO_LZSS:  return DecompressLZSS(in);
        case ALGO_DELTA1: case ALGO_DELTA2:
        case ALGO_DELTA3: case ALGO_DELTA4:                 // 逆順: LZSS -> Delta
            return Decode_Delta(DecompressLZSS(in), algo - ALGO_LZSS);
        case ALGO_BCJ:                                       // 逆順: LZSS -> BCJ
            return Decode_BCJ(DecompressLZSS(in));
        case ALGO_WAV:                                        // 逆順: LZSS -> WAV
            return Decode_Wav_MidSide_Delta(DecompressLZSS(in));
        case ALGO_BMP:                                        // 逆順: LZSS -> BMP
            return Decode_Bmp_2DPredict(DecompressLZSS(in));
        case ALGO_RAW:                                         // Entropy 直掛けの逆
            return Decode_Entropy(in);
        case ALGO_CM:                                          // CM 直掛けの逆
            return Decode_CM(in);
        case ALGO_BCJ_CM:                                      // 逆順: CM -> BCJ (FAST プロファイル)
            return Decode_BCJ(Decode_CM(in, CM_PROF_FAST));
        case ALGO_WAV_CM:                                      // 逆順: CM -> WAV (WAV プロファイル)
            return Decode_Wav_MidSide_Delta(Decode_CM(in, CM_PROF_WAV));
        case ALGO_BMP_CM:                                      // 逆順: CM -> BMP (BMP プロファイル)
            return Decode_Bmp_2DPredict(Decode_CM(in, CM_PROF_BMP));
        case ALGO_BMP_CM2:                                     // 逆順: CM -> チャンネル結合 -> BMP
            return Decode_Bmp_2DPredict(BmpJoinChannels(Decode_CM(in)));
        default:         return in;
    }
}

// トーナメントで試す方式一覧
static const uint8_t kTournamentAlgos[] = {
    ALGO_STORE, ALGO_BWT, ALGO_LZSS,
    ALGO_DELTA1, ALGO_DELTA2, ALGO_DELTA3, ALGO_DELTA4,
    ALGO_BCJ, ALGO_WAV, ALGO_BMP, ALGO_RAW, ALGO_CM,
    ALGO_BCJ_CM, ALGO_WAV_CM, ALGO_BMP_CM, ALGO_BMP_CM2
};

// ---- コンテナの構築 / 解析 (新フォーマット 'ARC1') ----
std::vector<uint8_t> BuildArchive(const std::vector<StoredFile>& files) {
    std::vector<uint8_t> out;
    out.insert(out.end(), ARCHIVE_MAGIC, ARCHIVE_MAGIC + 4);
    PutU32(out, static_cast<uint32_t>(files.size()));
    for (const auto& f : files) {
        PutU32(out, static_cast<uint32_t>(f.name.size()));
        out.insert(out.end(), f.name.begin(), f.name.end());
        out.push_back(f.algo);
        PutU64(out, f.originalSize);
        PutU64(out, static_cast<uint64_t>(f.data.size()));
        out.insert(out.end(), f.data.begin(), f.data.end());
    }
    return out;
}

bool ParseArchive(const std::vector<uint8_t>& buf, std::vector<StoredFile>& out) {
    out.clear();
    size_t pos = 0;
    auto have = [&](size_t k) { return pos + k <= buf.size(); };

    if (!have(4)) return false;
    if (std::memcmp(&buf[0], ARCHIVE_MAGIC, 4) != 0) return false;   // マジック検証
    pos += 4;

    if (!have(4)) return false;
    uint32_t count = GetU32(&buf[pos]); pos += 4;

    for (uint32_t i = 0; i < count; ++i) {
        if (!have(4)) return false;
        uint32_t nlen = GetU32(&buf[pos]); pos += 4;
        if (!have(nlen)) return false;
        std::string name(reinterpret_cast<const char*>(&buf[pos]), nlen); pos += nlen;

        if (!have(1)) return false;
        uint8_t algo = buf[pos]; pos += 1;
        if (!have(8)) return false;
        uint64_t origSize = GetU64(&buf[pos]); pos += 8;
        if (!have(8)) return false;
        uint64_t compSize = GetU64(&buf[pos]); pos += 8;
        if (!have(static_cast<size_t>(compSize))) return false;

        StoredFile e;
        e.name = std::move(name);
        e.algo = algo;
        e.originalSize = origSize;
        e.data.assign(buf.begin() + pos, buf.begin() + pos + static_cast<size_t>(compSize));
        pos += static_cast<size_t>(compSize);
        out.push_back(std::move(e));
    }
    return true;
}

// ==========================================================================
// フォルダ一括 圧縮 / 復元
// ==========================================================================
namespace fs = std::filesystem;

static const char* AlgoName(uint8_t algo) {
    switch (algo) {
        case ALGO_BWT:    return "BWT";
        case ALGO_LZSS:   return "LZSS";
        case ALGO_DELTA1: return "Delta1+LZSS";
        case ALGO_DELTA2: return "Delta2+LZSS";
        case ALGO_DELTA3: return "Delta3+LZSS";
        case ALGO_DELTA4: return "Delta4+LZSS";
        case ALGO_BCJ:    return "BCJ+LZSS";
        case ALGO_WAV:    return "WAV+LZSS";
        case ALGO_BMP:    return "BMP+LZSS";
        case ALGO_RAW:    return "Range(o0/o1)";
        case ALGO_CM:     return "CM";
        case ALGO_BCJ_CM: return "BCJ+CM";
        case ALGO_WAV_CM: return "WAV+CM";
        case ALGO_BMP_CM:  return "BMP+CM";
        case ALGO_BMP_CM2: return "BMP+CM(sep)";
        case ALGO_STORE:  return "Store";
        default:          return "?";
    }
}

// 1 ファイルに対し全方式を試し、最小サイズの方式を採用する (トーナメント)
static StoredFile CompressFileTournament(const std::string& name,
                                         const std::vector<uint8_t>& raw) {
    StoredFile best;
    best.name = name;
    best.originalSize = raw.size();
    best.algo = ALGO_STORE;
    best.data = raw;                       // Store を初期ベスト (膨張しない保証)

    for (uint8_t algo : kTournamentAlgos) {
        if (algo == ALGO_STORE) continue;  // Store は初期値で済み
        std::vector<uint8_t> blob = CompressOne(algo, raw);
        if (blob.size() < best.data.size()) {
            best.data = std::move(blob);
            best.algo = algo;
        }
    }
    return best;
}

// 入力フォルダを再帰スキャン -> 各ファイルをトーナメントで圧縮 -> 1 ファイル出力
bool CompressFolder(const fs::path& inputDir, const fs::path& outputFile) {
    if (!fs::exists(inputDir) || !fs::is_directory(inputDir)) {
        std::cout << "[ERROR] 入力フォルダがありません: " << inputDir.string() << "\n";
        return false;
    }

    std::vector<StoredFile> files;
    uint64_t totalOrig = 0;
    for (const auto& de : fs::recursive_directory_iterator(inputDir)) {
        if (!de.is_regular_file()) continue;

        std::vector<uint8_t> raw;
        if (!ReadFileFs(de.path(), raw)) {
            std::cout << "[ERROR] 読込失敗: " << de.path().string() << "\n";
            return false;
        }

        std::string name = PathToUtf8(fs::relative(de.path(), inputDir));
        StoredFile e = CompressFileTournament(name, raw);
        totalOrig += raw.size();

        double saved = raw.size() ? 100.0 - 100.0 * e.data.size() / raw.size() : 0.0;
        std::printf("  %-24s %9zu -> %9zu B  [%-11s] Saved %5.1f%%\n",
                    name.c_str(), raw.size(), e.data.size(), AlgoName(e.algo), saved);
        files.push_back(std::move(e));
    }

    std::vector<uint8_t> archive = BuildArchive(files);

    if (!WriteFileFs(outputFile, archive)) {
        std::cout << "[ERROR] 出力書き込み失敗: " << outputFile.string() << "\n";
        return false;
    }

    std::cout << "compress : " << files.size() << " files, "
              << "orig=" << totalOrig << " B -> "
              << "archive=" << archive.size() << " B -> " << outputFile.string() << "\n";
    if (totalOrig > 0) {
        double ratio = 100.0 * archive.size() / totalOrig;   // 圧縮後割合
        double saved = 100.0 - ratio;                         // 削減率
        std::printf("           Saved space: %.1f%% (Ratio: %.1f%%)\n", saved, ratio);
    }
    return true;
}

// 1 ファイル読込 -> ParseArchive -> エントリ毎に DecompressOne -> 出力フォルダへ全展開
bool DecompressArchive(const fs::path& inputFile, const fs::path& outputDir) {
    std::vector<uint8_t> archive;
    if (!ReadFileFs(inputFile, archive)) {
        std::cout << "[ERROR] 入力を開けません: " << inputFile.string() << "\n";
        return false;
    }

    std::vector<StoredFile> files;
    if (!ParseArchive(archive, files)) {
        std::cout << "[ERROR] アーカイブの解析に失敗しました (マジック不一致/破損)\n";
        return false;
    }

    for (const auto& f : files) {
        std::vector<uint8_t> raw = DecompressOne(f.algo, f.data, f.originalSize);
        if (raw.size() != f.originalSize) {
            std::cout << "[ERROR] サイズ不一致 (" << f.name << "): "
                      << raw.size() << " != " << f.originalSize << "\n";
            return false;
        }
        fs::path outPath = outputDir / Utf8ToPath(f.name);
        if (!WriteFileFs(outPath, raw)) {
            std::cout << "[ERROR] 復元書き込み失敗: " << outPath.string() << "\n";
            return false;
        }
    }

    std::cout << "decompress: " << files.size() << " files -> "
              << outputDir.string() << "\n";
    return true;
}

// ==========================================================================
// 内蔵セルフテスト (アルゴリズムの正しさ確認)
// ==========================================================================
static std::vector<uint8_t> Str(const std::string& s) {
    return std::vector<uint8_t>(s.begin(), s.end());
}

// 1 ケースを「指定した enc/dec 関数ペア」で検証する汎用テスト
template <class Enc, class Dec>
static bool RunCase(const std::string& name, const std::vector<uint8_t>& data,
                    Enc enc, Dec dec) {
    std::vector<uint8_t> e = enc(data);
    std::vector<uint8_t> d = dec(e);
    bool ok = (d == data);
    std::cout << (ok ? "[OK]   " : "[FAIL] ")
              << name << "  (size=" << data.size() << ")\n";
    return ok;
}

// 各段共通で使うテストデータ群に対して enc/dec を検証
template <class Enc, class Dec>
static bool RunSuite(const std::string& title, Enc enc, Dec dec) {
    std::cout << "--- " << title << " ---\n";
    bool all = true;
    all &= RunCase("abracadabra", Str("abracadabra"), enc, dec);
    all &= RunCase("empty",       Str(""),            enc, dec);
    all &= RunCase("single",      Str("A"),           enc, dec);
    all &= RunCase("all same",    std::vector<uint8_t>(1000, 0x41), enc, dec);
    {
        std::vector<uint8_t> v = {0x00, 0x01, 0x00, 0xFF, 0x00};
        all &= RunCase("NUL bytes", v, enc, dec);
    }
    {
        std::mt19937 rng(12345);
        std::uniform_int_distribution<int> byte(0, 255);
        std::vector<uint8_t> v(65536);
        for (auto& b : v) b = static_cast<uint8_t>(byte(rng));
        all &= RunCase("random 65536", v, enc, dec);
    }
    return all;
}

static bool RunSelfTests() {
    std::cout << "=== self tests ===\n";
    bool all = true;
    all &= RunSuite("BWT",                  Encode_BWT,      Decode_BWT);
    all &= RunSuite("MTF",                  Encode_MTF,      Decode_MTF);
    all &= RunSuite("RLE",                  Encode_RLE,      Decode_RLE);
    all &= RunSuite("Huffman",              Encode_Huffman,      Decode_Huffman);
    all &= RunSuite("RangeCoder o0",        Encode_RangeCoder,   Decode_RangeCoder);
    all &= RunSuite("RangeCoder o1",        Encode_RangeCoderO1, Decode_RangeCoderO1);
    all &= RunSuite("RangeCoder o2",        Encode_RangeCoderO2, Decode_RangeCoderO2);
    all &= RunSuite("CM",
                    [](const std::vector<uint8_t>& v){ return Encode_CM(v); },
                    [](const std::vector<uint8_t>& v){ return Decode_CM(v); });
    all &= RunSuite("Entropy(o0/o1)",       Encode_Entropy,      Decode_Entropy);
    all &= RunSuite("LZSS",                 Encode_LZSS_Greedy,  Decode_LZSS);
    all &= RunSuite("LZSS-opt",             Encode_LZSS_Optimal, Decode_LZSS);
    all &= RunSuite("LZSS-split",           Encode_LZSS_Split,   Decode_LZSS_Split);
    all &= RunSuite("LZSS-4stream",         Encode_LZSS_4Stream, Decode_LZSS_4Stream);
    for (int s = 1; s <= 4; ++s) {
        all &= RunSuite("Delta stride=" + std::to_string(s),
                        [s](const std::vector<uint8_t>& v) { return Encode_Delta(v, s); },
                        [s](const std::vector<uint8_t>& v) { return Decode_Delta(v, s); });
    }
    all &= RunSuite("BCJ",                  Encode_BCJ,          Decode_BCJ);
    all &= RunSuite("WAV mid/side",         Encode_Wav_MidSide_Delta, Decode_Wav_MidSide_Delta);
    all &= RunSuite("BMP 2D filter",        Encode_Bmp_2DPredict,     Decode_Bmp_2DPredict);
    all &= RunSuite("full pipe (B+M+R+H)",  Pipeline_Encode, Pipeline_Decode);

    // マルチブロック (>900KiB) の検証: 圧縮しやすい部分と乱雑な部分を混在
    {
        std::cout << "--- multi-block pipeline ---\n";
        std::mt19937 rng(2024);
        std::uniform_int_distribution<int> byte(0, 255);
        std::vector<uint8_t> big;
        big.reserve(2'300'000);
        while (big.size() < 2'300'000) {
            // 反復しやすいパターン
            for (int k = 0; k < 5000; ++k) big.push_back(static_cast<uint8_t>(k & 7));
            // 乱雑なパターン
            for (int k = 0; k < 3000; ++k) big.push_back(static_cast<uint8_t>(byte(rng)));
        }
        std::vector<uint8_t> enc = Pipeline_Encode(big);
        std::vector<uint8_t> dec = Pipeline_Decode(enc);
        uint32_t blocks = (enc.size() >= 4) ? GetU32(enc.data()) : 0;
        bool ok = (dec == big);
        std::cout << (ok ? "[OK]   " : "[FAIL] ")
                  << "multi-block  (size=" << big.size()
                  << ", blocks=" << blocks
                  << ", encoded=" << enc.size() << ")\n";
        all &= ok;
    }

    std::cout << (all ? "self tests PASSED\n" : "self tests FAILED\n");
    return all;
}

// ==========================================================================
// テスト用: data/ にダミーファイル群を用意する (存在しない/再生成)
// ==========================================================================
static void PrepareDummyData(const fs::path& dir) {
    std::error_code ec;
    fs::remove_all(dir, ec);             // 毎回まっさらにして再現性を確保
    fs::create_directories(dir, ec);

    // 1) テキスト (繰り返しが多く圧縮しやすい)
    {
        std::string s;
        for (int i = 0; i < 2000; ++i)
            s += "The quick brown fox jumps over the lazy dog. ";
        WriteFileFs(dir / "lorem.txt", Str(s));
    }
    // 2) 短いテキスト
    WriteFileFs(dir / "hello.txt", Str("Hello, Archive!\nこんにちは、アーカイブ！\n"));

    // 3) バイナリ (ゼロ連 + 擬似乱数を混在)
    {
        std::vector<uint8_t> v;
        std::mt19937 rng(7);
        std::uniform_int_distribution<int> byte(0, 255);
        for (int blk = 0; blk < 200; ++blk) {
            for (int z = 0; z < 50; ++z) v.push_back(0x00);              // ゼロ連
            for (int r = 0; r < 30; ++r) v.push_back((uint8_t)byte(rng)); // 乱数
        }
        WriteFileFs(dir / "binary.dat", v);
    }
    // 4) サブフォルダ内のファイル (相対パス保持の検証)
    WriteFileFs(dir / "sub" / "note.txt", Str("nested file in subfolder\n"));

    // 5) Store ルート確認用: 既圧縮を模した拡張子 (.jpg / .zip) に乱雑なバイト列
    {
        std::vector<uint8_t> v(8000);
        std::mt19937 rng(55);
        std::uniform_int_distribution<int> byte(0, 255);
        for (auto& b : v) b = static_cast<uint8_t>(byte(rng));
        WriteFileFs(dir / "already.jpg", v);
        WriteFileFs(dir / "pack.zip",    v);
    }

    // 6) LZSS ルート確認用: 反復の多いバイナリ (.exe)
    {
        std::vector<uint8_t> v;
        std::mt19937 rng(99);
        std::uniform_int_distribution<int> byte(0, 255);
        const char* phrase = "MZ_program_text_section_data_";
        for (int i = 0; i < 4000; ++i) {
            for (const char* q = phrase; *q; ++q) v.push_back(static_cast<uint8_t>(*q));
            if (i % 9 == 0)                              // たまにノイズを混ぜる
                for (int r = 0; r < 16; ++r) v.push_back(static_cast<uint8_t>(byte(rng)));
        }
        WriteFileFs(dir / "dummy.exe", v);
    }

    // 7) 実画像があれば取り込む (TEST/pika.bmp -> BWT ルート)
    for (const char* cand : {"TEST/pika.bmp", "TEST/pika256.bmp"}) {
        fs::path src(cand);
        if (fs::exists(src)) { fs::copy_file(src, dir / src.filename(), fs::copy_options::overwrite_existing, ec); break; }
    }

    // 8) 実 exe があれば取り込む (data/ は読むだけ、LZSS ルートの実データ検証)
    {
        fs::path src("data/TeraPad.exe");
        if (fs::exists(src)) fs::copy_file(src, dir / "TeraPad_copy.exe", fs::copy_options::overwrite_existing, ec);
    }

    // 9) 実 WAV/BMP があれば取り込む (Delta+LZSS ルートの実データ検証, data/ は読むだけ)
    for (const char* f : {"data/explosion.wav", "data/hal.bmp", "data/yuuki_256.bmp"}) {
        fs::path src(f);
        if (fs::exists(src)) fs::copy_file(src, dir / src.filename(), fs::copy_options::overwrite_existing, ec);
    }
}

// ==========================================================================
// data/ と data_restored/ を全ファイル完全一致比較
// ==========================================================================
static bool VerifyFolders(const fs::path& a, const fs::path& b) {
    bool all = true;
    size_t n = 0;
    for (const auto& de : fs::recursive_directory_iterator(a)) {
        if (!de.is_regular_file()) continue;
        ++n;
        fs::path rel = fs::relative(de.path(), a);
        fs::path other = b / rel;

        std::vector<uint8_t> da, db;
        bool ra = ReadFileFs(de.path(), da);
        bool rb = ReadFileFs(other, db);
        bool ok = ra && rb && (da == db);
        all &= ok;
        std::cout << (ok ? "  [OK]   " : "  [FAIL] ")
                  << PathToUtf8(rel) << "  (" << da.size() << " B)";
        if (!rb) std::cout << "  <- 復元先に存在しません";
        else if (ra && da != db) std::cout << "  <- 内容不一致!!";
        std::cout << "\n";
    }
    std::cout << (all ? "[OK] " : "[FAIL] ") << n << " files matched\n";
    return all;
}

// ==========================================================================
// メイン: data/ を 1 ファイルに圧縮 -> data_restored/ へ復元 -> 完全一致検証
// ==========================================================================
int main(int argc, char** argv) {
#ifdef _WIN32
    SetConsoleOutputCP(65001);   // コンソール出力を UTF-8 に (文字化け対策)
#endif

    // --extract <archive.enc> <outdir/> : アーカイブからファイルを展開する (開発用)
    if (argc >= 4 && std::string(argv[1]) == "--extract") {
        std::vector<uint8_t> archive;
        if (!ReadFileFs(fs::path(argv[2]), archive)) {
            std::cout << "[ERROR] 読込失敗: " << argv[2] << "\n"; return 1;
        }
        std::vector<StoredFile> files;
        if (!ParseArchive(archive, files)) {
            std::cout << "[ERROR] アーカイブ解析失敗\n"; return 1;
        }
        fs::path outDir = argv[3];
        fs::create_directories(outDir);
        for (const auto& f : files) {
            std::vector<uint8_t> raw = DecompressOne(f.algo, f.data, f.originalSize);
            if (raw.size() != f.originalSize) {
                std::cout << "[ERROR] size mismatch for " << f.name << "\n"; return 1;
            }
            fs::path outPath = outDir / Utf8ToPath(f.name);
            fs::create_directories(outPath.parent_path());
            if (!WriteFileFs(outPath, raw)) {
                std::cout << "[ERROR] write fail: " << outPath.string() << "\n"; return 1;
            }
            std::printf("  extracted: %s (%zu B)\n", f.name.c_str(), raw.size());
        }
        std::cout << "done.\n";
        return 0;
    }

    // ---- アルゴリズムのセルフテスト ----
    if (!RunSelfTests()) {
        std::cout << "アルゴリズムのセルフテストに失敗しました。\n";
        return 1;
    }
    std::cout << "\n";

    // ====================================================================
    // 本番データ data/ をトーナメント圧縮 -> data_restored/ へ復元 -> 完全一致検証
    //   data/ は読み取りのみ (PrepareDummyData は呼ばない)。
    // ====================================================================
    const fs::path inputDir   = "data";            // 本番コンテストデータ
    const fs::path outputFile = "output.enc";      // 圧縮済み 1 ファイル
    const fs::path restoreDir = "data_restored";   // 復元先フォルダ
    // --------------------------------------------------------------------

    std::cout << "=== archive round-trip test (tournament) ===\n";
    std::cout << "cwd        : " << fs::current_path().string() << "\n";

    // ---- 1. 圧縮: data/ -> output.enc (各ファイル全方式トーナメント) ----
    if (!CompressFolder(inputDir, outputFile)) return 1;

    // 参考: 7z (data.7z) との比較
    {
        std::error_code ec;
        auto enc = fs::file_size(outputFile, ec);
        const uint64_t kSevenZip = 1640836;   // data.7z の実測サイズ
        if (!ec) {
            std::printf("           vs 7z(data.7z)=%llu B : %s (diff %+lld B)\n",
                        (unsigned long long)kSevenZip,
                        enc < kSevenZip ? "WIN" : "lose",
                        (long long)enc - (long long)kSevenZip);
        }
    }

    // ---- 2. 復元: output.enc -> data_restored/ ----
    {
        std::error_code ec;
        fs::remove_all(restoreDir, ec);            // 前回の残骸を除去
    }
    if (!DecompressArchive(outputFile, restoreDir)) return 1;

    // ---- 3. 完全一致検証 ----
    std::cout << "verify     :\n";
    bool ok = VerifyFolders(inputDir, restoreDir);

    std::cout << "\nround-trip : " << (ok ? "[OK] 全ファイル完全一致" : "[FAIL] 不一致!!") << "\n";
    return ok ? 0 : 1;
}
