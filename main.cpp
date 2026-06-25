// main.cpp — 可逆圧縮プログラムのドライバ (エントリポイント)
//
// アルゴリズム本体は機能ごとに分割した各 .cpp に置き、宣言は compress.h で共有する
// (transform / huffman / rangecoder / cm / lzss / filters / pipeline / io / archive /
//  selftest)。このファイルは main と、ダミーデータ生成・フォルダ一致検証だけを持つ。
//
// 本番動作: data/ を 1 ファイル (output.enc) にトーナメント圧縮 -> data_restored/ へ
//           復元 -> 完全一致を検証する。data/ は読み取りのみ。
//
// ビルド (g++):
//   g++ -O2 -std=c++20 main.cpp transform.cpp huffman.cpp rangecoder.cpp cm.cpp \
//       lzss.cpp filters.cpp pipeline.cpp io.cpp archive.cpp selftest.cpp -o bwt
#include "compress.h"

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
