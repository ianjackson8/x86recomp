// The INI rules of GetPrivateProfileString / WritePrivateProfileString
// (runtime/hle/ini.h): lookup, quoting, enumeration, edits that keep the
// other lines, both encodings. Portable.

#include <cstdio>
#include <string>

#include "runtime/hle/ini.h"

namespace ini = recomp::hle::ini;
static int g_fail = 0;
static void Eq(const std::u16string& got, const std::u16string& want, const char* what) {
  if (got != want) {
    std::printf("FAIL %s: got \"%s\" want \"%s\"\n", what, recomp::win32::Narrow(got).c_str(),
                recomp::win32::Narrow(want).c_str());
    g_fail++;
  }
}
static void True(bool v, const char* what) {
  if (!v) {
    std::printf("FAIL %s\n", what);
    g_fail++;
  }
}

int main() {
  const std::string text =
      "[Graphics]\r\n"
      "  Width = 1920  \r\n"
      "Title=\"Quoted value\"\r\n"
      "single='x'\r\n"
      ";comment=7\r\n"
      "width=999\r\n"
      "\r\n"
      "[ Audio ]\r\n"
      "Volume=80\r\n";
  ini::File f = ini::Parse(text);
  True(!f.utf16, "ANSI detected");
  Eq(*ini::Get(f, u"graphics", u"WIDTH"), u"1920", "case-insensitive, trimmed, first wins");
  Eq(*ini::Get(f, u"Graphics", u"Title"), u"Quoted value", "double quotes stripped");
  Eq(*ini::Get(f, u"Graphics", u"single"), u"x", "single quotes stripped");
  Eq(*ini::Get(f, u"Graphics", u";comment"), u"7", "no comment syntax");
  Eq(*ini::Get(f, u"audio", u"volume"), u"80", "section name trimmed");
  True(!ini::Get(f, u"Audio", u"Width"), "keys are per section");
  True(!ini::Get(f, u"Nope", u"x"), "missing section");
  auto secs = ini::Sections(f);
  True(secs.size() == 2 && secs[0] == u"Graphics" && secs[1] == u"Audio", "section list");
  auto keys = ini::Keys(f, u"GRAPHICS");
  True(keys.size() == 5 && keys[0] == u"Width" && keys[3] == u";comment", "key list");

  // Edits: replace in place, add after the section's last line, new
  // section at the end, deletions; everything else stays.
  ini::Set(f, u"graphics", u"WIDTH", u"1280");
  Eq(f.lines[1], u"Width=1280", "replace keeps the line's key spelling");
  ini::Set(f, u"Graphics", u"Height", u"720");
  Eq(f.lines[6], u"Height=720", "new key after the section's last non-blank line");
  Eq(f.lines[7], u"", "blank line kept before the next section");
  ini::Set(f, u"Network", u"Port", u"8080");
  Eq(f.lines[f.lines.size() - 2], u"[Network]", "new section appended");
  ini::Set(f, u"Audio", u"Volume", std::nullopt);
  True(!ini::Get(f, u"Audio", u"Volume"), "key deleted");
  ini::DeleteSection(f, u"network");
  True(ini::Sections(f).size() == 2, "section deleted");
  const std::string out = ini::Serialize(f);
  True(out.find("Width=1280\r\nTitle=\"Quoted value\"\r\n") != std::string::npos,
       "serialised with CRLF, other lines verbatim");

  // UTF-16LE with a BOM round-trips as UTF-16.
  std::string w = "\xFF\xFE";
  for (char16_t ch : std::u16string(u"[S]\r\nName=é中\r\n")) {
    w.push_back(char(ch & 0xFF));
    w.push_back(char(ch >> 8));
  }
  ini::File u = ini::Parse(w);
  True(u.utf16, "UTF-16 detected");
  Eq(*ini::Get(u, u"s", u"name"), u"é中", "UTF-16 value");
  True(ini::Serialize(u) == w, "UTF-16 round trip");

  std::printf("ini: %d failures\n", g_fail);
  return g_fail ? 1 : 0;
}
