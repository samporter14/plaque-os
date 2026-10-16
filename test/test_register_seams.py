"""Run with python3 test/test_register_seams.py

Register (the sleep screen that shows an instrument's schedule) and the university Wi-Fi work were built on two branches
and meet in this one. Where they meet, the wiring is held here, by reading the source (no host test can compile the
renderer-bound RegisterProvider.cpp or main.cpp):

  - Register is a capability, not a model: none of its files (and none of the places that decide whether it is selected)
    asks BoardConfig for the X4 Pro;
  - the Register poll gets the clock the way every mailbox request does: RegisterProvider::fetch() calls
    message_tls::prepareClock() (which applies the installed profile's time servers) before it opens a connection, and
    only for https, as the note poll does;
  - a scheduled Register poll joins with ambient::connectSavedWifi() (the profile-aware wake join) and fetches inside that
    branch, never before it; a boundary wake and the poll share that path and join nothing of their own;
  - "Check the register now" tries the installed profile (beginProfileJoin) before the saved networks, as Messages does;
  - no Register file starts Wi-Fi itself.
"""

from pathlib import Path
import re
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
from test_no_upstream_updates import read_c  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]

# Where Register is decided or drawn. None of them may ask for the X4 Pro by name.
CAPABILITY_FILES = [
    "src/ambient/AmbientSettings.cpp",
    "src/ambient/RegisterProvider.cpp",
    "src/activities/ambient/MessagesActivity.cpp",
    "src/activities/ambient/RegisterActivity.cpp",
    "src/activities/boot_sleep/SleepActivity.cpp",
]
# Register's files: none of them may start the radio (the join belongs to netjoin and WifiSelectionActivity).
RADIO_CALLS = re.compile(r"\bWiFi\s*\.\s*(?:begin|mode|reconnect|disconnect)\b|\besp_wifi\w*|\bWiFiClient\w*\b")


def function_body(code, signature):
    """The text of the function whose definition starts with `signature` (to its matching closing brace)."""
    start = code.find(signature)
    if start < 0:
        return None
    open_at = code.find("{", start)
    depth = 0
    for i in range(open_at, len(code)):
        if code[i] == "{":
            depth += 1
        elif code[i] == "}":
            depth -= 1
            if depth == 0:
                return code[open_at:i + 1]
    return None


