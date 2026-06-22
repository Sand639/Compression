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
// ここまでの圧縮パイプライン (段を増やすたびにここへ追加していく)
//   encode:  original ->[BWT]->[MTF]-> encoded
//   decode:  encoded  ->[MTF^-1]->[BWT^-1]-> restored
// ==========================================================================
std::vector<uint8_t> Pipeline_Encode(const std::vector<uint8_t>& input) {
    std::vector<uint8_t> x = Encode_BWT(input);
    x = Encode_MTF(x);
    return x;
}
std::vector<uint8_t> Pipeline_Decode(const std::vector<uint8_t>& input) {
    std::vector<uint8_t> x = Decode_MTF(input);
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
    all &= RunSuite("BWT",          Encode_BWT,      Decode_BWT);
    all &= RunSuite("MTF",          Encode_MTF,      Decode_MTF);
    all &= RunSuite("BWT+MTF pipe", Pipeline_Encode, Pipeline_Decode);
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
    // 注意: BWT/MTF はどちらも「変換」なのでサイズはほぼ不変 (+4 byte) です。
    //       実際の圧縮は この後 RLE -> Huffman を重ねた段階で効いてきます。
    if (original.size() > 0) {
        double ratio = 100.0 * encoded.size() / original.size();
        std::cout << "size ratio    : " << ratio << " %  (BWT+MTF 単体では圧縮されません)\n";
    }

    return ok ? 0 : 1;
}
