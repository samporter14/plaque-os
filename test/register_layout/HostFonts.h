#pragma once

#include <cstdint>
#include <string>

// The reader's real built-in fonts, measured the way GfxRenderer::getTextAdvanceX measures them (ligatures, kerning,
// differential rounding), so a layout checked here fits the panel. The roles are the ones RegisterProvider.cpp maps to
// fonts on the device (the notes and the footer are in the reader's UI face, Ubuntu 10).
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#include "EpdFont/builtinFonts/notosans_14_bold.h"
#include "EpdFont/builtinFonts/notosans_14_regular.h"
#include "EpdFont/builtinFonts/notosans_8_regular.h"
#include "EpdFont/builtinFonts/notoserif_18_bold.h"
#include "EpdFont/builtinFonts/notoserif_18_regular.h"
#include "EpdFont/builtinFonts/ubuntu_10_bold.h"
#include "EpdFont/builtinFonts/ubuntu_10_regular.h"
#pragma GCC diagnostic pop
#include "EpdFont/EpdFont.h"
#include "EpdFont/EpdFontFamily.h"
#include "RegisterLayout.h"
#include "Utf8.h"

namespace hostfonts {

// Letter-spacing of the overline, in pixels (drawText's tracking argument).
constexpr int OVERLINE_TRACKING = 2;

struct Face {
  const EpdFontFamily* family;
  EpdFontFamily::Style style;
  int tracking;
};

class Fonts final : public reg::TextMeter {
 public:
  Fonts()
      : serifRegular(&notoserif_18_regular),
        serifBold(&notoserif_18_bold),
        sans14Regular(&notosans_14_regular),
        sans14Bold(&notosans_14_bold),
        ui10Regular(&ubuntu_10_regular),
        ui10Bold(&ubuntu_10_bold),
        small(&notosans_8_regular),
        serif(&serifRegular, &serifBold),
        sans14(&sans14Regular, &sans14Bold),
        ui10(&ui10Regular, &ui10Bold),
        smallFamily(&small) {}

  Face face(const reg::FontRole role) const {
    using reg::FontRole;
    switch (role) {
      case FontRole::Name:
      case FontRole::State:
        return {&serif, EpdFontFamily::BOLD, 0};
      case FontRole::Detail:
      case FontRole::Row:
        return {&sans14, EpdFontFamily::REGULAR, 0};
      case FontRole::RowBold:
        return {&sans14, EpdFontFamily::BOLD, 0};
      case FontRole::Note:
        return {&ui10, EpdFontFamily::REGULAR, 0};
      case FontRole::NoteBold:
        return {&ui10, EpdFontFamily::BOLD, 0};
      case FontRole::Overline:
        return {&smallFamily, EpdFontFamily::REGULAR, OVERLINE_TRACKING};
    }
    return {&sans14, EpdFontFamily::REGULAR, 0};
  }

  int width(const reg::FontRole role, const std::string& text) const override {
    const Face f = face(role);
    const char* p = text.c_str();
    uint32_t cp;
    uint32_t prevCp = 0;
    int widthPx = 0;
    int32_t prevAdvanceFP = 0;
    const auto isSpace = [](const uint32_t c) { return c == ' ' || c == 0xA0 || c == 0x3000; };
    while ((cp = utf8NextCodepoint(reinterpret_cast<const unsigned char**>(&p)))) {
      cp = f.family->applyLigatures(cp, p, f.style);
      if (prevCp != 0) {
        const int8_t kernFP = f.family->getKerning(prevCp, cp, f.style);
        const int tracking = isSpace(prevCp) || isSpace(cp) ? 0 : f.tracking;
        widthPx += fp4::toPixel(prevAdvanceFP + kernFP) + tracking;
      }
      const EpdGlyph* glyph = f.family->getGlyph(cp, f.style);
      prevAdvanceFP = glyph ? glyph->advanceX : 0;
      prevCp = cp;
    }
    widthPx += fp4::toPixel(prevAdvanceFP);
    return widthPx;
  }

  int lineHeight(const reg::FontRole role) const override {
    return face(role).family->getData(EpdFontFamily::REGULAR)->advanceY;
  }

  // True when the face has a glyph of its own for the code point (not the replacement box).
  bool covers(const reg::FontRole role, const uint32_t cp) const {
    const Face f = face(role);
    return f.family->hasCodepoint(cp, f.style);
  }

 private:
  EpdFont serifRegular, serifBold, sans14Regular, sans14Bold, ui10Regular, ui10Bold, small;
  EpdFontFamily serif, sans14, ui10, smallFamily;
};

}  // namespace hostfonts
