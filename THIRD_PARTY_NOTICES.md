# Third-party notices

Plaque OS is MIT licensed (see [LICENSE](LICENSE) and [NOTICE](NOTICE)). It is built on CrossPoint Reader
and on the libraries below. This file lists every third-party component that is compiled into a firmware
image or shipped in this repository, with its version, its copyright holder and its licence. Where a
component's licence text is vendored in this repository or in a package this build pulls in, the notice below
was copied from it; nothing here is paraphrased from memory except where it says so.

## Read this first: firmware images are combined works

wolfSSL 5.7.2 is linked into every firmware image (`-DFREEINK_NET_WOLFSSL=1` and `-DFREEINK_CONTENT_WOLFSSL=1`
in `platformio.ini`). Its licence is the GNU General Public License, version 2 or (at your option) any later
version, or a commercial licence. No commercial licence has been bought. **A firmware binary built from this
repository is therefore a combined work distributed under the terms of GPL-2.0-or-later (together with the LGPL
and other licences below). The MIT licence of the Plaque OS and CrossPoint source code does not apply to the
combined binary.** The source of this repository stays MIT; the binary is the combination.

**This repository, at the commit or tag a binary was built from, with the library versions pinned in
`platformio.ini` and the build steps in `README.md` and `requirements-build.txt`, is the corresponding source of
that binary.** Anyone who passes on a binary must offer its recipients that same source (the simplest way is to
link the repository at the exact commit). The LGPL libraries can be replaced and the image rebuilt by editing
the pinned library versions in `platformio.ini`.

