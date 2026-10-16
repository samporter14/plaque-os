"""All layout, resizing and dithering happens here, before a device downloads."""
from io import BytesIO
from pathlib import Path
import textwrap

from PIL import Image, ImageDraw, ImageFont, ImageOps, UnidentifiedImageError

WIDTH, HEIGHT = 800, 480
PAYLOAD_BYTES = WIDTH * HEIGHT // 8
FORMAT = "mono1-msb-white"
Image.MAX_IMAGE_PIXELS = 24_000_000
FONT_PATH = Path(__file__).parent / "assets" / "NotoSans.ttf"


def font(size):
    if FONT_PATH.exists():
        return ImageFont.truetype(str(FONT_PATH), size)
    return ImageFont.load_default(size=size)


def text_image(text):
    if not isinstance(text, str) or not text.strip() or len(text) > 800:
        raise ValueError("Write a message between 1 and 800 characters.")
    image = Image.new("L", (WIDTH, HEIGHT), 255)
    draw = ImageDraw.Draw(image)
    # Measure each candidate using the exact font the server will draw.
    for size in (64, 56, 48, 40, 32, 26, 22):
        selected = font(size)
        lines = []
        for paragraph in text.strip().splitlines():
            current = ""
            for word in textwrap.wrap(paragraph, width=80, break_long_words=True, replace_whitespace=False) or [""]:
                # Process words individually to preserve deliberate line breaks.
                for chunk in word.split(" "):
                    trial = f"{current} {chunk}".strip()
                    if current and draw.textlength(trial, font=selected) > 688:
                        lines.append(current)
                        current = chunk
                    else:
                        current = trial
                    while draw.textlength(current, font=selected) > 688:
                        cut = len(current) - 1
                        while cut > 1 and draw.textlength(current[:cut], font=selected) > 688:
                            cut -= 1
                        lines.append(current[:cut])
                        current = current[cut:]
            lines.append(current)
        content = "\n".join(lines)
        box = draw.multiline_textbbox((0, 0), content, font=selected, spacing=12, align="center")
        if box[3] - box[1] <= 352:
            x = (WIDTH - (box[2] - box[0])) / 2 - box[0]
            y = (HEIGHT - (box[3] - box[1])) / 2 - box[1]
            draw.multiline_text((x, y), content, fill=0, font=selected, spacing=12, align="center")
            return image
    raise ValueError("This message needs more room. Try shortening it or adding fewer line breaks.")


def uploaded_image(data, fit="fit", mode="photo"):
    if fit not in ("fit", "crop"):
        raise ValueError("Choose Fit or Fill.")
    try:
        with Image.open(BytesIO(data)) as original:
            if original.format not in ("PNG", "JPEG", "WEBP"):
                raise ValueError("Choose a PNG, JPEG or WebP photo.")
            if original.width * original.height > Image.MAX_IMAGE_PIXELS:
                raise ValueError("Please choose a smaller image (under 24 megapixels).")
            if mode == "draw" and original.size != (WIDTH, HEIGHT):
                raise ValueError("A drawing must be 800 × 480.")
            # Apply camera orientation, composite transparency onto white, strip metadata.
            image = ImageOps.exif_transpose(original).convert("RGBA")
            background = Image.new("RGBA", image.size, "white")
            background.alpha_composite(image)
            image = background.convert("L")
    except (UnidentifiedImageError, OSError, Image.DecompressionBombError) as exc:
        raise ValueError("This image could not be opened. Try a PNG or JPEG.") from exc
    if mode == "draw":
        return image
    if fit == "crop":
        return ImageOps.fit(image, (WIDTH, HEIGHT), method=Image.Resampling.LANCZOS)
    scaled = ImageOps.contain(image, (WIDTH, HEIGHT), method=Image.Resampling.LANCZOS)
    result = Image.new("L", (WIDTH, HEIGHT), 255)
    result.paste(scaled, ((WIDTH - scaled.width) // 2, (HEIGHT - scaled.height) // 2))
    return result


def prepare(mode, text="", data=b"", fit="fit"):
    if mode == "text":
        image = text_image(text)
    elif mode in ("draw", "photo"):
        if not data:
            raise ValueError("Choose a photo or draw something first.")
        image = uploaded_image(data, fit, mode)
    else:
        raise ValueError("Choose Text, Draw or Photo.")
    mono = image.convert("1", dither=Image.Dither.FLOYDSTEINBERG)
    payload = mono.tobytes()
    if len(payload) != PAYLOAD_BYTES:
        raise ValueError("The prepared image has the wrong size.")
    preview = BytesIO()
    mono.convert("L").save(preview, format="PNG")
    return payload, preview.getvalue()
