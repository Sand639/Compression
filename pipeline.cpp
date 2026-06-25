#include "compress.h"

// ==========================================================================
// 1 ブロック分の 4 段パイプライン
//   encode:  block ->[BWT]->[MTF]->[RLE]->[RangeCoder]-> chunk
//   decode:  chunk ->[RangeCoder^-1]->[RLE^-1]->[MTF^-1]->[BWT^-1]-> block
// 最終段はハフマンからレンジコーダーに換装 (旧ハフマン版は下にコメントで保持)。
// ==========================================================================
static std::vector<uint8_t> EncodeBlock(const std::vector<uint8_t>& block) {
    std::vector<uint8_t> x = Encode_BWT(block);
    x = Encode_MTF(x);
    x = Encode_RLE(x);
    x = Encode_Entropy(x);        // 0次/1次の小さい方 (旧: Encode_Huffman)
    return x;
}
static std::vector<uint8_t> DecodeBlock(const std::vector<uint8_t>& chunk) {
    std::vector<uint8_t> x = Decode_Entropy(chunk);      // 旧: Decode_Huffman(chunk);
    x = Decode_RLE(x);
    x = Decode_MTF(x);
    x = Decode_BWT(x);
    return x;
}

// ==========================================================================
// ブロック分割パイプライン (最大 900KiB / ブロック)
//
//   コンテナ形式:
//     [uint32 LE] blockCount
//     各ブロック: [uint32 LE] chunkLen, 続けて chunkLen バイトの chunk
//
//   各ブロックは完全に独立して圧縮・復元される。メモリ使用量と速度
//   (BWT の O(n log^2 n)) をブロック単位に抑え、大容量ファイルでも安定する。
// ==========================================================================
static const size_t kBlockSize = 900 * 1024;     // 921600 bytes

std::vector<uint8_t> Pipeline_Encode(const std::vector<uint8_t>& input) {
    std::vector<uint8_t> out;
    const size_t n = input.size();
    uint32_t blockCount = static_cast<uint32_t>((n + kBlockSize - 1) / kBlockSize);

    PutU32(out, blockCount);
    for (size_t off = 0; off < n; off += kBlockSize) {
        size_t len = std::min(kBlockSize, n - off);
        std::vector<uint8_t> block(input.begin() + off, input.begin() + off + len);
        std::vector<uint8_t> chunk = EncodeBlock(block);
        PutU32(out, static_cast<uint32_t>(chunk.size()));
        out.insert(out.end(), chunk.begin(), chunk.end());
    }
    return out;
}

std::vector<uint8_t> Pipeline_Decode(const std::vector<uint8_t>& input) {
    std::vector<uint8_t> out;
    if (input.size() < 4) return out;              // ヘッダ未満

    size_t pos = 0;
    uint32_t blockCount = GetU32(&input[pos]); pos += 4;

    for (uint32_t b = 0; b < blockCount; ++b) {
        if (pos + 4 > input.size()) break;         // 防御的境界チェック
        uint32_t chunkLen = GetU32(&input[pos]); pos += 4;
        if (pos + chunkLen > input.size()) break;
        std::vector<uint8_t> chunk(input.begin() + pos, input.begin() + pos + chunkLen);
        pos += chunkLen;
        std::vector<uint8_t> block = DecodeBlock(chunk);
        out.insert(out.end(), block.begin(), block.end());
    }
    return out;
}
