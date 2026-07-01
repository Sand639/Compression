#include "compress.h"
#include <array>
#include <cstdio>
// TEXT_BIGRAM_PRIOR: wagahaiwa_nekodearu.txt の bigram (prevByte × bit-prefix)
// 条件付き確率を学習して t1 初期化用テーブルを出力する。
// t1 index: (prevByte & 0xFF) * 512 + c0, c0=1..255
struct Count { uint32_t z = 0, o = 0; };
int main() {
    std::vector<uint8_t> raw;
    if (!ReadFileFs("data/wagahaiwa_nekodearu.txt", raw)) { std::fprintf(stderr, "read fail\n"); return 1; }
    // counts[prevByte][c0] (c0=1..255)
    std::vector<std::array<Count, 256>> counts(256);
    int prevByte = 0;  // 最初の "前バイト" は 0 とする
    for (size_t pos = 0; pos < raw.size(); ++pos) {
        int c0 = 1;
        for (int k = 7; k >= 0; --k) {
            int bit = (raw[pos] >> k) & 1;
            counts[prevByte][c0].z += (1 - bit);
            counts[prevByte][c0].o += bit;
            c0 = (c0 << 1) | bit;
        }
        prevByte = raw[pos];
    }
    // uint16_t TEXT_BIGRAM_PRIOR[256][256]
    std::printf("static const uint16_t TEXT_BIGRAM_PRIOR[256][256] = {\n");
    for (int pb = 0; pb < 256; ++pb) {
        std::printf(" {");
        for (int c0 = 0; c0 < 256; ++c0) {
            const Count& c = counts[pb][c0];
            int p;
            if (c0 == 0) {
                p = 2048;  // c0=0 は使用されない（プレースホルダー）
            } else {
                p = c.z + c.o ? static_cast<int>(((static_cast<uint64_t>(c.o) + 1) * 4096) / (c.z + c.o + 2)) : 2048;
                if (p < 1) p = 1; else if (p > 4095) p = 4095;
            }
            std::printf("%d%s", p, c0 == 255 ? "" : ",");
        }
        std::printf("}%s\n", pb == 255 ? "" : ",");
    }
    std::printf("};\n");
}
