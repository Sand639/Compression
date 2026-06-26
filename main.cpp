// main.cpp — 可逆圧縮プログラムのドライバ (エントリポイント)
//
// 起動するとコンソールで圧縮(1)/展開(2)を選び、対象名を入力して処理する:
//   圧縮 : 入力したフォルダ/ファイルを「入力名.arc」へトーナメント圧縮する。
//   展開 : 入力した .arc を「ベース名_restored(必要なら連番)」フォルダへ展開する。
// アルゴリズム本体は機能ごとに分割した各 .cpp に置き、宣言は compress.h で共有する
// (transform / huffman / rangecoder / cm / lzss / filters / pipeline / io / archive / selftest)。
//
// ビルド (g++):
//   g++ -O2 -std=c++20 main.cpp transform.cpp huffman.cpp rangecoder.cpp cm.cpp \
//       lzss.cpp filters.cpp pipeline.cpp io.cpp archive.cpp selftest.cpp -o bwt
#include "compress.h"

// 入力文字列の前後の空白・改行を取り除く。
static std::string TrimInput(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return std::string();
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

// 文字列の末尾が ".arc" かどうか (大文字小文字は区別しない)。
static bool HasArcExt(const std::string& s) {
    if (s.size() < 4) return false;
    std::string tail = s.substr(s.size() - 4);
    for (char& c : tail) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return tail == ".arc";
}

int main(int argc, char** argv) {
#ifdef _WIN32
    SetConsoleOutputCP(65001);   // コンソール出力を UTF-8 に (文字化け対策)
    SetConsoleCP(65001);         // コンソール入力も UTF-8 に (日本語のファイル名対応)
#endif

    // --extract <archive.arc> <outdir/> : アーカイブを展開する (開発用・引数指定)
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

    // ==================================================================
    // 圧縮 / 展開 の選択 (1 か 2 以外は再入力)
    // ==================================================================
    int mode = 0;
    while (true) {
        std::cout << "圧縮:1 展開:2\n入力:";
        std::cout.flush();
        std::string line;
        if (!std::getline(std::cin, line)) return 0;   // 入力終端(EOF)で終了
        std::string in = TrimInput(line);
        if (in == "1") { mode = 1; break; }
        if (in == "2") { mode = 2; break; }
        std::cout << "1 または 2 を入力してください。\n";
    }

    if (mode == 1) {
        // ============================ 圧縮 ============================
        // 圧縮するフォルダ/ファイル名を入力させ、存在するまで再入力を促す。
        std::string target;
        fs::path    targetPath;
        while (true) {
            std::cout << "圧縮するフォルダ名またはファイル名を入力:";
            std::cout.flush();
            std::string line;
            if (!std::getline(std::cin, line)) return 0;
            target = TrimInput(line);
            if (target.empty()) {
                std::cout << "入力が空です。もう一度入力してください。\n";
                continue;
            }
            targetPath = Utf8ToPath(target);
            if (fs::exists(targetPath)) break;
            std::cout << "「" << target << "」が見つかりません。もう一度入力してください。\n";
        }

        // 出力名 = 入力文字列 + ".arc"
        const std::string outName = target + ".arc";
        const fs::path    outPath = Utf8ToPath(outName);

        std::cout << "\n=== 圧縮 (" << target << " -> " << outName << ") ===\n";
        if (!CompressFolder(targetPath, outPath)) {
            std::cout << "[ERROR] 圧縮に失敗しました。\n";
            return 1;
        }
        std::cout << "圧縮完了 -> " << outName << "\n";
        return 0;
    }

    // ============================ 展開 ============================
    // 展開する .arc を入力させる。拡張子が無ければ ".arc" を補い、存在するまで再入力を促す。
    std::string arcName;
    fs::path    arcPath;
    while (true) {
        std::cout << "展開するファイル名を入力:";
        std::cout.flush();
        std::string line;
        if (!std::getline(std::cin, line)) return 0;
        std::string in = TrimInput(line);
        if (in.empty()) {
            std::cout << "入力が空です。もう一度入力してください。\n";
            continue;
        }
        arcName = HasArcExt(in) ? in : (in + ".arc");   // .arc が無ければ補う (あれば付け足さない)
        arcPath = Utf8ToPath(arcName);
        if (fs::exists(arcPath)) break;
        std::cout << "「" << arcName << "」が見つかりません。もう一度入力してください。\n";
    }

    // 展開先フォルダ名 = (".arc" を除いたベース名) + "_restored"。
    // 既存フォルダ(元データ等)と被る場合は連番を付けて衝突を避ける。
    const std::string base = arcName.substr(0, arcName.size() - 4);   // 末尾 ".arc" を除く
    std::string outName = base + "_restored";
    fs::path    outDir  = Utf8ToPath(outName);
    for (int k = 2; fs::exists(outDir); ++k) {
        outName = base + "_restored" + std::to_string(k);
        outDir  = Utf8ToPath(outName);
    }

    std::cout << "\n=== 展開 (" << arcName << " -> " << outName << "/) ===\n";
    if (!DecompressArchive(arcPath, outDir)) {
        std::cout << "[ERROR] 展開に失敗しました。\n";
        return 1;
    }
    std::cout << "展開完了 -> " << outName << "/\n";
    return 0;
}
