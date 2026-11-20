#pragma once
// shlwapi's ANSI path helpers on host strings (the handlers in
// shell_net.cpp copy guest strings in and out). Windows paths: '\' is the
// separator ('/' is an ordinary character here, as in shlwapi), a root is
// "X:\", "\" or "\\server\share\". Pure functions, tested on any host.

#include <string>
#include <vector>

namespace recomp::hle::shlwapi {

// PathGetDriveNumber: 0-25 for "A:".."Z:" (either case), else -1.
inline int DriveNumber(const std::string& p) {
  if (p.size() >= 2 && p[1] == ':') {
    char d = p[0];
    if (d >= 'a' && d <= 'z') return d - 'a';
    if (d >= 'A' && d <= 'Z') return d - 'A';
  }
  return -1;
}

// PathIsRelative: not "\..." and not "X:...".
inline bool IsRelative(const std::string& p) {
  if (!p.empty() && p[0] == '\\') return false;
  return !(p.size() >= 2 && p[1] == ':');
}

// The length of p's root ("X:\", "X:", "\", "\\server\share\"), 0 if none.
inline size_t RootLength(const std::string& p) {
  if (p.size() >= 2 && p[0] == '\\' && p[1] == '\\') {  // UNC: \\server\share[\]
    size_t s = p.find('\\', 2);
    if (s == std::string::npos) return p.size();
    size_t t = p.find('\\', s + 1);
    return t == std::string::npos ? p.size() : t + 1;
  }
  if (DriveNumber(p) >= 0) return p.size() >= 3 && p[2] == '\\' ? 3 : 2;
  if (!p.empty() && p[0] == '\\') return 1;
  return 0;
}

// PathSkipRoot: the offset past the root, or npos if p has none (a
// relative path, or a bare "X:" without its backslash).
inline size_t SkipRoot(const std::string& p) {
  size_t r = RootLength(p);
  if (r == 0 || (DriveNumber(p) >= 0 && r == 2)) return std::string::npos;
  return r;
}

// PathRemoveBlanks: leading and trailing spaces removed.
inline std::string RemoveBlanks(const std::string& p) {
  size_t a = p.find_first_not_of(' ');
  if (a == std::string::npos) return {};
  size_t b = p.find_last_not_of(' ');
  return p.substr(a, b - a + 1);
}

// PathRemoveFileSpec: the last component and its separator removed (a
// root's separator stays); whether anything changed.
inline bool RemoveFileSpec(std::string* p) {
  const size_t root = RootLength(*p);
  const size_t k = p->find_last_of('\\');
  // No separator past the root: only the root remains.
  const std::string r = p->substr(0, k == std::string::npos || k < root ? root : k);
  const bool changed = r != *p;
  *p = r;
  return changed;
}

// PathCanonicalize: "." and ".." components resolved (".." never climbs
// above the root); an empty path becomes "\".
inline std::string Canonicalize(const std::string& p) {
  if (p.empty()) return "\\";
  const size_t root = RootLength(p);
  std::string out = p.substr(0, root);
  std::vector<std::string> parts;
  bool trailing = false;
  size_t i = root;
  while (i <= p.size()) {
    size_t j = p.find('\\', i);
    if (j == std::string::npos) j = p.size();
    std::string seg = p.substr(i, j - i);
    const bool last = j == p.size();
    if (seg == "..") {
      if (!parts.empty()) parts.pop_back();
      trailing = !last;
    } else if (seg == ".") {
      trailing = !last;
    } else if (!seg.empty()) {
      parts.push_back(seg);
      trailing = !last;
    } else if (last) {
      trailing = !parts.empty() && p.back() == '\\';
    }
    i = j + 1;
  }
  for (size_t k = 0; k < parts.size(); k++) {
    if (k) out += '\\';
    out += parts[k];
  }
  if (trailing && !parts.empty()) out += '\\';
  if (out.empty()) out = "\\";
  return out;
}

// PathCombine(dir, file): file if it is absolute; with a leading '\' only,
// it goes on dir's drive root; else dir\file. Then canonicalized.
inline std::string Combine(const std::string& dir, const std::string& file) {
  std::string r;
  if (file.empty()) r = dir;
  else if (dir.empty() || DriveNumber(file) >= 0 || (file.size() >= 2 && file[0] == '\\' && file[1] == '\\'))
    r = file;
  else if (file[0] == '\\') r = (DriveNumber(dir) >= 0 ? dir.substr(0, 2) : std::string()) + file;
  else r = dir + (dir.back() == '\\' ? "" : "\\") + file;
  return Canonicalize(r);
}

}  // namespace recomp::hle::shlwapi
