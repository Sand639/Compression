// compress.h — 可逆圧縮パイプライン共通ヘッダ
//
// 圧縮プログラムを機能ごとに複数の翻訳単位 (.cpp) へ分割した際、各モジュールが共有する
// インクルード・型・定数・関数プロトタイプをここに集約する。
//
// モジュール構成:
//   transform.cpp  : BWT / MTF / RLE
//   huffman.cpp    : 正準ハフマン
//   rangecoder.cpp : レンジコーダ (0/1/2 次) + Entropy セレクタ
//   cm.cpp         : コンテキストミキシング
//   lzss.cpp       : LZSS (貪欲 / 最適 / split / 4-stream)
//   filters.cpp    : Delta / BCJ / WAV / BMP 予測フィルタ
//   pipeline.cpp   : ブロック分割 BWT パイプライン
//   io.cpp         : ファイル / パス入出力
//   archive.cpp    : ARC4 コンテナ + ファイル単位トーナメント圧縮 + フォルダ圧縮/復元
//   selftest.cpp   : アルゴリズムの自己テスト
//   main.cpp       : ドライバ (main / ダミーデータ生成 / 一致検証)
//
// ※ ソースは UTF-8。MSVC ビルドは /utf-8 を指定しているため BOM は不要。
#pragma once

#include <cstdint>
#include <cstring>
#include <cctype>
#include <cstdio>
#include <cmath>
#include <vector>
#include <algorithm>
#include <numeric>
#include <queue>
#include <string>
#include <random>
#include <iostream>
#include <fstream>
#include <filesystem>
#include <cassert>

namespace fs = std::filesystem;

// Windows コンソールを UTF-8 に切り替える API (windows.h を避けて前方宣言)。
#ifdef _WIN32
extern "C" __declspec(dllimport) int __stdcall SetConsoleOutputCP(unsigned int wCodePageID);
extern "C" __declspec(dllimport) int __stdcall SetConsoleCP(unsigned int wCodePageID);
#endif

// ==========================================================================
// 小さな直列化ヘルパ (複数 TU から使うため inline)
// ==========================================================================
inline void PutU32(std::vector<uint8_t>& v, uint32_t x) {
    v.push_back(static_cast<uint8_t>(x & 0xFF));
    v.push_back(static_cast<uint8_t>((x >> 8) & 0xFF));
    v.push_back(static_cast<uint8_t>((x >> 16) & 0xFF));
    v.push_back(static_cast<uint8_t>((x >> 24) & 0xFF));
}
inline uint32_t GetU32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0])
         | (static_cast<uint32_t>(p[1]) << 8)
         | (static_cast<uint32_t>(p[2]) << 16)
         | (static_cast<uint32_t>(p[3]) << 24);
}
inline void PutU64(std::vector<uint8_t>& v, uint64_t x) {
    for (int i = 0; i < 8; ++i) v.push_back(static_cast<uint8_t>((x >> (8 * i)) & 0xFF));
}
inline uint64_t GetU64(const uint8_t* p) {
    uint64_t x = 0;
    for (int i = 0; i < 8; ++i) x |= static_cast<uint64_t>(p[i]) << (8 * i);
    return x;
}

// テスト / ダミーデータ用: 文字列 -> バイト列。
inline std::vector<uint8_t> Str(const std::string& s) {
    return std::vector<uint8_t>(s.begin(), s.end());
}

