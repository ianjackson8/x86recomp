// The CRT floating-point control bits (_controlfp) against the x87 CW and
// MXCSR they drive on 32-bit guests (runtime/hle/ucrt_fp.h). Expected
// values are float.h's documented constants and the Intel SDM's register
// layouts, not the implementation's tables. Portable.

#include <cstdio>
#include <cstdlib>
#include <initializer_list>

#include "runtime/hle/ucrt_fp.h"

namespace fp = recomp::hle::fp;

static int g_fail = 0;
static void Check(bool ok, const char* what, unsigned got, unsigned want) {
  if (!ok) {
    std::printf("FAIL %s: got %08X want %08X\n", what, got, want);
    g_fail++;
  }
}
#define EXPECT_EQ(got, want, what) Check((got) == (want), what, unsigned(got), unsigned(want))

int main() {
  // _CW_DEFAULT (float.h) is what the x86 CRT reports for Windows' default
  // x87 state: all exceptions masked, round to nearest, 53-bit precision.
  EXPECT_EQ(fp::FromUnits(0x027F, 0x1F80), 0x0009001Fu, "CW 027F reads as _CW_DEFAULT");
  // FNINIT state (64-bit precision).
  EXPECT_EQ(fp::FromUnits(0x037F, 0x1F80), 0x0008001Fu, "CW 037F reads as _PC_64");

  struct Case {
    const char* what;
    uint32_t ctl, mask;
    uint16_t cw0, cw1;
    uint32_t mx0, mx1;
  } cases[] = {
      // The x86 startup code: _controlfp_s(NULL, _PC_53, _MCW_PC).
      {"_PC_53 from FNINIT", fp::kPc53, fp::kMcwPc, 0x037F, 0x027F, 0x1F80, 0x1F80},
      {"_PC_24", fp::kPc24, fp::kMcwPc, 0x037F, 0x007F, 0x1F80, 0x1F80},
      {"_PC_64", fp::kPc64, fp::kMcwPc, 0x007F, 0x037F, 0x1F80, 0x1F80},
      // RC on both units: chop = 11 in CW bits 10-11 and MXCSR bits 13-14.
      {"_RC_CHOP", 0x300, fp::kMcwRc, 0x027F, 0x0E7F, 0x1F80, 0x7F80},
      {"_RC_DOWN", 0x100, fp::kMcwRc, 0x027F, 0x067F, 0x1F80, 0x3F80},
      {"_RC_UP", 0x200, fp::kMcwRc, 0x027F, 0x0A7F, 0x1F80, 0x5F80},
      // Unmask invalid: IM is CW bit 0 and MXCSR bit 7.
      {"unmask _EM_INVALID", 0, fp::kEmInvalid, 0x027F, 0x027E, 0x1F80, 0x1F00},
      {"unmask _EM_ZERODIVIDE", 0, fp::kEmZeroDivide, 0x027F, 0x027B, 0x1F80, 0x1D80},
      {"unmask _EM_INEXACT", 0, fp::kEmInexact, 0x027F, 0x025F, 0x1F80, 0x0F80},
      // Denormal modes are SSE only: DAZ bit 6, FZ bit 15.
      {"_DN_FLUSH", fp::kDnFlush, fp::kMcwDn, 0x027F, 0x027F, 0x1F80, 0x9FC0},
      {"_DN_FLUSH_OPERANDS", fp::kDnFlushOperandsSaveResults, fp::kMcwDn, 0x027F, 0x027F,
       0x1F80, 0x1FC0},
      {"_DN_FLUSH_RESULTS", fp::kDnSaveOperandsFlushResults, fp::kMcwDn, 0x027F, 0x027F,
       0x1F80, 0x9F80},
      {"_DN_SAVE", fp::kDnSave, fp::kMcwDn, 0x027F, 0x027F, 0x9FC0, 0x1F80},
      // Infinity control: CW bit 12.
      {"_IC_AFFINE", fp::kIcAffine, fp::kMcwIc, 0x027F, 0x127F, 0x1F80, 0x1F80},
      // _controlfp never touches the denormal exception mask (DM: CW bit 1,
      // MXCSR bit 8) even when _MCW_EM asks to unmask everything.
      {"_MCW_EM via _controlfp keeps DM", 0, fp::ControlFpMask(fp::kMcwEm), 0x027F, 0x0242,
       0x1F80, 0x0100},
      // Bits outside the fields stay (CW bits 6-7 and 13-15; MXCSR flags).
      {"reserved CW bits kept", fp::kPc53, fp::kMcwPc, 0xE3FF, 0xE2FF, 0x1FBF, 0x1FBF},
  };
  for (const Case& t : cases) {
    uint16_t cw = t.cw0;
    uint32_t mx = t.mx0;
    fp::ToUnits(t.ctl, t.mask, &cw, &mx);
    char what[96];
    std::snprintf(what, sizeof what, "%s: cw", t.what);
    EXPECT_EQ(cw, t.cw1, what);
    std::snprintf(what, sizeof what, "%s: mxcsr", t.what);
    EXPECT_EQ(mx, t.mx1, what);
  }

  // Round trip: the CRT view of any valid state, written back with every
  // field in the mask, reproduces that state.
  int n = 0;
  for (uint32_t ems = 0; ems < 64; ems++)
    for (uint32_t rc = 0; rc < 4; rc++)
      for (uint32_t pc : {0u, 2u, 3u})
        for (uint32_t ic = 0; ic < 2; ic++)
          for (uint32_t dn = 0; dn < 4; dn++) {
            uint16_t cw = uint16_t(0x0040 | ems | (pc << 8) | (rc << 10) | (ic << 12));
            uint32_t mx = 0x1F80 & ~0x6000u;
            for (const fp::EmBit& e : fp::kEm)
              if (!(cw & e.x87)) mx &= ~e.sse;
            mx |= rc << 13;
            if (dn & 1) mx |= fp::kSseDaz;
            if (dn & 2) mx |= fp::kSseFz;
            uint16_t cw2 = 0x0040;
            uint32_t mx2 = 0;
            fp::ToUnits(fp::FromUnits(cw, mx), fp::kControlFpMask, &cw2, &mx2);
            if (cw2 != cw || mx2 != mx) {
              std::printf("FAIL round trip cw %04X mx %08X -> %04X %08X\n", cw, mx, cw2, mx2);
              g_fail++;
            }
            n++;
          }
  std::printf("ucrt_fp: %zu cases, %d round trips, %d failures\n",
              sizeof cases / sizeof cases[0], n, g_fail);
  return g_fail ? 1 : 0;
}
