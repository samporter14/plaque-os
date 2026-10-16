#!/usr/bin/env bash
set -euo pipefail
if [[ $# -ne 0 ]]; then
  echo "This helper only builds the X4 Pro target and accepts no arguments." >&2
  exit 2
fi
if [[ "$(uname -s)" != "Darwin" ]]; then
  echo "Use the manual PlatformIO build steps in README.md on this host." >&2
  exit 2
fi
REPO_DIR="$(cd "$(dirname "$0")/.." && pwd)"
TOOLCHAIN_DIR="${AMBIENT_TOOLCHAIN_DIR:-$REPO_DIR/build/ambient-toolchain}"
VENV_DIR="${AMBIENT_VENV_DIR:-$TOOLCHAIN_DIR/venv}"
PYTHON_CMD="${AMBIENT_PYTHON:-python3.12}"
export PLATFORMIO_CORE_DIR="${PLATFORMIO_CORE_DIR:-$TOOLCHAIN_DIR/pio-core}"
export PIP_CACHE_DIR="$TOOLCHAIN_DIR/pip-cache"
export UV_CACHE_DIR="$TOOLCHAIN_DIR/uv-cache"
export UV_PYTHON_INSTALL_DIR="$TOOLCHAIN_DIR/uv-python"
mkdir -p "$TOOLCHAIN_DIR"
if [[ ! -x "$VENV_DIR/bin/python" ]]; then
  "$PYTHON_CMD" -m venv "$VENV_DIR"
fi
"$VENV_DIR/bin/python" -m pip install -r "$REPO_DIR/requirements-build.txt"
cd "$REPO_DIR"
"$VENV_DIR/bin/pio" run -e x4pro
