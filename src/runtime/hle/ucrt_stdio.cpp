// The UCRT's stdio (x64 and, where marked HLE64, 32-bit guests): FILE streams on host files and the
// printf/scanf families. A FILE* is a guest pointer to a small opaque block
// (the UCRT's FILE is opaque too); the host stream behind it is kept here.
// Text mode is Windows': reads drop the \r of \r\n and stop at ^Z, writes
// turn \n into \r\n. Formatting follows the UCRT (Visual Studio 2015 and
// later), not the host's printf where they differ: %p is 16 uppercase hex
// digits, NaNs print as nan, nan(ind) or nan(snan).

#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <unistd.h>
#include <sys/stat.h>
#include <map>
#include <mutex>
#include <string>

#include "runtime/hle/hle.h"
#include "runtime/hle/kernel32_state.h"
#include "runtime/hle/ucrt.h"
#include "runtime/hle/win32.h"
#include "runtime/log.h"
#include "runtime/runtime.h"

namespace recomp::hle {
namespace {

struct Stream {
  FILE* f = nullptr;
  bool text = false;
  bool owned = true;  // fclose closes it (not stdin/out/err)
};

std::recursive_mutex g_streams_mu;
std::map<gaddr, Stream> g_streams;  // by guest FILE*

// The guest FILE block: zeroed, so its first fields (the UCRT's _ptr,
// _base, _cnt: _get_stream_buffer_pointers) say the buffer is empty.
gaddr NewStream(FILE* f, bool text, bool owned) {
  gaddr p = HeapAllocProcess(32, true);
  std::lock_guard<std::recursive_mutex> l(g_streams_mu);
  g_streams[p] = {f, text, owned};
  return p;
}

// The three standard streams (__acrt_iob_func(0..2)).
gaddr StdStream(uint32_t i) {
  static gaddr s[3] = {NewStream(stdin, true, false), NewStream(stdout, true, false),
                       NewStream(stderr, true, false)};
  return i < 3 ? s[i] : 0;
}

Stream* Find(gaddr fp) {
  std::lock_guard<std::recursive_mutex> l(g_streams_mu);
  auto it = g_streams.find(fp);
  return it == g_streams.end() ? nullptr : &it->second;
}

Stream& Must(gaddr fp, const char* api) {
  Stream* s = Find(fp);
  if (!s) FatalF("%s on an unknown FILE* %08llX", api, (unsigned long long)fp);
  return *s;
}

int GetC(Stream& s) {
  int ch = std::fgetc(s.f);
  if (!s.text) return ch;
  if (ch == 0x1A) {  // ^Z ends a text stream
    std::ungetc(ch, s.f);
    return EOF;
  }
  if (ch == '\r') {
    int nx = std::fgetc(s.f);
    if (nx == '\n') return '\n';
    if (nx != EOF) std::ungetc(nx, s.f);
  }
  return ch;
}

size_t Write(Stream& s, const char* p, size_t n) {
  if (!s.text) return std::fwrite(p, 1, n, s.f);
  size_t done = 0;
  for (; done < n; done++) {
    // The host's own stdout and stderr keep plain newlines.
    if (p[done] == '\n' && s.owned && std::fputc('\r', s.f) == EOF) break;
    if (std::fputc(p[done], s.f) == EOF) break;
  }
  return done;
}

// ---- printf ----------------------------------------------------------------

// Successive variadic arguments: 8-byte slots of a guest va_list (floating
// point ones as doubles).
// A va_list. x64: every argument in an 8-byte slot. 32-bit guests: packed
// on 4-byte boundaries; 64-bit integers and doubles take 8 bytes, the rest
// (int, long, pointers, size_t) 4.
struct Args {
  gaddr p;
  bool x64 = g_proc->x64;
  uint64_t Next(int bits = 32) {
    uint64_t v;
    if (x64 || bits == 64) {
      v = Rd64(p);
      p += 8;
    } else {
      v = Rd32(p);
      p += 4;
    }
    return v;
  }
  gaddr NextPtr() { return Next(x64 ? 64 : 32); }
  double NextD() {
    uint64_t b = Next(64);
    double d;
    std::memcpy(&d, &b, 8);
    return d;
  }
  int SizeBits() const { return x64 ? 64 : 32; }  // size_t, ptrdiff_t
};

std::string NanText(double v, bool upper) {
  uint64_t b;
  std::memcpy(&b, &v, 8);
  std::string s = (b >> 63) ? "-" : "";
  bool quiet = b & (1ull << 51);
  if (!quiet)
    s += "nan(snan)";
  else if ((b & 0x7FFFFFFFFFFFFull) == 0x8000000000000ull && (b >> 63))
    s += "nan(ind)";  // the default NaN (0/0): sign set, empty payload
  else
    s += "nan";
  if (upper)
    for (char& ch : s) ch = char(std::toupper(uint8_t(ch)));
  return s;
}

// Pads `body` to `width` characters (left-justified with '-').
std::u16string Pad(const std::u16string& body, int width, bool left) {
  if (int(body.size()) >= width) return body;
  return left ? body + std::u16string(width - body.size(), u' ')
              : std::u16string(width - body.size(), u' ') + body;
}
std::u16string Ascii(const std::string& s) { return std::u16string(s.begin(), s.end()); }
// A char string as code units: its byte values (the "C" locale's
// char <-> wchar_t mapping).
std::u16string Bytes(const std::string& s) {
  std::u16string r;
  for (unsigned char ch : s) r.push_back(ch);
  return r;
}
bool IsAsciiIn(char16_t ch, const char* set) { return ch < 0x80 && ch && std::strchr(set, char(ch)); }

// The UCRT's printf over a guest format and va_list, in code units. Narrow
// functions (wide_fn false: %s and %c are char, %S and %C wchar_t) produce
// byte values, a wchar_t string going through the ANSI code page; wide
// functions (%s/%c wchar_t, %S/%C char) produce UTF-16, a char string's
// bytes becoming code units.
std::u16string FormatCore(const std::u16string& fmt, Args a, bool three_digit_exponents,
                          bool wide_fn) {
  std::u16string out;
  for (size_t i = 0; i < fmt.size(); i++) {
    if (fmt[i] != '%') {
      out.push_back(fmt[i]);
      continue;
    }
    size_t start = i++;
    std::string flags;
    while (i < fmt.size() && IsAsciiIn(fmt[i], "-+ #0")) flags.push_back(char(fmt[i++]));
    int width = -1, prec = -1;
    if (i < fmt.size() && fmt[i] == '*') {
      width = int32_t(a.Next());
      if (width < 0) {
        flags.push_back('-');
        width = -width;
      }
      i++;
    } else {
      while (i < fmt.size() && IsAsciiIn(fmt[i], "0123456789"))
        width = (width < 0 ? 0 : width) * 10 + (fmt[i++] - u'0');
    }
    if (i < fmt.size() && fmt[i] == '.') {
      i++;
      prec = 0;
      if (i < fmt.size() && fmt[i] == '*') {
        prec = int32_t(a.Next());
        i++;
      } else {
        while (i < fmt.size() && IsAsciiIn(fmt[i], "0123456789")) prec = prec * 10 + (fmt[i++] - u'0');
      }
    }
    // Length: hh h l ll L j z t I I32 I64 w.
    int bits = 32;
    bool wide = false, narrow = false;
    for (;;) {
      if (i >= fmt.size()) break;
      char ch = fmt[i] < 0x80 ? char(fmt[i]) : 0;
      if (ch == 'h') {
        bits = (i + 1 < fmt.size() && fmt[i + 1] == 'h') ? (i++, 8) : 16;
        narrow = true;
        i++;
      } else if (ch == 'l') {
        if (i + 1 < fmt.size() && fmt[i + 1] == 'l') {
          bits = 64;
          i += 2;
        } else {
          wide = true;  // long is 32-bit; %ls/%lc are wide
          i++;
        }
      } else if (ch == 'L' || ch == 'j') {
        bits = 64;
        i++;
      } else if (ch == 'z' || ch == 't') {
        bits = a.SizeBits();
        i++;
      } else if (ch == 'w') {
        wide = true;
        i++;
      } else if (ch == 'I') {
        if (fmt.compare(i, 3, u"I64") == 0) {
          bits = 64;
          i += 3;
        } else if (fmt.compare(i, 3, u"I32") == 0) {
          bits = 32;
          i += 3;
        } else {
          bits = a.SizeBits();  // size_t
          i++;
        }
      } else {
        break;
      }
    }
    if (i >= fmt.size()) {
      out += fmt.substr(start);
      break;
    }
    char conv = fmt[i] < 0x80 ? char(fmt[i]) : '?';
    std::string spec = "%" + flags + (width >= 0 ? std::to_string(width) : "") +
                       (prec >= 0 ? "." + std::to_string(prec) : "");
    char buf[512];
    switch (conv) {
      case '%':
        out.push_back(u'%');
        break;
      case 'd': case 'i': {
        uint64_t v = a.Next(bits);
        long long s = bits == 64 ? int64_t(v) : bits == 16 ? int16_t(v) : bits == 8 ? int8_t(v)
                                                                             : int32_t(v);
        std::snprintf(buf, sizeof buf, (spec + "lld").c_str(), s);
        out += Ascii(buf);
        break;
      }
      case 'u': case 'o': case 'x': case 'X': {
        uint64_t v = a.Next(bits);
        unsigned long long u = bits == 64 ? v : bits == 16 ? uint16_t(v) : bits == 8 ? uint8_t(v)
                                                                           : uint32_t(v);
        std::snprintf(buf, sizeof buf, (spec + "ll" + conv).c_str(), u);
        out += Ascii(buf);
        break;
      }
      case 'p': {
        // As many hex digits as a pointer has: 16 (x64) or 8.
        if (a.x64) std::snprintf(buf, sizeof buf, "%016llX", (unsigned long long)a.NextPtr());
        else std::snprintf(buf, sizeof buf, "%08llX", (unsigned long long)a.NextPtr());
        out += Pad(Ascii(buf), width, flags.find('-') != std::string::npos);
        break;
      }
      case 'c': case 'C': {
        uint64_t v = a.Next();
        // The argument's width, as for %s.
        bool w = conv == 'C' ? (wide_fn ? wide : !narrow) : (wide_fn ? !narrow : wide);
        std::u16string ch;
        if (w && wide_fn) ch = std::u16string(1, char16_t(v));                   // wchar_t out
        else if (w) ch = Bytes(win32::Narrow(std::u16string(1, char16_t(v))));  // via ANSI
        else ch = std::u16string(1, char16_t(uint8_t(v)));                        // a char
        out += Pad(ch, width, flags.find('-') != std::string::npos);
        break;
      }
      case 's': case 'S': {
        gaddr p = a.NextPtr();
        // Which width the argument has: the function's own unless S (the
        // other one) or h/l/w say otherwise.
        bool w = conv == 'S' ? (wide_fn ? wide : !narrow) : (wide_fn ? !narrow : wide);
        std::u16string s = !p                 ? Ascii("(null)")
                           : w && wide_fn     ? GWStr(p)
                           : w                ? Bytes(win32::Narrow(GWStr(p)))
                                              : Bytes(GStr(p));
        if (prec >= 0 && size_t(prec) < s.size()) s.resize(size_t(prec));
        out += Pad(s, width, flags.find('-') != std::string::npos);
        break;
      }
      case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': case 'a': case 'A': {
        double v = a.NextD();
        bool upper = std::isupper(uint8_t(conv));
        if (std::isnan(v) || std::isinf(v)) {
          std::string s = std::isnan(v) ? NanText(v, upper)
                                        : std::string(std::signbit(v) ? "-" : "") +
                                              (upper ? "INF" : "inf");
          if (!std::signbit(v) && flags.find('+') != std::string::npos) s = "+" + s;
          else if (!std::signbit(v) && flags.find(' ') != std::string::npos) s = " " + s;
          out += Pad(Ascii(s), width, flags.find('-') != std::string::npos);
          break;
        }
        if (conv == 'a' || conv == 'A')
          FatalF("guest printf: %%%c is not supported yet", conv);
        std::snprintf(buf, sizeof buf, (spec + conv).c_str(), v);
        std::string s = buf;
        if (three_digit_exponents) {  // the legacy e+005: three exponent digits
          size_t e = s.find_first_of("eE");
          if (e != std::string::npos && s.size() - e == 4) s.insert(e + 2, "0");
        }
        out += Ascii(s);
        break;
      }
      case 'n':
        FatalF("guest printf: %%n (disabled by default in the UCRT)");
      default:
        // An invalid conversion: the UCRT's invalid parameter handler.
        FatalF("guest printf: invalid conversion %%%c in \"%s\"", conv, win32::Narrow(fmt).c_str());
    }
  }
  return out;
}

// The narrow functions' text: FormatCore's units are bytes.
std::string Format(const std::string& fmt, Args a, bool three_digit_exponents) {
  const std::u16string u = FormatCore(Bytes(fmt), a, three_digit_exponents, false);
  return std::string(u.begin(), u.end());
}

// __stdio_common_v*printf options.
constexpr uint64_t kStandardSnprintf = 2, kThreeDigitExponents = 16;

// Writes s into the guest buffer the way the option set asks for; returns
// the function's result.
uint64_t PutFormatted(uint64_t options, gaddr buf, uint64_t count, const std::string& s) {
  const uint64_t n = s.size();
  if (!buf) return uint32_t(int32_t(n));
  if (options & kStandardSnprintf) {  // snprintf: truncate, always NUL
    if (count) {
      uint64_t k = std::min(n, count - 1);
      std::memcpy(GPtr(buf), s.data(), k);
      Wr8(buf + k, 0);
    }
    return uint32_t(int32_t(n));
  }
  // _vsnprintf: -1 if it does not fit; NUL only when there is room.
  uint64_t k = std::min(n, count);
  std::memcpy(GPtr(buf), s.data(), k);
  if (n < count) Wr8(buf + n, 0);
  return n <= count ? uint32_t(int32_t(n)) : uint32_t(-1);
}

// The __stdio_common_* functions start with an unsigned __int64 options
// argument: two stack slots on 32-bit guests, which shift the others.
uint64_t Opt(const Ctx& c) {
  return g_proc->x64 ? Arg(c, 0) : (Arg(c, 0) & 0xFFFFFFFFu) | (Arg(c, 1) << 32);
}
uint64_t ArgAfterOpt(const Ctx& c, int i) { return Arg(c, g_proc->x64 ? i : i + 1); }
// size_t(-1) (_TRUNCATE) at the guest's size_t width.
bool IsTruncate(uint64_t v) { return v == (g_proc->x64 ? ~0ull : 0xFFFFFFFFull); }

}  // namespace