// ==========================================================================
// アーカイブ層の圧縮方式フラグ / 1 ファイル分の格納構造
// ==========================================================================
static const uint8_t ALGO_BWT    = 0x00;   // BWT パイプライン
static const uint8_t ALGO_LZSS   = 0x01;   // LZSS 最適構文解析 -> Huffman
static const uint8_t ALGO_DELTA1 = 0x02;   // Delta(stride=1) -> LZSS -> Huffman
static const uint8_t ALGO_DELTA2 = 0x03;   // Delta(stride=2) -> LZSS -> Huffman
static const uint8_t ALGO_DELTA3 = 0x04;   // Delta(stride=3) -> LZSS -> Huffman
static const uint8_t ALGO_DELTA4 = 0x05;   // Delta(stride=4) -> LZSS -> Huffman
static const uint8_t ALGO_BCJ    = 0x06;   // BCJ(x86) -> LZSS -> Huffman (.exe 向け)
static const uint8_t ALGO_WAV    = 0x07;   // WAV(Mid/Side+Delta) -> LZSS -> Huffman
static const uint8_t ALGO_BMP    = 0x08;   // BMP(2D 予測フィルタ) -> LZSS -> Huffman
static const uint8_t ALGO_RAW    = 0x09;   // 生データを直接 Entropy(0次/1次) で符号化
static const uint8_t ALGO_CM     = 0x0A;   // コンテキストミキシング (生データ直接, テキスト/exe 向け)
static const uint8_t ALGO_BCJ_CM = 0x0B;   // BCJ(x86) -> CM (.exe 向け)
static const uint8_t ALGO_WAV_CM = 0x0C;   // WAV(Mid/Side+LPC) 残差 -> CM (音声向け)
static const uint8_t ALGO_BMP_CM  = 0x0D;   // BMP(2D 予測フィルタ) 残差 -> CM (画像向け)
static const uint8_t ALGO_BMP_CM2 = 0x0E;  // BMP(2D 予測) 残差 + チャンネル分離 -> CM
static const uint8_t ALGO_WAV_CM_LEGACY = 0x0F; // WAV 残差 -> CM (WAV_PRIOR/4位相なし。yuuki 等の副作用回避用の候補)
static const uint8_t ALGO_YUUKI_CM = 0x10;       // 800x800 8bit index BMP 専用の列帯域prior + CM
static const uint8_t ALGO_STORE  = 0xFE;   // 無圧縮で格納
// 0x02..0x05 の stride は (algo - ALGO_LZSS) で求まる (0x02->1 ... 0x05->4)

// アーカイブに格納する 1 ファイル分 (data は「圧縮後」のバイト列)
struct StoredFile {
    std::string name;                  // UTF-8 の相対パス
    uint8_t algo = ALGO_BWT;           // 使用した圧縮方式
    uint64_t originalSize = 0;         // 非圧縮サイズ
    std::vector<uint8_t> data;         // 圧縮後データ
};

// wav 同位相order-1文脈 (tWav, st[14]兼用) 追加でCMストリーム非互換のためARC22へ更新。
static const char ARCHIVE_MAGIC[4] = {'A', 'R', 'C', 'M'};  // ARCM = ARC22

// ==========================================================================
// CM プロファイル
//   Encode_CM / Decode_CM のデフォルト引数 (CM_PROF_SLOW) で参照するため、また
//   archive.cpp が algo ごとに使い分けるため、ここで共有する。
//   subShift: order-2/3/4 sub-mixer 文脈のハッシュ右シフト。小さいほど文脈が細かい。
//   strideLen: スパース文脈の刻み幅 (テキスト UTF-8 は 3、x86 exe は dword 整列の 4)。
//   tbits: 文脈テーブル t2..t9 のサイズ指数 (1<<tbits)。
// ==========================================================================
// applyPrior=false のとき、そのプロファイル種別に対応する静的事前確率(WAV_PRIOR 等)と
// 位相別 order-0 分割を無効化し、事前確率導入前(legacy)の挙動を再現する。既存プロファイルは
// 既定 true なのでビットストリーム不変。
// fileKind: ファイル種別の決め打ちID。対象5ファイルは確定しているので、プロファイル→モデル分岐を
// パラメータ組の暗黙判定 (旧: tbits==27 && mixShift==12 && ... ) ではなく明示IDで行う。
enum CMFileKind { CMK_OTHER = 0, CMK_TEXT = 1, CMK_HAL = 2, CMK_EXE = 3, CMK_WAV = 4, CMK_YUUKI = 5 };
struct CMProfile { const int* rate; int mixShift; int apmShift; int subShift; int strideLen; int tbits; bool applyPrior = true; int fileKind = CMK_OTHER; };

