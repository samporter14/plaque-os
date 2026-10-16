"""Run with python3 test/test_x4c_capability_docs.py

docs/x4c-support.md says what each bit of plaque::Capabilities gates (the table between the capability-gates markers).
This keeps that table true: every bit of the struct has a row; a row that names a helper finds it defined in
lib/PlaqueBoard/PlaqueBoard.h; every consumer file named exists and uses that helper or the bit itself (`caps.<bit>`); and
a bit marked "advertised only" has no consumer listed, and nothing in src/ or lib/ outside the capability layer asks for it.
"""

from pathlib import Path
import re
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
from test_no_upstream_updates import read_c  # noqa: E402  (the same comment stripper the upstream guard uses)

ROOT = Path(__file__).resolve().parents[1]
HEADER = ROOT / "lib/PlaqueBoard/PlaqueBoard.h"
DOC = ROOT / "docs/x4c-support.md"


def code_of(text):
    """The source with its comments blanked: a name that only a comment mentions is not a use."""
    return read_c(text)[0]


def struct_bits(header_text):
    body = re.search(r"struct Capabilities \{(.*?)\n\};", header_text, re.S).group(1)
    return re.findall(r"^\s*bool (\w+) = false;", body, re.M)


def table_rows(doc_text):
    block = re.search(r"<!-- capability-gates -->(.*?)<!-- /capability-gates -->", doc_text, re.S).group(1)
    rows = {}
    for line in block.splitlines():
        cells = [c.strip() for c in line.strip().strip("|").split("|")]
        if len(cells) != 4 or not cells[0].startswith("`"):
            continue
        bit = cells[0].strip("`")
        helper = cells[2].strip("`")
        consumers = [c.strip().strip("`") for c in cells[3].split(",") if c.strip().strip("`") not in ("", "-")]
        rows[bit] = (cells[1], None if helper == "-" else helper, consumers)
    return rows


def helper_definition(header_text, helper):
    """The text of a constexpr helper's definition (to its closing brace or semicolon)."""
    m = re.search(r"constexpr\s+\w+\s+" + re.escape(helper) + r"\s*\(.*?(?=\nconstexpr |\n// |\nenum |\n\}|\Z)", header_text, re.S)
    return m.group(0) if m else None


def check(root=ROOT, header=HEADER, doc=DOC):
    problems = []
    header_text = header.read_text(encoding="utf-8")
    doc_text = doc.read_text(encoding="utf-8")
    bits = struct_bits(header_text)
    header_code = code_of(header_text)
    rows = table_rows(doc_text)
    for bit in bits:
        if bit not in rows:
            problems.append(f"{bit}: no row in the capability table of {doc.name}")
    for bit in rows:
        if bit not in bits:
            problems.append(f"{bit}: a row for a capability that does not exist")
    for bit, (gates, helper, consumers) in rows.items():
        advertised = gates.startswith("advertised only")
        if advertised and (helper or consumers):
            problems.append(f"{bit}: marked advertised only but names a helper or consumers")
        if not advertised and not consumers:
            problems.append(f"{bit}: gates something but names no file")
        if helper:
            definition = helper_definition(header_code, helper)
            if definition is None:
                problems.append(f"{bit}: helper {helper} is not defined in PlaqueBoard.h")
            elif not re.search(r"\b" + bit + r"\b", definition):
                problems.append(f"{bit}: helper {helper} does not mention it")
        for name in consumers:
            path = root / name
            if not path.is_file():
                problems.append(f"{bit}: {name} does not exist")
                continue
            text = code_of(path.read_text(encoding="utf-8"))
            uses_helper = bool(helper) and re.search(r"\b" + re.escape(helper) + r"\b", text)
            uses_bit = re.search(r"\bcaps\." + bit + r"\b", text)
            if not uses_helper and not uses_bit:
                problems.append(f"{bit}: {name} uses neither {helper or 'a helper'} nor caps.{bit}")
    # An advertised-only bit is asked by nothing outside the capability layer.
    for bit, (gates, _, _) in rows.items():
        if not gates.startswith("advertised only"):
            continue
        for folder in ("src", "lib"):
            for path in (root / folder).rglob("*"):
                if path.suffix not in (".cpp", ".h") or "PlaqueBoard" in path.parts or path.parent.name == "PlaqueBoard":
                    continue
                if re.search(r"caps\." + bit + r"\b", code_of(path.read_text(encoding="utf-8", errors="replace"))):
                    problems.append(f"{bit}: documented as advertised only, but {path.relative_to(root)} asks for it")
    return problems


