// Game Optimizer - the GOAT Cam App Store QR, shared by the GDI sponsor strip (sponsor.cpp), the
// QR magnifier popup (qr_magnifier.cpp) and the WebView2 host (webview_host.cpp).
//
// Header-only and free of Win32 drawing, so tests\test_main.cpp can check the message parser
// below without linking anything.
//
// THE MATRIX
//   GENERATED from brand\goatcam-banner\assets\qr-matrix.json (its "rows" array, verbatim) by
//     python -I -c "import json; [print('    \"%s\",' % r) for r in json.load(open('qr-matrix.json'))['rows']]"
//   Do not edit by hand. 37 x 37 modules INCLUDING the 4-module quiet zone, '1' = dark.
//   [M] It decodes to https://apps.apple.com/app/id6816268316. Never re-encode it, never draw over
//   it, and draw every module on whole pixels.
//
// THE MESSAGE
//   The WebView2 page reports the pointer entering and leaving its QR tile with
//     window.chrome.webview.postMessage({goatcamQr: "enter" | "leave", rect: {x, y, w, h}})
//   - the tile's rectangle in CSS px inside the view - so the host can show the same native
//   magnifier the GDI strip shows. That string comes from a web page, so it is parsed STRICTLY:
//   exactly those two keys at the top, exactly x/y/w/h inside rect, finite JSON numbers with sane
//   bounds, no escapes, no extra keys, no duplicates, nothing after the closing brace. Anything
//   else is not this message and is ignored.
#pragma once

#include <cmath>
#include <cwchar>
#include <string>

