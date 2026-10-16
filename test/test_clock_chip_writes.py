"""Run with python3 test/test_clock_chip_writes.py

The reader's clock record (src/network/ClockProvenance*) judges every sync before anything believes it: the RTC chip is
its witness, so the chip is written only from a judged sync, and SNTP is only ever started through lib/TrustedTime, which
runs the installed profile's time servers first. The firmware has several callers of a sync (message_tls, main.cpp's wake
path, the Wi-Fi setup, Settings > Clock); this holds the wiring that makes them all alike:

  - the only code that calls HalClock::writeUtc() is ClockProvenanceDevice.cpp, and the only place the RTC is set is
    HalClock::writeUtc() itself;
  - the only code that starts SNTP (configTzTime, the sntp_/esp_sntp_ init and server calls) is lib/TrustedTime;
  - HalClock::syncFromNTP() syncs through trustedtime::beginSync()/syncCompleted() and hands the answer to the recorder
    main() installs, and main() installs both that recorder and TrustedTime's prepare hook, after clockprov::begin().
"""

from pathlib import Path
import re
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
from test_no_upstream_updates import read_c  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]


def sources():
    for base in ("src", "lib"):
        for path in sorted((ROOT / base).rglob("*")):
            if path.suffix in (".cpp", ".h", ".hpp", ".c"):
                yield path, read_c(path.read_text(errors="replace"))[0]


def problems():
    found = []
    for path, code in sources():
        rel = path.relative_to(ROOT).as_posix()
        if re.search(r"\bwriteUtc\s*\(", code) and rel not in (
            "src/network/ClockProvenanceDevice.cpp",
            "lib/hal/HalClock.cpp",
            "lib/hal/HalClock.h",
        ):
            found.append(f"{rel}: writes the RTC chip; only the clock record may (ClockProvenanceDevice.cpp)")
        if re.search(r"_sdkRtc\s*\.\s*set\s*\(", code) and rel != "lib/hal/HalClock.cpp":
            found.append(f"{rel}: sets the RTC outside HalClock::writeUtc()")
        if (
            re.search(r"\b(configTzTime|sntp_init|esp_sntp_init|sntp_setservername|esp_sntp_setservername)\s*\(", code)
            and not rel.startswith("lib/TrustedTime/")
        ):
            found.append(f"{rel}: starts or configures SNTP; only lib/TrustedTime may (it runs the profile's servers first)")

    hal = read_c((ROOT / "lib/hal/HalClock.cpp").read_text())[0]
    writes = len(re.findall(r"_sdkRtc\s*\.\s*set\s*\(", hal))
    if writes != 1:
        found.append(f"lib/hal/HalClock.cpp: expected the RTC to be set in exactly one place (writeUtc), found {writes}")
    sync = re.search(r"bool HalClock::syncFromNTP\(\)\s*\{.*", hal, re.S)
    body = sync.group(0) if sync else ""
    for needle in ("trustedtime::beginSync", "trustedtime::syncCompleted", "_recorder"):
        if needle not in body:
            found.append(f"lib/hal/HalClock.cpp: syncFromNTP() does not use {needle}")
    if re.search(r"_sdkRtc|writeUtc", body):
        found.append("lib/hal/HalClock.cpp: syncFromNTP() writes the chip itself")

    main = read_c((ROOT / "src/main.cpp").read_text())[0]
    order = [main.find(s) for s in ("trustedtime::init()", "clockprov::begin()", "setSyncPrepareHook(", "setSyncRecorder(")]
    if min(order) < 0 or order != sorted(order):
        found.append("src/main.cpp: expected trustedtime::init(), clockprov::begin(), setSyncPrepareHook(), setSyncRecorder() in that order")
    if "netprofile::applyTimeSettings" not in main[main.find("setSyncPrepareHook(") : main.find("setSyncPrepareHook(") + 120]:
        found.append("src/main.cpp: the prepare hook is not netprofile::applyTimeSettings")

    # The recovery (Reset the clock check) and the clock Register reads: three wirings the host cannot compile.
    settings = read_c((ROOT / "src/activities/settings/SettingsActivity.cpp").read_text())[0]
    clock_row = re.search(r"if\s*\((.*?)\)\s*\{\s*systemSettings\.push_back\(SettingInfo::Action\(StrId::STR_CLOCK,", settings)
    if not clock_row or "offersClockReset" not in clock_row.group(1):
        found.append("SettingsActivity.cpp: the Clock row (and so Reset the clock check) must not depend on the RTC probe alone")
    device = read_c((ROOT / "src/network/ClockProvenanceDevice.cpp").read_text())[0]
    reset = re.search(r"void resetClockCheck\(\)\s*\{.*?\n\}", device, re.S)
    if not reset or "message_tls::withdrawClock()" not in reset.group(0) or "trustedtime::resetFloor()" not in reset.group(0):
        found.append("ClockProvenanceDevice.cpp: resetClockCheck() must reset the floor and withdraw the HTTPS permission")
    provider = read_c((ROOT / "src/ambient/RegisterProvider.cpp").read_text())[0]
    now = re.search(r"int64_t registerNow\(\)\s*\{.*?\n\}", provider, re.S)
    if not now or "reg::trustedReading" not in now.group(0) or "clockSyncedThisBoot" not in now.group(0):
        found.append("RegisterProvider.cpp: registerNow() must give no time unless a sync this boot or the retained clock vouches")
    return found


def self_test():
    assert re.search(r"\bwriteUtc\s*\(", "halClock.writeUtc(5)")
    assert not re.search(r"\bwriteUtc\s*\(", read_c("// halClock.writeUtc(5)")[0])


if __name__ == "__main__":
    self_test()
    found = problems()
    for line in found:
        print(line)
    if found:
        sys.exit(1)
    print("clock chip write checks passed")
