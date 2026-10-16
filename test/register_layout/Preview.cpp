// Draws the Register screens the way the panel gets them: the layout from RegisterLayout.cpp, the glyph bitmaps from the
// reader's built-in fonts (decompressed as FontDecompressor does), every non-white pixel of a 2-bit glyph painted black
// as GfxRenderer does in black-and-white mode. Writes one binary PGM per screen; a tiny script turns them into PNGs.
//
//   RegisterPreview <output directory>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "HostFonts.h"
#include "InflateReader.h"
#include "RegisterLayout.h"

// tinflate.c names the checksum routines the firmware never links; nothing here asks for a checksum.
extern "C" {
unsigned int uzlib_adler32(const void*, unsigned int, unsigned int) { return 0; }
unsigned int uzlib_crc32(const void*, unsigned int, unsigned int) { return 0; }
}

namespace {
struct Canvas {
  std::vector<uint8_t> pixels = std::vector<uint8_t>(reg::PANEL_W * reg::PANEL_H, 255);
  void black(int x, int y) {
    if (x >= 0 && y >= 0 && x < reg::PANEL_W && y < reg::PANEL_H) pixels[static_cast<size_t>(y) * reg::PANEL_W + x] = 0;
  }
  void fill(int x, int y, int w, int h) {
    for (int j = 0; j < h; ++j)
      for (int i = 0; i < w; ++i) black(x + i, y + j);
  }
};

// One glyph's 2-bit bitmap, packed (rows contiguous, four pixels a byte, MSB first).
class Glyphs {
 public:
  const uint8_t* bitmap(const EpdFontData* font, const EpdGlyph* glyph) {
    if (!font->groups || font->groupCount == 0) return &font->bitmap[glyph->dataOffset];
    const uint32_t index = static_cast<uint32_t>(glyph - font->glyph);
    const uint16_t group = groupOf(font, index);
    const auto key = std::make_pair(font, group);
    auto it = groups_.find(key);
    if (it == groups_.end()) {
      const EpdFontGroup& g = font->groups[group];
      std::vector<uint8_t> data(g.uncompressedSize);
      InflateReader reader;
      reader.init(false);
      reader.setSource(&font->bitmap[g.compressedOffset], g.compressedSize);
      if (!reader.read(data.data(), data.size())) {
        std::fprintf(stderr, "could not inflate group %u\n", group);
        std::exit(1);
      }
      it = groups_.emplace(key, std::move(data)).first;
    }
    const uint32_t offset = alignedOffset(font, group, index);
    packed_.assign(static_cast<size_t>(((glyph->width + 3) / 4) * glyph->height) + 1, 0);
    compact(&it->second[offset], packed_.data(), glyph->width, glyph->height);
    return packed_.data();
  }

