// train_t1.cpp — t1 (order-1 文脈: prevByte×c0) の事前確率を5ファイル分学習して C 配列で出力。
// 各ファイルの CM 入力ストリーム (フィルタ後) で bit 頻度を集計。ix = prev*256 + c0 (16bit)。
// t1 の実インデックスは prev*512 + c0 (cm.cpp)。焼き込み側で変換する。
#include "compress.h"

#include <cstdio>
#include <vector>

static void trainOne(const char* name, const std::vector<uint8_t>& v, uint32_t TH) {
    std::vector<uint32_t> zc(256 * 256, 0), oc(256 * 256, 0);
    int prev = 0;
    for (size_t p = 0; p < v.size(); ++p) {
        int c0 = 1;
        for (int k = 7; k >= 0; --k) {
            int bit = (v[p] >> k) & 1;
            size_t ix = static_cast<size_t>(prev) * 256 + c0;
            if (bit) ++oc[ix]; else ++zc[ix];
            c0 = (c0 << 1) | bit;
        }
        prev = v[p];
    }
    std::vector<uint32_t> out;
    for (size_t ix = 0; ix < zc.size(); ++ix) {
        uint32_t n = zc[ix] + oc[ix];
        if (n < TH) continue;
        int p4 = static_cast<int>(((static_cast<uint64_t>(oc[ix]) + 1) * 4096) / (n + 2));
        if (p4 < 1) p4 = 1; else if (p4 > 4095) p4 = 4095;
        out.push_back((static_cast<uint32_t>(ix) << 12) | static_cast<uint32_t>(p4));
    }
    std::printf("// t1 prior %s: %zu entries (TH=%u). ix=prev*256+c0\n", name, out.size(), TH);
    std::printf("static const uint32_t T1_PRIOR_%s[%zu] = {\n", name, out.size());
    for (size_t i = 0; i < out.size(); ++i)
        std::printf("%uu%s%s", out[i], i + 1 < out.size() ? "," : "", (i % 12 == 11) ? "\n" : "");
    std::printf("};\n");
}

int main(int argc, char** argv) {
    const uint32_t TH = (argc > 1) ? static_cast<uint32_t>(std::atoi(argv[1])) : 2;
    std::vector<uint8_t> raw;
    if (!ReadFileFs("data/yuuki_256.bmp", raw)) return 1;
    trainOne("YUUKI", raw, TH);
    if (!ReadFileFs("data/hal.bmp", raw)) return 1;
    trainOne("HAL", Encode_Bmp_2DPredict(raw), TH);
    if (!ReadFileFs("data/explosion.wav", raw)) return 1;
    trainOne("WAV", Encode_Wav_MidSide_Delta(raw, 3), TH);
    if (!ReadFileFs("data/wagahaiwa_nekodearu.txt", raw)) return 1;
    trainOne("TEXT", raw, TH);
    if (!ReadFileFs("data/TeraPad.exe", raw)) return 1;
    trainOne("EXE", Encode_BCJ(raw), TH);
    return 0;
}
