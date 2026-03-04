#include "base64.h"

namespace agent {

static constexpr uint8_t k_DecodeTable[] = {
    64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,
    64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,
    64,64,64,64,64,64,64,64,64,64,64,62,64,64,64,63,
    52,53,54,55,56,57,58,59,60,61,64,64,64,64,64,64,
    64, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,
    15,16,17,18,19,20,21,22,23,24,25,64,64,64,64,64,
    64,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,
    41,42,43,44,45,46,47,48,49,50,51,64,64,64,64,64,
};

std::vector<uint8_t> base64_decode(const std::string& encoded) {
    if (encoded.empty()) return {};

    size_t in_len = encoded.size();
    size_t padding = 0;
    if (in_len >= 1 && encoded[in_len - 1] == '=') padding++;
    if (in_len >= 2 && encoded[in_len - 2] == '=') padding++;

    size_t out_len = (in_len / 4) * 3 - padding;
    std::vector<uint8_t> result;
    result.reserve(out_len);

    uint32_t buf = 0;
    int bits = 0;

    for (char c : encoded) {
        if (c == '=' || c == '\n' || c == '\r') continue;
        auto uc = static_cast<unsigned char>(c);
        if (uc >= 128) continue;
        uint8_t val = k_DecodeTable[uc];
        if (val == 64) continue;

        buf = (buf << 6) | val;
        bits += 6;

        if (bits >= 8) {
            bits -= 8;
            result.push_back(static_cast<uint8_t>((buf >> bits) & 0xFF));
        }
    }

    return result;
}

} // namespace agent