static const int CM_RATE_SLOW[16] = {
    43690, 26214, 18724, 14563, 11915, 10082, 8738, 7710,
     6898,  6241,  5461,  4681,  4096,  3500, 3100, 2849
};
static const int CM_RATE_FAST[16] = {   // exe (BCJ_CM) 用: 速い床
    43690, 26214, 18724, 15000, 15000, 15000, 15000, 15000,
    15000, 15000, 15000, 15000, 15000, 15000, 15000, 15000
};
static const int CM_RATE_WAV[16] = {    // 音声 (WAV_CM) 用: やや遅い床 (残差は exe より定常)
    43690, 26214, 18724, 14563, 11915, 10082, 8738, 7710,
     6898,  6241,  5461,  4681,  4096,  4096, 4096, 4096
};
static const int CM_RATE_BMP[16] = {    // 画像 (BMP_CM) 用: 残差は定常なので遅い床
    43690, 26214, 18724, 14563, 11915, 10082, 8738, 7710,
     6898,  6241,  5461,  4096,  3000,  2185, 1820, 1638
};
static const CMProfile CM_PROF_SLOW { CM_RATE_SLOW, 11, 8, 24, 2, 27, true,  CMK_TEXT };  // テキスト (CM)
static const CMProfile CM_PROF_BMP  { CM_RATE_BMP,  12, 8, 24, 3, 27, true,  CMK_HAL };   // 画像 (BMP_CM)
static const CMProfile CM_PROF_FAST { CM_RATE_FAST, 10, 7, 14, 2, 29, true,  CMK_EXE };   // exe (BCJ_CM)
static const CMProfile CM_PROF_WAV  { CM_RATE_WAV,  11, 7, 24, 4, 27, true,  CMK_WAV };   // 音声 (WAV_CM, インターリーブ4B周期)
static const CMProfile CM_PROF_WAV_LEGACY { CM_RATE_WAV, 11, 7, 24, 4, 27, false, CMK_WAV };  // WAV_CM だが prior/位相なし
static const CMProfile CM_PROF_YUUKI { CM_RATE_WAV, 11, 7, 24, 4, 27, false, CMK_YUUKI };     // raw index BMP専用prior

// ==========================================================================
// 各モジュールの公開関数プロトタイプ
// ==========================================================================

// ---- transform.cpp (BWT / MTF / RLE) ----
std::vector<uint8_t> Encode_BWT(const std::vector<uint8_t>& input);
std::vector<uint8_t> Decode_BWT(const std::vector<uint8_t>& input);
std::vector<uint8_t> Encode_MTF(const std::vector<uint8_t>& input);
std::vector<uint8_t> Decode_MTF(const std::vector<uint8_t>& input);
std::vector<uint8_t> Encode_RLE(const std::vector<uint8_t>& input);
std::vector<uint8_t> Decode_RLE(const std::vector<uint8_t>& input);

// ---- huffman.cpp ----
std::vector<uint8_t> Encode_Huffman(const std::vector<uint8_t>& input);
std::vector<uint8_t> Decode_Huffman(const std::vector<uint8_t>& input);

// ---- rangecoder.cpp (レンジコーダ 0/1/2 次 + Entropy セレクタ) ----
std::vector<uint8_t> Encode_RangeCoder(const std::vector<uint8_t>& input);
std::vector<uint8_t> Decode_RangeCoder(const std::vector<uint8_t>& input);
std::vector<uint8_t> Encode_RangeCoderO1(const std::vector<uint8_t>& input);
std::vector<uint8_t> Decode_RangeCoderO1(const std::vector<uint8_t>& input);
std::vector<uint8_t> Encode_RangeCoderO2(const std::vector<uint8_t>& input);
std::vector<uint8_t> Decode_RangeCoderO2(const std::vector<uint8_t>& input);
std::vector<uint8_t> Encode_Entropy(const std::vector<uint8_t>& input);
std::vector<uint8_t> Decode_Entropy(const std::vector<uint8_t>& input);

