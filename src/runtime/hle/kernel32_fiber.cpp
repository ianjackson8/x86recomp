// kernel32: fibers (32-bit guests). Lifted code runs a guest function as a
// host function, so a fiber is a guest stack and a host stack, switched
// together on the same host thread (platform::SwitchHostContext): the
// thread's identity, TLS and per-thread runtime state stay, as on Windows.
//
// A fiber is a guest block whose first field is its parameter (what
// GetFiberData reads through the TEB's FiberData, fs:[0x10]). SwitchToFiber
// saves and restores what Windows keeps per fiber: the integer registers
// (eflags with them), the TEB's ExceptionList, StackBase and StackLimit,
// and FiberData. The x87/SSE state stays the thread's, as for fibers made
// without FIBER_FLAG_FLOAT_SWITCH. A start routine that returns (Windows
// would exit the thread) and deleting the running fiber are fatal, named.

#include <map>
#include <memory>
#include <mutex>

#include "runtime/hle/hle.h"
#include "runtime/hle/kernel32_state.h"
#include "runtime/log.h"
#include "runtime/platform.h"
#include "runtime/runtime.h"

namespace recomp::hle {
namespace {

constexpr uint32_t kErrorAlreadyFiber = 1280;
constexpr uint64_t kFiberHostStack = 64ull << 20;  // reserved, not committed
constexpr uint32_t kTebExceptionList = 0x0, kTebStackBase = 0x4, kTebStackLimit = 0x8,
                   kTebFiberData = 0x10;

// What a fiber keeps of the guest's registers while another runs.
struct Regs {
  uint64_t r[16];
  uint32_t eflags, flags_valid;
};
void SaveRegs(const Ctx& c, Regs* s) {
  const uint64_t* src[16] = {&c.rax, &c.rcx, &c.rdx, &c.rbx, &c.rsp, &c.rbp, &c.rsi, &c.rdi,
                             &c.r8,  &c.r9,  &c.r10, &c.r11, &c.r12, &c.r13, &c.r14, &c.r15};
  for (int i = 0; i < 16; i++) s->r[i] = *src[i];
  s->eflags = c.eflags;
  s->flags_valid = c.flags_valid;
}
void LoadRegs(Ctx& c, const Regs& s) {
  uint64_t* dst[16] = {&c.rax, &c.rcx, &c.rdx, &c.rbx, &c.rsp, &c.rbp, &c.rsi, &c.rdi,
                       &c.r8,  &c.r9,  &c.r10, &c.r11, &c.r12, &c.r13, &c.r14, &c.r15};
  for (int i = 0; i < 16; i++) *dst[i] = s.r[i];
  c.eflags = s.eflags;
  c.flags_valid = s.flags_valid;
}

struct Fiber {
  gaddr block = 0;             // the guest fiber: +0 its parameter
  gaddr start = 0;             // CreateFiber's start routine (0: a converted thread)
  gaddr stack_lo = 0, stack_hi = 0;
  bool owns_stack = false;
  platform::HostContext* host = nullptr;
  Regs regs{};
  uint32_t exception_list = 0xFFFFFFFF;
  Ctx* ctx = nullptr;          // the thread context it runs in
};

std::mutex g_mu;
std::map<gaddr, std::unique_ptr<Fiber>> g_fibers;  // by guest block
thread_local Fiber* t_current = nullptr;

Fiber* Find(gaddr block) {
  std::lock_guard<std::mutex> l(g_mu);
  auto it = g_fibers.find(block);
  return it == g_fibers.end() ? nullptr : it->second.get();
}

gaddr NewBlock(gaddr param) {
  gaddr b = HeapAllocProcess(16, true);
  Wr32(b, uint32_t(param));
  return b;
}

// A created fiber's first run, on its own host stack.
void FiberEntry(void* arg) {
  Fiber* f = static_cast<Fiber*>(arg);
  Ctx& c = *f->ctx;
  CallGuest(c, f->start, {Rd32(f->block)});
  FatalF("fiber %08X: its start routine returned (Windows would exit the thread)", f->block);
}

}  // namespace

// ConvertThreadToFiber(param): the calling thread becomes a fiber.
HLE("kernel32.dll", ConvertThreadToFiber, 1) {
  if (t_current) {
    SetLastError(kErrorAlreadyFiber);
    return 0;
  }
  Thread* t = CurrentThread();
  auto f = std::make_unique<Fiber>();
  f->block = NewBlock(Arg(c, 0));
  f->stack_lo = t->stack_lo;
  f->stack_hi = t->stack_hi;
  f->host = platform::CurrentHostContext();
  f->ctx = &c;
  t_current = f.get();
  Wr32(t->teb + kTebFiberData, f->block);
  const gaddr block = f->block;
  std::lock_guard<std::mutex> l(g_mu);
  g_fibers[block] = std::move(f);
  return block;
}

// CreateFiber(stack_size, start, param): 0 is the default 1 MiB.
HLE("kernel32.dll", CreateFiber, 3) {
  uint64_t size = Arg(c, 0) ? uint64_t(Arg(c, 0)) : 0x100000;
  size = (size + 0xFFFF) & ~uint64_t(0xFFFF);
  Process& p = *g_proc;
  const gaddr stack = p.vm.Reserve(0, size);
  if (!stack) {
    SetLastError(8);  // ERROR_NOT_ENOUGH_MEMORY
    return 0;
  }
  p.vm.Commit(stack, size, Prot::RW);
  auto f = std::make_unique<Fiber>();
  f->block = NewBlock(Arg(c, 2));
  f->start = Arg(c, 1);
  f->stack_lo = stack;
  f->stack_hi = stack + size;
  f->owns_stack = true;
  f->host = platform::NewHostContext(kFiberHostStack, FiberEntry, f.get());
  const gaddr block = f->block;
  std::lock_guard<std::mutex> l(g_mu);
  g_fibers[block] = std::move(f);
  return block;
}

HLE("kernel32.dll", SwitchToFiber, 1) {
  Fiber* to = Find(Arg(c, 0));
  Fiber* from = t_current;
  if (!to) FatalF("SwitchToFiber(%08X): not a fiber", uint32_t(Arg(c, 0)));
  if (!from) FatalF("SwitchToFiber from a thread that is not a fiber");
  if (to == from) return 0;
  Thread* t = CurrentThread();
  // This fiber's state; it resumes here, inside this handler.
  SaveRegs(c, &from->regs);
  from->exception_list = Rd32(t->teb + kTebExceptionList);
  from->ctx = &c;
  // The next one's.
  t_current = to;
  Wr32(t->teb + kTebFiberData, to->block);
  Wr32(t->teb + kTebStackBase, uint32_t(to->stack_hi));
  Wr32(t->teb + kTebStackLimit, uint32_t(to->stack_lo));
  Wr32(t->teb + kTebExceptionList, to->exception_list);
  t->stack_lo = to->stack_lo;
  t->stack_hi = to->stack_hi;
  if (to->start && !to->ctx) {  // its first run: a fresh stack, in this thread's context
    to->ctx = &c;
    c.esp = uint32_t(to->stack_hi - 16);
    c.ebp = 0;
  } else {
    LoadRegs(c, to->regs);
  }
  platform::SwitchHostContext(from->host, to->host);
  // Back: whoever switched here loaded this fiber's registers and TEB fields.
  return uint32_t(c.eax);
}

// DeleteFiber(fiber): not the running one (Windows would exit the thread).
HLE("kernel32.dll", DeleteFiber, 1) {
  std::unique_ptr<Fiber> f;
  {
    std::lock_guard<std::mutex> l(g_mu);
    auto it = g_fibers.find(Arg(c, 0));
    if (it == g_fibers.end()) return 0;
    if (it->second.get() == t_current)
      FatalF("DeleteFiber on the running fiber (Windows would exit the thread)");
    f = std::move(it->second);
    g_fibers.erase(it);
  }
  if (f->owns_stack) g_proc->vm.Release(f->stack_lo);
  if (f->host) platform::FreeHostContext(f->host);
  HeapFreeProcess(f->block);
  return 0;
}

}  // namespace recomp::hle
