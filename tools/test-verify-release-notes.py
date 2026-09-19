# Regression checks for the release-notes grammar and the nine review findings.
# Fixtures use real archives, including duplicate metadata entries, and actual hashes.

import hashlib
from pathlib import Path
import subprocess
import sys
import tempfile
import warnings
import zipfile

ROOT = Path(__file__).resolve().parents[1]
CHECKER = ROOT / "tools" / "verify-release-notes.ps1"
ZIP_NAME = "pkg-v9.9.9-x64.zip"
EMPTY = hashlib.sha256(b"").hexdigest().upper()
ABC = hashlib.sha256(b"abc").hexdigest().upper()
STD = [("GameOptimizer.exe", b""), ("WebView2Loader.dll", b"abc")]


def block(rows=None):
    if rows is None:
        rows = f"{EMPTY} GameOptimizer.exe\n{ABC} WebView2Loader.dll\n"
    return "```\nSHA256\n{ZIP_SHA256} " + ZIP_NAME + "\n" + rows + "```\n"


def build(work, case, entries, notes_text, encoding="utf-8"):
    """Return notes and archive paths; fill the archive hash after writing it."""
    directory = work / case
    directory.mkdir()
    archive = directory / ZIP_NAME
    with warnings.catch_warnings():
        warnings.filterwarnings("ignore", message="Duplicate name:", category=UserWarning)
        with zipfile.ZipFile(archive, "w") as package:
            for name, data in entries:
                package.writestr(name, data)
    notes = directory / "notes.md"
    notes.write_bytes(notes_text.replace(
        "{ZIP_SHA256}", hashlib.sha256(archive.read_bytes()).hexdigest().upper()
    ).encode(encoding))
    return notes, archive


def run(notes, archive, require=None):
    command = ["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass"]
    if require is None:
        command += ["-File", str(CHECKER), "-Notes", str(notes), "-Zip", str(archive)]
    else:
        # Windows PowerShell -File cannot transport an array parameter from native argv.
        def literal(value):
            return "'" + str(value).replace("'", "''") + "'"

        command += ["-Command", "& " + literal(CHECKER) + " -Notes " + literal(notes)
                    + " -Zip " + literal(archive) + " -Require @("
                    + ",".join(literal(name) for name in require) + "); exit $LASTEXITCODE"]
    process = subprocess.run(command, capture_output=True, text=True, errors="replace")
    return process.returncode, process.stdout + process.stderr


# Each failure checks its diagnostic too: a syntax/runtime error is not evidence
# that the intended hash, encoding, archive, or coverage check ran.
cases = []


def case(name, entries, notes, want, diagnostic="", require=None, encoding="utf-8"):
    cases.append((name, entries, notes, want, diagnostic, require, encoding))


# 1-3 retain the adversarial prose, alongside a valid block. Prose cannot pair hashes.
case("1_two_claims_one_line", STD, block()
     + f"GameOptimizer.exe: {EMPTY}; WebView2Loader.dll: {EMPTY}\n",
     1, "64-hex run outside the fenced block")
case("2_orphan_second_hash", STD, block()
     + f"SHA256 GameOptimizer.exe\n{EMPTY}\n\nAnother SHA256 claim for the same file:\n{ABC}\n",
     1, "64-hex run outside the fenced block")
case("3_comment_steals_pairing", STD, block()
     + "SHA256 for GameOptimizer.exe\n<!-- Package also contains WebView2Loader.dll. -->\n"
     + ABC + "\n", 1, "64-hex run outside the fenced block")
case("4_uppercase_ext", STD,
     block(f"{EMPTY} GameOptimizer.exe\n{EMPTY} WebView2Loader.DLL\n"),
     1, "WebView2Loader.DLL: notes say")
case("5_prefix_match", [("GameOptimizer.exe", b""), ("GameOptimizer.exe.config", b"abc")],
     block(f"{EMPTY} GameOptimizer.exe\n{EMPTY} GameOptimizer.exe.config\n"),
     1, "GameOptimizer.exe.config: notes say")
case("6_duplicate_zip_entries", [("GameOptimizer.exe", b"abc"), ("GameOptimizer.exe", b"")],
     block(f"{EMPTY} GameOptimizer.exe\n"), 1, "duplicate zip entry name")

good_line = "A genuine em-dash \u2014 right here.\n"
mojibake = "Corrupted: \u00e2\u20ac\u201d and \u00f0\u0178\u201d\u00b4\n"
case("7_mixed_mojibake", STD, good_line + mojibake + block(),
     1, "double-encoded UTF-8 (mojibake) on line 2")
# CP1252 C4 80 decodes to U+0100 (A with macron), outside the high-Latin range.
case("8_per_line_mojibake", STD, good_line + "Corrupted accent: \u00c4\u20ac\n" + block(),
     1, "double-encoded UTF-8 (mojibake) on line 2")
# The new grammar requires explicitly labeling an intentional quotation "mojibake".
case("9_quoting_mojibake", STD, block()
     + "Fixed a mojibake bug where the page rendered \u00e2\u20ac\u201d instead of an em-dash.\n", 0)
case("10_require_missing", STD, block(), 1, "Missing.dll: required file must appear exactly once",
     require=[ZIP_NAME, "GameOptimizer.exe", "Missing.dll"])
case("0_control_good", STD, block(), 0)

# Grammar and coverage boundaries prevent unrelated errors from hiding regressions.
case("11_zero_blocks", STD, "No hashes here.\n", 1, "exactly one fenced code block")
case("12_multiple_blocks", STD, block() + "```text\nSHA256\n```\n",
     1, "exactly one fenced code block")
case("13_header_not_first", STD, block().replace("SHA256\n", "\nSHA256\n"),
     1, "first line inside the fenced block must be exactly SHA256")
