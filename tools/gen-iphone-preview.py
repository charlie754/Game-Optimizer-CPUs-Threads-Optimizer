"""Convert the Maps renderer's static iPhone WebP into a checked-in PNG header."""

import argparse
import base64
import hashlib
from io import BytesIO
import re
from pathlib import Path

from PIL import Image


SOURCE_COMMIT = "8c3b178"
PATTERN = re.compile(
    r"IPHONE_PREVIEW_DATA_URL\s*=\s*['\"]data:image/webp;base64,([^'\"]+)['\"]"
)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("source", type=Path)
    parser.add_argument("output", type=Path, nargs="?", default=Path("src/iphone_preview.h"))
    args = parser.parse_args()

    text = args.source.read_text(encoding="utf-8")
    match = PATTERN.search(text)
    if not match:
        raise SystemExit("IPHONE_PREVIEW_DATA_URL not found")
    webp = base64.b64decode(match.group(1), validate=True)
    png_path = args.output.with_suffix(".png")
    with Image.open(BytesIO(webp)) as image:
        out = BytesIO()
        image.convert("RGBA").save(out, format="PNG", optimize=False)
        png = out.getvalue()

    digest = hashlib.sha256(webp).hexdigest()
    values = ", ".join(f"0x{byte:02x}" for byte in png)
    lines = [
        "#pragma once",
        "#include <cstddef>",
        "#include <cstdint>",
        "",
        f"// Source: Maps extension/content/widget.js, commit {SOURCE_COMMIT}.",
        f"// Source WebP SHA-256: {digest}",
        "// Converted once to PNG for the native offscreen renderer; not part of the signed banner.",
        f"constexpr std::uint8_t kIphonePreviewPng[] = {{ {values} }};",
        "constexpr std::size_t kIphonePreviewPngSize = sizeof(kIphonePreviewPng);",
        "",
    ]
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text("\n".join(lines), encoding="utf-8", newline="\n")
    print(f"webp_sha256={digest}")
    print(f"png_bytes={len(png)}")
    print(f"png_path={png_path}")


if __name__ == "__main__":
    main()
