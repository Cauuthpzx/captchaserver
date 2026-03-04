#pragma once

#include <string>
#include <vector>
#include <cstdint>

namespace agent {

std::vector<uint8_t> base64_decode(const std::string& encoded);

} // namespace agent