case("14_bad_header_case", STD, block().replace("SHA256\n", "sha256\n"),
     1, "first line inside the fenced block must be exactly SHA256")
case("15_malformed_claim", STD, block().replace(f"{ABC} Web", f" {ABC} Web"),
     1, "malformed hash claim on line 5")
case("16_unclosed_block", STD, block()[:-4], 1, "fenced code block is not closed")
case("17_required_duplicate", STD, block()[:-4] + f"{EMPTY} GAMEOPTIMIZER.EXE\n```\n",
     1, "GameOptimizer.exe: required file must appear exactly once (found 2)")
case("18_required_exe_missing", STD, block(f"{ABC} WebView2Loader.dll\n"),
     1, "GameOptimizer.exe: required file must appear exactly once (found 0)")
case("19_required_zip_missing", STD, "```\nSHA256\n" + f"{EMPTY} GameOptimizer.exe\n```\n",
     1, ZIP_NAME + ": required file must appear exactly once (found 0)")
case("20_ambiguous_leaf", STD + [("sub/WEBVIEW2LOADER.DLL", b"other")],
     block(), 1, "ambiguous leaf in zip")
case("21_leaf_case_control", [("sub/GameOptimizer.exe", b""), ("lib/WebView2Loader.dll", b"abc")],
     "<!-- GameOptimizer.exe.config and unrelated.dll do not affect pairing. -->\n"
     + block(f"{EMPTY.lower()} dir/GAMEOPTIMIZER.EXE\n{ABC.lower()} lib\\WEBVIEW2LOADER.DLL\n")
     .replace("```\nSHA256", "```text\nSHA256").replace("\n```\n", "\n\t\n```\n"), 0)
case("22_require_override", STD, "```\nSHA256\n" + f"{ABC} WebView2Loader.dll\n```\n",
     0, require=["dir/WEBVIEW2LOADER.DLL"])
case("23_bom", STD, block(), 1, "notes begin with a UTF-8 BOM", encoding="utf-8-sig")
case("24_invalid_utf8", STD, "caf\u00e9\n" + block(), 1, "notes are not valid UTF-8", encoding="cp1252")
case("25_genuine_unicode", STD, "caf\u00e9 and \u4e2d\u6587 and \U0001f534\n" + good_line + block(), 0)
case("26_block_known_limit", STD + [("mojibake-\u00e2\u20ac\u201d.txt", b"quoted")],
     block()[:-4] + hashlib.sha256(b"quoted").hexdigest()
     + " mojibake-\u00e2\u20ac\u201d.txt\n```\n", 0, "[KNOWN-LIMIT] line 6")
# [KNOWN-LIMIT] The specified no-high-Latin-left predicate exempts cafÃ© -> café.
# This original finding cannot be rejected without changing the decided predicate.
case("27_latin1_known_limit", STD, "caf\u00c3\u00a9\n" + block(), 0)
case("28_two_claims_in_block", STD, block()[:-4]
     + f"{EMPTY} GameOptimizer.exe {ABC} WebView2Loader.dll\n```\n",
     1, "malformed hash claim on line 6")
case("29_long_hex_outside", STD, block() + "0" * 65 + "\n",
     1, "64-hex run outside the fenced block")
case("30_shared_hash", [("GameOptimizer.exe", b""), ("other.dll", b"")],
     block(f"{EMPTY} GameOptimizer.exe\n{EMPTY} other.dll\n"),
     1, "one hash claimed for several files")
case("31_unknown_file", STD, block()[:-4] + f"{ABC} absent.dll\n```\n",
     1, "absent.dll: named in the notes but not present in the zip")
case("32_placeholder", STD, "<filled at release time>\n" + block(),
     1, "an unfilled placeholder remains")
case("33_uppercase_fence", STD, block().replace("```\nSHA256", "```TEXT\nSHA256"),
     1, "64-hex run outside the fenced block")
case("35_unclaimed_duplicate_leaf", [
     ("app/GameOptimizer.exe", b""), ("app/README.md", b"abc"), ("app/sub/README.md", b"other")],
     block(f"{EMPTY} GameOptimizer.exe\n"), 0,
     "info  README.md: ambiguous leaf in zip - unclaimed and not required")


def main():
    print("EMPTY sha256 =", EMPTY)
    print("ABC   sha256 =", ABC)
    print()
    print("%-26s %-9s %-9s %s" % ("case", "expected", "actual", "verdict"))
    print("-" * 78)
    results = []
    # TemporaryDirectory owns exactly its newly created directory and cleans it on exit.
    with tempfile.TemporaryDirectory(prefix="verify-notes-tests-") as directory:
        for name, entries, notes, want, diagnostic, require, encoding in cases:
            notes_path, archive = build(Path(directory), name, entries, notes, encoding)
            actual, output = run(notes_path, archive, require)
            ok = actual == want and diagnostic in output and "=== RESULT:" in output
            results.append((name, want, actual, ok, diagnostic, output))
            print("%-26s %-9d %-9d %s" % (
                name, want, actual, "as predicted" if ok else "*** DIFFERS ***"))
    print()
    print("=" * 78)
    for name, want, actual, ok, diagnostic, output in results:
        if not ok:
            print(f"\n### {name}  expected {want} got {actual}")
            print(f"    required diagnostic: {diagnostic!r}")
            print("    ---- checker output ----")
            for line in output.splitlines():
                if line.strip():
                    print("    " + line)
    passed = sum(result[3] for result in results)
    print(f"\nTOTAL {len(results)} PASSED {passed} FAILED {len(results) - passed}")
    return 0 if passed == len(results) else 1


if __name__ == "__main__":
    sys.exit(main())
