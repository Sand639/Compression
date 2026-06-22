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
// ここまでの圧縮パイプライン (段を増やすたびにここへ追加していく)
//   encode:  original ->[BWT]->[MTF]->[RLE]->[Huffman]-> encoded
//   decode:  encoded  ->[Huffman^-1]->[RLE^-1]->[MTF^-1]->[BWT^-1]-> restored
// ==========================================================================
std::vector<uint8_t> Pipeline_Encode(const std::vector<uint8_t>& input) {
    std::vector<uint8_t> x = Encode_BWT(input);
    x = Encode_MTF(x);
    x = Encode_RLE(x);
    x = Encode_Huffman(x);
    return x;
}
std::vector<uint8_t> Pipeline_Decode(const std::vector<uint8_t>& input) {
    std::vector<uint8_t> x = Decode_Huffman(input);
    x = Decode_RLE(x);
    x = Decode_MTF(x);
    x = Decode_BWT(x);
    return x;
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
    all &= RunSuite("full pipe (B+M+R+H)",  Pipeline_Encode, Pipeline_Decode);
    std::cout << (all ? "self tests PASSED\n" : "self tests FAILED\n");
    return all;
}

// ==========================================================================
// メイン: TEST フォルダ内のファイルを 1 つ指定して 圧縮(変換)→復元→検証
// ==========================================================================
int main() {
    // ---- セルフテスト ----
    if (!RunSelfTests()) {
        std::cout << "アルゴリズムのセルフテストに失敗しました。\n";
        return 1;
    }
    std::cout << "\n";

    // ====================================================================
    // ここを書き換えるだけで対象ファイルを変えられます
    // ====================================================================
    const std::string TEST_DIR = "TEST/";          // テスト用フォルダ
    std::string filename = "pika256.bmp";          // ← 対象ファイル名

    // 例: "01_まばたき.mp4" / "test_01.fbx" / "悪堕組_企画書.pptx" など
    // --------------------------------------------------------------------

    std::string inPath   = TEST_DIR + filename;
    std::string compPath = TEST_DIR + filename + ".enc";       // 変換後データ
    std::string decPath  = TEST_DIR + filename + ".restored";  // 復元データ

    std::cout << "=== file round-trip test ===\n";
    std::cout << "cwd    : " << std::filesystem::current_path().string() << "\n";
    std::cout << "target : " << inPath << "\n\n";

    // ---- 1. 読み込み ----
    std::vector<uint8_t> original;
    if (!ReadFile(inPath, original)) {
        std::cout << "[ERROR] ファイルを開けません: " << inPath << "\n"
                  << "  作業ディレクトリ (cwd) からの相対パスを確認してください。\n"
                  << "  Visual Studio のデバッグ実行では cwd は通常プロジェクト\n"
                  << "  フォルダ (bwt.cpp のある場所) になります。\n";
        return 1;
    }
    std::cout << "original size : " << original.size() << " bytes\n";

    // ---- 2. エンコード (BWT -> MTF) ----
    std::vector<uint8_t> encoded = Pipeline_Encode(original);
    if (!WriteFile(compPath, encoded)) {
        std::cout << "[ERROR] 書き込み失敗: " << compPath << "\n";
        return 1;
    }
    std::cout << "encoded  size : " << encoded.size() << " bytes  -> " << compPath << "\n";

    // ---- 3. デコード (MTF^-1 -> BWT^-1) ----
    std::vector<uint8_t> decoded = Pipeline_Decode(encoded);
    if (!WriteFile(decPath, decoded)) {
        std::cout << "[ERROR] 書き込み失敗: " << decPath << "\n";
        return 1;
    }
    std::cout << "decoded  size : " << decoded.size() << " bytes  -> " << decPath << "\n\n";

    // ---- 4. 完全一致検証 ----
    bool ok = (decoded == original);
    std::cout << "round-trip    : " << (ok ? "[OK] 完全一致" : "[FAIL] 不一致!!") << "\n";

    // ---- 5. サイズ報告 ----
    //   BWT -> MTF -> RLE -> Huffman の全 4 段による圧縮結果。
    if (original.size() > 0) {
        double ratio = 100.0 * encoded.size() / original.size();
        std::cout << "size ratio    : " << ratio << " %  (BWT+MTF+RLE+Huffman, 全4段)\n";
    }

    return ok ? 0 : 1;
}
