"""Run with python3 test/test_no_upstream_updates.py (and test/check_firmware_image.py on a built image).

Plaque installs firmware from a .bin on the SD card and never checks, offers or downloads an update from upstream
CrossPoint's GitHub releases: an upstream image would replace Plaque. This keeps that true in three ways:

  1. no update machinery: no updater classes or APIs (HTTPUpdate, esp_https_ota, esp_ota_begin and friends, the
     release parser), and no Settings entry for an update check;
  2. no server nobody listed: every host in a string literal of the firmware's own sources (src/ and all of lib/
     except vendored code) is in ALLOWED_HOSTS, wherever in the literal it sits and even when the literal is built by
     concatenation, and only the files in NETWORK_FILES use a network client at all (an HTTP client, a socket of any
     kind, a name lookup, SNTP, lwIP, the Wi-Fi driver or its EAP supplicant, a Wi-Fi join, mDNS), so a renamed updater
     with its URL read at run time still shows up, and so does a new way onto the network. The list of patterns is a
     denylist and cannot be complete: that is why every hit needs a decision. SERVER_FILES are the files that listen
     (File Transfer's web server, WebDAV, Calibre Connect): a server is not an update client, but it is on this list,
     and one that grows a client call is reported. USB_SERIAL_FILES are the files that look like networking and are
     not: the USB setup link talks over the serial port, to a computer, and must stay out of both;
  3. test/check_firmware_image.py applies the same host list to the strings of the built image, which also covers
     freeink-sdk and the framework.

The sources are scanned with comments removed, so a link in a comment is not a request. The test also checks itself:
SELF_TESTS are the cases a guard like this tends to miss, each of which must be reported.
"""

from pathlib import Path
import re
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]

# ---- what is allowed -----------------------------------------------------------------------------------------------

# Hosts the firmware may name. Mailbox and OPDS servers, Calibre and plugin URLs are typed or declared by the person
# and are never in the image, so they are not here.
ALLOWED_HOSTS = {
    "github.com": "Manage Fonts: the crosspoint-fonts catalog (only that repository, see FONT_CATALOG_PREFIX)",
    "sync.crosspointreader.com": "KOReader Sync, the default server",
    "sync.koreader.rocks": "KOReader Sync, the legacy default it migrates from",
    "pool.ntp.org": "clock sync (NTP)",
    "time.nist.gov": "clock sync (NTP)",
    "time.google.com": "clock sync (NTP)",
    "time.cloudflare.com": "clock sync (NTP)",
    "www.w3.org": "XML namespaces, not requests",
    "www.idpf.org": "XML namespaces, not requests",
    "opds-spec.org": "OPDS link relations, not requests",
}
FONT_CATALOG_PREFIX = "https://github.com/crosspoint-reader/crosspoint-fonts/"

# Only in the built image: strings that come from the framework or the SDK, not from this repository's sources.
IMAGE_ONLY_HOSTS = {
    "docs.espressif.com": "the Arduino core's WebUSB landing page descriptor (WebUSB is off)",
    "entrust.net": "a certificate authority's name inside the CA bundle",
    "www.entrust.net": "a certificate authority's name inside the CA bundle",
    "godaddy.com": "a certificate authority's name inside the CA bundle",
    "ssl.com": "a certificate authority's name inside the CA bundle",
}

# Vendored third-party code, not scanned: it is not ours to change and does not touch the network.
VENDORED = ("lib/expat", "lib/miniz", "lib/uzlib", "lib/EpdFont/builtinFonts")
GENERATED_NAMES = {"I18nKeys.h", "I18nStrings.h", "I18nStrings.cpp"}

