#include "compress.h"
#include <cstdio>
int main() {
    std::printf("step 1: start\n"); std::fflush(stdout);
    {
        std::vector<uint8_t> tiny = {0x41,0x42,0x43};
        auto enc = Encode_CM(tiny);
        std::printf("step 2: CM encode OK size=%zu\n", enc.size()); std::fflush(stdout);
        auto dec = Decode_CM(enc);
        std::printf("step 3: CM decode OK match=%d\n", (dec==tiny)); std::fflush(stdout);
    }
    std::printf("done\n"); std::fflush(stdout);
    return 0;
}
