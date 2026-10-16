"""Regenerate synthetic 800×480 fixtures with the real server conversion path."""
import hashlib
import json
from pathlib import Path
import sys

from PIL import Image, ImageDraw

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "sender-web"))
from imaging import prepare

DEST = Path(__file__).parent / "fixtures"
DEST.mkdir(exist_ok=True)
payload, preview = prepare("text", "hello\nsee you soon")
(DEST / "hello-note.bin").write_bytes(payload)
(DEST / "hello-note.png").write_bytes(preview)
(DEST / "latest.json").write_text(json.dumps({
    "message_id": "fixture1", "created_at": "2026-10-03T14:00:00Z",
    "image_url": "/api/device/my-reader/image/fixture1",
    "sha256": hashlib.sha256(payload).hexdigest(), "format": "mono1-msb-white",
    "width": 800, "height": 480, "byte_length": 48000,
}, indent=2) + "\n")
image = Image.new("RGB", (800, 480))
draw = ImageDraw.Draw(image)
for y in range(480):
    draw.line((0, y, 799, y), fill=(80 + y // 4, 140 + y // 6, 210 - y // 5))
draw.ellipse((460, 35, 610, 185), fill=(255, 238, 180))
draw.polygon([(0, 480), (190, 170), (390, 430), (590, 210), (800, 480)], fill=(62, 86, 85))
image.save(DEST / "sample-photo.png")