def problems_in(files):
    """files: {relative path: source text}. Returns the list of what is wrong."""
    found = []
    code = {rel: read_c(text)[0] for rel, text in files.items()}

    for rel in CAPABILITY_FILES:
        if rel in code and re.search(r"\bisX4Pro\s*\(|\bBoardConfig\s*::", code[rel]):
            found.append(f"{rel}: asks BoardConfig for a board; Register is gated by plaque::offersRegister")
    if "src/main.cpp" in code:
        main = code["src/main.cpp"]
        enter = function_body(main, "void enterDeepSleep(") or ""
        if "registerSelected" in enter and re.search(r"\bx4Pro\b", enter):
            found.append("src/main.cpp: enterDeepSleep() gates the Register sleep on x4Pro")
        if "plaque::ambientSleepScreen(caps)" not in enter:
            found.append("src/main.cpp: enterDeepSleep() does not gate the ambient sleep screen on ambientSleepScreen(caps)")

    provider = code.get("src/ambient/RegisterProvider.cpp", "")
    fetch = function_body(provider, "FetchResult RegisterProvider::fetch(")
    if fetch is None:
        found.append("src/ambient/RegisterProvider.cpp: fetch() not found")
    else:
        clock = fetch.find("message_tls::prepareClock()")
        request = fetch.find("HttpDownloader::fetchBounded")
        if clock < 0:
            found.append("RegisterProvider::fetch() does not prepare the clock (message_tls::prepareClock)")
        elif request < 0 or clock > request:
            found.append("RegisterProvider::fetch() asks before it has the clock")
        if clock >= 0 and not re.search(r'strncmp\s*\(\s*config\.baseUrl\s*,\s*"https://"\s*,\s*8\s*\)\s*==\s*0\s*&&\s*!message_tls::prepareClock',
                                        fetch):
            found.append("RegisterProvider::fetch() prepares the clock for something other than https, or ignores its answer")

    main = code.get("src/main.cpp", "")
    join = re.search(r"if\s*\(\s*polling\s*&&\s*ambient::connectSavedWifi\(\)\s*\)\s*\{", main)
    if not join:
        found.append("src/main.cpp: the timer wake's poll does not join with ambient::connectSavedWifi()")
    else:
        branch = function_body(main[join.start():], "if") or ""
        if "registerScreen.fetch()" not in branch:
            found.append("src/main.cpp: the Register fetch is not inside the wake join's branch")
    if len(re.findall(r"registerScreen\s*\.\s*fetch\s*\(", main)) != 1:
        found.append("src/main.cpp: the Register fetch is called other than once, in the wake path")
    if "WifiSelectionActivity" in main:
        found.append("src/main.cpp: the timer wake joins through the Wi-Fi screen")

    # The rest of the Register wiring in main.cpp: the places a refactor of the wake path could drop one silently.
    if main:
        def squash(text):
            return re.sub(r"\s+", " ", text)

        flat = squash(main)
        enter = squash(function_body(main, "void enterDeepSleep(") or "")
        # enterDeepSleep forgets what the panel held, and disarms the wake chain when Register is not the sleep screen.
        if "registerScreen.clearPanel()" not in enter:
            found.append("src/main.cpp: enterDeepSleep() no longer clears what the panel held (registerScreen.clearPanel())")
        if not re.search(r"if \(!AMBIENT_SETTINGS\.registerSelected\(\)\) ambient::RegisterProvider::disarm\(\)", enter):
            found.append("src/main.cpp: enterDeepSleep() does not disarm Register when it is not the sleep screen")
        if not re.search(r"registerScreen\.wakeMicros\([^;]*sleepPlan\.methodStep", enter):
            found.append("src/main.cpp: enterDeepSleep() arms Register's wake without telling it a Method step is on the panel")
        # The power-bounce re-sleep reads the schedule while the card is up, then asks for the wake, then shuts the card.
        bounce = flat.find("wakeHoldVerified")
        load = flat.find("RegisterProvider::instance().loadSchedule()", bounce)
        ask = flat.find("wakeMicros(", load if load >= 0 else bounce)
        shut = flat.find("Storage.prepareForDeepSleep()", ask if ask >= 0 else bounce)
        if bounce < 0 or load < 0 or ask < 0 or shut < 0 or not (load < ask < shut):
            found.append("src/main.cpp: the power-bounce re-sleep must loadSchedule(), then wakeMicros(), then shut the card")
        elif "registerSelected()" not in flat[bounce:load]:
            found.append("src/main.cpp: the power-bounce re-sleep loads the schedule whether or not Register is selected")
        # The timer wake asks for the next wake before the card is shut down.
        tail = flat.find("if (!buttonWake) {")
        ask = flat.find("registerScreen.wakeMicros(", tail)
        shut = flat.find("Storage.prepareForDeepSleep()", tail)
        if tail < 0 or ask < 0 or shut < 0 or not ask < shut:
            found.append("src/main.cpp: the timer wake must ask Register for its next wake before the card is shut down")
        if not re.search(r"registerScreen\.wakeMicros\([^;]*APP_STATE\.methodStepOnPanel", flat[tail:] if tail >= 0 else ""):
            found.append("src/main.cpp: the timer wake's next-wake plan ignores the Method-step marker")
        # A redraw never goes over a Method step.
        if not re.search(r"planRedraw\(battery, APP_STATE\.methodStepOnPanel\)", flat):
            found.append("src/main.cpp: the Register redraw ignores the Method-step marker (planRedraw)")
        # A card that will not mount, or settings that will not load, with Register armed retry instead of ending the chain.
        if not re.search(r"registerWakeArmed = ambientTimerWake && ambient::RegisterProvider::armed\(\)", flat):
            found.append("src/main.cpp: registerWakeArmed is not 'a timer wake Register armed'")
        if not re.search(r"retry < 2 && \(.{0,120}registerWakeArmed", flat):
            found.append("src/main.cpp: the card mount is not retried for a Register wake")
        if not re.search(r"registerWakeArmed \? ambient::RegisterProvider::cardRetryMicros\(", flat):
            found.append("src/main.cpp: a card that will not mount ends a Register wake chain (cardRetryMicros)")
        if not re.search(r"!ambientSettingsRead && ambient::RegisterProvider::armed\(\).{0,400}cardRetryMicros\(", flat):
            found.append("src/main.cpp: mailbox settings that will not load end a Register wake chain (cardRetryMicros)")

    activity = code.get("src/activities/ambient/RegisterActivity.cpp", "")
    if activity:
        profile = activity.find("beginProfileJoin")
        saved = activity.find("makeUniqueNoThrow<WifiSelectionActivity>")
        if profile < 0:
            found.append("RegisterActivity does not try the installed profile (netjoin::beginProfileJoin)")
        elif saved >= 0 and "joinSavedNetworks();" not in activity:
            found.append("RegisterActivity never falls back to the saved networks")
        if "netjoin::abortProfileJoin" not in activity or "netjoin::pollProfileJoin" not in activity:
            found.append("RegisterActivity does not poll and abort the profile join")
        if "interactiveProfileJoinMs" not in activity:
            found.append("RegisterActivity waits for the profile for a time of its own")
        body = function_body(activity, "void RegisterActivity::onEnter()") or ""
        if body.find("beginProfileJoin") > body.find("joinSavedNetworks()") >= 0:
            found.append("RegisterActivity::onEnter() tries the saved networks before the profile")

    for rel in ("src/ambient/RegisterProvider.cpp", "src/activities/ambient/RegisterActivity.cpp"):
        if rel in code and RADIO_CALLS.search(code[rel]):
            found.append(f"{rel}: starts or stops the radio itself")
    return found


