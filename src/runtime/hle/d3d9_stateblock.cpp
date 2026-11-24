// IDirect3DStateBlock9: CreateStateBlock(D3DSBT_ALL), Capture, Apply.
// A UI layer drawn over a game (a SWF player, for one) saves the
// application's device state this way and puts it back afterwards.
//
// D3DSBT_ALL holds what the device state is (render targets are not part
// of it): render states, sampler states, texture stage states, textures,
// stream sources and frequencies, indices, the vertex declaration or FVF,
// both shaders and their float/int/bool constants, transforms, user clip
// planes, viewport and scissor rectangle. Bound objects are referenced
// while captured (Bind), as in D3D9. Apply goes through the device's own
// setters, so what changed is marked for gfx as after the game's calls.
// Recorded blocks (Begin/EndStateBlock) hold what was set in between
// (StateMask). The pixel- and vertex-only kinds are not modelled yet:
// fatal, named.

#include <cstring>
#include <map>
#include <memory>

#include "runtime/hle/com.h"
#include "runtime/hle/d3d9_impl.h"
#include "runtime/hle/hle.h"
#include "runtime/log.h"

namespace recomp::hle {
namespace {

using namespace d3d9;

constexpr uint32_t kSbtAll = 1;

struct StateBlock final : com::Tagged<com::TagOf("d3d9.StateBlock")> {
  uint32_t device = 0;
  uint32_t rs[256] = {};
  uint32_t ss[20][14] = {};
  uint32_t tss[8][33] = {};
  Slot<Texture> textures[20];
  struct Stream {
    Slot<Buffer> buffer;
    uint32_t offset = 0, stride = 0, freq = 1;
  } streams[16];
  Slot<Buffer> indices;
  Slot<VertexDecl> decl;
  uint32_t fvf = 0;
  Slot<Shader> vs, ps;
  float vs_f[256][4] = {}, ps_f[224][4] = {};
  int32_t vs_i[16][4] = {}, ps_i[16][4] = {};
  guest::BOOL vs_b[16] = {}, ps_b[16] = {};
  std::map<uint32_t, std::array<float, 16>> transforms;
  float clip_planes[6][4] = {};
  guest::D3DVIEWPORT9 viewport{};
  guest::D3DRECT scissor{};

  ~StateBlock() override {
    for (auto& t : textures) Bind(t, 0);
    for (auto& s : streams) Bind(s.buffer, 0);
    Bind(indices, 0);
    Bind(decl, 0);
    Bind(vs, 0);
    Bind(ps, 0);
  }

  // What the block holds (all of it for D3DSBT_ALL; a recorded block what
  // was set while recording). Capture and Apply touch only that.
  StateMask mask;

  void Capture(const Device& d) {
    for (size_t i = 0; i < 256; i++)
      if (mask.rs[i]) rs[i] = d.rs.v[i];
    for (size_t s = 0; s < 20; s++)
      for (size_t t = 0; t < 14; t++)
        if (mask.ss[s * 14 + t]) ss[s][t] = d.ss[s][t];
    for (size_t s = 0; s < 8; s++)
      for (size_t t = 0; t < 33; t++)
        if (mask.tss[s * 33 + t]) tss[s][t] = d.tss[s][t];
    for (int i = 0; i < 20; i++)
      if (mask.textures[i]) Bind(textures[i], d.textures[i].guest);
    for (int i = 0; i < 16; i++) {
      if (mask.streams[i]) {
        Bind(streams[i].buffer, d.streams[i].buffer.guest);
        streams[i].offset = d.streams[i].offset;
        streams[i].stride = d.streams[i].stride;
      }
      if (mask.stream_freq[i]) streams[i].freq = d.streams[i].freq;
    }
    if (mask.indices) Bind(indices, d.indices.guest);
    if (mask.vdecl) {
      Bind(decl, d.decl.guest);
      fvf = d.fvf;
    }
    if (mask.vs) Bind(vs, d.vs.guest);
    if (mask.ps) Bind(ps, d.ps.guest);
    auto regs = [](auto& dst, const auto& src, const auto& bits) {
      for (size_t i = 0; i < bits.size(); i++)
        if (bits[i]) std::memcpy(&dst[i], &src[i], sizeof dst[i]);
    };
    regs(vs_f, d.vs_f, mask.vs_f);
    regs(ps_f, d.ps_f, mask.ps_f);
    regs(vs_i, d.vs_i, mask.vs_i);
    regs(ps_i, d.ps_i, mask.ps_i);
    regs(vs_b, d.vs_b, mask.vs_b);
    regs(ps_b, d.ps_b, mask.ps_b);
    if (mask.all_transforms) {
      transforms = d.transforms;
    } else {
      for (uint32_t t : mask.transforms) {
        auto it = d.transforms.find(t);
        std::array<float, 16> m{};
        if (it != d.transforms.end()) m = it->second;
        else m[0] = m[5] = m[10] = m[15] = 1.0f;  // never set: identity
        transforms[t] = m;
      }
    }
    for (int i = 0; i < 6; i++)
      if (mask.clip_planes[i]) std::memcpy(clip_planes[i], d.clip_planes[i], sizeof clip_planes[i]);
    if (mask.viewport) viewport = d.viewport;
    if (mask.scissor) scissor = d.scissor;
  }

