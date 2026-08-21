"""Create deterministic application assets from the original project logo."""

from pathlib import Path

from PIL import Image


PROJECT_ROOT = Path(__file__).resolve().parents[1]
SOURCE = PROJECT_ROOT / "assets" / "branding" / "AmbiLight Logo.png"
CANONICAL_LOGO = PROJECT_ROOT / "assets" / "branding" / "logo.png"
ICON_DIRECTORY = PROJECT_ROOT / "desktop" / "assets" / "icons"
APPLICATION_PNG = ICON_DIRECTORY / "app-icon.png"
APPLICATION_ICO = ICON_DIRECTORY / "app.ico"

ICON_SIZES = (
    (16, 16),
    (20, 20),
    (24, 24),
    (32, 32),
    (40, 40),
    (48, 48),
    (64, 64),
    (128, 128),
    (256, 256),
)


def square_canvas(source: Image.Image) -> Image.Image:
    size = max(source.size)
    canvas = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    offset = ((size - source.width) // 2, (size - source.height) // 2)
    canvas.alpha_composite(source, offset)
    return canvas


def main() -> None:
    if not SOURCE.is_file():
        raise FileNotFoundError(f"Logo source not found: {SOURCE}")

    ICON_DIRECTORY.mkdir(parents=True, exist_ok=True)

    with Image.open(SOURCE) as input_image:
        logo = square_canvas(input_image.convert("RGBA"))

    logo.save(CANONICAL_LOGO, format="PNG", optimize=True)
    logo.resize((256, 256), Image.Resampling.LANCZOS).save(
        APPLICATION_PNG,
        format="PNG",
        optimize=True,
    )
    logo.save(
        APPLICATION_ICO,
        format="ICO",
        sizes=ICON_SIZES,
        bitmap_format="png",
    )

    print(f"Created {CANONICAL_LOGO.relative_to(PROJECT_ROOT)}")
    print(f"Created {APPLICATION_PNG.relative_to(PROJECT_ROOT)}")
    print(f"Created {APPLICATION_ICO.relative_to(PROJECT_ROOT)}")


if __name__ == "__main__":
    main()
