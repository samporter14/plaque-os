"""Run with python3 test/test_partitions.py.

The X4 Classic builds against partitions-x4c.csv, a table taken from the FreeInk SDK's documentation of the board
(freeink-sdk/docs/xteink-x4c-support.md), not read from a real reader. This checks that the file says what the
documentation says, that it is a sound 16 MB dual-OTA table, that it carries its caveat, and that the X4 Pro's own
table (read from a reader) is untouched. When a build is present, the image must fit its app slot.
"""

from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
FLASH_SIZE = 16 * 1024 * 1024
checks = 0


def check(condition, name):
    global checks
    if not condition:
        print("FAIL:", name)
        sys.exit(1)
    checks += 1


def read_table(path):
    rows = []
    for line in path.read_text().splitlines():
        if not line.strip() or line.lstrip().startswith("#"):
            continue
        cells = [c.strip() for c in line.split(",")]
        rows.append({"name": cells[0], "type": cells[1], "subtype": cells[2], "offset": int(cells[3], 16),
                     "size": int(cells[4], 16)})
    return rows


def documented_offsets():
    """The offsets the SDK documents, by label, from its Partitions section."""
    doc = (ROOT / "freeink-sdk/docs/xteink-x4c-support.md").read_text()
    section = doc.split("## Partitions", 1)[1]
    return {label: int(offset, 16) for label, offset in re.findall(r"`(\w+)`\s+@\s+(0x[0-9A-Fa-f]+)", section)}


x4c_path = ROOT / "partitions-x4c.csv"
x4pro_path = ROOT / "partitions-x4pro.csv"
x4c = read_table(x4c_path)
names = [row["name"] for row in x4c]
check(names == ["nvs", "otadata", "app0", "app1", "spiffs", "coredump"], "the six partitions, in order")

# What the SDK documents: the offsets as written, and each size the gap to the next one (16 MB at the end).
documented = documented_offsets()
check(set(documented) == set(names), f"the documentation names the same partitions: {sorted(documented)}")
for row in x4c:
    check(row["offset"] == documented[row["name"]], f"{row['name']} starts where the documentation says")
for row, after in zip(x4c, x4c[1:] + [None]):
    end = after["offset"] if after else FLASH_SIZE
    check(row["offset"] + row["size"] == end, f"{row['name']} fills the gap to the next partition")

# A sound table.
check(x4c[0]["offset"] == 0x9000, "nvs right after the partition table at 0x8000")
apps = [row for row in x4c if row["type"] == "app"]
check([a["subtype"] for a in apps] == ["ota_0", "ota_1"], "two OTA app slots")
check(apps[0]["size"] == apps[1]["size"] == 0x7E0000, "both app slots 0x7E0000")
check(all(a["offset"] % 0x10000 == 0 for a in apps), "app slots 64 KB aligned")
check(next(r for r in x4c if r["name"] == "otadata")["size"] == 0x2000, "otadata is two sectors")
check(all(r["offset"] % 0x1000 == 0 and r["size"] % 0x1000 == 0 for r in x4c), "every partition sector aligned")
check(x4c[-1]["offset"] + x4c[-1]["size"] <= FLASH_SIZE, "inside 16 MB")

# The caveat travels with the file.
header = x4c_path.read_text()
check("FreeInk SDK documentation" in header, "the header names its source")
check("NOT read from a real X4 Classic" in header, "the header says it was not read from a reader")
check("0x8000" in header, "the header says to compare with a read of 0x8000 before flashing")

# The X4 Pro's table, read from a reader, is exactly what it was; the X4 Classic's equals it numerically.
x4pro = read_table(x4pro_path)
check([(r["name"], r["offset"], r["size"]) for r in x4pro] ==
      [("nvs", 0x9000, 0x5000), ("otadata", 0xE000, 0x2000), ("app0", 0x10000, 0x7E0000),
       ("app1", 0x7F0000, 0x7E0000), ("spiffs", 0xFD0000, 0x14000), ("coredump", 0xFE4000, 0x1C000)],
      "the X4 Pro's table is unchanged")
check(x4c == x4pro, "the documented X4 Classic table equals the X4 Pro's")
check(x4c_path.read_text() != x4pro_path.read_text(), "but it is its own file")

# A built image fits its slot.
for env in ("x4c", "x4pro"):
    image = ROOT / ".pio/build" / env / "firmware.bin"
    if image.exists():
        check(image.stat().st_size <= 0x7E0000, f"{env}: the image fits the 0x7E0000 app slot")

print(f"partitions: {checks} checks passed")
