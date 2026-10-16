"""Run with python3 test/test_firmware_version.py.

The Plaque readers' firmware version (scripts/git_branch.py) goes to the mailbox as X-Lamina-Firmware, into
/Lamina/reader.json and shows in Settings > About. It must tell builds and boards apart, fit what the Worker accepts,
and not trip the OTA version check.
"""

import importlib.util
from pathlib import Path
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("git_branch", ROOT / "scripts/git_branch.py")
git_branch = importlib.util.module_from_spec(spec)
spec.loader.exec_module(git_branch)  # outside SCons this runs the script's own self-check and prints one line

# cloud/src/worker.js: const FIRMWARE = /^[\x20-\x7e]{1,40}$/
WORKER_FIRMWARE = re.compile(r"^[\x20-\x7e]{1,40}$")
checks = 0


def check(condition, name):
    global checks
    if not condition:
        print("FAIL:", name)
        sys.exit(1)
    checks += 1


def version(branch="lamina-stageb", sha="1ac8494", dirty=False, base="1.6.5", board="x4pro"):
    return git_branch.lamina_version(base, board, branch, sha, dirty)


# The shape.
check(version() == "1.6.5-x4pro+1ac8494.lamina_stageb", "a clean tree: base, board, sha, branch")
check(version(dirty=True) == "1.6.5-x4pro+1ac8494.dirty.lamina_stageb", "uncommitted changes are marked")
check(version(branch="main") == "1.6.5-x4pro+1ac8494.main", "a short branch")
check(version(branch="") == "1.6.5-x4pro+1ac8494", "no branch")
check(version(sha="unknown", branch="main") == "1.6.5-x4pro+unknown.main", "git unavailable")
check(version(sha="abc\"def\\") == "1.6.5-x4pro+abcdef.lamina_stageb", "odd characters in the sha go")

# Two builds of different commits differ; the same commit twice does not.
check(version(sha="1111111") != version(sha="2222222"), "different commits differ")
check(version(sha="1111111") == version(sha="1111111"), "the same commit gives the same string")
check(version(dirty=True) != version(dirty=False), "a dirty tree differs from its commit")

# The X4 Classic: the same shape, its own board name, never the X4 Pro's.
check(version(board="x4c") == "1.6.5-x4c+1ac8494.lamina_stageb", "the X4 Classic names its board")
check(version(board="x4c", branch="feature/x4c-capabilities") == "1.6.5-x4c+1ac8494.feature_x4c_capabiliti",
      "a long branch is cut to what is left beside the board name")
check(version(board="x4c") != version(board="x4pro"), "the two boards' builds of one commit differ")
check("x4pro" not in version(board="x4c", branch="main"), "an X4 Classic string never says x4pro")

# What the Worker accepts, whatever the branch is called.
branches = ["", "main", "lamina-stageb", "feature/very-long-branch-name-that-goes-on-and-on-and-on",
            "x" * 200, "-rc", "rc-1", "release-rc2", "a b", "ünï", "é" * 50, "..", "a" * 21, "a" * 23, "a" * 24]
for board in ("x4pro", "x4c"):
    for branch in branches:
        for dirty in (False, True):
            for sha in ("1ac8494", "unknown", "a1b2c3d4e5f6"):
                v = version(branch=branch, sha=sha, dirty=dirty, board=board)
                check(WORKER_FIRMWARE.match(v) is not None, f"the Worker accepts {v!r}")
                check(v.startswith(f"1.6.5-{board}+"), f"keeps the base and board: {v!r}")
                check(re.sub(r"[^A-Za-z0-9]", "", sha) in v, f"the sha survives the cut: {v!r}")
                # Upstream's OTA check treated a "-rc" anywhere in the current version as a release candidate.
                check("-rc" not in v, f"never looks like a release candidate: {v!r}")
                # ... and reads only the leading major.minor.patch.
                check(re.match(r"(\d+)\.(\d+)\.(\d+)", v).groups() == ("1", "6", "5"), f"leading version intact: {v!r}")
                check(v.count("-") == 1, f"one hyphen only: {v!r}")