# The only files that use a network client. A new one is a decision to make, not an accident to merge.
NETWORK_FILES = {
    "lib/KOReaderSync/KOReaderSyncClient.cpp": "KOReader Sync",
    "lib/TrustedTime/TrustedTime.cpp": "clock sync: SNTP (UDP 123) to the configured time servers",
    "lib/hal/HalClock.cpp": "clock sync: SNTP (configTzTime) from the Clock settings",
    "src/activities/CatalogActivity.cpp": "OPDS and plugin catalogs",
    "src/activities/CatalogActivity.h": "OPDS and plugin catalogs",
    "src/activities/ambient/MethodDevice.cpp": "the mailbox (shelf)",
    "src/activities/ambient/MethodDevice.h": "the mailbox (shelf)",
    "src/activities/ambient/MethodNetDevice.cpp": "the shelf's Wi-Fi join and clock sync",
    "src/activities/browser/OpdsBookBrowserActivity.cpp": "OPDS",
    "src/activities/network/WifiSelectionActivity.cpp": "joining the saved Wi-Fi networks",
    "src/activities/plugins/PluginCatalogActivity.cpp": "plugin catalog",
    "src/activities/plugins/PluginCatalogActivity.h": "plugin catalog",
    "src/activities/reader/KOReaderSyncActivity.cpp": "KOReader Sync: its Wi-Fi join",
    "src/activities/settings/ConnectionCheckDevice.cpp": "Check connection: the mailbox request, through method::DeviceHttp",
    "src/activities/settings/ConnectionCheckDevice.h": "Check connection: the ports it reaches the network through",
    "src/activities/settings/ConnectionCheckNetwork.cpp": "Check connection: the join, through method::DeviceWifi",
    "src/activities/settings/FontDownloadActivity.cpp": "Manage Fonts",
    "src/ambient/LatestMessageProvider.cpp": "the mailbox (frame)",
    "src/ambient/RegisterProvider.cpp": "the mailbox (register)",
    "src/network/CrossPointWebServer.cpp": "File Transfer",
    "src/network/DeviceNameDevice.cpp": "names the reader on the network: the DHCP host name and the hotspot's SSID",
    "src/network/HttpDownloader.cpp": "the shared client",
    "src/network/MessageTls.cpp": "the mailbox's clock sync and certificate-date checks",
    "src/network/NetworkJoinDevice.cpp": "joining the installed network profile and the wake join (open, PSK, EAP-TLS)",
    "src/network/WifiPowerSaveGuard.h": "Wi-Fi power-save around a transfer",
    "src/util/PluginHttp.cpp": "plugin requests",
    "src/util/PluginHttp.h": "plugin requests",
}
# Files that listen for connections instead of making them. File Transfer (the web server, WebDAV) and Calibre Connect run
# only while the person has them open, on the reader's own hotspot or the network it joined; none of them fetches
# anything, and none may be an update client. They are checked to stay free of the client calls in CLIENT_CALLS.
SERVER_FILES = {
    "src/activities/network/CalibreConnectActivity.cpp": "Calibre Connect: a listener the person opens",
    "src/activities/network/CrossPointWebServerActivity.cpp": "File Transfer: the screen that runs the web server",
    "src/network/CrossPointWebServer.h": "File Transfer: the web server's header",
    "src/network/WebDAVHandler.cpp": "File Transfer: WebDAV, served by the web server",
    "src/network/WebDAVHandler.h": "File Transfer: WebDAV, served by the web server",
}
# Files that look like networking and are not. The USB setup link (Settings > System > Network > Connect to Mac) speaks
# the PLAQUE-NET/1 line protocol over the reader's USB serial port, to the computer it is plugged into: no socket, no
# network client. They are checked to stay that way, and are not in NETWORK_FILES.
USB_SERIAL_FILES = {
    "src/activities/settings/ConnectToMacActivity.cpp": "USB serial (PLAQUE-NET/1), not a network client",
    "src/activities/settings/ConnectToMacActivity.h": "USB serial (PLAQUE-NET/1), not a network client",
    "src/network/NetProvisionProtocol.cpp": "the PLAQUE-NET/1 line protocol, pure",
    "src/network/NetProvisionProtocol.h": "the PLAQUE-NET/1 line protocol, pure",
}
# Calls that make a connection, resolve a name or set the clock from the network, or bring the radio or its supplicant up.
CLIENT_CALLS = re.compile(
    r"\b(?:HttpDownloader\s*::|SecureHttpClient|HTTPClient|WiFiClientSecure|WiFiClient|WiFiUDP|AsyncClient|"
    r"esp_http_client\w*|esp_tls\w*|lwip_\w+|esp_eap_client\w*|esp_wifi\w*|esp_netif\w*|esp_sntp\w*|sntp_\w+|"
    r"dns_gethostbyname|getaddrinfo|configTzTime|tcp_connect|ping_\w+|esp_ping\w*)\b"
    r"|\bsocket\s*\("
    r"|\bWiFi\s*\.\s*(?:begin|reconnect|(?:STA|AP)\s*\.\s*\w+)\b")
