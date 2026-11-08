// Game Optimizer - the sponsor strip. Implementation of src\sponsor.h.
//
// WHAT IS IN THE STRIP
// --------------------
// One row: Ko-fi, Star on GitHub, and the GOAT Cam banner - the headline "GOAT Cam - your
// iPhone, NOW your PC's webcam." beside a dark plate that the site's own hero photo fills edge to
// edge: the cup on the left, the "GOAT Cam" wordmark and "IPHONE APP . ON THE APP STORE" over the
// laptop's dark screen, viewfinder brackets and a 1080p . 60 fps readout, and the App Store QR on
// the right. ONLY THE PLATE IS LIVE; the headline is text, and hovering it plays nothing. Hovering
// the QR tile - or keyboard focus on the plate - opens the QR magnifier (qr_magnifier.h), because
// a 76 px band has no room to grow the code in place.
//
// This is the GDI FALLBACK. The Settings window shows it only when WebView2 is unavailable
// (see webview_host.h); the WebView2 page in src\sponsor_html.h is the primary rendering.
//
// WHAT THIS FILE IS A PORT OF
// ---------------------------
// Nothing here is drawn by eye. The sources are:
//
//   brand\goatcam-banner\SPEC.md section 7 (the v2 redesign, founder feedback 2026-10-26) and the
//       site's assets: the photo is site\public\assets\hero-coffee-800.jpg, unchanged.
//   F:\mac-fan-control-windows\src\goatcambanner.cpp - the native GDI+ port of the v1 banner,
//       whose photo, viewfinder, OSD and QR drawing this reshapes into this control's
//       one-window / one-timer / one-back-buffer design.
//   F:\google map plugin\extension\options\options.css + options.html - the Ko-fi button:
//       inline-flex, 8px gap, 9px/16px padding, radius 6, two-line label, hover background swap,
//       :active translateY(1px) - and its cup icon path.
//
// DRAWING
// -------
// Each control owns one 32bpp DIB as its back buffer (Layer below), so nothing flickers. GDI
// draws the Ko-fi and Star buttons, every piece of text (ClearType and letter-spacing, which
// GDI+ lacks) and the QR modules, which must land on exact integer pixels to stay scannable.
// GDI+ draws what needs anti-aliasing or alpha - the plate, the photo, the scrim behind the
// wordmark, the brackets and the OSD pill - straight into the same bits. GDI has no path parser,
// so ParsePath() reads the Ko-fi cup's SVG path once, in source units.
//
// WITHOUT GDI+ the strip still draws, in plain GDI: the headline, the plate and its border, the
// wordmark, the tagline and a scannable QR. Only the photo, the brackets and the OSD need GDI+,
// and only they are missing. See EnsureGdiplus() and PaintPlateGdi().
//
// THE CPU RULE, WHICH OUTRANKS ALL OF THE ABOVE
// ---------------------------------------------
// This application exists to SAVE CPU. The 16 ms timer exists only while a transition is still
// running: GoatMotion::Busy() is the single gate and SyncTimer() KILLS the timer the moment it
// goes false, so the strip at rest - hovered or not - costs nothing. The web banner's LOOPS (the
// steam over the cup, the pulsing OSD dot) are deliberately NOT ported: each would hold the timer
// open for as long as the pointer stays, in the one program whose purpose is not doing that.
// Ko-fi and Star change colour on hover and have no motion at all.
#define NOMINMAX
#include "sponsor.h"
#include "banner_host.h"

#include <commctrl.h>
#include <shellapi.h>
#include <objidl.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <memory>
#include <new>
#include <string>
#include <vector>

namespace Gdiplus { using std::min; using std::max; }
#include <gdiplus.h>
#include <delayimp.h>

#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "delayimp.lib")
// tools\build.bat links gdiplus.dll DELAY-LOADED (/DELAYLOAD:gdiplus.dll - the linker refuses
// /DELAYLOAD from a #pragma, LNK4229), because only this fallback strip uses it and a tray app
// that runs all day should not map it for the common case where WebView2 draws the strip.
//
// A DELAY-LOAD THAT FAILS DOES NOT RETURN AN ERROR - IT RAISES. The first call into a DLL that
// will not load throws VcppException(ERROR_SEVERITY_ERROR, ERROR_MOD_NOT_FOUND) from the
// delay-load helper, and unhandled that kills the tray app the moment this strip paints. So
// EnsureGdiplus() binds every gdiplus import up front, under an SEH guard, BEFORE the first
// GdiplusStartup; if that fails the strip is drawn in plain GDI and GDI+ is never touched.
// ponytail: the probe runs once per process and the answer is cached - a gdiplus.dll that
// appears later is not noticed until the app restarts.

#include "theme.h"
#include "iphone_preview.h"
#include "qr_magnifier.h"
#include "util.h"

#pragma comment(lib, "msimg32.lib")

namespace cd {

// ===========================================================================
// Reduced motion
// ===========================================================================
namespace {
bool g_animKnown = false;
bool g_animValue = true;
}  // namespace

bool AnimationsEnabled() {
    if (!g_animKnown) {
        BOOL v = TRUE;
        if (!SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &v, 0)) v = TRUE;
        g_animValue = (v != FALSE);
        g_animKnown = true;
    }
    return g_animValue;
}

namespace {
void ForgetAnimationPreference() { g_animKnown = false; }
}  // namespace

// ===========================================================================
// Easing - the two cubic-bezier curves from the stylesheet, solved properly.
// A linear stand-in loses the snap; the header says so and it is right.
// ===========================================================================
namespace sponsor_motion {
namespace {

double BezComponent(double t, double a, double b) {
    const double u = 1.0 - t;
    return 3.0 * u * u * t * a + 3.0 * u * t * t * b + t * t * t;
}

double BezDerivative(double t, double a, double b) {
    const double u = 1.0 - t;
    return 3.0 * u * u * a + 6.0 * u * t * (b - a) + 3.0 * t * t * (1.0 - b);
}

double CubicBezier(double x, double x1, double y1, double x2, double y2) {
    if (x <= 0.0) return 0.0;
    if (x >= 1.0) return 1.0;
    double t = x;
    for (int i = 0; i < 8; ++i) {
        const double err = BezComponent(t, x1, x2) - x;
        if (err < 1e-7 && err > -1e-7) break;
        const double d = BezDerivative(t, x1, x2);
        if (d < 1e-7 && d > -1e-7) break;
        t -= err / d;
        if (t < 0.0) t = 0.0;
        if (t > 1.0) t = 1.0;
    }
    return BezComponent(t, y1, y2);
}

}  // namespace

// --gl-in / --gc-in: cubic-bezier(0.16, 1, 0.3, 1)
double EaseIn(double t) { return CubicBezier(t, 0.16, 1.0, 0.3, 1.0); }

// --gl-out / --gc-out: cubic-bezier(0.4, 0, 0.2, 1)
double EaseOut(double t) { return CubicBezier(t, 0.4, 0.0, 0.2, 1.0); }

}  // namespace sponsor_motion

// ===========================================================================
namespace {

namespace gp = Gdiplus;
using sponsor_motion::EaseIn;
using sponsor_motion::EaseOut;

// ---- small numeric helpers (windows.h min/max are suppressed by NOMINMAX) --
inline int    IMax(int a, int b)          { return a > b ? a : b; }
inline double DMax(double a, double b)    { return a > b ? a : b; }
inline double Clamp01(double v)           { return v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v); }
inline int    RoundI(double v)            { return static_cast<int>(v < 0.0 ? v - 0.5 : v + 0.5); }

// One 96-dpi design pixel at `dpi`, for the FRACTIONAL sizes the stylesheet uses (10.5px, 5.4em).
// Whole-pixel sizes go through theme::Dp like everything else in the app.
inline int Px(double logical, int dpi) { return RoundI(logical * dpi / 96.0); }

RECT ScaleRectAboutCenter(const RECT& source, double scale) {
    const double cx = (source.left + source.right) / 2.0;
    const double cy = (source.top + source.bottom) / 2.0;
    const double w = (source.right - source.left) * scale;
    const double h = (source.bottom - source.top) * scale;
    return { static_cast<LONG>(std::floor(cx - w / 2.0)),
             static_cast<LONG>(std::floor(cy - h / 2.0)),
             static_cast<LONG>(std::ceil(cx + w / 2.0)),
             static_cast<LONG>(std::ceil(cy + h / 2.0)) };
}

inline BYTE Chan(double v) {
    const int i = RoundI(v);
    return static_cast<BYTE>(i < 0 ? 0 : (i > 255 ? 255 : i));
}

// ---------------------------------------------------------------------------
// Ko-fi - options.css :root: --kofi / --kofi-hover / --kofi-text
// ---------------------------------------------------------------------------
constexpr COLORREF kKofiBg      = RGB(0xd2, 0x41, 0x3e);
constexpr COLORREF kKofiBgHover = RGB(0xbb, 0x32, 0x2f);
constexpr COLORREF kKofiText    = RGB(0xff, 0xff, 0xff);

// ---------------------------------------------------------------------------
// GOAT Cam - the `.goat` tokens in goatcam-banner.css, which are goatcam.dagoat.io's own
// styles.css :root ("Golden-hour Desk").
// ---------------------------------------------------------------------------
struct Rgb { BYTE r, g, b; };
constexpr Rgb kEspresso950 = {0x14, 0x0d, 0x08};   // --gc-950: the plate, the scrim
constexpr Rgb kEspresso800 = {0x26, 0x1a, 0x11};   // the plate's fill if the photo cannot load
constexpr Rgb kCaramel     = {0xc9, 0x8a, 0x46};   // --gc-caramel: the QR ring
constexpr Rgb kGold        = {0xf0, 0xc0, 0x7a};   // --gc-gold: NOW, OSD dot, bracket lock, lift ring
constexpr Rgb kCream       = {0xf8, 0xee, 0xdf};   // --gc-cream: wordmark, brackets, OSD text
constexpr Rgb kMuted       = {0xb0, 0x98, 0x78};   // --gc-muted: the tagline at rest
constexpr Rgb kQrLight     = {0xfb, 0xf5, 0xea};   // the QR tile
constexpr Rgb kQrDark      = {0x1c, 0x13, 0x0c};   // the QR modules
// The headline: --ink-cream exactly as THIS app's WebView2 page defines it (src\sponsor_html.h,
// :host), so the two renderings of the strip agree on the window's own dark background.
constexpr Rgb kInkCream    = {0xf0, 0xe6, 0xd2};

inline COLORREF  Cr(Rgb c) { return RGB(c.r, c.g, c.b); }
inline gp::Color Gc(Rgb c, double a = 1.0) { return gp::Color(Chan(Clamp01(a) * 255.0), c.r, c.g, c.b); }
inline Rgb Mix(Rgb a, Rgb b, double t) {
    t = Clamp01(t);
    return Rgb{Chan(a.r + (b.r - a.r) * t), Chan(a.g + (b.g - a.g) * t), Chan(a.b + (b.b - a.b) * t)};
}

// ---------------------------------------------------------------------------
// Ko-fi and Star on GitHub - THE SIZE KNOB, UNCHANGED.
//
// Operator instruction: the buttons at 70% of the size they had. Every box metric below is its
// source value times kStripScale, through Sc().
//
// KO-FI AND STAR KEEP EXACTLY THE HEIGHT THEY HAD. That height was max(their own natural
// height, kBtnMarkH + Sc(6)) - 45 px at 96 dpi - where 41 was the mark height of the GOATPROJECT
// lockup they used to sit beside. The row is now taller (the GOAT Cam plate, below) and the two
// buttons sit centred in it, as they do in the web strip.
// ---------------------------------------------------------------------------
constexpr double kStripScale = 0.70;
constexpr int    kBtnMarkH   = 41;

// options.css, at the reference scale. Everything here goes through Sc().
constexpr int kKofiPadY   = 9;
constexpr int kKofiPadX   = 16;
constexpr int kKofiIcoGap = 8;
constexpr int kKofiIco    = 20;
constexpr int kKofiRadius = 6;
constexpr int kKofiFontPx = 14;   // [M] ref: 19-char title inks 147px, lines pitch 20px
constexpr double kKofiLineHeight = 1.4;

constexpr int kGhPadX   = 14;
constexpr int kGhIcoGap = 8;
constexpr int kGhIco    = 16;

// One logical source value at 70%. Ko-fi and Star box metrics only.
inline int Sc(int logical) { return IMax(1, RoundI(logical * kStripScale)); }

// ---------------------------------------------------------------------------
// GOAT Cam - SPEC section 7, in 96-dpi px.
//
// THE ROW HEIGHT IS THE QR's, NOT A CHOICE. The App Store QR is 37 modules INCLUDING its quiet
// zone and must land on whole pixels to scan; at the 2 px/module floor that is 74 px. The GDI
// fallback reserves a 4 px edge margin around that tile, plus the plate border, so its band is
// 84 px at 96 dpi and scales cleanly at higher DPIs.
//
// THE PLATE WIDTH IS THE PHOTO's. The hero photo is cover-fitted across the WHOLE plate, so its
// scale is plateW / 800 and the band shows 84 / scale source rows of it. The cup and saucer are
// 101 source rows tall ([M] below), so they fit the band with room only while the scale stays
// near 0.5 - a 400 px plate at 96 dpi, Dp(400) at any other, since both sides scale together.
// A much wider plate would crop the cup; a much narrower one would crowd the wordmark into the
// QR. And 400 keeps the whole strip inside the minimum Settings window at every dpi (checked by
// the harness's 96..480 sweep).
// ---------------------------------------------------------------------------
constexpr int    kPlateH      = 84;    // 74px QR + 4px edge margin on each side
constexpr int    kPlateW      = 400;
constexpr int    kPlateRadius = 10;    // border-radius: 10px
constexpr int    kGroupGap    = 16;    // Star -> the headline
constexpr int    kCopyGap     = 10;    // .goat--strip { gap: 10px }: headline -> plate
constexpr double kCopyPx      = 10.5;  // .goat__line { font-size: 10.5px; line-height: 1.4 }
constexpr double kCopyLineH   = 1.4;
constexpr double kNowEm       = 1.14;  // .goat__line em { font: italic 700 1.14em/1 serif }
constexpr double kNamePx      = 18.0;  // --gc-name, line-height 1
// --gc-tag is 7px (line-height 1.25, tracked .14em). [M] GDI ClearType at 7px rendered the
// tagline too faint to read at 96 dpi in the offscreen harness, so 8 - MacFanControl's size.
constexpr double kTagPx       = 8.0;
constexpr double kTagLineH    = 1.25;
constexpr double kTagTrackEm  = 0.14;
constexpr int    kWordGap     = 3;     // the wordmark -> the tagline
constexpr int    kQrSide      = 74;    // 37 modules x 2 px at 96 dpi (web --gc-qr: 72 css px)
constexpr int    kQrPadR      = 11;    // keep the QR inside the closing viewfinder bracket
constexpr int    kQrRadius    = 8;     // --gc-qr-r: 8px
constexpr int    kQrGap       = 8;     // the wordmark and the viewfinder stop this far from the QR
constexpr int    kVfIn        = 5;     // --gc-vf-in: 5px
constexpr int    kVfArm       = 8;     // --gc-arm: 8px
constexpr int    kVfBar       = 2;     // border: 2px
constexpr int    kVfLock      = 3;     // the hover tightens the viewfinder by 3px
constexpr double kOsdPx       = 6.5;   // .gc-osd { font: 500 6.5px/1 mono }
constexpr double kScrimA      = 0.82;  // the wordmark scrim's peak opacity (--gc-950)

// THE PHOTO - site\public\assets\hero-coffee-800.jpg (800 x 498), in its own source pixels.
// [M] Measured 2026-10-26 by a pixel scan of that file (python -I, PIL): the cup and saucer's
// bright pixels span x 20..190, y 283..384; the laptop screen is near-black (luminance <= 19)
// across x 262..572, y 125..328.
constexpr double kSrcW = 800.0, kSrcH = 498.0;
constexpr double kSrcCupL = 20.0, kSrcCupR = 190.0, kSrcCupT = 283.0, kSrcCupB = 384.0;
// The band is set as LOW as still holds the cup, so as much dark screen as possible shows above
// it for the wordmark: 6 source rows below the saucer's bright rim keep its rim and a sliver of
// its shadow inside the plate.
constexpr double kSrcCupMargin = 6.0;
constexpr double kSrcScreenL = 262.0, kSrcScreenR = 572.0, kSrcScreenB = 328.0;
constexpr double kSrcTextInset = 16.0;   // the wordmark starts this far inside the screen's edge
// The lift. The web plate scales to 1.02 with a shadow and a 1px gold ring; this row has no
// room above or below a 76 px plate, so here the lift is the RING alone - the border warms to
// gold - and nothing moves. Scaling would also resample the QR off its integer module grid.
constexpr double kLiftRingA = 0.45;

// GOAT Cam v1 is camera-only: never a microphone, never a price.
const wchar_t* const kCopy1    = L"GOAT Cam \u2014 your iPhone,";
const wchar_t* const kCopyNow  = L"NOW";
const wchar_t* const kCopy2    = L" your PC\u2019s webcam.";
const wchar_t* const kGoatName = L"GOAT Cam";
const wchar_t* const kDaGoatName = L"GOATPROJECT";
const wchar_t* const kDaGoatTag  = L"THE PEOPLE\u2019S COMPUTE COMMONS";
const wchar_t* const kDaGoatCopy1 =
    L"Goat Project - Help fight Cancer, Alzheimer’s, Parkinson’s, COVID-19, Dengue, Hepatitis C etc.";