HLE64("ucrtbase.dll", __acrt_iob_func, kCdecl) { return StdStream(uint32_t(Arg(c, 0))); }

// __stdio_common_vsprintf(options, buffer, count, format, locale, va_list)
HLE64("ucrtbase.dll", __stdio_common_vsprintf, kCdecl) {
  uint64_t opt = Opt(c);
  std::string s = Format(GStr(ArgAfterOpt(c, 3)), Args{ArgAfterOpt(c, 5)},
                         opt & kThreeDigitExponents);
  return PutFormatted(opt, ArgAfterOpt(c, 1), ArgAfterOpt(c, 2), s);
}

// __stdio_common_vsprintf_s: a buffer too small is an invalid parameter.
HLE64("ucrtbase.dll", __stdio_common_vsprintf_s, kCdecl) {
  uint64_t opt = Opt(c);
  gaddr buf = ArgAfterOpt(c, 1);
  uint64_t count = ArgAfterOpt(c, 2);
  std::string s = Format(GStr(ArgAfterOpt(c, 3)), Args{ArgAfterOpt(c, 5)},
                         opt & kThreeDigitExponents);
  if (!buf || s.size() + 1 > count)
    FatalF("guest sprintf_s: %zu characters into a buffer of %llu", s.size(),
           (unsigned long long)count);
  std::memcpy(GPtr(buf), s.c_str(), s.size() + 1);
  return uint32_t(s.size());
}

