// MD5 实现（Windows BCrypt，无需额外 vendored 代码）
#include "lnv/util/md5.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <bcrypt.h>

namespace lnv {

std::string md5_hex(const std::string& data) {
    BCRYPT_ALG_HANDLE alg = nullptr;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_MD5_ALGORITHM, nullptr, 0) != 0) return {};
    std::string out;
    BCRYPT_HASH_HANDLE hash = nullptr;
    if (BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) == 0) {
        const PUCHAR buf = reinterpret_cast<PUCHAR>(const_cast<char*>(data.data()));
        if (BCryptHashData(hash, buf, static_cast<ULONG>(data.size()), 0) == 0) {
            unsigned char digest[16] = {0};
            if (BCryptFinishHash(hash, digest, sizeof(digest), 0) == 0) {
                static const char* kHex = "0123456789abcdef";
                out.reserve(32);
                for (unsigned char c : digest) {
                    out.push_back(kHex[c >> 4]);
                    out.push_back(kHex[c & 0x0F]);
                }
            }
        }
        BCryptDestroyHash(hash);
    }
    BCryptCloseAlgorithmProvider(alg, 0);
    return out;
}

}  // namespace lnv