const wchar_t* const kDaGoatCopy2 = L"Contribute your idle compute to Earn GOAT.";
const wchar_t* const kGoatTag  = L"IPHONE APP \u00b7 ON THE APP STORE";
const wchar_t* const kGoatTag1 = L"IPHONE APP";         // the same tagline on two lines, when
const wchar_t* const kGoatTag2 = L"ON THE APP STORE";   // one line would reach the QR
const wchar_t* const kOsd      = L"1080p \u00b7 60 fps";

// The signed banner's QR is always 37 x 37. Its rows are part of the verified model; the
// built-in DaGoat strip intentionally has no QR and never consults the old compiled-in asset.
constexpr int kQrN = 37;
int g_dpiOverride = 0;

int DpiOfWnd(HWND hwnd) {
    if (g_dpiOverride > 0) return g_dpiOverride;
    typedef UINT(WINAPI * PFN_GetDpiForWindow)(HWND);
    static PFN_GetDpiForWindow fn = []() -> PFN_GetDpiForWindow {
        HMODULE u = GetModuleHandleW(L"user32.dll");
        if (u == nullptr) return nullptr;
        return reinterpret_cast<PFN_GetDpiForWindow>(
            reinterpret_cast<void*>(GetProcAddress(u, "GetDpiForWindow")));
    }();
    if (fn != nullptr && hwnd != nullptr) {
        const UINT d = fn(hwnd);
        if (d != 0) return static_cast<int>(d);
    }
    HDC dc = GetDC(hwnd);
    int d = 96;
    if (dc != nullptr) {
        const int got = GetDeviceCaps(dc, LOGPIXELSX);
        if (got > 0) d = got;
        ReleaseDC(hwnd, dc);
    }
    return d;
}

// ===========================================================================
// Fonts.
//
// Pixel sizes and weights from the stylesheets, so this file owns its faces. They live in
// SponsorState (and in a local for SponsorMeasure), so every HFONT is deleted with the object
// that created it.
// ===========================================================================
HFONT MakeFace(const wchar_t* const* faces, int count, int px, int weight, bool italic) {
    // Same resolve-then-verify route theme.cpp uses: GDI silently substitutes a family when the
    // name is unknown, and the only reliable test is GetTextFaceW.
    const int height = -IMax(1, px);
    HDC screen = GetDC(nullptr);
    HFONT chosen = nullptr;

    for (int i = 0; i < count; ++i) {
        HFONT f = CreateFontW(height, 0, 0, 0, weight, italic ? TRUE : FALSE, FALSE, FALSE,
                              DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                              CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, faces[i]);
        if (f == nullptr) continue;
        if (screen == nullptr) { chosen = f; break; }
        HGDIOBJ old = SelectObject(screen, f);
        wchar_t actual[LF_FACESIZE];
        actual[0] = L'\0';
        GetTextFaceW(screen, LF_FACESIZE, actual);
        SelectObject(screen, old);
        if (lstrcmpiW(actual, faces[i]) == 0) { chosen = f; break; }
        DeleteObject(f);
    }

    if (chosen == nullptr) {
        LOGFONTW lf;
        ZeroMemory(&lf, sizeof(lf));
        HFONT stock = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
        if (stock != nullptr && GetObjectW(stock, static_cast<int>(sizeof(lf)), &lf) != 0) {
            lf.lfHeight  = height;
            lf.lfWidth   = 0;
            lf.lfWeight  = weight;
            lf.lfItalic  = italic ? TRUE : FALSE;
            lf.lfQuality = CLEARTYPE_QUALITY;
            chosen = CreateFontIndirectW(&lf);
        }
        if (chosen == nullptr) {
            chosen = CreateFontW(height, 0, 0, 0, weight, italic ? TRUE : FALSE, FALSE, FALSE,
                                 DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                 CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, nullptr);
        }
    }
    if (screen != nullptr) ReleaseDC(nullptr, screen);
    return chosen;
}

// --gc-sans / --gc-serif / --gc-mono, as far as Windows has them.
const wchar_t* const kSansFaces[2]  = { L"Segoe UI Variable Text", L"Segoe UI" };
const wchar_t* const kSerifFaces[3] = { L"Palatino Linotype", L"Book Antiqua", L"Georgia" };
const wchar_t* const kMonoFaces[2]  = { L"Cascadia Mono", L"Consolas" };

HFONT MakeSans(int px, int weight)              { return MakeFace(kSansFaces, 2, px, weight, false); }
HFONT MakeSerif(int px, int weight, bool italic) { return MakeFace(kSerifFaces, 3, px, weight, italic); }
HFONT MakeMono(int px, int weight)              { return MakeFace(kMonoFaces, 2, px, weight, false); }

TEXTMETRICW Metrics(HFONT f) {
    TEXTMETRICW tm;
    ZeroMemory(&tm, sizeof(tm));
    if (f == nullptr) return tm;
    HDC screen = GetDC(nullptr);
    if (screen == nullptr) return tm;
    HGDIOBJ old = SelectObject(screen, f);
    GetTextMetricsW(screen, &tm);
    SelectObject(screen, old);
    ReleaseDC(nullptr, screen);
    return tm;
}

struct FontSet {
    HFONT label    = nullptr;   // Ko-fi title AND handle, weight 600
    HFONT labelReg = nullptr;   // the GitHub label, weight 400
    HFONT copy     = nullptr;   // .goat__line, weight 500
    HFONT now      = nullptr;   // .goat__line em - the site h1's gold italic serif NOW
    HFONT name     = nullptr;   // "GOAT Cam" in --gc-serif, weight 400, not uppercase
    HFONT tag      = nullptr;   // the tagline, tracked micro-caps, weight 600
    HFONT oldName  = nullptr;   // 71ea5ed Goatproject lockup, sans 300
    HFONT oldTag   = nullptr;   // 71ea5ed People’s Compute Commons, sans 500
    HFONT osd      = nullptr;   // the viewfinder readout, mono 500

    int dpi = 0;
    int labelPx = 0, labelH = 0;
    int namePx = 0, tagPx = 0, oldNamePx = 0, oldTagPx = 0, osdPx = 0;
    TEXTMETRICW copyTm{}, nameTm{}, tagTm{}, oldNameTm{}, oldTagTm{}, osdTm{};

    FontSet() {}
    ~FontSet() { Destroy(); }

    void Destroy() {
        HFONT* all[9] = { &label, &labelReg, &copy, &now, &name, &tag, &oldName, &oldTag, &osd };
        for (HFONT* f : all) {
            if (*f != nullptr) DeleteObject(*f);
            *f = nullptr;
        }
        dpi = 0;
    }

    void Build(int d) {
        if (dpi == d && label != nullptr) return;
        Destroy();
        labelPx  = IMax(1, theme::Dp(Sc(kKofiFontPx), d));
        label    = MakeSans(labelPx, FW_SEMIBOLD);
        labelReg = MakeSans(labelPx, FW_NORMAL);
        labelH   = static_cast<int>(Metrics(label).tmHeight);

        copy = MakeSans(Px(kCopyPx, d), FW_MEDIUM);
        now  = MakeSerif(Px(kCopyPx * kNowEm, d), FW_BOLD, true);
        namePx = Px(kNamePx, d);
        name   = MakeSerif(namePx, FW_NORMAL, false);
        tagPx  = Px(kTagPx, d);
        tag    = MakeSans(tagPx, FW_SEMIBOLD);
        oldName = MakeSans(IMax(theme::Dp(20, d), Px(7.6, d)), FW_LIGHT);
        oldTag  = MakeSans(IMax(theme::Dp(10, d), Px(3.3, d)), FW_MEDIUM);
        osdPx  = Px(kOsdPx, d);
        osd    = MakeMono(osdPx, FW_MEDIUM);

        copyTm = Metrics(copy);
        nameTm = Metrics(name);
        tagTm  = Metrics(tag);
        oldNamePx = IMax(theme::Dp(20, d), Px(7.6, d));
        oldTagPx = IMax(theme::Dp(10, d), Px(3.3, d));
        oldNameTm = Metrics(oldName);
        oldTagTm = Metrics(oldTag);
        osdTm  = Metrics(osd);
        dpi = d;
    }

private:
    FontSet(const FontSet&);
    FontSet& operator=(const FontSet&);
};

// ===========================================================================
// A minimal SVG path-data reader.
//
// DELIBERATELY NOT A GENERAL SVG PARSER. It reads path DATA only, for the command
// subset the source files actually contain, and it flattens every curve to a
// polyline. Arcs are here because the Ko-fi cup uses them; the lockup does not.
// ===========================================================================

struct PtF {
    double x = 0.0;
    double y = 0.0;
};

typedef std::vector<PtF>       SubPath;   // one closed or open contour, already flattened
typedef std::vector<SubPath>   Contours;  // one <path> element

// Curve flattening is done once, in source units, so the constants are budgets on the
// SOURCE viewBox (100 or 250 units tall). 20 segments across a 250-unit box is well
// under a device pixel at any DPI this app will ever see.
constexpr int kCubicSteps = 20;
constexpr int kQuadSteps  = 14;

class PathReader {
public:
    explicit PathReader(const char* d) : s_(d) {}

    void Run(Contours& out) {
        char cmd = 0;
        while (true) {
            SkipSep();
            if (*s_ == '\0') break;
            if (IsAlpha(*s_)) {
                cmd = *s_;
                ++s_;
            } else if (cmd == 'M') {
                cmd = 'L';   // repeated moveto coordinate pairs are implicit linetos
            } else if (cmd == 'm') {
                cmd = 'l';
            } else if (cmd == 0) {
                break;       // leading garbage: refuse rather than guess
            }
            if (!Command(cmd, out)) break;
        }
        Flush(out);
    }

private:
    static bool IsAlpha(char c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'); }
    static bool IsSep(char c)   { return c == ' ' || c == ',' || c == '\t' || c == '\r' || c == '\n'; }

    void SkipSep() { while (*s_ != '\0' && IsSep(*s_)) ++s_; }

    // Reads one number. Returns false at end-of-data or on a non-numeric byte, which is
    // how the command loop knows an argument list has ended.
    bool Num(double& out) {
        SkipSep();
        const char* p = s_;
        if (*p == '+' || *p == '-') ++p;
        if (!((*p >= '0' && *p <= '9') || *p == '.')) return false;
        char* end = nullptr;
        out = strtod(s_, &end);
        if (end == nullptr || end == s_) return false;
        s_ = end;
        return true;
    }

    bool Flag(double& out) {
        SkipSep();
        if (*s_ == '0' || *s_ == '1') { out = (*s_ == '1') ? 1.0 : 0.0; ++s_; return true; }
        return Num(out);
    }

    void Emit(double x, double y) {
        cur_.x = x;
        cur_.y = y;
        PtF p;
        p.x = x;
        p.y = y;
        open_.push_back(p);
    }

    void Flush(Contours& out) {
        if (open_.size() >= 2) out.push_back(open_);
        open_.clear();
    }

    void Cubic(double x1, double y1, double x2, double y2, double x, double y) {
        const double x0 = cur_.x, y0 = cur_.y;
        for (int i = 1; i <= kCubicSteps; ++i) {
            const double t = static_cast<double>(i) / kCubicSteps;
            const double u = 1.0 - t;
            const double bx = u * u * u * x0 + 3 * u * u * t * x1 + 3 * u * t * t * x2 + t * t * t * x;
            const double by = u * u * u * y0 + 3 * u * u * t * y1 + 3 * u * t * t * y2 + t * t * t * y;
            Emit(bx, by);
        }
        c1_.x = x2; c1_.y = y2;
        hasCubicCtl_ = true;
        hasQuadCtl_  = false;
    }

    void Quad(double x1, double y1, double x, double y) {
        const double x0 = cur_.x, y0 = cur_.y;
        for (int i = 1; i <= kQuadSteps; ++i) {
            const double t = static_cast<double>(i) / kQuadSteps;
            const double u = 1.0 - t;
            Emit(u * u * x0 + 2 * u * t * x1 + t * t * x,
                 u * u * y0 + 2 * u * t * y1 + t * t * y);
        }
        q1_.x = x1; q1_.y = y1;
        hasQuadCtl_  = true;
        hasCubicCtl_ = false;
    }

    // Endpoint -> centre parameterisation, W3C SVG 1.1 implementation notes F.6.5.
    void Arc(double rx, double ry, double rotDeg, bool large, bool sweep, double x, double y) {
        const double x0 = cur_.x, y0 = cur_.y;
        if (rx == 0.0 || ry == 0.0) { Emit(x, y); return; }   // degenerate: a straight line
        if (x0 == x && y0 == y) return;
        rx = std::fabs(rx);
        ry = std::fabs(ry);

        const double phi = rotDeg * 3.14159265358979323846 / 180.0;
        const double cp = std::cos(phi), sp = std::sin(phi);
        const double dx2 = (x0 - x) * 0.5, dy2 = (y0 - y) * 0.5;
        const double x1 =  cp * dx2 + sp * dy2;
        const double y1 = -sp * dx2 + cp * dy2;

        double lam = (x1 * x1) / (rx * rx) + (y1 * y1) / (ry * ry);
        if (lam > 1.0) {
            const double k = std::sqrt(lam);
            rx *= k;
            ry *= k;
        }

        const double num = rx * rx * ry * ry - rx * rx * y1 * y1 - ry * ry * x1 * x1;
        const double den = rx * rx * y1 * y1 + ry * ry * x1 * x1;
        double coef = (den <= 0.0) ? 0.0 : std::sqrt(DMax(0.0, num / den));
        if (large == sweep) coef = -coef;

        const double cx1 =  coef * rx * y1 / ry;
        const double cy1 = -coef * ry * x1 / rx;
        const double cx  = cp * cx1 - sp * cy1 + (x0 + x) * 0.5;
        const double cy  = sp * cx1 + cp * cy1 + (y0 + y) * 0.5;

        const double ux = (x1 - cx1) / rx, uy = (y1 - cy1) / ry;
        const double vx = (-x1 - cx1) / rx, vy = (-y1 - cy1) / ry;

        double th0 = std::atan2(uy, ux);
        double dth = std::atan2(ux * vy - uy * vx, ux * vx + uy * vy);
        const double kTwoPi = 6.28318530717958647692;
        if (!sweep && dth > 0.0) dth -= kTwoPi;
        if (sweep && dth < 0.0)  dth += kTwoPi;

        int steps = static_cast<int>(std::ceil(std::fabs(dth) / 0.35));
        if (steps < 3) steps = 3;
        if (steps > 64) steps = 64;
        for (int i = 1; i <= steps; ++i) {
            const double th = th0 + dth * (static_cast<double>(i) / steps);
            const double ex = rx * std::cos(th), ey = ry * std::sin(th);
            Emit(cx + cp * ex - sp * ey, cy + sp * ex + cp * ey);
        }
        hasCubicCtl_ = false;
        hasQuadCtl_  = false;
    }

    bool Command(char cmd, Contours& out) {
        const bool rel = (cmd >= 'a' && cmd <= 'z');
        const char c = static_cast<char>(rel ? cmd - 32 : cmd);
        double a[7];

        switch (c) {
        case 'M': {
            if (!Num(a[0]) || !Num(a[1])) return false;
            Flush(out);
            const double nx = rel ? cur_.x + a[0] : a[0];
            const double ny = rel ? cur_.y + a[1] : a[1];
            start_ = PtF{nx, ny};
            cur_   = start_;
            open_.clear();
            open_.push_back(start_);
            hasCubicCtl_ = hasQuadCtl_ = false;
            return true;
        }
        case 'L': {
            if (!Num(a[0]) || !Num(a[1])) return false;
            Emit(rel ? cur_.x + a[0] : a[0], rel ? cur_.y + a[1] : a[1]);
            hasCubicCtl_ = hasQuadCtl_ = false;
            return true;
        }
        case 'H': {
            if (!Num(a[0])) return false;
            Emit(rel ? cur_.x + a[0] : a[0], cur_.y);
            hasCubicCtl_ = hasQuadCtl_ = false;
            return true;
        }
        case 'V': {
            if (!Num(a[0])) return false;
            Emit(cur_.x, rel ? cur_.y + a[0] : a[0]);
            hasCubicCtl_ = hasQuadCtl_ = false;
            return true;
        }
        case 'C': {
            for (int i = 0; i < 6; ++i) if (!Num(a[i])) return false;
            const double bx = rel ? cur_.x : 0.0, by = rel ? cur_.y : 0.0;
            Cubic(bx + a[0], by + a[1], bx + a[2], by + a[3], bx + a[4], by + a[5]);
            return true;
        }
        case 'S': {
            for (int i = 0; i < 4; ++i) if (!Num(a[i])) return false;
            const double bx = rel ? cur_.x : 0.0, by = rel ? cur_.y : 0.0;
            const double rx = hasCubicCtl_ ? 2 * cur_.x - c1_.x : cur_.x;
            const double ry = hasCubicCtl_ ? 2 * cur_.y - c1_.y : cur_.y;
            Cubic(rx, ry, bx + a[0], by + a[1], bx + a[2], by + a[3]);
            return true;
        }
        case 'Q': {
            for (int i = 0; i < 4; ++i) if (!Num(a[i])) return false;
            const double bx = rel ? cur_.x : 0.0, by = rel ? cur_.y : 0.0;
            Quad(bx + a[0], by + a[1], bx + a[2], by + a[3]);
            return true;
        }
        case 'T': {
            if (!Num(a[0]) || !Num(a[1])) return false;
            const double bx = rel ? cur_.x : 0.0, by = rel ? cur_.y : 0.0;
            const double rx = hasQuadCtl_ ? 2 * cur_.x - q1_.x : cur_.x;
            const double ry = hasQuadCtl_ ? 2 * cur_.y - q1_.y : cur_.y;
            Quad(rx, ry, bx + a[0], by + a[1]);
            return true;
        }
        case 'A': {
            if (!Num(a[0]) || !Num(a[1]) || !Num(a[2])) return false;
            if (!Flag(a[3]) || !Flag(a[4])) return false;
            if (!Num(a[5]) || !Num(a[6])) return false;
            const double bx = rel ? cur_.x : 0.0, by = rel ? cur_.y : 0.0;
            Arc(a[0], a[1], a[2], a[3] != 0.0, a[4] != 0.0, bx + a[5], by + a[6]);
            return true;
        }
        case 'Z': {
            if (!open_.empty()) {
                Emit(start_.x, start_.y);
                Flush(out);
            }
            cur_ = start_;
            open_.clear();
            open_.push_back(start_);
            hasCubicCtl_ = hasQuadCtl_ = false;
            return true;
        }
        default:
            return false;   // an unsupported command: stop rather than draw nonsense
        }
    }

    const char* s_;
    PtF  cur_{}, start_{}, c1_{}, q1_{};
    bool hasCubicCtl_ = false;
    bool hasQuadCtl_  = false;
    SubPath open_;
};

Contours ParsePath(const char* d) {
    Contours out;
    PathReader r(d);
    r.Run(out);
    return out;
}

Contours CircleContour(double cx, double cy, double radius) {
    Contours out;
    SubPath ring;
    for (int i = 0; i <= 32; ++i) {
        const double a = 6.28318530717958647692 * i / 32.0;
        ring.push_back(PtF{cx + radius * std::cos(a), cy + radius * std::sin(a)});
    }
    out.push_back(std::move(ring));
    return out;
}

// ===========================================================================
// THE ARTWORK - the Ko-fi button's cup, the one vector shape left in the strip.
//
// Copied verbatim from the Ko-fi button in options.html. The numbers are the operator's.
// ===========================================================================
namespace kArt {

// DaGoat lockup geometry from the original built-in strip (commit 71ea5ed).
const char* const kHead[5] = {
    "M62 210 C74 148 120 98 190 80 C152 106 106 150 82 216 Z",
    "M186 84 C214 62 226 32 210 8 C216 36 200 62 174 80 Z",
    "M170 82 C190 60 194 34 180 16 C186 40 172 62 156 78 Z",
    "M188 82 C206 90 218 104 222 122 C212 106 200 94 182 90 Z",
    "M206 122 C215 134 217 149 209 160 C212 147 208 133 200 127 Z",
};
constexpr double kEyeCx = 197.0, kEyeCy = 99.0, kEyeR = 3.6;
constexpr double kSealX = 214.0, kSealY = 182.0, kSealW = 36.0, kSealH = 36.0, kSealRx = 4.0;
constexpr double kInnerBox = 100.0;
const char* const kThumb =
    "M30 50 C26 50 24 52 24 56 L24 87 C24 91 26 93 30 93 L73 93 C79 93 84 90 85 84 "
    "L89 55 C90 49 86 44 80 44 L63 44 C65 38 67 25 65 18 C63 12 55 12 53 18 "
    "C51 24 53 33 51 42 C50 47 47 50 42 50 Z";
constexpr double kThumbDx = -5.0, kThumbDy = -3.0;
const char* const kChar =
    "M33.5 18.12Q39.14 21.51 41.54 23.62Q43.93 25.74 43.65 27.85Q43.37 29.97 42.24 30.96"
    "Q41.11 31.94 40.55 31.94Q39.42 31.94 38.57 29.12Q37.45 25.17 32.93 18.97Z"
    "M20.52 33.07H52.68Q58.89 23.76 60.86 16.99L66.79 20.66Q63.96 22.07 61.14 25.74"
    "Q58.04 29.41 54.37 33.07H68.2L72.43 28.84L77.79 34.77H51.55V47.46H63.96"
    "L68.2 43.79L72.99 49.15H51.55V62.13H71.58L76.1 57.9L81.46 63.82H51.55V73.7"
    "Q51.55 76.8 51.83 81.32L47.04 83.01Q47.32 78.78 47.32 63.82H27.85"
    "Q24.19 63.82 21.08 64.67L18.54 62.13H47.32V49.15H35.47Q31.8 49.15 28.7 50L26.16 47.46"
    "H47.32V34.77H29.83Q26.16 34.77 23.06 35.61Z";

// Ko-fi cup, options.html, viewBox "0 0 24 24".
const char* const kKofiCup =
    "M4 4h13a4 4 0 0 1 0 8h-1.1A6 6 0 0 1 10 17H8a6 6 0 0 1-6-6V4a0 0 0 0 1 0 0z"
    "m12 2v4h1a2 2 0 0 0 0-4h-1z";
const char* const kKofiSaucer = "M3 19h14a1 1 0 0 1 0 2H3a1 1 0 0 1 0-2z";

const Contours& Head(int i) {
    static const Contours c[5] = {ParsePath(kHead[0]), ParsePath(kHead[1]), ParsePath(kHead[2]),
                                  ParsePath(kHead[3]), ParsePath(kHead[4])};
    return c[i];
}
const Contours& Eye() { static const Contours c = CircleContour(kEyeCx, kEyeCy, kEyeR); return c; }
const Contours& Thumb() { static const Contours c = ParsePath(kThumb); return c; }
const Contours& Char() { static const Contours c = ParsePath(kChar); return c; }

// Parsed exactly once, in source units, and shared by every DPI.
const Contours& KofiCup()    { static const Contours c = ParsePath(kKofiCup); return c; }
const Contours& KofiSaucer() { static const Contours c = ParsePath(kKofiSaucer); return c; }

}  // namespace kArt

// ===========================================================================
// Layer - a 32bpp top-down DIB section with a direct bits pointer. The strip's one back
// buffer: GDI draws into it through Dc(), GDI+ through a Bitmap wrapped over Bits().
// ===========================================================================
class Layer {
public:
    ~Layer() { Destroy(); }

