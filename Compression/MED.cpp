#include <vector>
#include <cstdint>
#include <algorithm>

using namespace std;

// ---- 符号化 (エンコード) -----------------------------------------------------
// 生ピクセル値から「予測残差（差分）」を計算する関数
vector<uint8_t> encodeMED(const vector<uint8_t>& image, int width, int height) {
    vector<uint8_t> residuals;
    residuals.reserve(image.size());

    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            int i = y * width + x;
            uint8_t actual = image[i];
            uint8_t predict = 0;

            // 境界処理 (フォールバック)
            if (x == 0 && y == 0) {
                predict = 0; // 左上端
            }
            else if (y == 0) {
                predict = image[i - 1]; // 一番上の行 (左のピクセルを使用)
            }
            else if (x == 0) {
                predict = image[i - width]; // 一番左の列 (上のピクセルを使用)
            }
            else {
                // 通常のMED予測
                uint8_t a = image[i - 1];         // 左
                uint8_t b = image[i - width];     // 上
                uint8_t c = image[i - width - 1]; // 左斜め上

                int max_ab = max(a, b);
                int min_ab = min(a, b);

                if (c >= max_ab) {
                    predict = min_ab;
                }
                else if (c <= min_ab) {
                    predict = max_ab;
                }
                else {
                    predict = a + b - c;
                }
            }

            // 残差の計算 (uint8_tのアンダーフローを利用してラップアラウンド)
            residuals.push_back(static_cast<uint8_t>(actual - predict));
        }
    }

    return residuals;
}

// ---- 復号 (デコード) -------------------------------------------------------
// 予測残差から生ピクセル値を完全に復元する関数
vector<uint8_t> decodeMED(const vector<uint8_t>& residuals, int width, int height) {
    vector<uint8_t> image;
    image.reserve(residuals.size());

    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            int i = y * width + x;
            uint8_t residual = residuals[i];
            uint8_t predict = 0;

            // 境界処理 (エンコード時と全く同じ計算を行う)
            if (x == 0 && y == 0) {
                predict = 0;
            }
            else if (y == 0) {
                predict = image[i - 1];
            }
            else if (x == 0) {
                predict = image[i - width];
            }
            else {
                uint8_t a = image[i - 1];
                uint8_t b = image[i - width];
                uint8_t c = image[i - width - 1];

                int max_ab = max(a, b);
                int min_ab = min(a, b);

                if (c >= max_ab) {
                    predict = min_ab;
                }
                else if (c <= min_ab) {
                    predict = max_ab;
                }
                else {
                    predict = a + b - c;
                }
            }

            // 元ピクセルの復元 (uint8_tのオーバーフローを利用してラップアラウンド)
            image.push_back(static_cast<uint8_t>(predict + residual));
        }
    }

    return image;
}