// ---- cm.cpp (コンテキストミキシング) ----
std::vector<uint8_t> Encode_CM(const std::vector<uint8_t>& input, const CMProfile& prof = CM_PROF_SLOW);
std::vector<uint8_t> Decode_CM(const std::vector<uint8_t>& input, const CMProfile& prof = CM_PROF_SLOW);

// ---- lzss.cpp ----
void     LzPutLEB(std::vector<uint8_t>& v, uint32_t x);
uint32_t LzGetLEB(const uint8_t* data, size_t& i);
std::vector<uint8_t> Encode_LZSS_Greedy(const std::vector<uint8_t>& input);
std::vector<uint8_t> Decode_LZSS(const std::vector<uint8_t>& input);
std::vector<uint8_t> Encode_LZSS_Optimal(const std::vector<uint8_t>& input);
std::vector<uint8_t> Encode_LZSS_Split(const std::vector<uint8_t>& input);
std::vector<uint8_t> Decode_LZSS_Split(const std::vector<uint8_t>& input);
std::vector<uint8_t> Encode_LZSS_4Stream(const std::vector<uint8_t>& input);
std::vector<uint8_t> Decode_LZSS_4Stream(const std::vector<uint8_t>& in);
std::vector<uint8_t> CompressLZSS(const std::vector<uint8_t>& filtered);
std::vector<uint8_t> DecompressLZSS(const std::vector<uint8_t>& in);

// ---- pipeline.cpp (ブロック分割 BWT パイプライン) ----
std::vector<uint8_t> Pipeline_Encode(const std::vector<uint8_t>& input);
std::vector<uint8_t> Pipeline_Decode(const std::vector<uint8_t>& input);

// ---- filters.cpp (Delta / BCJ / WAV / BMP) ----
std::vector<uint8_t> Encode_Delta(const std::vector<uint8_t>& in, int stride);
std::vector<uint8_t> Decode_Delta(const std::vector<uint8_t>& in, int stride);
std::vector<uint8_t> Encode_BCJ(const std::vector<uint8_t>& in);
std::vector<uint8_t> Decode_BCJ(const std::vector<uint8_t>& in);
std::vector<uint8_t> Encode_Wav_MidSide_Delta(const std::vector<uint8_t>& in, int stereoMode = 0);
std::vector<uint8_t> Decode_Wav_MidSide_Delta(const std::vector<uint8_t>& in);
std::vector<uint8_t> Encode_Bmp_2DPredict(const std::vector<uint8_t>& in);
std::vector<uint8_t> Decode_Bmp_2DPredict(const std::vector<uint8_t>& in);
std::vector<uint8_t> BmpSeparateChannels(const std::vector<uint8_t>& in);
std::vector<uint8_t> BmpJoinChannels(const std::vector<uint8_t>& in);

// ---- io.cpp (ファイル / パス) ----
bool ReadFile(const std::string& path, std::vector<uint8_t>& out);
bool WriteFile(const std::string& path, const std::vector<uint8_t>& data);
bool ReadFileFs(const std::filesystem::path& path, std::vector<uint8_t>& out);
bool WriteFileFs(const std::filesystem::path& path, const std::vector<uint8_t>& data);
std::string PathToUtf8(const std::filesystem::path& p);
std::filesystem::path Utf8ToPath(const std::string& s);

// ---- archive.cpp (ARC4 コンテナ / ファイル単位圧縮 / フォルダ圧縮・復元) ----
std::vector<uint8_t> CompressOne(uint8_t algo, const std::vector<uint8_t>& in);
std::vector<uint8_t> DecompressOne(uint8_t algo, const std::vector<uint8_t>& in, uint64_t originalSize);
std::vector<uint8_t> BuildArchive(const std::vector<StoredFile>& files);
bool ParseArchive(const std::vector<uint8_t>& buf, std::vector<StoredFile>& out);
bool CompressFolder(const fs::path& inputDir, const fs::path& outputFile);
bool DecompressArchive(const fs::path& inputFile, const fs::path& outputDir);

// ---- selftest.cpp ----
bool RunSelfTests();
