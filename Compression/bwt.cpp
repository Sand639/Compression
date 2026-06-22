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

// ==========================================================================
// 1 ブロック分の 4 段パイプライン
//   encode:  block ->[BWT]->[MTF]->[RLE]->[Huffman]-> chunk
//   decode:  chunk ->[Huffman^-1]->[RLE^-1]->[MTF^-1]->[BWT^-1]-> block
// chunk は内部 (BWT/Huffman) ヘッダにより自身の復元サイズを完全自己記述する。
// ==========================================================================
static std::vector<uint8_t> EncodeBlock(const std::vector<uint8_t>& block) {
    std::vector<uint8_t> x = Encode_BWT(block);
    x = Encode_MTF(x);
    x = Encode_RLE(x);
    x = Encode_Huffman(x);
    return x;
}
static std::vector<uint8_t> DecodeBlock(const std::vector<uint8_t>& chunk) {
    std::vector<uint8_t> x = Decode_Huffman(chunk);
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
static const uint8_t ALGO_BWT   = 0x00;   // 既存 BWT パイプライン
static const uint8_t ALGO_LZSS  = 0x01;   // LZSS (貪欲法) — バイナリ向け
static const uint8_t ALGO_STORE = 0xFE;   // 無圧縮で格納 (既圧縮ファイル向け)

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
// LZSS (貪欲法 + ハッシュチェーン探索)
//
//   出力フォーマット:
//     [8 byte] originalSize (uint64 LE)  ← Decode を自己完結にするため
//     [bitstream]
//        flag 0 : リテラル  -> 8bit のバイト
//        flag 1 : 一致      -> 距離(15bit, dist-1) + 長さ(8bit, len-3)
//
//   ウィンドウ 32KiB、最短一致 3、最長一致 258。最適構文解析は使わず貪欲法。
//   一致探索はナイーブだと巨大入力で破綻するため、3 バイトハッシュの
//   チェーンを辿って最長一致を探す (zlib 方式の簡易版)。
// ==========================================================================
static const int LZSS_WINDOW_BITS = 15;
static const int LZSS_WINDOW      = 1 << LZSS_WINDOW_BITS;  // 32768
static const int LZSS_MIN_MATCH   = 3;
static const int LZSS_MAX_MATCH   = 258;
static const int LZSS_LEN_BITS    = 8;                      // 258-3 = 255 -> 8bit
static const int LZSS_HASH_BITS   = 16;
static const int LZSS_HASH_SIZE   = 1 << LZSS_HASH_BITS;
static const int LZSS_MAX_CHAIN   = 4096;                   // 貪欲探索の最大チェーン長

static inline uint32_t LzssHash(const uint8_t* p) {
    uint32_t v = static_cast<uint32_t>(p[0])
               | (static_cast<uint32_t>(p[1]) << 8)
               | (static_cast<uint32_t>(p[2]) << 16);
    return (v * 2654435761u) >> (32 - LZSS_HASH_BITS);
}

std::vector<uint8_t> Encode_LZSS_Greedy(const std::vector<uint8_t>& input) {
    const int n = static_cast<int>(input.size());
    std::vector<uint8_t> out;
    PutU64(out, static_cast<uint64_t>(n));     // 先頭に元サイズ

    BitWriter bw(out);
    if (n == 0) { bw.Flush(); return out; }

    const uint8_t* d = input.data();
    std::vector<int> head(LZSS_HASH_SIZE, -1);
    std::vector<int> prev(n, -1);

    auto insert = [&](int p) {
        if (p + LZSS_MIN_MATCH > n) return;    // ハッシュに 3 バイト必要
        uint32_t h = LzssHash(d + p);
        prev[p] = head[h];
        head[h] = p;
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
                // 既知最長より先のバイトが一致しなければスキップ (高速化)
                if (d[cand + bestLen] == d[pos + bestLen]) {
                    int l = 0;
                    while (l < maxLen && d[cand + l] == d[pos + l]) ++l;
                    if (l > bestLen) {
                        bestLen = l;
                        bestDist = pos - cand;
                        if (l >= maxLen) break;     // これ以上は伸びない
                    }
                }
                cand = prev[cand];
            }
        }

        if (bestLen >= LZSS_MIN_MATCH) {
            bw.PutBit(1);
            bw.PutBits(static_cast<uint32_t>(bestDist - 1), LZSS_WINDOW_BITS);
            bw.PutBits(static_cast<uint32_t>(bestLen - LZSS_MIN_MATCH), LZSS_LEN_BITS);
            int end = pos + bestLen;
            while (pos < end) { insert(pos); ++pos; }   // 一致内の各位置も辞書登録
        } else {
            bw.PutBit(0);
            bw.PutBits(d[pos], 8);
            insert(pos);
            ++pos;
        }
    }
    bw.Flush();
    return out;
}