# Things that listen or announce: a server socket, a web server, mDNS.
SERVER_CALLS = re.compile(r"\b(?:WiFiServer|WebServer|AsyncWebServer|MDNS|mdns_\w+)\b|\bWiFi\s*\.\s*softAP\w*")
NETWORK_CLIENT = re.compile(CLIENT_CALLS.pattern + "|" + SERVER_CALLS.pattern)

# Update machinery that must not exist anywhere in the firmware's sources. esp_ota_get_* only reads partitions and
# is fine; the writing calls are forbidden outside the SD card flasher.
FLASHER = "src/network/FirmwareFlasher.cpp"
FORBIDDEN_CODE = [
    (re.compile(r"\bHTTPUpdate\w*|\bhttpUpdate\b"), "an Arduino HTTP updater"),
    (re.compile(r"#\s*include\s*[<\"](?:HTTPUpdate|HTTPUpdateServer|Update)\.h[>\"]"), "an Arduino updater header"),
    (re.compile(r"\bUpdate\s*\.\s*(?:begin|write|end)\b"), "the Arduino Update library"),
    (re.compile(r"\besp_https_ota\w*"), "an HTTPS OTA call"),
    (re.compile(r"\bReleaseJsonParser\b"), "the GitHub release parser"),
    (re.compile(r"\bOtaUpdat\w*"), "an OTA updater"),
    (re.compile(r"\bCheckForUpdates\b|\bSTR_CHECK_UPDATES\b"), "the Settings update check"),
]
FORBIDDEN_FLASH_CALLS = re.compile(
    r"\besp_ota_(?:begin|write|write_with_offset|end|abort|set_boot_partition|mark_app_\w+)\b")
FORBIDDEN_LITERAL = re.compile(r"releases/latest|api\.github\.com|crosspoint-reader/crosspoint-reader")

# ---- reading C and web sources -------------------------------------------------------------------------------------

C_SUFFIXES = {".c", ".cpp", ".h", ".hpp"}
WEB_SUFFIXES = {".html", ".js", ".inc"}  # JavaScript and HTML: searched as they are, comments included

TOKEN = re.compile(
    r"""(?P<raw>(?:u8|u|U|L)?R"(?P<d>[^()\\\s]{0,16})\((?s:.*?)\)(?P=d)")"""
    r"""|(?P<str>(?:u8|u|U|L)?"(?:\\.|[^"\\\n])*")"""
    r"""|(?P<chr>'(?:\\.|[^'\\\n])*')"""
    r"""|(?P<comment>//[^\n]*|/\*(?s:.*?)\*/)""")


def read_c(text):
    """(code, literals): code is the text with comments blanked (strings kept); literals are the string literals'
    contents, with adjacent ones (separated by white space or a +) joined, so a host split across them is whole."""
    code = []
    literals = []
    last = 0
    prev_end = None
    for m in TOKEN.finditer(text):
        code.append(text[last:m.start()])
        last = m.end()
        if m.group("comment") is not None:
            code.append(re.sub(r"[^\n]", " ", m.group()))  # a comment is white space
            continue
        code.append(m.group())
        if m.group("chr") is not None:
            continue
        token = m.group()
        body = token[token.index("(") + 1:token.rindex(")")] if m.group("raw") is not None else token[token.index('"') + 1:-1]
        if prev_end is not None and re.fullmatch(r"\s*\+?\s*", text[prev_end:m.start()]):
            literals[-1] += body
        else:
            literals.append(body)
        prev_end = m.end()
    code.append(text[last:])
    return "".join(code), literals


URL = re.compile(r"https?://([A-Za-z0-9][A-Za-z0-9.-]*)")
TLDS = "com|org|net|io|dev|app|cloud|rocks|info|xyz|ai|co|uk|de|eu|us|ch|nl|fr|cc|tv|me|edu|gov|biz|to|sh"
BARE = re.compile(r"(?<![A-Za-z0-9.-])((?:[A-Za-z0-9-]+\.)+(?:" + TLDS + r"))(?![A-Za-z0-9-])", re.I)


