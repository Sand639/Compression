#include "compress.h"

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
