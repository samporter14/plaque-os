#include "MethodLibrary.h"

#include <ArduinoJson.h>

#include <algorithm>
#include <cstdio>

#include "MethodTimers.h"

namespace method {
namespace {
constexpr size_t SHA_SUFFIX_CHARS = 8;
constexpr char EXTENSION[] = ".xtc";

// A title is shown and compared: control characters become spaces and the ends are trimmed.
std::string cleanTitle(std::string_view raw) {
  std::string title(raw);
  for (char& c : title)
    if (static_cast<unsigned char>(c) < 0x20) c = ' ';
  const size_t first = title.find_first_not_of(' ');
  if (first == std::string::npos) return {};
  const size_t last = title.find_last_not_of(' ');
  return title.substr(first, last - first + 1);
}

// "/Lamina/Method/PCR setup 1a2b3c4d.xtc" -> "PCR setup 1a2b3c4d"
std::string fileStem(std::string_view path) {
  const size_t slash = path.find_last_of('/');
  std::string_view name = slash == std::string_view::npos ? path : path.substr(slash + 1);
  const std::string_view extension(EXTENSION);
  if (name.size() >= extension.size() && name.substr(name.size() - extension.size()) == extension)
    name.remove_suffix(extension.size());
  return std::string(name);
}

// Files the reader downloads are named "<title> <8 hex>"; the hex part is not part of the title.
std::string stripShaSuffix(std::string stem) {
  if (stem.size() <= SHA_SUFFIX_CHARS + 1 || stem[stem.size() - SHA_SUFFIX_CHARS - 1] != ' ') return stem;
  const std::string_view tail = std::string_view(stem).substr(stem.size() - SHA_SUFFIX_CHARS);
  const bool hex = std::all_of(tail.begin(), tail.end(),
                               [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
  if (hex) stem.resize(stem.size() - SHA_SUFFIX_CHARS - 1);
  return stem;
}

void appendJsonString(std::string& out, std::string_view text) {
  out += '"';
  for (const char c : text) {
    const auto byte = static_cast<unsigned char>(c);
    if (c == '"') {
      out += "\\\"";
    } else if (c == '\\') {
      out += "\\\\";
    } else if (byte < 0x20) {
      char escape[8];
      snprintf(escape, sizeof(escape), "\\u%04x", byte);
      out += escape;
    } else {
      out += c;
    }
  }
  out += '"';
}

void appendNumber(std::string& out, int64_t value) {
  char digits[24];
  snprintf(digits, sizeof(digits), "%lld", static_cast<long long>(value));
  out += digits;
}

bool validTitleBytes(const std::string& title) { return !title.empty() && title.size() <= MAX_TITLE_BYTES; }

bool validEntryBookId(std::string_view id) { return id.empty() || validBookId(id); }

// One index entry as a JSON object, with the same rules whether it comes from the whole file or from one line of it.
// False when the entry is to be dropped. Timers are read last and never cost the entry: a set that does not validate
// is simply absent, and so is any set on a book the mailbox did not send or past `allowTimers`.
bool entryFromJson(JsonVariantConst item, Entry& entry, const bool allowTimers) {
  if (!item.is<JsonObjectConst>()) return false;
  // Read string members as const char* (see PersistableStore): std::string members drag in more JSON code.
  const char* path = item["path"] | "";
  const char* title = item["title"] | "";
  const char* sha = item["sha256"] | "";
  const char* bookId = item["book_id"] | "";
  entry = Entry();
  entry.path = path;
  // The path is later opened and deleted: only a plain book file in the Method folder qualifies.
  if (!ownedBookPath(entry.path)) return false;
  entry.title = cleanTitle(title);
  if (entry.title.empty()) entry.title = stripShaSuffix(fileStem(entry.path));
  entry.sha256 = sha;
  entry.bookId = bookId;
  // A missing or unknown kind reads as a method: that is what every older index and found book is.
  const char* kind = item["kind"] | "";
  if (!parseKind(kind, entry.kind)) entry.kind = Kind::Method;
  if (!validTitleBytes(entry.title) || (!entry.sha256.empty() && !validSha256Hex(entry.sha256)) ||
      !validEntryBookId(entry.bookId))
    return false;
  if (!item["page_count"].is<unsigned>()) return false;
  const unsigned pages = item["page_count"].as<unsigned>();
  if (pages < 1 || pages > UINT16_MAX) return false;
  entry.pageCount = pages;
  entry.receivedAt = item["received_at"].is<unsigned>() ? item["received_at"].as<unsigned>() : 0;
  entry.opened = item["opened"].is<bool>() && item["opened"].as<bool>();
  if (allowTimers && !entry.sha256.empty() && !item["timers"].isUnbound() &&
      !parseTimerArray(item["timers"], entry.pageCount, entry.timers))
    entry.timers.clear();
  return true;
}
}  // namespace

const Entry* Library::find(std::string_view path) const {
  const int index = indexOf(path);
  return index < 0 ? nullptr : &entries_[static_cast<size_t>(index)];
}

int Library::indexOf(std::string_view path) const {
  for (size_t i = 0; i < entries_.size(); ++i)
    if (entries_[i].path == path) return static_cast<int>(i);
  return -1;
}

size_t Library::insertSorted(Entry entry) {
  size_t position = 0;
  while (position < entries_.size() && entries_[position].receivedAt > entry.receivedAt) ++position;
  entries_.insert(entries_.begin() + static_cast<std::ptrdiff_t>(position), std::move(entry));
  return position;
}

bool Library::parse(std::string_view json) {
  entries_.clear();
  mark_ = 0;
  outOfMemory_ = false;
  if (json.empty() || json.size() > MAX_INDEX_BYTES) return false;
  JsonDocument doc;
  const DeserializationError error =
      deserializeJson(doc, json.data(), json.size(), DeserializationOption::NestingLimit(7));
  if (error == DeserializationError::NoMemory) outOfMemory_ = true;  // not a verdict on the text
  if (error || !doc.is<JsonObject>()) return false;

  size_t withTimers = 0;
  for (JsonVariantConst item : doc["methods"].as<JsonArrayConst>()) {
    Entry entry;
    if (!entryFromJson(item, entry, withTimers < MAX_METHODS)) continue;
    if (!entry.timers.empty()) ++withTimers;
    // First occurrence wins; the file may name the same path or title twice after a hand edit.
    const bool duplicate = std::any_of(entries_.begin(), entries_.end(), [&](const Entry& e) {
      return e.path == entry.path || e.title == entry.title;
    });
    if (!duplicate) entries_.push_back(std::move(entry));
  }
  std::stable_sort(entries_.begin(), entries_.end(),
                   [](const Entry& a, const Entry& b) { return a.receivedAt > b.receivedAt; });
  if (entries_.size() > MAX_INDEXED) entries_.resize(MAX_INDEXED);

  if (doc["mark"].is<unsigned>()) mark_ = doc["mark"].as<unsigned>();
  return true;
}

std::string Library::serialise() const {
  std::string out;
  out.reserve(256 + entries_.size() * 320);
  out += "{\"version\":1,\"mark\":";
  appendNumber(out, mark_);
  out += ",\"methods\":[";
  for (size_t i = 0; i < entries_.size(); ++i) {
    const Entry& entry = entries_[i];
    out += i == 0 ? "\n" : ",\n";
    out += "{\"title\":";
    appendJsonString(out, entry.title);
    out += ",\"path\":";
    appendJsonString(out, entry.path);
    out += ",\"sha256\":";
    appendJsonString(out, entry.sha256);
    out += ",\"book_id\":";
    appendJsonString(out, entry.bookId);
    out += ",\"kind\":";
    appendJsonString(out, kindName(entry.kind));
    out += ",\"received_at\":";
    appendNumber(out, entry.receivedAt);
    out += ",\"page_count\":";
    appendNumber(out, entry.pageCount);
    out += ",\"opened\":";
    out += entry.opened ? "true" : "false";
    if (!entry.timers.empty() && !entry.sha256.empty()) {
      std::string timers;
      appendTimersJson(timers, entry.timers);
      // Never write more than a reader will accept back (parse would drop the set, so the rest of the line is kept).
      if (timers.size() <= MAX_TIMERS_JSON_BYTES) {
        out += ",\"timers\":";
        out += timers;
      }
    }
    out += '}';
  }
  out += entries_.empty() ? "]}\n" : "\n]}\n";
  return out;
}

std::vector<std::string> Library::add(Entry entry) {
  std::vector<std::string> garbage;
  // Same title: this is a new version, the old one goes (after the index is written). That includes a book copied by
  // hand: a send with the same title is an explicit new version, so replacing it is intended. (The prune below never
  // takes a hand-copied book; this is the owner sending a newer one.)
  for (size_t i = 0; i < entries_.size();) {
    if (entries_[i].title == entry.title || entries_[i].path == entry.path) {
      if (entries_[i].path != entry.path) garbage.push_back(entries_[i].path);
      entries_.erase(entries_.begin() + static_cast<std::ptrdiff_t>(i));
    } else {
      ++i;
    }
  }
  const std::string added = entry.path;
  insertSorted(std::move(entry));
  while (entries_.size() > MAX_METHODS) {
    // The oldest book the reader downloaded itself, never the one that just arrived. A book without a SHA-256 was
    // found on the card (copied there by hand): it is the owner's, and no check deletes it.
    size_t victim = entries_.size();
    for (size_t i = entries_.size(); i-- > 0;) {
      if (entries_[i].path != added && !entries_[i].sha256.empty()) {
        victim = i;
        break;
      }
    }
    if (victim == entries_.size()) break;  // only hand-copied books left: keep them all
    garbage.push_back(entries_[victim].path);
    entries_.erase(entries_.begin() + static_cast<std::ptrdiff_t>(victim));
  }
  return garbage;
}

std::vector<std::string> Library::remove(std::string_view path) {
  const int index = indexOf(path);
  if (index < 0) return {};
  std::vector<std::string> garbage{entries_[static_cast<size_t>(index)].path};
  entries_.erase(entries_.begin() + index);
  return garbage;
}

Library::RemoveResult Library::removeAndCommit(FilePort& files, const std::string_view path) {
  const Library before = *this;
  const std::vector<std::string> garbage = remove(path);
  if (garbage.empty()) return RemoveResult::NotListed;
  if (!commit(files, garbage)) {
    *this = before;
    return RemoveResult::WriteFailed;
  }
  return RemoveResult::Removed;
}

bool Library::markOpened(std::string_view path) {
  const int index = indexOf(path);
  if (index < 0 || entries_[static_cast<size_t>(index)].opened) return false;
  entries_[static_cast<size_t>(index)].opened = true;
  return true;
}

bool Library::hasVersion(std::string_view sha256, std::string_view path) const {
  return std::any_of(entries_.begin(), entries_.end(), [&](const Entry& e) {
    return e.path == path || (!sha256.empty() && e.sha256 == sha256);
  });
}

IndexScanner::IndexScanner(std::string_view path) {
  // An owned path has only letters, digits, space, '-', '_', '.' and '/', so it is written in the file as it is.
  if (ownedBookPath(path)) {
    path_ = std::string(path);
    needle_ = "\"path\":\"" + path_ + "\"";
  }
}

bool IndexScanner::feed(std::string_view chunk) {
  constexpr size_t MAX_LINE = MAX_ENTRY_BYTES + MAX_ENTRY_TIMER_BYTES + 16;
  while (!found_ && !needle_.empty() && !chunk.empty()) {
    const size_t newline = chunk.find('\n');
    const std::string_view part = chunk.substr(0, newline);
    if (!tooLong_) {
      if (line_.size() + part.size() > MAX_LINE) {
        tooLong_ = true;
        line_.clear();
      } else {
        line_.append(part);
      }
    }
    if (newline == std::string_view::npos) break;  // the line goes on in the next piece
    endLine();
    chunk.remove_prefix(newline + 1);
  }
  return found_;
}

bool IndexScanner::finish() {
  if (!found_ && !needle_.empty()) endLine();
  return found_;
}

void IndexScanner::endLine() {
  const bool candidate = !tooLong_ && line_.find(needle_) != std::string::npos;
  if (candidate) {
    std::string_view text(line_);
    while (!text.empty() && (text.back() == ',' || text.back() == ' ' || text.back() == '\r')) text.remove_suffix(1);
    JsonDocument doc;
    Entry entry;
    if (!deserializeJson(doc, text.data(), text.size(), DeserializationOption::NestingLimit(6)) &&
        entryFromJson(doc.as<JsonVariantConst>(), entry, true) && entry.path == path_) {
      entry_ = std::move(entry);
      found_ = true;
    }
  }
  line_.clear();
  tooLong_ = false;
}

bool Library::updateTimers(std::string_view path, std::string_view sha256, const std::vector<Timer>& timers) {
  for (Entry& entry : entries_) {
    if (entry.path != path && (sha256.empty() || entry.sha256 != sha256)) continue;
    if (entry.sha256.empty() || entry.timers == timers) return false;
    entry.timers = timers;
    return true;
  }
  return false;
}

Kind indexedKind(const std::string_view indexJson, const std::string_view path) {
  Library library;
  if (!library.parse(indexJson)) return Kind::Method;
  const Entry* entry = library.find(path);
  return entry ? entry->kind : Kind::Method;
}

bool parseLegacyState(std::string_view json, LegacyBook& out) {
  if (json.empty() || json.size() > MAX_INDEX_BYTES) return false;
  JsonDocument doc;
  if (deserializeJson(doc, json.data(), json.size(), DeserializationOption::NestingLimit(3)) ||
      !doc.is<JsonObjectConst>())
    return false;
  const char* id = doc["book_id"] | "";
  const char* hash = doc["sha256"] | "";
  const char* file = doc["path"] | "";
  const char* name = doc["title"] | "";
  // A state file naming anything outside the Method folder is ignored whole.
  uint32_t number = 0;
  if (!validBookId(id) || !bookIdNumber(id, number) || !validSha256Hex(hash) || !ownedBookPath(file)) return false;
  out.bookId = id;
  out.idNumber = number;
  out.sha256 = hash;
  out.path = file;
  const std::string title = cleanTitle(name);
  out.title = title.empty() ? stripShaSuffix(fileStem(file)) : title.substr(0, utf8Prefix(title, MAX_TITLE_BYTES));
  return true;
}

Library::LoadReport Library::load(FilePort& files) {
  LoadReport report;
  entries_.clear();
  mark_ = 0;
  readOnly_ = false;
  bool dirty = false;

  files.remove(INCOMING_PATH);  // an interrupted download never counts

  // index.json, with recovery from an interrupted replace (the old file is removed before the
  // temp file is renamed, so a power cut in that window leaves only the complete temp file).
  // readAll() is false for an empty file as well as for a missing or unreadable one (the card library refuses a
  // zero-byte file): an empty file is not unreadable, it is nothing, and fileSize() says which.
  std::string text;
  enum class Read { Ok, Empty, Unreadable };
  const auto read = [&](const char* path) {
    if (files.readAll(path, MAX_INDEX_BYTES, text)) return Read::Ok;
    return files.fileSize(path) == 0 ? Read::Empty : Read::Unreadable;
  };
  if (!files.exists(INDEX_PATH) && files.exists(INDEX_TMP_PATH)) {
    switch (read(INDEX_TMP_PATH)) {
      case Read::Ok:
        if (parse(text)) {
          // Put the complete temp file in place by renaming it, not by writing it again: until the rename it is the
          // only copy, and a write that failed would have destroyed it. If the rename fails it stays, and nothing is
          // written this visit.
          report.recovered = true;
          if (files.replaceFile(INDEX_TMP_PATH, INDEX_PATH)) {
            report.saved = true;
          } else {
            readOnly_ = true;
          }
        } else if (outOfMemory_) {
          // The heap could not hold the parse: that says nothing about the file, which may be the only copy.
          report.indexUnreadable = true;
          readOnly_ = true;
        } else {
          files.remove(INDEX_TMP_PATH);  // a torn temp file is worth nothing
        }
        break;
      case Read::Empty:
        files.remove(INDEX_TMP_PATH);
        break;
      case Read::Unreadable:
        // It may be the only copy: leave it alone, and write nothing.
        report.indexUnreadable = true;
        readOnly_ = true;
        break;
    }
  } else if (files.exists(INDEX_PATH)) {
    const Read outcome = read(INDEX_PATH);
    if (outcome == Read::Unreadable) {
      // It is there but would not read (a card error, or larger than any index this reader writes). That says
      // nothing about its contents: show what the folder holds, and write nothing over it.
      report.indexUnreadable = true;
      readOnly_ = true;
    } else if (outcome == Read::Ok && parse(text)) {
      report.indexRead = true;
    } else if (outcome == Read::Ok && outOfMemory_) {
      // The heap could not hold the parse (an index with many timers on a busy heap). The file is fine: leave it,
      // show what the folder holds, and write nothing over it.
      report.indexUnreadable = true;
      readOnly_ = true;
    } else {
      // Empty, or not an index. Keep it where it can be looked at, then rebuild from the folder below.
      report.indexDamaged = true;
      entries_.clear();
      mark_ = 0;
      if (files.replaceFile(INDEX_PATH, INDEX_BAD_PATH)) {
        dirty = true;
      } else {
        readOnly_ = true;
      }
    }
  }
  if (!readOnly_) files.remove(INDEX_TMP_PATH);

  // The Stage-0 state file named one book. Import it, and remove the file once the index holds it.
  bool legacyPending = false;
  if (files.exists(LEGACY_STATE_PATH)) {
    legacyPending = true;
    LegacyBook legacy;
    BookInfo info;
    if (files.readAll(LEGACY_STATE_PATH, MAX_INDEX_BYTES, text) && parseLegacyState(text, legacy) &&
        files.exists(legacy.path.c_str()) && !find(legacy.path) && files.inspectBook(legacy.path.c_str(), info) &&
        info.parsed && info.pageCount > 0 &&
        std::none_of(entries_.begin(), entries_.end(), [&](const Entry& e) { return e.title == legacy.title; })) {
      Entry entry;
      entry.title = legacy.title;
      entry.path = legacy.path;
      entry.sha256 = legacy.sha256;
      entry.bookId = legacy.bookId;
      entry.receivedAt = files.modifiedAt(legacy.path.c_str());
      entry.pageCount = info.pageCount;
      insertSorted(std::move(entry));
      advanceMark(legacy.idNumber);  // that book came from the mailbox: its id has been dealt with
      report.migrated = true;
      dirty = true;
    }
  }

  // Drop entries whose book is gone, and the reading cache they left behind.
  for (size_t i = 0; i < entries_.size();) {
    if (files.exists(entries_[i].path.c_str())) {
      ++i;
      continue;
    }
    files.removeBookCache(entries_[i].path.c_str());
    entries_.erase(entries_.begin() + static_cast<std::ptrdiff_t>(i));
    ++report.dropped;
    dirty = true;
  }

  // Adopt .xtc files nobody indexed (copied over USB, or left by an interrupted index write).
  // Only plain names the reader itself could have written, only into free slots, and never a
  // second book under a title already on the shelf. Nothing here deletes a file.
  if (entries_.size() < MAX_METHODS) {
    struct Candidate {
      std::string path;
      int64_t modified;
    };
    std::vector<Candidate> candidates;
    for (const std::string& name : files.listBooks(DIRECTORY)) {
      std::string path = std::string(DIRECTORY_PREFIX) + name;
      if (!ownedBookPath(path) || find(path)) continue;
      candidates.push_back({path, files.modifiedAt(path.c_str())});
    }
    std::stable_sort(candidates.begin(), candidates.end(),
                     [](const Candidate& a, const Candidate& b) { return a.modified > b.modified; });
    for (const Candidate& candidate : candidates) {
      if (entries_.size() >= MAX_METHODS) break;
      BookInfo info;
      if (!files.inspectBook(candidate.path.c_str(), info) || !info.parsed || info.pageCount == 0) continue;
      const std::string stem = fileStem(candidate.path);
      // The XTC parser falls back to the file name when a book has no title of its own.
      std::string title = cleanTitle(info.title == stem ? stripShaSuffix(stem) : info.title);
      if (title.empty()) title = stripShaSuffix(stem);
      if (title.size() > MAX_TITLE_BYTES) title.resize(utf8Prefix(title, MAX_TITLE_BYTES));
      if (std::any_of(entries_.begin(), entries_.end(), [&](const Entry& e) { return e.title == title; })) continue;
      Entry entry;
      entry.title = std::move(title);
      entry.path = candidate.path;
      entry.receivedAt = candidate.modified;
      entry.pageCount = info.pageCount;
      insertSorted(std::move(entry));
      ++report.adopted;
      dirty = true;
    }
  }

  // A book with saved progress has been opened, wherever it was opened from.
  for (Entry& entry : entries_) {
    if (!entry.opened && files.savedPage(entry.path.c_str()) >= 0) {
      entry.opened = true;
      ++report.opened;
      dirty = true;
    }
  }

  if (dirty && !readOnly_) report.saved = save(files);
  if (legacyPending && (!report.migrated || report.saved)) files.remove(LEGACY_STATE_PATH);
  return report;
}

bool Library::save(FilePort& files) const {
  if (readOnly_ || !files.ensureDirectory(DIRECTORY)) return false;
  const std::string text = serialise();
  if (!files.writeFile(INDEX_TMP_PATH, text)) {
    files.remove(INDEX_TMP_PATH);  // a torn temp file is worth nothing
    return false;
  }
  if (!files.replaceFile(INDEX_TMP_PATH, INDEX_PATH)) {
    // The old index is gone if the replace got as far as the rename: then the temp file is the only complete copy,
    // and load() recovers from it.
    if (files.exists(INDEX_PATH)) files.remove(INDEX_TMP_PATH);
    return false;
  }
  return true;
}

bool Library::commit(FilePort& files, const std::vector<std::string>& garbage) const {
  if (!save(files)) return false;
  for (const std::string& path : garbage)
    if (ownedBookPath(path) && !find(path)) files.removeBook(path.c_str());
  return true;
}

}  // namespace method
