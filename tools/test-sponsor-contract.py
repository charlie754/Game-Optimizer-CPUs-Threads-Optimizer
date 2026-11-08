#!/usr/bin/env python3
"""Decode the checked-in sponsor page and prove the 0.5.9 control boundary."""

import hashlib
import re
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
HEADER = ROOT / "src" / "sponsor_html.h"
OUTPUT = ROOT / "build" / "panel" / "panel-app-061.html"
EXPECTED = "cec0d787dfec87acc917f19ba4f2d2161c7f88aa50d2fd90ccbc7ba053f7206f"


def decode_header():
    source = HEADER.read_text(encoding="utf-8")
    pieces = []
    literal = re.compile(r'out\s*\+=\s*L"((?:\\.|[^"\\])*)"\s*;')
    for match in literal.finditer(source):
        pieces.append(bytes(match.group(1), "utf-8").decode("unicode_escape"))
    if not pieces:
        raise SystemExit("C1=FAIL no sponsor page literals found in src/sponsor_html.h")
    return "".join(pieces)


def contract(page):
    start = page.index('<button type="button" class="kofi">')
    gh = page.index('<button type="button" class="gh">', start)
    end = page.index("</button>", gh) + len("</button>")
    markup = page[start:end]
    page_start = page.index("<script type='text/plain' id='p'><style>")
    style_start = page_start + len("<script type='text/plain' id='p'><style>")
    style_end = page.index("</style>", style_start)
    stylesheet = page[style_start:style_end]
    selector = re.compile(r"\.(?:kofi|gh)(?:[-_:.#\[]|[\s>{+~]|$)")
    rules = []
    for match in re.finditer(r"([^{}]+)\{([^{}]*)\}", stylesheet):
        if selector.search(match.group(1)):
            rules.append(match.group(1).strip() + "{" + match.group(2) + "}")
    digest = hashlib.sha256((markup + "\n" + "\n".join(rules)).encode("utf-8")).hexdigest()
    return digest, len(markup), len(rules)


page = decode_header()
digest, markup_len, rule_count = contract(page)
OUTPUT.parent.mkdir(parents=True, exist_ok=True)
OUTPUT.write_text(page, encoding="utf-8")
if digest != EXPECTED:
    raise SystemExit("C1=FAIL expected %s got %s markup=%d rules=%d" %
                     (EXPECTED, digest, markup_len, rule_count))
print("C1=PASS legacy Ko-fi/Star contract sha256=%s markup=%d rules=%d" %
      (digest, markup_len, rule_count))
print("page=%s bytes=%d" % (OUTPUT.relative_to(ROOT), OUTPUT.stat().st_size))
