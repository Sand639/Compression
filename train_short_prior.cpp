#include "compress.h"

#include <array>
#include <cstdio>
#include <vector>

int main() {
    std::vector<uint8_t> raw;
    if (!ReadFileFs("data/TeraPad.exe", raw)) return 1;
    const std::vector<uint8_t> data = Encode_BCJ(raw);
    std::array<std::array<uint32_t, 256>, 2> ones{};
    std::array<std::array<uint32_t, 256>, 2> total{};
    int remain = 0, cls = 0;
    bool prefix0F = false;
    for (uint8_t B : data) {
        if (remain > 0) {
            if (cls == 8 || cls == 9) {
                int g = cls - 8, c0 = 1;
                for (int bit = 7; bit >= 0; --bit) {
                    int y = (B >> bit) & 1;
                    ++total[g][c0];
                    ones[g][c0] += static_cast<uint32_t>(y);
                    c0 = (c0 << 1) | y;
                }
            }
            if (--remain == 0) cls = 0;
        } else if (prefix0F) {
            prefix0F = false;
            if (B >= 0x80 && B <= 0x8F) { cls = 7; remain = 4; }
        } else if (B == 0x0F) {
            prefix0F = true;
        } else if (B == 0xE8 || B == 0xE9) {
            cls = 1; remain = 4;
        } else if (B >= 0xB8 && B <= 0xBF) {
            cls = 2; remain = 4;
        } else if (B == 0x68) {
            cls = 3; remain = 4;
        } else if ((B & 0xC7) == 0x05 && B < 0x40) {
            cls = 4; remain = 4;
        } else if (B >= 0xA0 && B <= 0xA3) {
            cls = 5; remain = 4;
        } else if (B == 0xA9) {
            cls = 6; remain = 4;
        } else if (B >= 0x70 && B <= 0x7F) {
            cls = 8; remain = 1;
        } else if (B == 0xEB || (B >= 0xE0 && B <= 0xE3)) {
            cls = 9; remain = 1;
        }
    }
    for (int g = 0; g < 2; ++g) {
        std::printf(" {");
        for (int p = 0; p < 256; ++p) {
            int v = 2048;
            if (total[g][p]) {
                v = static_cast<int>((static_cast<uint64_t>(ones[g][p]) * 4096) / total[g][p]);
                if (v < 1) v = 1;
                if (v > 4095) v = 4095;
            }
            std::printf("%s%d", p ? "," : "", v);
        }
        std::printf("}, // samples=%u\n", total[g][1]);
    }
}
