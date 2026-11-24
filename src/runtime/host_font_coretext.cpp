// host_font.h on CoreText (macOS, iOS). Other hosts: none yet.
//
// Metrics follow GDI's TrueType rules: the cell (tmHeight) is the OS/2
// table's usWinAscent + usWinDescent scaled to the em, internal leading
// is the cell minus the em, external leading what hhea's line gap adds
// beyond the cell. A LOGFONT height > 0 asks for that cell height, < 0
// for that em. Fonts without an OS/2 table (some Apple-only faces) use
// CoreText's ascent and descent instead.

#include "runtime/host_font.h"

#if defined(__APPLE__)

#include <CoreFoundation/CoreFoundation.h>
#include <CoreGraphics/CoreGraphics.h>
#include <CoreText/CoreText.h>

#include <algorithm>
#include <cmath>
#include <mutex>
#include <set>

#include "runtime/log.h"

namespace recomp::host_font {
namespace {

struct Table {
  CFDataRef data = nullptr;
  explicit Table(CTFontRef f, CTFontTableTag tag)
      : data(CTFontCopyTable(f, tag, kCTFontTableOptionNoOptions)) {}
  ~Table() {
    if (data) CFRelease(data);
  }
  size_t size() const { return data ? size_t(CFDataGetLength(data)) : 0; }
  uint16_t U16(size_t o) const {
    if (o + 2 > size()) return 0;
    const uint8_t* p = CFDataGetBytePtr(data) + o;
    return uint16_t(p[0] << 8 | p[1]);
  }
  int16_t I16(size_t o) const { return int16_t(U16(o)); }
};

CFStringRef CfString(const std::u16string& s) {
  return CFStringCreateWithCharacters(nullptr, reinterpret_cast<const UniChar*>(s.data()),
                                      CFIndex(s.size()));
}

std::u16string FromCf(CFStringRef s) {
  if (!s) return {};
  std::u16string r(size_t(CFStringGetLength(s)), u'\0');
  CFStringGetCharacters(s, CFRangeMake(0, CFIndex(r.size())), reinterpret_cast<UniChar*>(r.data()));
  return r;
}

bool SameName(const std::u16string& a, const std::u16string& b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); i++) {
    char16_t x = a[i], y = b[i];
    if (x >= u'A' && x <= u'Z') x = char16_t(x + 32);
    if (y >= u'A' && y <= u'Z') y = char16_t(y + 32);
    if (x != y) return false;
  }
  return true;
}

class CtFont final : public Font {
 public:
  CtFont(CTFontRef f, Metrics m, bool aa) : font_(f), metrics_(std::move(m)), aa_(aa) {}
  ~CtFont() override { CFRelease(font_); }

  const Metrics& metrics() const override { return metrics_; }

  uint16_t Glyph(const char16_t* s, size_t n) const override {
    CGGlyph g[2] = {0, 0};
    n = std::min<size_t>(n, 2);
    if (!CTFontGetGlyphsForCharacters(font_, reinterpret_cast<const UniChar*>(s), g, CFIndex(n)))
      return 0;
    return g[0];
  }

  void Glyphs(const char16_t* s, size_t n, uint16_t* out) const override {
    std::vector<CGGlyph> g(n, 0);
    CTFontGetGlyphsForCharacters(font_, reinterpret_cast<const UniChar*>(s), g.data(), CFIndex(n));
    for (size_t i = 0; i < n; i++) out[i] = g[i];
  }

  GlyphMetrics Measure(uint16_t glyph) const override {
    const CGGlyph g = glyph;
    CGRect r;
    CTFontGetBoundingRectsForGlyphs(font_, kCTFontOrientationHorizontal, &g, &r, 1);
    CGSize adv;
    CTFontGetAdvancesForGlyphs(font_, kCTFontOrientationHorizontal, &g, &adv, 1);
    GlyphMetrics m;
    m.advance = int32_t(std::lround(adv.width));
    if (r.size.width <= 0 || r.size.height <= 0) return m;
    const double x0 = std::floor(r.origin.x), x1 = std::ceil(r.origin.x + r.size.width);
    const double y0 = std::floor(r.origin.y), y1 = std::ceil(r.origin.y + r.size.height);
    m.box_x = int32_t(x0);
    m.box_y = int32_t(y1);
    m.box_w = uint32_t(x1 - x0);
    m.box_h = uint32_t(y1 - y0);
    return m;
  }

