// Game Optimizer - the animated sponsor strip. Implementation of src\sponsor.h.
//
// WHAT THIS FILE IS A PORT OF
// ---------------------------
// Nothing here is drawn by eye. Three of the operator's own files are the source:
//
//   F:\google map plugin\extension\content\brand\goat-lockup-hover.css
//       every duration, delay, easing curve, opacity stop and colour below.
//   F:\google map plugin\extension\content\widget.js   (line 625, GOAT_LOCKUP_MARKUP)
//       every path string in kArt below is a byte-for-byte copy of the `d` attribute
//       from that markup, and every circle / rect / line is its numeric equivalent.
//   F:\google map plugin\extension\options\options.css + options.html
//       the Ko-fi button: inline-flex, 8px gap, 9px/16px padding, radius 6, two-line
//       label, hover background swap, :active translateY(1px) - and its cup icon path.
//
// HOW THE VECTOR ART GETS INTO GDI
// --------------------------------
// GDI has no path parser, so ParsePath() below reads the small SVG subset those files
// actually use - M m L l H h V v C c S s Q q T t A a Z z - and flattens every curve to
// polylines ONCE, in source viewBox units, into a function-local static. Per DPI those
// source polylines are mapped to POINT arrays and cached in Geo; the paint path only
// ever calls PolyPolygon. Nothing re-parses and nothing re-flattens per frame.
//
// ALPHA
// -----
// Fades, the seal bloom and the meteor gradients need real per-pixel alpha, which plain
// GDI blitting cannot express. Layer below wraps a 32bpp top-down DIB section with a
// direct bits pointer; AlphaBlend (msimg32 - a SYSTEM library, already on the link line
// in tools\build.bat, and pinned here with #pragma comment for good measure) composites
// it. Everything written into a Layer is PREMULTIPLIED, as AlphaBlend requires.
//
// THE CPU RULE, WHICH OUTRANKS ALL OF THE ABOVE
// ---------------------------------------------
// AnimBusy() is the single gate on the 16 ms timer. It is true only while a transition
// is still settling, or the meteor layer is both visible AND actually moving, or the
// spark is inside its firing window. When it goes false the timer is KILLED - see
// SyncTimer(). In reduced-motion mode the meteors and the ring are static, so they do
// not hold the timer open at all and the strip settles to zero cost.
#define NOMINMAX
#include "sponsor.h"

#include <commctrl.h>
#include <shellapi.h>
#include <wingdi.h>

#include <cmath>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>

#pragma comment(lib, "msimg32.lib")

#include "theme.h"

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

// --gl-in: cubic-bezier(0.16, 1, 0.3, 1)
double EaseIn(double t) { return CubicBezier(t, 0.16, 1.0, 0.3, 1.0); }

// --gl-out: cubic-bezier(0.4, 0, 0.2, 1)
double EaseOut(double t) { return CubicBezier(t, 0.4, 0.0, 0.2, 1.0); }

}  // namespace sponsor_motion

// ===========================================================================
namespace {

using sponsor_motion::EaseIn;
using sponsor_motion::EaseOut;

// ---- small numeric helpers (windows.h min/max are suppressed by NOMINMAX) --
inline int    IMax(int a, int b)          { return a > b ? a : b; }
inline double DMax(double a, double b)    { return a > b ? a : b; }
inline double Clamp01(double v)           { return v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v); }
inline int    RoundI(double v)            { return static_cast<int>(v < 0.0 ? v - 0.5 : v + 0.5); }

// ---------------------------------------------------------------------------
// The palette, straight out of .goat-lockup (the dark variant - this app is dark).
// ---------------------------------------------------------------------------
constexpr COLORREF kCream    = RGB(0xec, 0xe7, 0xdb);   // --gl-cream
constexpr COLORREF kGlyph    = RGB(0xf6, 0xef, 0xe6);   // --gl-glyph
constexpr COLORREF kSeal     = RGB(0xb3, 0x38, 0x2c);   // --gl-seal
constexpr COLORREF kSealLit  = RGB(0xc8, 0x41, 0x2f);   // --gl-seal-lit
constexpr COLORREF kName     = RGB(0xf2, 0xf3, 0xf5);   // --gl-name
constexpr COLORREF kTag      = RGB(0x9b, 0x9d, 0xa4);   // --gl-tag
constexpr COLORREF kSpark    = RGB(0xf7, 0xea, 0xd6);   // --gl-spark

// --gl-meteor-head rgba(236,231,219,0.68), --gl-meteor-halo rgba(236,231,219,0.55)
constexpr double kMeteorHeadA = 0.68;
constexpr double kMeteorHaloA = 0.55;

// options.css :root - --kofi / --kofi-hover / --kofi-text
constexpr COLORREF kKofiBg      = RGB(0xd2, 0x41, 0x3e);
constexpr COLORREF kKofiBgHover = RGB(0xbb, 0x32, 0x2f);
constexpr COLORREF kKofiText    = RGB(0xff, 0xff, 0xff);

// .gl-seal-glow at full strength
constexpr double kGlowPeak = 0.7;

// --gl-seal-scale. THE STYLESHEET'S DEFAULT IS 1 AND THAT IS WHAT THIS USES.
//
// [M] Measured off the reference render of the plugin's own CSS+SVG
//     (scratchpad\plugin-ref.png, System.Drawing pixel scan, 2026-08-29):
//       mark element 5.4em at em=20px  -> cream art bbox 74 x 88 px
//       red seal bbox                  -> 15 x 15 px
//     36 seal units of a 250-unit viewBox at 108px element height is 15.55px, so the
//     reference is drawn at scale EXACTLY 1. The previous 1.9 here was a judgement call,
//     not a measurement, and it is what made the stamp read as a component rather than a
//     detail: the whole point of the lockup is a tall thin cream mark with a small stamp
//     tucked at its lower right.
//
// The stylesheet's own comment says the knob exists only to RAISE this on very small
// lockups. If the thumbs-up stops reading, the answer is a LARGER MARK - kMarkH below -
// never a bigger seal, because a bigger seal changes the composition and a bigger mark
// does not.
constexpr double kSealScale = 1.0;

// ---------------------------------------------------------------------------
// THE SIZE KNOB.
//
// Operator instruction: the whole strip at 70% of the size it had. The stylesheet is
// built around a single font-size from which the mark, the gaps and the wordmark all
// derive, so ONE value moves and everything follows - no piece is shrunk on its own.
//
// kMarkH is that knob expressed the way this file already used it: the mark's height in
// logical px, which is 5.4em, so em = kMarkH / 5.4. It was 58 (em = 10.74px); 70% of that
// is 41 (em = 7.59px). Every other box metric below is its own source value times 0.70.
// ---------------------------------------------------------------------------
constexpr double kStripScale = 0.70;

// .goat-lockup__mark { height: 5.4em }
constexpr int kMarkH = 41;   // logical px, = round(58 * 0.70)

// The CSS type floors: --gl-name-min 20px, --gl-tag-min 10px. These are NOT scaled -
// a floor that scales is not a floor. At this size both of them bind, which is exactly
// what the stylesheet intends and what the reference render shows (its 20px and 14px
// lockups have IDENTICAL wordmarks; only the mark changes size).
constexpr int kNameMinPx = 20;   // font-size: max(20px, 1em)
constexpr int kTagMinPx  = 10;   // font-size: max(10px, .43em)

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

inline BYTE Chan(double v) {
    const int i = RoundI(v);
    return static_cast<BYTE>(i < 0 ? 0 : (i > 255 ? 255 : i));
}

COLORREF Lerp(COLORREF a, COLORREF b, double t) {
    t = Clamp01(t);
    return RGB(Chan(GetRValue(a) + (GetRValue(b) - GetRValue(a)) * t),
               Chan(GetGValue(a) + (GetGValue(b) - GetGValue(a)) * t),
               Chan(GetBValue(a) + (GetBValue(b) - GetBValue(a)) * t));
}

