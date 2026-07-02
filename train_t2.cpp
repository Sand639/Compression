// train_t2.cpp — t2 (order-2 ハッシュ文脈) の事前確率を wav 用に学習して C 配列で出力。
// idx[2] = (cx2 * 0x9E3779B1u + c0) & ((1<<27)-1)、cx2 = hash(prev1, prev2)。
#include "compress.h"

#include <cstdio>
#include <vector>

int main(int argc, char** argv) {
    const uint32_t TH = (argc > 1) ? static_cast<uint32_t>(std::atoi(argv[1])) : 8;
    std::vector<uint8_t> raw;
    if (!ReadFileFs("data/explosion.wav", raw)) return 1;
    const std::vector<uint8_t> v = Encode_Wav_MidSide_Delta(raw, 3);
    const uint32_t TMASK = (1u << 27) - 1;
    std::vector<uint32_t> zc(static_cast<size_t>(1) << 27, 0), oc(static_cast<size_t>(1) << 27, 0);
    for (size_t p = 0; p < v.size(); ++p) {
        uint32_t cx2 = 0;
        if (p >= 1) cx2 = cx2 * 0x9E3779B1u + v[p - 1] + 1u;
        if (p >= 2) cx2 = cx2 * 0x9E3779B1u + v[p - 2] + 1u;
        int c0 = 1;
        for (int k = 7; k >= 0; --k) {
            uint32_t idx = (cx2 * 0x9E3779B1u + static_cast<uint32_t>(c0)) & TMASK;
            int bit = (v[p] >> k) & 1;
            if (bit) ++oc[idx]; else ++zc[idx];
            c0 = (c0 << 1) | bit;
        }
    }
    std::vector<uint64_t> out;
    for (size_t ix = 0; ix < zc.size(); ++ix) {
        uint32_t n = zc[ix] + oc[ix];
        if (n < TH) continue;
        int p4 = static_cast<int>(((static_cast<uint64_t>(oc[ix]) + 1) * 4096) / (n + 2));
        if (p4 < 1) p4 = 1; else if (p4 > 4095) p4 = 4095;
        out.push_back((static_cast<uint64_t>(ix) << 12) | static_cast<uint64_t>(p4));
    }
    std::printf("// t2 prior WAV: %zu entries (TH=%u). 上位27bit=idx (ハッシュ済み), 下位12bit=p\n", out.size(), TH);
    std::printf("static const uint64_t T2_PRIOR_WAV[%zu] = {\n", out.size());
    for (size_t i = 0; i < out.size(); ++i)
        std::printf("%lluull%s%s", static_cast<unsigned long long>(out[i]), i + 1 < out.size() ? "," : "", (i % 8 == 7) ? "\n" : "");
    std::printf("};\n");
}
