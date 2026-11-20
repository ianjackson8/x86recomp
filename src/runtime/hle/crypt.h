#pragma once
// MD5 (RFC 1321) and RC4, for the CryptoAPI subset in advapi32_crypt.cpp.
// Pure and portable; tests/crypt_test.cpp checks them against RFC 1321's
// test suite and RFC 6229's RC4 keystreams.

#include <array>
#include <cstdint>
#include <cstring>
#include <string>

namespace recomp::hle::crypt {

class Md5 {
 public:
  Md5() { Reset(); }
  void Reset() {
    s_ = {0x67452301u, 0xefcdab89u, 0x98badcfeu, 0x10325476u};
    len_ = 0;
    used_ = 0;
  }
  void Update(const uint8_t* p, size_t n) {
    len_ += n;
    while (n) {
      size_t k = std::min(n, size_t(64) - used_);
      std::memcpy(buf_ + used_, p, k);
      used_ += k;
      p += k;
      n -= k;
      if (used_ == 64) {
        Block(buf_);
        used_ = 0;
      }
    }
  }
  // The digest of everything so far (the state is left as it was).
  std::array<uint8_t, 16> Digest() const {
    Md5 t = *this;
    const uint64_t bits = t.len_ * 8;
    const uint8_t pad = 0x80, zero = 0;
    t.Update(&pad, 1);
    while (t.used_ != 56) t.Update(&zero, 1);
    uint8_t l[8];
    for (int i = 0; i < 8; i++) l[i] = uint8_t(bits >> (8 * i));
    t.Update(l, 8);
    std::array<uint8_t, 16> d;
    for (int i = 0; i < 4; i++)
      for (int b = 0; b < 4; b++) d[4 * i + b] = uint8_t(t.s_[i] >> (8 * b));
    return d;
  }

 private:
  static uint32_t Rol(uint32_t x, int c) { return (x << c) | (x >> (32 - c)); }
  void Block(const uint8_t* p) {
    static const uint32_t k[64] = {
        0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613,
        0xfd469501, 0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193,
        0xa679438e, 0x49b40821, 0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d,
        0x02441453, 0xd8a1e681, 0xe7d3fbc8, 0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed,
        0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a, 0xfffa3942, 0x8771f681, 0x6d9d6122,
        0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70, 0x289b7ec6, 0xeaa127fa,
        0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665, 0xf4292244,
        0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
        0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb,
        0xeb86d391};
    static const int r[64] = {7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
                              5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20,
                              4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
                              6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};
    uint32_t m[16];
    for (int i = 0; i < 16; i++)
      m[i] = uint32_t(p[4 * i]) | uint32_t(p[4 * i + 1]) << 8 | uint32_t(p[4 * i + 2]) << 16 |
             uint32_t(p[4 * i + 3]) << 24;
    uint32_t a = s_[0], b = s_[1], c = s_[2], d = s_[3];
    for (int i = 0; i < 64; i++) {
      uint32_t f;
      int g;
      if (i < 16) f = (b & c) | (~b & d), g = i;
      else if (i < 32) f = (d & b) | (~d & c), g = (5 * i + 1) % 16;
      else if (i < 48) f = b ^ c ^ d, g = (3 * i + 5) % 16;
      else f = c ^ (b | ~d), g = (7 * i) % 16;
      const uint32_t t = d;
      d = c;
      c = b;
      b = b + Rol(a + f + k[i] + m[g], r[i]);
      a = t;
    }
    s_[0] += a, s_[1] += b, s_[2] += c, s_[3] += d;
  }
  std::array<uint32_t, 4> s_;
  uint64_t len_;
  uint8_t buf_[64];
  size_t used_;
};

class Rc4 {
 public:
  explicit Rc4(const std::string& key = std::string()) { SetKey(key); }
  void SetKey(const std::string& key) {
    key_ = key;
    Reset();
  }
  // Back to the key's initial state.
  void Reset() {
    for (int i = 0; i < 256; i++) s_[i] = uint8_t(i);
    i_ = j_ = 0;
    if (key_.empty()) return;
    uint8_t j = 0;
    for (int i = 0; i < 256; i++) {
      j = uint8_t(j + s_[i] + uint8_t(key_[size_t(i) % key_.size()]));
      std::swap(s_[i], s_[j]);
    }
  }
  void Apply(uint8_t* p, size_t n) {
    for (size_t k = 0; k < n; k++) {
      i_ = uint8_t(i_ + 1);
      j_ = uint8_t(j_ + s_[i_]);
      std::swap(s_[i_], s_[j_]);
      p[k] ^= s_[uint8_t(s_[i_] + s_[j_])];
    }
  }

 private:
  std::string key_;
  uint8_t s_[256];
  uint8_t i_ = 0, j_ = 0;
};

}  // namespace recomp::hle::crypt
