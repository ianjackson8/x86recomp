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
// m_iCallback at +8) are registered with native proxies. libsteam_api packs
// callback structs to 4 on macOS/Linux, the guest to 8: each delivery is
// copied field by field into the guest's layout (tools/steam_callbacks.py
// gives the guest offsets from the SDK headers; the native ones are the
// compiler's). A struct left out there (pointer fields) is fatal, named.
// Callbacks only run inside SteamAPI_RunCallbacks, on the guest thread
// that called it.
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

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "runtime/hle/com.h"
#include "runtime/hle/hle.h"
#include "runtime/hle/kernel32_state.h"
#include "runtime/log.h"
#include "runtime/runtime.h"
#include "steam/steam_api.h"
// Every client interface header, as tools/steam_callbacks.py laid them out.
#define RECOMP_STEAM_HEADERS
#include "steam_callbacks.inc"
#undef RECOMP_STEAM_HEADERS

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

// A callback struct's fields: native offset and size, guest offset.
struct Field {
  uint32_t native_off, size, guest_off;
};
struct Layout {
  const char* name;
  uint32_t native_size, guest_size;
  std::vector<Field> fields;
};

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Winvalid-offsetof"
const std::unordered_map<int, Layout>& Layouts() {
  static const std::unordered_map<int, Layout> m = [] {
    std::unordered_map<int, Layout> r;
#define F(path, goff) Field{uint32_t(offsetof(S_, path)), uint32_t(sizeof(S_::path)), goff}
#define RECOMP_STEAM_CALLBACK(Name, gsize, ...)                                     \
  {                                                                                 \
    using S_ = Name;                                                                \
    r[Name::k_iCallback] = Layout{#Name, uint32_t(sizeof(Name)), gsize, {__VA_ARGS__}}; \
  }
#include "steam_callbacks.inc"
#undef RECOMP_STEAM_CALLBACK
#undef F
    return r;
  }();
  return m;
}
#pragma clang diagnostic pop

const Layout& LayoutOf(int id) {
  auto it = Layouts().find(id);
  if (it == Layouts().end())
    FatalF("steam_api: callback %d has no guest layout (not in the SDK headers, or "
           "not translatable)", id);
  return it->second;
}

// The guest context SteamAPI_RunCallbacks runs in: callbacks are delivered
// inside it.
thread_local Ctx* t_callback_ctx = nullptr;

// A native stand-in for a guest CCallbackBase.
class Proxy final : public CCallbackBase {
 public:
  Proxy(uint32_t guest, int id, bool game_server) : guest_(guest) {
    m_iCallback = id;
    m_nCallbackFlags = game_server ? kCallbackGameServer : 0;
  }
  // Callbacks: the guest's Run(void*), vtable slot 1.
  void Run(void* param) override { Deliver(param, 1, false, 0); }
  // Call results: Run(void*, bool, SteamAPICall_t), slot 0.
  void Run(void* param, bool io_failure, SteamAPICall_t call) override {
    Deliver(param, 0, io_failure, call);
  }
  int GetCallbackSizeBytes() override { return int(LayoutOf(m_iCallback).native_size); }

 private:
  void Deliver(void* param, int slot, bool io_failure, uint64_t call) {
    Ctx* c = t_callback_ctx;
    if (!c)
      FatalF("steam_api: callback %d delivered outside SteamAPI_RunCallbacks", m_iCallback);
    const Layout& l = LayoutOf(m_iCallback);
    if (!buf_) buf_ = HeapAllocProcess(l.guest_size, true);
    std::memset(GPtr(buf_), 0, l.guest_size);
    const uint8_t* src = static_cast<const uint8_t*>(param);
    for (const Field& f : l.fields) std::memcpy(GPtr(buf_ + f.guest_off), src + f.native_off, f.size);
    const gaddr fn = Rd32(Rd32(guest_) + 4 * uint32_t(slot));
    const uint32_t ecx = c->ecx;
    c->ecx = guest_;  // thiscall
    if (slot == 1) CallGuest(*c, fn, {buf_});
    else CallGuest(*c, fn, {buf_, io_failure ? 1u : 0u, call & 0xFFFFFFFFu, call >> 32});
    c->ecx = ecx;
  }
  uint32_t guest_;
  gaddr buf_ = 0;  // the struct in the guest's layout
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
  Ctx* saved = t_callback_ctx;
  t_callback_ctx = &c;
  SteamAPI_RunCallbacks();
  t_callback_ctx = saved;
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
  const uint64_t call = Arg64(c, 1, 1);
  const int id = int(Rd32(guest + 8));
  auto proxy = std::make_unique<Proxy>(guest, id, false);
  SteamAPI_RegisterCallResult(proxy.get(), call);
  std::lock_guard<std::mutex> l(g_mu);
  g_results[{guest, call}] = std::move(proxy);
  return 0;
}

HLE("steam_api.dll", SteamAPI_UnregisterCallResult, kCdecl) {
  const uint32_t guest = Arg(c, 0);
  const uint64_t call = Arg64(c, 1, 1);
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

// ---- interface methods (thiscall: this in ecx; nargs counts stack slots) -----------

namespace {
// A string the Steam client returns, copied into guest memory once (the
// client keeps such strings alive; so does this cache).
gaddr GuestString(const char* s) {
  static std::mutex mu;
  static std::unordered_map<std::string, gaddr> cache;
  if (!s) return 0;
  std::lock_guard<std::mutex> l(mu);
  gaddr& g = cache[s];
  if (!g) {
    g = HeapAllocProcess(std::strlen(s) + 1, false);
    PutStr(g, s);
  }
  return g;
}
// A CSteamID result: MSVC returns a class through a hidden buffer, the
// first stack argument, and returns that pointer.
uint64_t RetSteamID(const Ctx& c, CSteamID id) {
  Wr64(Arg(c, 0), id.ConvertToUint64());
  return Arg(c, 0);
}
}  // namespace

HLE_NAMED("steam_api.dll", "ISteamUser::GetHSteamUser", ISteamUser_GetHSteamUser, 0) {
  return uint32_t(Self<ISteamUser>(c)->GetHSteamUser());
}
HLE_NAMED("steam_api.dll", "ISteamUser::BLoggedOn", ISteamUser_BLoggedOn, 0) {
  return Self<ISteamUser>(c)->BLoggedOn() ? 1 : 0;
}
HLE_NAMED("steam_api.dll", "ISteamUser::GetSteamID", ISteamUser_GetSteamID, 1) {
  return RetSteamID(c, Self<ISteamUser>(c)->GetSteamID());
}

HLE_NAMED("steam_api.dll", "ISteamApps::BIsSubscribed", ISteamApps_BIsSubscribed, 0) {
  return Self<ISteamApps>(c)->BIsSubscribed() ? 1 : 0;
}
HLE_NAMED("steam_api.dll", "ISteamApps::BIsSubscribedApp", ISteamApps_BIsSubscribedApp, 1) {
  return Self<ISteamApps>(c)->BIsSubscribedApp(AppId_t(Arg(c, 0))) ? 1 : 0;
}
HLE_NAMED("steam_api.dll", "ISteamApps::BIsDlcInstalled", ISteamApps_BIsDlcInstalled, 1) {
  return Self<ISteamApps>(c)->BIsDlcInstalled(AppId_t(Arg(c, 0))) ? 1 : 0;
}
HLE_NAMED("steam_api.dll", "ISteamApps::GetCurrentGameLanguage",
          ISteamApps_GetCurrentGameLanguage, 0) {
  return GuestString(Self<ISteamApps>(c)->GetCurrentGameLanguage());
}
HLE_NAMED("steam_api.dll", "ISteamApps::GetAvailableGameLanguages",
          ISteamApps_GetAvailableGameLanguages, 0) {
  return GuestString(Self<ISteamApps>(c)->GetAvailableGameLanguages());
}
HLE_NAMED("steam_api.dll", "ISteamApps::GetDLCCount", ISteamApps_GetDLCCount, 0) {
  return uint32_t(Self<ISteamApps>(c)->GetDLCCount());
}
// BGetDLCDataByIndex(i, &app, &available, name, name_size)
HLE_NAMED("steam_api.dll", "ISteamApps::BGetDLCDataByIndex", ISteamApps_BGetDLCDataByIndex, 5) {
  AppId_t app = 0;
  bool available = false;
  const int size = int(Arg(c, 4));
  std::string name(size > 0 ? size_t(size) : 0, '\0');
  const bool ok = Self<ISteamApps>(c)->BGetDLCDataByIndex(int(Arg(c, 0)), &app, &available,
                                                          size > 0 ? name.data() : nullptr, size);
  if (Arg(c, 1)) Wr32(Arg(c, 1), app);
  if (Arg(c, 2)) Wr8(Arg(c, 2), available ? 1 : 0);
  if (Arg(c, 3) && size > 0) std::memcpy(GPtr(Arg(c, 3)), name.data(), size_t(size));
  return ok ? 1 : 0;
}
HLE_NAMED("steam_api.dll", "ISteamApps::GetAppBuildId", ISteamApps_GetAppBuildId, 0) {
  return uint32_t(Self<ISteamApps>(c)->GetAppBuildId());
}

// SetWarningMessageHook(fn): the client may call the hook inside any API
// call, on any thread, so the guest's (cdecl (int severity, const char*))
// is recorded, not called; a native hook prints the client's warnings,
// which is what a game's hook does with them.
namespace {
gaddr g_warning_hook = 0;
void __cdecl PrintSteamWarning(int severity, const char* text) {
  std::fprintf(stderr, "steam %s: %s", severity ? "warning" : "message", text ? text : "");
}
}  // namespace
HLE_NAMED("steam_api.dll", "ISteamClient::SetWarningMessageHook",
          ISteamClient_SetWarningMessageHook, 1) {
  g_warning_hook = Arg(c, 0);
  Self<ISteamClient>(c)->SetWarningMessageHook(&PrintSteamWarning);
  return 0;
}

HLE_NAMED("steam_api.dll", "ISteamUtils::SetOverlayNotificationPosition",
          ISteamUtils_SetOverlayNotificationPosition, 1) {
  Self<ISteamUtils>(c)->SetOverlayNotificationPosition(ENotificationPosition(Arg(c, 0)));
  return 0;
}
HLE_NAMED("steam_api.dll", "ISteamUtils::SetOverlayNotificationInset",
          ISteamUtils_SetOverlayNotificationInset, 2) {
  Self<ISteamUtils>(c)->SetOverlayNotificationInset(int(Arg(c, 0)), int(Arg(c, 1)));
  return 0;
}
HLE_NAMED("steam_api.dll", "ISteamUtils::GetSteamUILanguage", ISteamUtils_GetSteamUILanguage, 0) {
  return GuestString(Self<ISteamUtils>(c)->GetSteamUILanguage());
}
HLE_NAMED("steam_api.dll", "ISteamUtils::IsSteamInBigPictureMode",
          ISteamUtils_IsSteamInBigPictureMode, 0) {
  return Self<ISteamUtils>(c)->IsSteamInBigPictureMode() ? 1 : 0;
}
HLE_NAMED("steam_api.dll", "ISteamUtils::IsSteamRunningInVR", ISteamUtils_IsSteamRunningInVR, 0) {
  return Self<ISteamUtils>(c)->IsSteamRunningInVR() ? 1 : 0;
}

HLE_NAMED("steam_api.dll", "ISteamFriends::GetPersonaName", ISteamFriends_GetPersonaName, 0) {
  return GuestString(Self<ISteamFriends>(c)->GetPersonaName());
}
// The friends list. CSteamID results come through the hidden buffer
// (RetSteamID); a CSteamID argument takes two stack slots.
HLE_NAMED("steam_api.dll", "ISteamFriends::GetFriendCount", ISteamFriends_GetFriendCount, 1) {
  return uint32_t(Self<ISteamFriends>(c)->GetFriendCount(int(Arg(c, 0))));
}
HLE_NAMED("steam_api.dll", "ISteamFriends::GetFriendByIndex", ISteamFriends_GetFriendByIndex, 3) {
  return RetSteamID(c, Self<ISteamFriends>(c)->GetFriendByIndex(int(Arg(c, 1)), int(Arg(c, 2))));
}
HLE_NAMED("steam_api.dll", "ISteamFriends::GetFriendPersonaName", ISteamFriends_GetFriendPersonaName, 2) {
  return GuestString(Self<ISteamFriends>(c)->GetFriendPersonaName(CSteamID(uint64(Arg64(c, 0, 0)))));
}
HLE_NAMED("steam_api.dll", "ISteamFriends::GetFriendPersonaState", ISteamFriends_GetFriendPersonaState, 2) {
  return uint32_t(Self<ISteamFriends>(c)->GetFriendPersonaState(CSteamID(uint64(Arg64(c, 0, 0)))));
}
HLE_NAMED("steam_api.dll", "ISteamFriends::SetRichPresence", ISteamFriends_SetRichPresence, 2) {
  const std::string key = GStr(Arg(c, 0));
  const std::string value = Arg(c, 1) ? GStr(Arg(c, 1)) : std::string();
  return Self<ISteamFriends>(c)->SetRichPresence(key.c_str(), Arg(c, 1) ? value.c_str() : nullptr)
             ? 1 : 0;
}
HLE_NAMED("steam_api.dll", "ISteamFriends::ClearRichPresence", ISteamFriends_ClearRichPresence, 0) {
  Self<ISteamFriends>(c)->ClearRichPresence();
  return 0;
}
HLE_NAMED("steam_api.dll", "ISteamFriends::ActivateGameOverlay", ISteamFriends_ActivateGameOverlay, 1) {
  const std::string dialog = Arg(c, 0) ? GStr(Arg(c, 0)) : std::string();
  Self<ISteamFriends>(c)->ActivateGameOverlay(dialog.c_str());
  return 0;
}
HLE_NAMED("steam_api.dll", "ISteamFriends::ActivateGameOverlayToWebPage",
          ISteamFriends_ActivateGameOverlayToWebPage, 2) {
  const std::string url = GStr(Arg(c, 0));
  Self<ISteamFriends>(c)->ActivateGameOverlayToWebPage(
      url.c_str(), EActivateGameOverlayToWebPageMode(Arg(c, 1)));
  return 0;
}

HLE_NAMED("steam_api.dll", "ISteamUserStats::RequestCurrentStats",
          ISteamUserStats_RequestCurrentStats, 0) {
  return Self<ISteamUserStats>(c)->RequestCurrentStats() ? 1 : 0;
}
HLE_NAMED("steam_api.dll", "ISteamUserStats::StoreStats", ISteamUserStats_StoreStats, 0) {
  return Self<ISteamUserStats>(c)->StoreStats() ? 1 : 0;
}
HLE_NAMED("steam_api.dll", "ISteamUserStats::GetStat(const char *pchName, int32 *pData)",
          ISteamUserStats_GetStatInt, 2) {
  int32 v = 0;
  const bool ok = Self<ISteamUserStats>(c)->GetStat(GStr(Arg(c, 0)).c_str(), &v);
  if (ok && Arg(c, 1)) Wr32(Arg(c, 1), uint32_t(v));
  return ok ? 1 : 0;
}
HLE_NAMED("steam_api.dll", "ISteamUserStats::GetStat(const char *pchName, float *pData)",
          ISteamUserStats_GetStatFloat, 2) {
  float v = 0;
  const bool ok = Self<ISteamUserStats>(c)->GetStat(GStr(Arg(c, 0)).c_str(), &v);
  if (ok && Arg(c, 1)) {
    uint32_t b;
    std::memcpy(&b, &v, 4);
    Wr32(Arg(c, 1), b);
  }
  return ok ? 1 : 0;
}
HLE_NAMED("steam_api.dll", "ISteamUserStats::SetStat(const char *pchName, int32 nData)",
          ISteamUserStats_SetStatInt, 2) {
  return Self<ISteamUserStats>(c)->SetStat(GStr(Arg(c, 0)).c_str(), int32(Arg(c, 1))) ? 1 : 0;
}
HLE_NAMED("steam_api.dll", "ISteamUserStats::SetStat(const char *pchName, float fData)",
          ISteamUserStats_SetStatFloat, 2) {
  return Self<ISteamUserStats>(c)->SetStat(GStr(Arg(c, 0)).c_str(), ArgF(c, 1)) ? 1 : 0;
}
HLE_NAMED("steam_api.dll", "ISteamUserStats::GetAchievement", ISteamUserStats_GetAchievement, 2) {
  bool v = false;
  const bool ok = Self<ISteamUserStats>(c)->GetAchievement(GStr(Arg(c, 0)).c_str(), &v);
  if (ok && Arg(c, 1)) Wr8(Arg(c, 1), v ? 1 : 0);
  return ok ? 1 : 0;
}
HLE_NAMED("steam_api.dll", "ISteamUserStats::SetAchievement", ISteamUserStats_SetAchievement, 1) {
  return Self<ISteamUserStats>(c)->SetAchievement(GStr(Arg(c, 0)).c_str()) ? 1 : 0;
}
HLE_NAMED("steam_api.dll", "ISteamUserStats::IndicateAchievementProgress",
          ISteamUserStats_IndicateAchievementProgress, 3) {
  return Self<ISteamUserStats>(c)->IndicateAchievementProgress(
             GStr(Arg(c, 0)).c_str(), uint32(Arg(c, 1)), uint32(Arg(c, 2))) ? 1 : 0;
}

// Leaderboards: found (or created) asynchronously, LeaderboardFindResult_t
// through the call result; a SteamLeaderboard_t is 64-bit (two slots).
HLE_NAMED("steam_api.dll", "ISteamUserStats::FindLeaderboard", ISteamUserStats_FindLeaderboard, 1) {
  return Ret64(c, Self<ISteamUserStats>(c)->FindLeaderboard(GStr(Arg(c, 0)).c_str()));
}
HLE_NAMED("steam_api.dll", "ISteamUserStats::FindOrCreateLeaderboard",
          ISteamUserStats_FindOrCreateLeaderboard, 3) {
  return Ret64(c, Self<ISteamUserStats>(c)->FindOrCreateLeaderboard(
                      GStr(Arg(c, 0)).c_str(), ELeaderboardSortMethod(Arg(c, 1)),
                      ELeaderboardDisplayType(Arg(c, 2))));
}
HLE_NAMED("steam_api.dll", "ISteamUserStats::GetLeaderboardName", ISteamUserStats_GetLeaderboardName, 2) {
  return GuestString(Self<ISteamUserStats>(c)->GetLeaderboardName(Arg64(c, 0, 0)));
}
HLE_NAMED("steam_api.dll", "ISteamUserStats::GetLeaderboardEntryCount",
          ISteamUserStats_GetLeaderboardEntryCount, 2) {
  return uint32_t(Self<ISteamUserStats>(c)->GetLeaderboardEntryCount(Arg64(c, 0, 0)));
}

// ISteamHTTP: the game's web requests, made by the Steam client. Results
// arrive as HTTPRequestCompleted_t call results. Request handles are 32-bit.
#define RECOMP_HTTP(method, n) \
  HLE_NAMED("steam_api.dll", "ISteamHTTP::" #method, ISteamHTTP_##method, n)
RECOMP_HTTP(CreateHTTPRequest, 2) {
  return Self<ISteamHTTP>(c)->CreateHTTPRequest(EHTTPMethod(Arg(c, 0)), GStr(Arg(c, 1)).c_str());
}
RECOMP_HTTP(SetHTTPRequestContextValue, 3) {
  return Self<ISteamHTTP>(c)->SetHTTPRequestContextValue(HTTPRequestHandle(Arg(c, 0)),
                                                         Arg64(c, 1, 1)) ? 1 : 0;
}
RECOMP_HTTP(SetHTTPRequestNetworkActivityTimeout, 2) {
  return Self<ISteamHTTP>(c)->SetHTTPRequestNetworkActivityTimeout(HTTPRequestHandle(Arg(c, 0)),
                                                                   uint32(Arg(c, 1))) ? 1 : 0;
}
RECOMP_HTTP(SetHTTPRequestHeaderValue, 3) {
  return Self<ISteamHTTP>(c)->SetHTTPRequestHeaderValue(
             HTTPRequestHandle(Arg(c, 0)), GStr(Arg(c, 1)).c_str(), GStr(Arg(c, 2)).c_str()) ? 1 : 0;
}
RECOMP_HTTP(SetHTTPRequestGetOrPostParameter, 3) {
  return Self<ISteamHTTP>(c)->SetHTTPRequestGetOrPostParameter(
             HTTPRequestHandle(Arg(c, 0)), GStr(Arg(c, 1)).c_str(), GStr(Arg(c, 2)).c_str()) ? 1 : 0;
}
RECOMP_HTTP(SetHTTPRequestUserAgentInfo, 2) {
  return Self<ISteamHTTP>(c)->SetHTTPRequestUserAgentInfo(HTTPRequestHandle(Arg(c, 0)),
                                                          GStr(Arg(c, 1)).c_str()) ? 1 : 0;
}
RECOMP_HTTP(SetHTTPRequestRawPostBody, 4) {
  const uint32 n = uint32(Arg(c, 3));
  std::vector<uint8_t> body(n);
  if (n) std::memcpy(body.data(), GPtr(Arg(c, 2)), n);
  return Self<ISteamHTTP>(c)->SetHTTPRequestRawPostBody(HTTPRequestHandle(Arg(c, 0)),
                                                        GStr(Arg(c, 1)).c_str(), body.data(), n) ? 1 : 0;
}
// SendHTTPRequest(request, &call): the SteamAPICall_t for the call result.
RECOMP_HTTP(SendHTTPRequest, 2) {
  SteamAPICall_t call = 0;
  const bool ok = Self<ISteamHTTP>(c)->SendHTTPRequest(HTTPRequestHandle(Arg(c, 0)), &call);
  if (Arg(c, 1)) Wr64(Arg(c, 1), call);
  return ok ? 1 : 0;
}
RECOMP_HTTP(GetHTTPResponseBodySize, 2) {
  uint32 n = 0;
  const bool ok = Self<ISteamHTTP>(c)->GetHTTPResponseBodySize(HTTPRequestHandle(Arg(c, 0)), &n);
  if (Arg(c, 1)) Wr32(Arg(c, 1), n);
  return ok ? 1 : 0;
}
RECOMP_HTTP(GetHTTPResponseBodyData, 3) {
  const uint32 n = uint32(Arg(c, 2));
  std::vector<uint8_t> buf(n);
  const bool ok = Self<ISteamHTTP>(c)->GetHTTPResponseBodyData(HTTPRequestHandle(Arg(c, 0)),
                                                               buf.data(), n);
  if (ok && n) std::memcpy(GPtr(Arg(c, 1)), buf.data(), n);
  return ok ? 1 : 0;
}
RECOMP_HTTP(GetHTTPResponseHeaderSize, 3) {
  uint32 n = 0;
  const bool ok = Self<ISteamHTTP>(c)->GetHTTPResponseHeaderSize(HTTPRequestHandle(Arg(c, 0)),
                                                                 GStr(Arg(c, 1)).c_str(), &n);
  if (Arg(c, 2)) Wr32(Arg(c, 2), n);
  return ok ? 1 : 0;
}
RECOMP_HTTP(GetHTTPResponseHeaderValue, 4) {
  const uint32 n = uint32(Arg(c, 3));
  std::vector<uint8_t> buf(n);
  const bool ok = Self<ISteamHTTP>(c)->GetHTTPResponseHeaderValue(
      HTTPRequestHandle(Arg(c, 0)), GStr(Arg(c, 1)).c_str(), buf.data(), n);
  if (ok && n) std::memcpy(GPtr(Arg(c, 2)), buf.data(), n);
  return ok ? 1 : 0;
}
RECOMP_HTTP(ReleaseHTTPRequest, 1) {
  return Self<ISteamHTTP>(c)->ReleaseHTTPRequest(HTTPRequestHandle(Arg(c, 0))) ? 1 : 0;
}
#undef RECOMP_HTTP

// ISteamUGC: the user's Workshop subscriptions.
HLE_NAMED("steam_api.dll", "ISteamUGC::GetNumSubscribedItems", ISteamUGC_GetNumSubscribedItems, 0) {
  return Self<ISteamUGC>(c)->GetNumSubscribedItems();
}
// GetSubscribedItems(ids, max): PublishedFileId_t (64-bit) each.
HLE_NAMED("steam_api.dll", "ISteamUGC::GetSubscribedItems", ISteamUGC_GetSubscribedItems, 2) {
  const uint32 max = uint32(Arg(c, 1));
  std::vector<PublishedFileId_t> ids(max);
  const uint32 n = Self<ISteamUGC>(c)->GetSubscribedItems(ids.data(), max);
  for (uint32 i = 0; i < n && i < max; i++) Wr64(Arg(c, 0) + 8 * i, ids[i]);
  return n;
}
// RequestUGCDetails(id, max_age_seconds): SteamUGCRequestUGCDetailsResult_t
// through the call result.
HLE_NAMED("steam_api.dll", "ISteamUGC::RequestUGCDetails", ISteamUGC_RequestUGCDetails, 3) {
  return Ret64(c, Self<ISteamUGC>(c)->RequestUGCDetails(PublishedFileId_t(Arg64(c, 0, 0)),
                                                        uint32(Arg(c, 2))));
}
// GetItemInstallInfo(id, &size, folder, folder_size, &timestamp): the
// client's folder is a host path, given to the guest as its guest path (a
// mount, GameConfig::mounts); one outside every mount is fatal, named.
HLE_NAMED("steam_api.dll", "ISteamUGC::GetItemInstallInfo", ISteamUGC_GetItemInstallInfo, 6) {
  uint64 size = 0;
  uint32 stamp = 0;
  char folder[4096] = {};
  const bool ok = Self<ISteamUGC>(c)->GetItemInstallInfo(PublishedFileId_t(Arg64(c, 0, 0)), &size,
                                                         folder, sizeof folder, &stamp);
  if (Arg(c, 2)) Wr64(Arg(c, 2), size);
  if (Arg(c, 5)) Wr32(Arg(c, 5), stamp);
  const uint32 cap = uint32(Arg(c, 4));
  if (ok && Arg(c, 3) && cap) {
    const std::string g = GuestPathOf(folder);
    if (g.empty())
      FatalF("ISteamUGC::GetItemInstallInfo: Workshop folder %s is outside the game's mounts", folder);
    const std::string s = g.substr(0, cap - 1);
    std::memcpy(GPtr(Arg(c, 3)), s.c_str(), s.size() + 1);
  }
  return ok ? 1 : 0;
}
HLE_NAMED("steam_api.dll", "ISteamUGC::GetItemState", ISteamUGC_GetItemState, 2) {
  return Self<ISteamUGC>(c)->GetItemState(PublishedFileId_t(Arg64(c, 0, 0)));
}

// ISteamRemoteStorage: Steam Cloud files, read and written by the client.
#define RECOMP_RS(method, n) \
  HLE_NAMED("steam_api.dll", "ISteamRemoteStorage::" #method, ISteamRemoteStorage_##method, n)
RECOMP_RS(FileExists, 1) {
  return Self<ISteamRemoteStorage>(c)->FileExists(GStr(Arg(c, 0)).c_str()) ? 1 : 0;
}
RECOMP_RS(FilePersisted, 1) {
  return Self<ISteamRemoteStorage>(c)->FilePersisted(GStr(Arg(c, 0)).c_str()) ? 1 : 0;
}
RECOMP_RS(GetFileSize, 1) {
  return uint32_t(Self<ISteamRemoteStorage>(c)->GetFileSize(GStr(Arg(c, 0)).c_str()));
}
RECOMP_RS(GetFileTimestamp, 1) {
  return Ret64(c, uint64_t(Self<ISteamRemoteStorage>(c)->GetFileTimestamp(GStr(Arg(c, 0)).c_str())));
}
// FileRead(name, buf, size) / FileWrite(name, data, size): through a host
// buffer (guest memory is not handed to the native library).
RECOMP_RS(FileRead, 3) {
  const int32 n = int32(Arg(c, 2));
  if (n <= 0) return 0;
  std::vector<uint8_t> buf(static_cast<size_t>(n));
  const int32 got = Self<ISteamRemoteStorage>(c)->FileRead(GStr(Arg(c, 0)).c_str(), buf.data(), n);
  if (got > 0) std::memcpy(GPtr(Arg(c, 1)), buf.data(), size_t(got));
  return uint32_t(got);
}
RECOMP_RS(FileWrite, 3) {
  const int32 n = int32(Arg(c, 2));
  std::vector<uint8_t> buf(size_t(std::max(n, 0)));
  if (n > 0) std::memcpy(buf.data(), GPtr(Arg(c, 1)), size_t(n));
  return Self<ISteamRemoteStorage>(c)->FileWrite(GStr(Arg(c, 0)).c_str(), buf.data(), n) ? 1 : 0;
}
// UGCDownload(handle, priority): RemoteStorageDownloadUGCResult_t through
// the call result. UGCRead(handle, buf, size, offset, action): what was
// downloaded, through a host buffer. UGCHandle_t is 64-bit (two slots).
RECOMP_RS(UGCDownload, 3) {
  return Ret64(c, Self<ISteamRemoteStorage>(c)->UGCDownload(UGCHandle_t(Arg64(c, 0, 0)),
                                                            uint32(Arg(c, 2))));
}
RECOMP_RS(UGCRead, 6) {
  const int32 n = int32(Arg(c, 3));
  if (n <= 0) return 0;
  std::vector<uint8_t> buf(static_cast<size_t>(n));
  const int32 got = Self<ISteamRemoteStorage>(c)->UGCRead(
      UGCHandle_t(Arg64(c, 0, 0)), buf.data(), n, uint32(Arg(c, 4)), EUGCReadAction(Arg(c, 5)));
  if (got > 0) std::memcpy(GPtr(Arg(c, 2)), buf.data(), size_t(got));
  return uint32_t(got);
}
RECOMP_RS(FileForget, 1) {
  return Self<ISteamRemoteStorage>(c)->FileForget(GStr(Arg(c, 0)).c_str()) ? 1 : 0;
}
RECOMP_RS(FileDelete, 1) {
  return Self<ISteamRemoteStorage>(c)->FileDelete(GStr(Arg(c, 0)).c_str()) ? 1 : 0;
}
RECOMP_RS(GetFileCount, 0) { return uint32_t(Self<ISteamRemoteStorage>(c)->GetFileCount()); }
RECOMP_RS(GetFileNameAndSize, 2) {
  int32 size = 0;
  const char* name = Self<ISteamRemoteStorage>(c)->GetFileNameAndSize(int(Arg(c, 0)), &size);
  if (Arg(c, 1)) Wr32(Arg(c, 1), uint32_t(size));
  return GuestString(name);
}
RECOMP_RS(GetQuota, 2) {
  uint64 total = 0, avail = 0;
  const bool ok = Self<ISteamRemoteStorage>(c)->GetQuota(&total, &avail);
  if (Arg(c, 0)) Wr64(Arg(c, 0), total);
  if (Arg(c, 1)) Wr64(Arg(c, 1), avail);
  return ok ? 1 : 0;
}
RECOMP_RS(IsCloudEnabledForAccount, 0) {
  return Self<ISteamRemoteStorage>(c)->IsCloudEnabledForAccount() ? 1 : 0;
}
RECOMP_RS(IsCloudEnabledForApp, 0) {
  return Self<ISteamRemoteStorage>(c)->IsCloudEnabledForApp() ? 1 : 0;
}
RECOMP_RS(SetCloudEnabledForApp, 1) {
  Self<ISteamRemoteStorage>(c)->SetCloudEnabledForApp((Arg(c, 0) & 0xFF) != 0);
  return 0;
}
#undef RECOMP_RS

HLE_NAMED("steam_api.dll", "ISteamApps::GetEarliestPurchaseUnixTime",
          ISteamApps_GetEarliestPurchaseUnixTime, 1) {
  return Self<ISteamApps>(c)->GetEarliestPurchaseUnixTime(AppId_t(Arg(c, 0)));
}
HLE_NAMED("steam_api.dll", "ISteamUtils::GetAppID", ISteamUtils_GetAppID, 0) {
  return Self<ISteamUtils>(c)->GetAppID();
}
// The two-letter country of the user's IP, as the Steam client reports it.
HLE_NAMED("steam_api.dll", "ISteamUtils::GetIPCountry", ISteamUtils_GetIPCountry, 0) {
  return GuestString(Self<ISteamUtils>(c)->GetIPCountry());
}
HLE_NAMED("steam_api.dll", "ISteamUtils::IsOverlayEnabled", ISteamUtils_IsOverlayEnabled, 0) {
  return Self<ISteamUtils>(c)->IsOverlayEnabled() ? 1 : 0;
}

}  // namespace recomp::hle
