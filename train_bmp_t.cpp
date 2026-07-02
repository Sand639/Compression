// train_bmp_t.cpp — tBmp (hal 残差難易度文脈) の事前確率を学習して C 配列で出力。
// cm.cpp の predict と同一手順で ctx = (phase*16+prevResMag)*16+upMag を再現し、
// prefix (c0) ごとの bit 頻度から確率を推定。観測数 >= TH のエントリのみ焼く。
// 入力は Encode_Bmp_2DPredict のフィルタ出力 (CM に渡るストリームと同一)。
#include "compress.h"

#include <cstdio>
#include <vector>

// cm.cpp の bmpResMag と同一 (残差の符号 + 0/255からの距離を量子化 0..15)
static int bmpResMag(int v) {
    if (v == 0) return 0;
    int neg = v >= 128;
    int d = neg ? 256 - v : v;
    int m;
    if (d <= 2) m = d;
    else if (d <= 4) m = 3;
    else if (d <= 8) m = 4;
    else if (d <= 16) m = 5;
    else if (d <= 32) m = 6;
    else if (!neg && d > 96) m = 15;
    else m = 7;
    return neg ? (7 + m) : m;
}

int main(int argc, char** argv) {
    const uint32_t TH = (argc > 1) ? static_cast<uint32_t>(std::atoi(argv[1])) : 8;
    std::vector<uint8_t> raw;
    if (!ReadFileFs("data/hal.bmp", raw)) return 1;
    const std::vector<uint8_t> v = Encode_Bmp_2DPredict(raw);
    const size_t NCTX = 3 * 16 * 16;               // 768
    std::vector<uint32_t> zc(NCTX * 256, 0), oc(NCTX * 256, 0);
    int prevMag = 0;
    for (size_t p = 0; p < v.size(); ++p) {
        int phase = static_cast<int>(p % 3);
        int upMag = (p >= 542 + 1800) ? bmpResMag(v[p - 1800]) : 0;
        int ctx = (phase * 16 + prevMag) * 16 + upMag;
        int c0 = 1;
        for (int k = 7; k >= 0; --k) {
            int bit = (v[p] >> k) & 1;
            size_t ix = static_cast<size_t>(ctx) * 256 + c0;
            if (bit) ++oc[ix]; else ++zc[ix];
            c0 = (c0 << 1) | bit;
        }
        prevMag = bmpResMag(v[p]);
    }
    std::vector<uint32_t> out;
    for (size_t ix = 0; ix < zc.size(); ++ix) {
        uint32_t n = zc[ix] + oc[ix];
        if (n < TH) continue;
        int p4 = static_cast<int>(((static_cast<uint64_t>(oc[ix]) + 1) * 4096) / (n + 2));
        if (p4 < 1) p4 = 1; else if (p4 > 4095) p4 = 4095;
        out.push_back((static_cast<uint32_t>(ix) << 12) | static_cast<uint32_t>(p4));
    }
    std::printf("// tBmp prior: %zu entries (TH=%u). ix=ctx*256+c0, ctx=(phase*16+prevMag)*16+upMag\n", out.size(), TH);
    std::printf("static const uint32_t BMP_TPRIOR[%zu] = {\n", out.size());
    for (size_t i = 0; i < out.size(); ++i)
        std::printf("%uu%s%s", out[i], i + 1 < out.size() ? "," : "", (i % 12 == 11) ? "\n" : "");
    std::printf("};\n");
}
