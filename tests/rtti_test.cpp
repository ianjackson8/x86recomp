// MSVC RTTI casts (runtime/hle/rtti.h) over hand-built tables in MSVC's
// 32-bit layout: single and multiple inheritance, downcasts, cross casts,
// visibility, ambiguity, and type identity by name across modules.
// Portable.

#include <cstdio>
#include <map>
#include <string>

#include "runtime/hle/rtti.h"

namespace rtti = recomp::hle::rtti;
static int g_fail = 0;
static std::map<uint32_t, uint32_t> g_mem;
static std::map<uint32_t, std::string> g_names;
static uint32_t g_next = 0x10000;

static void W(uint32_t a, uint32_t v) { g_mem[a] = v; }
static uint32_t Rd(uint32_t a) {
  auto it = g_mem.find(a);
  if (it == g_mem.end()) {
    std::printf("FAIL read of unset %08X\n", a);
    g_fail++;
    return 0;
  }
  return it->second;
}
static std::string Name(uint32_t a) { return g_names[a]; }
static uint32_t Alloc(uint32_t n) { uint32_t a = g_next; g_next += (n + 15) & ~15u; return a; }

static uint32_t Type(const char* name) {  // TypeDescriptor
  uint32_t t = Alloc(32);
  W(t, 0), W(t + 4, 0);
  g_names[t + 8] = name;
  return t;
}
static uint32_t Bcd(uint32_t type, uint32_t contained, int32_t mdisp, uint32_t attrs) {
  uint32_t b = Alloc(24);
  W(b, type), W(b + 4, contained), W(b + 8, uint32_t(mdisp)), W(b + 12, uint32_t(-1)), W(b + 16, 0),
      W(b + 20, attrs);
  return b;
}
static uint32_t Chd(uint32_t attrs, std::initializer_list<uint32_t> bcds) {
  uint32_t arr = Alloc(4 * uint32_t(bcds.size())), i = 0;
  for (uint32_t b : bcds) W(arr + 4 * i++, b);
  uint32_t h = Alloc(16);
  W(h, 0), W(h + 4, attrs), W(h + 8, uint32_t(bcds.size())), W(h + 12, arr);
  return h;
}
// An object subobject at `obj + offset` with a vftable whose [-1] is a COL.
static void Vfptr(uint32_t obj, int32_t offset, uint32_t type, uint32_t chd) {
  uint32_t col = Alloc(20);
  W(col, 0), W(col + 4, uint32_t(offset)), W(col + 8, 0), W(col + 12, type), W(col + 16, chd);
  uint32_t vt = Alloc(8);
  W(vt, col);
  W(obj + uint32_t(offset), vt + 4);
}
static void Expect(rtti::Result r, uint32_t got, rtti::Result want_r, uint32_t want, const char* what) {
  if (r != want_r || got != want) {
    std::printf("FAIL %s: result %d -> %08X, want %d -> %08X\n", what, int(r), got, int(want_r), want);
    g_fail++;
  }
}

int main() {
  const rtti::Read32 rd = Rd;
  const rtti::ReadStr nm = Name;
  uint32_t out;

  // Single inheritance: struct D : B {}, and an unrelated X.
  const uint32_t tB = Type(".?AVB@@"), tD = Type(".?AVD@@"), tX = Type(".?AVX@@"),
                 tP = Type(".?AVP@@");
  // D's bases: D, B, and a private base P (not visible from D's users).
  const uint32_t hD = Chd(0, {Bcd(tD, 2, 0, 0), Bcd(tB, 0, 0, 0), Bcd(tP, 0, 0, rtti::kBcdNotVisible)});
  const uint32_t d = Alloc(16);
  Vfptr(d, 0, tD, hD);
  auto r = rtti::DynamicCast(d, 0, tB, tD, rd, nm, &out);
  Expect(r, out, rtti::Result::kOk, d, "SI downcast B* -> D*");
  r = rtti::DynamicCast(d, 0, tB, tX, rd, nm, &out);
  Expect(r, out, rtti::Result::kNull, 0, "SI unrelated -> null");
  r = rtti::DynamicCast(d, 0, tD, tP, rd, nm, &out);
  Expect(r, out, rtti::Result::kNull, 0, "SI private base -> null");
  r = rtti::DynamicCast(0, 0, tB, tD, rd, nm, &out);
  Expect(r, out, rtti::Result::kNull, 0, "null stays null");
  if (rtti::TypeOf(d, rd) != tD) g_fail++, std::printf("FAIL typeid\n");
  if (rtti::CompleteObject(d, rd) != d) g_fail++, std::printf("FAIL dynamic_cast<void*>\n");

  // Multiple inheritance: struct C : A, B2 {} with B2 at +8. A second
  // descriptor of B2 (another module's) has the same name.
  const uint32_t tA = Type(".?AVA@@"), tB2 = Type(".?AVB2@@"), tB2other = Type(".?AVB2@@"),
                 tC = Type(".?AVC@@");
  const uint32_t hC = Chd(rtti::kChdMultiple, {Bcd(tC, 2, 0, 0), Bcd(tA, 0, 0, 0), Bcd(tB2, 0, 8, 0)});
  const uint32_t cobj = Alloc(32);
  Vfptr(cobj, 0, tC, hC);
  Vfptr(cobj, 8, tC, hC);
  r = rtti::DynamicCast(cobj + 8, 0, tB2, tC, rd, nm, &out);
  Expect(r, out, rtti::Result::kOk, cobj, "MI downcast B2* (at +8) -> C*");
  r = rtti::DynamicCast(cobj + 8, 0, tB2, tA, rd, nm, &out);
  Expect(r, out, rtti::Result::kOk, cobj, "MI cross cast B2* -> A*");
  r = rtti::DynamicCast(cobj, 0, tA, tB2other, rd, nm, &out);
  Expect(r, out, rtti::Result::kOk, cobj + 8, "MI cross cast A* -> B2* (by name)");
  if (rtti::CompleteObject(cobj + 8, rd) != cobj) g_fail++, std::printf("FAIL MI dynamic_cast<void*>\n");

  // Ambiguous: struct E : L, R {} where L and R both derive from Q; casting
  // across to Q is ambiguous (two Q subobjects).
  const uint32_t tQ = Type(".?AVQ@@"), tL = Type(".?AVL@@"), tR = Type(".?AVR@@"),
                 tE = Type(".?AVE@@"), tZ = Type(".?AVZ@@");
  const uint32_t hE = Chd(rtti::kChdMultiple,
                          {Bcd(tE, 5, 0, 0), Bcd(tL, 1, 0, 0), Bcd(tQ, 0, 0, rtti::kBcdAmbiguous),
                           Bcd(tR, 1, 8, 0), Bcd(tQ, 0, 8, rtti::kBcdAmbiguous), Bcd(tZ, 0, 16, 0)});
  const uint32_t e = Alloc(32);
  Vfptr(e, 0, tE, hE);
  Vfptr(e, 8, tE, hE);
  Vfptr(e, 16, tE, hE);
  r = rtti::DynamicCast(e + 16, 0, tZ, tQ, rd, nm, &out);
  Expect(r, out, rtti::Result::kNull, 0, "MI ambiguous cross cast -> null");
  r = rtti::DynamicCast(e + 8, 0, tQ, tR, rd, nm, &out);
  Expect(r, out, rtti::Result::kOk, e + 8, "MI downcast Q* (under R) -> R*");

  std::printf("rtti: %d failures\n", g_fail);
  return g_fail ? 1 : 0;
}