std::vector<uint8_t> Decode_LZSS(const std::vector<uint8_t>& input) {
    if (input.size() < 8) return {};
    uint64_t n = GetU64(input.data());

    std::vector<uint8_t> out;
    out.reserve(static_cast<size_t>(n));
    BitReader br(input.data() + 8, input.size() - 8);

    while (out.size() < n) {
        if (br.GetBit() == 0) {
            out.push_back(static_cast<uint8_t>(br.GetBits(8)));
        } else {
            uint32_t dist = br.GetBits(LZSS_WINDOW_BITS) + 1;
            uint32_t len  = br.GetBits(LZSS_LEN_BITS) + LZSS_MIN_MATCH;
            size_t start = out.size() - dist;
            for (uint32_t k = 0; k < len; ++k) out.push_back(out[start + k]);  // 重なり対応
        }
    }
    return out;
}

// ==========================================================================
// LZSS 最適構文解析 (Optimal Parsing)
//
//   出力フォーマットは Encode_LZSS_Greedy と完全に同一なので Decode_LZSS で
//   そのまま復元できる。エンコード側の「賢さ」だけを引き上げる。
//
//   手法: 各位置の最長一致を前計算し、前方 DP (最短経路) で
//         「リテラル / 長さ L の一致」の全分岐から推定ビット最小の経路を選ぶ。
//   コスト模型は zopfli 風の反復改善:
//     iter0 はフォーマット実ビット幅 (literal=9, match=24) で最適化、
//     以降はトークン統計からエントロピー推定コストを再計算して再最適化。
//   各反復で実際に Encode_Huffman を通した実サイズを測り、最小のものを採用
//   (greedy も候補に含めるので、貪欲法より悪化することはない)。
// ==========================================================================
std::vector<uint8_t> Encode_LZSS_Optimal(const std::vector<uint8_t>& input) {
    const int n = static_cast<int>(input.size());
    if (n == 0) {
        std::vector<uint8_t> out; PutU64(out, 0); BitWriter bw(out); bw.Flush(); return out;
    }
    const uint8_t* d = input.data();

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

    struct Tok { int len; int dist; };          // len==1 -> リテラル

    // トークン列 -> LZSS バイト列 (フォーマットは Greedy と同一)
    auto emit = [&](const std::vector<Tok>& toks) -> std::vector<uint8_t> {
        std::vector<uint8_t> out; PutU64(out, static_cast<uint64_t>(n));
        BitWriter bw(out);
        int pos = 0;
        for (const Tok& t : toks) {
            if (t.len == 1) { bw.PutBit(0); bw.PutBits(d[pos], 8); pos += 1; }
            else {
                bw.PutBit(1);
                bw.PutBits(static_cast<uint32_t>(t.dist - 1), LZSS_WINDOW_BITS);
                bw.PutBits(static_cast<uint32_t>(t.len - LZSS_MIN_MATCH), LZSS_LEN_BITS);
                pos += t.len;
            }
        }
        bw.Flush();
        return out;
    };

    auto bitlen   = [](uint32_t x) { int b = 0; while (x) { ++b; x >>= 1; } return b; };
    auto distSlot = [&](int D) { return bitlen(static_cast<uint32_t>(D - 1)); };   // 0..15

    // ---- 価格モデル (推定ビット数) ----
    double flag0P = 1.0, flag1P = 1.0;
    std::vector<double> litP(256, 8.0);
    std::vector<double> lenP(LZSS_MAX_MATCH + 1, 8.0);
    std::vector<double> slotP(16, 0.0);
    for (int b = 0; b < 16; ++b) { int extra = (b > 0 ? b - 1 : 0); slotP[b] = std::max(0.0, 15.0 - extra); }

    auto distPrice  = [&](int D) { int b = distSlot(D); int extra = (b > 0 ? b - 1 : 0); return slotP[b] + extra; };
    auto litPrice   = [&](uint8_t c) { return flag0P + litP[c]; };

    const double INF = 1e18;
    std::vector<double> cost(n + 1, INF);
    std::vector<int> pLen(n + 1, 0), pDist(n + 1, 0);

    auto runDP = [&]() -> std::vector<Tok> {
        std::fill(cost.begin(), cost.end(), INF);
        cost[0] = 0.0;
        for (int i = 0; i < n; ++i) {
            if (cost[i] >= INF) continue;
            double base = cost[i];
            double lc = base + litPrice(d[i]);                 // リテラル
            if (lc < cost[i + 1]) { cost[i + 1] = lc; pLen[i + 1] = 1; pDist[i + 1] = 0; }
            int Lmax = mlen[i];
            if (Lmax >= LZSS_MIN_MATCH) {
                int D = mdist[i];
                double pre = base + flag1P + distPrice(D);
                for (int L = LZSS_MIN_MATCH; L <= Lmax; ++L) {  // 長さ L の一致
                    double mc = pre + lenP[L];
                    if (mc < cost[i + L]) { cost[i + L] = mc; pLen[i + L] = L; pDist[i + L] = D; }
                }
            }
        }
        std::vector<Tok> toks;
        int j = n;
        while (j > 0) {
            int L = pLen[j];
            if (L == 1) { toks.push_back({1, 0}); j -= 1; }
            else        { toks.push_back({L, pDist[j]}); j -= L; }
        }
        std::reverse(toks.begin(), toks.end());
        return toks;
    };

    // トークン統計からエントロピー推定コストを再計算
    auto rebuildPrices = [&](const std::vector<Tok>& toks) {
        std::vector<double> lc(256, 0), le(LZSS_MAX_MATCH + 1, 0), sl(16, 0);
        double nl = 0, nm = 0;
        int pos = 0;
        for (const Tok& t : toks) {
            if (t.len == 1) { lc[d[pos]] += 1; nl += 1; pos += 1; }
            else { le[t.len] += 1; sl[distSlot(t.dist)] += 1; nm += 1; pos += t.len; }
        }
        double tot = nl + nm;
        flag0P = -std::log2((nl + 0.5) / (tot + 1.0));
        flag1P = -std::log2((nm + 0.5) / (tot + 1.0));
        for (int c = 0; c < 256; ++c) litP[c] = -std::log2((lc[c] + 0.5) / (nl + 128.0));
        for (int L = 0; L <= LZSS_MAX_MATCH; ++L) lenP[L] = -std::log2((le[L] + 0.5) / (nm + 0.5 * (LZSS_MAX_MATCH + 1)));
        for (int b = 0; b < 16; ++b) slotP[b] = -std::log2((sl[b] + 0.5) / (nm + 8.0));
    };

    // 貪欲法の候補 (最低保証)
    std::vector<Tok> greedy;
    { int pos = 0; while (pos < n) {
        if (mlen[pos] >= LZSS_MIN_MATCH) { greedy.push_back({mlen[pos], mdist[pos]}); pos += mlen[pos]; }
        else { greedy.push_back({1, 0}); pos += 1; } } }

    std::vector<uint8_t> best = emit(greedy);
    size_t bestSize = Encode_Huffman(best).size();

    const int ITERS = 5;
    for (int it = 0; it < ITERS; ++it) {
        std::vector<Tok> toks = runDP();
        std::vector<uint8_t> bytes = emit(toks);
        size_t hs = Encode_Huffman(bytes).size();      // 実際の後段ハフマンサイズで評価
        if (hs < bestSize) { bestSize = hs; best = std::move(bytes); }
        rebuildPrices(toks);                            // 次反復用のコスト
    }
    return best;
}

