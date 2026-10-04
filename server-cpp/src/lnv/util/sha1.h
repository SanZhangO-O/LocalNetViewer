#pragma once
// SHA-1（跨进程稳定哈希 stable_hash 的底层；对应 Python textutil.stable_hash）
#include <cstdint>
#include <cstring>
#include <string>

namespace lnv {

class Sha1 {
public:
    Sha1() { reset(); }
    void reset() {
        h_[0] = 0x67452301; h_[1] = 0xEFCDAB89; h_[2] = 0x98BADCFE;
        h_[3] = 0x10325476; h_[4] = 0xC3D2E1F0;
        len_ = 0; buf_len_ = 0;
    }
    void update(const void* data, size_t n) {
        const uint8_t* p = static_cast<const uint8_t*>(data);
        len_ += n;
        while (n > 0) {
            size_t take = (64 - buf_len_) < n ? (64 - buf_len_) : n;
            std::memcpy(buf_ + buf_len_, p, take);
            buf_len_ += take; p += take; n -= take;
            if (buf_len_ == 64) { block(buf_); buf_len_ = 0; }
        }
    }
    // 输出 40 位小写十六进制
    std::string hexdigest() {
        uint64_t bits = len_ * 8;
        uint8_t pad = 0x80;
        update(&pad, 1);
        uint8_t zero = 0;
        while (buf_len_ != 56) update(&zero, 1);
        uint8_t tail[8];
        for (int i = 0; i < 8; i++) tail[i] = uint8_t(bits >> (56 - 8 * i));
        update(tail, 8);
        // update 会改 len_，但此时已不再使用
        char out[41];
        for (int i = 0; i < 5; i++)
            std::snprintf(out + i * 8, 9, "%08x", h_[i]);
        out[40] = 0;
        return std::string(out, 40);
    }

private:
    static uint32_t rol(uint32_t x, int s) { return (x << s) | (x >> (32 - s)); }
    void block(const uint8_t* p) {
        uint32_t w[80];
        for (int i = 0; i < 16; i++)
            w[i] = (uint32_t(p[i*4]) << 24) | (uint32_t(p[i*4+1]) << 16) |
                   (uint32_t(p[i*4+2]) << 8) | uint32_t(p[i*4+3]);
        for (int i = 16; i < 80; i++)
            w[i] = rol(w[i-3] ^ w[i-8] ^ w[i-14] ^ w[i-16], 1);
        uint32_t a = h_[0], b = h_[1], c = h_[2], d = h_[3], e = h_[4];
        for (int i = 0; i < 80; i++) {
            uint32_t f, k;
            if (i < 20)      { f = (b & c) | ((~b) & d);          k = 0x5A827999; }
            else if (i < 40) { f = b ^ c ^ d;                     k = 0x6ED9EBA1; }
            else if (i < 60) { f = (b & c) | (b & d) | (c & d);   k = 0x8F1BBCDC; }
            else             { f = b ^ c ^ d;                     k = 0xCA62C1D6; }
            uint32_t t = rol(a, 5) + f + e + k + w[i];
            e = d; d = c; c = rol(b, 30); b = a; a = t;
        }
        h_[0] += a; h_[1] += b; h_[2] += c; h_[3] += d; h_[4] += e;
    }
    uint32_t h_[5];
    uint64_t len_;
    uint8_t buf_[64];
    size_t buf_len_;
};

// Python: sha1(str(value).encode("utf-8")).hexdigest()[:max(8, length)]
std::string stable_hash(const std::string& value, size_t length = 16);

}  // namespace lnv
