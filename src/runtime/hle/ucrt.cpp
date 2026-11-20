// The Universal CRT (ucrtbase.dll, imported through its api-ms-win-crt-*
// API sets) for x64 guests: the parts the statically linked startup code
// and the game call. Its process-wide data (argv, the environment, the
// onexit tables' arrays, errno) lives in guest memory, where the guest
// reads it through the pointers these functions return.

#include <cerrno>
#include <climits>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

#include "runtime/hle/hle.h"
#include "runtime/hle/kernel32_state.h"
#include "runtime/hle/ucrt.h"
#include "runtime/hle/ucrt_fp.h"
#include "runtime/hle/win32.h"
#include "runtime/log.h"
#include "runtime/runtime.h"
#include "runtime/x87.h"

namespace recomp::hle {
namespace {

// The program name: up to the closing quote or the first blank, no escapes.
const char* SkipProgramName(const char* p) {
  if (*p == '"') {
    p++;
    while (*p && *p != '"') p++;
    if (*p) p++;
  } else {
    while (*p && *p != ' ' && *p != '\t') p++;
  }
  while (*p == ' ' || *p == '\t') p++;
  return p;
}

// The size of a guest pointer (4 for 32-bit guests, 8 for x64 ones): the
// stride of the pointer arrays below, which are read with RdPtr/WrPtr.
uint32_t PtrSize() { return g_proc->x64 ? 8 : 4; }

// Guest copies of a list of strings: a null-terminated pointer array.
gaddr GuestStrings(const std::vector<std::string>& v) {
  const uint32_t ps = PtrSize();
  gaddr arr = HeapAllocProcess(ps * (v.size() + 1), true);
  for (size_t i = 0; i < v.size(); i++) {
    gaddr s = HeapAllocProcess(v[i].size() + 1, false);
    PutStr(s, v[i]);
    WrPtr(arr + ps * i, s);
  }
  return arr;
}

// The CRT's process-wide variables, in guest memory (their addresses are
// what __p___argc and friends return).
struct Globals {
  gaddr argc, argv, environ, fmode, commode, app_type;
};
Globals& G() {
  static Globals g = [] {
    Globals r;
    gaddr b = HeapAllocProcess(0x40, true);
    r.argc = b;            // int
    r.argv = b + 0x08;     // char**
    r.environ = b + 0x10;  // char**
    r.fmode = b + 0x18;    // int
    r.commode = b + 0x1C;  // int
    r.app_type = b + 0x20;  // int
    return r;
  }();
  return g;
}

// _onexit_table_t: {first, last, end} function pointer array (pointers of
// the guest's size), which the guest's own startup code may also walk.
std::mutex g_onexit_mu;

void RegisterOnExit(gaddr table, gaddr fn) {
  std::lock_guard<std::mutex> l(g_onexit_mu);
  const uint32_t ps = PtrSize();
  gaddr first = RdPtr(table), last = RdPtr(table + ps), end = RdPtr(table + 2 * ps);
  if (last == end) {
    uint64_t n = (end - first) / ps, cap = n ? 2 * n : 32;
    gaddr grown = HeapAllocProcess(ps * cap, true);
    if (n) std::memcpy(GPtr(grown), GPtr(first), ps * n);
    if (first) HeapFreeProcess(first);
    first = grown;
    last = grown + ps * n;
    end = grown + ps * cap;
  }
  WrPtr(last, fn);
  WrPtr(table, first);
  WrPtr(table + ps, last + ps);
  WrPtr(table + 2 * ps, end);
}

// Runs the table's functions last to first and empties it. A function may
// register more while this runs; those run too.
void ExecuteOnExit(Ctx& c, gaddr table) {
  for (;;) {
    gaddr fn;
    {
      std::lock_guard<std::mutex> l(g_onexit_mu);
      const uint32_t ps = PtrSize();
      gaddr first = RdPtr(table), last = RdPtr(table + ps);
      if (!first || last == first) break;
      last -= ps;
      fn = RdPtr(last);
      WrPtr(table + ps, last);
    }
    if (fn) CallGuest(c, fn, {});
  }
  std::lock_guard<std::mutex> l(g_onexit_mu);
  const uint32_t ps = PtrSize();
  if (gaddr first = RdPtr(table)) HeapFreeProcess(first);
  for (uint32_t i = 0; i < 3; i++) WrPtr(table + ps * i, 0);
}

// The CRT's own table (_crt_atexit, atexit): run by exit and _cexit.
gaddr AtExitTable() {
  static gaddr t = HeapAllocProcess(24, true);
  return t;
}

[[noreturn]] void ExitNow(uint32_t code) {
  std::fflush(stdout);
  std::fprintf(stderr, "exit(%d)\n", int32_t(code));
  std::fflush(stderr);
  std::_Exit(int(code));
}

// The calling thread's errno, in guest memory.
gaddr ErrnoSlot() {
  static std::mutex mu;
  static std::unordered_map<uint32_t, gaddr> slots;
  std::lock_guard<std::mutex> l(mu);
  gaddr& s = slots[CurrentThread()->tid];
  if (!s) s = HeapAllocProcess(8, true);
  return s;
}

void SetErrno(uint32_t e) { Wr32(ErrnoSlot(), e); }

}  // namespace

gaddr GuestErrno() { return ErrnoSlot(); }

// ---- startup and termination ---------------------------------------------

// _initterm(first, last): every non-null function pointer in [first, last).
HLE64("ucrtbase.dll", _initterm, kCdecl) {
  for (gaddr p = Arg(c, 0), end = Arg(c, 1); p < end; p += PtrSize())
    if (gaddr fn = RdPtr(p)) CallGuest(c, fn, {});
  return 0;
}

// _initterm_e: the same with int-returning functions; the first nonzero
// result stops it and is returned.
HLE64("ucrtbase.dll", _initterm_e, kCdecl) {
  for (gaddr p = Arg(c, 0), end = Arg(c, 1); p < end; p += PtrSize())
    if (gaddr fn = RdPtr(p))
      if (int32_t r = int32_t(CallGuest(c, fn, {}))) return uint32_t(r);
  return 0;
}

HLE64("ucrtbase.dll", _set_app_type, kCdecl) {
  Wr32(G().app_type, uint32_t(Arg(c, 0)));
  return 0;
}

HLE64("ucrtbase.dll", _configure_narrow_argv, kCdecl) {
  std::vector<std::string> args = win32::SplitCommandLine(GStr(g_proc->cmdline_a));
  Wr32(G().argc, uint32_t(args.size()));
  WrPtr(G().argv, GuestStrings(args));
  return 0;
}

HLE64("ucrtbase.dll", __p___argc, kCdecl) { return G().argc; }
HLE64("ucrtbase.dll", __p___argv, kCdecl) { return G().argv; }

HLE64("ucrtbase.dll", _initialize_narrow_environment, kCdecl) {
  WrPtr(G().environ, GuestStrings(EnvironmentVars()));
  return 0;
}
HLE64("ucrtbase.dll", _get_initial_narrow_environment, kCdecl) {
  return RdPtr(G().environ);
}

// The command line after the program name (WinMain's lpCmdLine).
HLE64("ucrtbase.dll", _get_narrow_winmain_command_line, kCdecl) {
  const char* base = g_proc->mem.ptr<const char>(g_proc->cmdline_a);
  return g_proc->cmdline_a + gaddr(SkipProgramName(base) - base);
}

HLE64("ucrtbase.dll", _set_fmode, kCdecl) {
  Wr32(G().fmode, uint32_t(Arg(c, 0)));
  return 0;
}
HLE64("ucrtbase.dll", __p__commode, kCdecl) { return G().commode; }

// The guest's floating-point environment needs no CRT setup.
HLE64("ucrtbase.dll", __setusermatherr, kCdecl) { return 0; }

HLE64("ucrtbase.dll", _initialize_onexit_table, kCdecl) {
  for (uint32_t i = 0; i < 3; i++) WrPtr(Arg(c, 0) + PtrSize() * i, 0);
  return 0;
}
HLE64("ucrtbase.dll", _register_onexit_function, kCdecl) {
  RegisterOnExit(Arg(c, 0), Arg(c, 1));
  return 0;
}
HLE64("ucrtbase.dll", _execute_onexit_table, kCdecl) {
  ExecuteOnExit(c, Arg(c, 0));
  return 0;
}
HLE64("ucrtbase.dll", _crt_atexit, kCdecl) {
  RegisterOnExit(AtExitTable(), Arg(c, 0));
  return 0;
}

// The thread_local destructor callback of the EXE: kept, never called
// (guest threads do not run TLS destructors here).
HLE64("ucrtbase.dll", _register_thread_local_exe_atexit_callback, kCdecl) { return 0; }

HLE64("ucrtbase.dll", exit, kCdecl) {
  uint32_t code = uint32_t(Arg(c, 0));
  ExecuteOnExit(c, AtExitTable());
  ExitNow(code);
}
HLE64("ucrtbase.dll", _exit, kCdecl) { ExitNow(uint32_t(Arg(c, 0))); }
HLE64("ucrtbase.dll", _cexit, kCdecl) {
  ExecuteOnExit(c, AtExitTable());
  return 0;
}
HLE64("ucrtbase.dll", _c_exit, kCdecl) { return 0; }

HLE64("ucrtbase.dll", terminate, kCdecl) { FatalF("guest called terminate()"); }
HLE64("ucrtbase.dll", _invalid_parameter_noinfo_noreturn, kCdecl) {
  FatalF("guest CRT: invalid parameter (_invalid_parameter_noinfo_noreturn)");
}

// ---- floating-point environment (32-bit guests) -------------------------------

// _controlfp_s(current, new, mask): the CRT's control bits under mask set on
// both the guest thread's x87 CW and its MXCSR (ucrt_fp.h); *current, if
// given, gets the bits after the change. The x86 startup code calls it to
// select 53-bit precision. Never the denormal exception mask (_controlfp's
// rule). Bits outside the valid set reach the CRT's invalid parameter
// handler, which ends the process by default: fatal here. x64 guests have
// no x87 precision control, so this handler is not audited for them.
HLE("ucrtbase.dll", _controlfp_s, kCdecl) {
  const gaddr current = Arg(c, 0);
  const uint32_t ctl = uint32_t(Arg(c, 1)), mask = uint32_t(Arg(c, 2));
  if (mask & ~fp::kControlFpMask)
    FatalF("guest CRT: invalid parameter (_controlfp_s mask %08X)", mask);
  uint16_t cw = x87::ControlWord(c.fpu);
  uint32_t mxcsr = c.mxcsr;
  fp::ToUnits(ctl, fp::ControlFpMask(mask), &cw, &mxcsr);
  x87::SetControlWord(c.fpu, cw);
  c.mxcsr = mxcsr;
  if (current) Wr32(current, fp::FromUnits(cw, mxcsr));
  return 0;
}

// ---- error handlers -------------------------------------------------------------
// A game (or a crash reporter in it) installs its own handlers for invalid
// parameters, new failures, terminate, signals. Here those conditions are
// already fatal errors that name what happened, so each handler is
// recorded (and returned by the next call, as the CRT does) but not called.

namespace {
std::mutex g_handlers_mu;
gaddr Swap(gaddr& slot, gaddr v) {
  std::lock_guard<std::mutex> l(g_handlers_mu);
  gaddr old = slot;
  slot = v;
  return old;
}
gaddr g_invalid_parameter = 0, g_new_handler = 0, g_terminate = 0;
uint32_t g_error_mode = 0, g_abort_behavior = 0x3;  // _WRITE_ABORT_MSG | _CALL_REPORTFAULT
gaddr g_signals[23] = {};                            // by signal number (SIGABRT 22)
}  // namespace

// _set_error_mode(mode): _OUT_TO_DEFAULT 0, _OUT_TO_STDERR 1, _OUT_TO_MSGBOX
// 2; _REPORT_ERRMODE 3 queries. Returns the previous mode.
HLE64("ucrtbase.dll", _set_error_mode, kCdecl) {
  const int32_t m = int32_t(Arg(c, 0));
  std::lock_guard<std::mutex> l(g_handlers_mu);
  const uint32_t old = g_error_mode;
  if (m >= 0 && m <= 2) g_error_mode = uint32_t(m);
  else if (m != 3) return uint32_t(-1);
  return old;
}
HLE64("ucrtbase.dll", _set_invalid_parameter_handler, kCdecl) {
  return Swap(g_invalid_parameter, Arg(c, 0));
}
HLE64("ucrtbase.dll", _set_new_handler, kCdecl) { return Swap(g_new_handler, Arg(c, 0)); }
HLE64("ucrtbase.dll", set_terminate, kCdecl) { return Swap(g_terminate, Arg(c, 0)); }
// _set_abort_behavior(flags, mask): the old flags.
HLE64("ucrtbase.dll", _set_abort_behavior, kCdecl) {
  std::lock_guard<std::mutex> l(g_handlers_mu);
  const uint32_t old = g_abort_behavior, mask = uint32_t(Arg(c, 1));
  g_abort_behavior = (old & ~mask) | (uint32_t(Arg(c, 0)) & mask);
  return old;
}
// signal(sig, handler): the previous handler (SIG_DFL 0); SIG_ERR (-1) for a
// signal the CRT does not know.
HLE64("ucrtbase.dll", signal, kCdecl) {
  const int32_t sig = int32_t(Arg(c, 0));
  if (sig != 2 && sig != 4 && sig != 8 && sig != 11 && sig != 15 && sig != 21 && sig != 22) {
    Wr32(ErrnoSlot(), EINVAL);
    return uint32_t(-1);
  }
  return Swap(g_signals[sig], Arg(c, 1));
}
HLE64("ucrtbase.dll", raise, kCdecl) {
  FatalF("guest raised signal %d", int32_t(Arg(c, 0)));
}

// The startup code's __except filters: no exception is handled there.
HLE64("ucrtbase.dll", _seh_filter_exe, kCdecl) { return 0; }  // CONTINUE_SEARCH
HLE64("ucrtbase.dll", _seh_filter_dll, kCdecl) { return 0; }

// errno: an int per guest thread.
HLE64("ucrtbase.dll", _errno, kCdecl) { return ErrnoSlot(); }

// ---- threads ----------------------------------------------------------------

// _beginthreadex(security, stack, start, arg, flags, &tid): a thread whose
// exit code is start's result; the handle is the caller's to close.
// Flags: CREATE_SUSPENDED, STACK_SIZE_PARAM_IS_A_RESERVATION.
HLE64_ONLY("ucrtbase.dll", _beginthreadex, kCdecl) {
  uint32_t flags = uint32_t(Arg(c, 4));
  if (flags & ~0x10004u) FatalF("_beginthreadex: flags %X not supported", flags);
  uint32_t tid;
  uint32_t h = NewThread(uint32_t(Arg(c, 1)), Arg(c, 2), Arg(c, 3), flags & 4, &tid);
  if (Arg(c, 5)) Wr32(Arg(c, 5), tid);
  return HandleRet(h);
}

// ---- heap ----------------------------------------------------------------
// The UCRT allocates from the process heap since Visual Studio 2015.

namespace {
gaddr Malloc(uint64_t n) { return HeapAllocProcess(n ? n : 1, false); }
}  // namespace

HLE64("ucrtbase.dll", malloc, kCdecl) { return Malloc(Arg(c, 0)); }
HLE64("ucrtbase.dll", free, kCdecl) {
  if (gaddr p = Arg(c, 0)) HeapFreeProcess(p);
  return 0;
}
// The largest size_t of the guest.
namespace {
uint64_t SizeMax() { return g_proc->x64 ? UINT64_MAX : UINT32_MAX; }
}  // namespace
HLE64("ucrtbase.dll", calloc, kCdecl) {
  uint64_t n = Arg(c, 0), size = Arg(c, 1);
  if (size && n > SizeMax() / size) {
    SetErrno(ENOMEM);
    return 0;
  }
  return HeapAllocProcess(n * size ? n * size : 1, true);
}
HLE64("ucrtbase.dll", realloc, kCdecl) {
  gaddr p = Arg(c, 0);
  uint64_t n = Arg(c, 1);
  if (!p) return Malloc(n);
  if (!n) {
    HeapFreeProcess(p);
    return 0;
  }
  return HeapReAllocProcess(p, n);
}
// _aligned_malloc: the block's own address is kept in the 8 bytes below the
// aligned pointer, as the UCRT does.
// _aligned_malloc(size, align): align a power of two, at least a pointer.
// Below the aligned block, at the guest's pointer size: the requested size,
// then the heap block it is in.
namespace {
gaddr AlignedMalloc(uint64_t n, uint64_t align) {
  const uint32_t ps = PtrSize();
  align = std::max<uint64_t>(align, ps);
  if (align & (align - 1)) {
    SetErrno(EINVAL);
    return 0;
  }
  gaddr block = Malloc(n + align + 2 * ps);
  if (!block) return 0;
  gaddr p = (block + 2 * ps + align - 1) & ~(align - 1);
  WrPtr(p - ps, block);
  WrPtr(p - 2 * ps, n);
  return p;
}
}  // namespace
HLE64("ucrtbase.dll", _aligned_malloc, kCdecl) { return AlignedMalloc(Arg(c, 0), Arg(c, 1)); }
HLE64("ucrtbase.dll", _aligned_free, kCdecl) {
  if (gaddr p = Arg(c, 0)) HeapFreeProcess(RdPtr(p - PtrSize()));
  return 0;
}
// _aligned_realloc(p, size, align): NULL p allocates, size 0 frees.
HLE64("ucrtbase.dll", _aligned_realloc, kCdecl) {
  const gaddr p = Arg(c, 0);
  const uint64_t n = Arg(c, 1);
  if (!p) return AlignedMalloc(n, Arg(c, 2));
  const uint32_t ps = PtrSize();
  if (!n) {
    HeapFreeProcess(RdPtr(p - ps));
    return 0;
  }
  const gaddr q = AlignedMalloc(n, Arg(c, 2));
  if (!q) return 0;
  std::memcpy(GPtr(q), GPtr(p), std::min<uint64_t>(RdPtr(p - 2 * ps), n));
  HeapFreeProcess(RdPtr(p - ps));
  return q;
}
// _recalloc(p, n, size): realloc to n * size, the grown part zeroed.
HLE64("ucrtbase.dll", _recalloc, kCdecl) {
  const gaddr p = Arg(c, 0);
  const uint64_t n = Arg(c, 1), size = Arg(c, 2);
  if (size && n > SizeMax() / size) {
    SetErrno(ENOMEM);
    return 0;
  }
  const uint64_t total = n * size;
  if (!p) return HeapAllocProcess(total ? total : 1, true);
  const uint64_t old = HeapSizeProcess(p);
  if (!total) {
    HeapFreeProcess(p);
    return 0;
  }
  const gaddr q = HeapReAllocProcess(p, total);
  if (q && total > old) std::memset(GPtr(q + old), 0, total - old);
  return q;
}
// No new handler is ever installed (_set_new_handler is not provided).
HLE64("ucrtbase.dll", _callnewh, kCdecl) { return 0; }
HLE64("ucrtbase.dll", _set_new_mode, kCdecl) {
  static int mode = 0;
  int old = mode;
  mode = int(Arg(c, 0));
  return uint32_t(old);
}

// ---- locale (only the "C" locale) ----------------------------------------

HLE64("ucrtbase.dll", _configthreadlocale, kCdecl) {
  // _ENABLE_PER_THREAD_LOCALE 1, _DISABLE_PER_THREAD_LOCALE 2; 0 queries,
  // -1 resets. Nothing differs between them with one locale.
  static std::mutex mu;
  static int mode = 2;
  std::lock_guard<std::mutex> l(mu);
  int old = mode, t = int(Arg(c, 0));
  if (t == 1 || t == 2) mode = t;
  if (t == -1) mode = 2;
  return uint32_t(old);
}

// localeconv: the "C" locale's struct lconv (x64 layout: ten char*, eight
// chars, six wchar_t*).
HLE64_ONLY("ucrtbase.dll", localeconv, kCdecl) {
  static gaddr lc = [] {
    gaddr p = HeapAllocProcess(136, true);
    gaddr dot = HeapAllocProcess(4, true), empty = dot + 2;
    gaddr wdot = HeapAllocProcess(8, true), wempty = wdot + 4;
    PutStr(dot, ".");
    Wr16(wdot, '.');
    for (int i = 0; i < 10; i++) Wr64(p + 8 * i, i == 0 ? dot : empty);
    for (int i = 0; i < 8; i++) Wr8(p + 80 + i, CHAR_MAX);
    for (int i = 0; i < 6; i++) Wr64(p + 88 + 8 * i, i == 0 ? wdot : wempty);
    return p;
  }();
  return lc;
}

// ---- character classes and strings ("C" locale) --------------------------

namespace {
int CArg(const Ctx& c, int i) { return int32_t(Arg(c, i)); }
bool InC(int ch) { return ch >= 0 && ch < 128; }  // the classes are ASCII's
}  // namespace

HLE64("ucrtbase.dll", iscntrl, kCdecl) { return InC(CArg(c, 0)) && std::iscntrl(CArg(c, 0)); }
HLE64("ucrtbase.dll", isdigit, kCdecl) { return InC(CArg(c, 0)) && std::isdigit(CArg(c, 0)); }
HLE64("ucrtbase.dll", isspace, kCdecl) { return InC(CArg(c, 0)) && std::isspace(CArg(c, 0)); }
HLE64("ucrtbase.dll", isupper, kCdecl) { return InC(CArg(c, 0)) && std::isupper(CArg(c, 0)); }
HLE64("ucrtbase.dll", isxdigit, kCdecl) { return InC(CArg(c, 0)) && std::isxdigit(CArg(c, 0)); }
HLE64("ucrtbase.dll", tolower, kCdecl) {
  int ch = CArg(c, 0);
  return uint32_t(ch >= 'A' && ch <= 'Z' ? ch + 32 : ch);
}
HLE64("ucrtbase.dll", toupper, kCdecl) {
  int ch = CArg(c, 0);
  return uint32_t(ch >= 'a' && ch <= 'z' ? ch - 32 : ch);
}

namespace {
const char* S(gaddr p) { return g_proc->mem.ptr<const char>(p); }
int Sign(int v) { return v < 0 ? -1 : v > 0 ? 1 : 0; }
int Ci(int ch) { return ch >= 'A' && ch <= 'Z' ? ch + 32 : ch; }  // _stricmp folds to lower
int StrICmp(const char* a, const char* b, size_t n) {
  for (size_t i = 0; i < n; i++) {
    int x = Ci(uint8_t(a[i])), y = Ci(uint8_t(b[i]));
    if (x != y) return x - y;
    if (!x) break;
  }
  return 0;
}
}  // namespace

HLE64("ucrtbase.dll", strcmp, kCdecl) {
  return uint32_t(Sign(std::strcmp(S(Arg(c, 0)), S(Arg(c, 1)))));
}
HLE64("ucrtbase.dll", strncmp, kCdecl) {
  return uint32_t(Sign(std::strncmp(S(Arg(c, 0)), S(Arg(c, 1)), Arg(c, 2))));
}
HLE64("ucrtbase.dll", _stricmp, kCdecl) {
  return uint32_t(Sign(StrICmp(S(Arg(c, 0)), S(Arg(c, 1)), SIZE_MAX)));
}
HLE64("ucrtbase.dll", _strnicmp, kCdecl) {
  return uint32_t(Sign(StrICmp(S(Arg(c, 0)), S(Arg(c, 1)), Arg(c, 2))));
}
HLE64("ucrtbase.dll", strcspn, kCdecl) { return std::strcspn(S(Arg(c, 0)), S(Arg(c, 1))); }
// strncat(dst, src, n): at most n chars of src appended, always terminated.
HLE64("ucrtbase.dll", strncat, kCdecl) {
  gaddr d = Arg(c, 0), src = Arg(c, 1);
  uint64_t n = Arg(c, 2), i = 0;
  gaddr e = d;
  while (Rd8(e)) e++;
  for (; i < n && S(src)[i]; i++) Wr8(e + i, uint8_t(S(src)[i]));
  Wr8(e + i, 0);
  return d;
}
HLE64("ucrtbase.dll", strncpy, kCdecl) {
  gaddr d = Arg(c, 0), src = Arg(c, 1);
  uint64_t n = Arg(c, 2), i = 0;
  for (; i < n && S(src)[i]; i++) Wr8(d + i, uint8_t(S(src)[i]));
  for (; i < n; i++) Wr8(d + i, 0);
  return d;
}
// strtok: the position is per thread, as in the UCRT.
HLE64("ucrtbase.dll", strtok, kCdecl) {
  static thread_local gaddr next = 0;
  gaddr s = Arg(c, 0) ? Arg(c, 0) : next;
  const char* delim = S(Arg(c, 1));
  if (!s) return 0;
  s += std::strspn(S(s), delim);
  if (!*S(s)) {
    next = 0;
    return 0;
  }
  gaddr e = s + std::strcspn(S(s), delim);
  if (*S(e)) {
    Wr8(e, 0);
    next = e + 1;
  } else {
    next = 0;
  }
  return s;
}
// wcscat_s: 0, or EINVAL / ERANGE (and an empty destination) as the UCRT.
HLE64("ucrtbase.dll", wcscat_s, kCdecl) {
  gaddr d = Arg(c, 0), src = Arg(c, 2);
  uint64_t size = Arg(c, 1);
  if (!d || !size) return EINVAL;
  if (!src) {
    Wr16(d, 0);
    return EINVAL;
  }
  uint64_t n = 0;
  while (n < size && Rd16(d + 2 * n)) n++;
  if (n == size) {
    Wr16(d, 0);
    return EINVAL;
  }
  for (uint64_t i = 0;; i++) {
    if (n + i >= size) {
      Wr16(d, 0);
      return ERANGE;
    }
    uint16_t ch = Rd16(src + 2 * i);
    Wr16(d + 2 * (n + i), ch);
    if (!ch) return 0;
  }
}


// ---- more strings ("C" locale) ---------------------------------------------
// Character classes, case and collation are ASCII's: the "C" locale.
// Secure (_s) functions: a bad argument or a destination too small reaches
// the CRT's invalid parameter handler, which ends the process by default:
// fatal here, naming the function.

HLE64("ucrtbase.dll", isalnum, kCdecl) { return InC(CArg(c, 0)) && std::isalnum(CArg(c, 0)); }
HLE64("ucrtbase.dll", isalpha, kCdecl) { return InC(CArg(c, 0)) && std::isalpha(CArg(c, 0)); }
HLE64("ucrtbase.dll", islower, kCdecl) { return InC(CArg(c, 0)) && std::islower(CArg(c, 0)); }
HLE64("ucrtbase.dll", isprint, kCdecl) { return InC(CArg(c, 0)) && std::isprint(CArg(c, 0)); }
HLE64("ucrtbase.dll", ispunct, kCdecl) { return InC(CArg(c, 0)) && std::ispunct(CArg(c, 0)); }

HLE64("ucrtbase.dll", strlen, kCdecl) { return std::strlen(S(Arg(c, 0))); }
HLE64("ucrtbase.dll", strcpy, kCdecl) {
  const gaddr d = Arg(c, 0), src = Arg(c, 1);
  const size_t n = std::strlen(S(src)) + 1;
  std::memmove(GPtr(d), GPtr(src), n);
  return d;
}
HLE64("ucrtbase.dll", strcoll, kCdecl) {
  return uint32_t(Sign(std::strcmp(S(Arg(c, 0)), S(Arg(c, 1)))));
}
HLE64("ucrtbase.dll", strspn, kCdecl) { return std::strspn(S(Arg(c, 0)), S(Arg(c, 1))); }
HLE64("ucrtbase.dll", strpbrk, kCdecl) {
  const char* p = S(Arg(c, 0));
  const char* r = std::strpbrk(p, S(Arg(c, 1)));
  return r ? Arg(c, 0) + gaddr(r - p) : 0;
}
// _mbsrchr: with the single-byte code page a program starts with, strrchr.
HLE64("ucrtbase.dll", _mbsrchr, kCdecl) {
  const char* p = S(Arg(c, 0));
  const char* r = std::strrchr(p, int(char(Arg(c, 1))));
  return r ? Arg(c, 0) + gaddr(r - p) : 0;
}
HLE64("ucrtbase.dll", _strdup, kCdecl) {
  const gaddr src = Arg(c, 0);
  if (!src) return 0;
  const std::string str = S(src);
  gaddr p = Malloc(str.size() + 1);
  PutStr(p, str);
  return p;
}
HLE64("ucrtbase.dll", _strlwr, kCdecl) {
  for (gaddr p = Arg(c, 0); Rd8(p); p++) Wr8(p, uint8_t(Ci(Rd8(p))));
  return Arg(c, 0);
}
HLE64("ucrtbase.dll", _strupr, kCdecl) {
  for (gaddr p = Arg(c, 0); Rd8(p); p++) {
    uint8_t ch = Rd8(p);
    if (ch >= 'a' && ch <= 'z') Wr8(p, uint8_t(ch - 32));
  }
  return Arg(c, 0);
}
HLE64("ucrtbase.dll", strcpy_s, kCdecl) {
  const gaddr d = Arg(c, 0), src = Arg(c, 2);
  const uint64_t size = Arg(c, 1);
  if (!d || !size) FatalF("guest CRT: invalid parameter (strcpy_s: no destination)");
  if (!src) {
    Wr8(d, 0);
    FatalF("guest CRT: invalid parameter (strcpy_s: no source)");
  }
  const size_t n = std::strlen(S(src)) + 1;
  if (n > size) {
    Wr8(d, 0);
    FatalF("guest CRT: invalid parameter (strcpy_s: %zu bytes into %llu)", n,
           (unsigned long long)size);
  }
  std::memmove(GPtr(d), GPtr(src), n);
  return 0;
}
HLE64("ucrtbase.dll", strcat_s, kCdecl) {
  const gaddr d = Arg(c, 0), src = Arg(c, 2);
  const uint64_t size = Arg(c, 1);
  if (!d || !size) FatalF("guest CRT: invalid parameter (strcat_s: no destination)");
  uint64_t len = 0;
  while (len < size && Rd8(d + len)) len++;
  if (len == size) FatalF("guest CRT: invalid parameter (strcat_s: unterminated destination)");
  if (!src) {
    Wr8(d, 0);
    FatalF("guest CRT: invalid parameter (strcat_s: no source)");
  }
  const size_t n = std::strlen(S(src)) + 1;
  if (len + n > size) {
    Wr8(d, 0);
    FatalF("guest CRT: invalid parameter (strcat_s: %llu + %zu bytes into %llu)",
           (unsigned long long)len, n, (unsigned long long)size);
  }
  std::memmove(GPtr(d + len), GPtr(src), n);
  return 0;
}
// strncpy_s(dst, size, src, count): count _TRUNCATE copies what fits and
// returns STRUNCATE when it had to cut.
HLE64("ucrtbase.dll", strncpy_s, kCdecl) {
  constexpr uint32_t kStruncate = 80;
  const gaddr d = Arg(c, 0), src = Arg(c, 2);
  const uint64_t size = Arg(c, 1), count = Arg(c, 3);
  const bool trunc = count == (g_proc->x64 ? ~0ull : 0xFFFFFFFFull);
  if (!d || !size) {
    if (!d && !size && !count) return 0;
    FatalF("guest CRT: invalid parameter (strncpy_s: no destination)");
  }
  if (!count) {
    Wr8(d, 0);
    return 0;
  }
  if (!src) {
    Wr8(d, 0);
    FatalF("guest CRT: invalid parameter (strncpy_s: no source)");
  }
  size_t n = 0;
  const char* p = S(src);
  while ((trunc || n < count) && p[n]) n++;
  if (n + 1 > size) {
    if (trunc) {
      std::memmove(GPtr(d), p, size - 1);
      Wr8(d + size - 1, 0);
      return kStruncate;
    }
    Wr8(d, 0);
    FatalF("guest CRT: invalid parameter (strncpy_s: %zu bytes into %llu)", n + 1,
           (unsigned long long)size);
  }
  std::memmove(GPtr(d), p, n);
  Wr8(d + n, 0);
  return 0;
}
// strtok_s(str, delim, &context): strtok with the position in *context.
HLE64("ucrtbase.dll", strtok_s, kCdecl) {
  const gaddr ctx = Arg(c, 2);
  if (!ctx || !Arg(c, 1)) FatalF("guest CRT: invalid parameter (strtok_s)");
  gaddr s = Arg(c, 0) ? Arg(c, 0) : RdPtr(ctx);
  const char* delim = S(Arg(c, 1));
  if (!s) return 0;
  s += std::strspn(S(s), delim);
  if (!*S(s)) {
    WrPtr(ctx, s);
    return 0;
  }
  gaddr e = s + std::strcspn(S(s), delim);
  if (*S(e)) {
    Wr8(e, 0);
    WrPtr(ctx, e + 1);
  } else {
    WrPtr(ctx, e);
  }
  return s;
}

// ---- wide character classes ---------------------------------------------------
// Exact for ASCII. Beyond it Windows classifies by its Unicode tables
// (GetStringTypeW), not modelled yet: such a character is a fatal error
// naming the call, never a guessed class. WEOF (0xFFFF) is in no class.

namespace {
int WClass(const Ctx& c, const char* fn) {
  const uint32_t ch = uint32_t(Arg(c, 0)) & 0xFFFF;
  if (ch == 0xFFFF) return -1;
  if (ch >= 0x80) FatalF("guest CRT: %s(U+%04X): non-ASCII classes not modelled yet", fn, ch);
  return int(ch);
}
}  // namespace
HLE64("ucrtbase.dll", iswdigit, kCdecl) {
  int ch = WClass(c, "iswdigit");
  return ch >= 0 && std::isdigit(ch) ? 1 : 0;
}
HLE64("ucrtbase.dll", iswspace, kCdecl) {
  int ch = WClass(c, "iswspace");
  return ch >= 0 && std::isspace(ch) ? 1 : 0;
}
HLE64("ucrtbase.dll", iswalnum, kCdecl) {
  int ch = WClass(c, "iswalnum");
  return ch >= 0 && std::isalnum(ch) ? 1 : 0;
}

// ---- wide strings ("C" locale) ---------------------------------------------

namespace {
uint16_t WAt(gaddr p, uint64_t i) { return Rd16(p + 2 * i); }
uint64_t WLen(gaddr p) {
  uint64_t n = 0;
  while (WAt(p, n)) n++;
  return n;
}
uint16_t WLower(uint16_t ch) { return ch >= 'A' && ch <= 'Z' ? uint16_t(ch + 32) : ch; }
int WCmp(gaddr a, gaddr b, uint64_t n, bool fold) {
  for (uint64_t i = 0; i < n; i++) {
    int x = WAt(a, i), y = WAt(b, i);
    if (fold) x = WLower(uint16_t(x)), y = WLower(uint16_t(y));
    if (x != y) return x < y ? -1 : 1;
    if (!x) break;
  }
  return 0;
}
bool WIn(uint16_t ch, gaddr set) {
  for (uint64_t i = 0; WAt(set, i); i++)
    if (WAt(set, i) == ch) return true;
  return false;
}
}  // namespace

HLE64("ucrtbase.dll", wcslen, kCdecl) { return WLen(Arg(c, 0)); }
HLE64("ucrtbase.dll", wcscmp, kCdecl) { return uint32_t(WCmp(Arg(c, 0), Arg(c, 1), ~0ull, false)); }
HLE64("ucrtbase.dll", wcscoll, kCdecl) { return uint32_t(WCmp(Arg(c, 0), Arg(c, 1), ~0ull, false)); }
HLE64("ucrtbase.dll", _wcsicmp, kCdecl) { return uint32_t(WCmp(Arg(c, 0), Arg(c, 1), ~0ull, true)); }
HLE64("ucrtbase.dll", _wcsicoll, kCdecl) { return uint32_t(WCmp(Arg(c, 0), Arg(c, 1), ~0ull, true)); }
HLE64("ucrtbase.dll", _wcsnicmp, kCdecl) {
  return uint32_t(WCmp(Arg(c, 0), Arg(c, 1), Arg(c, 2), true));
}
HLE64("ucrtbase.dll", _wcsupr, kCdecl) {
  for (gaddr p = Arg(c, 0); Rd16(p); p += 2) {
    uint16_t ch = Rd16(p);
    if (ch >= 'a' && ch <= 'z') Wr16(p, uint16_t(ch - 32));
  }
  return Arg(c, 0);
}
HLE64("ucrtbase.dll", wcscspn, kCdecl) {
  const gaddr p = Arg(c, 0), set = Arg(c, 1);
  uint64_t i = 0;
  while (WAt(p, i) && !WIn(WAt(p, i), set)) i++;
  return i;
}
HLE64("ucrtbase.dll", wcspbrk, kCdecl) {
  const gaddr p = Arg(c, 0), set = Arg(c, 1);
  for (uint64_t i = 0; WAt(p, i); i++)
    if (WIn(WAt(p, i), set)) return p + 2 * i;
  return 0;
}
// wcsncpy: at most n characters, the rest of the n padded with NULs.
HLE64("ucrtbase.dll", wcsncpy, kCdecl) {
  const gaddr d = Arg(c, 0), src = Arg(c, 1);
  const uint64_t n = Arg(c, 2);
  uint64_t i = 0;
  for (; i < n && WAt(src, i); i++) Wr16(d + 2 * i, WAt(src, i));
  for (; i < n; i++) Wr16(d + 2 * i, 0);
  return d;
}
HLE64("ucrtbase.dll", _wcsdup, kCdecl) {
  const gaddr src = Arg(c, 0);
  if (!src) return 0;
  const uint64_t n = WLen(src) + 1;
  gaddr p = Malloc(2 * n);
  std::memmove(GPtr(p), GPtr(src), 2 * n);
  return p;
}
HLE64("ucrtbase.dll", wcscpy_s, kCdecl) {
  const gaddr d = Arg(c, 0), src = Arg(c, 2);
  const uint64_t size = Arg(c, 1);
  if (!d || !size) FatalF("guest CRT: invalid parameter (wcscpy_s: no destination)");
  if (!src) {
    Wr16(d, 0);
    FatalF("guest CRT: invalid parameter (wcscpy_s: no source)");
  }
  const uint64_t n = WLen(src) + 1;
  if (n > size) {
    Wr16(d, 0);
    FatalF("guest CRT: invalid parameter (wcscpy_s: %llu characters into %llu)",
           (unsigned long long)n, (unsigned long long)size);
  }
  std::memmove(GPtr(d), GPtr(src), 2 * n);
  return 0;
}
HLE64("ucrtbase.dll", wcsncpy_s, kCdecl) {
  constexpr uint32_t kStruncate = 80;
  const gaddr d = Arg(c, 0), src = Arg(c, 2);
  const uint64_t size = Arg(c, 1), count = Arg(c, 3);
  const bool trunc = count == (g_proc->x64 ? ~0ull : 0xFFFFFFFFull);
  if (!d || !size) {
    if (!d && !size && !count) return 0;
    FatalF("guest CRT: invalid parameter (wcsncpy_s: no destination)");
  }
  if (!count) {
    Wr16(d, 0);
    return 0;
  }
  if (!src) {
    Wr16(d, 0);
    FatalF("guest CRT: invalid parameter (wcsncpy_s: no source)");
  }
  uint64_t n = 0;
  while ((trunc || n < count) && WAt(src, n)) n++;
  if (n + 1 > size) {
    if (trunc) {
      std::memmove(GPtr(d), GPtr(src), 2 * (size - 1));
      Wr16(d + 2 * (size - 1), 0);
      return kStruncate;
    }
    Wr16(d, 0);
    FatalF("guest CRT: invalid parameter (wcsncpy_s: %llu characters into %llu)",
           (unsigned long long)(n + 1), (unsigned long long)size);
  }
  std::memmove(GPtr(d), GPtr(src), 2 * n);
  Wr16(d + 2 * n, 0);
  return 0;
}
HLE64("ucrtbase.dll", wcstok_s, kCdecl) {
  const gaddr ctx = Arg(c, 2), delim = Arg(c, 1);
  if (!ctx || !delim) FatalF("guest CRT: invalid parameter (wcstok_s)");
  gaddr s = Arg(c, 0) ? Arg(c, 0) : RdPtr(ctx);
  if (!s) return 0;
  while (Rd16(s) && WIn(Rd16(s), delim)) s += 2;
  if (!Rd16(s)) {
    WrPtr(ctx, s);
    return 0;
  }
  gaddr e = s;
  while (Rd16(e) && !WIn(Rd16(e), delim)) e += 2;
  if (Rd16(e)) {
    Wr16(e, 0);
    WrPtr(ctx, e + 2);
  } else {
    WrPtr(ctx, e);
  }
  return s;
}

// ---- numeric conversion ("C" locale) -------------------------------------

namespace {

// strto*: the host's (the same grammar since Visual Studio 2015, hex floats,
// inf and nan included) on the guest string; *end is a guest pointer.
template <typename T, typename F>
T StrTo(gaddr s, gaddr end, F f) {
  const char* p = S(s);
  char* e = nullptr;
  errno = 0;
  T v = f(p, &e);
  if (errno == ERANGE) SetErrno(ERANGE);
  if (end) WrPtr(end, s + gaddr(e - p));
  return v;
}
}  // namespace

HLE64("ucrtbase.dll", strtod, kCdecl) {
  return RetD(c, StrTo<double>(Arg(c, 0), Arg(c, 1),
                               [](const char* p, char** e) { return std::strtod(p, e); }));
}
HLE64("ucrtbase.dll", strtol, kCdecl) {
  int base = int(Arg(c, 2));
  return uint32_t(int32_t(StrTo<long>(Arg(c, 0), Arg(c, 1), [&](const char* p, char** e) {
    long long v = std::strtoll(p, e, base);
    if (v > INT32_MAX || v < INT32_MIN) {  // long is 32-bit in Win64
      errno = ERANGE;
      return long(v > 0 ? INT32_MAX : INT32_MIN);
    }
    return long(v);
  })));
}
HLE64("ucrtbase.dll", strtoul, kCdecl) {
  int base = int(Arg(c, 2));
  return uint32_t(StrTo<unsigned long>(Arg(c, 0), Arg(c, 1), [&](const char* p, char** e) {
    const char* q = p;
    while (std::isspace(uint8_t(*q))) q++;
    bool neg = *q == '-';
    unsigned long long v = std::strtoull(p, e, base);
    // 32-bit unsigned long: out of range unless the magnitude fits.
    unsigned long long mag = neg ? 0 - v : v;
    if (errno == ERANGE || mag > UINT32_MAX) {
      errno = ERANGE;
      return (unsigned long)UINT32_MAX;
    }
    return (unsigned long)(uint32_t(v));
  }));
}
HLE64_ONLY("ucrtbase.dll", strtoull, kCdecl) {
  int base = int(Arg(c, 2));
  return StrTo<unsigned long long>(Arg(c, 0), Arg(c, 1), [&](const char* p, char** e) {
    return std::strtoull(p, e, base);
  });
}
HLE64("ucrtbase.dll", atof, kCdecl) { return RetD(c, std::strtod(S(Arg(c, 0)), nullptr)); }
HLE64("ucrtbase.dll", atoi, kCdecl) {
  long long v = std::strtoll(S(Arg(c, 0)), nullptr, 10);
  return uint32_t(int32_t(v > INT32_MAX ? INT32_MAX : v < INT32_MIN ? INT32_MIN : v));
}
HLE64_ONLY("ucrtbase.dll", atol, kCdecl) {
  long long v = std::strtoll(S(Arg(c, 0)), nullptr, 10);
  return uint32_t(int32_t(v > INT32_MAX ? INT32_MAX : v < INT32_MIN ? INT32_MIN : v));
}
HLE64_ONLY("ucrtbase.dll", atoll, kCdecl) { return uint64_t(std::strtoll(S(Arg(c, 0)), nullptr, 10)); }

// mbstowcs in the "C" locale: each byte is the wchar_t of the same value.
HLE64("ucrtbase.dll", mbstowcs, kCdecl) {
  gaddr d = Arg(c, 0);
  const char* src = S(Arg(c, 1));
  uint64_t n = Arg(c, 2), len = std::strlen(src);
  if (!d) return len;
  uint64_t i = 0;
  for (; i < n && i < len; i++) Wr16(d + 2 * i, uint8_t(src[i]));
  if (i < n) Wr16(d + 2 * i, 0);
  return i;
}

// ---- more numeric and character conversion ("C" locale) -------------------

namespace {
// The leading ASCII of a wide string (numbers parse within it; characters
// map one to one, so an end position carries back).
std::string AsciiPrefix(gaddr w) {
  std::string s;
  for (uint16_t ch; (ch = Rd16(w + 2 * s.size())) && ch < 0x80;) s.push_back(char(ch));
  return s;
}
constexpr uint32_t kEilseq = 42;  // EILSEQ in the UCRT
}  // namespace

HLE64("ucrtbase.dll", _wtoi, kCdecl) {
  long long v = std::strtoll(AsciiPrefix(Arg(c, 0)).c_str(), nullptr, 10);
  return uint32_t(int32_t(v > INT32_MAX ? INT32_MAX : v < INT32_MIN ? INT32_MIN : v));
}
HLE64("ucrtbase.dll", _wtof, kCdecl) {
  return RetD(c, std::strtod(AsciiPrefix(Arg(c, 0)).c_str(), nullptr));
}
// _atof_l(str, locale): only the "C" locale exists here.
HLE64("ucrtbase.dll", _atof_l, kCdecl) { return RetD(c, std::strtod(S(Arg(c, 0)), nullptr)); }
// wcstod(str, &end): *end a guest wchar_t pointer.
HLE64("ucrtbase.dll", wcstod, kCdecl) {
  const gaddr w = Arg(c, 0), end = Arg(c, 1);
  const std::string a = AsciiPrefix(w);
  char* e = nullptr;
  errno = 0;
  const double v = std::strtod(a.c_str(), &e);
  if (errno == ERANGE) SetErrno(ERANGE);
  if (end) WrPtr(end, w + 2 * gaddr(e - a.c_str()));
  return RetD(c, v);
}
// _strtoui64(str, &end, base): ULLONG_MAX and ERANGE on overflow.
HLE64("ucrtbase.dll", _strtoui64, kCdecl) {
  const int base = int(Arg(c, 2));
  const unsigned long long v = StrTo<unsigned long long>(
      Arg(c, 0), Arg(c, 1), [&](const char* p, char** e) { return std::strtoull(p, e, base); });
  return Ret64(c, v);
}

namespace {
// _itoa's text: a minus sign only in radix 10; other radixes show the
// value's 32-bit two's complement.
std::string IToA(int32_t v, int radix) {
  if (radix < 2 || radix > 36) return {};
  const bool neg = radix == 10 && v < 0;
  uint32_t u = neg ? uint32_t(0) - uint32_t(v) : uint32_t(v);
  std::string r;
  do {
    uint32_t d = u % uint32_t(radix);
    r.insert(r.begin(), char(d < 10 ? '0' + d : 'a' + d - 10));
    u /= uint32_t(radix);
  } while (u);
  return neg ? "-" + r : r;
}
}  // namespace
HLE64("ucrtbase.dll", _itoa, kCdecl) {
  const std::string t = IToA(int32_t(Arg(c, 0)), int(Arg(c, 2)));
  if (t.empty()) FatalF("guest CRT: invalid parameter (_itoa radix %d)", int(Arg(c, 2)));
  PutStr(Arg(c, 1), t);
  return Arg(c, 1);
}
HLE64("ucrtbase.dll", _itoa_s, kCdecl) {
  const gaddr buf = Arg(c, 1);
  const uint64_t size = Arg(c, 2);
  const std::string t = IToA(int32_t(Arg(c, 0)), int(Arg(c, 3)));
  if (!buf || !size || t.empty() || t.size() + 1 > size) {
    if (buf && size) Wr8(buf, 0);
    FatalF("guest CRT: invalid parameter (_itoa_s)");
  }
  PutStr(buf, t);
  return 0;
}

// wcstombs(dst, src, n): the "C" locale maps characters 0-255 to the byte
// of the same value; any other is EILSEQ and (size_t)-1. NULL dst: the
// length needed.
HLE64("ucrtbase.dll", wcstombs, kCdecl) {
  const gaddr d = Arg(c, 0), src = Arg(c, 1);
  const uint64_t n = Arg(c, 2), fail = g_proc->x64 ? ~0ull : 0xFFFFFFFFull;
  uint64_t i = 0;
  for (;; i++) {
    const uint16_t ch = Rd16(src + 2 * i);
    if (ch > 0xFF) {
      SetErrno(kEilseq);
      return fail;
    }
    if (!ch) break;
    if (d) {
      if (i == n) return n;
      Wr8(d + i, uint8_t(ch));
    }
  }
  if (d && i < n) Wr8(d + i, 0);
  return i;
}
// mbstowcs_s(&converted, dst, dstsize, src, count): converted includes the
// NUL; count _TRUNCATE converts what fits (STRUNCATE).
HLE64("ucrtbase.dll", mbstowcs_s, kCdecl) {
  constexpr uint32_t kStruncate = 80;
  const gaddr out = Arg(c, 0), d = Arg(c, 1), src = Arg(c, 3);
  const uint64_t size = Arg(c, 2), count = Arg(c, 4);
  const bool trunc = count == (g_proc->x64 ? ~0ull : 0xFFFFFFFFull);
  if (!src) FatalF("guest CRT: invalid parameter (mbstowcs_s: no source)");
  const char* p = S(src);
  uint64_t len = 0;
  while (p[len] && (trunc || len < count)) len++;
  if (!d) {
    if (out) WrPtr(out, len + 1);
    return 0;
  }
  uint32_t r = 0;
  if (len + 1 > size) {
    if (!trunc) {
      Wr16(d, 0);
      FatalF("guest CRT: invalid parameter (mbstowcs_s: %llu characters into %llu)",
             (unsigned long long)(len + 1), (unsigned long long)size);
    }
    len = size - 1;
    r = kStruncate;
  }
  for (uint64_t i = 0; i < len; i++) Wr16(d + 2 * i, uint8_t(p[i]));
  Wr16(d + 2 * len, 0);
  if (out) WrPtr(out, len + 1);
  return r;
}

// ---- utility ---------------------------------------------------------------

// rand: the UCRT's per-thread linear congruential generator (seed 1).
// rand/srand: the UCRT's generator, its state per thread (seed 1).
namespace {
uint32_t& RandSeed() {
  static thread_local uint32_t seed = 1;
  return seed;
}
}  // namespace
HLE64("ucrtbase.dll", rand, kCdecl) {
  uint32_t& seed = RandSeed();
  seed = seed * 214013u + 2531011u;
  return (seed >> 16) & 0x7FFF;
}
HLE64("ucrtbase.dll", srand, kCdecl) {
  RandSeed() = uint32_t(Arg(c, 0));
  return 0;
}

// qsort(base, n, width, compare): the MSVC CRT's algorithm, so elements
// that compare equal end up in the order they do on Windows: quicksort on
// the median of the first, middle and last elements, an explicit stack of
// the larger partitions, and a selection sort (largest to the end) for
// partitions of up to 8 elements.
HLE64("ucrtbase.dll", qsort, kCdecl) {
  gaddr base = Arg(c, 0), cmp = Arg(c, 3);
  uint64_t n = Arg(c, 1), w = Arg(c, 2);
  if (n < 2 || !w) return 0;
  auto compare = [&](gaddr a, gaddr b) { return int32_t(CallGuest(c, cmp, {a, b})); };
  auto swap = [&](gaddr a, gaddr b) {
    if (a == b) return;
    uint8_t* pa = GPtr(a);
    uint8_t* pb = GPtr(b);
    for (uint64_t i = 0; i < w; i++) std::swap(pa[i], pb[i]);
  };
  auto shortsort = [&](gaddr lo, gaddr hi) {
    for (; hi > lo; hi -= w) {
      gaddr max = lo;
      for (gaddr p = lo + w; p <= hi; p += w)
        if (compare(p, max) > 0) max = p;
      swap(max, hi);
    }
  };
  constexpr uint64_t kCutoff = 8;
  std::vector<std::pair<gaddr, gaddr>> stack;
  gaddr lo = base, hi = base + w * (n - 1);
  for (;;) {
    uint64_t size = (hi - lo) / w + 1;
    if (size <= kCutoff) {
      shortsort(lo, hi);
    } else {
      gaddr mid = lo + (size / 2) * w;
      if (compare(lo, mid) > 0) swap(lo, mid);
      if (compare(lo, hi) > 0) swap(lo, hi);
      if (compare(mid, hi) > 0) swap(mid, hi);
      gaddr loguy = lo, higuy = hi;
      for (;;) {
        if (mid > loguy) {
          do loguy += w;
          while (loguy < mid && compare(loguy, mid) <= 0);
        }
        if (mid <= loguy) {
          do loguy += w;
          while (loguy <= hi && compare(loguy, mid) <= 0);
        }
        do higuy -= w;
        while (higuy > mid && compare(higuy, mid) > 0);
        if (higuy < loguy) break;
        swap(loguy, higuy);
        if (mid == higuy) mid = loguy;  // the pivot moved
      }
      higuy += w;
      if (mid < higuy) {
        do higuy -= w;
        while (higuy > mid && compare(higuy, mid) == 0);
      }
      if (mid >= higuy) {
        do higuy -= w;
        while (higuy > lo && compare(higuy, mid) == 0);
      }
      // [lo, higuy] and [loguy, hi] remain: the smaller one next, the
      // larger one stacked.
      if (higuy - lo >= hi - loguy) {
        if (lo < higuy) stack.push_back({lo, higuy});
        if (loguy < hi) {
          lo = loguy;
          continue;
        }
      } else {
        if (loguy < hi) stack.push_back({loguy, hi});
        if (lo < higuy) {
          hi = higuy;
          continue;
        }
      }
    }
    if (stack.empty()) return 0;
    std::tie(lo, hi) = stack.back();
    stack.pop_back();
  }
}

// ---- environment -----------------------------------------------------------

HLE64_ONLY("ucrtbase.dll", getenv, kCdecl) {
  std::string name = S(Arg(c, 0));
  gaddr env = Rd64(G().environ);
  if (!env) {
    Wr64(G().environ, GuestStrings(EnvironmentVars()));
    env = Rd64(G().environ);
  }
  for (gaddr e; (e = Rd64(env)); env += 8) {
    const char* v = S(e);
    if (StrICmp(v, name.c_str(), name.size()) == 0 && v[name.size()] == '=')
      return e + name.size() + 1;
  }
  return 0;
}

// ---- elementary functions ------------------------------------------------
// Correctly rounded (CORE-MATH, cmake/coremath.cmake), so the same on every
// host; the UCRT's own are not correctly rounded and may differ from these
// in the last bit. Domain and range errors set the guest's errno, as the
// UCRT does. Computed in the host's round-to-nearest mode.

extern "C" {
float cr_acosf(float), cr_asinf(float), cr_atanf(float), cr_atan2f(float, float),
    cr_cosf(float), cr_logf(float), cr_log2f(float), cr_log10f(float),
    cr_powf(float, float), cr_sinf(float), cr_tanf(float);
double cr_atan(double), cr_cos(double), cr_exp(double), cr_log(double),
    cr_pow(double, double), cr_sin(double);
}

namespace {
template <typename T, typename F>
T WithErrno(F f) {
  errno = 0;
  T v = f();
  if (errno == EDOM || errno == ERANGE) SetErrno(uint32_t(errno));
  return v;
}
}  // namespace

#define RECOMP_CR_F1(name)                                                  \
  HLE64_ONLY("ucrtbase.dll", name, kCdecl) {                                     \
    return RetF(c, WithErrno<float>([&] { return cr_##name(ArgF(c, 0)); })); \
  }
#define RECOMP_CR_D1(name)                                                   \
  HLE64_ONLY("ucrtbase.dll", name, kCdecl) {                                      \
    return RetD(c, WithErrno<double>([&] { return cr_##name(ArgD(c, 0)); })); \
  }
RECOMP_CR_F1(acosf)
RECOMP_CR_F1(asinf)
RECOMP_CR_F1(atanf)
RECOMP_CR_F1(cosf)
RECOMP_CR_F1(logf)
RECOMP_CR_F1(log2f)
RECOMP_CR_F1(log10f)
RECOMP_CR_F1(sinf)
RECOMP_CR_F1(tanf)
RECOMP_CR_D1(atan)
RECOMP_CR_D1(cos)
RECOMP_CR_D1(exp)
RECOMP_CR_D1(log)
RECOMP_CR_D1(sin)
#undef RECOMP_CR_F1
#undef RECOMP_CR_D1
HLE64_ONLY("ucrtbase.dll", atan2f, kCdecl) {
  return RetF(c, WithErrno<float>([&] { return cr_atan2f(ArgF(c, 0), ArgF(c, 1)); }));
}
HLE64_ONLY("ucrtbase.dll", powf, kCdecl) {
  return RetF(c, WithErrno<float>([&] { return cr_powf(ArgF(c, 0), ArgF(c, 1)); }));
}
HLE64_ONLY("ucrtbase.dll", pow, kCdecl) {
  return RetD(c, WithErrno<double>([&] { return cr_pow(ArgD(c, 0), ArgD(c, 1)); }));
}

// ---- math with exact results (the same on every IEEE host) ---------------

HLE64_ONLY("ucrtbase.dll", sqrt, kCdecl) { return RetD(c, std::sqrt(ArgD(c, 0))); }
HLE64_ONLY("ucrtbase.dll", sqrtf, kCdecl) { return RetF(c, std::sqrt(ArgF(c, 0))); }
HLE64("ucrtbase.dll", ceil, kCdecl) { return RetD(c, std::ceil(ArgD(c, 0))); }
HLE64_ONLY("ucrtbase.dll", ceilf, kCdecl) { return RetF(c, std::ceil(ArgF(c, 0))); }
HLE64("ucrtbase.dll", floor, kCdecl) { return RetD(c, std::floor(ArgD(c, 0))); }
HLE64_ONLY("ucrtbase.dll", floorf, kCdecl) { return RetF(c, std::floor(ArgF(c, 0))); }
HLE64_ONLY("ucrtbase.dll", truncf, kCdecl) { return RetF(c, std::trunc(ArgF(c, 0))); }
HLE64("ucrtbase.dll", roundf, kCdecl) { return RetF(c, std::round(ArgF(c, 0))); }
HLE64_ONLY("ucrtbase.dll", fmod, kCdecl) { return RetD(c, std::fmod(ArgD(c, 0), ArgD(c, 1))); }
HLE64_ONLY("ucrtbase.dll", fmodf, kCdecl) { return RetF(c, std::fmod(ArgF(c, 0), ArgF(c, 1))); }
// _isnan(x): nonzero for a NaN (exponent all ones, fraction nonzero).
HLE64("ucrtbase.dll", _isnan, kCdecl) {
  double x = ArgD(c, 0);
  uint64_t b;
  std::memcpy(&b, &x, 8);
  return ((b >> 52) & 0x7FF) == 0x7FF && (b & 0xFFFFFFFFFFFFFull) ? 1 : 0;
}

// (double, int): on 32-bit guests the int follows the double's two slots.
HLE64("ucrtbase.dll", ldexp, kCdecl) {
  return RetD(c, std::ldexp(ArgD(c, 0), int32_t(Arg(c, g_proc->x64 ? 1 : 2))));
}
HLE64_ONLY("ucrtbase.dll", modf, kCdecl) {
  double ip;
  double f = std::modf(ArgD(c, 0), &ip);
  uint64_t b;
  std::memcpy(&b, &ip, 8);
  Wr64(Arg(c, 1), b);
  return RetD(c, f);
}
HLE64_ONLY("ucrtbase.dll", nextafterf, kCdecl) {
  return RetF(c, std::nextafter(ArgF(c, 0), ArgF(c, 1)));
}
// _dclass/_fdclass/_ldclass: FP_INFINITE 1, FP_NAN 2, FP_NORMAL -1,
// FP_SUBNORMAL -2, FP_ZERO 0 (MSVC's values). long double is double.
namespace {
uint64_t Class(double v) {
  switch (std::fpclassify(v)) {
    case FP_INFINITE: return 1;
    case FP_NAN: return 2;
    case FP_NORMAL: return uint32_t(-1);
    case FP_SUBNORMAL: return uint32_t(-2);
    default: return 0;
  }
}
}  // namespace
HLE64_ONLY("ucrtbase.dll", _dclass, kCdecl) { return Class(ArgD(c, 0)); }
HLE64_ONLY("ucrtbase.dll", _ldclass, kCdecl) { return Class(ArgD(c, 0)); }
HLE64_ONLY("ucrtbase.dll", _fdclass, kCdecl) { return Class(ArgF(c, 0)); }
HLE64_ONLY("ucrtbase.dll", _dsign, kCdecl) { return std::signbit(ArgD(c, 0)) ? 0x8000 : 0; }

}  // namespace recomp::hle