ESP-IDF, JPEGDEC and PNGdec are Apache-2.0. The FSF treats Apache-2.0 as compatible with GPL version 3 but not
with version 2 alone, so the careful course is to distribute an image under GPL-3.0-or-later (wolfSSL's "or any
later version" allows that). This is the maintainer's reading, not legal advice. The texts are in
[LICENSES/](LICENSES/): `GPL-2.0.txt`, `LGPL-2.1.txt`, `Apache-2.0.txt`.

## Summary

| Component | Version / source | Licence | Where |
|---|---|---|---|
| CrossPoint Reader | 1.6.5 (base commit 1f77b83 of `develop`) | MIT | this repository |
| FreeInk SDK | submodule `freeink-sdk` at bbd528c | MIT, with its NOTICE (OpenX4 MIT) | linked |
| wolfSSL (Arduino-wolfSSL) | 5.7.2 | GPL-2.0-or-later or commercial | linked |
| links2004/WebSockets | 2.7.3 | LGPL-2.1-or-later | linked |
| JPEGDEC | git 86282979224c8a32fd51e091ed5a35b0c699a52b | Apache-2.0 | linked |
| PNGdec | 1.1.6 (bundles zlib 1.2.11) | Apache-2.0; zlib licence | linked |
| ArduinoJson | 7.4.2 | MIT | linked |
| SdFat | 2.3.1 | MIT | linked |
| QRCode | 0.0.1 (ricmoo/QRCode) | MIT | linked |
| Arduino-ESP32 core | 3.3.11 | LGPL-2.1-or-later | linked |
| ESP-IDF | 5.5.5 (via pioarduino) | Apache-2.0 (plus its own third-party components) | linked |
| pioarduino platform-espressif32 | 55.03.311 | Apache-2.0 | build tool |
| expat | 2.7.3 (`lib/expat`) | MIT | compiled in |
| miniz | 3.1.2 (`lib/miniz/third_party`) | MIT, or the Unlicense | compiled in |
| uzlib | 2.9.8 (`lib/uzlib`) | zlib licence | compiled in |
| minibidi (from mintty) | `lib/MiniBidi` | MIT | compiled in |
| FreeType (in the SDK's FreeInkFont) | 2.13.3 | FreeType License (FTL) | linked |
| stb_truetype (in the SDK's FreeInkFont) | 1.26 | public domain, or MIT | linked |
| miniz (in the SDK's ContentProtection) | SDK copy | MIT | linked |
| Lucide icons (as generated 1-bit bitmaps) | via the SDK | ISC (portions MIT) | `src/components/icons` |
| jszip | 3.10.1 (`src/network/html/js/jszip.min.js`) | MIT (dual-licensed MIT or GPLv3; MIT chosen) | served by the web UI |
| Noto Sans, Serif, Sans Arabic, Sans Hebrew | `lib/EpdFont/builtinFonts/source` | SIL OFL 1.1 | font bitmaps in the image |
| OpenDyslexic | same | SIL OFL 1.1 (Reserved Font Name "OpenDyslexic") | font bitmaps in the image |
| Ubuntu | same | Ubuntu Font Licence 1.0 | font bitmaps in the image |
| Hyphenation tries | `lib/Epub/Epub/hyphenation/generated` | mixed, see below | compiled in |

## Libraries linked into the firmware

### wolfSSL 5.7.2 (GPL-2.0-or-later or commercial)

Copyright (C) 2006-2023 wolfSSL Inc. (the year in the headers of the pinned package). wolfSSL is free software;
you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the
Free Software Foundation; either version 2 of the License, or (at your option) any later version. See the
section above for what that means for firmware images, and `LICENSES/GPL-2.0.txt` for the text (it is the
`COPYING` file of the pinned package). The package's `library.json` says "GPL-2.0-only" while its `LICENSING`
file and every source header say "version 2 ... or any later version"; this repository relies on the source
headers. A commercial licence is available from wolfSSL Inc. (licensing@wolfssl.com).

The build patches the package with `scripts/patch_wolfssl.py`; the patches are part of the corresponding source.

### links2004/WebSockets 2.7.3 (LGPL-2.1-or-later)

Copyright (c) 2015 Markus Sattler. All rights reserved. Licensed under the GNU Lesser General Public License,
version 2.1 or (at your option) any later version. Text: `LICENSES/LGPL-2.1.txt`. To use a modified version,
change the pinned version (or point the library at your own copy) in `platformio.ini` and rebuild.

### Arduino-ESP32 3.3.11 (LGPL-2.1-or-later)

Copyright Espressif Systems (Shanghai) and the Arduino-ESP32 contributors. The pioarduino package's
`package.json` declares LGPL-2.1-or-later. The same relinking note applies: the exact package is fetched by
PlatformIO from the URLs pinned in the platform (55.03.311) and can be replaced.

### ESP-IDF 5.5.5 and the pioarduino platform (Apache-2.0)

Copyright Espressif Systems (Shanghai) CO LTD and the ESP-IDF contributors. Apache-2.0 (`LICENSES/Apache-2.0.txt`).
ESP-IDF includes components with their own licences (for example lwIP, FreeRTOS, mbedTLS, newlib); each carries
its licence inside the ESP-IDF distribution that PlatformIO downloads. The same applies to the Xtensa GCC
toolchain (GCC runtime library exception). pioarduino `platform-espressif32` 55.03.311 is Apache-2.0.

### JPEGDEC and PNGdec (Apache-2.0)

Copyright 2020 BitBank Software, Inc. All rights reserved (JPEGDEC; PNGdec carries 2020 and 2021 headers).
Licensed under the Apache License, Version 2.0 (`LICENSES/Apache-2.0.txt`). This repository patches JPEGDEC
(`scripts/jpegdec_patches`, `scripts/patch_jpegdec.py`).

PNGdec bundles zlib 1.2.11 (Copyright (C) 1995-2017 Jean-loup Gailly and Mark Adler), under the zlib licence
reproduced below.

### ArduinoJson, SdFat, QRCode, expat, miniz, minibidi (MIT)

- ArduinoJson 7.4.2: Copyright (c) 2014-2025 Benoit Blanchon.
- SdFat 2.3.1: Copyright (c) 2011-2025 Bill Greiman. Patched by `scripts/patch_sdfat.py` and
  `scripts/sdfat_patches`.
- QRCode 0.0.1: Copyright (c) 2017 Richard Moore.
- expat 2.7.3 (`lib/expat`): Copyright (c) 1997-2000 Thai Open Source Software Center Ltd; (c) 2000 Clark Cooper;
  (c) 2000-2005 Fred L. Drake, Jr.; (c) 2001-2002 Greg Stein; (c) 2002-2016 Karl Waclawek; (c) 2016-2025
  Sebastian Pipping; and the other contributors listed in `lib/expat/expat.h`.
- miniz 3.1.2 (`lib/miniz/third_party`): Copyright 2013-2014 RAD Game Tools and Valve Software; Copyright
  2010-2014 Rich Geldreich and Tenacious Software LLC. Dual-licensed MIT and the Unlicense; MIT is used here.
- minibidi (`lib/MiniBidi/minibidi.c`): original author Ahmad Khalifa (www.arabeyes.org, MIT licence per the file
  header); changes from mintty by Thomas Wolff. The file header states the licence but carries no separate
  licence text.

The MIT licence text, which applies to each item above with its own copyright line:

> Permission notice (MIT License):
>
> Permission is hereby granted, free of charge, to any person obtaining a copy
> of this software and associated documentation files (the "Software"), to deal
> in the Software without restriction, including without limitation the rights
> to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
> copies of the Software, and to permit persons to whom the Software is
> furnished to do so, subject to the following conditions:
>
> The above copyright notice and this permission notice shall be included in all
> copies or substantial portions of the Software.
>
> THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
> IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
> FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
> AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
> LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
> OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
> SOFTWARE.

### uzlib 2.9.8 and zlib 1.2.11 (zlib licence)

uzlib (`lib/uzlib`): Copyright (c) 2003 by Joergen Ibsen / Jibz; Copyright (c) 2014-2018 by Paul Sokolovsky.
zlib (inside PNGdec): Copyright (C) 1995-2017 Jean-loup Gailly and Mark Adler. The zlib text below is from zlib.h; uzlib's own headers (`lib/uzlib/src`) carry the same style of permission
notice with its two copyright lines.

```
This software is provided 'as-is', without any express or implied
warranty. In no event will the authors be held liable for any damages
arising from the use of this software.

Permission is granted to anyone to use this software for any purpose,
including commercial applications, and to alter it and redistribute it
freely, subject to the following restrictions:

1. The origin of this software must not be misrepresented; you must not
   claim that you wrote the original software. If you use this software
   in a product, an acknowledgment in the product documentation would be
   appreciated but is not required.
2. Altered source versions must be plainly marked as such, and must not be
   misrepresented as being the original software.
3. This notice may not be removed or altered from any source distribution.
```

### FreeInk SDK (MIT) and what it vendors

The SDK is the git submodule `freeink-sdk` (https://github.com/Free-Ink/freeink-sdk, pinned at bbd528c).
Its own notice, copied from `freeink-sdk/NOTICE`:

```
FreeInk SDK
Copyright (c) 2026 FreeInk

This product is licensed under the MIT License (see LICENSE).

-----------------------------------------------------------------------------
Third-party attribution
-----------------------------------------------------------------------------

This project is based in part on, and contains code derived from, the OpenX4
E-Paper Community SDK (https://github.com/open-x4-epaper/community-sdk) and its
community device ports (M5Stack PaperColor, Murphy M3, and the
community-sdk-de-link ESP32-S3 port). In particular, the SSD1677 and UC8253
panel initialization sequences and waveform LUTs are derived from that work.
Original e-paper driver authorship is credited to CidVonHighwind.

The upstream is provided under the MIT License, reproduced here as required by
its terms:

    MIT License

    Copyright (c) 2025 Open X4 E-Paper Contributors

    Permission is hereby granted, free of charge, to any person obtaining a copy
    of this software and associated documentation files (the "Software"), to deal
    in the Software without restriction, including without limitation the rights
    to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
    copies of the Software, and to permit persons to whom the Software is
    furnished to do so, subject to the following conditions:

    The above copyright notice and this permission notice shall be included in all
    copies or substantial portions of the Software.

    THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
    IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
    FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
    AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
    LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
    OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
    SOFTWARE.
```

The SDK's MIT licence is in `freeink-sdk/LICENSE`. Parts of the SDK that are compiled into the firmware vendor
more code:

- **FreeType 2.13.3** (FreeInkFont). FreeType is dual-licensed (the FreeType License, or GPLv2). This build uses
  it under the FreeType License (`freeink-sdk/libs/font/FreeInkFont/third_party/freetype/FTL.TXT`), which asks
  for this credit: *Portions of this software are copyright (c) 2024 The FreeType Project (www.freetype.org).
  All rights reserved.*
- **stb_truetype 1.26** (FreeInkFont): Sean Barrett / RAD Game Tools, public domain (or MIT, the file offers both).
- **miniz** (ContentProtection): Copyright 2013-2014 RAD Game Tools and Valve Software; Copyright 2010-2014 Rich
  Geldreich and Tenacious Software LLC; MIT. Text: `freeink-sdk/libs/book/ContentProtection/third_party/miniz/LICENSE`.
- **Lucide icons**: the 1-bit icon bitmaps in `src/components/icons` are generated from the Lucide SVG set
  (https://lucide.dev), which is ISC licensed with portions MIT (Feather, Cole Bemis). The Lucide licence text
  is not vendored here (it sits in a nested submodule of the SDK that is not checked out); read it at
  https://lucide.dev/license.

The SDK's `FreeInkBook` library (expat, pngle, tjpgd, libunibreak, miniz) is not used by this build.

## Web UI

`src/network/html/js/jszip.min.js` is JSZip v3.10.1, (c) 2009-2016 Stuart Knightley. It is dual licensed under
the MIT licence or GPLv3; **Plaque OS uses it under the MIT licence.** The file header names its pako dependency
(MIT). The header is kept in the file.

The Python sender in `sender-web` is a development demo and is not part of a firmware image. Its Python
dependencies (Flask, Pillow) are installed from `sender-web/requirements.txt` and not redistributed here. It
bundles Noto Sans (`sender-web/assets/NotoSans.ttf`, OFL 1.1, text in `sender-web/assets/OFL.txt`).

## Fonts

The built-in reader fonts are bitmap fonts generated from the font files under
`lib/EpdFont/builtinFonts/source`; the generated bitmaps are derivatives and stay under the fonts' licences.
Each family's licence text is next to its files.

- **Noto Sans, Noto Serif, Noto Sans Arabic, Noto Sans Hebrew.** Copyright 2022 The Noto Project Authors
  (https://github.com/notofonts). SIL Open Font License 1.1 (`OFL.txt` in each folder).
- **OpenDyslexic.** Copyright (c) 2019-07-29, Abbie Gonzalez (https://abbiecod.es), with Reserved Font Name
  OpenDyslexic. SIL Open Font License 1.1 (`OFL.txt`). Modified versions must not use the Reserved Font Name.
- **Ubuntu (Regular, Bold, Vietnamese).** Copyright 2010,2011 Canonical Ltd. Ubuntu Font Licence 1.0
  (`lib/EpdFont/builtinFonts/source/Ubuntu/UFL.txt`). Ubuntu and Canonical are registered trademarks of
  Canonical Ltd.

## Hyphenation

The hyphenation tries in `lib/Epub/Epub/hyphenation/generated` (en, fr, de, es, ru, it, uk, pl, pt, sv, fi) are
converted from the compiled pattern tries of the typst/hypher project (https://github.com/typst/hypher) by
`scripts/update_hyphenation.sh` and `scripts/generate_hyphenation_trie.py`. hypher's patterns derive from the
TeX hyphenation patterns of the hyph-utf8 project (https://github.com/hyphenation/tex-hyphen). **Those patterns
carry mixed per-language licences** (each pattern file states its own). The authoritative listing is the hyph-utf8 licence overview in
the tex-hyphen repository and the licence section of the hypher README. This repository has not audited each
language's pattern licence individually; if you redistribute the tries, read the upstream listing first. The
hyphenation test word lists in `test/hyphenation_eval/resources` are test data derived from the same sources.

## Test data

Test fixtures under `test/` are synthetic. The test certificates in `test/message_tls/fixtures` are generated by
`test/message_tls/generate_certificates.py` and contain no private keys.
