#include "compress.h"

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
