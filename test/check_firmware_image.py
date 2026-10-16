"""Run with python3 test/check_firmware_image.py [--board x4pro|x4c|none] [firmware.bin] after a build (default
.pio/build/x4pro/firmware.bin).

The strings of the built image, which include freeink-sdk and the framework that test/test_no_upstream_updates.py
cannot read: every host in them must be in ALLOWED_HOSTS (the sources' list) or IMAGE_ONLY_HOSTS (what the framework
and the CA bundle bring), and none of the upstream update strings may be there. The mailbox, OPDS and Calibre
addresses are typed in at run time, so they are not in the image.

An image built for a Plaque reader must also carry exactly one board tag, naming that board
(src/network/FirmwareBoardTag.cpp: the SD card installer refuses an image whose tag names another board, so a Pro image
is refused on a Classic and the other way round), and introduce itself on USB as that Plaque reader. Which board the image
is for is never guessed from its name: it is the --board the caller passes (`none` for an image that is not a Plaque
reader's, which then gets the host check only), or else the build folder .pio/build/<env>/ of an x4pro* or x4c* environment.
A file anywhere else with no --board is an error, so a check cannot quietly weaken itself by looking in the wrong place.
"""

from pathlib import Path
import re
import sys

from test_no_upstream_updates import ALLOWED_HOSTS, IMAGE_ONLY_HOSTS, ROOT, host_problem, hosts_in

MIN_RUN = 6        # the shortest string looked at, as with strings(1)
MIN_BARE_RUN = 8   # a bare domain is only believed in a run this long: shorter ones are noise from binary data
FORBIDDEN_IMAGE = [b"releases/latest", b"api.github.com", b"crosspoint-reader/crosspoint-reader", b"esp_https_ota",
                   b"OtaUpdater", b"HTTPUpdate", b"Checking for update"]
BOARD_TAG = re.compile(rb"CROSSPOINT-BOARD-V1:([\x21-\x3a\x3c-\x7e]{1,23});")
# The board tag and USB product string each Plaque environment's image carries, by the environment's first word.
PLAQUE_IMAGES = {
    "x4pro": ("x4pro", b"Plaque X4 Pro"),
    "x4c": ("x4c", b"Plaque X4 Classic"),
}


def board_tags(data):
    return [m.group(1).decode("ascii") for m in BOARD_TAG.finditer(data)]


class UnknownBoard(Exception):
    pass


def plaque_board(image_path, board=None):
    """(tag, USB product) the image must carry, or None for an image that is not a Plaque reader's build (board "none").

    `board` is what the caller says the image is for. Without it the image must sit in .pio/build/<env>/ of an x4pro* or
    x4c* environment; any other place raises UnknownBoard rather than skipping the board checks."""
    if board is not None:
        if board == "none":
            return None
        if board not in PLAQUE_IMAGES:
            raise UnknownBoard(f"unknown board {board!r}: x4pro, x4c or none")
        return PLAQUE_IMAGES[board]
    folder = Path(image_path).resolve().parent
    env = folder.name.split("-")[0]
    if folder.parent.name == "build" and folder.parent.parent.name == ".pio" and env in PLAQUE_IMAGES:
        return PLAQUE_IMAGES[env]
    raise UnknownBoard(f"cannot tell which board {image_path} is for (not in .pio/build/x4pro* or x4c*): "
                       f"pass --board x4pro, --board x4c, or --board none for an image that is not a Plaque reader's")


def check(image_path, board=None):
    data = Path(image_path).read_bytes()
    expected = plaque_board(image_path, board)
    allowed = {**ALLOWED_HOSTS, **IMAGE_ONLY_HOSTS}
    problems = []
    seen = {}
    for m in re.finditer(rb"[\x20-\x7e]{%d,}" % MIN_RUN, data):
        run = m.group().decode("ascii")
        for host in hosts_in(run, MIN_BARE_RUN):
            seen.setdefault(host, run[:100])
            problem = host_problem(host, run, allowed)
            if problem:
                problems.append(f"{problem}: ...{run[:100]}")
    for word in FORBIDDEN_IMAGE:
        if word in data:
            problems.append(f"the image contains {word.decode()}")
    tags = board_tags(data)
    if len(tags) > 1:
        problems.append(f"the image carries {len(tags)} board tags: {tags}")
    if expected:
        tag, product = expected
        if tags != [tag]:
            problems.append(f"the image's board tag is {tags}, not [{tag!r}]")
        if product not in data:
            problems.append(f"the image does not introduce itself on USB as {product.decode()}")
        for other_tag, other_product in PLAQUE_IMAGES.values():
            if other_tag != tag and other_product in data:
                problems.append(f"the image introduces itself as {other_product.decode()}")
        if b"CrossPoint_X4" in data:
            problems.append("the image still introduces itself on USB as CrossPoint")
    return problems, seen


def main():
    args = sys.argv[1:]
    board = None
    if "--board" in args:
        at = args.index("--board")
        if at + 1 >= len(args):
            print("--board needs x4pro, x4c or none")
            sys.exit(2)
        board = args[at + 1]
        del args[at:at + 2]
    image = Path(args[0]) if args else ROOT / ".pio/build/x4pro/firmware.bin"
    if not image.exists():
        print(f"no image at {image}; build it first (pio run -e x4pro)")
        sys.exit(2)
    try:
        problems, seen = check(image, board)
    except UnknownBoard as error:
        print("FAIL:", error)
        sys.exit(2)
    for problem in sorted(set(problems)):
        print("FAIL:", problem)
    if problems:
        sys.exit(1)
    tags = board_tags(image.read_bytes())
    print(f"firmware image: board tag {tags[0] if tags else '(none)'}; {len(seen)} hosts, all allowed:")
    for host in sorted(seen):
        why = ALLOWED_HOSTS.get(host) or IMAGE_ONLY_HOSTS[host]
        print(f"  {host:28} {why}")


if __name__ == "__main__":
    main()
