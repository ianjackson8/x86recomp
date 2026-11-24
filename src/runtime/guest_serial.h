// <ID>_SERIAL=1: guest threads run one at a time, a diagnostic for races
// the lifted code may have on a weakly ordered host (ARCHITECTURE.md,
// "Memory ordering"). A thread holds one fair (ticket) lock while it runs
// guest code and gives it up for every import call (CallThunk), taking it
// back on return: blocking calls and spin loops that call the runtime let
// the others run. A spin loop of plain guest instructions, waiting on a
// thread that cannot get the lock, would deadlock; the watchdog then
// shows it. Off by default; with it off, nothing here costs more than a
// flag test.
#pragma once

namespace recomp::serial {

// Whether the mode is on (read once, on first use).
bool On();

// For CallGuest: holds the lock for the guest code run inside, unless the
// thread holds it already.
class Run {
 public:
  Run();
  ~Run();
  Run(const Run&) = delete;
  Run& operator=(const Run&) = delete;

 private:
  bool took_ = false;
};

// For an import call: gives the lock up while the handler runs, if held.
class Yield {
 public:
  Yield();
  ~Yield();
  Yield(const Yield&) = delete;
  Yield& operator=(const Yield&) = delete;

 private:
  bool gave_ = false;
};

}  // namespace recomp::serial
