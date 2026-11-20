// advapi32: the CryptoAPI subset games use to hash and obscure data:
// ephemeral (CRYPT_VERIFYCONTEXT) RSA providers, MD5 hashes, RC4 keys
// derived from a hash (crypt.h). Anything else is a fatal error naming it.
//
// For PROV_RSA_FULL with no provider name Windows uses its default provider
// for the type (the Microsoft Strong Cryptographic Provider), whose RC4 key
// is 128 bits unless the flags give a length: CryptDeriveKey then takes the
// first 16 bytes of the hash value, all of MD5's. Other lengths (salted on
// some providers) are fatal, not guessed. RC4 is a stream cipher: the
// keystream continues across CryptEncrypt/CryptDecrypt calls until one with
// Final set, after which the key starts over.

#include <map>
#include <memory>
#include <mutex>
#include <string>

#include "runtime/hle/crypt.h"
#include "runtime/hle/hle.h"
#include "runtime/hle/win32.h"
#include "runtime/log.h"
#include "runtime/runtime.h"

namespace recomp::hle {
namespace {

constexpr uint32_t kProvRsaFull = 1, kProvRsaAes = 24, kVerifyContext = 0xF0000000;
constexpr uint32_t kCalgMd5 = 0x8003, kCalgRc4 = 0x6801;
constexpr uint32_t kHpAlgId = 1, kHpHashVal = 2, kHpHashSize = 4;
constexpr uint32_t kErrorMoreData = 234;
constexpr uint32_t kNteBadHashState = 0x8009000C, kNteBadHash = 0x80090002,
                   kNteBadKey = 0x80090003, kNteBadUid = 0x80090001;

struct Hash {
  crypt::Md5 md5;
  bool finished = false;  // its value has been read: no more data
};
struct Key {
  crypt::Rc4 rc4;
};

std::mutex g_mu;
uint32_t g_next = 0x100;
std::map<uint32_t, int> g_provs;
std::map<uint32_t, std::shared_ptr<Hash>> g_hashes;
std::map<uint32_t, std::shared_ptr<Key>> g_keys;

uint32_t NewHandle() { return g_next += 4; }

std::shared_ptr<Hash> HashOf(uint32_t h) {
  auto it = g_hashes.find(h);
  return it == g_hashes.end() ? nullptr : it->second;
}
std::shared_ptr<Key> KeyOf(uint32_t h) {
  auto it = g_keys.find(h);
  return it == g_keys.end() ? nullptr : it->second;
}

// CryptEncrypt/CryptDecrypt(key, hash, final, flags, data, &len, ...)
uint32_t Rc4Crypt(Ctx& c, const char* fn) {
  std::lock_guard<std::mutex> l(g_mu);
  auto k = KeyOf(uint32_t(Arg(c, 0)));
  if (!k) {
    SetLastError(kNteBadKey);
    return 0;
  }
  if (Arg(c, 1)) FatalF("%s: hashing while encrypting is not supported", fn);
  const gaddr data = Arg(c, 4), plen = Arg(c, 5);
  if (data) k->rc4.Apply(GPtr(data), Rd32(plen));
  if (Arg(c, 2)) k->rc4.Reset();  // Final: the next call starts over
  return 1;
}

}  // namespace

// CryptAcquireContextA(&prov, container, provider, type, flags)
HLE64("advapi32.dll", CryptAcquireContextA, 5) {
  const uint32_t type = uint32_t(Arg(c, 3)), flags = uint32_t(Arg(c, 4));
  if ((type != kProvRsaFull && type != kProvRsaAes) || !(flags & kVerifyContext) || Arg(c, 1))
    FatalF("CryptAcquireContextA: provider type %u, flags %08X%s not supported", type, flags,
           Arg(c, 1) ? ", a key container" : "");
  if (Arg(c, 2)) FatalF("CryptAcquireContextA: a named provider (%s) not supported",
                        GStr(Arg(c, 2)).c_str());
  std::lock_guard<std::mutex> l(g_mu);
  const uint32_t h = NewHandle();
  g_provs[h] = int(type);
  WrPtr(Arg(c, 0), h);
  return 1;
}
HLE64("advapi32.dll", CryptReleaseContext, 2) {
  std::lock_guard<std::mutex> l(g_mu);
  if (!g_provs.erase(uint32_t(Arg(c, 0)))) {
    SetLastError(kNteBadUid);
    return 0;
  }
  return 1;
}

// CryptCreateHash(prov, alg, key, flags, &hash)
HLE64("advapi32.dll", CryptCreateHash, 5) {
  const uint32_t alg = uint32_t(Arg(c, 1));
  if (alg != kCalgMd5 || Arg(c, 2))
    FatalF("CryptCreateHash: algorithm %04X%s not supported", alg, Arg(c, 2) ? " (keyed)" : "");
  std::lock_guard<std::mutex> l(g_mu);
  if (!g_provs.count(uint32_t(Arg(c, 0)))) {
    SetLastError(kNteBadUid);
    return 0;
  }
  const uint32_t h = NewHandle();
  g_hashes[h] = std::make_shared<Hash>();
  WrPtr(Arg(c, 4), h);
  return 1;
}
HLE64("advapi32.dll", CryptHashData, 4) {
  std::lock_guard<std::mutex> l(g_mu);
  auto h = HashOf(uint32_t(Arg(c, 0)));
  if (!h) {
    SetLastError(kNteBadHash);
    return 0;
  }
  if (h->finished) {
    SetLastError(kNteBadHashState);
    return 0;
  }
  h->md5.Update(GPtr(Arg(c, 1)), uint32_t(Arg(c, 2)));
  return 1;
}
// CryptGetHashParam(hash, param, data, &len, flags)
HLE64("advapi32.dll", CryptGetHashParam, 5) {
  std::lock_guard<std::mutex> l(g_mu);
  auto h = HashOf(uint32_t(Arg(c, 0)));
  if (!h) {
    SetLastError(kNteBadHash);
    return 0;
  }
  const uint32_t param = uint32_t(Arg(c, 1));
  const gaddr data = Arg(c, 2), plen = Arg(c, 3);
  uint8_t out[16];
  uint32_t n;
  if (param == kHpHashVal) {
    auto d = h->md5.Digest();
    std::memcpy(out, d.data(), 16);
    n = 16;
  } else if (param == kHpHashSize || param == kHpAlgId) {
    const uint32_t v = param == kHpHashSize ? 16 : kCalgMd5;
    std::memcpy(out, &v, 4);
    n = 4;
  } else {
    FatalF("CryptGetHashParam: parameter %u not supported", param);
  }
  if (!data) {
    Wr32(plen, n);
    return 1;
  }
  if (Rd32(plen) < n) {
    Wr32(plen, n);
    SetLastError(kErrorMoreData);
    return 0;
  }
  std::memcpy(GPtr(data), out, n);
  Wr32(plen, n);
  if (param == kHpHashVal) h->finished = true;
  return 1;
}
// CryptDuplicateHash(hash, reserved, flags, &new)
HLE64("advapi32.dll", CryptDuplicateHash, 4) {
  std::lock_guard<std::mutex> l(g_mu);
  auto h = HashOf(uint32_t(Arg(c, 0)));
  if (!h) {
    SetLastError(kNteBadHash);
    return 0;
  }
  const uint32_t n = NewHandle();
  g_hashes[n] = std::make_shared<Hash>(*h);
  WrPtr(Arg(c, 3), n);
  return 1;
}
HLE64("advapi32.dll", CryptDestroyHash, 1) {
  std::lock_guard<std::mutex> l(g_mu);
  if (!g_hashes.erase(uint32_t(Arg(c, 0)))) {
    SetLastError(kNteBadHash);
    return 0;
  }
  return 1;
}

// CryptDeriveKey(prov, alg, hash, flags, &key): flags' upper 16 bits are
// the key length in bits (0: the default, 128 for RC4).
HLE64("advapi32.dll", CryptDeriveKey, 5) {
  const uint32_t alg = uint32_t(Arg(c, 1)), flags = uint32_t(Arg(c, 3));
  const uint32_t bits = (flags >> 16) ? (flags >> 16) : 128;
  if (alg != kCalgRc4 || bits != 128)
    FatalF("CryptDeriveKey: algorithm %04X with %u-bit keys not supported", alg, bits);
  std::lock_guard<std::mutex> l(g_mu);
  auto h = HashOf(uint32_t(Arg(c, 2)));
  if (!h) {
    SetLastError(kNteBadHash);
    return 0;
  }
  auto d = h->md5.Digest();
  h->finished = true;
  auto k = std::make_shared<Key>();
  k->rc4.SetKey(std::string(reinterpret_cast<const char*>(d.data()), 16));
  const uint32_t n = NewHandle();
  g_keys[n] = k;
  WrPtr(Arg(c, 4), n);
  return 1;
}
HLE64("advapi32.dll", CryptDestroyKey, 1) {
  std::lock_guard<std::mutex> l(g_mu);
  if (!g_keys.erase(uint32_t(Arg(c, 0)))) {
    SetLastError(kNteBadKey);
    return 0;
  }
  return 1;
}
// CryptEncrypt(key, hash, final, flags, data, &len, buflen)
HLE64("advapi32.dll", CryptEncrypt, 7) { return Rc4Crypt(c, "CryptEncrypt"); }
// CryptDecrypt(key, hash, final, flags, data, &len)
HLE64("advapi32.dll", CryptDecrypt, 6) { return Rc4Crypt(c, "CryptDecrypt"); }

}  // namespace recomp::hle
