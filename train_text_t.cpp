// train_text_t.cpp — tText (Shift-JIS 構造文脈) の事前確率を学習して C 配列で出力。
// cm.cpp の predict/update と同一手順で textIdx (26bitハッシュ) を再現し bit 頻度を集計。
// textIdx は 26bit のため要素は uint64 ((idx<<12)|p)。TH で頻出エントリに絞る。
#include "compress.h"

#include <cstdio>
#include <vector>

int main(int argc, char** argv) {
    const uint32_t TH = (argc > 1) ? static_cast<uint32_t>(std::atoi(argv[1])) : 8;
    std::vector<uint8_t> v;
    if (!ReadFileFs("data/wagahaiwa_nekodearu.txt", v)) return 1;
    const int TEXT_BITS = 26;
    const uint32_t TEXT_MASK = (1u << TEXT_BITS) - 1;
    std::vector<uint32_t> zc(static_cast<size_t>(1) << TEXT_BITS, 0), oc(static_cast<size_t>(1) << TEXT_BITS, 0);
    // text 状態機械 (cm.cpp と同一)
    bool sjisTrail = false;
    int sjisLead = 0;
    uint16_t textPrevChar = 0;
    uint32_t textClasses = 0;
    auto isLead = [](int x) { return (x >= 0x81 && x <= 0x9F) || (x >= 0xE0 && x <= 0xFC); };
    for (size_t p = 0; p < v.size(); ++p) {
        int B = v[p];
        int c0 = 1;
        for (int k = 7; k >= 0; --k) {
            uint32_t th = static_cast<uint32_t>(c0);
            th = th * 0x9E3779B1u + textClasses;
            th = th * 0x9E3779B1u + static_cast<uint32_t>(textPrevChar + 1);
            th = th * 0x9E3779B1u + static_cast<uint32_t>(sjisTrail ? (0x100 | sjisLead) : 0);
            uint32_t idx = th & TEXT_MASK;
            int bit = (B >> k) & 1;
            if (bit) ++oc[idx]; else ++zc[idx];
            c0 = (c0 << 1) | bit;
        }
        // バイト境界の状態更新 (cm.cpp update と同一)
        int cls = 0;
        if (sjisTrail) {
            uint16_t ch = static_cast<uint16_t>((sjisLead << 8) | B);
            if (sjisLead == 0x82 && B >= 0x9F && B <= 0xF1) cls = 6;
            else if (sjisLead == 0x83) cls = 7;
            else if (sjisLead == 0x81) cls = 8;
            else cls = 9;
            textPrevChar = ch;
            sjisTrail = false; sjisLead = 0;
        } else if (isLead(B)) {
            sjisLead = B; sjisTrail = true;
        } else {
            if (B == '\r' || B == '\n') cls = 1;
            else if (B == ' ' || B == '\t') cls = 2;
            else if (B >= '0' && B <= '9') cls = 3;
            else if ((B >= 'A' && B <= 'Z') || (B >= 'a' && B <= 'z')) cls = 4;
            else cls = 5;
            textPrevChar = static_cast<uint16_t>(B);
        }
        if (cls != 0) textClasses = ((textClasses << 4) | static_cast<uint32_t>(cls)) & 0xFFFFFFu;
    }
    std::vector<uint64_t> out;
    for (size_t ix = 0; ix < zc.size(); ++ix) {
        uint32_t n = zc[ix] + oc[ix];
        if (n < TH) continue;
        int p4 = static_cast<int>(((static_cast<uint64_t>(oc[ix]) + 1) * 4096) / (n + 2));
        if (p4 < 1) p4 = 1; else if (p4 > 4095) p4 = 4095;
        out.push_back((static_cast<uint64_t>(ix) << 12) | static_cast<uint64_t>(p4));
    }
    std::printf("// tText prior: %zu entries (TH=%u). 上位26bit=textIdx, 下位12bit=p\n", out.size(), TH);
    std::printf("static const uint64_t TEXT_TPRIOR[%zu] = {\n", out.size());
    for (size_t i = 0; i < out.size(); ++i)
        std::printf("%lluull%s%s", static_cast<unsigned long long>(out[i]), i + 1 < out.size() ? "," : "", (i % 8 == 7) ? "\n" : "");
    std::printf("};\n");
}
