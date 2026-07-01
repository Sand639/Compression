#include "compress.h"
#include <array>
#include <cstdio>
// TEXT_PRIOR: wagahaiwa_nekodearu.txt の生バイト分布から
// bit-prefix 条件付き確率を学習して出力する（位相なし）。
struct Count { uint32_t z = 0, o = 0; };
int main() {
    std::vector<uint8_t> raw;
    if (!ReadFileFs("data/wagahaiwa_nekodearu.txt", raw)) { std::fprintf(stderr, "read fail\n"); return 1; }
    std::array<Count, 256> counts{};
    for (size_t pos = 0; pos < raw.size(); ++pos) {
        int c0 = 1;
        for (int k = 7; k >= 0; --k) {
            int bit = (raw[pos] >> k) & 1;
            counts[c0].z += (1 - bit);
            counts[c0].o += bit;
            c0 = (c0 << 1) | bit;
        }
    }
    std::printf("static const uint16_t TEXT_PRIOR[256] = {\n ");
    for (int c0 = 0; c0 < 256; ++c0) {
        const Count& c = counts[c0];
        int p = c.z + c.o ? static_cast<int>(((static_cast<uint64_t>(c.o) + 1) * 4096) / (c.z + c.o + 2)) : 2048;
        if (p < 1) p = 1; else if (p > 4095) p = 4095;
        std::printf("%d%s", p, c0 == 255 ? "" : ",");
    }
    std::printf("\n};\n");
}
