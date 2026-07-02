// train_exe_t.cpp — tExe (x86 operand 位置別文脈) の事前確率を学習して C 配列で出力。
// cm.cpp の exeClass 状態機械 (class1-9) と tExe ハッシュを同一手順で再現し bit 頻度を集計。
// idx は 22bit のため要素は uint64 ((idx<<12)|p)。
#include "compress.h"

#include <cstdio>
#include <vector>

int main(int argc, char** argv) {
    const uint32_t TH = (argc > 1) ? static_cast<uint32_t>(std::atoi(argv[1])) : 4;
    std::vector<uint8_t> raw;
    if (!ReadFileFs("data/TeraPad.exe", raw)) return 1;
    const std::vector<uint8_t> data = Encode_BCJ(raw);
    const int EXE_BITS = 22;
    const uint32_t EXE_MASK = (1u << EXE_BITS) - 1;
    std::vector<uint32_t> zc(static_cast<size_t>(1) << EXE_BITS, 0), oc(static_cast<size_t>(1) << EXE_BITS, 0);
    int remain = 0, cls = 0, opcode = 0;
    bool prefix0F = false;
    for (size_t p = 0; p < data.size(); ++p) {
        int B = data[p];
        // predict 相当: exeRemain > 0 のバイトのみ tExe が有効
        if (remain > 0) {
            int c0 = 1;
            for (int k = 7; k >= 0; --k) {
                uint32_t eh = static_cast<uint32_t>(c0);
                eh = eh * 0x9E3779B1u + static_cast<uint32_t>(opcode + 1);
                eh = eh * 0x9E3779B1u + static_cast<uint32_t>((cls << 3) | remain);
                eh = eh * 0x9E3779B1u + static_cast<uint32_t>(p & 15);
                uint32_t idx = eh & EXE_MASK;
                int bit = (B >> k) & 1;
                if (bit) ++oc[idx]; else ++zc[idx];
                c0 = (c0 << 1) | bit;
            }
        }
        // update 相当: バイト境界の状態遷移 (cm.cpp と同一)
        if (remain > 0) {
            if (--remain == 0) { cls = 0; opcode = 0; }
        } else if (prefix0F) {
            prefix0F = false;
            if (B >= 0x80 && B <= 0x8F) { cls = 7; opcode = B; remain = 4; }
        } else if (B == 0x0F) {
            prefix0F = true;
        } else if (B == 0xE8 || B == 0xE9) {
            cls = 1; opcode = B; remain = 4;
        } else if (B >= 0xB8 && B <= 0xBF) {
            cls = 2; opcode = B; remain = 4;
        } else if (B == 0x68) {
            cls = 3; opcode = B; remain = 4;
        } else if ((B & 0xC7) == 0x05 && B < 0x40) {
            cls = 4; opcode = B; remain = 4;
        } else if (B >= 0xA0 && B <= 0xA3) {
            cls = 5; opcode = B; remain = 4;
        } else if (B == 0xA9) {
            cls = 6; opcode = B; remain = 4;
        } else if (B >= 0x70 && B <= 0x7F) {
            cls = 8; opcode = B; remain = 1;
        } else if (B == 0xEB || (B >= 0xE0 && B <= 0xE3)) {
            cls = 9; opcode = B; remain = 1;
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
    std::printf("// tExe prior: %zu entries (TH=%u). 上位22bit=exeIdx (ハッシュ済み), 下位12bit=p\n", out.size(), TH);
    std::printf("static const uint64_t EXE_TPRIOR[%zu] = {\n", out.size());
    for (size_t i = 0; i < out.size(); ++i)
        std::printf("%lluull%s%s", static_cast<unsigned long long>(out[i]), i + 1 < out.size() ? "," : "", (i % 8 == 7) ? "\n" : "");
    std::printf("};\n");
}
