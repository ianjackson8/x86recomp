// rpcrt4: the UUID helpers games call (string <-> UUID, a fresh random
// UUID). A UUID in memory is {uint32 Data1; uint16 Data2, Data3;
// uint8 Data4[8]}, little-endian; its string form is
// xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx (no braces), lowercase from
// UuidToString.

#include <cstdint>
#include <cstdio>
#include <random>
#include <string>

#include "runtime/hle/hle.h"
#include "runtime/hle/kernel32_state.h"
#include "runtime/runtime.h"

namespace recomp::hle {
namespace {

constexpr uint32_t kRpcOk = 0;
constexpr uint32_t kRpcInvalidStringUuid = 1705;  // RPC_S_INVALID_STRING_UUID

int HexDigit(char ch) {
  if (ch >= '0' && ch <= '9') return ch - '0';
  if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
  if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
  return -1;
}

// The 16 bytes of a UUID's in-memory layout from its string form.
bool ParseUuid(const char* s, uint8_t out[16]) {
  // Character offsets of each byte's two digits, in string order.
  static const int kPos[16] = {0, 2, 4, 6, 9, 11, 14, 16, 19, 21, 24, 26, 28, 30, 32, 34};
  for (int i = 0; i < 36; i++) {
    if (!s[i]) return false;
    const bool dash = i == 8 || i == 13 || i == 18 || i == 23;
    if (dash ? s[i] != '-' : HexDigit(s[i]) < 0) return false;
  }
  if (s[36]) return false;
  uint8_t b[16];
  for (int i = 0; i < 16; i++) b[i] = uint8_t(HexDigit(s[kPos[i]]) << 4 | HexDigit(s[kPos[i] + 1]));
  // Data1-3 are stored little-endian; Data4 as written.
  const int kOrder[16] = {3, 2, 1, 0, 5, 4, 7, 6, 8, 9, 10, 11, 12, 13, 14, 15};
  for (int i = 0; i < 16; i++) out[i] = b[kOrder[i]];
  return true;
}

std::string FormatUuid(const uint8_t u[16]) {
  char buf[37];
  std::snprintf(buf, sizeof buf,
                "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x", u[3],
                u[2], u[1], u[0], u[5], u[4], u[7], u[6], u[8], u[9], u[10], u[11], u[12], u[13],
                u[14], u[15]);
  return buf;
}

}  // namespace

// UuidFromStringA(string, uuid): NULL is the nil UUID.
HLE64("rpcrt4.dll", UuidFromStringA, 2) {
  const gaddr str = Arg(c, 0), uuid = Arg(c, 1);
  uint8_t u[16] = {};
  if (str && !ParseUuid(g_proc->mem.ptr<const char>(str), u)) return kRpcInvalidStringUuid;
  for (int i = 0; i < 16; i++) Wr8(uuid + i, u[i]);
  return kRpcOk;
}

// UuidToStringA(uuid, &string): the string is RpcStringFreeA's to free.
HLE64("rpcrt4.dll", UuidToStringA, 2) {
  uint8_t u[16];
  for (int i = 0; i < 16; i++) u[i] = Rd8(Arg(c, 0) + i);
  const std::string s = FormatUuid(u);
  gaddr p = HeapAllocProcess(s.size() + 1, false);
  PutStr(p, s);
  WrPtr(Arg(c, 1), p);
  return kRpcOk;
}

// RpcStringFreeA(&string): frees it and clears the pointer.
HLE64("rpcrt4.dll", RpcStringFreeA, 1) {
  const gaddr pp = Arg(c, 0);
  if (gaddr p = RdPtr(pp)) HeapFreeProcess(p);
  WrPtr(pp, 0);
  return kRpcOk;
}

// UuidCreate(uuid): a random (version 4, RFC 4122 variant) UUID.
HLE64("rpcrt4.dll", UuidCreate, 1) {
  static thread_local std::mt19937_64 rng{std::random_device{}()};
  uint8_t u[16];
  for (int i = 0; i < 16; i += 8) {
    uint64_t r = rng();
    for (int k = 0; k < 8; k++) u[i + k] = uint8_t(r >> (8 * k));
  }
  u[7] = uint8_t((u[7] & 0x0F) | 0x40);  // Data3's high nibble: version 4
  u[8] = uint8_t((u[8] & 0x3F) | 0x80);  // Data4[0]: variant 10
  for (int i = 0; i < 16; i++) Wr8(Arg(c, 0) + i, u[i]);
  return kRpcOk;
}

}  // namespace recomp::hle