  std::vector<uint8_t> Render(uint16_t glyph, const GlyphMetrics& m) const override {
    std::vector<uint8_t> px(size_t(m.box_w) * m.box_h, 0);
    if (px.empty()) return px;
    CGColorSpaceRef gray = CGColorSpaceCreateDeviceGray();
    CGContextRef cg = CGBitmapContextCreate(px.data(), m.box_w, m.box_h, 8, m.box_w, gray,
                                            kCGImageAlphaNone);
    CGColorSpaceRelease(gray);
    if (!cg) FatalF("host_font: CGBitmapContextCreate %ux%u failed", m.box_w, m.box_h);
    CGContextSetGrayFillColor(cg, 1.0, 1.0);
    CGContextSetShouldAntialias(cg, aa_);
    CGContextSetShouldSmoothFonts(cg, false);  // no macOS stem darkening
    CGContextSetShouldSubpixelPositionFonts(cg, false);
    CGContextSetShouldSubpixelQuantizeFonts(cg, false);
    // The box's bottom-left corner is the bitmap's origin; the bitmap's
    // first row in memory is its top.
    const CGGlyph g = glyph;
    const CGPoint at = CGPointMake(-double(m.box_x), -(double(m.box_y) - double(m.box_h)));
    CTFontDrawGlyphs(font_, &g, &at, 1, cg);
    CGContextRelease(cg);
    return px;
  }

 private:
  CTFontRef font_;
  Metrics metrics_;
  bool aa_;
};

}  // namespace

bool Available() { return true; }

std::shared_ptr<const Font> Create(const Desc& d) {
  const std::u16string want = d.face.empty() ? u"Arial" : d.face;
  CFStringRef name = CfString(want);
  // GDI matches family names: a descriptor for the family first (one
  // registered for the process included), then CoreText's name lookup.
  CTFontRef base = nullptr;
  {
    const void* keys[] = {kCTFontFamilyNameAttribute};
    const void* vals[] = {name};
    CFDictionaryRef attrs = CFDictionaryCreate(nullptr, keys, vals, 1, &kCFTypeDictionaryKeyCallBacks,
                                               &kCFTypeDictionaryValueCallBacks);
    CTFontDescriptorRef desc = CTFontDescriptorCreateWithAttributes(attrs);
    CFRelease(attrs);
    const void* mkeys[] = {kCTFontFamilyNameAttribute};
    CFSetRef mandatory = CFSetCreate(nullptr, mkeys, 1, &kCFTypeSetCallBacks);
    if (CTFontDescriptorRef match = CTFontDescriptorCreateMatchingFontDescriptor(desc, mandatory)) {
      base = CTFontCreateWithFontDescriptor(match, 16.0, nullptr);
      CFRelease(match);
    }
    CFRelease(mandatory);
    CFRelease(desc);
  }
  if (!base) base = CTFontCreateWithName(name, 16.0, nullptr);
  CFRelease(name);
  if (!base) FatalF("host_font: CTFontCreateWithName failed");

  CTFontSymbolicTraits traits = 0;
  if (d.weight >= 600) traits |= kCTFontTraitBold;
  if (d.italic) traits |= kCTFontTraitItalic;
  if (traits) {
    if (CTFontRef styled = CTFontCreateCopyWithSymbolicTraits(base, 0, nullptr, traits, traits)) {
      CFRelease(base);
      base = styled;
    }
  }

  CFStringRef family = CTFontCopyFamilyName(base);
  const std::u16string used = FromCf(family);
  if (family) CFRelease(family);
  if (!SameName(used, want)) {
    static std::mutex mu;
    static std::set<std::u16string> told;
    std::lock_guard<std::mutex> l(mu);
    if (told.insert(want).second) {
      std::string a(want.begin(), want.end()), b(used.begin(), used.end());
      WarnF("host_font: no face \"%s\" on this host; using \"%s\"\n", a.c_str(), b.c_str());
    }
  }

  Table head(base, kCTFontTableHead), hhea(base, kCTFontTableHhea), os2(base, kCTFontTableOS2);
  const double upem = head.size() >= 20 && head.U16(18) ? head.U16(18) : CTFontGetUnitsPerEm(base);
  double asc_u, desc_u;
  const bool have_os2 = os2.size() >= 78;
  if (have_os2) {
    asc_u = os2.U16(74);
    desc_u = os2.U16(76);
  } else {
    asc_u = CTFontGetAscent(base) * upem / 16.0;
    desc_u = CTFontGetDescent(base) * upem / 16.0;
  }
  double em;
  if (d.height < 0) em = -d.height;
  else if (d.height > 0) em = d.height * upem / (asc_u + desc_u);
  else em = 16.0;  // GDI's default height (12 pt at 96 dpi)
  const double k = em / upem;

  CTFontRef f = CTFontCreateCopyWithAttributes(base, em, nullptr, nullptr);
  CFRelease(base);
  if (!f) FatalF("host_font: CTFontCreateCopyWithAttributes failed");

  Metrics m{};
  m.ascent = int32_t(std::lround(asc_u * k));
  m.descent = int32_t(std::lround(desc_u * k));
  m.height = m.ascent + m.descent;
  m.internal_leading = std::max(0, m.height - int32_t(std::lround(em)));
  if (hhea.size() >= 12) {
    const double gap = hhea.I16(8), hasc = hhea.I16(4), hdesc = hhea.I16(6);
    m.external_leading =
        std::max(0, int32_t(std::lround((gap - ((asc_u + desc_u) - (hasc - hdesc))) * k)));
    m.max_char_width = int32_t(std::lround(hhea.U16(10) * k));
  }
  m.weight = have_os2 ? os2.U16(4) : (d.weight ? d.weight : 400);
  m.ave_char_width = have_os2 ? int32_t(std::lround(os2.I16(2) * k)) : 0;
  if (!m.ave_char_width) {
    const UniChar x = 'x';
    CGGlyph g;
    CGSize a{};
    if (CTFontGetGlyphsForCharacters(f, &x, &g, 1))
      CTFontGetAdvancesForGlyphs(f, kCTFontOrientationHorizontal, &g, &a, 1);
    m.ave_char_width = int32_t(std::lround(a.width));
  }
  m.first_char = have_os2 && os2.size() >= 68 ? os2.U16(64) : 0x20;
  m.last_char = have_os2 && os2.size() >= 68 ? os2.U16(66) : 0xFFFC;
  const bool v2 = have_os2 && os2.U16(0) >= 2 && os2.size() >= 94;
  m.default_char = v2 && os2.U16(90) ? os2.U16(90) : m.first_char;
  m.break_char = v2 && os2.U16(92) ? os2.U16(92) : 0x20;
  const CTFontSymbolicTraits t = CTFontGetSymbolicTraits(f);
  m.italic = (t & kCTFontTraitItalic) != 0;
  m.fixed_pitch = (t & kCTFontTraitMonoSpace) != 0;
  m.face = used;
  return std::make_shared<CtFont>(f, std::move(m), d.antialias);
}