int DpiOfWnd(HWND hwnd) {
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

// One logical source value at 70%. Applied to BOX metrics only - paddings, gaps, icon
// boxes, radii, the label size - never to the two type floors, which are floors.
inline int Sc(int logical) { return IMax(1, RoundI(logical * kStripScale)); }

// ===========================================================================
// Fonts.
//
// The theme's fonts are fixed point sizes and this lockup needs PIXEL sizes derived
// from the em knob, with the stylesheet's own weights:
//   .goat-lockup__name  font-size max(20px, 1em)   weight 300   letter-spacing .24em
//   .goat-lockup__tag   font-size max(10px, .43em) weight 500   letter-spacing .28em
//   .kofi label         weight 600, both lines the SAME size
// so this file owns four small faces. They live in SponsorState (and in a local for
// SponsorMeasure), so every HFONT is deleted with the object that created it.
// ===========================================================================
HFONT MakeUiFont(int px, int weight) {
    // Same resolve-then-verify route theme.cpp uses: GDI silently substitutes a family
    // when the name is unknown, and the only reliable test is GetTextFaceW.
    const wchar_t* const faces[2] = { L"Segoe UI Variable Text", L"Segoe UI" };
    const int height = -IMax(1, px);
    HDC screen = GetDC(nullptr);
    HFONT chosen = nullptr;

    for (int i = 0; i < 2; ++i) {
        HFONT f = CreateFontW(height, 0, 0, 0, weight, FALSE, FALSE, FALSE,
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
            lf.lfQuality = CLEARTYPE_QUALITY;
            chosen = CreateFontIndirectW(&lf);
        }
        if (chosen == nullptr) {
            chosen = CreateFontW(height, 0, 0, 0, weight, FALSE, FALSE, FALSE,
                                 DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                 CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, nullptr);
        }
    }
    if (screen != nullptr) ReleaseDC(nullptr, screen);
    return chosen;
}

int FontTmHeight(HFONT f) {
    if (f == nullptr) return 0;
    HDC screen = GetDC(nullptr);
    if (screen == nullptr) return 0;
    TEXTMETRICW tm;
    ZeroMemory(&tm, sizeof(tm));
    HGDIOBJ old = SelectObject(screen, f);
    GetTextMetricsW(screen, &tm);
    SelectObject(screen, old);
    ReleaseDC(nullptr, screen);
    return static_cast<int>(tm.tmHeight);
}

struct FontSet {
    HFONT name     = nullptr;   // weight 300
    HFONT tag      = nullptr;   // weight 500
    HFONT label    = nullptr;   // weight 600 - the Ko-fi title AND handle
    HFONT labelReg = nullptr;   // weight 400 - the GitHub label

    int dpi     = 0;
    int namePx  = 0, tagPx  = 0, labelPx  = 0;   // em box, i.e. CSS font-size in device px
    int nameH   = 0, tagH   = 0, labelH   = 0;   // tmHeight, i.e. the GDI text cell

    FontSet() {}
    ~FontSet() { Destroy(); }

    void Destroy() {
        if (name     != nullptr) DeleteObject(name);
        if (tag      != nullptr) DeleteObject(tag);
        if (label    != nullptr) DeleteObject(label);
        if (labelReg != nullptr) DeleteObject(labelReg);
        name = tag = label = labelReg = nullptr;
        dpi = 0;
    }

    void Build(int d) {
        // The em knob, in device px, is the ONLY input: everything else derives.
        const double em = theme::Dp(kMarkH, d) / 5.4;
        const int np = IMax(theme::Dp(kNameMinPx, d), RoundI(em));
        const int tp = IMax(theme::Dp(kTagMinPx, d),  RoundI(0.43 * em));
        const int lp = IMax(1, theme::Dp(Sc(kKofiFontPx), d));
        if (dpi == d && name != nullptr && np == namePx && tp == tagPx && lp == labelPx) return;

        Destroy();
        namePx = np; tagPx = tp; labelPx = lp;
        name     = MakeUiFont(namePx, FW_LIGHT);      // 300
        tag      = MakeUiFont(tagPx,  FW_MEDIUM);     // 500
        label    = MakeUiFont(labelPx, FW_SEMIBOLD);  // 600
        labelReg = MakeUiFont(labelPx, FW_NORMAL);
        nameH  = FontTmHeight(name);
        tagH   = FontTmHeight(tag);
        labelH = FontTmHeight(label);
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
// subset the three source files actually contain, and it flattens every curve to a
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

// A circle as a flattened contour, so circles and paths take the same draw route.
Contours CircleContour(double cx, double cy, double r) {
    Contours out;
    SubPath sp;
    const int n = 48;
    for (int i = 0; i <= n; ++i) {
        const double a = 6.28318530717958647692 * i / n;
        PtF p;
        p.x = cx + r * std::cos(a);
        p.y = cy + r * std::sin(a);
        sp.push_back(p);
    }
    out.push_back(sp);
    return out;
}

// ===========================================================================
// THE ARTWORK.
//
// Every string below is copied verbatim from the `d` attribute of the corresponding
// element in GOAT_LOCKUP_MARKUP (widget.js line 625) or, for the cup, from the Ko-fi
// button in options.html. The numbers are the operator's, not mine.
// ===========================================================================
namespace kArt {

// .gl-head - five paths, outer viewBox "46 -16 220 250", plus one eye circle.
const char* const kHead[5] = {
    "M62 210 C74 148 120 98 190 80 C152 106 106 150 82 216 Z",
    "M186 84 C214 62 226 32 210 8 C216 36 200 62 174 80 Z",
    "M170 82 C190 60 194 34 180 16 C186 40 172 62 156 78 Z",
    "M188 82 C206 90 218 104 222 122 C212 106 200 94 182 90 Z",
    "M206 122 C215 134 217 149 209 160 C212 147 208 133 200 127 Z",
};
constexpr double kEyeCx = 197.0, kEyeCy = 99.0, kEyeR = 3.6;

// .gl-seal-glow / .gl-seal-face - x=214 y=182 w=36 h=36 rx=4, outer units.
constexpr double kSealX = 214.0, kSealY = 182.0, kSealW = 36.0, kSealH = 36.0, kSealRx = 4.0;

// .gl-glyph - an inner <svg> at (214,182) 36x36 with viewBox "0 0 100 100".
constexpr double kInnerBox = 100.0;

// .gl-thumb, with its own transform="translate(-5 -3)".
const char* const kThumb =
    "M30 50 C26 50 24 52 24 56 L24 87 C24 91 26 93 30 93 L73 93 C79 93 84 90 85 84 "
    "L89 55 C90 49 86 44 80 44 L63 44 C65 38 67 25 65 18 C63 12 55 12 53 18 "
    "C51 24 53 33 51 42 C50 47 47 50 42 50 Z";
constexpr double kThumbDx = -5.0, kThumbDy = -3.0;

// .gl-char - the SimSun 羊 outline baked to a path.
const char* const kChar =
    "M33.5 18.12Q39.14 21.51 41.54 23.62Q43.93 25.74 43.65 27.85Q43.37 29.97 42.24 30.96"
    "Q41.11 31.94 40.55 31.94Q39.42 31.94 38.57 29.12Q37.45 25.17 32.93 18.97Z"
    "M20.52 33.07H52.68Q58.89 23.76 60.86 16.99L66.79 20.66Q63.96 22.07 61.14 25.74"
    "Q58.04 29.41 54.37 33.07H68.2L72.43 28.84L77.79 34.77H51.55V47.46H63.96L68.2 43.79"
    "L72.99 49.15H51.55V62.13H71.58L76.1 57.9L81.46 63.82H51.55V73.7Q51.55 76.8 51.83 81.32"
    "L47.04 83.01Q47.32 78.78 47.32 63.82H27.85Q24.19 63.82 21.08 64.67L18.54 62.13H47.32"
    "V49.15H35.47Q31.8 49.15 28.7 50L26.16 47.46H47.32V34.77H29.83Q26.16 34.77 23.06 35.61Z";

// .gl-burst, transform="translate(53 0)", inside the 100x100 glyph viewBox.
constexpr double kBurstCx = 53.0, kBurstCy = 0.0;
constexpr double kRingR = 12.0, kRingStroke = 3.0;
constexpr double kRayR0 = 10.0, kRayR1 = 40.0, kRayStroke = 3.2;
constexpr double kRayDeg[6] = { -168.0, -138.0, -108.0, -76.0, -44.0, -14.0 };
constexpr double kRayDashOn = 9.0, kRayDashOff = 32.0;
constexpr double kEmberCx[3] = { -7.0, 8.0, 0.0 };
constexpr double kEmberCy[3] = { -2.0, -5.0, -9.0 };
constexpr double kEmberR [3] = { 2.2, 1.9, 1.6 };

// Ko-fi cup, options.html, viewBox "0 0 24 24".
const char* const kKofiCup =
    "M4 4h13a4 4 0 0 1 0 8h-1.1A6 6 0 0 1 10 17H8a6 6 0 0 1-6-6V4a0 0 0 0 1 0 0z"
    "m12 2v4h1a2 2 0 0 0 0-4h-1z";
const char* const kKofiSaucer = "M3 19h14a1 1 0 0 1 0 2H3a1 1 0 0 1 0-2z";

// Parsed exactly once, in source units, and shared by every DPI.
const Contours& Head(int i)  { static const Contours c[5] = { ParsePath(kHead[0]), ParsePath(kHead[1]),
                                                              ParsePath(kHead[2]), ParsePath(kHead[3]),
                                                              ParsePath(kHead[4]) }; return c[i]; }
const Contours& Eye()        { static const Contours c = CircleContour(kEyeCx, kEyeCy, kEyeR); return c; }
const Contours& Thumb()      { static const Contours c = ParsePath(kThumb);  return c; }
const Contours& Char()       { static const Contours c = ParsePath(kChar);   return c; }
const Contours& KofiCup()    { static const Contours c = ParsePath(kKofiCup); return c; }
const Contours& KofiSaucer() { static const Contours c = ParsePath(kKofiSaucer); return c; }

}  // namespace kArt

// ===========================================================================
// Layer - a 32bpp premultiplied DIB section, drawable by GDI and blendable by
// AlphaBlend. Every helper below keeps the premultiplication invariant.
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

    bool Valid() const { return dc_ != nullptr && bits_ != nullptr; }
    HDC  Dc()    const { return dc_; }
    int  W()     const { return w_; }
    int  H()     const { return h_; }
    BYTE* Bits() const { return bits_; }

    void Clear() { if (bits_ != nullptr) memset(bits_, 0, static_cast<size_t>(w_) * h_ * 4); }

    // GDI never writes the alpha byte, so after drawing into a cleared layer a pixel is
    // "covered" exactly when its BGR is non-zero. Every colour drawn into a layer here is
    // a light brand colour, so pure black cannot occur and cannot be mistaken for a gap.
    void SealCoverage() {
        if (bits_ == nullptr) return;
        const int n = w_ * h_;
        BYTE* p = bits_;
        for (int i = 0; i < n; ++i, p += 4) {
            p[3] = (p[0] != 0 || p[1] != 0 || p[2] != 0) ? 255 : 0;
        }
    }

    // Composites `col` at `a` into this layer at (x,y), premultiplied, source-over.
    void PlotPremul(int x, int y, COLORREF col, double a) {
        if (bits_ == nullptr || x < 0 || y < 0 || x >= w_ || y >= h_) return;
        if (a <= 0.0) return;
        if (a > 1.0) a = 1.0;
        BYTE* p = bits_ + (static_cast<size_t>(y) * w_ + x) * 4;
        const double sb = GetBValue(col) * a, sg = GetGValue(col) * a, sr = GetRValue(col) * a;
        const double inv = 1.0 - a;
        p[0] = Chan(sb + p[0] * inv);
        p[1] = Chan(sg + p[1] * inv);
        p[2] = Chan(sr + p[2] * inv);
        p[3] = Chan(a * 255.0 + p[3] * inv);
    }

    void Blend(HDC dst, int x, int y, double alpha) const {
        if (dc_ == nullptr || alpha <= 0.0) return;
        BLENDFUNCTION bf;
        bf.BlendOp             = AC_SRC_OVER;
        bf.BlendFlags          = 0;
        bf.SourceConstantAlpha = Chan(Clamp01(alpha) * 255.0);
        bf.AlphaFormat         = AC_SRC_ALPHA;
        AlphaBlend(dst, x, y, w_, h_, dc_, 0, 0, w_, h_, bf);
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
// Motion state - one per button, though only the GoatProject lockup uses most of it.
// Every field is a CSS transition or animation from goat-lockup-hover.css.
// ===========================================================================
struct Motion {
    // Linear transition progress, 0 = at rest, 1 = fully hovered. Each has its own
    // duration because the stylesheet gives each its own duration.
    double face   = 0.0;   // .gl-seal-face fill,  in 690ms --gl-out / out 360ms --gl-out
    double glow   = 0.0;   // .gl-seal-glow op,    in 690ms --gl-in  / out 360ms --gl-out
    double chr    = 0.0;   // .gl-char opacity,    in 450ms linear   / out 360ms --gl-out
    double thumb  = 0.0;   // .gl-thumb opacity,   in 450ms linear +180ms / out 360ms --gl-out
    double meteor = 0.0;   // meteor layer op,     in 630ms --gl-out / out 360ms --gl-out
    double tag    = 0.0;   // tagline colour,      in 630ms --gl-out / out 360ms --gl-out

    double hoverMs  = 0.0;   // ms since the button became active (drives the 180ms delay)
    double sparkMs  = -1.0;  // ms since the spark fired; < 0 = not firing
    double meteorMs = 0.0;   // free-running meteor clock

    bool active = false;

    // The whole spark: the last ember lag (975) plus its duration (1350).
    static double SparkWindowMs() {
        return static_cast<double>(sponsor_motion::kEmberLagMs[1] + sponsor_motion::kEmberMs);
    }

    static void Advance(double& p, bool on, double dt, double inMs, double outMs) {
        if (on) {
            p += dt / inMs;
            if (p > 1.0) p = 1.0;
        } else {
            p -= dt / outMs;
            if (p < 0.0) p = 0.0;
        }
    }

    void SetActive(bool on) {
        if (on == active) return;
        active = on;
        if (on) {
            hoverMs = 0.0;
            sparkMs = 0.0;
        } else {
            hoverMs = 0.0;
            sparkMs = -1.0;
        }
    }

    void Step(double dt) {
        using namespace sponsor_motion;
        const double out = static_cast<double>(kOutMs);
        if (active) {
            hoverMs += dt;
            if (sparkMs >= 0.0) sparkMs += dt;
        }
        Advance(face,   active, dt, static_cast<double>(kBloomMs), out);
        Advance(glow,   active, dt, static_cast<double>(kBloomMs), out);
        Advance(chr,    active, dt, static_cast<double>(kFadeMs),  out);
        Advance(meteor, active, dt, 630.0,                          out);
        Advance(tag,    active, dt, 630.0,                          out);

        // The thumb's 180ms delay applies to the IN direction only; leaving reverses
        // immediately, which is part of why leaving reads as faster.
        if (active) {
            if (hoverMs >= static_cast<double>(kThumbDelayMs)) {
                Advance(thumb, true, dt, static_cast<double>(kFadeMs), out);
            }
        } else {
            Advance(thumb, false, dt, static_cast<double>(kFadeMs), out);
        }

        if (MeteorOpacity() > 0.001 && AnimationsEnabled()) meteorMs += dt;
    }

    // ---- eased outputs -----------------------------------------------------
    // The curve is chosen by DIRECTION, as the stylesheet does: --gl-in when arriving,
    // --gl-out when leaving. The two properties whose IN transition is `linear` say so.
    double FaceMix()      const { return EaseOut(face); }
    double GlowOpacity()  const { return kGlowPeak * (active ? EaseIn(glow) : EaseOut(glow)); }
    double CharOpacity()  const { return 1.0 - (active ? chr : EaseOut(chr)); }   // in is linear
    double ThumbOpacity() const { return active ? thumb : EaseOut(thumb); }       // in is linear
    double MeteorOpacity()const { return EaseOut(meteor); }
    double TagMix()       const { return EaseOut(tag); }
    double BurstOpacity() const { return active ? 1.0 : 0.0; }   // .gl-burst has no transition

    // Reduced motion: the ring is shown statically at scale 1.15, faded in over 330ms
    // after a 300ms delay, and removed instantly on leave (the rule simply stops applying).
    double ReducedRingOpacity() const {
        if (!active) return 0.0;
        return 0.5 * Clamp01((hoverMs - 300.0) / 330.0);
    }

    bool AtRest(double p) const { return active ? (p >= 1.0) : (p <= 0.0); }

    bool Busy() const {
        if (!AtRest(face) || !AtRest(glow) || !AtRest(chr) || !AtRest(meteor) || !AtRest(tag))
            return true;
        // The thumb is only "at rest high" once its delayed run has finished.
        if (active) { if (thumb < 1.0) return true; }
        else        { if (thumb > 0.0) return true; }

        if (AnimationsEnabled()) {
            if (MeteorOpacity() > 0.001) return true;         // visible AND moving
            if (sparkMs >= 0.0 && sparkMs < SparkWindowMs()) return true;
        } else {
            if (active && hoverMs < 640.0) return true;        // the static ring's 300+330ms fade
        }
        return false;
    }
};

// ===========================================================================
// Per-DPI geometry cache.
//
// The device-space point arrays are rebuilt only when the DPI or the button rect
// changes. Nothing in the paint path allocates.
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
    int  dpi   = 0;
    int  markW = 0, markH = 0;

    double s     = 0.0;   // device px per outer viewBox unit
    double inner = 0.0;   // device px per inner (100x100 glyph) unit, seal scale included

    POINT markOrg{};      // device origin of the outer viewBox, relative to the goat button

    DevPath head[5];
    DevPath eye;

    RECT  sealRc{};
    int   sealRadius = 0;

    POINT glyphOrg{};     // top-left of the glyph layer, relative to the goat button
    int   glyphW = 0, glyphH = 0;
    DevPath chr, thumb;   // relative to glyphOrg

    POINT burstC{};       // spark centre, relative to the goat button
    int   burstHalf = 0;

    // Bloom: a blurred rounded rect, premultiplied, built once per DPI.
    Layer bloom;
    POINT bloomOrg{};

    // Radial mask for the meteor field, one byte per pixel of the goat button.
    std::vector<BYTE> mask;
    int maskW = 0, maskH = 0;

    DevPath kofiCup, kofiSaucer;
};

// -- the seal's own transform: scale(kSealScale) about the stamp's centre ----
inline double SealX(double x) { return 232.0 + (x - 232.0) * kSealScale; }
inline double SealY(double y) { return 200.0 + (y - 200.0) * kSealScale; }

void BuildBloom(Geo& g) {
    // filter: blur(5px) on a 36-unit stamp - i.e. a halo about a third of the stamp wide.
    // Reproduced as three box passes (a good Gaussian approximation) over the stamp's
    // coverage, then premultiplied with --gl-seal.
    const int sw = g.sealRc.right - g.sealRc.left;
    const int sh = g.sealRc.bottom - g.sealRc.top;
    if (sw <= 0 || sh <= 0) return;

    const double blurPx = DMax(2.0, sw / 3.0);
    const int    pad    = IMax(3, RoundI(blurPx * 2.2));
    const int    w = sw + pad * 2, h = sh + pad * 2;
    if (!g.bloom.Ensure(w, h)) return;
    g.bloom.Clear();
    g.bloomOrg.x = g.sealRc.left - pad;
    g.bloomOrg.y = g.sealRc.top - pad;

    std::vector<float> a(static_cast<size_t>(w) * h, 0.0f);
    const int r = IMax(1, RoundI(blurPx * 0.9));
    const int rad = RoundI(kArt::kSealRx * g.s * kSealScale);

    for (int y = 0; y < sh; ++y) {
        for (int x = 0; x < sw; ++x) {
            // rounded-rect coverage
            const int dx = (x < rad) ? (rad - x) : (x >= sw - rad ? x - (sw - rad - 1) : 0);
            const int dy = (y < rad) ? (rad - y) : (y >= sh - rad ? y - (sh - rad - 1) : 0);
            if (dx > 0 && dy > 0 && (dx * dx + dy * dy) > rad * rad) continue;
            a[static_cast<size_t>(y + pad) * w + (x + pad)] = 1.0f;
        }
    }

    std::vector<float> tmp(a.size(), 0.0f);
    for (int pass = 0; pass < 3; ++pass) {
        // horizontal
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                float sum = 0.0f;
                int   cnt = 0;
                for (int k = -r; k <= r; ++k) {
                    const int xx = x + k;
                    if (xx < 0 || xx >= w) continue;
                    sum += a[static_cast<size_t>(y) * w + xx];
                    ++cnt;
                }
                tmp[static_cast<size_t>(y) * w + x] = (cnt > 0) ? sum / cnt : 0.0f;
            }
        }
        // vertical
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                float sum = 0.0f;
                int   cnt = 0;
                for (int k = -r; k <= r; ++k) {
                    const int yy = y + k;
                    if (yy < 0 || yy >= h) continue;
                    sum += tmp[static_cast<size_t>(yy) * w + x];
                    ++cnt;
                }
                a[static_cast<size_t>(y) * w + x] = (cnt > 0) ? sum / cnt : 0.0f;
            }
        }
    }

    BYTE* bits = g.bloom.Bits();
    for (int i = 0; i < w * h; ++i) {
        const double v = a[static_cast<size_t>(i)];
        BYTE* p = bits + static_cast<size_t>(i) * 4;
        p[0] = Chan(GetBValue(kSeal) * v);
        p[1] = Chan(GetGValue(kSeal) * v);
        p[2] = Chan(GetRValue(kSeal) * v);
        p[3] = Chan(255.0 * v);
    }
}