    bool Ensure(int w, int h) {
        if (w <= 0 || h <= 0) return false;
        if (dc_ != nullptr && w_ == w && h_ == h) return true;
        Destroy();

        BITMAPINFO bi;
        ZeroMemory(&bi, sizeof(bi));
        bi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth       = w;
        bi.bmiHeader.biHeight      = -h;   // top-down, so row 0 is the top
        bi.bmiHeader.biPlanes      = 1;
        bi.bmiHeader.biBitCount    = 32;
        bi.bmiHeader.biCompression = BI_RGB;

        void* bits = nullptr;
        HBITMAP bmp = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (bmp == nullptr || bits == nullptr) {
            if (bmp != nullptr) DeleteObject(bmp);
            return false;
        }
        HDC dc = CreateCompatibleDC(nullptr);
        if (dc == nullptr) { DeleteObject(bmp); return false; }

        old_  = static_cast<HBITMAP>(SelectObject(dc, bmp));
        dc_   = dc;
        bmp_  = bmp;
        bits_ = static_cast<BYTE*>(bits);
        w_    = w;
        h_    = h;
        return true;
    }

    void Destroy() {
        if (dc_ != nullptr) {
            if (old_ != nullptr) SelectObject(dc_, old_);
            DeleteDC(dc_);
        }
        if (bmp_ != nullptr) DeleteObject(bmp_);
        dc_ = nullptr; bmp_ = nullptr; old_ = nullptr; bits_ = nullptr; w_ = h_ = 0;
    }

    HDC   Dc()   const { return dc_; }
    int   W()    const { return w_; }
    int   H()    const { return h_; }
    BYTE* Bits() const { return bits_; }
    void Clear() {
        if (bits_ != nullptr && w_ > 0 && h_ > 0)
            std::memset(bits_, 0, static_cast<size_t>(w_) * h_ * 4);
    }

private:
    HDC     dc_   = nullptr;
    HBITMAP bmp_  = nullptr;
    HBITMAP old_  = nullptr;
    BYTE*   bits_ = nullptr;
    int     w_ = 0, h_ = 0;
};

// A scoped GDI object selection - nothing leaks even on an early return.
class SelectScope {
public:
    SelectScope(HDC dc, HGDIOBJ obj) : dc_(dc), old_(SelectObject(dc, obj)) {}
    ~SelectScope() { SelectObject(dc_, old_); }
private:
    HDC     dc_;
    HGDIOBJ old_;
};

// ===========================================================================
// Motion - the GOAT Cam plate only. Ko-fi and Star swap colour on hover and do not move.
//
// Every value is a CSS-style TRANSITION of 0..1, evaluated from the clock at paint time, so a
// frame shows exactly the state for the moment it is drawn and nothing is stepped per tick.
// Retargeting starts from wherever the value is NOW, and a part-finished transition reverses in
// the matching part of its duration (CSS "reversing shortening"), so a quick in-and-out never
// jumps. (MacFanControl's goatcambanner.cpp, verbatim in behaviour.)
// ===========================================================================
double Linear(double t) { return t; }
typedef double (*EaseFn)(double);

struct Tween {
    double from = 0.0, to = 0.0, t0 = 0.0, delay = 0.0, dur = 0.0;
    EaseFn ease = Linear;

    double At(double now) const {
        if (dur <= 0.0) return to;
        const double t = (now - t0 - delay) / dur;
        if (t <= 0.0) return from;
        if (t >= 1.0) return to;
        return from + (to - from) * ease(t);
    }
    bool Busy(double now) const { return dur > 0.0 && now < t0 + delay + dur; }
    void Go(double now, double target, double fullMs, EaseFn e, double delayMs = 0.0) {
        const double cur = At(now);
        from = cur; to = target; t0 = now; delay = delayMs; ease = e;
        dur = fullMs * std::fabs(target - cur);
        if (dur < 1.0) { from = to; dur = 0.0; delay = 0.0; }
    }
    void Snap(double v) { from = to = v; dur = 0.0; delay = 0.0; }
};

double NowMs() {
    static const double perMs = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return static_cast<double>(f.QuadPart) / 1000.0;
    }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return static_cast<double>(c.QuadPart) / perMs;
}

struct GoatMotion {
    Tween push, bracket;           // TRAVELLING parts: off under reduced motion
    Tween lift, tag, qr, lock;     // fades: always
    bool active = false;

    // `travel` is SPI_GETCLIENTAREAANIMATION. Off = the fades only, which is what the
    // stylesheet's prefers-reduced-motion block keeps.
    void SetActive(bool on, double now, bool travel) {
        using namespace sponsor_motion;
        if (on == active) return;
        active = on;
        const double out = static_cast<double>(kOutMs);
        if (on) {
            if (travel) {
                push.Go(now, 1, kPushInMs, EaseIn);         // the photo: scale(1.06) 1200ms --gc-in
                bracket.Go(now, 1, kBracketInMs, EaseIn);   // .gc-vf inset +3px, 420ms --gc-in
            }
            lift.Go(now, 1, kLiftInMs, EaseIn);              // the lift ring, 420ms --gc-in
            tag.Go(now, 1, kAccentInMs, EaseOut);            // tagline colour 630ms --gl-out
            qr.Go(now, 1, kAccentInMs, EaseIn);              // QR ring 630ms --gc-in
            lock.Go(now, 1, kLockInMs, Linear, kLockDelayMs);// .gc-vf::after 300ms linear 140ms
        } else {
            // Leaving is always faster than arriving: every part goes out in 360ms --gl-out.
            push.Go(now, 0, out, EaseOut);
            bracket.Go(now, 0, out, EaseOut);
            lift.Go(now, 0, out, EaseOut);
            tag.Go(now, 0, out, EaseOut);
            qr.Go(now, 0, out, EaseOut);
            lock.Go(now, 0, out, EaseOut);
        }
    }

    // THE single gate on the animation timer.
    bool Busy(double now) const {
        return push.Busy(now) || bracket.Busy(now) || lift.Busy(now) || tag.Busy(now) ||
               qr.Busy(now) || lock.Busy(now);
    }
};

// ===========================================================================
// The Ko-fi cup - plain GDI, as it always was. Device-space point arrays, rebuilt only when
// the DPI or the layout changes; the paint path only calls PolyPolygon.
// ===========================================================================
struct DevPath {
    std::vector<POINT> pts;
    std::vector<INT>   counts;
};

DevPath BuildDevPath(const Contours& src, double sx, double sy, double tx, double ty) {
    DevPath d;
    for (size_t i = 0; i < src.size(); ++i) {
        const SubPath& sp = src[i];
        if (sp.size() < 2) continue;
        int n = 0;
        for (size_t j = 0; j < sp.size(); ++j) {
            POINT p;
            p.x = RoundI(tx + sp[j].x * sx);
            p.y = RoundI(ty + sp[j].y * sy);
            // Collapse consecutive duplicates - at footer size many flattened steps land
            // on the same device pixel and GDI is faster with fewer points.
            if (n > 0 && d.pts.back().x == p.x && d.pts.back().y == p.y) continue;
            d.pts.push_back(p);
            ++n;
        }
        if (n >= 3) d.counts.push_back(n);
        else        d.pts.resize(d.pts.size() - static_cast<size_t>(n));
    }
    return d;
}

void FillDevPath(HDC dc, const DevPath& d, COLORREF colour) {
    if (d.counts.empty()) return;
    HBRUSH br = CreateSolidBrush(colour);
    if (br == nullptr) return;
    const int oldMode = SetPolyFillMode(dc, WINDING);   // SVG's default fill-rule is nonzero
    {
        SelectScope brs(dc, br);
        SelectScope pns(dc, GetStockObject(NULL_PEN));
        PolyPolygon(dc, d.pts.data(), d.counts.data(), static_cast<int>(d.counts.size()));
    }
    SetPolyFillMode(dc, oldMode);
    DeleteObject(br);
}

struct Geo {
    bool valid = false;
    DevPath kofiCup, kofiSaucer;
    DevPath head[5], eye, chr, thumb;
    Layer bloom;
    POINT bloomOrg{};
    RECT sealRc{};
    int sealRadius = 0;
    double s = 0.0;
    double inner = 0.0;
    POINT glyphOrg{};
    int glyphW = 0, glyphH = 0;
    std::vector<BYTE> mask;
    int maskW = 0, maskH = 0;
};

// ===========================================================================
// GDI+ helpers.
// ===========================================================================
void AddRoundRect(gp::GraphicsPath& p, const gp::RectF& r, float rad) {
    rad = std::min(rad, std::min(r.Width, r.Height) / 2.0f);
    if (rad <= 0.0f) { p.AddRectangle(r); return; }
    const float d = rad * 2.0f;
    p.StartFigure();
    p.AddArc(r.X, r.Y, d, d, 180.0f, 90.0f);
    p.AddArc(r.X + r.Width - d, r.Y, d, d, 270.0f, 90.0f);
    p.AddArc(r.X + r.Width - d, r.Y + r.Height - d, d, d, 0.0f, 90.0f);
    p.AddArc(r.X, r.Y + r.Height - d, d, d, 90.0f, 90.0f);
    p.CloseFigure();
}

gp::RectF ToRectF(const RECT& r) {
    return gp::RectF(static_cast<float>(r.left), static_cast<float>(r.top),
                     static_cast<float>(r.right - r.left), static_cast<float>(r.bottom - r.top));
}

// ===========================================================================
// Layout
// ===========================================================================
struct Layout {
    int stripH = 0;      // the row: the plate's height
    int btnH   = 0;      // Ko-fi and Star, centred in the row

    // btn[2] IS THE PLATE - the only live part of the GOAT Cam item, and the only rect the hit
    // test, the tooltip and the hand cursor know about. The headline beside it is not in it.
    RECT btn[3]{};

    int kofiRadius = 0;
    int btnRadius  = 0;   // GitHub, and the Ko-fi / GitHub focus rings

    // Ko-fi internals. The two label lines are CENTRED with respect to each other, as the
    // reference render shows, so their draw origins are computed here rather than derived
    // from a shared left edge at paint time.
    RECT  kofiIcon{};
    RECT  kofiLabel{};    // the label block both lines are centred inside
    POINT kofiTitlePt{};
    POINT kofiHandlePt{};

    // GitHub internals
    RECT  ghStar{};
    POINT ghLabelPt{};

    // GOAT Cam
    int   goatLeft = 0, goatRight = 0;   // the whole item, headline + plate
    int   copyX = 0;
    int   copyBase[2] = {};              // the headline's two baselines
    int   nowW = 0;                      // "NOW", which opens line 2
    int   plateRadius = 0;
    RECT  goatMark{};                    // restored 71ea5ed mark box
    POINT fallbackNamePt{};
    POINT fallbackTagPt{};
    int   fallbackNameTrack = 0;
    int   fallbackTagTrack = 0;
    RECT  fallbackCopy{};
    // The photo at rest: the source image's top-left lands at (photoX, photoY) and every source
    // pixel is photoS device px. The push-in scales about (pushX, pushY), the cup's centre.
    float photoX = 0.0f, photoY = 0.0f, photoS = 0.0f;
    float pushX = 0.0f, pushY = 0.0f;
    RECT  cup{};                         // the cup + saucer at rest - nothing is drawn over it
    RECT  vf{};                          // the viewfinder the brackets frame
    RECT  text{};                        // the wordmark + tagline block
    RECT  copy{};                        // the two DaGoat copy lines' line boxes
    RECT  iphone{};                      // the static iPhone preview between text and QR
    RECT  scrim{};                       // the local gradient behind it
    RECT  wordmark{};
    RECT  tagline[2]{};
    int   tagBaseline[2]{};
    int   tagEmBaseline[2]{};
    bool  taglineHasEm[2]{};
    RECT  badge{};
    RECT  stage{};
    POINT namePt{};
    int   tagLines = 1;                  // 1, or 2 when one line would reach the QR
    POINT tagPt[2]{};
    int   tagTrack = 0;
    RECT  tile{};
    int   module = 0;
    POINT qrOrg{};
    int   vfIn = 0, vfLock = 0, vfArm = 0, vfBar = 0;
    bool  osdOn = false;
    RECT  osdPill{};
    RECT  osdDot{};
    POINT osdText{};
    RECT  fallbackWordmark{};
    RECT  fallbackTag{};
};

