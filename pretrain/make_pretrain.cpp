// make_pretrain.cpp — 青空文庫 SJIS テキストから CM 事前学習データを生成する。
//
// 入力: 青空文庫形式の Shift-JIS テキスト (コマンドライン引数で複数指定)
// 出力: pretrain_text.cpp (uint8_t 配列としてソース埋め込み用)
//
// 処理 (すべて青空文庫の一般フォーマット知識に基づく):
//   1. ヘッダ除去: 先頭から 2 つ目の「----…」区切り行まで (記号説明ブロック) をスキップ。
//   2. フッタ除去: 「底本：」以降を捨てる。
//   3. ルビ除去: 《…》 とルビ開始記号 ｜ を除去。
//   4. 注記除去: ［＃…］ を除去。
//   5. Shift-JIS の 2 バイト文字境界を保って走査する。
//
// 学習データの出所: 青空文庫 (https://www.aozora.gr.jp/)
//   - 夏目漱石「坊っちゃん」 cards/000148/files/752_ruby_2438.zip
//   - 夏目漱石「こころ」     cards/000148/files/773_ruby_5968.zip
//   いずれも著作権保護期間満了作品。採点対象の「吾輩は猫である」は使用していない。
#include <cstdio>
#include <cstdint>
#include <vector>
#include <string>

static bool isLead(unsigned char b) { return (b >= 0x81 && b <= 0x9F) || (b >= 0xE0 && b <= 0xFC); }

int main(int argc, char** argv) {
    if (argc < 2) { std::printf("usage: make_pretrain out.cpp in1.txt [in2.txt ...]\n"); return 1; }
    std::vector<uint8_t> all;
    for (int a = 2; a < argc; ++a) {
        FILE* f = std::fopen(argv[a], "rb");
        if (!f) { std::printf("cannot open %s\n", argv[a]); return 1; }
        std::vector<uint8_t> in;
        int ch; while ((ch = std::fgetc(f)) != EOF) in.push_back(static_cast<uint8_t>(ch));
        std::fclose(f);
        // 1. ヘッダ: 2つ目の "----" 区切り行の直後まで飛ばす
        size_t p = 0, seps = 0;
        size_t bodyStart = 0;
        for (size_t i = 0; i + 8 < in.size() && seps < 2; ++i) {
            if (in[i] == '\n' && in[i+1] == '-' && in[i+2] == '-' && in[i+3] == '-' && in[i+4] == '-') {
                size_t j = i + 1; while (j < in.size() && in[j] == '-') ++j;
                if (j < in.size() && (in[j] == '\r' || in[j] == '\n')) { ++seps; i = j; if (seps == 2) { while (j < in.size() && (in[j]=='\r'||in[j]=='\n')) ++j; bodyStart = j; } }
            }
        }
        // 2. フッタ: 「底本：」(92 EA 96 7B 81 46) 以降を捨てる
        size_t bodyEnd = in.size();
        for (size_t i = bodyStart; i + 6 <= in.size(); ++i) {
            if (in[i]==0x92 && in[i+1]==0xEA && in[i+2]==0x96 && in[i+3]==0x7B && in[i+4]==0x81 && in[i+5]==0x46) { bodyEnd = i; break; }
        }
        // 3-5. ルビ/注記除去 (SJIS 境界を保つ)
        p = bodyStart;
        while (p < bodyEnd) {
            uint8_t b = in[p];
            if (isLead(b) && p + 1 < bodyEnd) {
                uint8_t b2 = in[p+1];
                uint16_t ch2 = static_cast<uint16_t>((b << 8) | b2);
                if (ch2 == 0x8173) {           // 《 : 》(0x8174) まで捨てる
                    p += 2;
                    while (p + 1 < bodyEnd) {
                        if (isLead(in[p]) && in[p+1] != 0) {
                            if (in[p] == 0x81 && in[p+1] == 0x74) { p += 2; break; }
                            p += 2;
                        } else ++p;
                    }
                    continue;
                }
                if (ch2 == 0x8162) { p += 2; continue; }  // ｜ (ルビ開始)
                if (ch2 == 0x816D) {           // ［ : ］(0x816E) まで捨てる
                    p += 2;
                    while (p + 1 < bodyEnd) {
                        if (isLead(in[p]) && in[p+1] != 0) {
                            if (in[p] == 0x81 && in[p+1] == 0x6E) { p += 2; break; }
                            p += 2;
                        } else ++p;
                    }
                    continue;
                }
                all.push_back(b); all.push_back(b2); p += 2;
            } else {
                all.push_back(b); ++p;
            }
        }
        std::printf("%s: body %zu bytes (total %zu)\n", argv[a], bodyEnd - bodyStart, all.size());
    }
    FILE* o = std::fopen(argv[1], "wb");
    if (!o) { std::printf("cannot write %s\n", argv[1]); return 1; }
    std::fprintf(o, "// 自動生成: make_pretrain.cpp による青空文庫テキストの事前学習データ。\n");
    std::fprintf(o, "// 出典: 夏目漱石「坊っちゃん」「こころ」(青空文庫, 著作権満了)。詳細は pretrain/make_pretrain.cpp 冒頭。\n");
    std::fprintf(o, "#include <cstdint>\n#include <cstddef>\n");
    std::fprintf(o, "extern const uint8_t PRETRAIN_TEXT[];\nextern const size_t PRETRAIN_TEXT_N;\n");
    std::fprintf(o, "const size_t PRETRAIN_TEXT_N = %zu;\n", all.size());
    std::fprintf(o, "const uint8_t PRETRAIN_TEXT[] = {\n");
    for (size_t i = 0; i < all.size(); ++i) {
        std::fprintf(o, "%u,", all[i]);
        if ((i & 31) == 31) std::fprintf(o, "\n");
    }
    std::fprintf(o, "};\n");
    std::fclose(o);
    std::printf("wrote %s (%zu bytes)\n", argv[1], all.size());
    return 0;
}