void BuildMask(Geo& g, int w, int h) {
    // -webkit-mask-image: radial-gradient(120% 100% at 62% 40%, #000 34%, transparent 78%)
    // over the meteor layer's own box (inset -22% -12% of the lockup).
    g.maskW = w;
    g.maskH = h;
    g.mask.assign(static_cast<size_t>(w) * h, 0);
    const double insetX = 0.12 * w, insetY = 0.22 * h;
    const double lw = w + insetX * 2.0, lh = h + insetY * 2.0;
    const double cx = -insetX + 0.62 * lw, cy = -insetY + 0.40 * lh;
    const double rx = 1.20 * lw, ry = 1.00 * lh;
    if (rx <= 0.0 || ry <= 0.0) return;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const double ux = (x - cx) / rx, uy = (y - cy) / ry;
            const double d = std::sqrt(ux * ux + uy * uy);
            double a;
            if (d <= 0.34)      a = 1.0;
            else if (d >= 0.78) a = 0.0;
            else                a = 1.0 - (d - 0.34) / (0.78 - 0.34);
            g.mask[static_cast<size_t>(y) * w + x] = Chan(a * 255.0);
        }
    }
}

// ===========================================================================
// Layout
// ===========================================================================
struct Layout {
    int stripH = 0;
    int markH  = 0;
    double em  = 0.0;

