#pragma once
// The CRT's portable floating-point control bits (_controlfp, _control87)
// and the two units they drive on 32-bit x86 guests: the x87 control word
// and SSE's MXCSR. Pure functions, so they can be tested on any host.
// Constants as documented for float.h; the bit layouts of FPU CW and
// MXCSR as in the Intel SDM.

#include <cstdint>

namespace recomp::hle::fp {

// float.h
inline constexpr uint32_t kEmInexact = 0x00000001, kEmUnderflow = 0x00000002,
                          kEmOverflow = 0x00000004, kEmZeroDivide = 0x00000008,
                          kEmInvalid = 0x00000010, kEmDenormal = 0x00080000;
inline constexpr uint32_t kMcwEm = 0x0008001F, kMcwIc = 0x00040000, kMcwRc = 0x00000300,
                          kMcwPc = 0x00030000, kMcwDn = 0x03000000;
inline constexpr uint32_t kIcAffine = 0x00040000;
inline constexpr uint32_t kPc64 = 0x00000000, kPc53 = 0x00010000, kPc24 = 0x00020000;
inline constexpr uint32_t kDnSave = 0x00000000, kDnFlush = 0x01000000,
                          kDnFlushOperandsSaveResults = 0x02000000,
                          kDnSaveOperandsFlushResults = 0x03000000;

// x87 CW: IM DM ZM OM UM PM in bits 0-5, PC bits 8-9 (00 = 24-bit,
// 10 = 53-bit, 11 = 64-bit), RC bits 10-11, IC bit 12.
// MXCSR: DAZ bit 6, IM..PM bits 7-12, RC bits 13-14, FZ bit 15.
// The RC encodings (near, down, up, chop) are the same in all three.
struct EmBit {
  uint32_t crt;
  uint16_t x87;
  uint32_t sse;
};
inline constexpr EmBit kEm[] = {
    {kEmInvalid, 0x01, 0x0080},   {kEmDenormal, 0x02, 0x0100}, {kEmZeroDivide, 0x04, 0x0200},
    {kEmOverflow, 0x08, 0x0400},  {kEmUnderflow, 0x10, 0x0800}, {kEmInexact, 0x20, 0x1000},
};
inline constexpr uint32_t kSseDaz = 0x0040, kSseFz = 0x8000;

// The CRT bits that describe an x87 CW (exception masks, RC, PC, IC) and an
// MXCSR's denormal mode.
inline uint32_t FromUnits(uint16_t cw, uint32_t mxcsr) {
  uint32_t r = 0;
  for (const EmBit& e : kEm)
    if (cw & e.x87) r |= e.crt;
  r |= uint32_t((cw >> 10) & 3) << 8;
  const uint32_t pc = (cw >> 8) & 3;
  r |= pc == 0 ? kPc24 : pc == 2 ? kPc53 : kPc64;  // 01 (reserved) reads as 64
  if (cw & 0x1000) r |= kIcAffine;
  const bool daz = mxcsr & kSseDaz, fz = mxcsr & kSseFz;
  r |= daz && fz ? kDnFlush : daz ? kDnFlushOperandsSaveResults
                     : fz       ? kDnSaveOperandsFlushResults
                                : kDnSave;
  return r;
}

// Applies CRT bits `ctl` under `mask` to both units: exception masks and
// rounding to both, precision and infinity control to the x87, the
// denormal mode to SSE.
inline void ToUnits(uint32_t ctl, uint32_t mask, uint16_t* cw, uint32_t* mxcsr) {
  uint16_t c = *cw;
  uint32_t m = *mxcsr;
  for (const EmBit& e : kEm)
    if (mask & e.crt) {
      c = uint16_t(ctl & e.crt ? c | e.x87 : c & ~e.x87);
      m = ctl & e.crt ? m | e.sse : m & ~e.sse;
    }
  if (mask & kMcwRc) {
    const uint32_t rc = (ctl >> 8) & 3;
    c = uint16_t((c & ~0x0C00u) | (rc << 10));
    m = (m & ~0x6000u) | (rc << 13);
  }
  if (mask & kMcwPc) {
    const uint32_t pc = ctl & kMcwPc;
    const uint32_t bits = pc == kPc24 ? 0 : pc == kPc53 ? 2 : 3;
    c = uint16_t((c & ~0x0300u) | (bits << 8));
  }
  if (mask & kMcwIc) c = uint16_t(ctl & kIcAffine ? c | 0x1000 : c & ~0x1000);
  if (mask & kMcwDn) {
    const uint32_t dn = ctl & kMcwDn;
    m &= ~(kSseDaz | kSseFz);
    if (dn == kDnFlush || dn == kDnFlushOperandsSaveResults) m |= kSseDaz;
    if (dn == kDnFlush || dn == kDnSaveOperandsFlushResults) m |= kSseFz;
  }
  *cw = c;
  *mxcsr = m;
}

// _controlfp's view: it never changes the denormal exception mask.
inline constexpr uint32_t kControlFpMask = kMcwDn | kMcwEm | kMcwIc | kMcwRc | kMcwPc;
inline uint32_t ControlFpMask(uint32_t mask) { return mask & ~kEmDenormal; }

}  // namespace recomp::hle::fp