const wchar_t* const kKofiTitle  = L"Support me on Ko-fi";
const wchar_t* const kKofiHandle = L"@IRP_HongKong";
const wchar_t* const kGhLabel    = L"Star on GitHub";
const wchar_t* const kNoLink     = L"link not configured";

// Width of `s` in `f`, with `track` of letter-spacing. GetTextExtentPoint32W does not see
// SetTextCharacterExtra, so the tracking is added here - and CSS letter-spacing, like GDI's
// character extra, also adds after the LAST character, so the counts match.
int TextWidth(HDC dc, HFONT f, const std::wstring& s, int track) {
    if (f == nullptr) return 0;
    SelectScope fs(dc, f);
    SIZE sz;
    sz.cx = 0;
    sz.cy = 0;
    GetTextExtentPoint32W(dc, s.c_str(), static_cast<int>(s.size()), &sz);
    return static_cast<int>(sz.cx) + track * static_cast<int>(s.size());
}

void DrawWithFont(HDC dc, int x, int y, const std::wstring& s, HFONT f,
                  COLORREF colour, int track) {
    if (f == nullptr) return;
    SelectScope fs(dc, f);
    const int oldMode = SetBkMode(dc, TRANSPARENT);
    const COLORREF oldCol = SetTextColor(dc, colour);
    const int oldExtra = SetTextCharacterExtra(dc, track);
    TextOutW(dc, x, y, s.c_str(), static_cast<int>(s.size()));
    SetTextCharacterExtra(dc, oldExtra);
    SetTextColor(dc, oldCol);
    SetBkMode(dc, oldMode);
}

// The same, with `y` as the BASELINE - two faces on one line (the serif NOW inside the sans
// copy) only line up when both are drawn on the same baseline.
void DrawOnBaseline(HDC dc, int x, int y, const std::wstring& s, HFONT f,
                    COLORREF colour, int track = 0) {
    const UINT oldAlign = SetTextAlign(dc, TA_BASELINE | TA_LEFT | TA_NOUPDATECP);
    DrawWithFont(dc, x, y, s, f, colour, track);
    SetTextAlign(dc, oldAlign);
}

// CSS puts a line box of `boxH` around the text; GDI's TextOut origin is the top of a cell
// of tmHeight. Centring one inside the other is what makes a px font-size land where the
// browser puts it (CSS half-leading).
inline int CellTop(int boxTop, int boxH, int tmHeight) {
    return boxTop + (boxH - tmHeight) / 2;
}

void ComputeLayout(HDC dc, int dpi, const RECT& client, const FontSet& F, Layout& L,
                   const ob::Model* current) {
    using theme::Dp;

    L.kofiRadius = Dp(Sc(kKofiRadius), dpi);
    L.btnRadius  = Dp(Sc(theme::metric::kButtonRadius), dpi);

    // .kofi { padding: 9px 16px; line-height: 1.4 } - two lines, both at labelPx.
    const int kofiPadY  = Dp(Sc(kKofiPadY), dpi);
    const int kofiLineH = IMax(F.labelH, RoundI(kKofiLineHeight * F.labelPx));
    const int kofiNatural = kofiPadY * 2 + kofiLineH * 2;

    L.btnH   = IMax(kofiNatural, Dp(kBtnMarkH, dpi) + Dp(Sc(6), dpi));
    const int plateH = Dp(kPlateH, dpi);
    L.stripH = IMax(plateH, L.btnH);

    const int top    = client.top + IMax(0, ((client.bottom - client.top) - L.stripH) / 2);
    const int btnTop = top + (L.stripH - L.btnH) / 2;
    const int gap    = Dp(Sc(theme::metric::kGap), dpi);
    int x = client.left;

    // ---- Ko-fi ------------------------------------------------------------
    {
        const int padX   = Dp(Sc(kKofiPadX), dpi);
        const int icoGap = Dp(Sc(kKofiIcoGap), dpi);
        const int ico    = Dp(Sc(kKofiIco), dpi);
        // Both lines are the SAME face and the SAME size in the reference - the handle is
        // not a smaller or lighter afterthought.
        const int titleW  = TextWidth(dc, F.label, kKofiTitle, 0);
        const int handleW = TextWidth(dc, F.label, kKofiHandle, 0);
        const int labelW  = IMax(titleW, handleW);
        const int w = padX * 2 + ico + icoGap + labelW;

        L.btn[0].left = x; L.btn[0].top = btnTop;
        L.btn[0].right = x + w; L.btn[0].bottom = btnTop + L.btnH;

        const int cy = btnTop + L.btnH / 2;
        L.kofiIcon.left = x + padX; L.kofiIcon.right = L.kofiIcon.left + ico;
        L.kofiIcon.top = cy - ico / 2; L.kofiIcon.bottom = L.kofiIcon.top + ico;

        const int lx = L.kofiIcon.right + icoGap;
        const int blockTop = cy - kofiLineH;          // two lines of kofiLineH, centred
        L.kofiLabel.left = lx; L.kofiLabel.right = lx + labelW;
        L.kofiLabel.top = blockTop; L.kofiLabel.bottom = blockTop + kofiLineH * 2;

        L.kofiTitlePt.x  = lx + (labelW - titleW) / 2;
        L.kofiTitlePt.y  = CellTop(blockTop, kofiLineH, F.labelH);
        L.kofiHandlePt.x = lx + (labelW - handleW) / 2;
        L.kofiHandlePt.y = CellTop(blockTop + kofiLineH, kofiLineH, F.labelH);

        x = L.btn[0].right + gap;
    }

    // ---- GitHub star ------------------------------------------------------
    {
        const int padX   = Dp(Sc(kGhPadX), dpi);
        const int icoGap = Dp(Sc(kGhIcoGap), dpi);
        const int ico    = Dp(Sc(kGhIco), dpi);
        const int labelW = TextWidth(dc, F.labelReg, kGhLabel, 0);
        const int w = padX * 2 + ico + icoGap + labelW;

        L.btn[1].left = x; L.btn[1].top = btnTop;
        L.btn[1].right = x + w; L.btn[1].bottom = btnTop + L.btnH;

        const int cy = btnTop + L.btnH / 2;
        L.ghStar.left = x + padX; L.ghStar.right = L.ghStar.left + ico;
        L.ghStar.top = cy - ico / 2; L.ghStar.bottom = L.ghStar.top + ico;
        L.ghLabelPt.x = L.ghStar.right + icoGap;
        L.ghLabelPt.y = cy - F.labelH / 2;

        x = L.btn[1].right + Dp(kGroupGap, dpi);
    }

    // ---- GOAT Cam: the headline ------------------------------------------
    // Two lines, broken where the web strip breaks them. Mixed faces on a line rule out
    // DT_WORDBREAK, so the break is explicit and the column is exactly as wide as its widest
    // line - measured, never assumed, so nothing can clip.
    const int plateT = top + (L.stripH - plateH) / 2;
    const int mid    = plateT + plateH / 2;
    L.goatLeft = x;
    {
        const int lineH  = Px(kCopyPx * kCopyLineH, dpi);
        // GDI's text cell carries more leading below the glyphs than the CSS line box does. Lift
        // the two rendered lines by the measured leading so their ink, rather than their cell,
        // is centred on the strip with the buttons and plate.
        const int blockT = mid - lineH - Dp(6, dpi);
        const int base = (lineH - static_cast<int>(F.copyTm.tmHeight)) / 2 + static_cast<int>(F.copyTm.tmAscent);
        L.copyBase[0] = blockT + base;
        L.copyBase[1] = blockT + lineH + base;

        L.nowW = TextWidth(dc, F.now, kCopyNow, 0);
        const int onlineCopyW = IMax(TextWidth(dc, F.copy, kCopy1, 0),
                                     L.nowW + TextWidth(dc, F.copy, kCopy2, 0));
        const int fallbackCopyW = IMax(TextWidth(dc, F.copy, kDaGoatCopy1, 0),
                                       TextWidth(dc, F.copy, kDaGoatCopy2, 0));
        int copyW = onlineCopyW;
        if (current != nullptr && current->online && !current->banner.headline.empty()) {
            auto headlineWidth = [&](const std::wstring& line) {
                if (!current->banner.hasHeadlineEm || current->banner.headlineEm.empty())
                    return TextWidth(dc, F.copy, line, 0);
                const size_t at = line.find(current->banner.headlineEm);
                if (at == std::wstring::npos) return TextWidth(dc, F.copy, line, 0);
                return TextWidth(dc, F.copy, line.substr(0, at), 0) +
                       TextWidth(dc, F.now, current->banner.headlineEm, 0) +
                       TextWidth(dc, F.copy,
                                 line.substr(at + current->banner.headlineEm.size()), 0);
            };
            std::wstring line1 = current->banner.headline, line2;
            const size_t dash = line1.find(L'\u2014');
            if (dash != std::wstring::npos) {
                line2 = line1.substr(dash + 1);
                line1.resize(dash + 1);
                while (!line2.empty() && line2.front() == L' ') line2.erase(line2.begin());
            }
            copyW = IMax(headlineWidth(line1), headlineWidth(line2));
        } else if (current == nullptr || !current->online) {
            copyW = fallbackCopyW;
        }
        L.copyX = x;
        L.fallbackCopy = { L.copyX, blockT, L.copyX + fallbackCopyW, blockT + lineH * 2 };
        L.copy = L.fallbackCopy;
        x += copyW + Dp(kCopyGap, dpi);
    }

    // ---- GOAT Cam: the plate ----------------------------------------------
    // The border is one DEVICE pixel at every dpi (a hairline, as in MacFanControl): a 2px
    // border at 150% would leave the 111px QR no room.
    {
        const int plateW = Dp(kPlateW, dpi);
        const bool onlineLayout = current != nullptr && current->online;
        // SponsorMeasure is intentionally based on the wider built-in copy. A verified online
        // headline can be shorter, so keep the online plate attached to the control's right edge
        // instead of letting that model-specific width create a dark tail in the host row.
        const int plateL = onlineLayout ? std::max(x, static_cast<int>(client.right) - plateW) : x;
        L.plateRadius = Dp(kPlateRadius, dpi);
        L.btn[2].left = plateL;
        L.btn[2].top = plateT;
        L.btn[2].right = plateL + plateW;
        L.btn[2].bottom = plateT + plateH;
        L.goatRight = L.btn[2].right;

        // The QR tile: an INTEGER module size, never resampled. The tile grows with the dpi and
        // any slack between it and the code is extra quiet zone, never a fractional module.
        const int inner = plateH - 2;
        const int side  = std::min(Dp(kQrSide, dpi), inner);
        L.module = IMax(2, side / kQrN);
        const int qr = L.module * kQrN;
        const int tileSide = IMax(side, qr);
        const int tileR = L.btn[2].right - 1 - Dp(kQrPadR, dpi);
        const int tileT = plateT + (plateH - tileSide) / 2;
        L.tile.left = tileR - tileSide;     L.tile.top = tileT;
        L.tile.right = tileR;               L.tile.bottom = tileT + tileSide;
        L.qrOrg.x = L.tile.left + (tileSide - qr) / 2;
        L.qrOrg.y = tileT + (tileSide - qr) / 2;
        const int stopX = L.tile.left - Dp(kQrGap, dpi);   // nothing but the photo passes this

        // The photo: object-fit cover across the WHOLE plate, its left edge on the plate's left
        // (the cup is on the left), and the band placed so the cup and saucer are wholly inside
        // it with the most of the dark screen above them.
        const double W = plateW, H = plateH;
        const double s = std::max(W / kSrcW, H / kSrcH);
        const double bandRows = H / s;
        double r0 = (kSrcCupB + kSrcCupMargin) - bandRows;   // lowest band that still holds the cup
        if (r0 > kSrcCupT - kSrcCupMargin) r0 = (kSrcCupT + kSrcCupB) / 2.0 - bandRows / 2.0;
        r0 = std::min(std::max(r0, 0.0), kSrcH - bandRows);
        L.photoS = static_cast<float>(s);
        L.photoX = static_cast<float>(plateL);
        L.photoY = static_cast<float>(plateT - r0 * s);
        L.pushX = static_cast<float>(plateL + (kSrcCupL + kSrcCupR) / 2.0 * s);
        L.pushY = static_cast<float>(L.photoY + (kSrcCupT + kSrcCupB) / 2.0 * s);
        L.cup.left   = static_cast<LONG>(std::floor(plateL + kSrcCupL * s));
        L.cup.right  = static_cast<LONG>(std::ceil(plateL + kSrcCupR * s));
        L.cup.top    = static_cast<LONG>(std::floor(L.photoY + kSrcCupT * s));
        L.cup.bottom = static_cast<LONG>(std::ceil(L.photoY + kSrcCupB * s));

        // The wordmark: over the laptop's dark screen, centred in what of it the band shows.
        L.tagTrack = IMax(1, RoundI(kTagTrackEm * F.tagPx));
        const bool online = current != nullptr && current->online;
        const std::wstring wordmark = online && !current->banner.wordmark.empty()
                                          ? current->banner.wordmark : kGoatName;
        std::wstring tags[2] = { kGoatTag1, kGoatTag2 };
        if (online) {
            for (size_t i = 0; i < current->banner.tagline.size() && i < 2; ++i)
                tags[i] = current->banner.tagline[i];
            if (current->banner.tagline.empty()) tags[0] = kGoatTag;
        }
        const int nameW = TextWidth(dc, F.name, wordmark, 0);
        const int tagW0 = TextWidth(dc, F.tag, tags[0], L.tagTrack);
        const int tagW1 = TextWidth(dc, F.tag, tags[1], L.tagTrack);
        const int tagW  = online && current->banner.tagline.size() > 1 ? IMax(tagW0, tagW1) : tagW0;
        const int tagW2 = IMax(tagW0, tagW1);
        int textX = RoundI(plateL + (kSrcScreenL + kSrcTextInset) * s);
        // One line only if it ENDS ON THE SCREEN: past the screen's right edge it would sit on the
        // bright hinge and the wood. [M] The first render put "...STORE" there, half lost.
        const int screenR = RoundI(plateL + kSrcScreenR * s) - Dp(4, dpi);
        L.tagLines = online ? (current->banner.tagline.size() > 1 ? 2 : 1)
                            : ((textX + tagW <= std::min(stopX, screenR)) ? 1 : 2);
        const int wordW = IMax(nameW, L.tagLines == 1 ? tagW : tagW2);
        if (textX + wordW > stopX) textX = stopX - wordW;   // never into the QR
        const int tagLineH = IMax(1, RoundI(kTagLineH * F.tagPx));
        const int wordGap  = Dp(kWordGap, dpi);
        const int blockH   = F.namePx + wordGap + L.tagLines * tagLineH;
        const int screenB  = std::min(static_cast<int>(L.btn[2].bottom) - 1, RoundI(L.photoY + kSrcScreenB * s));
        int blockT = plateT + IMax(Dp(3, dpi), (screenB - plateT - blockH) / 2);
        if (blockT + blockH > L.btn[2].bottom - Dp(3, dpi)) blockT = L.btn[2].bottom - Dp(3, dpi) - blockH;
        L.text.left = textX;            L.text.top = blockT;
        L.text.right = textX + wordW;   L.text.bottom = blockT + blockH;
        L.wordmark = { textX, blockT, textX + nameW, blockT + F.namePx };
        L.namePt.x = textX;
        L.namePt.y = CellTop(blockT, F.namePx, static_cast<int>(F.nameTm.tmHeight));
        for (int i = 0; i < 2; ++i) {
            const int tagTop = blockT + F.namePx + wordGap + i * tagLineH;
            L.tagPt[i].x = textX;
            L.tagPt[i].y = CellTop(tagTop, tagLineH,
                                   static_cast<int>(F.tagTm.tmHeight));
            const int width = (i == 0) ? tagW0 : tagW1;
            L.tagline[i] = { textX, tagTop, textX + width, tagTop + tagLineH };
            L.tagBaseline[i] = tagTop + (tagLineH - static_cast<int>(F.tagTm.tmHeight)) / 2 +
                               static_cast<int>(F.tagTm.tmAscent);
            L.taglineHasEm[i] = online && current->banner.hasHeadlineEm &&
                                !current->banner.headlineEm.empty() &&
                                tags[i].find(current->banner.headlineEm) != std::wstring::npos;
            L.tagEmBaseline[i] = L.tagBaseline[i];
        }
        // The scrim: a soft-edged gradient whose SOLID part is the text block itself, so no glyph
        // ever sits on its fade. It fades out over the margin around the block.
        L.scrim = L.text;
        InflateRect(&L.scrim, Dp(14, dpi), Dp(7, dpi));

        // The viewfinder frames the photo left of the QR.
        L.vf.left = plateL + 1;                  L.vf.top = plateT + 1;
        L.vf.right = L.btn[2].right - 1;
        L.vf.bottom = plateT + plateH - 1;
        L.stage = L.vf;

        // The renderer's fixed preview is 60x128. Keep its device-pixel box explicit so the
        // capture gate can prove the text/phone and phone/QR clearances at 100/150/200%.
        const double phoneW = 60.0;
        const double phoneH = 128.0;
        const double phoneGap = static_cast<double>(theme::Dp(6, dpi));
        const double phoneLeft = static_cast<double>(L.text.right) + phoneGap;
        const double phoneRight = static_cast<double>(L.tile.left) - phoneGap;
        const double phoneTop = static_cast<double>(L.vf.top) + phoneGap;
        const double phoneBottom = static_cast<double>(L.vf.bottom) - phoneGap;
        L.iphone = {};
        if (phoneRight > phoneLeft && phoneBottom > phoneTop) {
            const double phoneScale = std::min((phoneRight - phoneLeft) / phoneW,
                                               (phoneBottom - phoneTop) / phoneH);
            if (phoneScale > 0.0) {
                const double pw = phoneW * phoneScale;
                const double ph = phoneH * phoneScale;
                const double px = phoneLeft + (phoneRight - phoneLeft - pw) / 2.0;
                const double py = phoneTop + (phoneBottom - phoneTop - ph) / 2.0;
                L.iphone = { static_cast<LONG>(std::ceil(px)), static_cast<LONG>(std::ceil(py)),
                             static_cast<LONG>(std::floor(px + pw)),
                             static_cast<LONG>(std::floor(py + ph)) };
            }
        }

        // The offline lockup keeps the exact 71ea5ed composition inside the plate. It is
        // deliberately independent from the online photo/text stage above.
        const int markH = Dp(41, dpi);
        const int markW = RoundI(220.0 * markH / 250.0);
        const int padX = Dp(Sc(10), dpi);
        const int em = IMax(1, RoundI(markH / 5.4));
        const int lockupGap = IMax(1, RoundI(2.0 * em));
        L.fallbackNameTrack = IMax(1, RoundI(0.24 * F.oldNamePx));
        L.fallbackTagTrack = IMax(1, RoundI(0.28 * F.oldTagPx));
        const int colGap = IMax(1, RoundI(0.62 * em));
        const int colH = F.oldNamePx + colGap + F.oldTagPx;
        const int cy = plateT + plateH / 2;
        L.goatMark = { plateL + padX, cy - markH / 2,
                       plateL + padX + markW, cy - markH / 2 + markH };
        L.fallbackNamePt = { L.goatMark.right + lockupGap,
                             CellTop(cy - colH / 2, F.oldNamePx,
                                     static_cast<int>(F.oldNameTm.tmHeight)) };
        L.fallbackTagPt = { L.fallbackNamePt.x,
                            CellTop(cy - colH / 2 + F.oldNamePx + colGap, F.oldTagPx,
                                    static_cast<int>(F.oldTagTm.tmHeight)) };
        L.fallbackWordmark = { L.fallbackNamePt.x, L.fallbackNamePt.y,
                               L.fallbackNamePt.x +
                                   TextWidth(dc, F.oldName, kDaGoatName, L.fallbackNameTrack),
                               L.fallbackNamePt.y + F.oldNamePx };
        L.fallbackTag = { L.fallbackTagPt.x, L.fallbackTagPt.y,
                          L.fallbackTagPt.x +
                              TextWidth(dc, F.oldTag, kDaGoatTag, L.fallbackTagTrack),
                          L.fallbackTagPt.y + F.oldTagPx };
    }

    // ---- the viewfinder and its OSD readout --------------------------------
    {
        L.vfIn   = Dp(kVfIn, dpi);
        L.vfLock = Dp(kVfLock, dpi);
        L.vfArm  = Dp(kVfArm, dpi);
        L.vfBar  = IMax(1, Dp(kVfBar, dpi));

        // .gc-osd: beside the top-left bracket arm (left = vf-in + arm + 6px, top 3px), padding
        // 3px 4px, a 5px gold dot, a 3px gap, then "1080p . 60 fps". Shown only when it clears
        // the cup, the wordmark and the brackets - it never sits on the coffee.
        const int padX = Dp(4, dpi), padY = Dp(3, dpi), dot = Px(5, dpi), gapX = Dp(3, dpi);
        const std::wstring badgeText = current != nullptr && current->online &&
                                               current->banner.hasBadge &&
                                               !current->banner.badge.empty()
                                           ? current->banner.badge : kOsd;
        const int tw = IMax(TextWidth(dc, F.osd, kOsd, 0),
                            TextWidth(dc, F.osd, badgeText, 0));
        const int pillW = padX + dot + gapX + tw + padX;
        const int pillH = padY + F.osdPx + padY;
        L.osdPill.left = L.vf.left + L.vfIn + L.vfArm + Dp(6, dpi);
        L.osdPill.top  = L.btn[2].top + Dp(3, dpi);
        L.osdPill.right  = L.osdPill.left + pillW;
        L.osdPill.bottom = L.osdPill.top + pillH;
        L.osdDot.left = L.osdPill.left + padX;
        L.osdDot.top  = L.osdPill.top + (pillH - dot) / 2;
        L.osdDot.right  = L.osdDot.left + dot;
        L.osdDot.bottom = L.osdDot.top + dot;
        L.osdText.x = L.osdDot.right + gapX;
        L.osdText.y = CellTop(L.osdPill.top + padY, F.osdPx, static_cast<int>(F.osdTm.tmHeight));
        RECT hit;
        L.badge = L.osdPill;
        L.osdOn = !IntersectRect(&hit, &L.osdPill, &L.text) &&
                  L.osdPill.left >= L.btn[2].left && L.osdPill.top >= L.btn[2].top &&
                  L.osdPill.right <= L.btn[2].right && L.osdPill.bottom <= L.btn[2].bottom;
    }
}

