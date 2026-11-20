// steam_api.dll bridged to Valve's native libsteam_api (recomp_steam), so
// that the real Steam client answers: whether the user owns the app and its
// DLC, the user's identity, stats, workshop. Nothing is answered here in the
// client's place.
//
// Interfaces: the guest gets a COM-style object (com.h) per native interface
// pointer, whose vtable is the version's method list in MSVC order,
// generated from the SDK headers the build was configured with
// (tools/steam_vtables.py); the SDK must declare the game's interface
// versions. Methods are thiscall (this in ecx, arguments on the stack,
// callee pops); each is a handler named "Interface::Method", so one not
// written yet fails by name.
//
// Callbacks: the guest's CCallbackBase objects (MSVC vtable: Run(void*,
// bool, SteamAPICall_t), Run(void*), GetCallbackSizeBytes; flags byte at +4,
// m_iCallback at +8) are registered with native proxies. Delivering one
// needs the callback struct moved from the native layout (pack 4 on
// macOS/Linux) to the guest's (pack 8): not written yet, so a delivery is
// fatal and names the callback.
//
// Policy, as Windows would behave for a game Steam did not launch:
// - SteamAPI_RestartAppIfNecessary: false. It relaunches a game through
//   Steam; a native port cannot be launched that way. The app id is given
//   to libsteam_api through SteamAppId (its documented way to run outside a
//   Steam launch), from GameConfig::steam_app_id.
// - SteamAPI_Init also asks the client whether the user owns the app
//   (BIsSubscribedApp) and fails if not.
// - SteamAPI_WriteMiniDump: nothing (a Windows minidump of a recompiled
//   process means nothing to Steam's crash reporting).

#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "runtime/hle/com.h"
#include "runtime/hle/hle.h"
#include "runtime/log.h"
#include "runtime/runtime.h"
#include "steam/steam_api.h"

namespace recomp::hle {
namespace {

constexpr const char* kDll = "steam_api.dll";

// The interfaces the SDK headers declare, with their MSVC method order.
struct Iface {
  const char* cls;
  const char* version;
  std::vector<const char*> methods;
};
#define RECOMP_STEAM_IFACE(cls, ver, ...) {#cls, ver, {__VA_ARGS__}},
const Iface kIfaces[] = {
#include "steam_vtables.inc"
};
#undef RECOMP_STEAM_IFACE

struct SteamObject final : com::Tagged<com::TagOf("steam.Interface")> {
  void* native = nullptr;
  const Iface* iface = nullptr;
};

std::mutex g_mu;
std::unordered_map<void*, uint32_t> g_wrapped;  // native interface -> guest object
uint32_t g_init_counter = 0;                     // SteamInternal_ContextInit's counter

uint32_t AppId() {
  const uint32_t id = Game().steam_app_id;
  if (!id) FatalF("steam_api: GameConfig::steam_app_id is not set");
  return id;
}

// Names the app to libsteam_api when Steam did not launch the process.
void SetAppEnv() {
  const std::string id = std::to_string(AppId());
  setenv("SteamAppId", id.c_str(), 1);
  setenv("SteamGameId", id.c_str(), 1);
}

// The guest object for a native interface pointer of `version`.
uint32_t Wrap(void* native, const std::string& version) {
  if (!native) return 0;
  std::lock_guard<std::mutex> l(g_mu);
  auto it = g_wrapped.find(native);
  if (it != g_wrapped.end()) return it->second;
  const Iface* found = nullptr;
  for (const Iface& f : kIfaces)
    if (version == f.version) found = &f;
  if (!found)
    FatalF("steam_api: interface %s is not bridged (this build's SDK headers do not "
           "declare it, or it is not in tools/steam_vtables.py)", version.c_str());
  auto obj = std::make_shared<SteamObject>();
  obj->native = native;
  obj->iface = found;
  uint32_t guest = com::Publish(obj, com::Vtable(kDll, found->cls, found->methods));
  g_wrapped[native] = guest;
  return guest;
}

// The native interface behind the guest's this (ecx), as T.
template <typename T>
T* Self(const Ctx& c) {
  SteamObject* o = com::As<SteamObject>(c.ecx);
  if (!o) FatalF("steam_api: a Steam method called on %08X, not a bridged interface", c.ecx);
  return static_cast<T*>(o->native);
}

// ---- callbacks ---------------------------------------------------------------

constexpr uint8_t kCallbackRegistered = 0x01, kCallbackGameServer = 0x02;

// A native stand-in for a guest CCallbackBase.
class Proxy final : public CCallbackBase {
 public:
  Proxy(uint32_t guest, int id, bool game_server) : guest_(guest) {
    m_iCallback = id;
    m_nCallbackFlags = game_server ? kCallbackGameServer : 0;
  }
  void Run(void*) override { Deliver(); }
  void Run(void*, bool, SteamAPICall_t) override { Deliver(); }
  int GetCallbackSizeBytes() override {
    // The native size of the struct, which libsteam_api checks; it is
    // known once the layout translation exists.
    FatalF("steam_api: callback %d (guest %08X): size and layout translation not written yet",
           m_iCallback, guest_);
  }
  uint32_t guest() const { return guest_; }

