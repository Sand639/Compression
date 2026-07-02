// train_wav_t.cpp — tWav (wav 同位相order-1文脈) の事前確率を学習して C 配列で出力。
// explosion.wav の WAV_CM 勝ちモードは stereoMode=3 (L/R独立, wav_mode_check で確認)。
// cm.cpp の predict と同一手順で ctx = phase*256 + prev(buf[p-4]) を再現し bit 頻度を集計。
#include "compress.h"

#include <cstdio>
#include <vector>

int main(int argc, char** argv) {
    const uint32_t TH = (argc > 1) ? static_cast<uint32_t>(std::atoi(argv[1])) : 8;
    std::vector<uint8_t> raw;
    if (!ReadFileFs("data/explosion.wav", raw)) return 1;
    const std::vector<uint8_t> v = Encode_Wav_MidSide_Delta(raw, 3);
    const size_t NCTX = 4 * 256;                   // 1024
    std::vector<uint32_t> zc(NCTX * 256, 0), oc(NCTX * 256, 0);
    for (size_t p = 0; p < v.size(); ++p) {
        int phase = static_cast<int>(p % 4);
        int prev = (p >= 4) ? v[p - 4] : 0;
        int ctx = phase * 256 + prev;
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
    std::printf("// tWav prior: %zu entries (TH=%u). ix=ctx*256+c0, ctx=phase*256+prev(p-4)\n", out.size(), TH);
    std::printf("static const uint32_t WAV_TPRIOR[%zu] = {\n", out.size());
    for (size_t i = 0; i < out.size(); ++i)
        std::printf("%uu%s%s", out[i], i + 1 < out.size() ? "," : "", (i % 12 == 11) ? "\n" : "");
    std::printf("};\n");
}