// ===========================================================================
// GDI+ and the photo. Both are started on the FIRST PAINT of the strip, never earlier: the
// control is created (hidden) even when WebView2 draws the strip, and on that path neither
// is ever touched.
// ===========================================================================
HINSTANCE  g_inst = nullptr;
ULONG_PTR  g_gdiplusToken = 0;
bool       g_gdiplusTried = false;
gp::Bitmap* g_photo = nullptr;
std::string g_photoKey;
gp::Bitmap* g_iphone = nullptr;
bool        g_iphoneTried = false;

// The delay-load helper's two failure codes (delayhlp.cpp: ERROR_MOD_NOT_FOUND when the DLL will
// not load, ERROR_PROC_NOT_FOUND when an import is missing from it). Nothing else is caught.
int DelayLoadFilter(DWORD code) {
    return (code == VcppException(ERROR_SEVERITY_ERROR, ERROR_MOD_NOT_FOUND) ||
            code == VcppException(ERROR_SEVERITY_ERROR, ERROR_PROC_NOT_FOUND))
               ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH;
}

enum class GdiplusBind { kBound, kNotDelayLoaded, kFailed };

// Binds every gdiplus.dll import now, so no later GDI+ call can raise. No C++ objects in here:
// __try cannot unwind them (C2712). __HrLoadAllImportsForDll matches the descriptor's name
// CASE-SENSITIVELY, and the linker records it as "gdiplus.dll". It RETURNS a failure only when
// the DLL is not delay-loaded at all; a DLL that will not load makes it RAISE.
GdiplusBind BindGdiplusImports() {
    __try {
        return SUCCEEDED(__HrLoadAllImportsForDll("gdiplus.dll")) ? GdiplusBind::kBound
                                                                  : GdiplusBind::kNotDelayLoaded;
    } __except (DelayLoadFilter(GetExceptionCode())) {
        return GdiplusBind::kFailed;
    }
}

bool EnsureGdiplus() {
    if (!g_gdiplusTried) {
        g_gdiplusTried = true;
        g_gdiplusToken = 0;
        const GdiplusBind bind = BindGdiplusImports();
        // A build that imports gdiplus.dll normally (no /DELAYLOAD) has nothing to bind: the
        // loader mapped it at start-up, or the process would not be running.
        const bool usable = bind == GdiplusBind::kBound ||
                            (bind == GdiplusBind::kNotDelayLoaded && GetModuleHandleW(L"gdiplus.dll") != nullptr);
        if (usable) {
            // ponytail: started once and never shut down - process exit reclaims it, and shutting
            // down early would invalidate the cached photo. Add GdiplusShutdown at exit if this
            // file is ever built into a DLL.
            gp::GdiplusStartupInput in;
            if (gp::GdiplusStartup(&g_gdiplusToken, &in, nullptr) != gp::Ok) g_gdiplusToken = 0;
        }
    }
    return g_gdiplusToken != 0;
}

bool DecodeBitmapBytes(const void* data, DWORD size, gp::Bitmap** output) {
    if (output == nullptr) return false;
    *output = nullptr;
    if (data == nullptr || size == 0) return false;
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, size);
    if (mem == nullptr) return false;
    void* p = GlobalLock(mem);
    if (p == nullptr) { GlobalFree(mem); return false; }
    std::memcpy(p, data, size);
    GlobalUnlock(mem);
    IStream* stream = nullptr;
    if (FAILED(CreateStreamOnHGlobal(mem, TRUE, &stream))) { GlobalFree(mem); return false; }
    bool ok = false;
    {
        // Decoded into a PARGB copy so nothing keeps the source stream alive.
        gp::Bitmap src(stream);
        if (src.GetLastStatus() == gp::Ok && src.GetWidth() > 0 && src.GetHeight() > 0) {
            const INT w = static_cast<INT>(src.GetWidth()), h = static_cast<INT>(src.GetHeight());
            // GDI+ classes carry their own operator new (GdipAlloc), which returns null rather than throwing.
            std::unique_ptr<gp::Bitmap> copy(new gp::Bitmap(w, h, PixelFormat32bppPARGB));
            if (copy && copy->GetLastStatus() == gp::Ok) {
                gp::Status drawn = gp::GenericError;
                {
                    gp::Graphics g(copy.get());
                    if (g.GetLastStatus() == gp::Ok) drawn = g.DrawImage(&src, 0, 0, w, h);
                }   // the Graphics flushes into `copy` here, before anything can publish it
                if (drawn == gp::Ok) {
                    *output = copy.release();
                    ok = true;
                }
            }
        }
    }
    stream->Release();   // frees `mem` too (fDeleteOnRelease)
    return ok;
}

gp::Bitmap* Photo(const ob::Model& model) {
    if (!model.online || model.jpg.empty()) return nullptr;
    const std::string key = model.banner.sha256Jpg.empty() ? model.jpg : model.banner.sha256Jpg;
    if (key != g_photoKey) {
        delete g_photo;
        g_photo = nullptr;
        g_photoKey = key;
        DecodeBitmapBytes(model.jpg.data(), static_cast<DWORD>(model.jpg.size()), &g_photo);
    }
    return g_photo;
}

gp::Bitmap* IPhonePreview() {
    if (!g_iphoneTried) {
        g_iphoneTried = true;
        DecodeBitmapBytes(kIphonePreviewPng, static_cast<DWORD>(kIphonePreviewPngSize), &g_iphone);
    }
    return g_iphone;
}

// ===========================================================================
// Control state
// ===========================================================================
struct SponsorState {
    GoatMotion goat;          // the plate's motion; Ko-fi and Star have none
    int    bannerToken = 0;
    int    hot     = -1;
    int    focus   = -1;
    int    pressed = -1;
    bool   tracking = false;
    bool   timerOn  = false;
    bool   qrHot    = false;   // the pointer is on the QR tile
    bool   kbdFocus = false;   // the focus arrived by keyboard (Tab, arrows), not a click
    bool   magShown = false;   // this strip has the QR magnifier up

    Layout  layout{};
    FontSet fonts;      // owns its HFONTs and frees them in its destructor
    bool   layoutValid = false;
    int    dpi = 96;

    Geo    geo;
    Layer  back;        // THE back buffer

    HWND   tip = nullptr;

    std::wstring url[3];
};

SponsorState* Get(HWND h) {
    if (h == nullptr) return nullptr;
    return reinterpret_cast<SponsorState*>(GetWindowLongPtrW(h, GWLP_USERDATA));
}

constexpr UINT_PTR kTimerId = 1;
constexpr UINT     kTickMs  = 16;

void SyncTimer(HWND hwnd, SponsorState* st) {
    // THE CPU RULE. Nothing moving means no timer at all - not a slower one.
    const bool want = IsWindowVisible(hwnd) && st->goat.Busy(NowMs());
    if (want && !st->timerOn) {
        st->timerOn = SetTimer(hwnd, kTimerId, kTickMs, nullptr) != 0;
    } else if (!want && st->timerOn) {
        KillTimer(hwnd, kTimerId);
        st->timerOn = false;
    }
}

// The QR magnifier (qr_magnifier.h): up while the pointer is on the QR tile, or while the plate
// has focus that arrived by KEYBOARD; down otherwise. A click on the plate focuses it too, and
// that must not pop the magnifier up - hence kbdFocus.
void SyncMagnifier(HWND hwnd, SponsorState* st) {
    const bool want = (st->hot == 2 && st->qrHot) || (st->kbdFocus && st->focus == 2);
    if (want) {
        RECT r = st->layout.tile;
        SetLastError(0);
        if (MapWindowPoints(hwnd, nullptr, reinterpret_cast<POINT*>(&r), 2) == 0 && GetLastError() != 0) {
            if (st->magShown) {
                QrMagnifierHide();   // fail closed if client-to-screen mapping fails
                st->magShown = false;
            }
            return;
        }
        QrMagnifierShow(hwnd, r, st->dpi);   // a no-op while it is already up for this tile
        st->magShown = true;
    } else if (st->magShown) {
        QrMagnifierHide();
        st->magShown = false;
    }
}

// Down at once, no fade: the strip moved, resized, hid, changed dpi or is going away.
void DropMagnifier(SponsorState* st) {
    if (st->magShown) QrMagnifierHideNow();
    st->magShown = false;
}

// Hover or keyboard focus on the plate plays the GOAT Cam hover state. Ko-fi and Star repaint
// (their handlers invalidate) but have nothing to animate.
void RefreshActive(HWND hwnd, SponsorState* st) {
    const bool on = (st->hot == 2) || (st->focus == 2);
    if (on != st->goat.active) {
        st->goat.SetActive(on, NowMs(), AnimationsEnabled());
        InvalidateRect(hwnd, nullptr, FALSE);
    }
    SyncTimer(hwnd, st);
    SyncMagnifier(hwnd, st);
}

void EnsureLayout(HWND hwnd, SponsorState* st, HDC dc, const ob::Model* current = nullptr) {
    const int dpi = DpiOfWnd(hwnd);
    // WM_SIZE and WM_DPICHANGED_AFTERPARENT clear layoutValid, so this only rebuilds when
    // something actually moved.
    if (st->layoutValid && st->dpi == dpi) return;
    RECT client;
    GetClientRect(hwnd, &client);
    st->fonts.Build(dpi);   // no-op unless the DPI actually changed
    const ob::Model model = current != nullptr ? *current : BannerHostCurrent();
    ComputeLayout(dc, dpi, client, st->fonts, st->layout, &model);
    st->dpi = dpi;
    st->layoutValid = true;
    st->geo.valid = false;
}

int HitTest(SponsorState* st, int x, int y) {
    for (int i = 0; i < 3; ++i) {
        const RECT& r = st->layout.btn[i];
        if (x >= r.left && x < r.right && y >= r.top && y < r.bottom) return i;
    }
    return -1;
}

void BuildBloom(Geo&);
void BuildMask(Geo&, int, int);

void BuildGeo(SponsorState* st) {
    // Ko-fi cup: viewBox 0 0 24 24 rendered into the icon rect.
    const RECT& r = st->layout.kofiIcon;
    const double k = (r.right - r.left) / 24.0;
    st->geo.kofiCup    = BuildDevPath(kArt::KofiCup(),    k, k, r.left, r.top);
    st->geo.kofiSaucer = BuildDevPath(kArt::KofiSaucer(), k, k, r.left, r.top);

    // DaGoat lockup: use the exact 71ea5ed mark box, not the online photo's width or font.
    // Keeping this geometry independent prevents an online model from shrinking the fallback.
    const RECT& plate = st->layout.btn[2];
    const RECT& mark = st->layout.goatMark;
    const int markH = IMax(1, mark.bottom - mark.top);
    st->geo.s = markH / 250.0;
    const double otx = mark.left - 46.0 * st->geo.s;
    const double oty = mark.top + 16.0 * st->geo.s;
    for (int i = 0; i < 5; ++i)
        st->geo.head[i] = BuildDevPath(kArt::Head(i), st->geo.s, st->geo.s, otx, oty);
    st->geo.eye = BuildDevPath(kArt::Eye(), st->geo.s, st->geo.s, otx, oty);
    const double sx0 = otx + kArt::kSealX * st->geo.s;
    const double sy0 = oty + kArt::kSealY * st->geo.s;
    st->geo.sealRc.left = RoundI(sx0);
    st->geo.sealRc.top = RoundI(sy0);
    st->geo.sealRc.right = RoundI(sx0 + kArt::kSealW * st->geo.s);
    st->geo.sealRc.bottom = RoundI(sy0 + kArt::kSealH * st->geo.s);
    st->geo.sealRadius = IMax(1, RoundI(kArt::kSealRx * st->geo.s));
    st->geo.glyphOrg = POINT{st->geo.sealRc.left, st->geo.sealRc.top};
    st->geo.glyphW = IMax(1, st->geo.sealRc.right - st->geo.sealRc.left);
    st->geo.glyphH = IMax(1, st->geo.sealRc.bottom - st->geo.sealRc.top);
    st->geo.inner = st->geo.s * (kArt::kSealW / kArt::kInnerBox);
    st->geo.chr = BuildDevPath(kArt::Char(), st->geo.inner, st->geo.inner,
                               st->geo.glyphOrg.x, st->geo.glyphOrg.y);
    Contours moved = kArt::Thumb();
    for (SubPath& path : moved)
        for (PtF& p : path) {
            p.x += kArt::kThumbDx;
            p.y += kArt::kThumbDy;
        }
    st->geo.thumb = BuildDevPath(moved, st->geo.inner, st->geo.inner,
                                 st->geo.glyphOrg.x, st->geo.glyphOrg.y);
    BuildBloom(st->geo);
    BuildMask(st->geo, st->layout.btn[2].right - st->layout.btn[2].left,
              st->layout.btn[2].bottom - st->layout.btn[2].top);
    st->geo.valid = true;
}

