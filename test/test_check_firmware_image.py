"""Run with python3 test/test_check_firmware_image.py

test/check_firmware_image.py checked against synthetic images: it must pass an image that is right, and fail the ones that
are wrong, in the ways that have mattered: the board tag of another board, an unlisted host (in any case: DNS names are
case-insensitive, so a mixed-case name is a name), an upstream update string, two board tags, and an image it cannot tell
the board of (no --board, outside .pio/build/<env>/), which is an error rather than a check that quietly does less.
"""

from pathlib import Path
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parent))
import check_firmware_image as image  # noqa: E402

PRO = b"CROSSPOINT-BOARD-V1:x4pro;" + b"\x00" * 40 + b"Plaque X4 Pro" + b"\x00" * 40
CLASSIC = b"CROSSPOINT-BOARD-V1:x4c;" + b"\x00" * 40 + b"Plaque X4 Classic" + b"\x00" * 40
NOISE = b"\x00\x01\x02 pool.ntp.org \x00 time.google.com \x00"


def write(root, relative, body):
    path = Path(root) / relative
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(body)
    return path


def run(path, board=None):
    """(problems, error): the problems check() reports, or the UnknownBoard error."""
    try:
        return image.check(path, board)[0], None
    except image.UnknownBoard as error:
        return None, str(error)


def main():
    checks = 0

    def expect(condition, name):
        nonlocal checks
        if not condition:
            print("FAIL:", name)
            sys.exit(1)
        checks += 1

    with tempfile.TemporaryDirectory() as tmp:
        # An image that is right, in the build folder of its board.
        problems, error = run(write(tmp, ".pio/build/x4pro/firmware.bin", PRO + NOISE))
        expect(problems == [] and error is None, "a Pro image in .pio/build/x4pro passes")
        problems, error = run(write(tmp, ".pio/build/x4c-gh_release/firmware.bin", CLASSIC + NOISE))
        expect(problems == [] and error is None, "a Classic image in a release folder of the x4c environment passes")

        # The board tag of the other board, in a folder that says otherwise.
        problems, _ = run(write(tmp, ".pio/build/x4c/firmware.bin", PRO + NOISE))
        expect(problems and any("board tag" in p for p in problems), "a Pro image in the Classic's build folder fails")
        expect(any("Plaque X4 Pro" in p for p in problems), "and says it introduces itself as the Pro")

        # Where the image sits is not taken on trust.
        problems, error = run(write(tmp, "artifacts/plaque-x4pro-wifi-abc1234.bin", PRO + NOISE))
        expect(problems is None and error and "--board" in error, "an image outside .pio/build, no --board: an error")
        problems, error = run(write(tmp, "build/x4pro/firmware.bin", PRO + NOISE))
        expect(problems is None and error, "a folder named x4pro that is not under .pio/build: an error")
        problems, error = run(write(tmp, "x4c/firmware.bin", PRO + NOISE))
        expect(problems is None and error, "nor is a bare x4c folder believed")

        # --board names it.
        problems, error = run(write(tmp, "artifacts/pro.bin", PRO + NOISE), "x4pro")
        expect(problems == [] and error is None, "--board x4pro on a Pro image passes anywhere")
        problems, _ = run(write(tmp, "artifacts/pro.bin", PRO + NOISE), "x4c")
        expect(problems, "--board x4c on a Pro image fails")
        problems, error = run(write(tmp, "artifacts/classic.bin", CLASSIC + NOISE), "x4c")
        expect(problems == [] and error is None, "--board x4c on a Classic image passes")
        problems, _ = run(write(tmp, "artifacts/classic.bin", CLASSIC + NOISE), "x4pro")
        expect(problems, "--board x4pro on a Classic image fails")
        problems, error = run(write(tmp, "artifacts/other.bin", NOISE), "none")
        expect(problems == [] and error is None, "--board none checks the hosts only")
        problems, error = run(write(tmp, "artifacts/other.bin", NOISE), "x4c-typo")
        expect(problems is None and error, "an unknown --board is an error")

        # Hosts: any case, with or without a URL around it.
        for name, body in [
            ("a URL host", b"see https://updates.example.com/fw.bin for"),
            ("a bare host", b"  updates.example.com  "),
            ("a bare host in mixed case", b"  Updates.Example.COM  "),
            ("a mixed-case host in a run with other text", b"xxxx ZVbb,evil-host.Example.Org yyyy"),
            ("a URL in mixed case", b"HTTPS://Updates.Example.COM/x"),
        ]:
            problems, error = run(write(tmp, ".pio/build/x4pro/firmware.bin", PRO + NOISE + b" " + body + b" "))
            expect(problems and any("not an allowed host" in p for p in problems), f"{name} is reported")
        # An allowed host in any case is still allowed (it is the same name).
        problems, error = run(write(tmp, ".pio/build/x4pro/firmware.bin", PRO + b"  Pool.NTP.org  time.GOOGLE.com  "))
        expect(problems == [], "an allowed host in another case is the same host")

        # Upstream update strings, and two board tags.
        problems, _ = run(write(tmp, ".pio/build/x4pro/firmware.bin", PRO + b" releases/latest "))
        expect(any("releases/latest" in p for p in problems), "an upstream update string is reported")
        problems, _ = run(write(tmp, ".pio/build/x4pro/firmware.bin", PRO + CLASSIC))
        expect(any("2 board tags" in p for p in problems), "two board tags are reported")

    print(f"check_firmware_image: {checks} checks passed")


if __name__ == "__main__":
    main()
