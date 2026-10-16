#include "NetworkCardImport.h"

namespace netprofile {

CardRead readCard(CardFiles& files, const ValidateOptions& options, const int64_t at) {
  CardRead read;
  if (!files.available()) {
    read.outcome = CardOutcome::NoCard;
    return read;
  }
  const int64_t size = files.inboxSize();
  if (size < 0) {
    read.outcome = CardOutcome::NoFile;
    return read;
  }

  if (size == 0) {
    read.error = Error::BadJson;
  } else if (static_cast<uint64_t>(size) > MAX_BUNDLE_BYTES) {
    read.error = Error::TooLarge;  // not read at all
  } else {
    SecretBytes text;
    if (!text.allocate(static_cast<size_t>(size)) || !files.readInbox(text.data(), text.size())) {
      read.error = Error::StorageFailed;
    } else {
      ValidateOptions cardOptions = options;
      cardOptions.source = Source::Card;
      read.error = parseCardBundle(text.view(), cardOptions, read.profile);
    }
  }
  // Whatever it held and whatever happens next, the file goes now.
  read.wiped = files.wipeInbox();

  if (read.error == Error::None) {
    read.pending = true;  // the person decides; the result file is written then
    // Until then the last import's result must not stand for this one: it says "pending" (same schema, a result word an
    // older reader of the file does not know). A failed write is reported by resultWritten and is not fatal.
    read.resultWritten = files.writeResult(importPendingJson(at, &read.profile.info));
    return read;
  }
  read.outcome = CardOutcome::Rejected;
  read.resultWritten =
      files.writeResult(importResultJson(at, read.error, read.profile.described ? &read.profile.info : nullptr));
  return read;
}

CardResult finishCard(CardFiles& files, Store& store, CardRead& read, const bool install, const int64_t at) {
  CardResult result;
  result.wiped = read.wiped;
  result.error = install ? store.install(read.profile) : Error::Declined;
  result.outcome = result.error == Error::None ? CardOutcome::Installed : CardOutcome::Rejected;
  if (read.profile.described) {
    result.profile = read.profile.info;
    result.profile.installed = result.error == Error::None;
  }
  result.resultWritten =
      files.writeResult(importResultJson(at, result.error, read.profile.described ? &read.profile.info : nullptr));
  read.pending = false;
  read.profile = Profile();  // the PSK goes with it
  return result;
}

CardResult importFromCard(CardFiles& files, Store& store, const ValidateOptions& options, const int64_t at) {
  CardRead read = readCard(files, options, at);
  if (read.pending) return finishCard(files, store, read, true, at);
  CardResult result;
  result.outcome = read.outcome;
  result.error = read.error;
  result.wiped = read.wiped;
  result.resultWritten = read.resultWritten;
  if (read.profile.described) {
    result.profile = read.profile.info;
    result.profile.installed = false;
  }
  return result;
}

}  // namespace netprofile