// ---- 拡張子から圧縮方式を決定するルーティング ----
//   今回は仮ルーティング。LZSS 等の新エンジン追加時にここを拡張する。
static uint8_t RouteByExtension(const std::string& name) {
    // 拡張子を小文字で取り出す
    size_t dot = name.find_last_of('.');
    std::string ext;
    if (dot != std::string::npos) {
        ext = name.substr(dot);
        for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }

    // 既に圧縮済みの形式は Store (再圧縮しても縮まないため)
    static const char* kStoreExt[] = {
        ".png", ".jpg", ".jpeg", ".gif",
        ".zip", ".7z", ".gz", ".rar", ".bz2", ".xz",
        ".mp3", ".mp4", ".avi", ".mkv", ".webp"
    };
    for (const char* e : kStoreExt)
        if (ext == e) return ALGO_STORE;

    // 実行ファイル・バイナリは LZSS
    static const char* kLzssExt[] = {
        ".exe", ".dll", ".bin", ".so", ".dylib", ".o", ".obj", ".lib", ".sys"
    };
    for (const char* e : kLzssExt)
        if (ext == e) return ALGO_LZSS;

    // それ以外は既存 BWT パイプライン
    return ALGO_BWT;
}

// ---- 方式に応じた 1 ファイルの圧縮 / 復元 ----
static std::vector<uint8_t> CompressOne(uint8_t algo, const std::vector<uint8_t>& in) {
    switch (algo) {
        case ALGO_STORE: return in;                       // そのまま
        case ALGO_BWT:   return Pipeline_Encode(in);      // 既存 4 段パイプライン
        // Step 3-A/3-B: 最適構文解析 LZSS の出力をハフマンで圧縮
        case ALGO_LZSS:  return Encode_Huffman(Encode_LZSS_Optimal(in));
        default:         return in;
    }
}
static std::vector<uint8_t> DecompressOne(uint8_t algo, const std::vector<uint8_t>& in,
                                          uint64_t /*originalSize*/) {
    switch (algo) {
        case ALGO_STORE: return in;
        case ALGO_BWT:   return Pipeline_Decode(in);
        // Step 3-A: ハフマン復号 -> LZSS 復号 の順
        case ALGO_LZSS:  return Decode_LZSS(Decode_Huffman(in));
        default:         return in;
    }
}

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
        case ALGO_BWT:   return "BWT";
        case ALGO_LZSS:  return "LZSS";
        case ALGO_STORE: return "Store";
        default:         return "?";
    }
}

