// openal32.dll: OpenAL 1.1 on the runtime's mixer (audio/mixer.h).
//
// One device and context at a time (the mixer). Buffers hold PCM (8-bit
// unsigned or 16-bit signed, mono or stereo), decoded to float when
// alBufferData copies them out of guest memory. Each source owns a mixer
// voice fed by an AlStream: its static buffer, or its queue of buffers,
// with OpenAL's looping, processed/queued counts and unqueueing. A buffer
// counts as processed once the mixer has read all of it (the mixer reads a
// little ahead of what is audible; that only refills a stream earlier).
//
// Spatialisation follows the OpenAL 1.1 specification: a mono source's
// gain through the distance model (all six), clamped by AL_MIN/MAX_GAIN,
// its cone, the listener's gain; Doppler shift into the pitch (doppler
// factor, speed of sound, velocities); panned with equal power by the
// source's direction in the listener's frame (at x up = right). Stereo
// buffers play as they are. The left and right gains go to the voice as
// DirectSound-style linear volume and pan (Voice::dsound), which gives
// them exactly.
//
// No extensions (EFX, ...): alIsExtensionPresent says so and
// alGetProcAddress returns NULL for them. Parameters outside this model
// are fatal, by name, not ignored. Windows' OpenAL32 is __cdecl.

#include <algorithm>
#include <cmath>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "runtime/audio/mixer.h"
#include "runtime/hle/hle.h"
#include "runtime/hle/kernel32_state.h"
#include "runtime/log.h"

