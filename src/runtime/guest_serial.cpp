#include "runtime/guest_serial.h"

#include <atomic>
#include <cstdint>

#include "runtime/log.h"
#include "runtime/runtime.h"

namespace recomp::serial {
namespace {

std::atomic<uint32_t> g_next{0}, g_serving{0};
thread_local bool t_holding = false;

void Acquire() {
  const uint32_t me = g_next.fetch_add(1, std::memory_order_relaxed);
  for (uint32_t s = g_serving.load(std::memory_order_acquire); s != me;
       s = g_serving.load(std::memory_order_acquire))
    g_serving.wait(s, std::memory_order_acquire);
  t_holding = true;
}

void Release() {
  t_holding = false;
  g_serving.fetch_add(1, std::memory_order_release);
  g_serving.notify_all();
}

}  // namespace

bool On() {
  static const bool on = [] {
    const char* e = Env("SERIAL");
    const bool v = e && *e == '1';
    if (v) WarnF("guest threads run one at a time (SERIAL diagnostic mode)\n");
    return v;
  }();
  return on;
}

Run::Run() {
  if (!On() || t_holding) return;
  Acquire();
  took_ = true;
}
Run::~Run() {
  if (took_) Release();
}

Yield::Yield() {
  if (!On() || !t_holding) return;
  Release();
  gave_ = true;
}
Yield::~Yield() {
  if (gave_) Acquire();
}

}  // namespace recomp::serial