  void Apply(Device& d) const {
    for (uint32_t i = 0; i < 256; i++)
      if (mask.rs[i]) d.rs.Set(i, rs[i]);
    for (uint32_t s = 0; s < 20; s++)
      for (uint32_t t = 0; t < 14; t++)
        if (mask.ss[s * 14 + t]) d.SetSamplerState(s, t, ss[s][t]);
    for (size_t s = 0; s < 8; s++)
      for (size_t t = 0; t < 33; t++)
        if (mask.tss[s * 33 + t]) d.tss[s][t] = tss[s][t];
    for (int i = 0; i < 20; i++)
      if (mask.textures[i]) Bind(d.textures[i], textures[i].guest);
    for (int i = 0; i < 16; i++) {
      if (mask.streams[i]) {
        Bind(d.streams[i].buffer, streams[i].buffer.guest);
        d.streams[i].offset = streams[i].offset;
        d.streams[i].stride = streams[i].stride;
      }
      if (mask.stream_freq[i]) d.streams[i].freq = streams[i].freq;
    }
    if (mask.indices) Bind(d.indices, indices.guest);
    // A declaration or an FVF: SetFVF clears the declaration and vice versa.
    if (mask.vdecl) {
      Bind(d.decl, decl.guest);
      d.fvf = fvf;
    }
    if (mask.vs) Bind(d.vs, vs.guest);
    if (mask.ps) Bind(d.ps, ps.guest);
    auto regs = [](auto& dst, const auto& src, const auto& bits) {
      bool any = false;
      for (size_t i = 0; i < bits.size(); i++)
        if (bits[i]) {
          std::memcpy(&dst[i], &src[i], sizeof dst[i]);
          any = true;
        }
      return any;
    };
    if (regs(d.vs_f, vs_f, mask.vs_f)) d.vs_f_version++;
    if (regs(d.ps_f, ps_f, mask.ps_f)) d.ps_f_version++;
    if (regs(d.vs_i, vs_i, mask.vs_i) | regs(d.vs_b, vs_b, mask.vs_b)) d.vs_ib_version++;
    if (regs(d.ps_i, ps_i, mask.ps_i) | regs(d.ps_b, ps_b, mask.ps_b)) d.ps_ib_version++;
    if (mask.all_transforms) {
      d.transforms = transforms;
    } else {
      for (uint32_t t : mask.transforms) d.transforms[t] = transforms.at(t);
    }
    for (uint32_t i = 0; i < 6; i++)
      if (mask.clip_planes[i]) {
        std::memcpy(d.clip_planes[i], clip_planes[i], sizeof clip_planes[i]);
        d.gfx->SetClipPlane(i, d.clip_planes[i]);
      }
    if (mask.viewport) d.viewport = viewport;
    if (mask.scissor) d.scissor = scissor;
  }
};

uint32_t StateBlockVtable() {
  return com::Vtable("d3d9.dll", "IDirect3DStateBlock9",
                     {"QueryInterface", "AddRef", "Release", "GetDevice", "Capture", "Apply"});
}

StateBlock* Self(Ctx& c) {
  StateBlock* b = com::As<StateBlock>(Arg(c, 0));
  if (!b) FatalF("IDirect3DStateBlock9 call on unknown object %08X", uint32_t(Arg(c, 0)));
  return b;
}

}  // namespace

// CreateStateBlock(type, &block)
HLE_NAMED("d3d9.dll", "IDirect3DDevice9::CreateStateBlock", D3DDev9_CreateStateBlock, 3) {
  const uint32_t type = Arg(c, 1), out = Arg(c, 2);
  if (type != kSbtAll) FatalF("CreateStateBlock: type %u (pixel/vertex only) not supported yet", type);
  if (!out) return D3DERR_INVALIDCALL;
  auto b = std::make_shared<StateBlock>();
  b->device = Arg(c, 0);
  b->mask.SetAll();
  b->Capture(*DeviceOf(b->device));
  Wr32(out, com::Publish(b, StateBlockVtable()));
  return D3D_OK;
}
// BeginStateBlock / EndStateBlock(&block): the block holds what the
// setters in between set, with the values they set. In D3D9 those calls
// do not change the device's state: here they do (they go through the
// same setters), and EndStateBlock puts back the state saved at Begin.
HLE_NAMED("d3d9.dll", "IDirect3DDevice9::BeginStateBlock", D3DDev9_BeginStateBlock, 1) {
  Device* d = DeviceOf(Arg(c, 0));
  if (d->recording) return D3DERR_INVALIDCALL;
  auto saved = std::make_shared<StateBlock>();
  saved->device = Arg(c, 0);
  saved->mask.SetAll();
  saved->Capture(*d);
  d->before_recording = saved;
  d->recording = std::make_unique<StateMask>();
  return D3D_OK;
}
HLE_NAMED("d3d9.dll", "IDirect3DDevice9::EndStateBlock", D3DDev9_EndStateBlock, 2) {
  Device* d = DeviceOf(Arg(c, 0));
  const uint32_t out = Arg(c, 1);
  if (!d->recording || !out) return D3DERR_INVALIDCALL;
  auto b = std::make_shared<StateBlock>();
  b->device = Arg(c, 0);
  b->mask = *d->recording;
  d->recording.reset();
  b->Capture(*d);
  std::static_pointer_cast<StateBlock>(d->before_recording)->Apply(*d);
  d->before_recording.reset();
  Wr32(out, com::Publish(b, StateBlockVtable()));
  return D3D_OK;
}

HLE_NAMED("d3d9.dll", "IDirect3DStateBlock9::AddRef", D3DSB9_AddRef, 1) { return com::AddRef(Arg(c, 0)); }
HLE_NAMED("d3d9.dll", "IDirect3DStateBlock9::Release", D3DSB9_Release, 1) { return com::Release(Arg(c, 0)); }
HLE_NAMED("d3d9.dll", "IDirect3DStateBlock9::GetDevice", D3DSB9_GetDevice, 2) {
  const uint32_t dev = Self(c)->device;
  com::AddRef(dev);
  Wr32(Arg(c, 1), dev);
  return D3D_OK;
}
HLE_NAMED("d3d9.dll", "IDirect3DStateBlock9::Capture", D3DSB9_Capture, 1) {
  StateBlock* b = Self(c);
  b->Capture(*DeviceOf(b->device));
  return D3D_OK;
}
HLE_NAMED("d3d9.dll", "IDirect3DStateBlock9::Apply", D3DSB9_Apply, 1) {
  StateBlock* b = Self(c);
  b->Apply(*DeviceOf(b->device));
  return D3D_OK;
}

}  // namespace recomp::hle
