"""Generate the two PNG install icons from the checked-in vector design."""
from pathlib import Path
from PIL import Image, ImageDraw

OUTPUT = Path(__file__).resolve().parents[1] / "public"
for size in (192, 512):
    scale = 4
    canvas = Image.new("RGB", (size * scale, size * scale), "#0c1d28")
    draw = ImageDraw.Draw(canvas)
    s = size * scale / 192
    point = lambda x, y: (round(x * s), round(y * s))
    draw.rounded_rectangle((0, 0, size * scale - 1, size * scale - 1), radius=round(42 * s), fill="#0c1d28")
    draw.polygon([point(23, 117), point(96, 44), point(169, 117), point(96, 95)], fill="#34d399")
    draw.line([point(96, 44), point(96, 148)], fill="#e8f5ef", width=round(11 * s))
    draw.line([point(48, 149), point(96, 131), point(144, 149)], fill="#e8f5ef", width=round(10 * s), joint="curve")
    canvas.resize((size, size), Image.Resampling.LANCZOS).save(OUTPUT / f"icon-{size}.png")
