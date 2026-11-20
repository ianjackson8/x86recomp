// kernel32: INI files (GetPrivateProfileString, WritePrivateProfileString),
// on ini.h. A file name without a path is in the Windows directory, as on
// Windows. Writes go through HostPath's overlay, so an install's files are
// never changed. A new file is ANSI; an existing one keeps its encoding.
// The A and W forms differ only in their strings (ANSI is Windows-1252).

#include <fstream>
#include <iterator>
#include <mutex>
#include <optional>
#include <sstream>

#include "runtime/hle/hle.h"
#include "runtime/hle/ini.h"
#include "runtime/hle/kernel32_state.h"
#include "runtime/hle/win32.h"
#include "runtime/log.h"
#include "runtime/runtime.h"

namespace recomp::hle {
namespace {

std::mutex g_ini_mu;

std::string GuestIniPath(const std::u16string& name) {
  std::string n = NarrowPath(name);
  if (n.find('\\') == std::string::npos && n.find('/') == std::string::npos &&
      n.find(':') == std::string::npos)
    n = "C:\\WINDOWS\\" + n;
  return n;
}

ini::File Load(const std::string& guest) {
  std::ifstream in(HostPath(guest), std::ios::binary);
  if (!in) return {};
  std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  return ini::Parse(bytes);
}

// Strings in and out of the guest for the A (narrow) or W form.
std::optional<std::u16string> In(gaddr p, bool wide) {
  if (!p) return std::nullopt;
  return wide ? GWStr(p) : win32::Widen(GStr(p));
}
void PutChars(gaddr buf, const std::u16string& s, bool wide) {
  if (wide) {
    for (size_t i = 0; i < s.size(); i++) Wr16(buf + 2 * i, s[i]);
  } else {
    const std::string n = win32::Narrow(s);
    for (size_t i = 0; i < n.size(); i++) Wr8(buf + i, uint8_t(n[i]));
  }
}
void PutNul(gaddr buf, uint64_t i, bool wide) {
  if (wide) Wr16(buf + 2 * i, 0);
  else Wr8(buf + i, 0);
}

// GetPrivateProfileString(app, key, default, buf, size, file)
uint32_t GetProfileString(Ctx& c, bool wide) {
  const auto app = In(Arg(c, 0), wide), key = In(Arg(c, 1), wide), def = In(Arg(c, 2), wide);
  const gaddr buf = Arg(c, 3);
  const uint64_t size = Arg(c, 4);
  const auto file = In(Arg(c, 5), wide);
  if (!buf || !size) return 0;
  RECOMP_TRACE("  profile %s: [%s] %s\n", file ? GuestIniPath(*file).c_str() : "(none)",
               app ? win32::Narrow(*app).c_str() : "(sections)",
               key ? win32::Narrow(*key).c_str() : "(keys)");
  ini::File f;
  {
    std::lock_guard<std::mutex> l(g_ini_mu);
    if (file) f = Load(GuestIniPath(*file));
  }
  if (!app || !key) {  // a list of names, each NUL-terminated, then a NUL
    const std::vector<std::u16string> names = !app ? ini::Sections(f) : ini::Keys(f, *app);
    std::u16string all;
    for (const auto& n : names) all += n + u'\0';
    if (all.size() + 1 > size) {  // cut to size - 2, then two NULs
      all.resize(size >= 2 ? size - 2 : 0);
      PutChars(buf, all, wide);
      PutNul(buf, all.size(), wide);
      if (size >= 2) PutNul(buf, all.size() + 1, wide);
      return uint32_t(size >= 2 ? size - 2 : 0);
    }
    PutChars(buf, all, wide);
    PutNul(buf, all.size(), wide);
    return uint32_t(all.size());
  }
  std::u16string v;
  if (auto got = ini::Get(f, *app, *key)) {
    v = *got;
  } else if (def) {  // the default's trailing blanks are removed
    v = *def;
    while (!v.empty() && ini::Blank(v.back())) v.pop_back();
  }
  if (v.size() > size - 1) v.resize(size - 1);
  PutChars(buf, v, wide);
  PutNul(buf, v.size(), wide);
  return uint32_t(v.size());
}

// WritePrivateProfileString(app, key, string, file): NULL key deletes the
// section, NULL string the key.
uint32_t WriteProfileString(Ctx& c, bool wide) {
  const auto app = In(Arg(c, 0), wide), key = In(Arg(c, 1), wide), str = In(Arg(c, 2), wide);
  const auto file = In(Arg(c, 3), wide);
  if (!file) return 1;  // all NULL: flush the (non-existent) cache
  if (!app) {
    SetLastError(win32::kErrorInvalidParameter);
    return 0;
  }
  std::lock_guard<std::mutex> l(g_ini_mu);
  const std::string guest = GuestIniPath(*file);
  ini::File f = Load(guest);
  if (!key) ini::DeleteSection(f, *app);
  else ini::Set(f, *app, *key, str);
  std::ofstream out(HostPath(guest, PathAccess::kUpdate), std::ios::binary | std::ios::trunc);
  if (!out) {
    SetLastError(5);  // ERROR_ACCESS_DENIED
    return 0;
  }
  out << ini::Serialize(f);
  return 1;
}

}  // namespace

HLE64("kernel32.dll", GetPrivateProfileStringA, 6) { return GetProfileString(c, false); }
HLE64("kernel32.dll", GetPrivateProfileStringW, 6) { return GetProfileString(c, true); }
HLE64("kernel32.dll", WritePrivateProfileStringA, 4) { return WriteProfileString(c, false); }
HLE64("kernel32.dll", WritePrivateProfileStringW, 4) { return WriteProfileString(c, true); }

}  // namespace recomp::hle