// 入力フォルダを再帰スキャン -> ファイルごとに方式選択して個別圧縮 -> 1 ファイル出力
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

        StoredFile e;
        e.name = PathToUtf8(fs::relative(de.path(), inputDir));   // 相対パスを保存
        e.algo = RouteByExtension(e.name);
        e.originalSize = raw.size();
        e.data = CompressOne(e.algo, raw);
        totalOrig += raw.size();

        std::cout << "  [" << AlgoName(e.algo) << "] " << e.name
                  << "  " << raw.size() << " -> " << e.data.size() << " B"
                  << (raw.size() ? "  (" + std::to_string(100 * e.data.size() / raw.size()) + "%)" : "")
                  << "\n";
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
    all &= RunSuite("Huffman",              Encode_Huffman,  Decode_Huffman);
    all &= RunSuite("LZSS",                 Encode_LZSS_Greedy,  Decode_LZSS);
    all &= RunSuite("LZSS-opt",             Encode_LZSS_Optimal, Decode_LZSS);
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
int main() {
#ifdef _WIN32
    SetConsoleOutputCP(65001);   // コンソール出力を UTF-8 に (文字化け対策)
#endif

    // ---- アルゴリズムのセルフテスト ----
    if (!RunSelfTests()) {
        std::cout << "アルゴリズムのセルフテストに失敗しました。\n";
        return 1;
    }
    std::cout << "\n";

    // ====================================================================
    // フォルダのパスはここで変更できます
    //   注意: PrepareDummyData は inputDir を毎回削除して作り直します。
    //         コンテスト本番の data/ を壊さないよう、テストは別名フォルダで行います。
    // ====================================================================
    const fs::path inputDir   = "archive_test";            // 圧縮対象 (テスト用ダミー)
    const fs::path outputFile = "archive_test.enc";        // 圧縮済み 1 ファイル
    const fs::path restoreDir = "archive_test_restored";   // 復元先フォルダ
    // --------------------------------------------------------------------

    std::cout << "=== archive round-trip test ===\n";
    std::cout << "cwd        : " << fs::current_path().string() << "\n";

    // ---- 0. ダミーデータを用意 (inputDir=archive_test を再生成。data/ は触らない) ----
    PrepareDummyData(inputDir);

    // ---- 1. 圧縮: archive_test/ -> archive_test.enc ----
    if (!CompressFolder(inputDir, outputFile)) return 1;

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
