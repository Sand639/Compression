// CMパラメータ探索ハーネス (gitignore対象・使い捨て)。各ファイルを前処理後、CMProfile の
// パラメータを振って Encode_CM のサイズを測る。round-trip も確認。悪化しない最良点を探す。
#include "compress.h"
#include <iostream>
#include <cstdio>

static std::vector<uint8_t> readf(const char* p) {
    std::vector<uint8_t> v;
    if (!ReadFileFs(p, v)) std::printf("READ FAIL %s\n", p);
    return v;
}

static void probe(const char* name, const std::vector<uint8_t>& filtered, CMProfile base) {
    std::printf("== %s (filtered=%zu) ==\n", name, filtered.size());
    auto test = [&](CMProfile p, const char* tag) -> size_t {
        auto blob = Encode_CM(filtered, p);
        auto back = Decode_CM(blob, p);
        bool ok = (back == filtered);
        std::printf("  %-16s %9zu B  %s\n", tag, blob.size(), ok ? "OK" : "*** RT FAIL ***");
        return blob.size();
    };
    char buf[64];
    test(base, "base");
    for (int tb : { base.tbits + 1, base.tbits + 2 }) {
        if (tb > 30) continue;
        CMProfile p = base; p.tbits = tb;
        std::snprintf(buf, sizeof buf, "tbits=%d", tb); test(p, buf);
    }
    for (int ss : { base.subShift - 2, base.subShift - 1, base.subShift + 1 }) {
        CMProfile p = base; p.subShift = ss;
        std::snprintf(buf, sizeof buf, "subShift=%d", ss); test(p, buf);
    }
    for (int sl : { base.strideLen - 1, base.strideLen + 1 }) {
        if (sl < 1) continue;
        CMProfile p = base; p.strideLen = sl;
        std::snprintf(buf, sizeof buf, "strideLen=%d", sl); test(p, buf);
    }
    for (int ms : { base.mixShift - 1, base.mixShift + 1 }) {
        CMProfile p = base; p.mixShift = ms;
        std::snprintf(buf, sizeof buf, "mixShift=%d", ms); test(p, buf);
    }
}

int main() {
    probe("txt  CM(SLOW)",  readf("data/wagahaiwa_nekodearu.txt"),                 CM_PROF_SLOW);
    probe("hal  Bmp+CM",    Encode_Bmp_2DPredict(readf("data/hal.bmp")),           CM_PROF_BMP);
    probe("wav  Wav+CM",    Encode_Wav_MidSide_Delta(readf("data/explosion.wav"), 0), CM_PROF_WAV);
    probe("yuuki Wav+CM",   Encode_Wav_MidSide_Delta(readf("data/yuuki_256.bmp"), 0), CM_PROF_WAV);
    return 0;
}