SELF_TEST_HEADER = """struct Capabilities {
  bool keys = false;
  bool wifiPsk = false;
};
constexpr bool offersThing(const Capabilities& caps) { return caps.wifiPsk; }
"""
SELF_TEST_DOC = """<!-- capability-gates -->
| bit | gates | helper | consumers |
|---|---|---|---|
| `keys` | advertised only: nothing asks | - | - |
| `wifiPsk` | a thing | `offersThing` | `src/a.cpp` |
<!-- /capability-gates -->
"""


def self_test():
    import tempfile

    cases = [
        ("a true table", SELF_TEST_DOC, "src/a.cpp", "bool x = offersThing(caps);", False),
        ("a missing bit", SELF_TEST_DOC.replace("| `keys` | advertised only: nothing asks | - | - |\n", ""), "src/a.cpp",
         "offersThing(caps);", True),
        ("a consumer that uses neither the helper nor the bit", SELF_TEST_DOC, "src/a.cpp", "int x;", True),
        ("a consumer that asks for the bit itself", SELF_TEST_DOC, "src/a.cpp", "if (caps.wifiPsk) {}", False),
        ("a consumer file that is not there", SELF_TEST_DOC.replace("src/a.cpp", "src/b.cpp"), "src/a.cpp",
         "offersThing(caps);", True),
        ("a helper that does not exist", SELF_TEST_DOC.replace("offersThing", "offersNothing"), "src/a.cpp",
         "offersNothing(caps);", True),
        ("advertised only but asked for", SELF_TEST_DOC, "src/a.cpp", "offersThing(caps); if (board().caps.keys) {}", True),
        ("a consumer whose only mention is a comment", SELF_TEST_DOC, "src/a.cpp",
         "// offersThing(caps) and caps.wifiPsk are described here\nint x;", True),
        ("a consumer whose mention is in a block comment", SELF_TEST_DOC, "src/a.cpp",
         "/* offersThing(caps) */ int x;", True),
        ("a bit asked for only in a comment is still advertised only", SELF_TEST_DOC, "src/a.cpp",
         "offersThing(caps); // caps.keys is not asked here", False),
        ("a bit that is not a capability", SELF_TEST_DOC + "| `ghost` | x | - | - |\n".replace("| `ghost`", "<!-- -->"), "src/a.cpp",
         "offersThing(caps);", False),
    ]
    for name, doc_text, rel, source, must_fail in cases:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "lib/PlaqueBoard").mkdir(parents=True)
            (root / "docs").mkdir()
            (root / "src").mkdir()
            (root / "lib/PlaqueBoard/PlaqueBoard.h").write_text(SELF_TEST_HEADER)
            (root / "docs/x4c-support.md").write_text(doc_text)
            (root / rel).write_text(source)
            problems = check(root, root / "lib/PlaqueBoard/PlaqueBoard.h", root / "docs/x4c-support.md")
            if bool(problems) != must_fail:
                print(f"FAIL (self test): {name}: {problems}")
                sys.exit(1)
    return len(cases)


def main():
    cases = self_test()
    problems = check()
    for problem in problems:
        print("FAIL:", problem)
    if problems:
        sys.exit(1)
    print(f"x4c capability table: {len(table_rows(DOC.read_text(encoding='utf-8')))} capabilities, all true "
          f"({cases} self-test cases)")


if __name__ == "__main__":
    main()
