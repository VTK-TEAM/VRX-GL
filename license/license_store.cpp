#include "license/license_store.hpp"

#include <fstream>

namespace vrx::license {

bool read_signature(const std::string& store_path, uint8_t out_sig[64]) {
    std::ifstream f(store_path, std::ios::binary);
    if (!f.is_open()) return false;
    f.seekg(kSignatureOffset, std::ios::beg);
    if (!f) return false;
    f.read(reinterpret_cast<char*>(out_sig), kSignatureLen);
    return f.gcount() == kSignatureLen;
}

} // namespace vrx::license