// ===========================================================================
// Painting - Ko-fi and Star, unchanged.
// ===========================================================================
void DrawStar(HDC dc, const RECT& r, COLORREF colour) {
    const double cx = (r.left + r.right) * 0.5, cy = (r.top + r.bottom) * 0.5;
    const double R = (r.right - r.left) * 0.5, ri = R * 0.382;
    POINT pts[10];
    for (int i = 0; i < 10; ++i) {
        const double a = -1.57079632679 + i * 3.14159265358979 / 5.0;
        const double rr = (i % 2 == 0) ? R : ri;
        pts[i].x = RoundI(cx + rr * std::cos(a));
        pts[i].y = RoundI(cy + rr * std::sin(a));
    }
    HBRUSH br = CreateSolidBrush(colour);
    if (br == nullptr) return;
    {
        // The inner scope matters: DeleteObject on a still-selected object silently fails,
        // which is how GDI handles leak. Deselect first, then delete.
        SelectScope bs(dc, br);
        SelectScope ps(dc, GetStockObject(NULL_PEN));
        Polygon(dc, pts, 10);
    }
    DeleteObject(br);
}

void DrawFocusRing(HDC dc, const RECT& r, int dpi, int radius) {
    RECT f = r;
    const int in = theme::Dp(theme::metric::kFocusInset, dpi);
    InflateRect(&f, -in, -in);
    HPEN pen = CreatePen(PS_SOLID, IMax(1, theme::Dp(1, dpi)), theme::P().borderStrong);
    if (pen == nullptr) return;
    {
        SelectScope ps(dc, pen);
        SelectScope bs(dc, GetStockObject(NULL_BRUSH));
        RoundRect(dc, f.left, f.top, f.right, f.bottom, radius * 2, radius * 2);
    }
    DeleteObject(pen);
}

void PaintKofi(HDC dc, SponsorState* st, int dpi) {
    const Layout& L = st->layout;
    RECT r = L.btn[0];
    const bool hot = (st->hot == 0);
    // options.css: .kofi:active { transform: translateY(1px) }
    const int dy = (st->pressed == 0) ? theme::Dp(1, dpi) : 0;
    OffsetRect(&r, 0, dy);

    theme::FillRoundRect(dc, r, L.kofiRadius, hot ? kKofiBgHover : kKofiBg);

    if (st->geo.valid) {
        // The cup is built at the un-offset icon rect; shift the DC instead of rebuilding.
        POINT old{};
        OffsetViewportOrgEx(dc, 0, dy, &old);
        FillDevPath(dc, st->geo.kofiCup, kKofiText);
        FillDevPath(dc, st->geo.kofiSaucer, kKofiText);
        SetViewportOrgEx(dc, old.x, old.y, nullptr);
    }

    // Two lines, same size, same weight, centred on each other - the reference render puts
    // both line centres on the same x to the pixel.
    DrawWithFont(dc, L.kofiTitlePt.x,  L.kofiTitlePt.y + dy,  kKofiTitle,
                 st->fonts.label, kKofiText, 0);
    DrawWithFont(dc, L.kofiHandlePt.x, L.kofiHandlePt.y + dy, kKofiHandle,
                 st->fonts.label, kKofiText, 0);

    if (st->focus == 0) DrawFocusRing(dc, r, dpi, L.kofiRadius);
}

void PaintGitHub(HDC dc, SponsorState* st, int dpi) {
    const Layout& L = st->layout;
    RECT r = L.btn[1];
    const bool hot = (st->hot == 1);
    const int dy = (st->pressed == 1) ? theme::Dp(1, dpi) : 0;
    OffsetRect(&r, 0, dy);

    const theme::Palette& P = theme::P();
    theme::DrawRoundRect(dc, r, L.btnRadius,
                         hot ? P.cardBgAlt : P.cardBg, hot ? P.borderStrong : P.border);

    RECT s = L.ghStar;  OffsetRect(&s, 0, dy);
    DrawStar(dc, s, hot ? P.accentHover : P.accent);
    DrawWithFont(dc, L.ghLabelPt.x, L.ghLabelPt.y + dy, kGhLabel,
                 st->fonts.labelReg, P.textPrimary, 0);

    if (st->focus == 1) DrawFocusRing(dc, r, dpi, L.btnRadius);
}

// ===========================================================================
// Painting - GOAT Cam.
// ===========================================================================
// The plate's GDI+ layer: the photo across the whole plate, the scrim behind the wordmark, the
// viewfinder, the OSD pill, the border, the focus ring and the QR tile - everything that needs
// anti-aliasing or alpha. Text and QR modules come after, in GDI. `bits` is the back buffer.
void PaintPlateGdiplus(SponsorState* st, BYTE* bits, int dpi, double now,
                       const ob::Model& model) {
    const Layout& L = st->layout;
    const GoatMotion& m = st->goat;
    const double push = m.push.At(now), brk = m.bracket.At(now), lift = m.lift.At(now);
    const double qr = m.qr.At(now), lock = m.lock.At(now);

    gp::Bitmap wrap(st->back.W(), st->back.H(), st->back.W() * 4, PixelFormat32bppRGB, bits);
    if (wrap.GetLastStatus() != gp::Ok) return;
    gp::Graphics g(&wrap);
    g.SetPageUnit(gp::UnitPixel);
    g.SetSmoothingMode(gp::SmoothingModeAntiAlias);
    g.SetPixelOffsetMode(gp::PixelOffsetModeHalf);
    g.SetInterpolationMode(gp::InterpolationModeHighQualityBicubic);

    const gp::RectF plateF = ToRectF(L.btn[2]);
    const float radius = static_cast<float>(L.plateRadius);

    gp::GraphicsPath platePath;
    AddRoundRect(platePath, plateF, radius);
    {
        gp::SolidBrush b(Gc(kEspresso950));
        g.FillPath(&b, &platePath);
    }

    // The inside of the plate: everything drawn in it is clipped by its rounded corners.
    gp::GraphicsPath innerPath;
    {
        gp::RectF ir = plateF;
        ir.Inflate(-1.0f, -1.0f);
        AddRoundRect(innerPath, ir, radius - 1.0f);
    }

    {
        gp::Region clip(&innerPath);
        g.SetClip(&clip);

        // ---- the photo, edge to edge, pushed in about the cup on hover ----
        if (gp::Bitmap* img = Photo(model)) {
            const float iw = static_cast<float>(img->GetWidth()), ih = static_cast<float>(img->GetHeight());
            const float k  = static_cast<float>(1.0 + sponsor_motion::kPushScale * push);
            // The layout measured the photo in its 800-px source pixels; a different decode size
            // is rescaled here so the same source pixels still land in the same place.
            const float s  = L.photoS * static_cast<float>(kSrcW) / iw * k;
            const float x0 = L.pushX - (L.pushX - L.photoX) * k;
            const float y0 = L.pushY - (L.pushY - L.photoY) * k;
            const gp::RectF dst(x0, y0, iw * s, ih * s);
            gp::ImageAttributes ia;
            ia.SetWrapMode(gp::WrapModeTileFlipXY);   // no soft seam where the image meets the clip
            g.DrawImage(img, dst, 0.0f, 0.0f, iw, ih, gp::UnitPixel, &ia);
        } else {
            gp::SolidBrush b(Gc(kEspresso800));
            g.FillRectangle(&b, plateF);
        }

        // Static preview art from the Maps renderer, deliberately not part of the signed
        // banner. Keep at least six device pixels clear of both the text and QR columns.
        if (gp::Bitmap* phone = IPhonePreview()) {
            const float iw = static_cast<float>(phone->GetWidth());
            const float ih = static_cast<float>(phone->GetHeight());
            if (L.iphone.right > L.iphone.left && L.iphone.bottom > L.iphone.top &&
                iw > 0.0f && ih > 0.0f) {
                const gp::RectF dst(static_cast<float>(L.iphone.left),
                                    static_cast<float>(L.iphone.top),
                                    static_cast<float>(L.iphone.right - L.iphone.left),
                                    static_cast<float>(L.iphone.bottom - L.iphone.top));
                g.DrawImage(phone, dst, 0.0f, 0.0f, iw, ih, gp::UnitPixel);
            }
        }

        // ---- the scrim: a local espresso gradient behind the wordmark, not a box. The path is
        // the text block plus a margin, rounded; the focus scales make the full-strength part
        // exactly the text block, and the margin is the fade. ----
        {
            const gp::RectF sr = ToRectF(L.scrim);
            const gp::RectF tr = ToRectF(L.text);
            gp::GraphicsPath ep;
            // A small radius, so the scaled-down solid part keeps the text block's square corners.
            AddRoundRect(ep, sr, static_cast<float>(theme::Dp(8, dpi)));
            gp::PathGradientBrush pb(&ep);
            pb.SetCenterPoint(gp::PointF(tr.X + tr.Width / 2.0f, tr.Y + tr.Height / 2.0f));
            pb.SetCenterColor(Gc(kEspresso950, kScrimA));
            gp::Color edge = Gc(kEspresso950, 0.0);
            INT n = 1;
            pb.SetSurroundColors(&edge, &n);
            pb.SetFocusScales(sr.Width > 0.0f ? tr.Width / sr.Width : 0.0f,
                              sr.Height > 0.0f ? tr.Height / sr.Height : 0.0f);
            g.FillPath(&pb, &ep);
        }

        // ---- the viewfinder: four 2px corner brackets, cream .62, tightening 3px on hover and
        // warming to gold (.gc-vf::after). Whole pixels, so they stay crisp while they move.
        if (model.banner.viewfinder) {
            g.SetSmoothingMode(gp::SmoothingModeNone);
            g.SetPixelOffsetMode(gp::PixelOffsetModeNone);
            RECT stage = ScaleRectAboutCenter(L.stage, 1.0 + sponsor_motion::kStageScale * brk);
            RECT clippedStage{};
            if (IntersectRect(&clippedStage, &stage, &L.btn[2])) stage = clippedStage;
            const int in = L.vfIn + RoundI(L.vfLock * brk);
            const int arm = L.vfArm, bw = L.vfBar;
            const int l = stage.left + in, t = stage.top + in;
            const int r = stage.right - in, b = stage.bottom - in;
            const gp::Rect bars[8] = {
                gp::Rect(l, t, arm, bw),         gp::Rect(l, t, bw, arm),
                gp::Rect(r - arm, t, arm, bw),   gp::Rect(r - bw, t, bw, arm),
                gp::Rect(l, b - bw, arm, bw),    gp::Rect(l, b - arm, bw, arm),
                gp::Rect(r - arm, b - bw, arm, bw), gp::Rect(r - bw, b - arm, bw, arm),
            };
            {
                gp::SolidBrush cream(Gc(kCream, 0.62));
                for (const gp::Rect& br : bars) g.FillRectangle(&cream, br);
            }
            if (lock > 0.002) {
                gp::SolidBrush gold(Gc(kGold, lock));
                for (const gp::Rect& br : bars) g.FillRectangle(&gold, br);
            }
            g.SetPixelOffsetMode(gp::PixelOffsetModeHalf);
            g.SetSmoothingMode(gp::SmoothingModeAntiAlias);

            if (L.osdOn) {   // the pill and its gold dot; the dot does not pulse (the timer rule)
                gp::GraphicsPath pp;
                AddRoundRect(pp, ToRectF(L.osdPill), static_cast<float>(theme::Dp(4, dpi)));
                gp::SolidBrush pb(Gc(kEspresso950, 0.62));
                g.FillPath(&pb, &pp);
                gp::SolidBrush db(Gc(kGold));
                g.FillEllipse(&db, ToRectF(L.osdDot));
            }
        }
        if (model.banner.steam) {
            const float sx = plateF.X + static_cast<float>(Clamp01(model.banner.steamAt[0])) * plateF.Width;
            const float sy = plateF.Y + static_cast<float>(Clamp01(model.banner.steamAt[1])) * plateF.Height;
            gp::SolidBrush steam(Gc(kGold, 0.20));
            for (int i = 0; i < 3; ++i) {
                const float x = sx + static_cast<float>((i - 1) * theme::Dp(5, dpi));
                const float y = sy - static_cast<float>(theme::Dp(8 + i * 4, dpi));
                g.FillEllipse(&steam, x, y, static_cast<float>(theme::Dp(3, dpi)),
                              static_cast<float>(theme::Dp(9, dpi)));
            }
        }
        g.ResetClip();
    }

    // ---- the border; the lift warms it to the web lift-shadow's 1px gold ring ----
    {
        gp::GraphicsPath bp;
        gp::RectF br = plateF;
        br.Inflate(-0.5f, -0.5f);
        AddRoundRect(bp, br, radius - 0.5f);
        gp::Pen pen(Gc(kCream, 0.14), 1.0f);
        g.DrawPath(&pen, &bp);
        if (lift > 0.002) {
            gp::Pen gold(Gc(kGold, kLiftRingA * lift), 1.0f);
            g.DrawPath(&gold, &bp);
        }
    }

    // ---- keyboard focus, inside the plate (no room outside it in this row). Drawn BEFORE the
    // QR tile so the tile covers it: a line through the code's quiet zone could cost a scan.
    if (st->focus == 2) {
        gp::GraphicsPath fp;
        gp::RectF fr = plateF;
        const float fin = static_cast<float>(theme::Dp(3, dpi));
        fr.Inflate(-fin + 0.5f, -fin + 0.5f);
        AddRoundRect(fp, fr, radius - fin);
        gp::Pen pen(Gc(kCream, 0.72), static_cast<float>(IMax(1, theme::Dp(1, dpi))));
        g.DrawPath(&pen, &fp);
    }

    // ---- the QR tile, and its caramel ring on hover (clipped inside the plate, as the web's
    // overflow: clip does to the box-shadow ring) ----
    const gp::RectF tileF = ToRectF(L.tile);
    if (qr > 0.002) {
        gp::Region clip(&innerPath);
        g.SetClip(&clip);
        gp::GraphicsPath rp;
        gp::RectF rr = tileF;
        rr.Inflate(1.0f, 1.0f);
        AddRoundRect(rp, rr, static_cast<float>(theme::Dp(kQrRadius, dpi)) + 1.0f);
        gp::Pen pen(Gc(kCaramel, 0.6 * qr), 2.0f);
        g.DrawPath(&pen, &rp);
        g.ResetClip();
    }
    {
        gp::GraphicsPath tp;
        AddRoundRect(tp, tileF, static_cast<float>(theme::Dp(kQrRadius, dpi)));
        gp::SolidBrush b(Gc(kQrLight));
        g.FillPath(&b, &tp);
    }
}   // the Graphics flushes into the bits here

// The plate WITHOUT GDI+ - gdiplus.dll would not load, or the back buffer could not be made.
// Plain GDI, so no alpha: the plate, its border (warming to gold with the lift, blended by hand),
// the focus ring and the QR tile. No photo, brackets or OSD, which need GDI+. The headline, the
// wordmark, the tagline and the QR modules are GDI on both paths and draw as usual.
void PaintPlateGdi(HDC dc, const SponsorState* st, int dpi, double now) {
    const Layout& L = st->layout;
    const double lift = st->goat.lift.At(now);
    const Rgb edge = Mix(Mix(kEspresso950, kCream, 0.14), kGold, kLiftRingA * lift);
    theme::DrawRoundRect(dc, L.btn[2], L.plateRadius, Cr(kEspresso950), Cr(edge));
    if (st->focus == 2) {   // before the tile, so the tile covers it, as on the GDI+ path
        RECT f = L.btn[2];
        const int in = theme::Dp(3, dpi);
        InflateRect(&f, -in, -in);
        HPEN pen = CreatePen(PS_SOLID, IMax(1, theme::Dp(1, dpi)), Cr(Mix(kEspresso950, kCream, 0.72)));
        if (pen != nullptr) {
            {
                SelectScope ps(dc, pen);
                SelectScope bs(dc, GetStockObject(NULL_BRUSH));
                const int r = IMax(0, L.plateRadius - in);
                RoundRect(dc, f.left, f.top, f.right, f.bottom, r * 2, r * 2);
            }
            DeleteObject(pen);
        }
    }
    theme::FillRoundRect(dc, L.tile, theme::Dp(kQrRadius, dpi), Cr(kQrLight));
}

void DrawHeadlinePart(HDC dc, int x, int baseline, const std::wstring& text,
                      const ob::Banner& banner, const FontSet& F) {
    if (text.empty()) return;
    size_t em = std::wstring::npos;
    if (banner.hasHeadlineEm && !banner.headlineEm.empty()) em = text.find(banner.headlineEm);
    if (em == std::wstring::npos) {
        DrawOnBaseline(dc, x, baseline, text, F.copy, Cr(kInkCream));
        return;
    }
    const std::wstring before = text.substr(0, em);
    const std::wstring after = text.substr(em + banner.headlineEm.size());
    DrawOnBaseline(dc, x, baseline, before, F.copy, Cr(kInkCream));
    x += TextWidth(dc, F.copy, before, 0);
    DrawOnBaseline(dc, x, baseline, banner.headlineEm, F.now, Cr(kGold));
    x += TextWidth(dc, F.now, banner.headlineEm, 0);
    DrawOnBaseline(dc, x, baseline, after, F.copy, Cr(kInkCream));
}