def read_tree():
    names = set(CAPABILITY_FILES) | {"src/main.cpp"}
    return {rel: (ROOT / rel).read_text(encoding="utf-8") for rel in sorted(names)}


def self_test():
    tree = read_tree()
    assert not problems_in(tree), problems_in(tree)

    def mutated(rel, old, new):
        # `old` is matched whatever the line breaks and indentation are: the sources are clang-formatted.
        pattern = re.compile(r"\s+".join(re.escape(word) for word in old.split()))
        assert pattern.search(tree[rel]), f"self test: {old!r} is not in {rel}"
        copy = dict(tree)
        copy[rel] = pattern.sub(lambda _: new, tree[rel], count=1)
        return copy

    cases = [
        ("a BoardConfig gate comes back in AmbientSettings",
         mutated("src/ambient/AmbientSettings.cpp", "plaque::offersRegister(plaque::board().caps) && useRegisterScreen",
                 "BoardConfig::isX4Pro() && useRegisterScreen")),
        ("the fetch drops the clock", mutated("src/ambient/RegisterProvider.cpp", "message_tls::prepareClock()", "true")),
        ("the fetch asks before the clock",
         mutated("src/ambient/RegisterProvider.cpp",
                 "if (std::strncmp(config.baseUrl, \"https://\", 8) == 0 && !message_tls::prepareClock()) return FetchResult::Failed;",
                 "HttpDownloader::fetchBounded(\"u\", nullptr, {}, 0);\n  if (std::strncmp(config.baseUrl, \"https://\", 8) == 0 && !message_tls::prepareClock()) return FetchResult::Failed;")),
        ("the fetch ignores the clock's answer",
         mutated("src/ambient/RegisterProvider.cpp", "&& !message_tls::prepareClock()) return FetchResult::Failed;",
                 "&& false) return FetchResult::Failed; message_tls::prepareClock();")),
        ("the poll joins through the Wi-Fi screen",
         mutated("src/main.cpp", "if (polling && ambient::connectSavedWifi()) {", "if (polling && WifiSelectionActivity::join()) {")),
        ("the Register fetch moves out of the join's branch",
         mutated("src/main.cpp", "registerMode ? registerScreen.fetch() :", "registerMode ? FetchResult::Failed :")),
        ("the check forgets the profile", mutated("src/activities/ambient/RegisterActivity.cpp", "beginProfileJoin", "beginNothing")),
        ("the check waits its own time",
         mutated("src/activities/ambient/RegisterActivity.cpp", "netjoin::interactiveProfileJoinMs(", "std::max<uint32_t>(")),
        ("Register's sleep is gated on x4Pro again",
         mutated("src/main.cpp", "const bool ambientSleep = !methodStep && plaque::ambientSleepScreen(caps) &&",
                 "const bool x4Pro = true; const bool ambientSleep = !methodStep && x4Pro && registerSelected() &&")),
        ("the power-bounce re-sleep forgets to load the schedule",
         mutated("src/main.cpp", "if (AMBIENT_SETTINGS.registerSelected()) ambient::RegisterProvider::instance().loadSchedule();", "")),
        ("the card-retry branch ignores a Register wake",
         mutated("src/main.cpp", "registerWakeArmed ? ambient::RegisterProvider::cardRetryMicros(powerManager.getBatteryPercentage()) : (",
                 "(")),
        ("the redraw ignores the Method-step marker",
         mutated("src/main.cpp", "registerScreen.planRedraw(battery, APP_STATE.methodStepOnPanel)", "registerScreen.planRedraw(battery, false)")),
        ("enterDeepSleep forgets the panel", mutated("src/main.cpp", "registerScreen.clearPanel();", "")),
        ("enterDeepSleep forgets to disarm",
         mutated("src/main.cpp", "if (!AMBIENT_SETTINGS.registerSelected()) ambient::RegisterProvider::disarm();", "")),
        ("the timer wake asks for the next wake after the card is shut",
         mutated("src/main.cpp", "const uint64_t wake = registerMode ?",
                 "Storage.prepareForDeepSleep(); const uint64_t wake = registerMode ?")),
        ("the provider starts the radio", mutated("src/ambient/RegisterProvider.cpp", "if (WiFi.status() != WL_CONNECTED) return FetchResult::Failed;",
                                                  "WiFi.begin(\"x\", \"y\");")),
    ]
    for name, files in cases:
        if not problems_in(files):
            print(f"FAIL (self test): {name}: nothing noticed")
            sys.exit(1)
    return len(cases)


def main():
    cases = self_test()
    found = problems_in(read_tree())
    for problem in found:
        print("FAIL:", problem)
    if found:
        sys.exit(1)
    print(f"register seams: the wiring holds ({cases} self-test cases)")


if __name__ == "__main__":
    main()
