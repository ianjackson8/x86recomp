// shlwapi's ANSI path helpers (runtime/hle/shlwapi_path.h) on the examples
// of Microsoft's documentation for each function. Portable.

#include <cstdio>
#include <string>

#include "runtime/hle/shlwapi_path.h"

namespace sh = recomp::hle::shlwapi;
static int g_fail = 0;
static void Eq(const std::string& got, const std::string& want, const char* what) {
  if (got != want) {
    std::printf("FAIL %s: got \"%s\" want \"%s\"\n", what, got.c_str(), want.c_str());
    g_fail++;
  }
}
static void EqI(long got, long want, const char* what) {
  if (got != want) {
    std::printf("FAIL %s: got %ld want %ld\n", what, got, want);
    g_fail++;
  }
}

int main() {
  // PathCanonicalize (documentation's four examples).
  Eq(sh::Canonicalize("A:\\name_1\\.\\name_2\\..\\name_3"), "A:\\name_1\\name_3", "canon 1");
  Eq(sh::Canonicalize("A:\\name_1\\..\\name_2\\.\\name_3"), "A:\\name_2\\name_3", "canon 2");
  Eq(sh::Canonicalize("A:\\name_1\\name_2\\.\\name_3\\..\\name_4"), "A:\\name_1\\name_2\\name_4",
     "canon 3");
  Eq(sh::Canonicalize("A:\\name_1\\.\\name_2\\.\\name_3\\..\\name_4\\.."), "A:\\name_1\\name_2",
     "canon 4");
  Eq(sh::Canonicalize("C:\\..\\x"), "C:\\x", "canon: .. stops at the root");
  Eq(sh::Canonicalize("data\\a\\..\\b.bin"), "data\\b.bin", "canon: relative");
  Eq(sh::Canonicalize("C:\\a\\"), "C:\\a\\", "canon: trailing separator kept");

  // PathCombine (documentation: "C:" + "One\Two\Three", and climbing back).
  Eq(sh::Combine("C:", "One\\Two\\Three"), "C:\\One\\Two\\Three", "combine 1");
  Eq(sh::Combine("C:\\One\\Two\\Three", "..\\..\\..\\Four"), "C:\\Four", "combine 2");
  Eq(sh::Combine("C:\\Dir", "D:\\Other\\f.txt"), "D:\\Other\\f.txt", "combine: absolute file");
  Eq(sh::Combine("C:\\Dir", "\\Root\\f.txt"), "C:\\Root\\f.txt", "combine: drive root");

  // PathRemoveFileSpec (documentation: "C:\TEST\sample.txt" -> "C:\TEST").
  std::string p = "C:\\TEST\\sample.txt";
  EqI(sh::RemoveFileSpec(&p), 1, "removefilespec changed");
  Eq(p, "C:\\TEST", "removefilespec");
  p = "C:\\";
  EqI(sh::RemoveFileSpec(&p), 0, "removefilespec root unchanged");
  Eq(p, "C:\\", "removefilespec root");
  p = "C:\\file.txt";
  sh::RemoveFileSpec(&p);
  Eq(p, "C:\\", "removefilespec keeps root separator");

  // PathSkipRoot (documentation: "C:\path1\path2" -> "path1\path2").
  std::string s = "C:\\path1\\path2";
  Eq(s.substr(sh::SkipRoot(s)), "path1\\path2", "skiproot drive");
  s = "\\\\server\\share\\dir";
  Eq(s.substr(sh::SkipRoot(s)), "dir", "skiproot UNC");
  EqI(long(sh::SkipRoot("relative\\x")), long(std::string::npos), "skiproot relative");

  // PathGetDriveNumber (documentation: "C:\TEST\bar.txt" -> 2), PathIsRelative.
  EqI(sh::DriveNumber("C:\\TEST\\bar.txt"), 2, "drive C");
  EqI(sh::DriveNumber("z:"), 25, "drive z");
  EqI(sh::DriveNumber("\\x"), -1, "no drive");
  EqI(sh::IsRelative("c:\\a"), 0, "isrelative drive");
  EqI(sh::IsRelative("\\a"), 0, "isrelative root");
  EqI(sh::IsRelative("file.txt"), 1, "isrelative file");

  // PathRemoveBlanks (documentation: leading and trailing spaces).
  Eq(sh::RemoveBlanks("    sample string     "), "sample string", "removeblanks");
  Eq(sh::RemoveBlanks("   "), "", "removeblanks all");

  std::printf("shlwapi_path: %d failures\n", g_fail);
  return g_fail ? 1 : 0;
}