void DrawTaglinePart(HDC dc, int x, int baseline, const std::wstring& text,
                     const ob::Banner& banner, const FontSet& F, COLORREF colour, int track) {
    if (text.empty()) return;
    const std::wstring& emphasis = banner.headlineEm;
    if (!banner.hasHeadlineEm || emphasis.empty()) {
        DrawOnBaseline(dc, x, baseline, text, F.tag, colour, track);
        return;
    }
    std::size_t at = 0;
    for (;;) {
        const std::size_t hit = text.find(emphasis, at);
        if (hit == std::wstring::npos) {
            DrawOnBaseline(dc, x, baseline, text.substr(at), F.tag, colour, track);
            return;
        }
        const std::wstring before = text.substr(at, hit - at);
        DrawOnBaseline(dc, x, baseline, before, F.tag, colour, track);
        x += TextWidth(dc, F.tag, before, track);
        DrawOnBaseline(dc, x, baseline, emphasis, F.now, Cr(kGold), 0);
        x += TextWidth(dc, F.now, emphasis, 0);
        at = hit + emphasis.size();
    }
}

void PaintOnlineGoat(HDC dc, BYTE* bits, SponsorState* st, int dpi, double now,
                     const ob::Model& model) {
    const Layout& L = st->layout;
    const FontSet& F = st->fonts;

    // ---- 1. the signed headline, split at its em dash into exactly two rows ----
    std::wstring line1 = model.banner.headline;
    std::wstring line2;
    const size_t dash = line1.find(L'\u2014');
    if (dash != std::wstring::npos) {
        line2 = line1.substr(dash + 1);
        line1.resize(dash + 1);  // Maps keeps the dash at the end of row one.
        while (!line2.empty() && line2.front() == L' ') line2.erase(line2.begin());
    }
    if (line1.empty()) line1 = kCopy1;
    DrawHeadlinePart(dc, L.copyX, L.copyBase[0], line1, model.banner, F);
    DrawHeadlinePart(dc, L.copyX, L.copyBase[1], line2, model.banner, F);

    // ---- 2. the plate's shapes and photo (GDI+, into the same bits) ----
    // EVERY GDI+ CALL IN THIS FILE IS BEHIND THIS ONE TEST: PaintPlateGdiplus and, through it,
    // Photo(). When it fails, nothing below touches GDI+.
    const bool rich = bits != nullptr && EnsureGdiplus();
    if (rich) {
        GdiFlush();
        PaintPlateGdiplus(st, bits, dpi, now, model);
    } else {
        PaintPlateGdi(dc, st, dpi, now);
    }

    // The signed matrix includes its own quiet zone. Keep all 37x37 module samples on a solid
    // light field; the rounded plate/tile corner must not eat the outer quiet-zone modules.
    HBRUSH qrTile = CreateSolidBrush(Cr(kQrLight));
    if (qrTile != nullptr) {
        FillRect(dc, &L.tile, qrTile);
        DeleteObject(qrTile);
    }

    // ---- 3. the QR modules and the plate's text (GDI) ----
    {
        HBRUSH dark = CreateSolidBrush(Cr(kQrDark));
        if (dark != nullptr) {
            const int mod = L.module, qx = L.qrOrg.x, qy = L.qrOrg.y;
            for (int y = 0; y < kQrN; ++y) {
                for (int x = 0; x < kQrN;) {
                    int run = x;
                    if (static_cast<size_t>(y) < model.banner.qrRows.size()) {
                        const std::string& row = model.banner.qrRows[static_cast<size_t>(y)];
                        while (run < kQrN && static_cast<size_t>(run) < row.size() &&
                               row[static_cast<size_t>(run)] == '1') ++run;
                    }
                    if (run == x) { ++x; continue; }
                    RECT r = { qx + x * mod, qy + y * mod, qx + run * mod, qy + (y + 1) * mod };
                    FillRect(dc, &r, dark);
                    x = run;
                }
            }
            DeleteObject(dark);
        }
    }
    if (L.osdOn && rich) {  // the signed badge belongs at the photo's top-left
        const std::wstring badge = model.banner.hasBadge && !model.banner.badge.empty()
                                       ? model.banner.badge : kOsd;
        DrawWithFont(dc, L.osdText.x, L.osdText.y, badge, F.osd, Cr(kCream), 0);
    }
    const std::wstring wordmark = model.banner.wordmark.empty() ? kGoatName : model.banner.wordmark;
    DrawWithFont(dc, L.namePt.x, L.namePt.y, wordmark, F.name, Cr(kCream), 0);
    const COLORREF tagCol = Cr(Mix(kMuted, kCream, st->goat.tag.At(now)));
    if (!model.banner.tagline.empty()) {
        for (size_t i = 0; i < model.banner.tagline.size() && i < 2; ++i) {
            DrawTaglinePart(dc, L.tagPt[i].x, L.tagBaseline[i], model.banner.tagline[i],
                            model.banner, F, tagCol, L.tagTrack);
        }
    } else if (L.tagLines == 1) {
        DrawWithFont(dc, L.tagPt[0].x, L.tagPt[0].y, kGoatTag, F.tag, tagCol, L.tagTrack);
    } else {
        DrawWithFont(dc, L.tagPt[0].x, L.tagPt[0].y, kGoatTag1, F.tag, tagCol, L.tagTrack);
        DrawWithFont(dc, L.tagPt[1].x, L.tagPt[1].y, kGoatTag2, F.tag, tagCol, L.tagTrack);
    }
    GdiFlush();
}

// DaGoat fallback art restored from 71ea5ed. It deliberately has no signed photo or QR: the
// online path above is the only path that consumes ob::Model bytes. These are the original
// cached seal bloom and radial meteor mask builders, retained even though the compact fallback
// also remains safe when the optional alpha layer cannot be allocated.
void BuildBloom(Geo& g) {
    const int sw = g.sealRc.right - g.sealRc.left;
    const int sh = g.sealRc.bottom - g.sealRc.top;
    if (sw <= 0 || sh <= 0) return;
    const int pad = IMax(3, RoundI(DMax(2.0, sw / 3.0) * 2.2));
    const int w = sw + pad * 2, h = sh + pad * 2;
    if (!g.bloom.Ensure(w, h)) return;
    g.bloom.Clear();
    g.bloomOrg.x = g.sealRc.left - pad;
    g.bloomOrg.y = g.sealRc.top - pad;
    std::vector<float> alpha(static_cast<size_t>(w) * h, 0.0f);
    const int rad = IMax(1, RoundI(kArt::kSealRx * g.s));
    for (int y = 0; y < sh; ++y) {
        for (int x = 0; x < sw; ++x) {
            const int dx = (x < rad) ? (rad - x) : (x >= sw - rad ? x - (sw - rad - 1) : 0);
            const int dy = (y < rad) ? (rad - y) : (y >= sh - rad ? y - (sh - rad - 1) : 0);
            if (dx > 0 && dy > 0 && dx * dx + dy * dy > rad * rad) continue;
            alpha[static_cast<size_t>(y + pad) * w + x + pad] = 1.0f;
        }
    }
    std::vector<float> tmp(alpha.size(), 0.0f);
    const int blur = IMax(1, RoundI(DMax(2.0, sw / 3.0) * 0.9));
    for (int pass = 0; pass < 3; ++pass) {
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                float sum = 0.0f;
                int count = 0;
                for (int k = -blur; k <= blur; ++k) {
                    const int xx = x + k;
                    if (xx >= 0 && xx < w) { sum += alpha[static_cast<size_t>(y) * w + xx]; ++count; }
                }
                tmp[static_cast<size_t>(y) * w + x] = count == 0 ? 0.0f : sum / count;
            }
        }
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                float sum = 0.0f;
                int count = 0;
                for (int k = -blur; k <= blur; ++k) {
                    const int yy = y + k;
                    if (yy >= 0 && yy < h) { sum += tmp[static_cast<size_t>(yy) * w + x]; ++count; }
                }
                alpha[static_cast<size_t>(y) * w + x] = count == 0 ? 0.0f : sum / count;
            }
        }
    }
    BYTE* bits = g.bloom.Bits();
    for (int i = 0; i < w * h; ++i) {
        const BYTE a = Chan(alpha[static_cast<size_t>(i)] * 255.0);
        BYTE* p = bits + static_cast<size_t>(i) * 4;
        p[0] = static_cast<BYTE>(0x2e * a / 255);
        p[1] = static_cast<BYTE>(0x38 * a / 255);
        p[2] = static_cast<BYTE>(0xb3 * a / 255);
        p[3] = a;
    }
}

void BuildMask(Geo& g, int w, int h) {
    g.maskW = w;
    g.maskH = h;
    g.mask.assign(static_cast<size_t>(IMax(0, w)) * IMax(0, h), 0);
    const double insetX = 0.12 * w, insetY = 0.22 * h;
    const double lw = w + insetX * 2.0, lh = h + insetY * 2.0;
    const double cx = -insetX + 0.62 * lw, cy = -insetY + 0.40 * lh;
    const double rx = 1.20 * lw, ry = lh;
    if (rx <= 0.0 || ry <= 0.0) return;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const double ux = (x - cx) / rx, uy = (y - cy) / ry;
            const double d = std::sqrt(ux * ux + uy * uy);
            const double a = d <= 0.34 ? 1.0 : d >= 0.78 ? 0.0 : 1.0 - (d - 0.34) / 0.44;
            g.mask[static_cast<size_t>(y) * w + x] = Chan(a * 255.0);
        }
    }
}

void RenderMeteors(HDC dc, const RECT& r, double now) {
    HPEN pen = CreatePen(PS_SOLID, 1, RGB(0x79, 0x5b, 0x2e));
    if (pen == nullptr) return;
    SelectScope ps(dc, pen);
    for (int i = 0; i < 7; ++i) {
        const int span = std::max(1, static_cast<int>(r.right - r.left) - 36);
        const int x = static_cast<int>(r.left) + 18 +
                      ((i * 53 + static_cast<int>(now / 20.0)) % span);
        MoveToEx(dc, x, r.top + 4 + (i * 7) % 28, nullptr);
        LineTo(dc, x - 10, r.top + 15 + (i * 7) % 28);
    }
}

void RenderBurst(HDC dc, const RECT& r, double amount) {
    if (amount <= 0.01) return;
    const int cx = r.left + (r.right - r.left) / 3;
    const int cy = r.top + (r.bottom - r.top) / 2;
    HPEN pen = CreatePen(PS_SOLID, 1, RGB(0xf0, 0xc0, 0x7a));
    if (pen == nullptr) return;
    SelectScope ps(dc, pen);
    for (int i = 0; i < 6; ++i) {
        const int dx = (i % 2 == 0 ? 1 : -1) * (10 + i * 2);
        const int dy = (i < 3 ? -1 : 1) * (8 + (i % 3) * 3);
        MoveToEx(dc, cx, cy, nullptr);
        LineTo(dc, cx + dx, cy + dy);
    }
}

void PaintGoat(HDC dc, BYTE*, SponsorState* st, int dpi, double now) {
    const Layout& L = st->layout;
    const RECT r = L.btn[2];
    theme::DrawRoundRect(dc, r, L.plateRadius, RGB(0x0b, 0x0b, 0x0d), RGB(0x56, 0x48, 0x34));
    // The original strip's decorative rays live behind the lockup, but the raster lines must
    // not occupy any pixel in either word box: at small DPIs the gaps between glyphs otherwise
    // read as a stray X through the title and tag. Keep the exclusion in the clip, so both the
    // resting meteors and the hover burst obey the same rule.
    const int saved = SaveDC(dc);
    IntersectClipRect(dc, r.left, r.top, r.right, r.bottom);
    ExcludeClipRect(dc, L.fallbackWordmark.left, L.fallbackWordmark.top,
                    L.fallbackWordmark.right, L.fallbackWordmark.bottom);
    ExcludeClipRect(dc, L.fallbackTag.left, L.fallbackTag.top,
                    L.fallbackTag.right, L.fallbackTag.bottom);
    RenderMeteors(dc, r, now);
    RenderBurst(dc, r, st->goat.lift.At(now));
    RestoreDC(dc, saved);

    // The original vector lockup: five cream head contours, the eye, and the seal/glyph
    // cross-fade. Its geometry is cached in Geo and is independent of the network model.
    Geo& g = st->geo;
    if (g.bloom.Dc() != nullptr) {
        BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
        AlphaBlend(dc, g.bloomOrg.x, g.bloomOrg.y, g.bloom.W(), g.bloom.H(),
                   g.bloom.Dc(), 0, 0, g.bloom.W(), g.bloom.H(), blend);
    }
    for (int i = 0; i < 5; ++i) FillDevPath(dc, g.head[i], RGB(0xec, 0xe7, 0xdb));
    FillDevPath(dc, g.eye, RGB(0xec, 0xe7, 0xdb));
    theme::FillRoundRect(dc, g.sealRc, g.sealRadius,
                         st->goat.lift.At(now) > 0.5 ? RGB(0xc8, 0x41, 0x2f)
                                                     : RGB(0xb3, 0x38, 0x2c));
    const bool showThumb = st->goat.lift.At(now) > 0.5;
    FillDevPath(dc, showThumb ? g.thumb : g.chr, RGB(0xf6, 0xef, 0xe6));

    DrawWithFont(dc, st->layout.fallbackNamePt.x, st->layout.fallbackNamePt.y,
                 kDaGoatName, st->fonts.oldName, RGB(0xf2, 0xf3, 0xf5),
                 st->layout.fallbackNameTrack);
    DrawWithFont(dc, st->layout.fallbackTagPt.x, st->layout.fallbackTagPt.y,
                 kDaGoatTag, st->fonts.oldTag, RGB(0x9b, 0x9d, 0xa4),
                 st->layout.fallbackTagTrack);
    if (st->focus == 2) DrawFocusRing(dc, r, dpi, L.plateRadius);

    // The original DaGoat copy is part of the built-in banner, not a network field.
    DrawWithFont(dc, L.copyX, L.copyBase[0], kDaGoatCopy1,
                 st->fonts.copy, Cr(kInkCream), 0);
    DrawWithFont(dc, L.copyX, L.copyBase[1], kDaGoatCopy2,
                 st->fonts.copy, Cr(kInkCream), 0);
}

// Renders the whole strip into its back buffer at the current time, then copies it to `target`.
void RenderStrip(HWND hwnd, SponsorState* st, HDC target) {
    RECT client;
    GetClientRect(hwnd, &client);
    const int w = client.right - client.left, h = client.bottom - client.top;
    if (w <= 0 || h <= 0) return;

    HDC dc = target;
    BYTE* bits = nullptr;
    if (st->back.Ensure(w, h)) {
        dc = st->back.Dc();
        bits = st->back.Bits();
    }

    theme::FillBackground(dc, client);
    const ob::Model model = BannerHostCurrent();
    EnsureLayout(hwnd, st, dc, &model);
    if (!st->geo.valid) BuildGeo(st);
    const int dpi = st->dpi;
    PaintKofi(dc, st, dpi);
    PaintGitHub(dc, st, dpi);
    if (model.online) PaintOnlineGoat(dc, bits, st, dpi, NowMs(), model);
    else PaintGoat(dc, bits, st, dpi, NowMs());

    if (dc != target) BitBlt(target, 0, 0, w, h, dc, 0, 0, SRCCOPY);
}

void OnPaint(HWND hwnd) {
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hwnd, &ps);
    if (hdc == nullptr) return;
    SponsorState* st = Get(hwnd);
    if (st != nullptr) {
        RenderStrip(hwnd, st, hdc);
    } else {
        RECT client;
        GetClientRect(hwnd, &client);
        theme::FillBackground(hdc, client);
    }
    EndPaint(hwnd, &ps);
    // A paint while something is still moving restarts the clock - e.g. the strip was hidden
    // mid-transition and has just been shown again.
    if (st != nullptr) SyncTimer(hwnd, st);
}