namespace cd {
namespace goatcam_qr {

constexpr int kN = 37;
constexpr const char* kRows[kN] = {
    "0000000000000000000000000000000000000",
    "0000000000000000000000000000000000000",
    "0000000000000000000000000000000000000",
    "0000000000000000000000000000000000000",
    "0000111111101100010010101011111110000",
    "0000100000101010101001010010000010000",
    "0000101110101000011110011010111010000",
    "0000101110100010111101011010111010000",
    "0000101110101001000001110010111010000",
    "0000100000100111000011101010000010000",
    "0000111111101010101010101011111110000",
    "0000000000000100110110000000000000000",
    "0000100111111111100101000100101110000",
    "0000010111001001100110010000101100000",
    "0000011101100001010101100010001000000",
    "0000111111010110000111110001110010000",
    "0000101100110101111100011010000010000",
    "0000000010010110001100000011111110000",
    "0000111111100110101110110110101010000",
    "0000100100001100111100110110101010000",
    "0000000110111110100110111000010000000",
    "0000110111010100100110110100101100000",
    "0000111000111111111100010111110010000",
    "0000111101000011110000010000111000000",
    "0000111001101101010101011111111100000",
    "0000000000001000010111101000110000000",
    "0000111111101010110101111010110000000",
    "0000100000101010110011011000100100000",
    "0000101110101110010000011111110010000",
    "0000101110101100101100111101000010000",
    "0000101110100010101110001001101110000",
    "0000100000100110011100100011111010000",
    "0000111111101101001110111101100000000",
    "0000000000000000000000000000000000000",
    "0000000000000000000000000000000000000",
    "0000000000000000000000000000000000000",
    "0000000000000000000000000000000000000",
};

constexpr bool WellFormed() {
    for (int y = 0; y < kN; ++y) {
        if (kRows[y] == nullptr) return false;
        for (int x = 0; x < kN; ++x)
            if (kRows[y][x] != '0' && kRows[y][x] != '1') return false;
        if (kRows[y][kN] != '\0') return false;
    }
    return true;
}
static_assert(WellFormed(), "kRows must be 37 rows of exactly 37 '0'/'1' - regenerate it");

// The run of dark modules starting at column x of row y: returns its end (exclusive), or x when
// the module at x is light. Drawing one rectangle per run keeps the QR a few dozen fills.
inline int DarkRunEnd(int y, int x) {
    int e = x;
    while (e < kN && kRows[y][e] == '1') ++e;
    return e;
}

// ---------------------------------------------------------------------------
// The page's QR hover report.
// ---------------------------------------------------------------------------
enum class QrEvent { kEnter, kLeave };

struct QrMessage {
    QrEvent event = QrEvent::kLeave;
    double x = 0.0, y = 0.0, w = 0.0, h = 0.0;   // CSS px, inside the view
};

// The bounds a real tile can have. A rect outside them is not a tile.
constexpr double kMaxCoord = 100000.0;
constexpr double kMaxSide  = 4096.0;
constexpr size_t kMaxJson  = 512;

// QrCssRectToClient's sane ranges. Anything outside them is refused, never clamped into use.
constexpr int    kMinDpi   = 48;          // 50% scaling
constexpr int    kMaxDpi   = 960;         // 1000%
constexpr double kMinZoom  = 0.25;
constexpr double kMaxZoom  = 5.0;
constexpr long   kMaxBound = 16777216L;   // 2^24: view bounds beyond +-16M device px are not real

namespace detail {

// The length of `s`, reading AT MOST cap + 1 characters: a string longer than the cap - or one
// that is not terminated at all - comes back as cap + 1 and nothing past that is ever read.
inline size_t BoundedLen(const wchar_t* s, size_t cap) {
    size_t n = 0;
    while (n <= cap && s[n] != L'\0') ++n;
    return n;
}

struct Cursor {
    const wchar_t* s;
    const wchar_t* end;
};

inline void Ws(Cursor& c) {
    while (c.s < c.end && (*c.s == L' ' || *c.s == L'\t' || *c.s == L'\r' || *c.s == L'\n')) ++c.s;
}

inline bool Lit(Cursor& c, wchar_t ch) {
    Ws(c);
    if (c.s < c.end && *c.s == ch) { ++c.s; return true; }
    return false;
}

// A JSON string of plain printable ASCII, no escapes - every key and value in this message is
// one. An escape, a control character or a non-ASCII character is a refusal, not a decode.
inline bool Str(Cursor& c, std::wstring& out) {
    Ws(c);
    if (c.s >= c.end || *c.s != L'"') return false;
    ++c.s;
    out.clear();
    while (c.s < c.end && *c.s != L'"') {
        const wchar_t ch = *c.s;
        if (ch == L'\\' || ch < 0x20 || ch > 0x7e) return false;
        out.push_back(ch);
        if (out.size() > 32) return false;
        ++c.s;
    }
    if (c.s >= c.end) return false;
    ++c.s;   // the closing quote
    return true;
}

inline bool IsDigit(wchar_t ch) { return ch >= L'0' && ch <= L'9'; }

// A JSON number by the JSON grammar: -?(0|[1-9][0-9]*)(\.[0-9]+)?([eE][+-]?[0-9]+)? - so "01",
// ".5", "+1", "1." and hex are refused - and finite once converted.
inline bool Num(Cursor& c, double& out) {
    Ws(c);
    const wchar_t* p = c.s;
    if (p < c.end && *p == L'-') ++p;
    if (p >= c.end || !IsDigit(*p)) return false;
    if (*p == L'0') {
        ++p;
    } else {
        while (p < c.end && IsDigit(*p)) ++p;
    }
    if (p < c.end && *p == L'.') {
        ++p;
        if (p >= c.end || !IsDigit(*p)) return false;
        while (p < c.end && IsDigit(*p)) ++p;
    }
    if (p < c.end && (*p == L'e' || *p == L'E')) {
        ++p;
        if (p < c.end && (*p == L'+' || *p == L'-')) ++p;
        if (p >= c.end || !IsDigit(*p)) return false;
        while (p < c.end && IsDigit(*p)) ++p;
    }
    const size_t n = static_cast<size_t>(p - c.s);
    if (n == 0 || n > 40) return false;
    wchar_t buf[48];
    for (size_t i = 0; i < n; ++i) buf[i] = c.s[i];
    buf[n] = L'\0';
    wchar_t* stop = nullptr;
    const double v = std::wcstod(buf, &stop);
    if (stop != buf + n || !std::isfinite(v)) return false;
    out = v;
    c.s = p;
    return true;
}

}  // namespace detail

// True only for exactly {"goatcamQr":"enter"|"leave","rect":{"x":..,"y":..,"w":..,"h":..}}, keys
// in any order, insignificant whitespace allowed. `out` is untouched on false. The length check
// reads at most kMaxJson + 1 characters, so a huge or unterminated string is refused without
// being scanned (tests\test_main.cpp AM4b proves it against a guard page).
inline bool ParseQrMessage(const wchar_t* json, QrMessage* out) {
    using namespace detail;
    if (json == nullptr || out == nullptr) return false;
    const size_t n = BoundedLen(json, kMaxJson);
    if (n == 0 || n > kMaxJson) return false;
    Cursor c{ json, json + n };

    QrMessage m;
    bool haveEvent = false, haveRect = false;
    if (!Lit(c, L'{')) return false;
    for (;;) {
        std::wstring key;
        if (!Str(c, key) || !Lit(c, L':')) return false;
        if (key == L"goatcamQr" && !haveEvent) {
            std::wstring v;
            if (!Str(c, v)) return false;
            if (v == L"enter")      m.event = QrEvent::kEnter;
            else if (v == L"leave") m.event = QrEvent::kLeave;
            else return false;
            haveEvent = true;
        } else if (key == L"rect" && !haveRect) {
            if (!Lit(c, L'{')) return false;
            bool hx = false, hy = false, hw = false, hh = false;
            for (;;) {
                std::wstring k;
                double v = 0.0;
                if (!Str(c, k) || !Lit(c, L':') || !Num(c, v)) return false;
                if (k == L"x" && !hx)      { m.x = v; hx = true; }
                else if (k == L"y" && !hy) { m.y = v; hy = true; }
                else if (k == L"w" && !hw) { m.w = v; hw = true; }
                else if (k == L"h" && !hh) { m.h = v; hh = true; }
                else return false;   // an unknown or repeated key
                if (Lit(c, L',')) continue;
                if (Lit(c, L'}')) break;
                return false;
            }
            if (!(hx && hy && hw && hh)) return false;
            haveRect = true;
        } else {
            return false;            // an unknown or repeated key
        }
        if (Lit(c, L',')) continue;
        if (Lit(c, L'}')) break;
        return false;
    }
    Ws(c);
    if (c.s != c.end) return false;  // anything after the object
    if (!haveEvent || !haveRect) return false;
    if (!(m.w > 0.0 && m.h > 0.0 && m.w <= kMaxSide && m.h <= kMaxSide)) return false;
    if (std::fabs(m.x) > kMaxCoord || std::fabs(m.y) > kMaxCoord) return false;
    *out = m;
    return true;
}

// The tile's rectangle in the view's CSS px -> device px in the host's client coordinates.
// `bounds` is the WebView's bounds in that client area (ICoreWebView2Controller::get_Bounds,
// raw pixels), and one CSS px is dpi/96 * zoom device px - the WebView2 default of a
// rasterization scale that follows the monitor. Refused - false - when the result is not
// inside the bounds: a tile the view cannot be showing is not one to magnify.
//
// NOTHING HERE CAN OVERFLOW. dpi and zoom must sit inside kMinDpi..kMaxDpi / kMinZoom..kMaxZoom,
// every input must be finite, the bounds must be ordered and within +-kMaxBound, and all the
// arithmetic is done in double; no endpoint is rounded to an integer until it is known to lie
// inside the view (with one device pixel of slack for rounding), and the rounded result is then
// clamped to the view, so what comes back is always inside it.
inline bool QrCssRectToClient(const QrMessage& m, long bLeft, long bTop, long bRight, long bBottom,
                              int dpi, double zoom, long* l, long* t, long* r, long* b) {
    if (l == nullptr || t == nullptr || r == nullptr || b == nullptr) return false;
    if (dpi < kMinDpi || dpi > kMaxDpi) return false;
    if (!std::isfinite(zoom) || zoom < kMinZoom || zoom > kMaxZoom) return false;
    if (!std::isfinite(m.x) || !std::isfinite(m.y) || !std::isfinite(m.w) || !std::isfinite(m.h)) return false;
    if (!(m.w > 0.0) || !(m.h > 0.0)) return false;
    if (bLeft >= bRight || bTop >= bBottom) return false;
    if (bLeft < -kMaxBound || bTop < -kMaxBound || bRight > kMaxBound || bBottom > kMaxBound) return false;

    const double k  = dpi / 96.0 * zoom;
    const double bl = static_cast<double>(bLeft), bt = static_cast<double>(bTop);
    const double br = static_cast<double>(bRight), bb = static_cast<double>(bBottom);
    const double L = bl + m.x * k, T = bt + m.y * k;
    const double R = bl + (m.x + m.w) * k, B = bt + (m.y + m.h) * k;
    if (!std::isfinite(L) || !std::isfinite(T) || !std::isfinite(R) || !std::isfinite(B)) return false;
    if (L < bl - 1.0 || T < bt - 1.0 || R > br + 1.0 || B > bb + 1.0) return false;
    if (!(R > L) || !(B > T)) return false;

    // Every endpoint is now within the view +-1 px, so well inside the range of a long.
    long Li = std::lround(L), Ti = std::lround(T), Ri = std::lround(R), Bi = std::lround(B);
    if (Li < bLeft) Li = bLeft;
    if (Ti < bTop) Ti = bTop;
    if (Ri > bRight) Ri = bRight;
    if (Bi > bBottom) Bi = bBottom;
    if (Ri <= Li || Bi <= Ti) return false;   // nothing left once rounded
    *l = Li; *t = Ti; *r = Ri; *b = Bi;
    return true;
}

}  // namespace goatcam_qr
}  // namespace cd
