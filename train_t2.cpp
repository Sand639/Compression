// train_t2.cpp — t2 (order-2 ハッシュ文脈) の事前確率を wav 用に学習して C 配列で出力。
// idx[2] = (cx2 * 0x9E3779B1u + c0) & ((1<<27)-1)、cx2 = hash(prev1, prev2)。
#include "compress.h"

#include <cstdio>
#include <vector>

int main(int argc, char** argv) {
    const uint32_t TH = (argc > 1) ? static_cast<uint32_t>(std::atoi(argv[1])) : 8;
    const std::string kind = (argc > 2) ? argv[2] : "WAV";   // WAV / HAL / TEXT / YUUKI / EXE
    const int ORDER = (argc > 3) ? std::atoi(argv[3]) : 2;   // 2..8 (cx[k] の次数)
    std::vector<uint8_t> raw;
    std::vector<uint8_t> v;
    if (kind == "HAL") {
        if (!ReadFileFs("data/hal.bmp", raw)) return 1;
        v = Encode_Bmp_2DPredict(raw);
    } else if (kind == "TEXT") {
        if (!ReadFileFs("data/wagahaiwa_nekodearu.txt", v)) return 1;
    } else if (kind == "YUUKI") {
        if (!ReadFileFs("data/yuuki_256.bmp", v)) return 1;
    } else if (kind == "EXE") {
        if (!ReadFileFs("data/TeraPad.exe", raw)) return 1;
        v = Encode_BCJ(raw);
    } else {
        if (!ReadFileFs("data/explosion.wav", raw)) return 1;
        v = Encode_Wav_MidSide_Delta(raw, 3);
    }
    const int TBITS = (kind == "EXE") ? 29 : 27;   // exe プロファイルのみ tbits=29
    const uint32_t TMASK = (1u << TBITS) - 1;
    std::vector<uint32_t> zc(static_cast<size_t>(1) << TBITS, 0), oc(static_cast<size_t>(1) << TBITS, 0);
    // ORDER >= 90: stride/sparse 文脈モード (t9)。stride = ORDER - 90。
    // cm.cpp: sh3 = c0; sh3 = sh3*K + buf[p-s]+1; ... (c0 起点で順序が cxN 系と逆)
    const int STRIDE = (ORDER >= 90) ? ORDER - 90 : 0;
    for (size_t p = 0; p < v.size(); ++p) {
        uint32_t cxN = 0;
        if (STRIDE == 0)
            for (int j = 1; j <= ORDER; ++j)
                if (p >= static_cast<size_t>(j)) cxN = cxN * 0x9E3779B1u + v[p - j] + 1u;
        int c0 = 1;
        for (int k = 7; k >= 0; --k) {
            uint32_t idx;
            if (STRIDE > 0) {
                uint32_t sh3 = static_cast<uint32_t>(c0);
                for (int j = 1; j <= 3; ++j)
                    if (p >= static_cast<size_t>(j * STRIDE)) sh3 = sh3 * 0x9E3779B1u + v[p - j * STRIDE] + 1u;
                idx = sh3 & TMASK;
            } else
                idx = (cxN * 0x9E3779B1u + static_cast<uint32_t>(c0)) & TMASK;
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
    std::printf("// %s%d prior %s: %zu entries (TH=%u). idx=ハッシュ済み, 下位12bit=p\n", STRIDE > 0 ? "t9s" : "t", STRIDE > 0 ? STRIDE : ORDER, kind.c_str(), out.size(), TH);
    std::printf("static const uint64_t T%s%d_PRIOR_%s[%zu] = {\n", STRIDE > 0 ? "9S" : "", STRIDE > 0 ? STRIDE : ORDER, kind.c_str(), out.size());
    for (size_t i = 0; i < out.size(); ++i)
        std::printf("%lluull%s%s", static_cast<unsigned long long>(out[i]), i + 1 < out.size() ? "," : "", (i % 8 == 7) ? "\n" : "");
    std::printf("};\n");
}