 private:
  static uint16_t groupOf(const EpdFontData* font, uint32_t index) {
    if (font->glyphToGroup) return font->glyphToGroup[index];
    for (uint16_t i = 0; i < font->groupCount; ++i)
      if (index >= font->groups[i].firstGlyphIndex && index < font->groups[i].firstGlyphIndex + font->groups[i].glyphCount) return i;
    return font->groupCount;
  }
  static uint32_t alignedOffset(const EpdFontData* font, uint16_t group, uint32_t index) {
    uint32_t offset = 0;
    const auto add = [&](const EpdGlyph& g) {
      if (g.width > 0 && g.height > 0) offset += ((g.width + 3) / 4) * g.height;
    };
    if (font->glyphToGroup) {
      for (uint32_t i = 0; i < index; ++i)
        if (font->glyphToGroup[i] == group) add(font->glyph[i]);
    } else {
      for (uint32_t i = font->groups[group].firstGlyphIndex; i < index; ++i) add(font->glyph[i]);
    }
    return offset;
  }
  static void compact(const uint8_t* src, uint8_t* dst, uint8_t width, uint8_t height) {
    if (width == 0 || height == 0) return;
    const uint32_t stride = (width + 3) / 4;
    uint8_t out = 0, bits = 0;
    uint32_t write = 0;
    for (uint8_t y = 0; y < height; ++y)
      for (uint8_t x = 0; x < width; ++x) {
        out = static_cast<uint8_t>((out << 2) | ((src[y * stride + x / 4] >> ((3 - (x % 4)) * 2)) & 0x3));
        bits += 2;
        if (bits == 8) {
          dst[write++] = out;
          out = 0;
          bits = 0;
        }
      }
    if (bits > 0) dst[write] = static_cast<uint8_t>(out << (8 - bits));
  }
  std::map<std::pair<const EpdFontData*, uint16_t>, std::vector<uint8_t>> groups_;
  std::vector<uint8_t> packed_;
};

// GfxRenderer::drawText for the plain left-to-right case.
void drawText(Canvas& canvas, Glyphs& glyphs, const hostfonts::Fonts& fonts, const reg::FontRole role, int x, int y, const std::string& text) {
  const hostfonts::Face face = fonts.face(role);
  const EpdFontData* regular = face.family->getData(EpdFontFamily::REGULAR);
  const int baseline = y + regular->ascender;
  const char* p = text.c_str();
  uint32_t cp;
  uint32_t prev = 0;
  int32_t prevAdvance = 0;
  int cursor = x;
  const auto isSpace = [](const uint32_t c) { return c == ' ' || c == 0xA0 || c == 0x3000; };
  while ((cp = utf8NextCodepoint(reinterpret_cast<const unsigned char**>(&p)))) {
    cp = face.family->applyLigatures(cp, p, face.style);
    if (prev != 0) {
      const int8_t kern = face.family->getKerning(prev, cp, face.style);
      cursor += fp4::toPixel(prevAdvance + kern) + (isSpace(prev) || isSpace(cp) ? 0 : face.tracking);
    }
    const EpdGlyph* glyph = face.family->getGlyph(cp, face.style);
    const EpdFontData* data = face.family->getData(face.style);
    if (glyph && glyph->width && glyph->height) {
      const uint8_t* bitmap = glyphs.bitmap(data, glyph);
      for (int gy = 0; gy < glyph->height; ++gy)
        for (int gx = 0; gx < glyph->width; ++gx) {
          const int pos = gy * glyph->width + gx;
          // 2-bit glyphs: any non-white value is ink. 1-bit glyphs (the small and UI faces) are plain bits, uncompressed.
          const int value = data->is2Bit ? (bitmap[pos >> 2] >> (6 - (pos & 3) * 2)) & 3 : (bitmap[pos >> 3] >> (7 - (pos & 7))) & 1;
          if (value) canvas.black(cursor + glyph->left + gx, baseline - glyph->top + gy);
        }
    }
    prevAdvance = glyph ? glyph->advanceX : 0;
    prev = cp;
  }
}

void render(const reg::Layout& layout, const hostfonts::Fonts& fonts, const std::string& path) {
  Canvas canvas;
  Glyphs glyphs;
  for (const reg::Item& item : layout.items) {
    switch (item.kind) {
      case reg::Item::Kind::Text:
        drawText(canvas, glyphs, fonts, item.role, item.x, item.y, item.text);
        break;
      case reg::Item::Kind::Rule:
        canvas.fill(item.x, item.y, item.w, item.thickness);
        break;
      case reg::Item::Kind::Frame:
        canvas.fill(item.x, item.y, item.w, item.thickness);
        canvas.fill(item.x, item.y + item.h - item.thickness, item.w, item.thickness);
        canvas.fill(item.x, item.y, item.thickness, item.h);
        canvas.fill(item.x + item.w - item.thickness, item.y, item.thickness, item.h);
        break;
    }
  }
  std::ofstream out(path, std::ios::binary);
  out << "P5\n" << reg::PANEL_W << " " << reg::PANEL_H << "\n255\n";
  out.write(reinterpret_cast<const char*>(canvas.pixels.data()), static_cast<std::streamsize>(canvas.pixels.size()));
}

int64_t at(int year, int month, int day, int hour, int minute = 0) {
  struct tm parts = {};
  parts.tm_year = year - 1900;
  parts.tm_mon = month - 1;
  parts.tm_mday = day;
  parts.tm_hour = hour;
  parts.tm_min = minute;
  parts.tm_isdst = -1;
  return static_cast<int64_t>(mktime(&parts));
}

reg::Entry book(const char* title, int64_t start, int64_t end) { return {title, start, end, false}; }

// A Wednesday at a confocal: a morning, an afternoon and a Thursday.
reg::Schedule confocal() {
  reg::Schedule s;
  s.instrument = "Zeiss LSM 880 Confocal";
  s.timezone = "America/Detroit";
  s.status = reg::Status::Ok;
  s.fetchedAt = at(2026, 10, 14, 14, 5);
  s.today = {book("Ana", at(2026, 10, 14, 13), at(2026, 10, 14, 15)), book("Sam", at(2026, 10, 14, 16), at(2026, 10, 14, 18)),
             book("Priya (training)", at(2026, 10, 14, 18, 30), at(2026, 10, 14, 20))};
  s.tomorrow = {book("Lee", at(2026, 10, 15, 9), at(2026, 10, 15, 11)), book("Maintenance", at(2026, 10, 15, 13), at(2026, 10, 15, 14, 30))};
  s.hasNow = true;
  s.now = s.today[0];
  s.hasNext = true;
  s.next = s.today[1];
  return s;
}
}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: RegisterPreview <output directory>\n");
    return 1;
  }
  const std::string dir = argv[1];
  setenv("TZ", "EST5EDT,M3.2.0,M11.1.0", 1);
  tzset();
  const hostfonts::Fonts fonts;
  const reg::Words words;
  reg::Options options;
  options.twelveHour = true;
  options.pollMinutes = 15;

  const auto show = [&](const std::string& name, const reg::Schedule& s, int64_t when, const reg::Options& o) {
    const reg::Model model = reg::buildModel(s, reg::viewTime(when), o, words);
    render(reg::layoutScreen(model, words, fonts), fonts, dir + "/" + name + ".pgm");
  };

  // The three states the brief names.
  {
    reg::Schedule free = confocal();  // between Ana's booking and Sam's: free until 4:00 PM
    free.fetchedAt = at(2026, 10, 14, 15, 15);
    free.hasNow = false;
    show("free", free, at(2026, 10, 14, 15, 20), options);
    show("inuse", confocal(), at(2026, 10, 14, 14, 10), options);
    reg::Schedule unread = confocal();  // nothing in progress, but the Worker could not read a repeating booking
    unread.fetchedAt = at(2026, 10, 14, 15, 15);
    unread.hasNow = false;
    unread.note = "1 repeating booking couldn't be read.";
    show("free-unread", unread, at(2026, 10, 14, 15, 20), options);
    reg::Schedule stale = confocal();
    stale.status = reg::Status::Stale;
    stale.note = "1 repeating booking couldn't be read";
    show("stale", stale, at(2026, 10, 14, 14, 10), options);
  }
  // Others.
  {
    reg::Schedule nothing = confocal();
    nothing.hasNow = nothing.hasNext = false;
    nothing.today.clear();
    nothing.tomorrow.clear();
    show("free-nothing-booked", nothing, at(2026, 10, 14, 14, 10), options);
    show("inuse-24h", confocal(), at(2026, 10, 14, 14, 10), [&] { reg::Options o = options; o.twelveHour = false; return o; }());
    reg::Schedule allDay = confocal();
    allDay.hasNow = true;
    allDay.now = book("Annual service", at(2026, 10, 14, 0), at(2026, 10, 15, 0));
    allDay.now.allDay = true;
    allDay.today = {allDay.now};
    allDay.hasNext = false;
    show("inuse-all-day", allDay, at(2026, 10, 14, 14, 10), options);
    reg::Schedule full = confocal();
    full.today.clear();
    full.tomorrow.clear();
    for (int i = 0; i < 20; ++i) {
      static const char* names[] = {"Ana", "Sam", "Lee", "Priya", "Marcus", "Dr. Okonkwo-Bell", "Jo", "Tomasz", "Wei"};
      full.today.push_back(book(names[i % 9], at(2026, 10, 14, 8) + i * 2700, at(2026, 10, 14, 8) + i * 2700 + 1800));
      full.tomorrow.push_back(book(names[(i + 3) % 9], at(2026, 10, 15, 6) + i * 2700, at(2026, 10, 15, 6) + i * 2700 + 1800));
    }
    full.fetchedAt = at(2026, 10, 14, 8, 5);
    full.hasNow = true;
    full.now = full.today[0];
    full.hasNext = true;
    full.next = full.today[1];
    show("busy-day", full, at(2026, 10, 14, 8, 10), options);
    reg::Schedule longNames = confocal();
    longNames.instrument = "Bruker Avance III HD 600 MHz NMR spectro";
    longNames.today[0].title = "Dr. Alexandra Featherstonehaugh-Smythe";
    longNames.now = longNames.today[0];
    longNames.today[1].title = "Overnight sample queue for the cryo group";
    longNames.next = longNames.today[1];
    show("long-names", longNames, at(2026, 10, 14, 14, 10), options);
    reg::Schedule never;
    never.instrument = "Zeiss LSM 880 Confocal";
    never.timezone = "America/Detroit";
    show("never-read", never, at(2026, 10, 14, 14, 10), options);
    reg::Options low = options;
    low.batteryLow = true;
    show("battery-low", confocal(), at(2026, 10, 14, 14, 10), low);
    show("clock-not-set", confocal(), 0, options);
    reg::Schedule old = confocal();
    show("stale-yesterday", old, at(2026, 10, 15, 9, 40), options);
    show("tomorrow-hidden", old, at(2026, 10, 15, 8, 0), [&] { reg::Options o = options; o.pollMinutes = 1440; return o; }());
  }
  // The full day: two pages of a busy day, the first and the last (the hints change at each end of the ring).
  {
    reg::Schedule busy = confocal();
    busy.today.clear();
    busy.tomorrow.clear();
    for (int i = 0; i < 12; ++i) {
      static const char* names[] = {"Ana", "Sam", "Lee", "Priya", "Marcus", "Dr. Okonkwo-Bell", "Jo", "Tomasz", "Wei"};
      busy.today.push_back(book(names[i % 9], at(2026, 10, 14, 8) + i * 2700, at(2026, 10, 14, 8) + i * 2700 + 1800));
      busy.tomorrow.push_back(book(names[(i + 3) % 9], at(2026, 10, 15, 6) + i * 2700, at(2026, 10, 15, 6) + i * 2700 + 1800));
    }
    busy.fetchedAt = at(2026, 10, 14, 8, 5);
    const reg::FullDay day = reg::buildFullDay(busy, reg::viewTime(at(2026, 10, 14, 8, 10)), options, words);
    const reg::Layout first = reg::layoutFullDay(day, words, fonts, 0);
    render(first, fonts, dir + "/fullday-p1.pgm");
    render(reg::layoutFullDay(day, words, fonts, first.pages - 1), fonts, dir + "/fullday-last.pgm");
    std::fprintf(stderr, "full day: %d pages\n", first.pages);
  }
  // A reader set to UTC (the default of a fresh one) showing a Detroit instrument, on the instrument's own clock.
  {
    reg::Schedule detroit = confocal();  // built under New York time: the same offsets as Detroit
    detroit.hasZone = true;
    detroit.zone.name = "America/Detroit";
    detroit.zone.spans = {{at(2026, 3, 8, 8), -14400}, {at(2026, 11, 1, 8), -18000}};
    const int64_t when = at(2026, 10, 14, 14, 10);
    setenv("TZ", "UTC0", 1);
    tzset();
    show("utc-reader-detroit", detroit, when, options);
    // A clamped booking: in use until further notice, from before 2020.
    reg::Schedule clamped = detroit;
    clamped.hasNow = true;
    clamped.now = {"Out of service", 1577836800, 1612396800, false, true};
    clamped.today = {clamped.now, clamped.today[1]};
    clamped.tomorrow.clear();
    show("clamped", clamped, when, options);
    // Kolkata, +5:30, on the same UTC reader.
    reg::Schedule kolkata;
    kolkata.instrument = "Bruker Avance NMR";
    kolkata.timezone = "Asia/Kolkata";
    kolkata.status = reg::Status::Ok;
    kolkata.hasZone = true;
    kolkata.zone.name = "Asia/Kolkata";
    kolkata.zone.spans = {{1767225600, 19800}};
    const int64_t base = 1792000000;  // 2026-10-14 17:46 UTC = 23:16 IST
    const int64_t midnight = base - ((base + 19800) % 86400);  // Kolkata's midnight before it
    kolkata.fetchedAt = midnight + 14 * 3600 + 300;
    kolkata.today = {book("Ana", midnight + 13 * 3600 + 1800, midnight + 15 * 3600 + 900), book("Sam", midnight + 16 * 3600, midnight + 18 * 3600)};
    kolkata.tomorrow = {book("Lee", midnight + 86400 + 9 * 3600 + 2700, midnight + 86400 + 11 * 3600)};
    kolkata.hasNow = true;
    kolkata.now = kolkata.today[0];
    kolkata.hasNext = true;
    kolkata.next = kolkata.today[1];
    show("kolkata", kolkata, midnight + 14 * 3600 + 600, options);
    setenv("TZ", "EST5EDT,M3.2.0,M11.1.0", 1);
    tzset();
  }
  return 0;
}