    RECT btn[3]{};

    int kofiRadius = 0;
    int btnRadius  = 0;   // GitHub + GoatProject, and every focus ring

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

    // GoatProject internals
    RECT goatMark{};    // the outer viewBox box
    POINT namePt{};
    POINT tagPt{};
    int   nameTrack = 0;
    int   tagTrack  = 0;
};

const wchar_t* const kKofiTitle  = L"Support me on Ko-fi";
const wchar_t* const kKofiHandle = L"@IRP_HongKong";
const wchar_t* const kGhLabel    = L"Star on GitHub";
const wchar_t* const kGoatName   = L"GOATPROJECT";
const wchar_t* const kGoatTag    = L"THE PEOPLE\u2019S COMPUTE COMMONS";
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

// CSS puts a line box of `boxH` around the text; GDI's TextOut origin is the top of a cell
// of tmHeight. Centring one inside the other is what makes a px font-size land where the
// browser puts it - and it is why the reference's tagline cap-top is reproducible to the
// pixel rather than approximately.
inline int CellTop(int boxTop, int boxH, int tmHeight) {
    return boxTop + (boxH - tmHeight) / 2;
}

void ComputeLayout(HDC dc, int dpi, const RECT& client, const FontSet& F, Layout& L) {
    using theme::Dp;

    // ---- the one knob -----------------------------------------------------
    L.markH = Dp(kMarkH, dpi);       // .goat-lockup__mark { height: 5.4em }
    L.em    = L.markH / 5.4;

    L.kofiRadius = Dp(Sc(kKofiRadius), dpi);
    L.btnRadius  = Dp(Sc(theme::metric::kButtonRadius), dpi);

    // .kofi { padding: 9px 16px; line-height: 1.4 } - two lines, both at labelPx.
    const int kofiPadY  = Dp(Sc(kKofiPadY), dpi);
    const int kofiLineH = IMax(F.labelH, RoundI(kKofiLineHeight * F.labelPx));
    const int kofiNatural = kofiPadY * 2 + kofiLineH * 2;

    L.stripH = IMax(kofiNatural, L.markH + Dp(Sc(6), dpi));

    const int top = client.top + IMax(0, ((client.bottom - client.top) - L.stripH) / 2);
    const int gap = Dp(Sc(theme::metric::kGap), dpi);
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

        L.btn[0].left = x; L.btn[0].top = top;
        L.btn[0].right = x + w; L.btn[0].bottom = top + L.stripH;

        const int cy = top + L.stripH / 2;
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

        L.btn[1].left = x; L.btn[1].top = top;
        L.btn[1].right = x + w; L.btn[1].bottom = top + L.stripH;

        const int cy = top + L.stripH / 2;
        L.ghStar.left = x + padX; L.ghStar.right = L.ghStar.left + ico;
        L.ghStar.top = cy - ico / 2; L.ghStar.bottom = L.ghStar.top + ico;
        L.ghLabelPt.x = L.ghStar.right + icoGap;
        L.ghLabelPt.y = cy - F.labelH / 2;

        x = L.btn[1].right + gap;
    }

    // ---- GoatProject lockup ----------------------------------------------
    {
        const int padX  = Dp(Sc(10), dpi);
        const int markW = RoundI(220.0 * L.markH / 250.0);   // width auto, viewBox 220x250
        const int wordGap = RoundI(2.0 * L.em);              // .goat-lockup { gap: 2em }

        // letter-spacing resolves against the element's OWN font-size, and font-size is now
        // a real px value rather than a text-cell height, so there is no fudge factor.
        L.nameTrack = IMax(1, RoundI(0.24 * F.namePx));
        L.tagTrack  = IMax(1, RoundI(0.28 * F.tagPx));

        const int nameW = TextWidth(dc, F.name, kGoatName, L.nameTrack);
        const int tagW  = TextWidth(dc, F.tag,  kGoatTag,  L.tagTrack);
        const int wordW = IMax(nameW, tagW);
        const int w = padX * 2 + markW + wordGap + wordW;

        L.btn[2].left = x; L.btn[2].top = top;
        L.btn[2].right = x + w; L.btn[2].bottom = top + L.stripH;

        const int cy = top + L.stripH / 2;
        L.goatMark.left = x + padX;
        L.goatMark.right = L.goatMark.left + markW;
        L.goatMark.top = cy - L.markH / 2;
        L.goatMark.bottom = L.goatMark.top + L.markH;

        // The wordmark column: two line boxes of exactly font-size (line-height 1, which is
        // what the reference measures out at), separated by the .62em column gap, the whole
        // column centred on the same line as the mark.
        const int colGap = IMax(1, RoundI(0.62 * L.em));
        const int colH   = F.namePx + colGap + F.tagPx;
        const int colTop = cy - colH / 2;
        L.namePt.x = L.goatMark.right + wordGap;
        L.namePt.y = CellTop(colTop, F.namePx, F.nameH);
        L.tagPt.x  = L.namePt.x;
        L.tagPt.y  = CellTop(colTop + F.namePx + colGap, F.tagPx, F.tagH);
    }
}

