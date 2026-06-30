#include "compress.h"
#include <array>
#include <cstdio>
// WAV_PRIOR: explosion.wav を最良ステレオモードで変換した後の
// 4-byte位相 × bit-prefix の条件付き確率を学習して出力する。
struct Count { uint32_t z = 0, o = 0; };
int main() {
    std::vector<uint8_t> raw;
    if (!ReadFileFs("data/explosion.wav", raw)) { std::fprintf(stderr, "read fail\n"); return 1; }
    // 4モードを全試行し、最小WAV変換出力のモードを採用
    int bestMode = 0;
    size_t bestSize = (size_t)-1;
    for (int m = 0; m < 4; ++m) {
        auto t = Encode_Wav_MidSide_Delta(raw, m);
        if (t.size() < bestSize) { bestSize = t.size(); bestMode = m; }
    }
    std::fprintf(stderr, "bestMode=%d, transformed size=%zu\n", bestMode, bestSize);
    std::vector<uint8_t> data = Encode_Wav_MidSide_Delta(raw, bestMode);
    // strideLen=4 なので 4-byte 位相別に bit-prefix 条件付き確率を計算
    std::array<Count, 4 * 256> counts{};
    for (size_t pos = 0; pos < data.size(); ++pos) {
        int c0 = 1;
        for (int k = 7; k >= 0; --k) {
            int bit = (data[pos] >> k) & 1;
            Count& c = counts[(pos % 4) * 256 + c0];
            if (bit) ++c.o; else ++c.z;
            c0 = (c0 << 1) | bit;
        }
    }
    std::printf("static const uint16_t WAV_PRIOR[4][256] = {\n");
    for (int phase = 0; phase < 4; ++phase) {
        std::printf(" {");
        for (int c0 = 0; c0 < 256; ++c0) {
            const Count& c = counts[phase * 256 + c0];
            int p = c.z + c.o ? static_cast<int>(((static_cast<uint64_t>(c.o) + 1) * 4096) / (c.z + c.o + 2)) : 2048;
            if (p < 1) p = 1; else if (p > 4095) p = 4095;
            std::printf("%d%s", p, c0 == 255 ? "" : ",");
        }
        std::printf("}%s\n", phase == 3 ? "" : ",");
    }
    std::printf("};\n");
}
