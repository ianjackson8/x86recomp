// urlmon.dll: URLOpenBlockingStreamA, a URL downloaded synchronously
// (libcurl) into a read-only IStream in memory: Read, Seek, Stat and the
// IUnknown methods; the stream's other methods have no handler (fatal,
// named). HTTP errors are INET_E_RESOURCE_NOT_FOUND, others
// INET_E_DOWNLOAD_FAILURE. A bind-status callback is not called.

#include <curl/curl.h>

#include <algorithm>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "runtime/hle/com.h"
#include "runtime/hle/hle.h"
#include "runtime/log.h"

namespace recomp::hle {
namespace {

constexpr uint32_t kSOk = 0, kSFalse = 1, kEInvalidArg = 0x80070057, kENoInterface = 0x80004002;
constexpr uint32_t kInetDownloadFailure = 0x800C0008, kInetResourceNotFound = 0x800C0005;
constexpr guest::GUID kIID_IUnknown = {0x00000000, 0x0000, 0x0000, {0xC0, 0, 0, 0, 0, 0, 0, 0x46}};
constexpr guest::GUID kIID_ISequentialStream = {0x0C733A30, 0x2A1C, 0x11CE, {0xAD, 0xE5, 0x00, 0xAA, 0x00, 0x44, 0x77, 0x3D}};
constexpr guest::GUID kIID_IStream = {0x0000000C, 0x0000, 0x0000, {0xC0, 0, 0, 0, 0, 0, 0, 0x46}};

struct MemStream final : com::Tagged<com::TagOf("urlmon.MemStream")> {
  std::vector<uint8_t> data;
  uint64_t pos = 0;
};

MemStream* Self(Ctx& c) {
  MemStream* s = com::As<MemStream>(uint32_t(Arg(c, 0)));
  if (!s) FatalF("IStream call on an unknown object %08X", uint32_t(Arg(c, 0)));
  return s;
}

size_t Collect(char* p, size_t size, size_t n, void* user) {
  auto* v = static_cast<std::vector<uint8_t>*>(user);
  v->insert(v->end(), p, p + size * n);
  return size * n;
}

}  // namespace

// URLOpenBlockingStreamA(caller, url, &stream, reserved, callback)
HLE("urlmon.dll", URLOpenBlockingStreamA, 5) {
  const gaddr out = Arg(c, 2);
  if (!Arg(c, 1) || !out) return kEInvalidArg;
  Wr32(out, 0);
  const std::string url = GStr(Arg(c, 1));
  if (Arg(c, 4)) {
    static std::once_flag once;
    std::call_once(once, [] { WarnF("URLOpenBlockingStreamA: the bind-status callback is not called\n"); });
  }
  static std::once_flag init;
  std::call_once(init, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
  auto s = std::make_shared<MemStream>();
  CURL* h = curl_easy_init();
  if (!h) return kInetDownloadFailure;
  curl_easy_setopt(h, CURLOPT_URL, url.c_str());
  curl_easy_setopt(h, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, &Collect);
  curl_easy_setopt(h, CURLOPT_WRITEDATA, &s->data);
  curl_easy_setopt(h, CURLOPT_CONNECTTIMEOUT, 15L);
  curl_easy_setopt(h, CURLOPT_TIMEOUT, 60L);
  const CURLcode r = curl_easy_perform(h);
  long status = 0;
  curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, &status);
  curl_easy_cleanup(h);
  RECOMP_TRACE("  URLOpenBlockingStreamA(%s) -> curl %d, HTTP %ld, %zu bytes\n", url.c_str(), int(r),
               status, s->data.size());
  if (r != CURLE_OK) return kInetDownloadFailure;
  if (status >= 400) return kInetResourceNotFound;
  Wr32(out, com::Publish(s, com::Vtable("urlmon.dll", "IStream",
                                        {"QueryInterface", "AddRef", "Release", "Read", "Write",
                                         "Seek", "SetSize", "CopyTo", "Commit", "Revert",
                                         "LockRegion", "UnlockRegion", "Stat", "Clone"})));
  return kSOk;
}

HLE_NAMED("urlmon.dll", "IStream::QueryInterface", MemStream_QueryInterface, 3) {
  Self(c);
  const uint32_t riid = uint32_t(Arg(c, 1)), out = uint32_t(Arg(c, 2));
  if (com::GuidEq(riid, kIID_IUnknown) || com::GuidEq(riid, kIID_ISequentialStream) ||
      com::GuidEq(riid, kIID_IStream)) {
    com::AddRef(uint32_t(Arg(c, 0)));
    Wr32(out, uint32_t(Arg(c, 0)));
    return kSOk;
  }
  Wr32(out, 0);
  return kENoInterface;
}
HLE_NAMED("urlmon.dll", "IStream::AddRef", MemStream_AddRef, 1) { return com::AddRef(uint32_t(Arg(c, 0))); }
HLE_NAMED("urlmon.dll", "IStream::Release", MemStream_Release, 1) { return com::Release(uint32_t(Arg(c, 0))); }
// Read(buf, n, &read): S_FALSE when fewer than n bytes were left.
HLE_NAMED("urlmon.dll", "IStream::Read", MemStream_Read, 4) {
  MemStream* s = Self(c);
  const uint64_t want = uint32_t(Arg(c, 2));
  const uint64_t left = s->pos < s->data.size() ? s->data.size() - s->pos : 0;
  const uint64_t n = std::min(want, left);
  if (n) std::memcpy(GPtr(Arg(c, 1)), s->data.data() + s->pos, size_t(n));
  s->pos += n;
  if (Arg(c, 3)) Wr32(Arg(c, 3), uint32_t(n));
  return n == want ? kSOk : kSFalse;
}
// Seek(move (64-bit, two slots), origin, &new_pos): STREAM_SEEK_SET 0,
// CUR 1, END 2.
HLE_NAMED("urlmon.dll", "IStream::Seek", MemStream_Seek, 5) {
  MemStream* s = Self(c);
  const int64_t move = int64_t(Arg64(c, 1, 1));
  const uint32_t origin = uint32_t(Arg(c, 3));
  int64_t base = origin == 0 ? 0 : origin == 1 ? int64_t(s->pos) : origin == 2 ? int64_t(s->data.size()) : -1;
  if (base < 0 || base + move < 0) return 0x80030019;  // STG_E_INVALIDFUNCTION
  s->pos = uint64_t(base + move);
  if (Arg(c, 4)) Wr64(Arg(c, 4), s->pos);
  return kSOk;
}
// Stat(&statstg, flags): STATSTG {pwcsName, type, cbSize, times, grfMode,
// grfLocksSupported, clsid, grfStateBits, reserved}; no name.
HLE_NAMED("urlmon.dll", "IStream::Stat", MemStream_Stat, 3) {
  MemStream* s = Self(c);
  const gaddr st = Arg(c, 1);
  if (!st) return kEInvalidArg;
  std::memset(GPtr(st), 0, 72);
  Wr32(st + 4, 2);  // STGTY_STREAM
  Wr64(st + 8, s->data.size());
  return kSOk;
}

}  // namespace recomp::hle
