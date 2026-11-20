// MD5 against RFC 1321's test suite and RC4 against RFC 6229's keystreams
// (runtime/hle/crypt.h). Portable.

#include <cstdio>
#include <string>

#include "runtime/hle/crypt.h"

using namespace recomp::hle::crypt;
static int g_fail = 0;

static std::string Hex(const uint8_t* p, size_t n) {
  static const char* d = "0123456789abcdef";
  std::string s;
  for (size_t i = 0; i < n; i++) s += d[p[i] >> 4], s += d[p[i] & 15];
  return s;
}

int main() {
  const char* md5[][2] = {  // RFC 1321, A.5
      {"", "d41d8cd98f00b204e9800998ecf8427e"},
      {"a", "0cc175b9c0f1b6a831c399e269772661"},
      {"abc", "900150983cd24fb0d6963f7d28e17f72"},
      {"message digest", "f96b697d7cb7938d525a2f31aaf161d0"},
      {"abcdefghijklmnopqrstuvwxyz", "c3fcd3d76192e4007dfb496cca67e13b"},
      {"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789",
       "d174ab98d277d9f5a5611c2c9f419d9f"},
      {"12345678901234567890123456789012345678901234567890123456789012345678901234567890",
       "57edf4a22be3c955ac49da2e2107b67a"},
  };
  for (auto& t : md5) {
    Md5 m;
    std::string in = t[0];
    // In two pieces, to cross block boundaries.
    m.Update(reinterpret_cast<const uint8_t*>(in.data()), in.size() / 2);
    m.Update(reinterpret_cast<const uint8_t*>(in.data()) + in.size() / 2, in.size() - in.size() / 2);
    auto d = m.Digest();
    if (Hex(d.data(), 16) != t[1]) {
      std::printf("FAIL md5(\"%s\") = %s\n", t[0], Hex(d.data(), 16).c_str());
      g_fail++;
    }
  }
  // RFC 6229: key 0x0102030405, keystream at offsets 0 and 16.
  {
    Rc4 r(std::string("\x01\x02\x03\x04\x05", 5));
    uint8_t z[32] = {};
    r.Apply(z, 32);
    if (Hex(z, 16) != "b2396305f03dc027ccc3524a0a1118a8" ||
        Hex(z + 16, 16) != "6982944f18fc82d589c403a47a0d0919") {
      std::printf("FAIL rc4 40-bit key: %s\n", Hex(z, 32).c_str());
      g_fail++;
    }
  }
  // RFC 6229: 128-bit key 0x0102...10, offset 0; and continuing vs Reset.
  {
    std::string k;
    for (int i = 1; i <= 16; i++) k.push_back(char(i));
    Rc4 r(k);
    uint8_t z[16] = {};
    r.Apply(z, 16);
    if (Hex(z, 16) != "9ac7cc9a609d1ef7b2932899cde41b97") {
      std::printf("FAIL rc4 128-bit key: %s\n", Hex(z, 16).c_str());
      g_fail++;
    }
    r.Reset();
    uint8_t a[16] = {};
    r.Apply(a, 16);
    if (Hex(a, 16) != "9ac7cc9a609d1ef7b2932899cde41b97") {
      std::printf("FAIL rc4 Reset\n");
      g_fail++;
    }
  }
  std::printf("crypt: %d failures\n", g_fail);
  return g_fail ? 1 : 0;
}
