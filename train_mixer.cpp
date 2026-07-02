// train_mixer.cpp — 第1ミキサー重み w の「1パス後状態」を採取し、初期値 (1<<14) との
// 差分が大きい上位 maxN エントリを C 配列で出力する (prior としてコードに焼く)。
// 使い方: train_mixer.exe <maxN> <KIND>   (KIND: TEXT/HAL/WAV/YUUKI/EXE)
#include "compress.h"

#include <cstdio>
#include <vector>

int main(int argc, char** argv) {
    const size_t maxN = (argc > 1) ? static_cast<size_t>(std::atoll(argv[1])) : 60000;
    const std::string kind = (argc > 2) ? argv[2] : "TEXT";
    std::vector<uint8_t> raw, v;
    CMProfile prof = CM_PROF_SLOW;
    if (kind == "HAL") {
        if (!ReadFileFs("data/hal.bmp", raw)) return 1;
        v = Encode_Bmp_2DPredict(raw); prof = CM_PROF_BMP;
    } else if (kind == "YUUKI") {
        if (!ReadFileFs("data/yuuki_256.bmp", v)) return 1;
        prof = CM_PROF_YUUKI;
    } else if (kind == "EXE") {
        if (!ReadFileFs("data/TeraPad.exe", raw)) return 1;
        v = Encode_BCJ(raw); prof = CM_PROF_FAST;
    } else if (kind == "WAV") {
        if (!ReadFileFs("data/explosion.wav", raw)) return 1;
        v = Encode_Wav_MidSide_Delta(raw, 3); prof = CM_PROF_WAV;
    } else {
        if (!ReadFileFs("data/wagahaiwa_nekodearu.txt", v)) return 1;
    }
    const int which = (argc > 3) ? std::atoi(argv[3]) : 1;   // 1-4=w..w4, 5=wf, 6-9=apm..apm4
    const char* dump = "mixer_dump.bin";
    Encode_CM_DumpState(v, prof, dump);
    FILE* f = std::fopen(dump, "rb");
    if (!f) return 1;
    uint64_t wn = 0;
    std::vector<int> w;
    for (int t = 1; t <= which; ++t) {                        // 目的テーブルまで読み飛ばす
        if (std::fread(&wn, 8, 1, f) != 1) return 1;
        const int esz = (t <= 5) ? 4 : 2;                     // 1-5: int32, 6-9: uint16
        if (t == which) {
            w.resize(wn);
            if (esz == 4) {
                if (std::fread(w.data(), 4, wn, f) != wn) return 1;
            } else {
                std::vector<uint16_t> tmp(wn);
                if (std::fread(tmp.data(), 2, wn, f) != wn) return 1;
                for (uint64_t i = 0; i < wn; ++i) w[i] = tmp[i];
            }
        } else {
            _fseeki64(f, static_cast<long long>(wn) * esz, SEEK_CUR);
        }
    }
    std::fclose(f);
    // APM (6-9) の初期値は initv[j] = CM_squash((j-32)*64)*16 (j = idx%65)。ミキサーは 1<<14。
    static const int sqt[33] = {1,2,3,6,10,16,27,45,73,120,194,310,488,747,1101,1546,2047,
                                2549,2994,3348,3607,3785,3901,3975,4022,4050,4068,4079,4085,4089,4092,4093,4094};
    auto squash = [&](int d) {
        if (d > 2047) return 4095;
        if (d < -2047) return 0;
        int wq = d & 127; d = (d >> 7) + 16;
        return (sqt[d] * (128 - wq) + sqt[d + 1] * wq + 64) >> 7;
    };
    int initv[65];
    for (int j = 0; j < 65; ++j) initv[j] = squash((j - 32) * 64) * 16;
    // 初期値との差の絶対値で降順ソートし上位 maxN を出力
    std::vector<std::pair<long, uint32_t>> diffs;
    diffs.reserve(wn);
    for (uint32_t i = 0; i < wn; ++i) {
        int init = (which <= 5) ? (1 << 14) : initv[i % 65];
        long d = static_cast<long>(w[i]) - init;
        if (d != 0) diffs.push_back({d < 0 ? -d : d, i});
    }
    std::sort(diffs.begin(), diffs.end(), [](auto& a, auto& b) { return a.first > b.first; });
    if (diffs.size() > maxN) diffs.resize(maxN);
    std::sort(diffs.begin(), diffs.end(), [](auto& a, auto& b) { return a.second < b.second; });
    static const char* names[10] = {"", "W", "W2", "W3", "W4", "WF", "APM", "APM2", "APM3", "APM4"};
    std::printf("// mixer/apm prior %s_%s: %zu entries (maxN=%zu, size=%llu). 上位32bit=idx, 下位32bit=値\n",
                names[which], kind.c_str(), diffs.size(), maxN, static_cast<unsigned long long>(wn));
    std::printf("static const uint64_t %s_PRIOR_%s[%zu] = {\n", names[which], kind.c_str(), diffs.size());
    for (size_t i = 0; i < diffs.size(); ++i) {
        uint64_t e = (static_cast<uint64_t>(diffs[i].second) << 32) | static_cast<uint32_t>(w[diffs[i].second]);
        std::printf("%lluull%s%s", static_cast<unsigned long long>(e), i + 1 < diffs.size() ? "," : "", (i % 8 == 7) ? "\n" : "");
    }
    std::printf("};\n");
    return 0;
}
