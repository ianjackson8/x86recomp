#pragma once
// The INI files of GetPrivateProfileString / WritePrivateProfileString, as
// UTF-16 lines (kernel32_profile.cpp reads and writes the files). Windows'
// rules: sections "[name]", entries "key=value", names compared without
// case, keys and values trimmed of blanks, one pair of surrounding quotes
// stripped from a value on reading; there is no comment syntax (";x=1" is
// a key named ";x"), and the first of duplicate names wins. A file is
// UTF-16LE when it starts with a BOM, else ANSI (Windows-1252). Edits keep
// every other line, the encoding and CRLF line ends. Pure; tested by
// tests/ini_test.cpp.

#include <optional>
#include <string>
#include <vector>

#include "runtime/hle/win32.h"

namespace recomp::hle::ini {

struct File {
  bool utf16 = false;
  std::vector<std::u16string> lines;
};

inline bool Blank(char16_t ch) { return ch == u' ' || ch == u'\t'; }
inline std::u16string Trim(const std::u16string& s) {
  size_t a = 0, b = s.size();
  while (a < b && Blank(s[a])) a++;
  while (b > a && Blank(s[b - 1])) b--;
  return s.substr(a, b - a);
}
inline char16_t Fold(char16_t ch) { return ch >= u'A' && ch <= u'Z' ? char16_t(ch + 32) : ch; }
inline bool Same(const std::u16string& a, const std::u16string& b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); i++)
    if (Fold(a[i]) != Fold(b[i])) return false;
  return true;
}

inline File Parse(const std::string& bytes) {
  File f;
  std::u16string text;
  if (bytes.size() >= 2 && uint8_t(bytes[0]) == 0xFF && uint8_t(bytes[1]) == 0xFE) {
    f.utf16 = true;
    for (size_t i = 2; i + 1 < bytes.size(); i += 2)
      text.push_back(char16_t(uint8_t(bytes[i]) | uint8_t(bytes[i + 1]) << 8));
  } else {
    text = win32::Widen(bytes);
  }
  size_t s = 0;
  while (s < text.size()) {
    size_t e = text.find(u'\n', s);
    if (e == std::u16string::npos) e = text.size();
    std::u16string line = text.substr(s, e - s);
    if (!line.empty() && line.back() == u'\r') line.pop_back();
    f.lines.push_back(line);
    s = e + 1;
  }
  return f;
}

inline std::string Serialize(const File& f) {
  std::u16string text;
  for (const auto& l : f.lines) text += l + u"\r\n";
  if (!f.utf16) return win32::Narrow(text);
  std::string b = "\xFF\xFE";
  for (char16_t ch : text) {
    b.push_back(char(ch & 0xFF));
    b.push_back(char(ch >> 8));
  }
  return b;
}

// A section header's name, if the line is one.
inline std::optional<std::u16string> SectionName(const std::u16string& line) {
  std::u16string t = Trim(line);
  if (t.empty() || t[0] != u'[') return std::nullopt;
  size_t e = t.find(u']');
  return Trim(t.substr(1, e == std::u16string::npos ? std::u16string::npos : e - 1));
}
// [first line after the header, end) of the first section named sec;
// begin == end and found false if there is none.
struct Range {
  size_t header, begin, end;
  bool found;
};
inline Range Find(const File& f, const std::u16string& sec) {
  for (size_t i = 0; i < f.lines.size(); i++) {
    auto n = SectionName(f.lines[i]);
    if (n && Same(*n, sec)) {
      size_t e = i + 1;
      while (e < f.lines.size() && !SectionName(f.lines[e])) e++;
      return {i, i + 1, e, true};
    }
  }
  return {0, 0, 0, false};
}
inline bool KeyOf(const std::u16string& line, std::u16string* key, std::u16string* value) {
  size_t eq = line.find(u'=');
  if (eq == std::u16string::npos) return false;
  *key = Trim(line.substr(0, eq));
  *value = Trim(line.substr(eq + 1));
  return true;
}

inline std::optional<std::u16string> Get(const File& f, const std::u16string& sec,
                                         const std::u16string& key) {
  Range r = Find(f, sec);
  for (size_t i = r.begin; i < r.end; i++) {
    std::u16string k, v;
    if (KeyOf(f.lines[i], &k, &v) && Same(k, key)) {
      if (v.size() >= 2 && (v[0] == u'"' || v[0] == u'\'') && v.back() == v[0])
        v = v.substr(1, v.size() - 2);
      return v;
    }
  }
  return std::nullopt;
}
inline std::vector<std::u16string> Sections(const File& f) {
  std::vector<std::u16string> r;
  for (const auto& l : f.lines)
    if (auto n = SectionName(l)) r.push_back(*n);
  return r;
}
inline std::vector<std::u16string> Keys(const File& f, const std::u16string& sec) {
  std::vector<std::u16string> r;
  Range rg = Find(f, sec);
  for (size_t i = rg.begin; i < rg.end; i++) {
    std::u16string k, v;
    if (KeyOf(f.lines[i], &k, &v)) r.push_back(k);
  }
  return r;
}

// WritePrivateProfileString: value nullopt deletes the key; Set with an
// empty key deletes the section (the API's NULL key).
inline void Set(File& f, const std::u16string& sec, const std::u16string& key,
                const std::optional<std::u16string>& value) {
  Range r = Find(f, sec);
  for (size_t i = r.begin; i < r.end; i++) {
    std::u16string k, v;
    if (KeyOf(f.lines[i], &k, &v) && Same(k, key)) {
      if (value) f.lines[i] = k + u"=" + *value;
      else f.lines.erase(f.lines.begin() + long(i));
      return;
    }
  }
  if (!value) return;
  if (!r.found) {
    f.lines.push_back(u"[" + sec + u"]");
    f.lines.push_back(key + u"=" + *value);
    return;
  }
  size_t at = r.end;  // after the section's last non-blank line
  while (at > r.begin && Trim(f.lines[at - 1]).empty()) at--;
  f.lines.insert(f.lines.begin() + long(at), key + u"=" + *value);
}
inline void DeleteSection(File& f, const std::u16string& sec) {
  Range r = Find(f, sec);
  if (r.found) f.lines.erase(f.lines.begin() + long(r.header), f.lines.begin() + long(r.end));
}

}  // namespace recomp::hle::ini
