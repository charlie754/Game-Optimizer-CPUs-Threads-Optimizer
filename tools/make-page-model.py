"""Build the WebView2 page model from a verified Task 2 banner cache."""

import argparse
import base64
import hashlib
import json
from pathlib import Path


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--cache", required=True, type=Path)
    parser.add_argument("--out", required=True, type=Path)
    args = parser.parse_args()

    record = json.loads((args.cache / "record.json").read_text(encoding="utf-8"))
    current = record.get("current")
    if not current or not current.get("enabled"):
        raise SystemExit("cache has no enabled current banner")
    set_dir = args.cache / "sets" / current["sha256"]
    signed_path = set_dir / "v1.signed"
    signed_lines = signed_path.read_bytes().splitlines()
    if len(signed_lines) < 2:
        raise SystemExit("current signed file is incomplete")
    signed = json.loads(b"\n".join(signed_lines[1:]).decode("utf-8"))

    webp = set_dir / "photo.webp"
    webp_bytes = webp.read_bytes()
    expected_hash = current.get("photos", {}).get("webp", "")
    if expected_hash and hashlib.sha256(webp_bytes).hexdigest() != expected_hash:
        raise SystemExit("photo.webp does not match record.json")
    signed_hash = signed.get("photo", {}).get("sha256", {}).get("webp", "")
    if signed_hash and hashlib.sha256(webp_bytes).hexdigest() != signed_hash:
        raise SystemExit("photo.webp does not match the signed banner")

    effects = signed.get("effects", {})
    photo = signed.get("photo", {})
    banner = {
        "headline": signed.get("headline", ""),
        "wordmark": signed.get("wordmark", ""),
        "tagline": signed.get("tagline", []),
        "effects": {
            "viewfinder": bool(effects.get("viewfinder")),
            "steam": bool(effects.get("steam")),
        },
        "link": signed.get("link", ""),
        "labels": signed.get("labels", {}),
        "qr": {
            "size": signed.get("qr", {}).get("size", 37),
            "rows": signed.get("qr", {}).get("rows", []),
        },
        "photo": {
            "w": photo.get("w", 800),
            "h": photo.get("h", 498),
            "focus": photo.get("focus", [0.5, 0.5]),
            "textBox": photo.get("textBox", [0.0, 0.0, 1.0, 1.0]),
            "steamAt": photo.get("steamAt", [0.5, 0.5]),
        },
    }
    if "headlineEm" in signed:
        banner["headlineEm"] = signed["headlineEm"]
    if "badge" in signed:
        banner["badge"] = signed["badge"]
    model = {
        "kind": "online",
        "banner": banner,
        "photo": {
            "kind": "webp",
            "dataUrl": "data:image/webp;base64," + base64.b64encode(webp_bytes).decode("ascii"),
        },
    }
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(model, ensure_ascii=False, separators=(",", ":")), encoding="utf-8")
    print("model: online revision=%s webp=%d bytes -> %s" %
          (current.get("revision", "?"), len(webp_bytes), args.out))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
