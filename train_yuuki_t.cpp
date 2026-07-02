// train_yuuki_t.cpp — tYuuki (yuuki 縦order-1+フラグ文脈) の事前確率を学習して C 配列で出力。
// 文脈 ctx = (((up*2+flat)*2+vflat)*2+dflat) を cm.cpp の predict と同一手順で再現し、
// prefix (c0) ごとの bit 頻度から確率を推定。観測数 >= TH の頻出エントリのみ焼く。
#include "compress.h"

#include <cstdio>
#include <vector>

int main(int argc, char** argv) {
    const uint32_t TH = (argc > 1) ? static_cast<uint32_t>(std::atoi(argv[1])) : 400;
    std::vector<uint8_t> v;
    if (!ReadFileFs("data/yuuki_256.bmp", v)) return 1;
    std::vector<uint32_t> zc(2048 * 256, 0), oc(2048 * 256, 0);
    for (size_t p = 0; p < v.size(); ++p) {
        int up = (p >= 1074 + 800) ? v[p - 800] : 0;
        int flat = (p >= 1 && v[p - 1] == up) ? 1 : 0;
        int vflat = (p >= 1074 + 1600 && v[p - 1600] == up) ? 1 : 0;
        int dflat = (p >= 1074 + 799 && v[p - 799] == up) ? 1 : 0;
        int ctx = ((up * 2 + flat) * 2 + vflat) * 2 + dflat;
        int c0 = 1;
        for (int k = 7; k >= 0; --k) {
            int bit = (v[p] >> k) & 1;
            size_t ix = static_cast<size_t>(ctx) * 256 + c0;
            if (bit) ++oc[ix]; else ++zc[ix];
            c0 = (c0 << 1) | bit;
        }
    }
    std::vector<uint32_t> out;
    for (size_t ix = 0; ix < zc.size(); ++ix) {
        uint32_t n = zc[ix] + oc[ix];
        if (n < TH) continue;
        int p4 = static_cast<int>(((static_cast<uint64_t>(oc[ix]) + 1) * 4096) / (n + 2));
        if (p4 < 1) p4 = 1; else if (p4 > 4095) p4 = 4095;
        out.push_back((static_cast<uint32_t>(ix) << 12) | static_cast<uint32_t>(p4));
    }
    std::printf("// tYuuki prior: %zu entries (TH=%u). 上位12bit=ctx*256+c0, 下位12bit=p\n", out.size(), TH);
    std::printf("static const uint32_t YUUKI_TPRIOR[%zu] = {\n", out.size());
    for (size_t i = 0; i < out.size(); ++i)
        std::printf("%uu%s%s", out[i], i + 1 < out.size() ? "," : "", (i % 12 == 11) ? "\n" : "");
    std::printf("};\n");
}
