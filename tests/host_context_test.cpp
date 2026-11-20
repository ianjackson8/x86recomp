// platform::HostContext (guest fibers' host side): switching between
// contexts on one thread keeps thread-locals, runs deep recursion on the
// context's own stack, and lets C++ exceptions be thrown and caught inside
// a context. Portable (not iOS/tvOS).

#include <cstdio>
#include <stdexcept>

#include "runtime/platform.h"

namespace platform = recomp::platform;
static int g_fail = 0;
static thread_local int t_value = 42;
static platform::HostContext *g_main, *g_a, *g_b;
static int g_trace[8], g_n = 0;

static int Depth(int n) {
  volatile char pad[256];
  pad[0] = char(n & 1);
  return n ? Depth(n - 1) + pad[0] : 0;
}

static void FnA(void*) {
  g_trace[g_n++] = 1;
  if (t_value != 42) g_fail++, std::printf("FAIL thread_local in A\n");
  if (Depth(20000) != 10000) g_fail++, std::printf("FAIL recursion in A\n");
  try {
    throw std::runtime_error("inside");
  } catch (const std::exception&) {
    g_trace[g_n++] = 2;
  }
  platform::SwitchHostContext(g_a, g_b);  // to B, which comes back here
  g_trace[g_n++] = 4;
  platform::SwitchHostContext(g_a, g_main);
  std::printf("FAIL A resumed after its last switch\n");
  g_fail++;
  platform::SwitchHostContext(g_a, g_main);
}
static void FnB(void*) {
  g_trace[g_n++] = 3;
  t_value = 7;  // the same thread: visible to everyone
  platform::SwitchHostContext(g_b, g_a);
  std::printf("FAIL B resumed\n");
  g_fail++;
  platform::SwitchHostContext(g_b, g_main);
}

int main() {
  g_main = platform::CurrentHostContext();
  g_a = platform::NewHostContext(16 << 20, FnA, nullptr);
  g_b = platform::NewHostContext(16 << 20, FnB, nullptr);
  platform::SwitchHostContext(g_main, g_a);
  const int want[] = {1, 2, 3, 4};
  for (int i = 0; i < 4; i++)
    if (g_n != 4 || g_trace[i] != want[i]) {
      std::printf("FAIL order: step %d\n", i);
      g_fail++;
      break;
    }
  if (t_value != 7) g_fail++, std::printf("FAIL thread_local after switches\n");
  platform::FreeHostContext(g_a);
  platform::FreeHostContext(g_b);
  std::printf("host_context: %d failures\n", g_fail);
  return g_fail ? 1 : 0;
}
