// cudart32_30_9.dll: NVIDIA's CUDA 3.0 runtime (32-bit), as on a machine
// with no CUDA device. Middleware that ships CUDA kernels (PhysX 2.x's
// core, for one) imports it statically and registers its kernels, device
// variables and textures from static constructors at load, whether or not
// a GPU is ever used; those registrations are recorded nowhere (nothing
// can launch them). Calls that would need a device (cudaMalloc,
// cudaLaunch, ...) have no handler: such middleware only makes them after
// choosing GPU work, so one is a bug to find, by name.
//
// CUDARTAPI is __stdcall on Windows.

#include <cstdint>

#include "runtime/hle/hle.h"
#include "runtime/hle/kernel32_state.h"

namespace recomp::hle {
namespace {

constexpr const char* kDll = "cudart32_30_9.dll";

// __cudaRegisterFatBinary(fatCubin): a handle for the module's later
// registrations and its __cudaUnregisterFatBinary.
HLE_NAMED(kDll, "__cudaRegisterFatBinary", CudaRegisterFatBinary, 1) {
  return HeapAllocProcess(16, true);
}
HLE_NAMED(kDll, "__cudaUnregisterFatBinary", CudaUnregisterFatBinary, 1) {
  if (gaddr h = Arg(c, 0)) HeapFreeProcess(h);
  return 0;
}
// (handle, hostFun, deviceFun, deviceName, thread_limit, tid, bid, bDim,
// gDim, wSize)
HLE_NAMED(kDll, "__cudaRegisterFunction", CudaRegisterFunction, 10) { return 0; }
// (handle, hostVar, deviceAddress, deviceName, ext, size, constant, global)
HLE_NAMED(kDll, "__cudaRegisterVar", CudaRegisterVar, 8) { return 0; }
// (handle, hostVar, deviceAddress, deviceName, dim, norm, ext)
HLE_NAMED(kDll, "__cudaRegisterTexture", CudaRegisterTexture, 7) { return 0; }
// (handle, devicePtr)
HLE_NAMED(kDll, "__cudaRegisterShared", CudaRegisterShared, 2) { return 0; }

// cudaCreateChannelDesc(x, y, z, w, kind): a cudaChannelFormatDesc {x, y,
// z, w, f}, returned through the hidden buffer (first argument).
HLE_NAMED(kDll, "cudaCreateChannelDesc", CudaCreateChannelDesc, 6) {
  const gaddr out = Arg(c, 0);
  for (int i = 0; i < 5; i++) Wr32(out + 4 * i, uint32_t(Arg(c, 1 + i)));
  return out;
}

}  // namespace
}  // namespace recomp::hle