 private:
  [[noreturn]] void Deliver() {
    FatalF("steam_api: callback %d delivered to guest %08X: layout translation not written yet",
           m_iCallback, guest_);
  }
  uint32_t guest_;
};

std::map<uint32_t, std::unique_ptr<Proxy>> g_callbacks;            // by guest object
std::map<std::pair<uint32_t, uint64_t>, std::unique_ptr<Proxy>> g_results;

// The guest object's m_nCallbackFlags (+4) and m_iCallback (+8), which
// libsteam_api maintains and the guest's destructor reads.
void SetGuestRegistered(uint32_t guest, bool on, int id) {
  uint8_t f = Rd8(guest + 4);
  Wr8(guest + 4, uint8_t(on ? f | kCallbackRegistered : f & ~kCallbackRegistered));
  if (on) Wr32(guest + 8, uint32_t(id));
}

uint64_t Arg64(const Ctx& c, int slot) {
  return (Arg(c, slot) & 0xFFFFFFFFu) | (Arg(c, slot + 1) << 32);
}

}  // namespace

// ---- the flat API (cdecl) ----------------------------------------------------------

HLE("steam_api.dll", SteamAPI_RestartAppIfNecessary, kCdecl) {
  const uint32_t id = uint32_t(Arg(c, 0));
  if (id != AppId())
    FatalF("steam_api: the game says it is app %u, GameConfig says %u", id, AppId());
  SetAppEnv();
  return 0;
}

HLE("steam_api.dll", SteamAPI_Init, kCdecl) {
  SetAppEnv();
  if (!SteamAPI_Init()) {
    std::fprintf(stderr, "steam_api: SteamAPI_Init failed (is Steam running and logged in?)\n");
    return 0;
  }
  if (!SteamApps() || !SteamApps()->BIsSubscribedApp(AppId())) {
    std::fprintf(stderr, "steam_api: the Steam client says this account does not own app %u\n",
                 AppId());
    SteamAPI_Shutdown();
    return 0;
  }
  std::lock_guard<std::mutex> l(g_mu);
  g_init_counter++;
  return 1;
}

HLE("steam_api.dll", SteamAPI_RunCallbacks, kCdecl) {
  SteamAPI_RunCallbacks();
  return 0;
}

HLE("steam_api.dll", SteamAPI_GetHSteamUser, kCdecl) { return uint32_t(SteamAPI_GetHSteamUser()); }

// SteamInternal_ContextInit(p): p is {void (*init)(void* ctx); uintptr_t
// counter; void* ctx} in guest memory. When counter is stale, init(&ctx)
// runs (it stores the interface pointer there); returns &ctx.
HLE("steam_api.dll", SteamInternal_ContextInit, kCdecl) {
  const gaddr p = Arg(c, 0);
  uint32_t counter;
  {
    std::lock_guard<std::mutex> l(g_mu);
    counter = g_init_counter;
  }
  if (Rd32(p + 4) != counter) {
    Wr32(p + 4, counter);
    CallGuest(c, Rd32(p), {p + 8});
  }
  return p + 8;
}

HLE("steam_api.dll", SteamInternal_FindOrCreateUserInterface, kCdecl) {
  const std::string version = GStr(Arg(c, 1));
  return Wrap(SteamInternal_FindOrCreateUserInterface(HSteamUser(Arg(c, 0)), version.c_str()),
              version);
}

HLE("steam_api.dll", SteamInternal_CreateInterface, kCdecl) {
  const std::string version = GStr(Arg(c, 0));
  return Wrap(SteamInternal_CreateInterface(version.c_str()), version);
}

HLE("steam_api.dll", SteamAPI_RegisterCallback, kCdecl) {
  const uint32_t guest = Arg(c, 0);
  const int id = int(Arg(c, 1));
  const bool gs = Rd8(guest + 4) & kCallbackGameServer;
  auto proxy = std::make_unique<Proxy>(guest, id, gs);
  SteamAPI_RegisterCallback(proxy.get(), id);
  SetGuestRegistered(guest, true, id);
  std::lock_guard<std::mutex> l(g_mu);
  g_callbacks[guest] = std::move(proxy);
  return 0;
}

HLE("steam_api.dll", SteamAPI_UnregisterCallback, kCdecl) {
  const uint32_t guest = Arg(c, 0);
  std::unique_ptr<Proxy> proxy;
  {
    std::lock_guard<std::mutex> l(g_mu);
    auto it = g_callbacks.find(guest);
    if (it == g_callbacks.end()) return 0;
    proxy = std::move(it->second);
    g_callbacks.erase(it);
  }
  SteamAPI_UnregisterCallback(proxy.get());
  SetGuestRegistered(guest, false, 0);
  return 0;
}

// (pCallback, SteamAPICall_t): the call handle takes two stack slots.
HLE("steam_api.dll", SteamAPI_RegisterCallResult, kCdecl) {
  const uint32_t guest = Arg(c, 0);
  const uint64_t call = Arg64(c, 1);
  const int id = int(Rd32(guest + 8));
  auto proxy = std::make_unique<Proxy>(guest, id, false);
  SteamAPI_RegisterCallResult(proxy.get(), call);
  std::lock_guard<std::mutex> l(g_mu);
  g_results[{guest, call}] = std::move(proxy);
  return 0;
}

HLE("steam_api.dll", SteamAPI_UnregisterCallResult, kCdecl) {
  const uint32_t guest = Arg(c, 0);
  const uint64_t call = Arg64(c, 1);
  std::unique_ptr<Proxy> proxy;
  {
    std::lock_guard<std::mutex> l(g_mu);
    auto it = g_results.find({guest, call});
    if (it == g_results.end()) return 0;
    proxy = std::move(it->second);
    g_results.erase(it);
  }
  SteamAPI_UnregisterCallResult(proxy.get(), call);
  return 0;
}

HLE("steam_api.dll", SteamAPI_WriteMiniDump, kCdecl) { return 0; }
HLE("steam_api.dll", SteamAPI_SetMiniDumpComment, kCdecl) { return 0; }

}  // namespace recomp::hle