// __stdio_common_vsnprintf_s(options, buffer, size, max_count, format,
// locale, va_list): max_count _TRUNCATE ((size_t)-1) truncates to the
// buffer and returns -1 when it did.
HLE64("ucrtbase.dll", __stdio_common_vsnprintf_s, kCdecl) {
  uint64_t opt = Opt(c);
  gaddr buf = ArgAfterOpt(c, 1);
  uint64_t size = ArgAfterOpt(c, 2), max = ArgAfterOpt(c, 3);
  std::string s = Format(GStr(ArgAfterOpt(c, 4)), Args{ArgAfterOpt(c, 6)},
                         opt & kThreeDigitExponents);
  if (!buf || !size) FatalF("guest vsnprintf_s: no buffer");
  const bool trunc = IsTruncate(max);
  uint64_t limit = std::min(trunc ? size - 1 : max, size - 1);
  bool truncated = s.size() > limit;
  if (truncated && !trunc && max >= size)
    FatalF("guest vsnprintf_s: %zu characters into a buffer of %llu", s.size(),
           (unsigned long long)size);
  uint64_t k = std::min<uint64_t>(s.size(), limit);
  std::memcpy(GPtr(buf), s.data(), k);
  Wr8(buf + k, 0);
  return truncated ? uint32_t(-1) : uint32_t(k);
}

