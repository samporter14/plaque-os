#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>

#include "network/FirmwareBoardTag.h"

// Host tests for the board tag the SD card firmware installer checks (src/network/FirmwareBoardTag.*): built once as
// the X4 Pro firmware and once as the X4 Classic's (EXPECTED_BOARD / OTHER_BOARD), each must accept an image tagged for
// itself and refuse one tagged for the other board, however the image is cut into chunks.

#ifndef EXPECTED_BOARD
#error "EXPECTED_BOARD must name the board this test is built as"
#endif
#ifndef OTHER_BOARD
#error "OTHER_BOARD must name the other Plaque board"
#endif

namespace {
int checks = 0;

void check(const bool condition, const std::string& name) {
  if (!condition) {
    std::cerr << "FAIL [" << EXPECTED_BOARD << "]: " << name << '\n';
    std::exit(1);
  }
  ++checks;
}

std::string tagFor(const char* board) { return std::string("CROSSPOINT-BOARD-V1:") + board + ";"; }

// An image: some code bytes, the tag somewhere in .rodata, more bytes after it.
std::string imageWith(const std::string& tag) {
  std::string image(3000, '\0');
  for (size_t i = 0; i < image.size(); ++i) image[i] = static_cast<char>((i * 37 + 11) & 0xFF);
  image.insert(1700, tag);
  return image;
}

bool refuses(const std::string& image, const size_t chunk) {
  board_tag::Scanner scanner;
  for (size_t at = 0; at < image.size(); at += chunk) {
    const size_t len = std::min(chunk, image.size() - at);
    scanner.feed(reinterpret_cast<const uint8_t*>(image.data() + at), len);
  }
  return scanner.mismatch();
}
}  // namespace

int main() {
  const std::string own = std::string(board_tag::boardName(), board_tag::boardNameLen());
  check(own == EXPECTED_BOARD, "the firmware is tagged for its own board");
  check(std::string(board_tag::TAG) == tagFor(EXPECTED_BOARD), "the whole tag");

  for (const size_t chunk : {size_t{1}, size_t{7}, size_t{24}, size_t{4096}}) {
    const std::string label = " (chunks of " + std::to_string(chunk) + ")";
    check(!refuses(imageWith(tagFor(EXPECTED_BOARD)), chunk), "an image for this board is accepted" + label);
    check(refuses(imageWith(tagFor(OTHER_BOARD)), chunk), "an image for the other Plaque board is refused" + label);
    check(refuses(imageWith(tagFor("x4")), chunk), "an image for the C3 X4 is refused" + label);
    check(!refuses(imageWith(""), chunk), "an untagged image is left to the other checks" + label);
  }

  board_tag::Scanner scanner;
  const std::string other = imageWith(tagFor(OTHER_BOARD));
  scanner.feed(reinterpret_cast<const uint8_t*>(other.data()), other.size());
  check(std::strcmp(scanner.foundName(), OTHER_BOARD) == 0, "the refusal names the board the image was built for");

  // A board name that only starts like this one (x4c is a prefix of neither, but x4 is of both) is still another board.
  check(refuses(imageWith(tagFor(EXPECTED_BOARD "x")), 4096), "a longer name is another board");

  std::cout << "firmware board tag (" << EXPECTED_BOARD << "): " << checks << " checks passed\n";
  return 0;
}