// ===========================================================================
// Control state
// ===========================================================================
struct SponsorState {
    Motion m[3];
    int    hot     = -1;
    int    focus   = -1;
    int    pressed = -1;
    bool   tracking = false;
    bool   timerOn  = false;
    ULONGLONG lastTick = 0;

    Layout  layout{};
    FontSet fonts;      // owns its four HFONTs and frees them in its destructor
    bool   layoutValid = false;
    int    dpi = 96;

    Geo    geo;
    Layer  glyph;     // one glyph element at a time
    Layer  meteors;   // the whole meteor field, software-rasterised
    Layer  burstAcc;  // premultiplied accumulation of the spark
    Layer  burstTmp;  // GDI scratch for one spark element

    HWND   tip = nullptr;

    std::wstring url[3];
};

SponsorState* Get(HWND h) {
    if (h == nullptr) return nullptr;
    return reinterpret_cast<SponsorState*>(GetWindowLongPtrW(h, GWLP_USERDATA));
}

constexpr UINT_PTR kTimerId = 1;
constexpr UINT     kTickMs  = 16;

bool AnyBusy(const SponsorState* st) {
    for (int i = 0; i < 3; ++i) if (st->m[i].Busy()) return true;
    return false;
}

void SyncTimer(HWND hwnd, SponsorState* st) {
    // THE CPU RULE. Nothing to draw means no timer at all - not a slower one.
    const bool want = IsWindowVisible(hwnd) && AnyBusy(st);
    if (want && !st->timerOn) {
        st->lastTick = GetTickCount64();
        SetTimer(hwnd, kTimerId, kTickMs, nullptr);
        st->timerOn = true;
    } else if (!want && st->timerOn) {
        KillTimer(hwnd, kTimerId);
        st->timerOn = false;
    }
}

void SetActive(HWND hwnd, SponsorState* st, int i, bool on) {
    if (i < 0 || i > 2) return;
    if (st->m[i].active == on) return;
    st->m[i].SetActive(on);
    InvalidateRect(hwnd, nullptr, FALSE);
    SyncTimer(hwnd, st);
}

void RefreshActive(HWND hwnd, SponsorState* st) {
    for (int i = 0; i < 3; ++i) {
        SetActive(hwnd, st, i, (st->hot == i) || (st->focus == i));
    }
}