// ===========================================================================
// Activation
// ===========================================================================
void Activate(HWND hwnd, SponsorState* st, int i) {
    if (st == nullptr || i < 0 || i > 2) return;

    // The parent gets first refusal, exactly as sponsor.h describes: it does not have to
    // handle this, but returning non-zero suppresses the navigation.
    HWND parent = GetParent(hwnd);
    if (parent != nullptr) {
        const int id = (i == 0) ? kSponsorKofi : (i == 1) ? kSponsorGitHub : kSponsorGoat;
        const LRESULT r = SendMessageW(parent, WM_COMMAND,
                                       MAKEWPARAM(static_cast<WORD>(id), SPN_CLICKED),
                                       reinterpret_cast<LPARAM>(hwnd));
        if (r != 0) return;
    }

    if (i == 2) {
        const ob::Model current = BannerHostCurrent();
        if (!current.online) {
            ShellExecuteW(hwnd, L"open", sponsor_url::kDaGoat, nullptr, nullptr, SW_SHOWNORMAL);
            return;
        }
        std::string url;
        if (!BannerHostClick(&url) || url.empty()) return;
        const std::wstring wide = Widen(url);
        if (SponsorLinkAllowed(wide, current)) {
            ShellExecuteW(hwnd, L"open", wide.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        }
        return;
    }

    // An EMPTY url opens NOTHING. Opening a wrong URL is worse than opening none.
    if (st->url[i].empty()) return;
    ShellExecuteW(hwnd, L"open", st->url[i].c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

// ===========================================================================
// Tooltips - the URL, or "link not configured" when the destination is empty.
// ===========================================================================
void CreateTips(HWND hwnd, SponsorState* st) {
    if (st->tip != nullptr) return;
    st->tip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr,
                              WS_POPUP | TTS_NOPREFIX | TTS_ALWAYSTIP,
                              CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
                              hwnd, nullptr,
                              reinterpret_cast<HINSTANCE>(
                                  GetWindowLongPtrW(hwnd, GWLP_HINSTANCE)),
                              nullptr);
    if (st->tip == nullptr) return;
    for (int i = 0; i < 3; ++i) {
        TTTOOLINFOW ti;
        ZeroMemory(&ti, sizeof(ti));
        ti.cbSize   = sizeof(ti);
        ti.uFlags   = TTF_SUBCLASS;
        ti.hwnd     = hwnd;
        ti.uId      = static_cast<UINT_PTR>(i);
        ti.lpszText = LPSTR_TEXTCALLBACKW;
        ti.rect     = st->layout.btn[i];
        SendMessageW(st->tip, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&ti));
    }
}

void UpdateTipRects(HWND hwnd, SponsorState* st) {
    if (st->tip == nullptr) return;
    for (int i = 0; i < 3; ++i) {
        TTTOOLINFOW ti;
        ZeroMemory(&ti, sizeof(ti));
        ti.cbSize = sizeof(ti);
        ti.hwnd   = hwnd;
        ti.uId    = static_cast<UINT_PTR>(i);
        ti.rect   = st->layout.btn[i];
        SendMessageW(st->tip, TTM_NEWTOOLRECTW, 0, reinterpret_cast<LPARAM>(&ti));
    }
}

// ===========================================================================
// Window procedure
// ===========================================================================
void MoveFocus(HWND hwnd, SponsorState* st, bool back) {
    st->kbdFocus = true;
    const int next = (st->focus < 0) ? (back ? 2 : 0) : st->focus + (back ? -1 : 1);
    if (next >= 0 && next <= 2) {
        st->focus = next;
        InvalidateRect(hwnd, nullptr, FALSE);
        RefreshActive(hwnd, st);
        return;
    }
    // Off the end of the strip: hand the focus back to the dialog manager.
    HWND parent = GetParent(hwnd);
    if (parent != nullptr) {
        HWND nxt = GetNextDlgTabItem(parent, hwnd, back ? TRUE : FALSE);
        if (nxt != nullptr && nxt != hwnd) { SetFocus(nxt); return; }
    }
    st->focus = back ? 0 : 2;
    InvalidateRect(hwnd, nullptr, FALSE);
    RefreshActive(hwnd, st);
}

LRESULT CALLBACK SponsorProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == kBannerHostChanged) {
        SponsorState* st = Get(hwnd);
        if (st != nullptr) {
            // The client has already applied its pointer/focus swap rule. Re-read the immutable
            // snapshot here and repaint; never manufacture a second local model or swap.
            (void)BannerHostCurrent();
            st->layoutValid = false;
            st->geo.valid = false;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    }
    switch (msg) {
    case WM_NCCREATE: {
        SponsorState* st = new (std::nothrow) SponsorState();
        if (st != nullptr) {
            st->url[0] = sponsor_url::kKofi;
            st->url[1] = sponsor_url::kGitHub;
            st->url[2] = sponsor_url::kDaGoat;
        }
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(st));
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
    case WM_CREATE: {
        SponsorState* st = Get(hwnd);
        if (st != nullptr) {
            st->bannerToken = BannerHostSubscribe(hwnd, kBannerHostChanged);
            HDC dc = GetDC(hwnd);
            if (dc != nullptr) { EnsureLayout(hwnd, st, dc); ReleaseDC(hwnd, dc); }
            CreateTips(hwnd, st);
        }
        return 0;
    }
    case WM_NCDESTROY: {
        SponsorState* st = Get(hwnd);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        if (st != nullptr) {
            BannerHostVisible(st->bannerToken, false);
            BannerHostUnsubscribe(st->bannerToken);
            DropMagnifier(st);
            if (st->timerOn) KillTimer(hwnd, kTimerId);
            if (st->tip != nullptr) DestroyWindow(st->tip);
            delete st;   // the back buffer and the fonts free themselves
        }
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
    case WM_ERASEBKGND:
        return 1;   // WM_PAINT covers every pixel

    case WM_PAINT:
        OnPaint(hwnd);
        return 0;

    case WM_PRINTCLIENT: {
        // The same render into a caller's DC: PrintWindow, capture tools, and the offscreen
        // harness that measures this control without ever showing a window.
        SponsorState* st = Get(hwnd);
        if (st != nullptr && wParam != 0) RenderStrip(hwnd, st, reinterpret_cast<HDC>(wParam));
        return 0;
    }

    case WM_MOVE: {
        SponsorState* st = Get(hwnd);
        if (st != nullptr) DropMagnifier(st);   // the tile it points at is not there any more
        return 0;
    }
    case WM_SIZE: {
        SponsorState* st = Get(hwnd);
        if (st != nullptr) {
            DropMagnifier(st);
            st->layoutValid = false;
            st->geo.valid = false;
            HDC dc = GetDC(hwnd);
            if (dc != nullptr) { EnsureLayout(hwnd, st, dc); ReleaseDC(hwnd, dc); }
            UpdateTipRects(hwnd, st);
            SyncTimer(hwnd, st);
        }
        return 0;
    }
    case WM_DPICHANGED_AFTERPARENT: {
        SponsorState* st = Get(hwnd);
        if (st != nullptr) { DropMagnifier(st); st->layoutValid = false; st->geo.valid = false; }
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }
    case WM_SETTINGCHANGE: {
        // The animation preference is re-read here rather than cached forever.
        ForgetAnimationPreference();
        SponsorState* st = Get(hwnd);
        if (st != nullptr) { InvalidateRect(hwnd, nullptr, FALSE); SyncTimer(hwnd, st); }
        return 0;
    }
    case WM_SHOWWINDOW: {
        SponsorState* st = Get(hwnd);
        if (st != nullptr) {
            if (wParam == FALSE) DropMagnifier(st);
            BannerHostVisible(st->bannerToken, wParam != FALSE);
            SyncTimer(hwnd, st);
        }
        break;
    }
    case WM_TIMER: {
        if (wParam != kTimerId) break;
        SponsorState* st = Get(hwnd);
        if (st == nullptr) return 0;
        // Nothing is stepped: every value is a function of the clock. This frame paints even
        // when it is the last one, and the timer dies the moment nothing is moving.
        InvalidateRect(hwnd, nullptr, FALSE);
        SyncTimer(hwnd, st);
        return 0;
    }
    case WM_SETCURSOR: {
        // The hand only over something that opens a link - never over the copy lines or the
        // gaps between the buttons.
        SponsorState* st = Get(hwnd);
        if (st != nullptr && LOWORD(lParam) == HTCLIENT) {
            const DWORD pos = GetMessagePos();
            POINT p;
            p.x = static_cast<short>(LOWORD(pos));
            p.y = static_cast<short>(HIWORD(pos));
            ScreenToClient(hwnd, &p);
            SetCursor(LoadCursorW(nullptr, HitTest(st, p.x, p.y) >= 0 ? IDC_HAND : IDC_ARROW));
            return TRUE;
        }
        break;
    }
    case WM_MOUSEMOVE: {
        SponsorState* st = Get(hwnd);
        if (st == nullptr) break;
        if (!st->tracking) {
            TRACKMOUSEEVENT tme;
            ZeroMemory(&tme, sizeof(tme));
            tme.cbSize    = sizeof(tme);
            tme.dwFlags   = TME_LEAVE;
            tme.hwndTrack = hwnd;
            if (TrackMouseEvent(&tme)) st->tracking = true;
        }
        const int x = static_cast<int>(static_cast<short>(LOWORD(lParam)));
        const int y = static_cast<int>(static_cast<short>(HIWORD(lParam)));
        const int hit = HitTest(st, x, y);
        POINT pt;
        pt.x = x;
        pt.y = y;
        st->qrHot = BannerHostCurrent().online && (hit == 2) &&
                    PtInRect(&st->layout.tile, pt) != FALSE;
        BannerHostPointer(hit == 2);
        if (hit != st->hot) {
            st->hot = hit;
            InvalidateRect(hwnd, nullptr, FALSE);
            RefreshActive(hwnd, st);
        } else {
            SyncMagnifier(hwnd, st);   // on and off the QR tile without leaving the plate
        }
        return 0;
    }
    case WM_MOUSELEAVE: {
        SponsorState* st = Get(hwnd);
        if (st == nullptr) break;
        st->tracking = false;
        st->qrHot = false;
        BannerHostPointer(false);
        if (st->hot != -1) {
            st->hot = -1;
            st->pressed = -1;
            InvalidateRect(hwnd, nullptr, FALSE);
            RefreshActive(hwnd, st);
        } else {
            SyncMagnifier(hwnd, st);
        }
        return 0;
    }
    case WM_LBUTTONDOWN: {
        SponsorState* st = Get(hwnd);
        if (st == nullptr) break;
        const int x = static_cast<int>(static_cast<short>(LOWORD(lParam)));
        const int y = static_cast<int>(static_cast<short>(HIWORD(lParam)));
        const int hit = HitTest(st, x, y);
        if (hit >= 0) {
            SetFocus(hwnd);
            st->kbdFocus = false;   // a click, not the keyboard: no magnifier for focus alone
            st->focus = hit;
            st->pressed = hit;
            SetCapture(hwnd);
            InvalidateRect(hwnd, nullptr, FALSE);
            RefreshActive(hwnd, st);
        }
        return 0;
    }
    case WM_LBUTTONUP: {
        SponsorState* st = Get(hwnd);
        if (st == nullptr) break;
        const int was = st->pressed;
        st->pressed = -1;
        if (GetCapture() == hwnd) ReleaseCapture();
        const int x = static_cast<int>(static_cast<short>(LOWORD(lParam)));
        const int y = static_cast<int>(static_cast<short>(HIWORD(lParam)));
        InvalidateRect(hwnd, nullptr, FALSE);
        if (was >= 0 && HitTest(st, x, y) == was) Activate(hwnd, st, was);
        return 0;
    }
    case WM_CAPTURECHANGED: {
        SponsorState* st = Get(hwnd);
        if (st != nullptr && st->pressed != -1) {
            st->pressed = -1;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    }
    case WM_SETFOCUS: {
        SponsorState* st = Get(hwnd);
        if (st != nullptr) {
            // Tab from the dialog manager or a programmatic SetFocus. A click resets this
            // straight after, in WM_LBUTTONDOWN.
            st->kbdFocus = true;
            if (st->focus < 0) st->focus = 0;
            BannerHostFocus(st->focus == 2);
            InvalidateRect(hwnd, nullptr, FALSE);
            RefreshActive(hwnd, st);
        }
        return 0;
    }
    case WM_KILLFOCUS: {
        SponsorState* st = Get(hwnd);
        if (st != nullptr) {
            st->kbdFocus = false;
            st->focus = -1;
            BannerHostFocus(false);
            InvalidateRect(hwnd, nullptr, FALSE);
            RefreshActive(hwnd, st);
        }
        return 0;
    }
    case WM_GETDLGCODE: {
        const LRESULT base = DLGC_WANTARROWS | DLGC_WANTTAB;
        const MSG* m = reinterpret_cast<const MSG*>(lParam);
        // Esc must still reach the dialog manager, so it is the one key not claimed.
        if (m != nullptr && (m->message == WM_KEYDOWN || m->message == WM_CHAR) &&
            m->wParam == VK_ESCAPE) {
            return base;
        }
        return base | DLGC_WANTALLKEYS;
    }
    case WM_KEYDOWN: {
        SponsorState* st = Get(hwnd);
        if (st == nullptr) break;
        switch (wParam) {
        case VK_TAB:
            MoveFocus(hwnd, st, GetKeyState(VK_SHIFT) < 0);
            return 0;
        case VK_RIGHT:
        case VK_DOWN:
            MoveFocus(hwnd, st, false);
            return 0;
        case VK_LEFT:
        case VK_UP:
            MoveFocus(hwnd, st, true);
            return 0;
        case VK_SPACE:
        case VK_RETURN:
            if (st->focus >= 0) {
                st->pressed = st->focus;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        default:
            break;
        }
        break;
    }
    case WM_KEYUP: {
        SponsorState* st = Get(hwnd);
        if (st == nullptr) break;
        if (wParam == VK_SPACE || wParam == VK_RETURN) {
            const int was = st->pressed;
            st->pressed = -1;
            InvalidateRect(hwnd, nullptr, FALSE);
            if (was >= 0 && was == st->focus) Activate(hwnd, st, was);
            return 0;
        }
        break;
    }
    case WM_NOTIFY: {
        NMHDR* nm = reinterpret_cast<NMHDR*>(lParam);
        SponsorState* st = Get(hwnd);
        if (st != nullptr && nm != nullptr &&
            (nm->code == TTN_GETDISPINFOW || nm->code == TTN_NEEDTEXTW)) {
            NMTTDISPINFOW* di = reinterpret_cast<NMTTDISPINFOW*>(lParam);
            const int i = static_cast<int>(di->hdr.idFrom);
            if (i >= 0 && i <= 2) {
                const std::wstring& u = st->url[i];
                // An unconfigured destination says so rather than pretending to work.
                const wchar_t* text = u.empty() ? kNoLink : u.c_str();
                di->lpszText = const_cast<wchar_t*>(text);
                di->hinst = nullptr;
                return 0;
            }
        }
        break;
    }
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

}  // namespace

// ===========================================================================
// Public API
// ===========================================================================
const wchar_t* kSponsorClass = L"GameOptimizerSponsor";

void SponsorRegister(HINSTANCE hInst) {
    g_inst = hInst;   // where the RCDATA photo lives

    INITCOMMONCONTROLSEX icc;
    icc.dwSize = sizeof(icc);
    icc.dwICC  = ICC_BAR_CLASSES;
    InitCommonControlsEx(&icc);

    WNDCLASSEXW existing;
    ZeroMemory(&existing, sizeof(existing));
    existing.cbSize = sizeof(existing);
    if (GetClassInfoExW(hInst, kSponsorClass, &existing)) return;

    WNDCLASSEXW wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = SponsorProc;
    wc.hInstance     = hInst;
    wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);   // WM_SETCURSOR shows the hand on links
    wc.hbrBackground = nullptr;   // WM_PAINT owns every pixel
    wc.lpszClassName = kSponsorClass;
    RegisterClassExW(&wc);
}

SIZE SponsorMeasure(int dpi) {
    SIZE out;
    out.cx = 0;
    out.cy = 0;
    HDC dc = GetDC(nullptr);
    if (dc == nullptr) return out;

    RECT client;
    client.left = 0;
    client.top = 0;
    client.right = 4000;
    client.bottom = 4000;

    FontSet F;          // local: its destructor frees the faces on the way out
    F.Build(dpi);
    Layout L;
    ComputeLayout(dc, dpi, client, F, L, nullptr);
    ReleaseDC(nullptr, dc);

    out.cx = L.goatRight - L.btn[0].left;
    out.cy = L.stripH;
    return out;
}

#if defined(CD_TESTS) || defined(CD_SPONSOR_HARNESS)
void SponsorSetDpiForTest(int dpi) {
    g_dpiOverride = dpi > 0 ? dpi : 0;
}

void SponsorSetHoverForTest(HWND hwnd, bool hover) {
    SponsorState* st = Get(hwnd);
    if (st == nullptr) return;
    st->hot = hover ? 2 : -1;
    st->qrHot = hover;
    st->goat.active = hover;
    const double value = hover ? 1.0 : 0.0;
    st->goat.push.Snap(value);
    st->goat.bracket.Snap(value);
    st->goat.lift.Snap(value);
    st->goat.tag.Snap(value);
    st->goat.qr.Snap(value);
    st->goat.lock.Snap(value);
    InvalidateRect(hwnd, nullptr, FALSE);
}

bool SponsorGetLayoutForTest(HWND hwnd, SponsorHarnessLayout* out) {
    SponsorState* st = Get(hwnd);
    if (st == nullptr || out == nullptr) return false;
    if (!st->layoutValid) {
        HDC dc = GetDC(hwnd);
        if (dc == nullptr) return false;
        const ob::Model model = BannerHostCurrent();
        EnsureLayout(hwnd, st, dc, &model);
        ReleaseDC(hwnd, dc);
    }
    const Layout& L = st->layout;
    for (int i = 0; i < 3; ++i) out->buttons[i] = L.btn[i];
    out->plate = L.btn[2];
    out->viewfinder = L.vf;
    out->tile = L.tile;
    out->wordmark = L.wordmark;
    out->tagline[0] = L.tagline[0];
    out->tagline[1] = L.tagline[1];
    out->copy = L.copy;
    out->tagBaseline[0] = L.tagBaseline[0];
    out->tagBaseline[1] = L.tagBaseline[1];
    out->tagEmBaseline[0] = L.tagEmBaseline[0];
    out->tagEmBaseline[1] = L.tagEmBaseline[1];
    out->taglineHasEm[0] = L.taglineHasEm[0];
    out->taglineHasEm[1] = L.taglineHasEm[1];
    out->badge = L.badge;
    out->stage = ScaleRectAboutCenter(L.stage, st->goat.active ? 1.0 + sponsor_motion::kStageScale : 1.0);
    RECT clippedStage{};
    if (IntersectRect(&clippedStage, &out->stage, &L.btn[2])) out->stage = clippedStage;
    const int bracketInset = L.vfIn + (st->goat.active ? L.vfLock : 0);
    out->brackets = { out->stage.left + bracketInset, out->stage.top + bracketInset,
                      out->stage.right - bracketInset, out->stage.bottom - bracketInset };
    out->text = L.text;
    out->iphone = L.iphone;
    out->fallbackWordmark = L.fallbackWordmark;
    out->fallbackTag = L.fallbackTag;
    out->qrOrigin = L.qrOrg;
    out->module = L.module;
    return true;
}
#endif

}  // namespace cd
