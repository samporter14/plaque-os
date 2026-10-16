#pragma once

#include <HalStorage.h>

#include <string>

#include "ambient/MethodLibrary.h"

// What the library index says about one book, read without loading the index: the reader opens a book with only its
// path, and an index with timers can be tens of KB on a heap the reader has little of. The file is read in small pieces
// and only the book's own line is parsed (method::IndexScanner).
namespace method {

inline bool readIndexedEntry(const std::string& path, Entry& out) {
  IndexScanner scanner(path);
  HalFile file;
  if (!Storage.openFileForRead("MINDEX", INDEX_PATH, file)) return false;
  char piece[512];
  while (file.available() > 0) {
    const int got = file.read(piece, sizeof(piece));
    if (got <= 0) break;
    if (scanner.feed(std::string_view(piece, static_cast<size_t>(got)))) break;
  }
  scanner.finish();
  if (!scanner.found()) return false;
  out = scanner.entry();
  return true;
}

}  // namespace method