// __stdio_common_vfprintf(options, stream, format, locale, va_list)
HLE64("ucrtbase.dll", __stdio_common_vfprintf, kCdecl) {
  uint64_t opt = Opt(c);
  Stream& st = Must(ArgAfterOpt(c, 1), "fprintf");
  std::string s = Format(GStr(ArgAfterOpt(c, 2)), Args{ArgAfterOpt(c, 4)},
                         opt & kThreeDigitExponents);
  std::lock_guard<std::recursive_mutex> l(g_streams_mu);
  return Write(st, s.data(), s.size()) == s.size() ? uint32_t(s.size()) : uint32_t(-1);
}

// ---- wide printf -------------------------------------------------------------------

namespace {
// The wide functions' format, as code units; the _p (positional) forms
// take %n$ arguments, not modelled yet: fatal if a format uses one.
std::u16string WideFormat(gaddr fmt, bool positional) {
  std::u16string f = GWStr(fmt);
  if (positional)
    for (size_t i = 0; i + 1 < f.size(); i++)
      if (f[i] == u'%' && f[i + 1] >= u'1' && f[i + 1] <= u'9') {
        size_t k = i + 1;
        while (k < f.size() && f[k] >= u'0' && f[k] <= u'9') k++;
        if (k < f.size() && f[k] == u'$') FatalF("guest wprintf_p: positional arguments not supported yet");
      }
  return f;
}
// PutFormatted for wchar_t buffers (count in characters).
uint64_t PutFormattedW(uint64_t options, gaddr buf, uint64_t count, const std::u16string& s) {
  const uint64_t n = s.size();
  if (!buf) return uint32_t(int32_t(n));
  auto put = [&](uint64_t k) {
    for (uint64_t i = 0; i < k; i++) Wr16(buf + 2 * i, s[i]);
  };
  if (options & kStandardSnprintf) {
    if (count) {
      uint64_t k = std::min(n, count - 1);
      put(k);
      Wr16(buf + 2 * k, 0);
    }
    return uint32_t(int32_t(n));
  }
  uint64_t k = std::min(n, count);
  put(k);
  if (n < count) Wr16(buf + 2 * n, 0);
  return n <= count ? uint32_t(int32_t(n)) : uint32_t(-1);
}
uint64_t VSwprintf(Ctx& c, bool positional) {
  uint64_t opt = Opt(c);
  std::u16string s = FormatCore(WideFormat(ArgAfterOpt(c, 3), positional), Args{ArgAfterOpt(c, 5)},
                                opt & kThreeDigitExponents, true);
  return PutFormattedW(opt, ArgAfterOpt(c, 1), ArgAfterOpt(c, 2), s);
}
// To a stream: the "C" locale's wctomb (units 0-255 to that byte); any
// other character fails with EILSEQ.
uint64_t VFwprintf(Ctx& c, bool positional) {
  uint64_t opt = Opt(c);
  Stream& st = Must(ArgAfterOpt(c, 1), "fwprintf");
  std::u16string s = FormatCore(WideFormat(ArgAfterOpt(c, 2), positional), Args{ArgAfterOpt(c, 4)},
                                opt & kThreeDigitExponents, true);
  std::string b;
  for (char16_t ch : s) {
    if (ch > 0xFF) {
      Wr32(GuestErrno(), 42);  // EILSEQ
      return uint32_t(-1);
    }
    b.push_back(char(ch));
  }
  std::lock_guard<std::recursive_mutex> l(g_streams_mu);
  return Write(st, b.data(), b.size()) == b.size() ? uint32_t(s.size()) : uint32_t(-1);
}
}  // namespace

