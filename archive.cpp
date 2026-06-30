#include "compress.h"

std::vector<uint8_t> CompressOne(uint8_t algo, const std::vector<uint8_t>& in) {
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
        case ALGO_WAV_CM: {                                    // WAV 残差 -> CM (4ステレオモードを試し最小を採用)
            std::vector<uint8_t> best;
            for (int m = 0; m < 4; ++m) {
                std::vector<uint8_t> cm = Encode_CM(Encode_Wav_MidSide_Delta(in, m), CM_PROF_WAV);
                if (best.empty() || cm.size() < best.size()) best = std::move(cm);
            }
            return best;
        }
        case ALGO_BMP_CM:                                      // BMP 残差 -> CM (BMP プロファイル)
            return Encode_CM(Encode_Bmp_2DPredict(in), CM_PROF_BMP);
        case ALGO_BMP_CM2:                                     // BMP 残差 + チャンネル分離 -> CM
            return Encode_CM(BmpSeparateChannels(Encode_Bmp_2DPredict(in)));
        default:         return in;
    }
}
std::vector<uint8_t> DecompressOne(uint8_t algo, const std::vector<uint8_t>& in,
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

// ---- コンテナの構築 / 解析 (フォーマット 'ARC3') ----
std::vector<uint8_t> BuildArchive(const std::vector<StoredFile>& files) {
    std::vector<uint8_t> out;
    out.insert(out.end(), ARCHIVE_MAGIC, ARCHIVE_MAGIC + 4);
    LzPutLEB(out, static_cast<uint32_t>(files.size()));               // ヘッダは LEB128 で節約
    for (const auto& f : files) {
        LzPutLEB(out, static_cast<uint32_t>(f.name.size()));
        out.insert(out.end(), f.name.begin(), f.name.end());
        out.push_back(f.algo);
        LzPutLEB(out, static_cast<uint32_t>(f.originalSize));
        LzPutLEB(out, static_cast<uint32_t>(f.data.size()));
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

    if (!have(1)) return false;
    uint32_t count = LzGetLEB(buf.data(), pos);                       // ヘッダは LEB128

    for (uint32_t i = 0; i < count; ++i) {
        if (!have(1)) return false;
        uint32_t nlen = LzGetLEB(buf.data(), pos);
        if (!have(nlen)) return false;
        std::string name(reinterpret_cast<const char*>(&buf[pos]), nlen); pos += nlen;

        if (!have(1)) return false;
        uint8_t algo = buf[pos]; pos += 1;
        if (!have(1)) return false;
        uint64_t origSize = LzGetLEB(buf.data(), pos);
        if (!have(1)) return false;
        uint64_t compSize = LzGetLEB(buf.data(), pos);
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

// 入力(フォルダ or 単一ファイル)を走査 -> 各ファイルをトーナメントで圧縮 -> 1 ファイル出力
bool CompressFolder(const fs::path& inputPath, const fs::path& outputFile) {
    if (!fs::exists(inputPath)) {
        std::cout << "[ERROR] 入力が見つかりません: " << inputPath.string() << "\n";
        return false;
    }

    std::vector<StoredFile> files;
    uint64_t totalOrig = 0;

    // 1 ファイルをトーナメント圧縮して files に追加する
    auto addOne = [&](const fs::path& filePath, const std::string& name) -> bool {
        std::vector<uint8_t> raw;
        if (!ReadFileFs(filePath, raw)) {
            std::cout << "[ERROR] 読込失敗: " << filePath.string() << "\n";
            return false;
        }
        StoredFile e = CompressFileTournament(name, raw);
        totalOrig += raw.size();
        double saved = raw.size() ? 100.0 - 100.0 * e.data.size() / raw.size() : 0.0;
        std::printf("  %-24s %9zu -> %9zu B  [%-11s] Saved %5.1f%%\n",
                    name.c_str(), raw.size(), e.data.size(), AlgoName(e.algo), saved);
        files.push_back(std::move(e));
        return true;
    };

    if (fs::is_directory(inputPath)) {
        // フォルダ: 再帰スキャンし、相対パスを名前として格納
        for (const auto& de : fs::recursive_directory_iterator(inputPath)) {
            if (!de.is_regular_file()) continue;
            std::string name = PathToUtf8(fs::relative(de.path(), inputPath));
            if (!addOne(de.path(), name)) return false;
        }
    } else {
        // 単一ファイル: ファイル名のみを名前として格納
        if (!addOne(inputPath, PathToUtf8(inputPath.filename()))) return false;
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
