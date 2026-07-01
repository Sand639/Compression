#include "compress.h"
#include <array>
#include <cstdio>
struct Count { uint32_t z = 0, o = 0; };
int main() {
    std::vector<uint8_t> raw;
    if (!ReadFileFs("data/hal.bmp", raw)) return 1;
    std::vector<uint8_t> data = Encode_Bmp_2DPredict(raw);
    std::array<Count, 3 * 256> counts{};
    for (size_t pos = 0; pos < data.size(); ++pos) {
        int c0 = 1;
        for (int k = 7; k >= 0; --k) {
            int bit = (data[pos] >> k) & 1;
            Count& c = counts[(pos % 3) * 256 + c0];
            if (bit) ++c.o; else ++c.z;
            c0 = (c0 << 1) | bit;
        }
    }
    std::printf("static const uint16_t BMP_PRIOR[3][256] = {\n");
    for (int phase = 0; phase < 3; ++phase) {
        std::printf(" {");
        for (int c0 = 0; c0 < 256; ++c0) {
            const Count& c = counts[phase * 256 + c0];
            int p = c.z + c.o ? static_cast<int>(((static_cast<uint64_t>(c.o) + 1) * 4096) / (c.z + c.o + 2)) : 2048;
            if (p < 1) p = 1; else if (p > 4095) p = 4095;
            std::printf("%d%s", p, c0 == 255 ? "" : ",");
        }
        std::printf("}%s\n", phase == 2 ? "" : ",");
    }
    std::printf("};\n");
}