// __stdio_common_vswprintf(options, buffer, count, format, locale, va_list)
HLE64("ucrtbase.dll", __stdio_common_vswprintf, kCdecl) { return VSwprintf(c, false); }
HLE64("ucrtbase.dll", __stdio_common_vswprintf_p, kCdecl) { return VSwprintf(c, true); }
// _s: a buffer too small is an invalid parameter.
HLE64("ucrtbase.dll", __stdio_common_vswprintf_s, kCdecl) {
  uint64_t opt = Opt(c);
  gaddr buf = ArgAfterOpt(c, 1);
  uint64_t count = ArgAfterOpt(c, 2);
  std::u16string s = FormatCore(WideFormat(ArgAfterOpt(c, 3), false), Args{ArgAfterOpt(c, 5)},
                                opt & kThreeDigitExponents, true);
  if (!buf || s.size() + 1 > count)
    FatalF("guest swprintf_s: %zu characters into a buffer of %llu", s.size(),
           (unsigned long long)count);
  for (size_t i = 0; i <= s.size(); i++) Wr16(buf + 2 * i, i < s.size() ? s[i] : 0);
  return uint32_t(s.size());
}
// __stdio_common_vsnwprintf_s(options, buffer, size, max_count, format,
// locale, va_list): as vsnprintf_s, in characters.
HLE64("ucrtbase.dll", __stdio_common_vsnwprintf_s, kCdecl) {
  uint64_t opt = Opt(c);
  gaddr buf = ArgAfterOpt(c, 1);
  uint64_t size = ArgAfterOpt(c, 2), max = ArgAfterOpt(c, 3);
  std::u16string s = FormatCore(WideFormat(ArgAfterOpt(c, 4), false), Args{ArgAfterOpt(c, 6)},
                                opt & kThreeDigitExponents, true);
  if (!buf || !size) FatalF("guest vsnwprintf_s: no buffer");
  const bool trunc = IsTruncate(max);
  uint64_t limit = std::min(trunc ? size - 1 : max, size - 1);
  bool truncated = s.size() > limit;
  if (truncated && !trunc && max >= size)
    FatalF("guest vsnwprintf_s: %zu characters into a buffer of %llu", s.size(),
           (unsigned long long)size);
  uint64_t k = std::min<uint64_t>(s.size(), limit);
  for (uint64_t i = 0; i < k; i++) Wr16(buf + 2 * i, s[i]);
  Wr16(buf + 2 * k, 0);
  return truncated ? uint32_t(-1) : uint32_t(k);
}
HLE64("ucrtbase.dll", __stdio_common_vfwprintf, kCdecl) { return VFwprintf(c, false); }
HLE64("ucrtbase.dll", __stdio_common_vfwprintf_s, kCdecl) { return VFwprintf(c, false); }
HLE64("ucrtbase.dll", __stdio_common_vfwprintf_p, kCdecl) { return VFwprintf(c, true); }