namespace recomp::hle {
namespace {

constexpr const char* kDll = "openal32.dll";

// AL / ALC enums.
enum : uint32_t {
  AL_NO_ERROR = 0, AL_INVALID_NAME = 0xA001, AL_INVALID_ENUM = 0xA002,
  AL_INVALID_VALUE = 0xA003, AL_INVALID_OPERATION = 0xA004, AL_OUT_OF_MEMORY = 0xA005,
  AL_SOURCE_RELATIVE = 0x202, AL_CONE_INNER_ANGLE = 0x1001, AL_CONE_OUTER_ANGLE = 0x1002,
  AL_PITCH = 0x1003, AL_POSITION = 0x1004, AL_DIRECTION = 0x1005, AL_VELOCITY = 0x1006,
  AL_LOOPING = 0x1007, AL_BUFFER = 0x1009, AL_GAIN = 0x100A, AL_MIN_GAIN = 0x100D,
  AL_MAX_GAIN = 0x100E, AL_ORIENTATION = 0x100F, AL_SOURCE_STATE = 0x1010,
  AL_INITIAL = 0x1011, AL_PLAYING = 0x1012, AL_PAUSED = 0x1013, AL_STOPPED = 0x1014,
  AL_BUFFERS_QUEUED = 0x1015, AL_BUFFERS_PROCESSED = 0x1016, AL_REFERENCE_DISTANCE = 0x1020,
  AL_ROLLOFF_FACTOR = 0x1021, AL_CONE_OUTER_GAIN = 0x1022, AL_MAX_DISTANCE = 0x1023,
  AL_SEC_OFFSET = 0x1024, AL_SAMPLE_OFFSET = 0x1025, AL_BYTE_OFFSET = 0x1026,
  AL_SOURCE_TYPE = 0x1027, AL_STATIC = 0x1028, AL_STREAMING = 0x1029, AL_UNDETERMINED = 0x1030,
  AL_FORMAT_MONO8 = 0x1100, AL_FORMAT_MONO16 = 0x1101, AL_FORMAT_STEREO8 = 0x1102,
  AL_FORMAT_STEREO16 = 0x1103, AL_FREQUENCY = 0x2001, AL_BITS = 0x2002, AL_CHANNELS = 0x2003,
  AL_SIZE = 0x2004, AL_VENDOR = 0xB001, AL_VERSION = 0xB002, AL_RENDERER = 0xB003,
  AL_EXTENSIONS = 0xB004, AL_NONE = 0, AL_INVERSE_DISTANCE = 0xD001,
  AL_INVERSE_DISTANCE_CLAMPED = 0xD002, AL_LINEAR_DISTANCE = 0xD003,
  AL_LINEAR_DISTANCE_CLAMPED = 0xD004, AL_EXPONENT_DISTANCE = 0xD005,
  AL_EXPONENT_DISTANCE_CLAMPED = 0xD006,
  ALC_NO_ERROR = 0, ALC_INVALID_DEVICE = 0xA001, ALC_INVALID_CONTEXT = 0xA002,
  ALC_INVALID_ENUM = 0xA003, ALC_INVALID_VALUE = 0xA004,
  ALC_MAJOR_VERSION = 0x1000, ALC_MINOR_VERSION = 0x1001, ALC_ATTRIBUTES_SIZE = 0x1002,
  ALC_ALL_ATTRIBUTES = 0x1003, ALC_DEFAULT_DEVICE_SPECIFIER = 0x1004,
  ALC_DEVICE_SPECIFIER = 0x1005, ALC_EXTENSIONS = 0x1006, ALC_FREQUENCY = 0x1007,
  ALC_REFRESH = 0x1008, ALC_SYNC = 0x1009, ALC_MONO_SOURCES = 0x1010,
  ALC_STEREO_SOURCES = 0x1011,
};

constexpr int kRate = 44100;
constexpr int kMaxSources = 256;  // ALC_MONO_SOURCES + ALC_STEREO_SOURCES
const char* const kDeviceName = "x86recomp mixer";

struct V3 {
  float x = 0, y = 0, z = 0;
};
V3 Sub(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
float Dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
V3 Cross(V3 a, V3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
float Len(V3 a) { return std::sqrt(Dot(a, a)); }

struct AlBuffer {
  int channels = 0, bits = 0, rate = 0;
  uint32_t size = 0;                  // bytes, as alBufferData gave them
  std::vector<float> frames;          // interleaved, channels per frame
  int64_t Frames() const { return channels ? int64_t(frames.size()) / channels : 0; }
};
using BufferPtr = std::shared_ptr<const AlBuffer>;

// A source's audio for the mixer: its queue (a static buffer is a queue of
// one), in the channel count of the first buffer. Touched by the mixer
// thread (Read) and by the AL calls, both with the mixer's lock held.
class AlStream final : public audio::Source {
 public:
  AlStream(int channels, int rate) {
    channels_ = channels;
    rate_ = rate;
  }
  std::deque<BufferPtr> queue;
  size_t cur = 0;    // the buffer playing (those before it are processed)
  int64_t pos = 0;   // frame in it
  bool looping = false;

  int Read(float* out, int n) override {
    int done = 0;
    while (done < n) {
      if (cur >= queue.size()) {
        if (!looping || queue.empty()) break;
        cur = 0;
        pos = 0;
      }
      const AlBuffer& b = *queue[cur];
      const int64_t avail = b.Frames() - pos;
      if (avail <= 0) {
        cur++;
        pos = 0;
        continue;
      }
      const int take = int(std::min<int64_t>(avail, n - done));
      for (int i = 0; i < take; i++) {
        const float* f = &b.frames[size_t(pos + i) * b.channels];
        float* o = out + size_t(done + i) * channels_;
        if (b.channels == channels_) {
          for (int k = 0; k < channels_; k++) o[k] = f[k];
        } else if (channels_ == 2) {  // mono into a stereo stream
          o[0] = o[1] = f[0];
        } else {  // stereo into a mono stream
          o[0] = 0.5f * (f[0] + f[1]);
        }
      }
      pos += take;
      done += take;
    }
    return done;
  }
  bool Seek(int64_t f) override {
    cur = 0;
    pos = 0;
    while (cur < queue.size() && f >= queue[cur]->Frames()) {
      f -= queue[cur]->Frames();
      cur++;
    }
    pos = f;
    return true;
  }
  // Frames before the read position, from the start of the queue.
  int64_t Offset() const {
    int64_t f = pos;
    for (size_t i = 0; i < cur && i < queue.size(); i++) f += queue[i]->Frames();
    return f;
  }
};

struct AlSource {
  uint32_t type = AL_UNDETERMINED;
  uint32_t state = AL_INITIAL;
  uint32_t buffer = 0;  // AL_BUFFER (static)
  float gain = 1, pitch = 1, min_gain = 0, max_gain = 1;
  float ref = 1, rolloff = 1, max_dist = 3.40282347e38f;
  float cone_inner = 360, cone_outer = 360, cone_outer_gain = 0;
  V3 pos, vel, dir;
  bool relative = false, looping = false;
  std::deque<std::pair<uint32_t, BufferPtr>> queue;  // the AL view (names)
  audio::Voice voice;
  AlStream* stream = nullptr;  // voice.src, once there is audio
  bool in_mixer = false;
};

struct Listener {
  V3 pos, vel, at{0, 0, -1}, up{0, 1, 0};
  float gain = 1;
};

// All AL state, under g_mu (then the mixer's lock, for voices).
std::mutex g_mu;
gaddr g_device = 0, g_context = 0, g_current = 0;
uint32_t g_error = AL_NO_ERROR, g_alc_error = ALC_NO_ERROR;
uint32_t g_next_name = 0;
std::map<uint32_t, std::shared_ptr<AlBuffer>> g_buffers;
std::map<uint32_t, std::unique_ptr<AlSource>> g_sources;
Listener g_listener;
uint32_t g_model = AL_INVERSE_DISTANCE_CLAMPED;
float g_doppler = 1, g_sound_speed = 343.3f;

void Error(uint32_t e) {
  if (g_error == AL_NO_ERROR) g_error = e;
}

gaddr CString(const std::string& s) {
  static std::map<std::string, gaddr> cache;
  gaddr& g = cache[s];
  if (!g) {
    g = HeapAllocProcess(s.size() + 1, true);
    std::memcpy(GPtr(g), s.data(), s.size());
  }
  return g;
}

AlSource* Src(uint32_t id) {
  auto it = g_sources.find(id);
  if (it == g_sources.end()) {
    Error(AL_INVALID_NAME);
    return nullptr;
  }
  return it->second.get();
}

// The voice's gains, pan and rate from the source, the listener and the
// globals (with the mixer's lock held).
void Update(AlSource& s) {
  if (!s.stream) return;
  float l, r, doppler = 1;
  if (s.stream->channels() == 2) {
    const float g = std::clamp(s.gain, s.min_gain, s.max_gain) * g_listener.gain;
    l = r = g;
  } else {
    const V3 rel = s.relative ? s.pos : Sub(s.pos, g_listener.pos);
    float d = Len(rel);
    float att = 1;
    const float ref = s.ref, roll = s.rolloff, maxd = s.max_dist;
    const bool clamped = g_model == AL_INVERSE_DISTANCE_CLAMPED ||
                         g_model == AL_LINEAR_DISTANCE_CLAMPED ||
                         g_model == AL_EXPONENT_DISTANCE_CLAMPED;
    if (clamped && g_model != AL_NONE) d = std::clamp(d, ref, std::max(ref, maxd));
    switch (g_model) {
      case AL_NONE: break;
      case AL_INVERSE_DISTANCE:
      case AL_INVERSE_DISTANCE_CLAMPED: {
        const float den = ref + roll * (d - ref);
        att = den > 0 ? ref / den : 1;
        break;
      }
      case AL_LINEAR_DISTANCE:
      case AL_LINEAR_DISTANCE_CLAMPED:
        att = maxd > ref ? 1 - roll * (std::min(d, maxd) - ref) / (maxd - ref) : 1;
        break;
      case AL_EXPONENT_DISTANCE:
      case AL_EXPONENT_DISTANCE_CLAMPED:
        att = d > 0 && ref > 0 ? std::pow(d / ref, -roll) : 1;
        break;
    }
    att = std::max(att, 0.0f);
    // The cone: the angle between the source's direction and the line to
    // the listener (cone angles are whole angles, in degrees).
    float cone = 1;
    if (Len(s.dir) > 0 && d > 0 && s.cone_inner < 360) {
      const V3 to_listener{-rel.x, -rel.y, -rel.z};
      const float cosang = std::clamp(Dot(s.dir, to_listener) / (Len(s.dir) * Len(to_listener)), -1.0f, 1.0f);
      const float ang = 2 * std::acos(cosang) * 180 / 3.14159265f;
      if (ang > s.cone_outer) cone = s.cone_outer_gain;
      else if (ang > s.cone_inner && s.cone_outer > s.cone_inner)
        cone = 1 + (s.cone_outer_gain - 1) * (ang - s.cone_inner) / (s.cone_outer - s.cone_inner);
    }
    const float g = std::clamp(s.gain * att * cone, s.min_gain, s.max_gain) * g_listener.gain;
    // Equal-power pan by the source's x in the listener's frame.
    float x = 0;
    const float dist = Len(rel);
    if (dist > 0) {
      if (s.relative) {
        x = rel.x / dist;
      } else {
        V3 right = Cross(g_listener.at, g_listener.up);
        const float rl = Len(right);
        if (rl > 0) x = Dot(rel, right) / (rl * dist);
      }
    }
    const float theta = (std::clamp(x, -1.0f, 1.0f) + 1) * 3.14159265f / 4;
    l = g * std::cos(theta);
    r = g * std::sin(theta);
    // Doppler (OpenAL 1.1): velocities projected on the source-listener
    // line, clamped below the speed of sound.
    if (g_doppler > 0 && g_sound_speed > 0 && dist > 0) {
      const V3 sl = Sub(g_listener.pos, s.relative ? V3{} : s.pos);
      const float sll = Len(sl);
      if (sll > 0) {
        const float lim = g_sound_speed / g_doppler;
        float vls = std::min(Dot(g_listener.vel, sl) / sll, lim);
        float vss = std::min(Dot(s.vel, sl) / sll, lim);
        doppler = (g_sound_speed - g_doppler * vls) / (g_sound_speed - g_doppler * vss);
      }
    }
  }
  // Exact left/right gains as DirectSound volume (linear) and pan (100ths
  // of a dB off the quieter side).
  audio::Voice& v = s.voice;
  v.volume = std::max(l, r);
  if (l <= 0 && r <= 0) v.pan = 0;
  else if (l >= r) v.pan = r > 0 ? std::max(-10000.0f, 2000 * std::log10(r / l)) : -10000.0f;
  else v.pan = l > 0 ? std::min(10000.0f, -2000 * std::log10(l / r)) : 10000.0f;
  v.rate = float(s.stream->rate()) * std::max(s.pitch * doppler, 1e-3f);
}

void UpdateAll() {
  for (auto& [id, s] : g_sources) Update(*s);
}

// The source's queue (or static buffer) into its stream, making the
// stream if needed (mixer lock held).
void Rebuild(AlSource& s) {
  if (s.queue.empty()) {
    if (s.stream) s.stream->queue.clear();
    return;
  }
  const AlBuffer& first = *s.queue.front().second;
  if (!s.stream || s.stream->channels() != first.channels || s.stream->rate() != first.rate) {
    auto st = std::make_unique<AlStream>(first.channels, first.rate);
    s.stream = st.get();
    s.voice.src = std::move(st);
  }
  s.stream->queue.clear();
  for (auto& [n, b] : s.queue) s.stream->queue.push_back(b);
  s.stream->looping = s.looping;
}

uint32_t StateOf(AlSource& s) {
  if (s.state == AL_PLAYING && s.voice.state == audio::Voice::State::Done) s.state = AL_STOPPED;
  return s.state;
}

void Stop(AlSource& s, audio::Mixer& m) {
  if (s.in_mixer) {
    m.Remove(&s.voice);
    s.in_mixer = false;
  }
  s.voice.state = audio::Voice::State::Idle;
  s.state = AL_STOPPED;
}

}  // namespace

// ---- ALC -----------------------------------------------------------------------

// alcOpenDevice(name): the mixer (NULL name or ours); NULL without an
// audio device, as OpenAL does.
HLE(kDll, alcOpenDevice, kCdecl) {
  std::lock_guard<std::mutex> l(g_mu);
  if (Arg(c, 0)) {
    const std::string n = GStr(Arg(c, 0));
    if (!n.empty() && n != kDeviceName) WarnF("alcOpenDevice(\"%s\"): opening the mixer\n", n.c_str());
  }
  if (g_device) return 0;  // one device
  audio::Mixer& m = audio::GetMixer();
  if (!m.rate() && !m.Open(kRate)) {
    g_alc_error = ALC_INVALID_VALUE;
    return 0;
  }
  g_device = HeapAllocProcess(16, true);
  return g_device;
}
HLE(kDll, alcCloseDevice, kCdecl) {
  std::lock_guard<std::mutex> l(g_mu);
  if (!g_device || Arg(c, 0) != g_device) {
    g_alc_error = ALC_INVALID_DEVICE;
    return 0;
  }
  if (g_context) return 0;  // contexts first
  g_device = 0;
  return 1;
}
HLE(kDll, alcCreateContext, kCdecl) {
  std::lock_guard<std::mutex> l(g_mu);
  if (!g_device || Arg(c, 0) != g_device) {
    g_alc_error = ALC_INVALID_DEVICE;
    return 0;
  }
  if (g_context) FatalF("alcCreateContext: a second context is not modelled");
  // Attributes (frequency, refresh, source counts) are requests: the mixer
  // keeps its own.
  g_context = HeapAllocProcess(16, true);
  return g_context;
}
HLE(kDll, alcMakeContextCurrent, kCdecl) {
  std::lock_guard<std::mutex> l(g_mu);
  const gaddr ctx = Arg(c, 0);
  if (ctx && ctx != g_context) {
    g_alc_error = ALC_INVALID_CONTEXT;
    return 0;
  }
  g_current = ctx;
  return 1;
}
HLE(kDll, alcDestroyContext, kCdecl) {
  std::lock_guard<std::mutex> l(g_mu);
  if (!g_context || Arg(c, 0) != g_context) {
    g_alc_error = ALC_INVALID_CONTEXT;
    return 0;
  }
  audio::Mixer& m = audio::GetMixer();
  {
    std::lock_guard<std::mutex> ml(m.mu());
    for (auto& [id, s] : g_sources) Stop(*s, m);
  }
  g_sources.clear();
  if (g_current == g_context) g_current = 0;
  g_context = 0;
  return 0;
}
HLE(kDll, alcGetError, kCdecl) {
  std::lock_guard<std::mutex> l(g_mu);
  return std::exchange(g_alc_error, uint32_t(ALC_NO_ERROR));
}
HLE(kDll, alcGetString, kCdecl) {
  switch (uint32_t(Arg(c, 1))) {
    case ALC_DEFAULT_DEVICE_SPECIFIER:
      return CString(kDeviceName);
    case ALC_DEVICE_SPECIFIER:  // with a NULL device: the list, NUL-separated
      return Arg(c, 0) ? CString(kDeviceName) : CString(std::string(kDeviceName) + '\0');
    case ALC_EXTENSIONS: return CString("");
    case ALC_NO_ERROR: return CString("No Error");
    case ALC_INVALID_DEVICE: return CString("Invalid Device");
    case ALC_INVALID_CONTEXT: return CString("Invalid Context");
    case ALC_INVALID_ENUM: return CString("Invalid Enum");
    case ALC_INVALID_VALUE: return CString("Invalid Value");
    default: FatalF("alcGetString(0x%X) not modelled", uint32_t(Arg(c, 1)));
  }
}
// alcGetIntegerv(device, param, size, values)
HLE(kDll, alcGetIntegerv, kCdecl) {
  const uint32_t param = uint32_t(Arg(c, 1)), size = uint32_t(Arg(c, 2));
  const gaddr out = Arg(c, 3);
  if (!out || !size) return 0;
  const int32_t attrs[] = {int32_t(ALC_FREQUENCY), kRate, int32_t(ALC_REFRESH), 100,
                           int32_t(ALC_SYNC), 0, int32_t(ALC_MONO_SOURCES), kMaxSources - 16,
                           int32_t(ALC_STEREO_SOURCES), 16, 0};
  switch (param) {
    case ALC_MAJOR_VERSION: Wr32(out, 1); break;
    case ALC_MINOR_VERSION: Wr32(out, 1); break;
    case ALC_FREQUENCY: Wr32(out, kRate); break;
    case ALC_REFRESH: Wr32(out, 100); break;
    case ALC_SYNC: Wr32(out, 0); break;
    case ALC_MONO_SOURCES: Wr32(out, kMaxSources - 16); break;
    case ALC_STEREO_SOURCES: Wr32(out, 16); break;
    case ALC_ATTRIBUTES_SIZE: Wr32(out, uint32_t(std::size(attrs))); break;
    case ALC_ALL_ATTRIBUTES:
      for (uint32_t i = 0; i < std::min<uint32_t>(size, uint32_t(std::size(attrs))); i++)
        Wr32(out + 4 * i, uint32_t(attrs[i]));
      break;
    default: FatalF("alcGetIntegerv(0x%X) not modelled", param);
  }
  return 0;
}
HLE(kDll, alcIsExtensionPresent, kCdecl) { return 0; }

// ---- AL state --------------------------------------------------------------------

HLE(kDll, alGetError, kCdecl) {
  std::lock_guard<std::mutex> l(g_mu);
  return std::exchange(g_error, uint32_t(AL_NO_ERROR));
}
HLE(kDll, alGetString, kCdecl) {
  switch (uint32_t(Arg(c, 0))) {
    case AL_VENDOR: return CString("x86recomp");
    case AL_VERSION: return CString("1.1");
    case AL_RENDERER: return CString("Software mixer");
    case AL_EXTENSIONS: return CString("");
    case AL_NO_ERROR: return CString("No Error");
    case AL_INVALID_NAME: return CString("Invalid Name");
    case AL_INVALID_ENUM: return CString("Invalid Enum");
    case AL_INVALID_VALUE: return CString("Invalid Value");
    case AL_INVALID_OPERATION: return CString("Invalid Operation");
    case AL_OUT_OF_MEMORY: return CString("Out of Memory");
    default: FatalF("alGetString(0x%X) not modelled", uint32_t(Arg(c, 0)));
  }
}
HLE(kDll, alIsExtensionPresent, kCdecl) { return 0; }
// alGetProcAddress(name): extension entry points; there are none.
HLE(kDll, alGetProcAddress, kCdecl) {
  WarnF("alGetProcAddress(\"%s\"): no extensions, NULL\n", GStr(Arg(c, 0)).c_str());
  return 0;
}
HLE(kDll, alDistanceModel, kCdecl) {
  std::lock_guard<std::mutex> l(g_mu);
  const uint32_t m = uint32_t(Arg(c, 0));
  if (m != AL_NONE && (m < AL_INVERSE_DISTANCE || m > AL_EXPONENT_DISTANCE_CLAMPED)) {
    Error(AL_INVALID_VALUE);
    return 0;
  }
  g_model = m;
  std::lock_guard<std::mutex> ml(audio::GetMixer().mu());
  UpdateAll();
  return 0;
}
HLE(kDll, alDopplerFactor, kCdecl) {
  std::lock_guard<std::mutex> l(g_mu);
  const float f = ArgF(c, 0);
  if (f < 0) {
    Error(AL_INVALID_VALUE);
    return 0;
  }
  g_doppler = f;
  std::lock_guard<std::mutex> ml(audio::GetMixer().mu());
  UpdateAll();
  return 0;
}
HLE(kDll, alSpeedOfSound, kCdecl) {
  std::lock_guard<std::mutex> l(g_mu);
  const float f = ArgF(c, 0);
  if (f <= 0) {
    Error(AL_INVALID_VALUE);
    return 0;
  }
  g_sound_speed = f;
  std::lock_guard<std::mutex> ml(audio::GetMixer().mu());
  UpdateAll();
  return 0;
}

// ---- listener ----------------------------------------------------------------------

HLE(kDll, alListenerf, kCdecl) {
  std::lock_guard<std::mutex> l(g_mu);
  if (uint32_t(Arg(c, 0)) != AL_GAIN) FatalF("alListenerf(0x%X) not modelled", uint32_t(Arg(c, 0)));
  const float v = ArgF(c, 1);
  if (v < 0) {
    Error(AL_INVALID_VALUE);
    return 0;
  }
  g_listener.gain = v;
  std::lock_guard<std::mutex> ml(audio::GetMixer().mu());
  UpdateAll();
  return 0;
}
HLE(kDll, alListenerfv, kCdecl) {
  std::lock_guard<std::mutex> l(g_mu);
  const uint32_t p = uint32_t(Arg(c, 0));
  const gaddr v = Arg(c, 1);
  auto f = [&](int i) {
    float x;
    std::memcpy(&x, GPtr(v + 4 * i), 4);
    return x;
  };
  switch (p) {
    case AL_POSITION: g_listener.pos = {f(0), f(1), f(2)}; break;
    case AL_VELOCITY: g_listener.vel = {f(0), f(1), f(2)}; break;
    case AL_ORIENTATION:
      g_listener.at = {f(0), f(1), f(2)};
      g_listener.up = {f(3), f(4), f(5)};
      break;
    case AL_GAIN: g_listener.gain = f(0); break;
    default: FatalF("alListenerfv(0x%X) not modelled", p);
  }
  std::lock_guard<std::mutex> ml(audio::GetMixer().mu());
  UpdateAll();
  return 0;
}

// ---- buffers -------------------------------------------------------------------------

HLE(kDll, alGenBuffers, kCdecl) {
  std::lock_guard<std::mutex> l(g_mu);
  const int32_t n = int32_t(Arg(c, 0));
  if (n < 0) {
    Error(AL_INVALID_VALUE);
    return 0;
  }
  for (int32_t i = 0; i < n; i++) {
    const uint32_t id = ++g_next_name;
    g_buffers[id] = std::make_shared<AlBuffer>();
    Wr32(Arg(c, 1) + 4 * i, id);
  }
  return 0;
}
HLE(kDll, alDeleteBuffers, kCdecl) {
  std::lock_guard<std::mutex> l(g_mu);
  const int32_t n = int32_t(Arg(c, 0));
  for (int32_t i = 0; i < n; i++) {
    const uint32_t id = Rd32(Arg(c, 1) + 4 * i);
    if (!id) continue;
    if (!g_buffers.count(id)) {
      Error(AL_INVALID_NAME);
      return 0;
    }
    for (auto& [sid, s] : g_sources)
      for (auto& [bn, b] : s->queue)
        if (bn == id) {
          Error(AL_INVALID_OPERATION);  // still in use
          return 0;
        }
  }
  for (int32_t i = 0; i < n; i++) g_buffers.erase(Rd32(Arg(c, 1) + 4 * i));
  return 0;
}
// alBufferData(buffer, format, data, size, frequency)
HLE(kDll, alBufferData, kCdecl) {
  std::lock_guard<std::mutex> l(g_mu);
  auto it = g_buffers.find(uint32_t(Arg(c, 0)));
  if (it == g_buffers.end()) {
    Error(AL_INVALID_NAME);
    return 0;
  }
  const uint32_t format = uint32_t(Arg(c, 1)), size = uint32_t(Arg(c, 3));
  const int32_t freq = int32_t(Arg(c, 4));
  int ch, bits;
  switch (format) {
    case AL_FORMAT_MONO8: ch = 1; bits = 8; break;
    case AL_FORMAT_MONO16: ch = 1; bits = 16; break;
    case AL_FORMAT_STEREO8: ch = 2; bits = 8; break;
    case AL_FORMAT_STEREO16: ch = 2; bits = 16; break;
    default: FatalF("alBufferData: format 0x%X not modelled", format);
  }
  if (freq <= 0 || size % (ch * bits / 8)) {
    Error(AL_INVALID_VALUE);
    return 0;
  }
  // A buffer queued on a source keeps its data until unqueued: a new one.
  auto b = std::make_shared<AlBuffer>();
  b->channels = ch;
  b->bits = bits;
  b->rate = freq;
  b->size = size;
  const uint8_t* p = GPtr(Arg(c, 2));
  if (bits == 8) {
    b->frames.resize(size);
    for (uint32_t i = 0; i < size; i++) b->frames[i] = (float(p[i]) - 128.0f) / 128.0f;
  } else {
    b->frames.resize(size / 2);
    for (uint32_t i = 0; i < size / 2; i++) {
      int16_t s;
      std::memcpy(&s, p + 2 * i, 2);
      b->frames[i] = float(s) / 32768.0f;
    }
  }
  for (auto& [sid, s] : g_sources)
    for (auto& [bn, qb] : s->queue)
      if (bn == it->first) {
        Error(AL_INVALID_OPERATION);  // in use: OpenAL refuses
        return 0;
      }
  it->second = std::move(b);
  return 0;
}
HLE(kDll, alGetBufferi, kCdecl) {
  std::lock_guard<std::mutex> l(g_mu);
  auto it = g_buffers.find(uint32_t(Arg(c, 0)));
  if (it == g_buffers.end()) {
    Error(AL_INVALID_NAME);
    return 0;
  }
  const AlBuffer& b = *it->second;
  uint32_t v;
  switch (uint32_t(Arg(c, 1))) {
    case AL_FREQUENCY: v = uint32_t(b.rate); break;
    case AL_BITS: v = uint32_t(b.bits); break;
    case AL_CHANNELS: v = uint32_t(b.channels); break;
    case AL_SIZE: v = b.size; break;
    default: FatalF("alGetBufferi(0x%X) not modelled", uint32_t(Arg(c, 1)));
  }
  if (Arg(c, 2)) Wr32(Arg(c, 2), v);
  return 0;
}

// ---- sources ---------------------------------------------------------------------------

HLE(kDll, alGenSources, kCdecl) {
  std::lock_guard<std::mutex> l(g_mu);
  const int32_t n = int32_t(Arg(c, 0));
  if (n < 0) {
    Error(AL_INVALID_VALUE);
    return 0;
  }
  if (g_sources.size() + size_t(n) > kMaxSources) {
    Error(AL_OUT_OF_MEMORY);
    return 0;
  }
  for (int32_t i = 0; i < n; i++) {
    const uint32_t id = ++g_next_name;
    auto s = std::make_unique<AlSource>();
    s->voice.dsound = true;
    g_sources[id] = std::move(s);
    Wr32(Arg(c, 1) + 4 * i, id);
  }
  return 0;
}
HLE(kDll, alDeleteSources, kCdecl) {
  std::lock_guard<std::mutex> l(g_mu);
  const int32_t n = int32_t(Arg(c, 0));
  for (int32_t i = 0; i < n; i++)
    if (!g_sources.count(Rd32(Arg(c, 1) + 4 * i))) {
      Error(AL_INVALID_NAME);
      return 0;
    }
  audio::Mixer& m = audio::GetMixer();
  std::lock_guard<std::mutex> ml(m.mu());
  for (int32_t i = 0; i < n; i++) {
    auto it = g_sources.find(Rd32(Arg(c, 1) + 4 * i));
    if (it == g_sources.end()) continue;
    Stop(*it->second, m);
    g_sources.erase(it);
  }
  return 0;
}

namespace {
void SetSourcef(AlSource& s, uint32_t p, float v) {
  switch (p) {
    case AL_GAIN: s.gain = v; break;
    case AL_PITCH: s.pitch = v; break;
    case AL_MIN_GAIN: s.min_gain = v; break;
    case AL_MAX_GAIN: s.max_gain = v; break;
    case AL_REFERENCE_DISTANCE: s.ref = v; break;
    case AL_ROLLOFF_FACTOR: s.rolloff = v; break;
    case AL_MAX_DISTANCE: s.max_dist = v; break;
    case AL_CONE_INNER_ANGLE: s.cone_inner = v; break;
    case AL_CONE_OUTER_ANGLE: s.cone_outer = v; break;
    case AL_CONE_OUTER_GAIN: s.cone_outer_gain = v; break;
    default: FatalF("alSourcef(0x%X) not modelled", p);
  }
}
void SetSource3(AlSource& s, uint32_t p, V3 v) {
  switch (p) {
    case AL_POSITION: s.pos = v; break;
    case AL_VELOCITY: s.vel = v; break;
    case AL_DIRECTION: s.dir = v; break;
    default: FatalF("alSource3f/3i(0x%X) not modelled", p);
  }
}
}  // namespace

HLE(kDll, alSourcef, kCdecl) {
  std::lock_guard<std::mutex> l(g_mu);
  AlSource* s = Src(uint32_t(Arg(c, 0)));
  if (!s) return 0;
  SetSourcef(*s, uint32_t(Arg(c, 1)), ArgF(c, 2));
  std::lock_guard<std::mutex> ml(audio::GetMixer().mu());
  Update(*s);
  return 0;
}
HLE(kDll, alSource3f, kCdecl) {
  std::lock_guard<std::mutex> l(g_mu);
  AlSource* s = Src(uint32_t(Arg(c, 0)));
  if (!s) return 0;
  SetSource3(*s, uint32_t(Arg(c, 1)), {ArgF(c, 2), ArgF(c, 3), ArgF(c, 4)});
  std::lock_guard<std::mutex> ml(audio::GetMixer().mu());
  Update(*s);
  return 0;
}
HLE(kDll, alSource3i, kCdecl) {
  std::lock_guard<std::mutex> l(g_mu);
  AlSource* s = Src(uint32_t(Arg(c, 0)));
  if (!s) return 0;
  SetSource3(*s, uint32_t(Arg(c, 1)),
             {float(int32_t(Arg(c, 2))), float(int32_t(Arg(c, 3))), float(int32_t(Arg(c, 4)))});
  std::lock_guard<std::mutex> ml(audio::GetMixer().mu());
  Update(*s);
  return 0;
}
// alSourcei: AL_BUFFER (a static buffer, or 0 to empty the source),
// AL_LOOPING, AL_SOURCE_RELATIVE; the float parameters as integers.
HLE(kDll, alSourcei, kCdecl) {
  std::lock_guard<std::mutex> l(g_mu);
  AlSource* s = Src(uint32_t(Arg(c, 0)));
  if (!s) return 0;
  const uint32_t p = uint32_t(Arg(c, 1));
  const int32_t v = int32_t(Arg(c, 2));
  audio::Mixer& m = audio::GetMixer();
  std::lock_guard<std::mutex> ml(m.mu());
  switch (p) {
    case AL_BUFFER: {
      const uint32_t st = StateOf(*s);
      if (st == AL_PLAYING || st == AL_PAUSED) {
        Error(AL_INVALID_OPERATION);
        return 0;
      }
      s->queue.clear();
      if (v) {
        auto it = g_buffers.find(uint32_t(v));
        if (it == g_buffers.end()) {
          Error(AL_INVALID_NAME);
          return 0;
        }
        s->queue.push_back({uint32_t(v), it->second});
        s->type = AL_STATIC;
      } else {
        s->type = AL_UNDETERMINED;
      }
      s->buffer = uint32_t(v);
      Rebuild(*s);
      break;
    }
    case AL_LOOPING:
      s->looping = v != 0;
      if (s->stream) s->stream->looping = s->looping;
      break;
    case AL_SOURCE_RELATIVE: s->relative = v != 0; break;
    case AL_CONE_INNER_ANGLE: case AL_CONE_OUTER_ANGLE: case AL_REFERENCE_DISTANCE:
    case AL_ROLLOFF_FACTOR: case AL_MAX_DISTANCE:
      SetSourcef(*s, p, float(v));
      break;
    default: FatalF("alSourcei(0x%X) not modelled", p);
  }
  Update(*s);
  return 0;
}
HLE(kDll, alGetSourcei, kCdecl) {
  std::lock_guard<std::mutex> l(g_mu);
  AlSource* s = Src(uint32_t(Arg(c, 0)));
  if (!s) return 0;
  std::lock_guard<std::mutex> ml(audio::GetMixer().mu());
  const uint32_t p = uint32_t(Arg(c, 1));
  int32_t v;
  switch (p) {
    case AL_SOURCE_STATE: v = int32_t(StateOf(*s)); break;
    case AL_SOURCE_TYPE: v = int32_t(s->type); break;
    case AL_BUFFER: v = int32_t(s->buffer); break;
    case AL_LOOPING: v = s->looping; break;
    case AL_SOURCE_RELATIVE: v = s->relative; break;
    case AL_BUFFERS_QUEUED: v = int32_t(s->queue.size()); break;
    case AL_BUFFERS_PROCESSED:
      if (s->looping && s->type == AL_STATIC) v = 0;
      else if (StateOf(*s) == AL_STOPPED || StateOf(*s) == AL_INITIAL)
        v = StateOf(*s) == AL_STOPPED ? int32_t(s->queue.size()) : 0;
      else v = s->stream ? int32_t(std::min(s->stream->cur, s->queue.size())) : 0;
      break;
    case AL_SAMPLE_OFFSET:
      v = s->stream && (s->state == AL_PLAYING || s->state == AL_PAUSED)
              ? int32_t(s->voice.Position())
              : 0;
      break;
    default: FatalF("alGetSourcei(0x%X) not modelled", p);
  }
  if (Arg(c, 2)) Wr32(Arg(c, 2), uint32_t(v));
  return 0;
}
HLE(kDll, alGetSourcef, kCdecl) {
  std::lock_guard<std::mutex> l(g_mu);
  AlSource* s = Src(uint32_t(Arg(c, 0)));
  if (!s) return 0;
  std::lock_guard<std::mutex> ml(audio::GetMixer().mu());
  const uint32_t p = uint32_t(Arg(c, 1));
  float v;
  switch (p) {
    case AL_GAIN: v = s->gain; break;
    case AL_PITCH: v = s->pitch; break;
    case AL_MIN_GAIN: v = s->min_gain; break;
    case AL_MAX_GAIN: v = s->max_gain; break;
    case AL_REFERENCE_DISTANCE: v = s->ref; break;
    case AL_ROLLOFF_FACTOR: v = s->rolloff; break;
    case AL_MAX_DISTANCE: v = s->max_dist; break;
    case AL_CONE_INNER_ANGLE: v = s->cone_inner; break;
    case AL_CONE_OUTER_ANGLE: v = s->cone_outer; break;
    case AL_CONE_OUTER_GAIN: v = s->cone_outer_gain; break;
    case AL_SEC_OFFSET:
      v = s->stream && (s->state == AL_PLAYING || s->state == AL_PAUSED)
              ? float(s->voice.Position()) / float(s->stream->rate())
              : 0.0f;
      break;
    default: FatalF("alGetSourcef(0x%X) not modelled", p);
  }
  if (Arg(c, 2)) std::memcpy(GPtr(Arg(c, 2)), &v, 4);
  return 0;
}

HLE(kDll, alSourcePlay, kCdecl) {
  std::lock_guard<std::mutex> l(g_mu);
  AlSource* s = Src(uint32_t(Arg(c, 0)));
  if (!s) return 0;
  audio::Mixer& m = audio::GetMixer();
  std::lock_guard<std::mutex> ml(m.mu());
  const uint32_t st = StateOf(*s);
  if (!s->stream || s->queue.empty()) {  // nothing to play: it stops at once
    s->state = AL_STOPPED;
    return 0;
  }
  if (st != AL_PAUSED) s->voice.Seek(0);  // from the start
  Update(*s);
  s->voice.loops = 1;  // looping is the stream's
  s->voice.eos = false;
  s->voice.state = audio::Voice::State::Playing;
  if (!s->in_mixer) {
    m.Add(&s->voice);
    s->in_mixer = true;
  }
  s->state = AL_PLAYING;
  return 0;
}
HLE(kDll, alSourcePause, kCdecl) {
  std::lock_guard<std::mutex> l(g_mu);
  AlSource* s = Src(uint32_t(Arg(c, 0)));
  if (!s) return 0;
  std::lock_guard<std::mutex> ml(audio::GetMixer().mu());
  if (StateOf(*s) == AL_PLAYING) {
    s->voice.state = audio::Voice::State::Paused;
    s->state = AL_PAUSED;
  }
  return 0;
}
HLE(kDll, alSourceStop, kCdecl) {
  std::lock_guard<std::mutex> l(g_mu);
  AlSource* s = Src(uint32_t(Arg(c, 0)));
  if (!s) return 0;
  audio::Mixer& m = audio::GetMixer();
  std::lock_guard<std::mutex> ml(m.mu());
  if (StateOf(*s) != AL_INITIAL) Stop(*s, m);
  return 0;
}
// alSourceQueueBuffers(source, n, buffers): appended to a streaming
// source's queue (same format as what is queued).
HLE(kDll, alSourceQueueBuffers, kCdecl) {
  std::lock_guard<std::mutex> l(g_mu);
  AlSource* s = Src(uint32_t(Arg(c, 0)));
  if (!s) return 0;
  if (s->type == AL_STATIC) {
    Error(AL_INVALID_OPERATION);
    return 0;
  }
  const int32_t n = int32_t(Arg(c, 1));
  std::vector<std::pair<uint32_t, BufferPtr>> add;
  for (int32_t i = 0; i < n; i++) {
    const uint32_t id = Rd32(Arg(c, 2) + 4 * i);
    auto it = g_buffers.find(id);
    if (it == g_buffers.end()) {
      Error(AL_INVALID_NAME);
      return 0;
    }
    const BufferPtr& first = s->queue.empty() ? (add.empty() ? it->second : add.front().second)
                                              : s->queue.front().second;
    if (first->channels != it->second->channels || first->bits != it->second->bits ||
        first->rate != it->second->rate) {
      Error(AL_INVALID_VALUE);
      return 0;
    }
    add.push_back({id, it->second});
  }
  std::lock_guard<std::mutex> ml(audio::GetMixer().mu());
  const bool was_empty = !s->stream || s->queue.empty();
  for (auto& b : add) s->queue.push_back(b);
  s->type = AL_STREAMING;
  if (was_empty) {
    Rebuild(*s);
    Update(*s);
  } else {
    for (auto& [bn, b] : add) s->stream->queue.push_back(b);
  }
  return 0;
}
// alSourceUnqueueBuffers(source, n, buffers): the first n buffers, which
// must have been processed, out of the queue (their names to `buffers`).
HLE(kDll, alSourceUnqueueBuffers, kCdecl) {
  std::lock_guard<std::mutex> l(g_mu);
  AlSource* s = Src(uint32_t(Arg(c, 0)));
  if (!s) return 0;
  const int32_t n = int32_t(Arg(c, 1));
  std::lock_guard<std::mutex> ml(audio::GetMixer().mu());
  const uint32_t st = StateOf(*s);
  const size_t processed = st == AL_STOPPED ? s->queue.size()
                           : st == AL_INITIAL ? 0
                           : s->stream ? std::min(s->stream->cur, s->queue.size())
                                       : 0;
  if (n < 0 || size_t(n) > processed || s->type != AL_STREAMING) {
    Error(AL_INVALID_VALUE);
    return 0;
  }
  for (int32_t i = 0; i < n; i++) {
    Wr32(Arg(c, 2) + 4 * i, s->queue.front().first);
    s->queue.pop_front();
    if (s->stream && !s->stream->queue.empty()) {
      s->stream->queue.pop_front();
      if (s->stream->cur) s->stream->cur--;
    }
  }
  if (s->queue.empty()) s->type = AL_UNDETERMINED;
  return 0;
}

}  // namespace recomp::hle
