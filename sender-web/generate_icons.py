"""Generate the PWA PNG icons from simple vector geometry."""
import math
from pathlib import Path
from PIL import Image, ImageDraw

for size in (192, 512):
    image = Image.new("RGB", (size, size), "#f1e6e2")
    points = []
    for i in range(240):
        t = 2 * math.pi * i / 240
        x = 16 * math.sin(t) ** 3
        y = 13 * math.cos(t) - 5 * math.cos(2*t) - 2 * math.cos(3*t) - math.cos(4*t)
        points.append((size/2 + x*size/48, size/2 - y*size/48))
    ImageDraw.Draw(image).polygon(points, fill="#864e53")
    image.save(Path(__file__).parent / "static" / f"icon-{size}.png")