def hosts_in(text, min_bare_run=0):
    """Every host a string names: in a URL anywhere in it, or as a bare domain (pool.ntp.org, or the half of a
    URL that was written separately). Bare domains are only looked for in runs of min_bare_run characters or more."""
    found = {m.rstrip(".").lower() for m in URL.findall(text)}
    if len(text) >= min_bare_run:
        found |= {m.lower() for m in BARE.findall(text)}
    return {re.sub(r"^\d(?=www\.)", "", h) for h in found}  # a DER length byte in front of a name in the image


def host_problem(host, text, allowed):
    if host not in allowed:
        return f"{host} is not an allowed host"
    if host == "github.com" and FONT_CATALOG_PREFIX not in text:
        return "github.com is allowed only for the crosspoint-fonts catalog"
    return None


# ---- the audit -----------------------------------------------------------------------------------------------------

def scannable(root):
    for folder in ("src", "lib"):
        for path in sorted((root / folder).rglob("*")):
            rel = path.relative_to(root).as_posix()
            if not path.is_file() or path.suffix not in C_SUFFIXES | WEB_SUFFIXES:
                continue
            if path.name in GENERATED_NAMES or ".generated." in path.name or rel.endswith(".min.js"):
                continue
            if any(rel == v or rel.startswith(v + "/") for v in VENDORED):
                continue
            yield rel, path


def audit(root):
    """The problems found in root/src and root/lib."""
    problems = []
    for rel, path in scannable(root):
        text = path.read_text(encoding="utf-8", errors="replace")
        if path.suffix in C_SUFFIXES:
            code, literals = read_c(text)
        else:
            code, literals = "", [text]  # web sources: the whole file is searched for hosts
        for literal in literals:
            for host in sorted(hosts_in(literal)):
                problem = host_problem(host, literal, ALLOWED_HOSTS)
                if problem:
                    problems.append(f"{rel}: {problem}")
            if FORBIDDEN_LITERAL.search(literal):
                problems.append(f"{rel}: names an upstream update URL")
        if path.suffix in C_SUFFIXES:
            for pattern, what in FORBIDDEN_CODE:
                if pattern.search(code):
                    problems.append(f"{rel}: uses {what}")
            if rel != FLASHER and FORBIDDEN_FLASH_CALLS.search(code):
                problems.append(f"{rel}: writes an OTA partition (only {FLASHER} may)")
            if NETWORK_CLIENT.search(code) and rel not in NETWORK_FILES:
                if rel in SERVER_FILES:
                    if CLIENT_CALLS.search(code):
                        problems.append(f"{rel}: is listed as a server but makes client calls: move it to NETWORK_FILES")
                else:
                    problems.append(f"{rel}: uses a network client but is not in NETWORK_FILES or SERVER_FILES")
    return problems


def audit_tree(root):
    """audit() plus the checks that need the real tree."""
    problems = audit(root)
    for rel, why in USB_SERIAL_FILES.items():
        path = root / rel
        if not path.exists():
            problems.append(f"{rel} (USB serial: {why}) is listed but not in the tree")
        elif NETWORK_CLIENT.search(read_c(path.read_text(encoding="utf-8"))[0]):
            problems.append(f"{rel} is listed as USB serial but uses a network client: move it to NETWORK_FILES")
    for gone in ("src/network/OtaUpdater.cpp", "src/network/OtaUpdater.h",
                 "src/activities/settings/OtaUpdateActivity.cpp", "src/activities/settings/OtaUpdateActivity.h",
                 "lib/JsonParser", "test/release_json_parser"):
        if (root / gone).exists():
            problems.append(f"{gone} is still in the tree")
    settings = (root / "src/activities/settings/SettingsActivity.cpp").read_text(encoding="utf-8")
    if "SettingAction::SdFirmwareUpdate" not in settings:
        problems.append("Settings no longer offers the SD card firmware update")
    return problems


# ---- the guard checks itself ---------------------------------------------------------------------------------------