namespace {
CFURLRef FileUrl(const std::string& path) {
  return CFURLCreateFromFileSystemRepresentation(nullptr, reinterpret_cast<const UInt8*>(path.data()),
                                                 CFIndex(path.size()), false);
}
}  // namespace

int RegisterFile(const std::string& host_path) {
  CFURLRef url = FileUrl(host_path);
  if (!url) return 0;
  int faces = 0;
  if (CFArrayRef descs = CTFontManagerCreateFontDescriptorsFromURL(url)) {
    faces = int(CFArrayGetCount(descs));
    CFRelease(descs);
  }
  if (faces) {
    CFErrorRef err = nullptr;
    if (!CTFontManagerRegisterFontsForURL(url, kCTFontManagerScopeProcess, &err)) {
      // Registered already (by an earlier call): still available.
      const bool already = err && CFErrorGetCode(err) == kCTFontManagerErrorAlreadyRegistered;
      if (!already) faces = 0;
    }
    if (err) CFRelease(err);
  }
  CFRelease(url);
  return faces;
}

bool UnregisterFile(const std::string& host_path) {
  CFURLRef url = FileUrl(host_path);
  if (!url) return false;
  const bool ok = CTFontManagerUnregisterFontsForURL(url, kCTFontManagerScopeProcess, nullptr);
  CFRelease(url);
  return ok;
}

}  // namespace recomp::host_font

#else  // !__APPLE__

#include "runtime/log.h"

namespace recomp::host_font {
bool Available() { return false; }
std::shared_ptr<const Font> Create(const Desc&) {
  FatalF("host_font: no font engine on this host yet (GDI text needs one)");
}
int RegisterFile(const std::string&) {
  FatalF("host_font: no font engine on this host yet (AddFontResourceEx needs one)");
}
bool UnregisterFile(const std::string&) { return false; }
}  // namespace recomp::host_font

#endif
