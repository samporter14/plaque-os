#pragma once
// I18n.h for the glue tests: the English strings, from the tables gen_i18n.py writes into the build directory, so the
// text the screens build (NetworkText.h) is the text the reader shows. The real I18n.h reads the generated files that
// sit in lib/I18n, which are only as new as the last firmware build.
#include "I18nKeys.h"

class I18n {
 public:
  static I18n& getInstance();
  const char* get(StrId id) const;
};

#define tr(id) I18n::getInstance().get(StrId::id)
#define I18N I18n::getInstance()