void EnsureLayout(HWND hwnd, SponsorState* st, HDC dc) {
    const int dpi = DpiOfWnd(hwnd);
    // WM_SIZE and WM_DPICHANGED_AFTERPARENT clear layoutValid, so this only rebuilds when
    // something actually moved.
    if (st->layoutValid && st->dpi == dpi) return;
    RECT client;
    GetClientRect(hwnd, &client);
    st->fonts.Build(dpi);   // no-op unless the DPI actually changed
    ComputeLayout(dc, dpi, client, st->fonts, st->layout);
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

// ===========================================================================
// Geometry build
// ===========================================================================
void BuildGeo(SponsorState* st) {
    Geo& g = st->geo;
    const Layout& L = st->layout;
    const RECT& b = L.btn[2];
    const int w = b.right - b.left, h = b.bottom - b.top;
    if (w <= 0 || h <= 0) return;

    g.dpi   = st->dpi;
    g.markW = L.goatMark.right - L.goatMark.left;
    g.markH = L.goatMark.bottom - L.goatMark.top;
    g.s     = (g.markH > 0) ? (g.markH / 250.0) : 0.0;
    g.inner = g.s * (kArt::kSealW / kArt::kInnerBox) * kSealScale;   // 36/100 outer per inner unit

    // Outer viewBox "46 -16 220 250" -> device, relative to the goat button's origin.
    g.markOrg.x = L.goatMark.left - b.left;
    g.markOrg.y = L.goatMark.top - b.top;
    const double otx = g.markOrg.x - 46.0 * g.s;
    const double oty = g.markOrg.y - (-16.0) * g.s;

    for (int i = 0; i < 5; ++i) g.head[i] = BuildDevPath(kArt::Head(i), g.s, g.s, otx, oty);
    g.eye = BuildDevPath(kArt::Eye(), g.s, g.s, otx, oty);

    // The stamp, scaled about its own centre.
    const double sx0 = SealX(kArt::kSealX), sy0 = SealY(kArt::kSealY);
    const double sx1 = SealX(kArt::kSealX + kArt::kSealW), sy1 = SealY(kArt::kSealY + kArt::kSealH);
    g.sealRc.left   = RoundI(otx + sx0 * g.s);
    g.sealRc.top    = RoundI(oty + sy0 * g.s);
    g.sealRc.right  = RoundI(otx + sx1 * g.s);
    g.sealRc.bottom = RoundI(oty + sy1 * g.s);
    g.sealRadius    = IMax(1, RoundI(kArt::kSealRx * g.s * kSealScale));

    // The glyph layer covers exactly the stamp; both glyph paths live inside 0..100.
    g.glyphOrg = POINT{ g.sealRc.left, g.sealRc.top };
    g.glyphW   = IMax(1, g.sealRc.right - g.sealRc.left);
    g.glyphH   = IMax(1, g.sealRc.bottom - g.sealRc.top);

    // Inner unit -> glyph-layer pixel.
    const double gtx = 0.0, gty = 0.0;
    g.chr   = BuildDevPath(kArt::Char(),  g.inner, g.inner, gtx, gty);
    {
        // the thumb carries transform="translate(-5 -3)"
        Contours moved = kArt::Thumb();
        for (size_t i = 0; i < moved.size(); ++i)
            for (size_t j = 0; j < moved[i].size(); ++j) {
                moved[i][j].x += kArt::kThumbDx;
                moved[i][j].y += kArt::kThumbDy;
            }
        g.thumb = BuildDevPath(moved, g.inner, g.inner, gtx, gty);
    }

    // The spark sits at inner (53,0), i.e. the top edge of the stamp.
    g.burstC.x = g.sealRc.left + RoundI(kArt::kBurstCx * g.inner);
    g.burstC.y = g.sealRc.top  + RoundI(kArt::kBurstCy * g.inner);
    g.burstHalf = IMax(6, RoundI((kArt::kRayR1 + kArt::kRayStroke) * g.inner) + 3);

    BuildBloom(g);
    BuildMask(g, w, h);

    // Ko-fi cup: viewBox 0 0 24 24 rendered into the icon rect.
    {
        const RECT& r = L.kofiIcon;
        const double k = (r.right - r.left) / 24.0;
        g.kofiCup    = BuildDevPath(kArt::KofiCup(),    k, k, r.left, r.top);
        g.kofiSaucer = BuildDevPath(kArt::KofiSaucer(), k, k, r.left, r.top);
    }

    g.valid = true;
}

// ===========================================================================
// The meteor field, rasterised in software so the tail gradient, the head halo
// and the radial mask are all real per-pixel alpha rather than an approximation.
// ===========================================================================
void PlotAA(Layer& lay, double fx, double fy, COLORREF col, double a) {
    const int x0 = static_cast<int>(std::floor(fx));
    const int y0 = static_cast<int>(std::floor(fy));
    const double tx = fx - x0, ty = fy - y0;
    lay.PlotPremul(x0,     y0,     col, a * (1 - tx) * (1 - ty));
    lay.PlotPremul(x0 + 1, y0,     col, a * tx       * (1 - ty));
    lay.PlotPremul(x0,     y0 + 1, col, a * (1 - tx) * ty);
    lay.PlotPremul(x0 + 1, y0 + 1, col, a * tx       * ty);
}

double MaskAt(const Geo& g, int x, int y) {
    if (g.mask.empty() || x < 0 || y < 0 || x >= g.maskW || y >= g.maskH) return 0.0;
    return g.mask[static_cast<size_t>(y) * g.maskW + x] / 255.0;
}

void RenderMeteors(SponsorState* st, const Motion& m) {
    using namespace sponsor_motion;
    Geo& g = st->geo;
    const RECT& b = st->layout.btn[2];
    const int w = b.right - b.left, h = b.bottom - b.top;
    if (!st->meteors.Ensure(w, h)) return;
    st->meteors.Clear();

    const double em = st->layout.em;
    const double insetX = 0.12 * w, insetY = 0.22 * h;
    const double lw = w + insetX * 2.0, lh = h + insetY * 2.0;
    const bool anim = AnimationsEnabled();

    const double ang = kMeteorAngleDeg * 3.14159265358979323846 / 180.0;
    const double ca = std::cos(ang), sa = std::sin(ang);

    for (int i = 0; i < kMeteorCount; ++i) {
        const double len = kMeteorLenEm[i] * em;
        double tx = 0.0, ty = 0.0, op = 0.5;

        if (anim) {
            const double dur = static_cast<double>(kMeteorDurMs[i]);
            double ph = (m.meteorMs - static_cast<double>(kMeteorDelayMs[i])) / dur;
            ph -= std::floor(ph);            // the negative delay seeds the phase: already mid-flight
            tx = (kMeteorFromX + (kMeteorToX - kMeteorFromX) * ph) * em;
            ty = (kMeteorFromY + (kMeteorToY - kMeteorFromY) * ph) * em;
            if (ph < kMeteorFadeIn)       op = ph / kMeteorFadeIn;
            else if (ph <= kMeteorFadeOut) op = 1.0;
            else                           op = 1.0 - (ph - kMeteorFadeOut) / (1.0 - kMeteorFadeOut);
        }
        if (op <= 0.001) continue;

        // Static box position inside the meteor layer, then translate, then rotate 34deg
        // about the streak's own centre - the order CSS applies translate/rotate in.
        const double bx = -insetX + kMeteorLeftPct[i] * lw + tx;
        const double by = -insetY + kMeteorTopPct[i]  * lh + ty;
        const double cx = bx + 0.5, cy = by + len * 0.5;
        const double hx = -sa * (len * 0.5), hy = ca * (len * 0.5);   // centre -> head
        const double headX = cx + hx, headY = cy + hy;
        const double tailX = cx - hx, tailY = cy - hy;

        const int steps = IMax(2, RoundI(len * 2.0));
        for (int k = 0; k <= steps; ++k) {
            const double u = static_cast<double>(k) / steps;      // 0 = tail, 1 = head
            const double px = tailX + (headX - tailX) * u;
            const double py = tailY + (headY - tailY) * u;
            const double a = kMeteorHeadA * u * op *
                             MaskAt(g, static_cast<int>(px), static_cast<int>(py));
            if (a > 0.002) PlotAA(st->meteors, px, py, kCream, a);
        }

        // ::after - a 2.5px dot with box-shadow 0 0 7px 1px var(--gl-meteor-halo).
        const double haloR = 4.0;
        const int hx0 = static_cast<int>(std::floor(headX - haloR));
        const int hy0 = static_cast<int>(std::floor(headY - haloR));
        for (int y = hy0; y <= hy0 + static_cast<int>(haloR * 2) + 1; ++y) {
            for (int x = hx0; x <= hx0 + static_cast<int>(haloR * 2) + 1; ++x) {
                const double dx = x + 0.5 - headX, dy = y + 0.5 - headY;
                const double d = std::sqrt(dx * dx + dy * dy);
                double a = 0.0;
                if (d <= 1.25) a = 1.0;
                else if (d < haloR) a = kMeteorHaloA * (1.0 - (d - 1.25) / (haloR - 1.25));
                if (a <= 0.002) continue;
                a *= op * MaskAt(g, x, y);
                if (a > 0.002) st->meteors.PlotPremul(x, y, kCream, a);
            }
        }
    }
}

// ===========================================================================
// The spark. Each element carries its own opacity, so they are drawn one at a
// time into a GDI scratch layer and composited into a premultiplied accumulator.
// ===========================================================================
double SegEase(double t, double a, double b, double va, double vb) {
    if (t <= a) return va;
    if (t >= b) return vb;
    const double u = EaseIn((t - a) / (b - a));
    return va + (vb - va) * u;
}

void CompositeScratch(SponsorState* st, double alpha, COLORREF col) {
    if (alpha <= 0.002) return;
    st->burstTmp.SealCoverage();
    const BYTE* src = st->burstTmp.Bits();
    const int n = st->burstTmp.W() * st->burstTmp.H();
    const int w = st->burstTmp.W();
    for (int i = 0; i < n; ++i) {
        if (src[static_cast<size_t>(i) * 4 + 3] == 0) continue;
        st->burstAcc.PlotPremul(i % w, i / w, col, alpha);
    }
}

void RenderBurst(SponsorState* st, const Motion& m) {
    using namespace sponsor_motion;
    Geo& g = st->geo;
    const int side = g.burstHalf * 2;
    if (side <= 0) return;
    if (!st->burstAcc.Ensure(side, side)) return;
    if (!st->burstTmp.Ensure(side, side)) return;
    st->burstAcc.Clear();

    const int cx = g.burstHalf, cy = g.burstHalf;
    const double u = g.inner;                 // device px per inner unit
    const bool anim = AnimationsEnabled();
    const double t = m.sparkMs;

    if (!anim) {
        // Reduced motion: no rays, no embers, the ring shown statically at scale 1.15.
        const double op = m.ReducedRingOpacity();
        if (op <= 0.002) return;
        st->burstTmp.Clear();
        const double r = kArt::kRingR * 1.15 * u;
        const int sw = IMax(1, RoundI(kArt::kRingStroke * 1.15 * u));
        HPEN pen = CreatePen(PS_SOLID, sw, kSpark);
        if (pen != nullptr) {
            {
                SelectScope ps(st->burstTmp.Dc(), pen);
                SelectScope bs(st->burstTmp.Dc(), GetStockObject(NULL_BRUSH));
                Ellipse(st->burstTmp.Dc(), cx - RoundI(r), cy - RoundI(r),
                        cx + RoundI(r), cy + RoundI(r));
            }
            DeleteObject(pen);
        }
        CompositeScratch(st, op, kSpark);
        return;
    }

    if (t < 0.0) return;

    // ---- six rays: stroke-dashoffset 9 -> -32 over 690ms, each with its own lag ----
    for (int i = 0; i < 6; ++i) {
        const double lag = static_cast<double>(kRayLagMs[i]);
        const double p = (t - lag) / static_cast<double>(kRayMs);
        if (p <= 0.0 || p >= 1.0) continue;
        // @keyframes gl-ray: dashoffset 9 -> -32, opacity 0 -> 1 at 22% -> 0.
        const double off = kArt::kRayDashOn +
                           (-kArt::kRayDashOff - kArt::kRayDashOn) * EaseIn(p);
        const double opacity = (p < 0.22) ? SegEase(p, 0.0, 0.22, 0.0, 1.0)
                                          : SegEase(p, 0.22, 1.0, 1.0, 0.0);
        if (opacity <= 0.002) continue;

        st->burstTmp.Clear();
        const double a = kArt::kRayDeg[i] * 3.14159265358979323846 / 180.0;
        const double dx = std::cos(a), dy = std::sin(a);
        const double period = kArt::kRayDashOn + kArt::kRayDashOff;
        const double lineLen = kArt::kRayR1 - kArt::kRayR0;

        LOGBRUSH lb;
        lb.lbStyle = BS_SOLID;
        lb.lbColor = kSpark;
        lb.lbHatch = 0;
        HPEN pen = ExtCreatePen(PS_GEOMETRIC | PS_SOLID | PS_ENDCAP_ROUND | PS_JOIN_ROUND,
                                IMax(1, RoundI(kArt::kRayStroke * u)), &lb, 0, nullptr);
        if (pen != nullptr) {
            {
                SelectScope ps(st->burstTmp.Dc(), pen);
                for (int k = -1; k <= 2; ++k) {
                    double s0 = k * period - off;
                    double s1 = s0 + kArt::kRayDashOn;
                    if (s1 <= 0.0 || s0 >= lineLen) continue;
                    if (s0 < 0.0) s0 = 0.0;
                    if (s1 > lineLen) s1 = lineLen;
                    const double r0 = kArt::kRayR0 + s0, r1 = kArt::kRayR0 + s1;
                    MoveToEx(st->burstTmp.Dc(), cx + RoundI(dx * r0 * u),
                             cy + RoundI(dy * r0 * u), nullptr);
                    LineTo(st->burstTmp.Dc(), cx + RoundI(dx * r1 * u),
                           cy + RoundI(dy * r1 * u));
                }
            }
            DeleteObject(pen);
        }
        CompositeScratch(st, opacity, kSpark);
    }

    // ---- the ring: scale 0.3 -> 1.9, opacity 0 -> 0.85 at 18% -> 0 ----
    {
        const double p = (t - static_cast<double>(kRingLagMs)) / static_cast<double>(kRingMs);
        if (p > 0.0 && p < 1.0) {
            const double k = 0.3 + (1.9 - 0.3) * EaseIn(p);
            const double opacity = (p < 0.18) ? SegEase(p, 0.0, 0.18, 0.0, 0.85)
                                              : SegEase(p, 0.18, 1.0, 0.85, 0.0);
            if (opacity > 0.002) {
                st->burstTmp.Clear();
                const double r = kArt::kRingR * k * u;
                const int sw = IMax(1, RoundI(kArt::kRingStroke * k * u));
                HPEN pen = CreatePen(PS_SOLID, sw, kSpark);
                if (pen != nullptr) {
                    {
                        SelectScope ps(st->burstTmp.Dc(), pen);
                        SelectScope bs(st->burstTmp.Dc(), GetStockObject(NULL_BRUSH));
                        Ellipse(st->burstTmp.Dc(), cx - RoundI(r), cy - RoundI(r),
                                cx + RoundI(r), cy + RoundI(r));
                    }
                    DeleteObject(pen);
                }
                CompositeScratch(st, opacity, kSpark);
            }
        }
    }

    // ---- three embers: translate(dx,dy) scale 0.4 -> 1, opacity 0 -> 0.9 at 25% -> 0 ----
    for (int i = 0; i < 3; ++i) {
        const double p = (t - static_cast<double>(kEmberLagMs[i])) / static_cast<double>(kEmberMs);
        if (p <= 0.0 || p >= 1.0) continue;
        const double e = EaseIn(p);
        const double opacity = (p < 0.25) ? SegEase(p, 0.0, 0.25, 0.0, 0.9)
                                          : SegEase(p, 0.25, 1.0, 0.9, 0.0);
        if (opacity <= 0.002) continue;

        st->burstTmp.Clear();
        const double k = 0.4 + 0.6 * e;
        const double ex = kArt::kEmberCx[i] + kEmberDx[i] * e;
        const double ey = kArt::kEmberCy[i] + kEmberDy[i] * e;
        const double r  = DMax(0.6, kArt::kEmberR[i] * k * u);
        const int px = cx + RoundI(ex * u), py = cy + RoundI(ey * u);
        HBRUSH br = CreateSolidBrush(kSpark);
        if (br != nullptr) {
            {
                SelectScope bs(st->burstTmp.Dc(), br);
                SelectScope ps(st->burstTmp.Dc(), GetStockObject(NULL_PEN));
                Ellipse(st->burstTmp.Dc(), px - RoundI(r), py - RoundI(r),
                        px + RoundI(r) + 1, py + RoundI(r) + 1);
            }
            DeleteObject(br);
        }
        CompositeScratch(st, opacity, kSpark);
    }
}

// ===========================================================================
// Painting
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

void PaintGoat(HDC dc, SponsorState* st, int dpi) {
    const Layout& L = st->layout;
    const Motion& m = st->m[2];
    Geo& g = st->geo;
    RECT r = L.btn[2];
    const bool hot = (st->hot == 2);
    const int dy = (st->pressed == 2) ? theme::Dp(1, dpi) : 0;
    OffsetRect(&r, 0, dy);

    const theme::Palette& P = theme::P();
    theme::DrawRoundRect(dc, r, L.btnRadius,
                         hot ? P.cardBgAlt : P.cardBg, hot ? P.borderStrong : P.border);

    if (!g.valid) return;

    const int ox = L.btn[2].left, oy = L.btn[2].top + dy;

    // ---- the meteor field, behind the mark, clipped to the button (overflow: clip) ----
    const double meteorOp = m.MeteorOpacity();
    if (meteorOp > 0.002) {
        RenderMeteors(st, m);
        HRGN clip = CreateRoundRectRgn(r.left, r.top, r.right + 1, r.bottom + 1,
                                       L.btnRadius * 2, L.btnRadius * 2);
        if (clip != nullptr) {
            SelectClipRgn(dc, clip);
            st->meteors.Blend(dc, ox, oy, meteorOp);
            SelectClipRgn(dc, nullptr);
            DeleteObject(clip);
        } else {
            st->meteors.Blend(dc, ox, oy, meteorOp);
        }
    }

    POINT oldOrg{};
    OffsetViewportOrgEx(dc, ox, oy, &oldOrg);

    // ---- .gl-head ---------------------------------------------------------
    for (int i = 0; i < 5; ++i) FillDevPath(dc, g.head[i], kCream);
    FillDevPath(dc, g.eye, kCream);

    SetViewportOrgEx(dc, oldOrg.x, oldOrg.y, nullptr);

    // ---- .gl-seal-glow (blurred, behind) then .gl-seal-face ---------------
    const double glow = m.GlowOpacity();
    if (glow > 0.002 && g.bloom.Valid()) {
        g.bloom.Blend(dc, ox + g.bloomOrg.x, oy + g.bloomOrg.y, glow);
    }
    {
        RECT sr = g.sealRc;
        OffsetRect(&sr, ox, oy);
        theme::FillRoundRect(dc, sr, g.sealRadius, Lerp(kSeal, kSealLit, m.FaceMix()));
    }

    // ---- the cross-fade: .gl-char out, .gl-thumb in -----------------------
    const double charOp = m.CharOpacity();
    const double thumbOp = m.ThumbOpacity();
    if (st->glyph.Ensure(g.glyphW, g.glyphH)) {
        if (charOp > 0.002) {
            st->glyph.Clear();
            FillDevPath(st->glyph.Dc(), g.chr, kGlyph);
            st->glyph.SealCoverage();
            st->glyph.Blend(dc, ox + g.glyphOrg.x, oy + g.glyphOrg.y, charOp);
        }
        if (thumbOp > 0.002) {
            st->glyph.Clear();
            FillDevPath(st->glyph.Dc(), g.thumb, kGlyph);
            st->glyph.SealCoverage();
            st->glyph.Blend(dc, ox + g.glyphOrg.x, oy + g.glyphOrg.y, thumbOp);
        }
    }

    // ---- .gl-burst --------------------------------------------------------
    const double burst = m.BurstOpacity();
    if (burst > 0.002) {
        RenderBurst(st, m);
        if (st->burstAcc.Valid()) {
            st->burstAcc.Blend(dc, ox + g.burstC.x - g.burstHalf,
                                   oy + g.burstC.y - g.burstHalf, burst);
        }
    }

    // ---- the wordmark -----------------------------------------------------
    DrawWithFont(dc, ox + (L.namePt.x - L.btn[2].left), oy + (L.namePt.y - L.btn[2].top),
                 kGoatName, st->fonts.name, kName, L.nameTrack);
    DrawWithFont(dc, ox + (L.tagPt.x - L.btn[2].left), oy + (L.tagPt.y - L.btn[2].top),
                 kGoatTag, st->fonts.tag, Lerp(kTag, kName, m.TagMix()), L.tagTrack);

    if (st->focus == 2) DrawFocusRing(dc, r, dpi, L.btnRadius);
}

void OnPaint(HWND hwnd) {
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hwnd, &ps);
    if (hdc == nullptr) return;

    SponsorState* st = Get(hwnd);
    RECT client;
    GetClientRect(hwnd, &client);
    const int w = client.right - client.left, h = client.bottom - client.top;

    HDC mem = CreateCompatibleDC(hdc);
    HBITMAP bmp = (mem != nullptr) ? CreateCompatibleBitmap(hdc, IMax(1, w), IMax(1, h)) : nullptr;
    HBITMAP oldBmp = nullptr;
    HDC dc = hdc;
    if (mem != nullptr && bmp != nullptr) {
        oldBmp = static_cast<HBITMAP>(SelectObject(mem, bmp));
        dc = mem;
    }

    theme::FillBackground(dc, client);

    if (st != nullptr) {
        EnsureLayout(hwnd, st, dc);
        if (!st->geo.valid) BuildGeo(st);
        const int dpi = st->dpi;
        PaintKofi(dc, st, dpi);
        PaintGitHub(dc, st, dpi);
        PaintGoat(dc, st, dpi);
    }

    if (dc == mem) BitBlt(hdc, 0, 0, w, h, mem, 0, 0, SRCCOPY);
    if (oldBmp != nullptr) SelectObject(mem, oldBmp);
    if (bmp != nullptr) DeleteObject(bmp);
    if (mem != nullptr) DeleteDC(mem);
    EndPaint(hwnd, &ps);
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
}