// ---- msvcrt's printf family ----------------------------------------------------------
// The legacy rules (PutFormatted without the standard-snprintf option): no
// NUL when the text fills the buffer exactly, -1 when it does not fit.
// cdecl; the variadic forms take their arguments after the format.
HLE("msvcrt.dll", _vsnprintf, kCdecl) {
  std::string s = Format(GStr(Arg(c, 2)), Args{Arg(c, 3)}, true);
  return PutFormatted(0, Arg(c, 0), Arg(c, 1), s);
}
HLE("msvcrt.dll", _snprintf, kCdecl) {
  std::string s = Format(GStr(Arg(c, 2)), Args{c.esp + 4 + 12}, true);
  return PutFormatted(0, Arg(c, 0), Arg(c, 1), s);
}
HLE("msvcrt.dll", _vsnwprintf, kCdecl) {
  std::u16string s = FormatCore(GWStr(Arg(c, 2)), Args{Arg(c, 3)}, true, true);
  return PutFormattedW(0, Arg(c, 0), Arg(c, 1), s);
}

// ---- streams --------------------------------------------------------------------

namespace {
// fopen's mode: r w a, +, b/t; anything else is the UCRT's invalid
// parameter. The host file, or null with the guest's errno set.
FILE* OpenHost(const std::string& path, const std::string& mode, bool* text) {
  if (mode.empty() || !std::strchr("rwa", mode[0]))
    FatalF("guest fopen(\"%s\", \"%s\"): invalid mode", path.c_str(), mode.c_str());
  bool plus = mode.find('+') != std::string::npos;
  *text = mode.find('b') == std::string::npos;  // text unless "b" (_fmode)
  PathAccess acc = mode[0] == 'r' ? (plus ? PathAccess::kUpdate : PathAccess::kRead)
                                  : PathAccess::kWrite;
  if (mode[0] == 'a') acc = PathAccess::kUpdate;
  std::string host = HostPath(path, acc);
  std::string m(1, mode[0]);
  if (plus) m += '+';
  m += 'b';
  FILE* f = std::fopen(host.c_str(), m.c_str());
  RECOMP_TRACE("  fopen(%s, %s) -> %s%s\n", path.c_str(), mode.c_str(), host.c_str(),
               f ? "" : " (failed)");
  if (!f)
    Wr32(GuestErrno(), uint32_t(errno == ENOENT ? ENOENT : errno == EACCES ? EACCES : EINVAL));
  return f;
}

gaddr Open(const std::string& path, const std::string& mode) {
  bool text;
  FILE* f = OpenHost(path, mode, &text);
  return f ? NewStream(f, text, true) : 0;
}

// freopen: the stream is closed, then the same FILE* opens `path`; on
// failure it stays closed and the result is null.
gaddr Reopen(const std::string& path, const std::string& mode, gaddr fp) {
  std::lock_guard<std::recursive_mutex> l(g_streams_mu);
  Stream& s = Must(fp, "freopen");
  if (s.owned) std::fclose(s.f);
  else std::fflush(s.f);
  bool text;
  FILE* f = OpenHost(path, mode, &text);
  if (!f) {
    g_streams.erase(fp);
    return 0;
  }
  s = {f, text, true};
  return fp;
}
}  // namespace

