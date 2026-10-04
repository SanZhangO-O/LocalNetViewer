#include "lnv/util/sha1.h"
#include <cstdio>

namespace lnv {

std::string stable_hash(const std::string& value, size_t length) {
    Sha1 sha;
    sha.update(value.data(), value.size());
    std::string hex = sha.hexdigest();
    if (length < 8) length = 8;
    if (length > hex.size()) length = hex.size();
    return hex.substr(0, length);
}

}  // namespace lnv
