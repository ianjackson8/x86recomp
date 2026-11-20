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
// The pixel- and vertex-only kinds and recorded blocks (Begin/
// EndStateBlock) are not modelled yet: fatal, named.

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

  void Capture(const Device& d) {
    std::memcpy(rs, d.rs.v, sizeof rs);
    std::memcpy(ss, d.ss, sizeof ss);
    std::memcpy(tss, d.tss, sizeof tss);
    for (int i = 0; i < 20; i++) Bind(textures[i], d.textures[i].guest);
    for (int i = 0; i < 16; i++) {
      Bind(streams[i].buffer, d.streams[i].buffer.guest);
      streams[i].offset = d.streams[i].offset;
      streams[i].stride = d.streams[i].stride;
      streams[i].freq = d.streams[i].freq;
    }
    Bind(indices, d.indices.guest);
    Bind(decl, d.decl.guest);
    fvf = d.fvf;
    Bind(vs, d.vs.guest);
    Bind(ps, d.ps.guest);
    std::memcpy(vs_f, d.vs_f, sizeof vs_f);
    std::memcpy(ps_f, d.ps_f, sizeof ps_f);
    std::memcpy(vs_i, d.vs_i, sizeof vs_i);
    std::memcpy(ps_i, d.ps_i, sizeof ps_i);
    std::memcpy(vs_b, d.vs_b, sizeof vs_b);
    std::memcpy(ps_b, d.ps_b, sizeof ps_b);
    transforms = d.transforms;
    std::memcpy(clip_planes, d.clip_planes, sizeof clip_planes);
    viewport = d.viewport;
    scissor = d.scissor;
  }

  void Apply(Device& d) const {
    for (uint32_t s = 0; s < 256; s++) d.rs.Set(s, rs[s]);
    for (uint32_t s = 0; s < 20; s++)
      for (uint32_t t = 0; t < 14; t++) d.SetSamplerState(s, t, ss[s][t]);
    std::memcpy(d.tss, tss, sizeof tss);
    for (int i = 0; i < 20; i++) Bind(d.textures[i], textures[i].guest);
    for (int i = 0; i < 16; i++) {
      Bind(d.streams[i].buffer, streams[i].buffer.guest);
      d.streams[i].offset = streams[i].offset;
      d.streams[i].stride = streams[i].stride;
      d.streams[i].freq = streams[i].freq;
    }
    Bind(d.indices, indices.guest);
    // A declaration or an FVF: SetFVF clears the declaration and vice versa.
    Bind(d.decl, decl.guest);
    d.fvf = fvf;
    Bind(d.vs, vs.guest);
    Bind(d.ps, ps.guest);
    std::memcpy(d.vs_f, vs_f, sizeof vs_f);
    std::memcpy(d.ps_f, ps_f, sizeof ps_f);
    std::memcpy(d.vs_i, vs_i, sizeof vs_i);
    std::memcpy(d.ps_i, ps_i, sizeof ps_i);
    std::memcpy(d.vs_b, vs_b, sizeof vs_b);
    std::memcpy(d.ps_b, ps_b, sizeof ps_b);
    d.vs_f_version++;
    d.ps_f_version++;
    d.vs_ib_version++;
    d.ps_ib_version++;
    d.transforms = transforms;
    std::memcpy(d.clip_planes, clip_planes, sizeof clip_planes);
    d.viewport = viewport;
    d.scissor = scissor;
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
  b->Capture(*DeviceOf(b->device));
  Wr32(out, com::Publish(b, StateBlockVtable()));
  return D3D_OK;
}
HLE_NAMED("d3d9.dll", "IDirect3DDevice9::BeginStateBlock", D3DDev9_BeginStateBlock, 1) {
  FatalF("BeginStateBlock: recorded state blocks are not supported yet");
}
HLE_NAMED("d3d9.dll", "IDirect3DDevice9::EndStateBlock", D3DDev9_EndStateBlock, 2) {
  FatalF("EndStateBlock: recorded state blocks are not supported yet");
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