// _getcwd(buf, maxlen): the guest's current directory, as
// GetCurrentDirectory gives it; NULL buf allocates (at least maxlen). Too
// small a buffer: ERANGE and NULL.
HLE64("ucrtbase.dll", _getcwd, kCdecl) {
  const std::string d = GuestCurrentDir();
  gaddr buf = Arg(c, 0);
  const uint64_t max = uint64_t(int32_t(Arg(c, 1)));
  if (!buf) {
    buf = HeapAllocProcess(std::max<uint64_t>(max, d.size() + 1), false);
  } else if (d.size() + 1 > max) {
    Wr32(GuestErrno(), ERANGE);
    return 0;
  }
  PutStr(buf, d);
  return buf;
}
HLE64("ucrtbase.dll", fopen, kCdecl) { return Open(GStr(Arg(c, 0)), GStr(Arg(c, 1))); }
// remove(path) / _unlink(path): as DeleteFile (the game install is never
// written: a file that exists only there is EACCES); 0, or -1 with errno
// ENOENT, or EACCES for a directory or a file that cannot be deleted.
uint32_t CrtRemove(const std::string& g) {
  const std::string host = HostPath(g), writable = HostPath(g, PathAccess::kWrite);
  struct stat st;
  int err = 0;
  if (host != writable && stat(host.c_str(), &st) == 0) err = EACCES;
  else if (unlink(writable.c_str()) != 0) err = errno == ENOENT ? ENOENT : EACCES;
  if (!err) return 0;
  Wr32(GuestErrno(), uint32_t(err));
  return uint32_t(-1);
}
HLE64("ucrtbase.dll", remove, kCdecl) { return CrtRemove(GStr(Arg(c, 0))); }
HLE64("ucrtbase.dll", _unlink, kCdecl) { return CrtRemove(GStr(Arg(c, 0))); }

// _fsopen(path, mode, shflag) / _wfsopen: fopen; sharing modes (_SH_DENYWR,
// ...) do not apply to the host's files.
HLE64("ucrtbase.dll", _fsopen, kCdecl) { return Open(GStr(Arg(c, 0)), GStr(Arg(c, 1))); }
HLE64("ucrtbase.dll", _wfsopen, kCdecl) {
  return Open(NarrowPath(GWStr(Arg(c, 0))), win32::Narrow(GWStr(Arg(c, 1))));
}
HLE64_ONLY("ucrtbase.dll", _wfopen, kCdecl) {
  return Open(NarrowPath(GWStr(Arg(c, 0))), win32::Narrow(GWStr(Arg(c, 1))));
}
// _wfreopen(path, mode, stream); a null path (changing the mode of the open
// file) is not supported.
HLE64_ONLY("ucrtbase.dll", _wfreopen, kCdecl) {
  if (!Arg(c, 0)) FatalF("_wfreopen with a null path (mode change) is not supported");
  return Reopen(NarrowPath(GWStr(Arg(c, 0))), win32::Narrow(GWStr(Arg(c, 1))), Arg(c, 2));
}
// fopen_s(&file, path, mode): 0 or the errno; the FILE* at pointer width.
HLE64("ucrtbase.dll", fopen_s, kCdecl) {
  gaddr f = Open(GStr(Arg(c, 1)), GStr(Arg(c, 2)));
  if (g_proc->x64) Wr64(Arg(c, 0), f);
  else Wr32(Arg(c, 0), uint32_t(f));
  return f ? 0 : Rd32(GuestErrno());
}

HLE64("ucrtbase.dll", fclose, kCdecl) {
  gaddr fp = Arg(c, 0);
  std::lock_guard<std::recursive_mutex> l(g_streams_mu);
  auto it = g_streams.find(fp);
  if (it == g_streams.end()) return uint32_t(EOF);
  int r = it->second.owned ? std::fclose(it->second.f) : std::fflush(it->second.f);
  if (it->second.owned) {
    g_streams.erase(it);
    HeapFreeProcess(fp);
  }
  return uint32_t(r);
}

HLE64("ucrtbase.dll", fread, kCdecl) {
  gaddr buf = Arg(c, 0);
  uint64_t size = Arg(c, 1), n = Arg(c, 2);
  Stream& s = Must(Arg(c, 3), "fread");
  std::lock_guard<std::recursive_mutex> l(g_streams_mu);
  if (!size || !n) return 0;
  if (!s.text) return std::fread(GPtr(buf), size_t(size), size_t(n), s.f);
  uint64_t total = size * n, got = 0;
  for (; got < total; got++) {
    int ch = GetC(s);
    if (ch == EOF) break;
    Wr8(buf + got, uint8_t(ch));
  }
  return got / size;
}

HLE64("ucrtbase.dll", fwrite, kCdecl) {
  gaddr buf = Arg(c, 0);
  uint64_t size = Arg(c, 1), n = Arg(c, 2);
  Stream& s = Must(Arg(c, 3), "fwrite");
  std::lock_guard<std::recursive_mutex> l(g_streams_mu);
  if (!size || !n) return 0;
  return Write(s, g_proc->mem.ptr<const char>(buf), size * n) / size;
}

HLE64("ucrtbase.dll", fgets, kCdecl) {
  gaddr buf = Arg(c, 0);
  int n = int32_t(Arg(c, 1));
  Stream& s = Must(Arg(c, 2), "fgets");
  std::lock_guard<std::recursive_mutex> l(g_streams_mu);
  if (n <= 0) return 0;
  int i = 0;
  while (i < n - 1) {
    int ch = GetC(s);
    if (ch == EOF) break;
    Wr8(buf + i++, uint8_t(ch));
    if (ch == '\n') break;
  }
  if (!i) return 0;
  Wr8(buf + i, 0);
  return buf;
}

