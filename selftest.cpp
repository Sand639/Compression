#include "compress.h"

template <class Enc, class Dec>
static bool RunCase(const std::string& name, const std::vector<uint8_t>& data,
                    Enc enc, Dec dec) {
    std::vector<uint8_t> e = enc(data);
    std::vector<uint8_t> d = dec(e);
    bool ok = (d == data);
    std::cout << (ok ? "[OK]   " : "[FAIL] ")
              << name << "  (size=" << data.size() << ")\n";
    return ok;
}

// 各段共通で使うテストデータ群に対して enc/dec を検証
template <class Enc, class Dec>
static bool RunSuite(const std::string& title, Enc enc, Dec dec) {
    std::cout << "--- " << title << " ---\n";
    bool all = true;
    all &= RunCase("abracadabra", Str("abracadabra"), enc, dec);
    all &= RunCase("empty",       Str(""),            enc, dec);
    all &= RunCase("single",      Str("A"),           enc, dec);
    all &= RunCase("all same",    std::vector<uint8_t>(1000, 0x41), enc, dec);
    {
        std::vector<uint8_t> v = {0x00, 0x01, 0x00, 0xFF, 0x00};
        all &= RunCase("NUL bytes", v, enc, dec);
    }
    {
        std::mt19937 rng(12345);
        std::uniform_int_distribution<int> byte(0, 255);
        std::vector<uint8_t> v(65536);
        for (auto& b : v) b = static_cast<uint8_t>(byte(rng));
        all &= RunCase("random 65536", v, enc, dec);
    }
    return all;
}

bool RunSelfTests() {
    std::cout << "=== self tests ===\n";
    bool all = true;
    all &= RunSuite("BWT",                  Encode_BWT,      Decode_BWT);
    all &= RunSuite("MTF",                  Encode_MTF,      Decode_MTF);
    all &= RunSuite("RLE",                  Encode_RLE,      Decode_RLE);
    all &= RunSuite("Huffman",              Encode_Huffman,      Decode_Huffman);
    all &= RunSuite("RangeCoder o0",        Encode_RangeCoder,   Decode_RangeCoder);
    all &= RunSuite("RangeCoder o1",        Encode_RangeCoderO1, Decode_RangeCoderO1);
    all &= RunSuite("RangeCoder o2",        Encode_RangeCoderO2, Decode_RangeCoderO2);
    all &= RunSuite("CM",
                    [](const std::vector<uint8_t>& v){ return Encode_CM(v); },
                    [](const std::vector<uint8_t>& v){ return Decode_CM(v); });
    all &= RunSuite("Entropy(o0/o1)",       Encode_Entropy,      Decode_Entropy);
    all &= RunSuite("LZSS",                 Encode_LZSS_Greedy,  Decode_LZSS);
    all &= RunSuite("LZSS-opt",             Encode_LZSS_Optimal, Decode_LZSS);
    all &= RunSuite("LZSS-split",           Encode_LZSS_Split,   Decode_LZSS_Split);
    all &= RunSuite("LZSS-4stream",         Encode_LZSS_4Stream, Decode_LZSS_4Stream);
    for (int s = 1; s <= 4; ++s) {
        all &= RunSuite("Delta stride=" + std::to_string(s),
                        [s](const std::vector<uint8_t>& v) { return Encode_Delta(v, s); },
                        [s](const std::vector<uint8_t>& v) { return Decode_Delta(v, s); });
    }
    all &= RunSuite("BCJ",                  Encode_BCJ,          Decode_BCJ);
    all &= RunSuite("WAV mid/side",
                    [](const std::vector<uint8_t>& v){ return Encode_Wav_MidSide_Delta(v); },
                    Decode_Wav_MidSide_Delta);
    all &= RunSuite("BMP 2D filter",        Encode_Bmp_2DPredict,     Decode_Bmp_2DPredict);
    all &= RunSuite("full pipe (B+M+R+H)",  Pipeline_Encode, Pipeline_Decode);

    // マルチブロック (>900KiB) の検証: 圧縮しやすい部分と乱雑な部分を混在
    {
        std::cout << "--- multi-block pipeline ---\n";
        std::mt19937 rng(2024);
        std::uniform_int_distribution<int> byte(0, 255);
        std::vector<uint8_t> big;
        big.reserve(2'300'000);
        while (big.size() < 2'300'000) {
            // 反復しやすいパターン
            for (int k = 0; k < 5000; ++k) big.push_back(static_cast<uint8_t>(k & 7));
            // 乱雑なパターン
            for (int k = 0; k < 3000; ++k) big.push_back(static_cast<uint8_t>(byte(rng)));
        }
        std::vector<uint8_t> enc = Pipeline_Encode(big);
        std::vector<uint8_t> dec = Pipeline_Decode(enc);
        uint32_t blocks = (enc.size() >= 4) ? GetU32(enc.data()) : 0;
        bool ok = (dec == big);
        std::cout << (ok ? "[OK]   " : "[FAIL] ")
                  << "multi-block  (size=" << big.size()
                  << ", blocks=" << blocks
                  << ", encoded=" << enc.size() << ")\n";
        all &= ok;
    }

    std::cout << (all ? "self tests PASSED\n" : "self tests FAILED\n");
    return all;
}

// ==========================================================================
// テスト用: data/ にダミーファイル群を用意する (存在しない/再生成)
// ==========================================================================