check(len(version(branch="x" * 200)) == 40, "a long branch is cut to exactly the Worker's limit")
check(len(version(branch="x" * 200, dirty=True)) == 40, "also when the tree is dirty")
check(len(version(branch="x" * 200, board="x4c")) == 40, "the X4 Classic's too")

# A leaner base version cannot push the sha out either.
check(WORKER_FIRMWARE.match(version(base="12.34.56", branch="x" * 100)) is not None, "a longer base version still fits")

# The environment's flags no longer set a second, fixed CROSSPOINT_VERSION: x4pro and x4c take it from the script.
ini = (ROOT / "platformio.ini").read_text()


def env_section(name):
    return ini.split(f"[env:{name}]", 1)[1].split("\n[env:", 1)[0]


x4pro = env_section("x4pro")
check("-DCROSSPOINT_VERSION" not in x4pro, "x4pro defines the version only through git_branch.py")
check("x4pro" in git_branch.LAMINA_ENVS, "git_branch.py handles x4pro")
x4c = env_section("x4c")
check("-DCROSSPOINT_VERSION" not in x4c, "x4c defines the version only through git_branch.py")
check("x4c" in git_branch.LAMINA_ENVS, "git_branch.py handles x4c")

# The X4 Classic's environments carry the Plaque identity of the X4 Pro's, on their own partition table.
for name in ("x4c", "x4c-gh_release", "x4c-gh_release_rc"):
    section = env_section(name)
    check('-DUSB_PRODUCT=\\"Plaque\\ X4\\ Classic\\"' in section, f"{name}: USB product is Plaque X4 Classic")
    check('-DUSB_MANUFACTURER=\\"Solanum\\"' in section, f"{name}: USB manufacturer is Solanum")
    check("board_build.partitions = partitions-x4c.csv" in section, f"{name}: the X4 Classic's partition table")
    check("CrossPoint" not in section.replace("CROSSPOINT_VERSION", ""), f"{name}: nothing user-facing says CrossPoint")
    check("-DFREEINK_DEVICE_X4CLASSIC=1" in section and "FREEINK_DEVICE_X4PRO" not in section, f"{name}: the X4C profile")
check("board_build.partitions = partitions-x4pro.csv" in x4pro, "x4pro keeps its own table")

# Other environments keep what they had: the dev string for default, nothing for release.
class Env(dict):
    defines = None

    def Append(self, **kw):
        Env.defines = kw["CPPDEFINES"]


Env.defines = None
git_branch.inject_version(Env({"PIOENV": "x4c-gh_release", "PROJECT_DIR": str(ROOT)}))
check(Env.defines is None, "a release environment is left to its own flags")
git_branch.inject_version(Env({"PIOENV": "x4c", "PROJECT_DIR": str(ROOT)}))
check(Env.defines[0][0] == "CROSSPOINT_VERSION" and "-x4c+" in Env.defines[0][1], "x4c gets its board's string")
check(WORKER_FIRMWARE.match(Env.defines[0][1].strip('\\"')) is not None, "as injected, the Worker accepts it")
git_branch.inject_version(Env({"PIOENV": "default", "PROJECT_DIR": str(ROOT)}))
check(Env.defines is not None and "-dev-" in Env.defines[0][1], "default keeps the dev string")
git_branch.inject_version(Env({"PIOENV": "x4pro", "PROJECT_DIR": str(ROOT)}))
check(Env.defines[0][0] == "CROSSPOINT_VERSION" and "-x4pro+" in Env.defines[0][1], "x4pro gets the new string")
check(WORKER_FIRMWARE.match(Env.defines[0][1].strip('\\"')) is not None, "as injected, the Worker accepts it")

print(f"firmware version: {checks} checks passed")