HLE64("ucrtbase.dll", fputs, kCdecl) {
  std::string str = GStr(Arg(c, 0));
  Stream& s = Must(Arg(c, 1), "fputs");
  std::lock_guard<std::recursive_mutex> l(g_streams_mu);
  return Write(s, str.data(), str.size()) == str.size() ? 0 : uint32_t(EOF);
}

// getc/fgetc(f): the next byte (text streams: CRLF as LF, ^Z as the end),
// or EOF. ungetc(ch, f): pushes one byte back. feof(f).
HLE64("ucrtbase.dll", fgetc, kCdecl) {
  Stream& s = Must(Arg(c, 0), "fgetc");
  std::lock_guard<std::recursive_mutex> l(g_streams_mu);
  return uint32_t(GetC(s));
}
HLE64("ucrtbase.dll", getc, kCdecl) {
  Stream& s = Must(Arg(c, 0), "getc");
  std::lock_guard<std::recursive_mutex> l(g_streams_mu);
  return uint32_t(GetC(s));
}
HLE64("ucrtbase.dll", ungetc, kCdecl) {
  const int ch = int32_t(Arg(c, 0));
  Stream& s = Must(Arg(c, 1), "ungetc");
  std::lock_guard<std::recursive_mutex> l(g_streams_mu);
  return uint32_t(ch == EOF ? EOF : std::ungetc(uint8_t(ch), s.f));
}
HLE64("ucrtbase.dll", feof, kCdecl) { return std::feof(Must(Arg(c, 0), "feof").f) != 0; }

// _get_stream_buffer_pointers(f, &base, &ptr, &count): where the FILE's
// buffer fields are (msvcp140's filebuf reads through them). They stay
// NULL/0 (no buffer the guest can read ahead in), so every access goes
// through the stdio functions here.
HLE64("ucrtbase.dll", _get_stream_buffer_pointers, kCdecl) {
  const gaddr f = Arg(c, 0);
  Must(f, "_get_stream_buffer_pointers");
  const uint32_t ps = g_proc->x64 ? 8 : 4;
  auto put = [&](gaddr out, gaddr v) {
    if (!out) return;
    if (ps == 8) Wr64(out, v);
    else Wr32(out, uint32_t(v));
  };
  put(Arg(c, 2), f);           // &_ptr
  put(Arg(c, 1), f + ps);      // &_base
  put(Arg(c, 3), f + 2 * ps);  // &_cnt
  return 0;
}

HLE64("ucrtbase.dll", fputc, kCdecl) {
  char ch = char(Arg(c, 0));
  Stream& s = Must(Arg(c, 1), "fputc");
  std::lock_guard<std::recursive_mutex> l(g_streams_mu);
  return Write(s, &ch, 1) == 1 ? uint8_t(ch) : uint32_t(EOF);
}

HLE64("ucrtbase.dll", fflush, kCdecl) {
  gaddr fp = Arg(c, 0);
  std::lock_guard<std::recursive_mutex> l(g_streams_mu);
  if (!fp) return uint32_t(std::fflush(nullptr));
  Stream& s = Must(fp, "fflush");
  return uint32_t(std::fflush(s.f));
}

HLE64("ucrtbase.dll", ferror, kCdecl) { return std::ferror(Must(Arg(c, 0), "ferror").f) != 0; }

HLE64("ucrtbase.dll", fseek, kCdecl) {
  Stream& s = Must(Arg(c, 0), "fseek");
  std::lock_guard<std::recursive_mutex> l(g_streams_mu);
  return uint32_t(std::fseek(s.f, long(int32_t(Arg(c, 1))), int(Arg(c, 2))));
}
HLE64_ONLY("ucrtbase.dll", _fseeki64, kCdecl) {
  Stream& s = Must(Arg(c, 0), "_fseeki64");
  std::lock_guard<std::recursive_mutex> l(g_streams_mu);
  return uint32_t(fseeko(s.f, off_t(int64_t(Arg(c, 1))), int(Arg(c, 2))));
}
HLE64("ucrtbase.dll", ftell, kCdecl) {
  Stream& s = Must(Arg(c, 0), "ftell");
  std::lock_guard<std::recursive_mutex> l(g_streams_mu);
  long p = std::ftell(s.f);
  return uint32_t(int32_t(p > INT32_MAX ? -1 : p));  // long is 32-bit
}
HLE64("ucrtbase.dll", rewind, kCdecl) {
  Stream& s = Must(Arg(c, 0), "rewind");
  std::lock_guard<std::recursive_mutex> l(g_streams_mu);
  std::rewind(s.f);
  return 0;
}

}  // namespace recomp::hle
