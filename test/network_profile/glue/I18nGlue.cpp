#include <I18n.h>

#include <cstddef>

#include "I18nStrings.h"

I18n& I18n::getInstance() {
  static I18n instance;
  return instance;
}

const char* I18n::get(const StrId id) const {
  const auto index = static_cast<size_t>(id);
  if (index >= static_cast<size_t>(StrId::_COUNT)) return "???";
  return i18n_strings::STRINGS_EN_DATA + i18n_strings::OFFSETS_EN[index];
}