LRESULT CALLBACK SponsorProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_NCCREATE: {
        SponsorState* st = new (std::nothrow) SponsorState();
        if (st != nullptr) {
            st->url[0] = sponsor_url::kKofi;
            st->url[1] = sponsor_url::kGitHub;
            st->url[2] = sponsor_url::kGoatProject;
        }
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(st));
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
    case WM_CREATE: {
        SponsorState* st = Get(hwnd);
        if (st != nullptr) {
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
            if (st->timerOn) KillTimer(hwnd, kTimerId);
            if (st->tip != nullptr) DestroyWindow(st->tip);
            delete st;   // every Layer frees its DIB and DC in its destructor
        }
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
    case WM_ERASEBKGND:
        return 1;   // WM_PAINT covers every pixel

    case WM_PAINT:
        OnPaint(hwnd);
        return 0;

    case WM_SIZE: {
        SponsorState* st = Get(hwnd);
        if (st != nullptr) {
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
        if (st != nullptr) { st->layoutValid = false; st->geo.valid = false; }
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
        if (st != nullptr) SyncTimer(hwnd, st);
        break;
    }
    case WM_TIMER: {
        if (wParam != kTimerId) break;
        SponsorState* st = Get(hwnd);
        if (st == nullptr) return 0;
        const ULONGLONG now = GetTickCount64();
        double dt = static_cast<double>(now - st->lastTick);
        st->lastTick = now;
        if (dt <= 0.0) dt = static_cast<double>(kTickMs);
        if (dt > 120.0) dt = 120.0;   // a stall must not teleport the animation
        for (int i = 0; i < 3; ++i) st->m[i].Step(dt);
        InvalidateRect(hwnd, nullptr, FALSE);
        SyncTimer(hwnd, st);
        return 0;
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
        if (hit != st->hot) {
            st->hot = hit;
            InvalidateRect(hwnd, nullptr, FALSE);
            RefreshActive(hwnd, st);
        }
        return 0;
    }
    case WM_MOUSELEAVE: {
        SponsorState* st = Get(hwnd);
        if (st == nullptr) break;
        st->tracking = false;
        if (st->hot != -1) {
            st->hot = -1;
            st->pressed = -1;
            InvalidateRect(hwnd, nullptr, FALSE);
            RefreshActive(hwnd, st);
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
            if (st->focus < 0) st->focus = 0;
            InvalidateRect(hwnd, nullptr, FALSE);
            RefreshActive(hwnd, st);
        }
        return 0;
    }
    case WM_KILLFOCUS: {
        SponsorState* st = Get(hwnd);
        if (st != nullptr) {
            st->focus = -1;
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
    wc.hCursor       = LoadCursorW(nullptr, IDC_HAND);
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

    FontSet F;          // local: its destructor frees the four faces on the way out
    F.Build(dpi);
    Layout L;
    ComputeLayout(dc, dpi, client, F, L);
    ReleaseDC(nullptr, dc);

    out.cx = L.btn[2].right - L.btn[0].left;
    out.cy = L.stripH;
    return out;
}

}  // namespace cd
