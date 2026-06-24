// extract.cpp — output.enc から元ファイルを data/ へ展開する小さなドライバ
// コンパイル: g++ -O1 -std=c++20 extract.cpp -o extract
// 使い方: ./extract <archive.enc> <output_dir/>

// bwt.cpp の内容をそのままインクルードする代わり、
// 共有関数をコンパイル済みオブジェクトから使うのが理想だが、
// 手っ取り早く bwt.cpp を #include する。
// メインを上書きするため先に定義。

#define SKIP_MAIN 1
// bwt.cpp の main() を無効化するために SKIP_MAIN マクロを使う方法は
// bwt.cpp 側が対応していないので別アプローチ:
// bwt.cpp をコピーして main を rename する。

// 代わりに直接 stdio で archive を parse して展開する小さな実装。

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <vector>
#include <string>
#include <filesystem>
#include <fstream>
#include <iostream>

namespace fs = std::filesystem;

static uint32_t ru32(const uint8_t* p) {
    return p[0] | (uint32_t(p[1])<<8) | (uint32_t(p[2])<<16) | (uint32_t(p[3])<<24);
}
static uint64_t ru64(const uint8_t* p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v |= uint64_t(p[i]) << (8*i);
    return v;
}

int main(int argc, char** argv) {
    if (argc < 3) { std::cerr << "usage: extract <archive.enc> <outdir/>\n"; return 1; }
    const char* archPath = argv[1];
    fs::path outDir = argv[2];

    // read archive
    std::ifstream f(archPath, std::ios::binary);
    if (!f) { std::cerr << "cannot open " << archPath << "\n"; return 1; }
    std::vector<uint8_t> buf((std::istreambuf_iterator<char>(f)), {});
    f.close();

    size_t pos = 0;
    auto have = [&](size_t k){ return pos + k <= buf.size(); };

    // magic check
    if (!have(4) || memcmp(buf.data(), "ARC1", 4) != 0) {
        std::cerr << "bad magic\n"; return 1;
    }
    pos += 4;

    if (!have(4)) { std::cerr << "truncated\n"; return 1; }
    uint32_t count = ru32(buf.data() + pos); pos += 4;
    std::printf("archive: %u files\n", count);

    fs::create_directories(outDir);

    for (uint32_t i = 0; i < count; ++i) {
        if (!have(4)) { std::cerr << "truncated at file " << i << "\n"; return 1; }
        uint32_t nameLen = ru32(buf.data() + pos); pos += 4;
        if (!have(nameLen)) { std::cerr << "truncated name\n"; return 1; }
        std::string name(buf.begin() + pos, buf.begin() + pos + nameLen); pos += nameLen;
        if (!have(1)) { std::cerr << "truncated algo\n"; return 1; }
        uint8_t algo = buf[pos++];
        if (!have(8)) { std::cerr << "truncated origSize\n"; return 1; }
        uint64_t origSize = ru64(buf.data() + pos); pos += 8;
        if (!have(8)) { std::cerr << "truncated compSize\n"; return 1; }
        uint64_t compSize = ru64(buf.data() + pos); pos += 8;
        if (!have(compSize)) { std::cerr << "truncated data for " << name << "\n"; return 1; }

        std::printf("  [%u] %-30s algo=%u  orig=%llu  comp=%llu\n",
            i, name.c_str(), (unsigned)algo, (unsigned long long)origSize, (unsigned long long)compSize);

        // save the compressed payload so we can decompress with bwt later
        fs::path outPath = outDir / name;
        fs::create_directories(outPath.parent_path());
        std::ofstream of(outPath.string() + ".algo" + std::to_string(algo) + ".bin", std::ios::binary);
        of.write((const char*)(buf.data() + pos), compSize);
        pos += compSize;
    }
    std::printf("done. saved compressed payloads to %s\n", outDir.string().c_str());
    return 0;
}