BAD = "updates.example.com"
SELF_TESTS = [  # (name, relative path, source): each must be reported
    ("a URL in the middle of a literal", "src/a.cpp", f'const char* s = "Updates: https://{BAD}/fw.bin (latest)";'),
    ("a URL after a pointer-deref line", "src/a.cpp", f'*p = "https://{BAD}/x";'),
    ("a host built by concatenation", "src/a.cpp", f'auto u = std::string("https://") + "{BAD}" + "/x";'),
    ("a host split over adjacent literals", "src/a.cpp", 'const char* h = "updates." "example.com";'),
    ("a bare host", "src/a.cpp", f'configTzTime("UTC0", "{BAD}");'),
    ("a URL in a header", "lib/Foo/Foo.h", f'static constexpr char URL[] = "https://{BAD}/";'),
    ("a URL in a .c file", "lib/Foo/foo.c", f'const char *url = "https://{BAD}/";'),
    ("a URL in a new lib folder", "lib/JsonParser/Other.cpp", f'const char* u = "https://{BAD}/";'),
    ("a URL in a raw string", "src/a.cpp", f'const char* u = R"x(see https://{BAD}/)x";'),
    ("a URL in web code", "src/network/html/A.html", f"<script>fetch('https://{BAD}/')</script>"),
    ("GitHub outside the font catalog", "src/a.cpp",
     'const char* u = "https://github.com/crosspoint-reader/crosspoint-reader/x";'),
    ("the releases API", "src/a.cpp", 'const char* u = "https://api.github.com/repos/o/r/releases/latest";'),
    ("an Arduino updater include", "src/a.cpp", "#include <HTTPUpdate.h>\n"),
    ("an Arduino updater include in quotes", "src/a.cpp", '#include "HTTPUpdate.h"\n'),
    ("the Arduino Update library", "src/a.cpp", "void f() { Update.begin(size); }"),
    ("an HTTPS OTA call", "src/a.cpp", "esp_https_ota(&config);"),
    ("esp_ota_begin outside the flasher", "src/a.cpp", "esp_ota_begin(part, size, &h);"),
    ("esp_ota_set_boot_partition outside the flasher", "lib/x/x.c", "esp_ota_set_boot_partition(p);"),
    ("the release parser", "src/a.cpp", "ReleaseJsonParser parser;"),
    ("an OTA updater class", "src/a.cpp", "OtaUpdater updater;"),
    ("a renamed updater with a run-time URL", "src/FirmwareRefresher.cpp",
     "void FirmwareRefresher::run(const std::string& url) { HttpDownloader::downloadToFile(url, path, nullptr); }"),
    ("a renamed updater with its own client", "src/Refresher.cpp", "SecureHttpClient http; http.begin(url);"),
    ("an updater on esp_http_client", "lib/x/y.c", "esp_http_client_handle_t c = esp_http_client_init(&cfg);"),
    ("the Settings update row", "src/a.cpp", "case SettingAction::CheckForUpdates:"),
    ("a Wi-Fi driver call", "src/a.cpp", "esp_wifi_connect();"),
    ("the EAP supplicant", "src/a.cpp", "esp_eap_client_set_identity(id, n);"),
    ("SNTP", "lib/Foo/Foo.cpp", "esp_sntp_init();"),
    ("a name lookup", "src/a.cpp", "dns_gethostbyname(host, &addr, cb, nullptr);"),
    ("getaddrinfo without the lwip prefix", "src/a.cpp", "getaddrinfo(host, nullptr, &hints, &res);"),
    ("a Wi-Fi join", "src/a.cpp", "WiFi.begin(ssid, password);"),
    ("a Wi-Fi join with spacing", "src/a.cpp", "WiFi . begin(ssid);"),
    ("a clock sync", "src/a.cpp", 'configTzTime("UTC0", "pool.ntp.org");'),
    ("USB serial code that grows a Wi-Fi join", "src/activities/settings/ConnectToMacActivity.cpp",
     "void f() { WiFi.begin(ssid); }"),
    # The second review's must-catch list: ways onto the network the first pattern did not name.
    ("a BSD socket", "src/a.cpp", "int s = socket(AF_INET, SOCK_STREAM, 0); connect(s, (sockaddr*)&a, sizeof a);"),
    ("an lwIP socket", "src/a.cpp", "int s = lwip_socket(AF_INET, SOCK_DGRAM, 0);"),
    ("lwIP's SNTP, unprefixed", "src/a.cpp", "sntp_setservername(0, name); sntp_init();"),
    ("the new Wi-Fi STA object", "src/a.cpp", "WiFi.STA.connect(ssid, pw);"),
    ("a Wi-Fi reconnect", "src/a.cpp", "WiFi.reconnect();"),
    ("a Wi-Fi access point", "src/a.cpp", "WiFi.softAP(ssid, pw);"),
    ("a Wi-Fi server", "src/a.cpp", "WiFiServer server(80); server.begin();"),
    ("a web server", "src/a.cpp", "WebServer server(80);"),
    ("the netif layer", "src/a.cpp", 'esp_netif_t* n = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");'),
    ("mDNS", "src/a.cpp", 'MDNS.begin("x");'),
    ("raw lwIP TCP", "src/a.cpp", "tcp_connect(pcb, &ip, 80, cb);"),
    ("an async TCP client", "src/a.cpp", 'AsyncClient c; c.connect("h", 80);'),
    ("a ping", "src/a.cpp", "ping_start(&cfg);"),
    ("a listed server that grows a client call", "src/network/WebDAVHandler.cpp",
     "void f() { WebServer s(80); HttpDownloader::fetchUrl(u, cb); }"),
]
SELF_TESTS_QUIET = [  # (name, relative path, source): none of these is a problem
    ("a link in a line comment", "src/a.cpp", f"// see https://{BAD}/docs\nint x;"),
    ("a link in a block comment", "src/a.cpp", f"/* see https://{BAD}/docs\n * and https://{BAD}/more */\nint x;"),
    ("a link in a comment line starting with a star", "src/a.cpp", f"/*\n * https://{BAD}\n */\nint x;"),
    ("an allowed host", "lib/TrustedTime/TrustedTime.cpp", 'configTzTime("UTC0", "pool.ntp.org", "time.google.com");'),
    ("the font catalog", "src/a.cpp", f'constexpr char U[] = "{FONT_CATALOG_PREFIX}releases/download/x/fonts.json";'),
    ("esp_ota_get_running_partition", "src/a.cpp", "const esp_partition_t* p = esp_ota_get_running_partition();"),
    ("the mailbox using the shared client", "src/ambient/LatestMessageProvider.cpp", "HttpDownloader::fetchUrl(url, cb);"),
    ("a path that ends like a file", "src/a.cpp", 'const char* f = "/.crosspoint/settings.json";'),
    ("a // inside a string", "src/a.cpp", 'const char* u = "http://pool.ntp.org//x"; int y;'),
    ("the profile join, which is listed", "src/network/NetworkJoinDevice.cpp", "esp_eap_client_set_identity(id, n);"),
    ("a Wi-Fi driver call named in a comment", "src/a.cpp", "// esp_wifi_connect() is called by the core\nint x;"),
    ("the USB link, which talks over the serial port", "src/activities/settings/ConnectToMacActivity.cpp",
     "void f() { logSerial.write(data, n); }"),
    ("a listed server", "src/network/WebDAVHandler.cpp", "WebServer server(80); server.begin();"),
    ("a name that only ends in socket", "src/a.cpp", "void f() { websocket(x); int mysocket = 0; }"),
]


def self_test():
    checks = 0
    for cases, must_report in ((SELF_TESTS, True), (SELF_TESTS_QUIET, False)):
        for name, rel, source in cases:
            with tempfile.TemporaryDirectory() as tmp:
                path = Path(tmp) / rel
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(source)
                problems = audit(Path(tmp))
            if bool(problems) != must_report:
                print(f"FAIL: the guard {'misses' if must_report else 'wrongly reports'}: {name}\n  {problems}")
                sys.exit(1)
            checks += 1
    return checks


def main():
    checks = self_test()
    problems = audit_tree(ROOT)
    for problem in problems:
        print("FAIL:", problem)
    if problems:
        sys.exit(1)
    checks += 1
    print(f"no upstream updates: {checks} checks passed ({len(SELF_TESTS)} cases it must catch, "
          f"{len(SELF_TESTS_QUIET)} it must leave alone, and the tree)")


if __name__ == "__main__":
    main()
