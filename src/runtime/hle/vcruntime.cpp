// VCRUNTIME140.dll for x64 guests: the memory and string primitives, the
// type_info and std::exception helpers. Its exception handling
// (_CxxThrowException, __CxxFrameHandler4, __C_specific_handler) belongs
// to the x64 unwinder (ARCHITECTURE.md, "64-bit guests").

#include <cstring>
#include <mutex>
#include <string>

#include "runtime/hle/hle.h"
#include "runtime/hle/kernel32_state.h"
#include "runtime/hle/rtti.h"
#include "runtime/hle/undname.h"
#include "runtime/log.h"
#include "runtime/runtime.h"

namespace recomp::hle {
namespace {

const char* S(gaddr p) { return g_proc->mem.ptr<const char>(p); }
// The size of a guest pointer: field offsets in the structures below.
uint32_t PtrSize() { return g_proc->x64 ? 8 : 4; }

}  // namespace

// ---- memory ---------------------------------------------------------------
// Guest memory is one host mapping, so these are the host's on it.

HLE64("vcruntime140.dll", memcpy, kCdecl) {
  std::memmove(GPtr(Arg(c, 0)), GPtr(Arg(c, 1)), Arg(c, 2));
  return Arg(c, 0);
}
HLE64("vcruntime140.dll", memmove, kCdecl) {
  std::memmove(GPtr(Arg(c, 0)), GPtr(Arg(c, 1)), Arg(c, 2));
  return Arg(c, 0);
}
HLE64("vcruntime140.dll", memset, kCdecl) {
  std::memset(GPtr(Arg(c, 0)), int(Arg(c, 1)), Arg(c, 2));
  return Arg(c, 0);
}
HLE64("vcruntime140.dll", memcmp, kCdecl) {
  int r = std::memcmp(GPtr(Arg(c, 0)), GPtr(Arg(c, 1)), Arg(c, 2));
  return uint32_t(r < 0 ? -1 : r > 0 ? 1 : 0);
}
HLE64("vcruntime140.dll", memchr, kCdecl) {
  const uint8_t* p = GPtr(Arg(c, 0));
  const void* r = std::memchr(p, int(uint8_t(Arg(c, 1))), Arg(c, 2));
  return r ? Arg(c, 0) + gaddr(static_cast<const uint8_t*>(r) - p) : 0;
}

// ---- strings ----------------------------------------------------------------

HLE64("vcruntime140.dll", strchr, kCdecl) {
  const char* p = S(Arg(c, 0));
  const char* r = std::strchr(p, int(char(Arg(c, 1))));
  return r ? Arg(c, 0) + gaddr(r - p) : 0;
}
HLE64("vcruntime140.dll", strrchr, kCdecl) {
  const char* p = S(Arg(c, 0));
  const char* r = std::strrchr(p, int(char(Arg(c, 1))));
  return r ? Arg(c, 0) + gaddr(r - p) : 0;
}
HLE64("vcruntime140.dll", strstr, kCdecl) {
  const char* p = S(Arg(c, 0));
  const char* r = std::strstr(p, S(Arg(c, 1)));
  return r ? Arg(c, 0) + gaddr(r - p) : 0;
}

// Wide (UTF-16) searches. wcschr/wcsrchr find c among the string's
// characters including its terminating NUL, as strchr does.
namespace {
uint16_t W(gaddr p, uint64_t i) { return Rd16(p + 2 * i); }
}  // namespace
HLE64("vcruntime140.dll", wcschr, kCdecl) {
  const gaddr p = Arg(c, 0);
  const uint16_t ch = uint16_t(Arg(c, 1));
  for (uint64_t i = 0;; i++) {
    if (W(p, i) == ch) return p + 2 * i;
    if (!W(p, i)) return 0;
  }
}
HLE64("vcruntime140.dll", wcsrchr, kCdecl) {
  const gaddr p = Arg(c, 0);
  const uint16_t ch = uint16_t(Arg(c, 1));
  gaddr found = 0;
  for (uint64_t i = 0;; i++) {
    if (W(p, i) == ch) found = p + 2 * i;
    if (!W(p, i)) return found;
  }
}
HLE64("vcruntime140.dll", wcsstr, kCdecl) {
  const gaddr p = Arg(c, 0), q = Arg(c, 1);
  if (!W(q, 0)) return p;
  for (uint64_t i = 0; W(p, i); i++) {
    uint64_t k = 0;
    while (W(q, k) && W(p, i + k) == W(q, k)) k++;
    if (!W(q, k)) return p + 2 * i;
  }
  return 0;
}

// ---- type_info ----------------------------------------------------------------
// __std_type_info_data {const char* _UndecoratedName; char _DecoratedName[];}
// (after type_info's vtable pointer). Types are equal when their decorated
// names are, past the leading '.'.

HLE64("vcruntime140.dll", __std_type_info_compare, kCdecl) {
  gaddr a = Arg(c, 0), b = Arg(c, 1);
  if (a == b) return 0;
  const uint32_t ps = PtrSize();  // past _UndecoratedName
  int r = std::strcmp(S(a + ps + 1), S(b + ps + 1));
  return uint32_t(r < 0 ? -1 : r > 0 ? 1 : 0);
}

// type_info::name(): the undecorated name, made once per type and kept in
// the descriptor's _UndecoratedName (published under a lock, as the CRT's
// compare-exchange does; a racing thread's copy is freed). Not linked into
// the guest's list (root): the strings live until the process ends, so
// __std_type_info_destroy_list has nothing to free. A decorated name that
// does not demangle is fatal and named, never a guessed text.
HLE64("vcruntime140.dll", __std_type_info_name, kCdecl) {
  static std::mutex mu;
  const gaddr data = Arg(c, 0);
  if (gaddr und = RdPtr(data)) return und;
  const std::string decorated = S(data + PtrSize());
  const std::string name = UndnameTypeName(decorated);
  if (name.empty()) FatalF("type_info::name(): cannot undecorate %s", decorated.c_str());
  gaddr p = HeapAllocProcess(name.size() + 1, false);
  PutStr(p, name);
  std::lock_guard<std::mutex> l(mu);
  if (gaddr won = RdPtr(data)) {
    HeapFreeProcess(p);
    return won;
  }
  WrPtr(data, p);
  return p;
}

// The list of undecorated names type_info::name() allocated: not built
// (see __std_type_info_name), so nothing to free.
HLE64("vcruntime140.dll", __std_type_info_destroy_list, kCdecl) { return 0; }

// ---- RTTI (32-bit guests: x64's RTTI holds image-relative offsets) -------------
// dynamic_cast, typeid, dynamic_cast<void*> over MSVC's RTTI (rtti.h). A
// failed cast to a reference throws std::bad_cast, and 32-bit C++
// exceptions are not modelled yet: fatal, named; so is a case rtti.h does
// not decide.

namespace {
const rtti::Read32 kRd = [](uint32_t a) { return Rd32(a); };
const rtti::ReadStr kName = [](uint32_t a) { return std::string(S(a)); };
}  // namespace

// __RTDynamicCast(p, vfdelta, src_type, target_type, is_reference)
HLE("vcruntime140.dll", __RTDynamicCast, kCdecl) {
  const uint32_t p = uint32_t(Arg(c, 0)), src = uint32_t(Arg(c, 2)), target = uint32_t(Arg(c, 3));
  uint32_t out = 0;
  const rtti::Result r =
      rtti::DynamicCast(p, int32_t(Arg(c, 1)), src, target, kRd, kName, &out);
  if (r == rtti::Result::kUndecided)
    FatalF("dynamic_cast<%s>(%s at %08X): a case not modelled", S(target + 8), S(src + 8), p);
  if (r == rtti::Result::kNull && Arg(c, 4) && p)
    FatalF("dynamic_cast<%s&>(%s at %08X) failed (std::bad_cast; C++ exceptions not modelled)",
           S(target + 8), S(src + 8), p);
  return out;
}
// __RTtypeid(p): the complete object's TypeDescriptor (typeid(*p)).
HLE("vcruntime140.dll", __RTtypeid, kCdecl) {
  const uint32_t p = uint32_t(Arg(c, 0));
  if (!p) FatalF("typeid(*nullptr) (std::bad_typeid; C++ exceptions not modelled)");
  return rtti::TypeOf(p, kRd);
}
// __RTCastToVoid(p): the complete object (dynamic_cast<void*>(p)).
HLE("vcruntime140.dll", __RTCastToVoid, kCdecl) { return rtti::CompleteObject(uint32_t(Arg(c, 0)), kRd); }

// ---- std::exception -------------------------------------------------------------
// __std_exception_data {const char* _What; bool _DoFree;}: _DoFree means
// _What was allocated (copied) and is owned.

HLE64("vcruntime140.dll", __std_exception_copy, kCdecl) {
  gaddr from = Arg(c, 0), to = Arg(c, 1);
  const uint32_t ps = PtrSize();
  gaddr what = RdPtr(from);
  if (Rd8(from + ps) && what) {
    std::string s = S(what);
    gaddr copy = HeapAllocProcess(s.size() + 1, false);
    PutStr(copy, s);
    WrPtr(to, copy);
    Wr8(to + ps, 1);
  } else {
    WrPtr(to, what);
    Wr8(to + ps, 0);
  }
  return 0;
}

HLE64("vcruntime140.dll", __std_exception_destroy, kCdecl) {
  gaddr data = Arg(c, 0);
  const uint32_t ps = PtrSize();
  if (Rd8(data + ps)) HeapFreeProcess(RdPtr(data));
  WrPtr(data, 0);
  Wr8(data + ps, 0);
  return 0;
}

// _set_purecall_handler / set_unexpected: recorded, never called (a pure
// virtual call or an unexpected exception is fatal here, named).
namespace {
std::mutex g_vc_mu;
gaddr g_purecall = 0, g_unexpected = 0;
}  // namespace
HLE64("vcruntime140.dll", _set_purecall_handler, kCdecl) {
  std::lock_guard<std::mutex> l(g_vc_mu);
  gaddr old = g_purecall;
  g_purecall = Arg(c, 0);
  return old;
}
HLE64("vcruntime140.dll", set_unexpected, kCdecl) {
  std::lock_guard<std::mutex> l(g_vc_mu);
  gaddr old = g_unexpected;
  g_unexpected = Arg(c, 0);
  return old;
}
HLE64("vcruntime140.dll", unexpected, kCdecl) { FatalF("guest called unexpected()"); }

HLE64("vcruntime140.dll", __std_terminate, kCdecl) { FatalF("guest called std::terminate()"); }
HLE64("vcruntime140.dll", _purecall, kCdecl) {
  FatalF("guest called a pure virtual function (_purecall)");
}

}  // namespace recomp::hle
