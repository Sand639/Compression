// wav_mode_check.cpp — explosion.wav の WAV_CM トーナメントでどの stereoMode が勝つか確認する使い捨てハーネス。
#include "compress.h"

#include <cstdio>

int main() {
    std::vector<uint8_t> raw;
    if (!ReadFileFs("data/explosion.wav", raw)) return 1;
    for (int m = 0; m < 4; ++m) {
        auto f = Encode_Wav_MidSide_Delta(raw, m);
        auto cm = Encode_CM(f, CM_PROF_WAV);
        std::printf("mode %d: filter=%zu cm=%zu\n", m, f.size(), cm.size());
    }
}
