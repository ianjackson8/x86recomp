#pragma once
// MSVC's run-time type information (32-bit layout), for dynamic_cast,
// typeid and dynamic_cast<void*> (vcruntime's __RTDynamicCast, __RTtypeid,
// __RTCastToVoid). Pure: guest memory is read through a callback, so
// tests/rtti_test.cpp can build tables in MSVC's layout.
//
// vftable[-1] -> RTTICompleteObjectLocator {signature, offset, cdOffset,
//   TypeDescriptor*, ClassHierarchyDescriptor*}
// ClassHierarchyDescriptor {signature, attributes (1 multiple, 2 virtual
//   inheritance), numBaseClasses, BaseClassDescriptor** array}: every base
//   class subobject of the complete object, pre-order, the class itself first
// BaseClassDescriptor {TypeDescriptor*, numContainedBases, PMD {mdisp,
//   pdisp, vdisp}, attributes (1 not visible, 2 ambiguous, ...)}
// TypeDescriptor {vftable, spare, decorated name}: two are the same type if
//   they are the same descriptor or have the same name (across modules).
//
// The cast follows C++'s rules over that table: with single inheritance
// the first visible base of the target type; otherwise the source
// subobject (by type and offset) is found, a downcast takes the one target
// subobject that contains it, else a cross cast takes the target base if
// it is visible and not ambiguous. A case these rules do not decide is
// Result::kUndecided (the caller makes it a fatal error).

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace recomp::hle::rtti {

using Read32 = std::function<uint32_t(uint32_t)>;
using ReadStr = std::function<std::string(uint32_t)>;

constexpr uint32_t kChdMultiple = 1;
constexpr uint32_t kBcdNotVisible = 1, kBcdAmbiguous = 2;

struct Base {
  uint32_t type, contained, attributes;
  int32_t mdisp, pdisp, vdisp;
};

struct Object {
  uint32_t complete = 0;   // the complete object's address
  uint32_t col = 0;        // its complete object locator
  uint32_t hierarchy = 0;  // ClassHierarchyDescriptor
};

inline Object Locate(uint32_t p, const Read32& rd) {
  Object o;
  o.col = rd(rd(p) - 4);
  const int32_t offset = int32_t(rd(o.col + 4)), cd = int32_t(rd(o.col + 8));
  o.complete = p - uint32_t(offset);
  if (cd) o.complete -= rd(p - uint32_t(cd));
  o.hierarchy = rd(o.col + 16);
  return o;
}

inline std::vector<Base> Bases(uint32_t hierarchy, const Read32& rd) {
  const uint32_t n = rd(hierarchy + 8), arr = rd(hierarchy + 12);
  std::vector<Base> v;
  for (uint32_t i = 0; i < n; i++) {
    const uint32_t b = rd(arr + 4 * i);
    v.push_back({rd(b), rd(b + 4), rd(b + 20), int32_t(rd(b + 8)), int32_t(rd(b + 12)),
                 int32_t(rd(b + 16))});
  }
  return v;
}

// The subobject's offset in the complete object (PMD: through the virtual
// base table when pdisp >= 0).
inline int32_t Offset(uint32_t complete, const Base& b, const Read32& rd) {
  int32_t off = b.mdisp;
  if (b.pdisp >= 0) {
    const uint32_t vbtable = rd(complete + uint32_t(b.pdisp));
    off += b.pdisp + int32_t(rd(vbtable + uint32_t(b.vdisp)));
  }
  return off;
}

inline bool SameType(uint32_t a, uint32_t b, const ReadStr& name) {
  return a == b || name(a + 8) == name(b + 8);
}

enum class Result { kOk, kNull, kUndecided };

// dynamic_cast of p (a pointer to a src_type subobject; vfdelta from the
// compiler) to target_type: *out gets the result.
inline Result DynamicCast(uint32_t p, int32_t vfdelta, uint32_t src_type, uint32_t target_type,
                          const Read32& rd, const ReadStr& name, uint32_t* out) {
  *out = 0;
  if (!p) return Result::kNull;
  const Object o = Locate(p, rd);
  const std::vector<Base> bases = Bases(o.hierarchy, rd);
  const int32_t delta = int32_t((p - uint32_t(vfdelta)) - o.complete);
  auto result = [&](const Base& b) {
    *out = o.complete + uint32_t(Offset(o.complete, b, rd));
    return Result::kOk;
  };
  if (!(rd(o.hierarchy + 4) & kChdMultiple)) {
    for (const Base& b : bases)
      if (SameType(b.type, target_type, name) && !(b.attributes & kBcdNotVisible)) return result(b);
    return Result::kNull;
  }
  // The source subobject.
  int src = -1;
  for (size_t i = 0; i < bases.size(); i++)
    if (SameType(bases[i].type, src_type, name) && Offset(o.complete, bases[i], rd) == delta) {
      src = int(i);
      break;
    }
  if (src < 0) return Result::kUndecided;
  // Downcast: target subobjects that contain the source one.
  std::vector<int> down;
  for (size_t t = 0; t < bases.size(); t++)
    if (SameType(bases[t].type, target_type, name) && int(t) <= src &&
        src <= int(t + bases[t].contained))
      down.push_back(int(t));
  if (down.size() == 1) {
    const Base& t = bases[size_t(down[0])];
    if (t.attributes & kBcdNotVisible) return Result::kUndecided;
    return result(t);
  }
  if (down.size() > 1) return Result::kUndecided;
  // Cross cast: from a visible source, the visible, unambiguous target base.
  if (bases[size_t(src)].attributes & kBcdNotVisible) return Result::kNull;
  const Base* hit = nullptr;
  int count = 0;
  for (const Base& b : bases)
    if (SameType(b.type, target_type, name)) {
      count++;
      if (!(b.attributes & (kBcdNotVisible | kBcdAmbiguous))) hit = &b;
    }
  if (count == 1 && hit) return result(*hit);
  return Result::kNull;
}

// typeid(*p): the complete object's TypeDescriptor; dynamic_cast<void*>(p):
// the complete object.
inline uint32_t TypeOf(uint32_t p, const Read32& rd) { return rd(Locate(p, rd).col + 12); }
inline uint32_t CompleteObject(uint32_t p, const Read32& rd) { return p ? Locate(p, rd).complete : 0; }

}  // namespace recomp::hle::rtti
