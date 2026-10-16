"""Desktop protocol client. This is not an ESP32/display/power simulator."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import time
from urllib.request import Request, urlopen

from PIL import Image

from app import read_env
from imaging import FORMAT, HEIGHT, PAYLOAD_BYTES, WIDTH


def validate_metadata(meta, device_id):
    if not isinstance(meta, dict) or "message_id" not in meta:
        raise ValueError("Bad metadata shape")
    if meta["message_id"] is None:
        return False
    ident = meta["message_id"]
    if not isinstance(ident, str) or not re.fullmatch(r"[A-Za-z0-9_-]{1,64}", ident):
        raise ValueError("Bad message ID")
    if meta.get("image_url") != f"/api/device/{device_id}/image/{ident}" or \
       meta.get("format") != FORMAT or meta.get("width") != WIDTH or meta.get("height") != HEIGHT or \
       meta.get("byte_length") != PAYLOAD_BYTES or not isinstance(meta.get("sha256"), str) or \
       not re.fullmatch(r"[0-9a-f]{64}", meta["sha256"]):
        raise ValueError("Bad image metadata")
    return True


def poll_once(origin, device_id, device_token, state_dir, validator=None, get=None):
    state_dir = Path(state_dir)
    state_path, frame_path = state_dir / "state.json", state_dir / "frame.bin"
    headers = {"Authorization": "Bearer " + device_token}

    def download(path, limit):
        # Refuse redirects: the bearer token must stay with this exact mailbox.
        from urllib.request import HTTPRedirectHandler, build_opener
        class NoRedirect(HTTPRedirectHandler):
            def redirect_request(self, req, fp, code, msg, response_headers, newurl):
                return None
        with build_opener(NoRedirect()).open(Request(origin + path, headers=headers), timeout=10) as response:
            data = response.read(limit + 1)
        if len(data) > limit:
            raise ValueError("Response exceeded limit")
        return data

    fetch = get or download
    try:
        body = fetch(f"/api/device/{device_id}/latest", 2048)
        if len(body) > 2048:
            raise ValueError("Oversized metadata")
        meta = json.loads(body)
        if not validate_metadata(meta, device_id):
            return "empty"
        state = {}
        if state_path.exists():
            state = json.loads(state_path.read_text())
        cached = frame_path.read_bytes() if frame_path.exists() else b""
        valid_cache = len(cached) == PAYLOAD_BYTES and hashlib.sha256(cached).hexdigest() == state.get("sha256")
        same = valid_cache and meta["message_id"] == state.get("message_id") and meta["sha256"] == state.get("sha256")
        payload = cached if same else fetch(meta["image_url"], PAYLOAD_BYTES)
        if len(payload) != PAYLOAD_BYTES or hashlib.sha256(payload).hexdigest() != meta["sha256"]:
            raise ValueError("Bitmap size/hash mismatch")
        if validator:
            import tempfile
            with tempfile.TemporaryDirectory() as scratch:
                metadata_path, bitmap_path = Path(scratch) / "metadata.json", Path(scratch) / "frame.bin"
                metadata_path.write_bytes(body)
                bitmap_path.write_bytes(payload)
                result = subprocess.run([str(validator), str(metadata_path), str(bitmap_path), device_id,
                    str(state.get("message_id", "")), str(state.get("sha256", "")), "1" if valid_cache else "0", "--poll"],
                    capture_output=True, text=True)
                if result.returncode != 0 or result.stdout.strip() != ("unchanged" if same else "updated"):
                    raise ValueError("Firmware parser rejected response")
        if same:
            return "unchanged"
        state_dir.mkdir(parents=True, exist_ok=True)
        staged = state_dir / "frame.part"
        staged.write_bytes(payload)
        staged.replace(frame_path)
        Image.frombytes("1", (WIDTH, HEIGHT), payload).convert("L").save(state_dir / "frame.png")
        next_state = {"message_id": meta["message_id"], "sha256": meta["sha256"], "refreshes": state.get("refreshes", 0) + 1}
        staged_state = state_dir / "state.part"
        staged_state.write_text(json.dumps(next_state, indent=2) + "\n")
        staged_state.replace(state_path)
        return "updated"
    except (OSError, ValueError, KeyError, TypeError, subprocess.SubprocessError):
        return "failed"


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--once", action="store_true")
    parser.add_argument("--interval", type=int, default=10, help="Desktop demo interval in seconds, not the firmware default")
    parser.add_argument("--state-dir", type=Path, default=Path(__file__).parent / ".data" / "demo-device")
    parser.add_argument("--validator", type=Path)
    args = parser.parse_args()
    config = read_env()
    if not config.get("DEVICE_TOKEN"):
        raise SystemExit("Run setup_local.py first.")
    if args.validator and not args.validator.is_file():
        raise SystemExit("Build the native protocol validator first; see README.md.")
    if args.interval < 1:
        raise SystemExit("Interval must be positive.")
    while True:
        result = poll_once(config["PUBLIC_ORIGIN"].rstrip("/"), config["DEVICE_ID"], config["DEVICE_TOKEN"],
                           args.state_dir, args.validator.resolve() if args.validator else None)
        print(result, flush=True)
        if args.once:
            raise SystemExit(1 if result == "failed" else 0)
        time.sleep(args.interval)


if __name__ == "__main__":
    main()
