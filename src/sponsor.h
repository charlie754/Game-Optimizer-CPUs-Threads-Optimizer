// Game Optimizer - the animated sponsor strip: Ko-fi, GitHub star, GOAT Cam.
//
// PORTED FROM CSS, NOT INVENTED. The GOAT Cam banner's design comes from
//   brand\goatcam-banner\SPEC.md (section 7, the v2 redesign) + goatcam-banner.css in the GOAT
// Cam repository (the `.goat--strip` variant), and the Ko-fi button from
//   F:\google map plugin\extension\options\options.css
// All are the operator's own work. Nothing is copied verbatim - CSS cannot be - but every
// duration, delay, easing curve and opacity stop below is taken from those files so the feel
// survives the port to GDI.
//
// ============================================================================
// THE ONE CONSTRAINT THAT OUTRANKS FIDELITY
// ============================================================================
// This application exists to SAVE CPU. A sponsor button that spins a 60 Hz repaint forever
// would burn a core's worth of tenths of a percent for decoration, in the one program whose
// entire purpose is not doing that. So:
//
//   * The animation clock runs ONLY while something is moving: the strip is visible AND a
//     transition is still settling. Hovering is NOT a reason by itself - a plate that has
//     finished its hover transition, with the pointer still on it, costs nothing.
//   * When nothing is moving the timer is KILLED, not left ticking at a lower rate.
//   * The web banner's LOOPS - steam over the cup, a pulsing OSD dot - are NOT ported. Each
//     would hold the timer open for as long as the pointer stays.
//
// Honour the system animation preference. Windows exposes it through
// SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION) - the native equivalent of the CSS
// @media (prefers-reduced-motion: reduce) block, which that stylesheet already implements.
// When animations are off: the fades still happen, the travelling parts do not.
#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <string>

namespace cd {

// ---- Destinations ----------------------------------------------------------
// A button whose URL is empty must NOT open a browser - it draws normally, shows
// "link not configured" on hover, and does nothing when clicked. Opening a WRONG URL is worse
// than opening none: it sends the user somewhere the author did not intend, and it is the kind
// of mistake nobody notices until a stranger does.
namespace sponsor_url {

// [M] From options.html in the operator's own sample project.
constexpr const wchar_t* kKofi = L"https://ko-fi.com/irp_hongkong";

// The GOAT Cam banner's plate. [M] Fetched 2026-10-26: HTTP 200 at https://goatcam.dagoat.io/,
// <title>GOAT Cam &mdash; your iPhone as a webcam for Windows</title>.
// It REPLACED kGoatProject, which pointed at dagoat.io (the GOATPROJECT lockup), on 2026-10-25.
// That URL is deliberately not spelled as a wide literal here: tools\gen-sponsor-html.py checks
// this header for the exact literal it emits, and a comment must not satisfy that check. The
// App Store link lives only in the QR's pixels: a phone scans it and this app never opens it,
// so it is deliberately NOT a destination here and not on webview_host.cpp's allow-list.
constexpr const wchar_t* kGoatCam = L"https://goatcam.dagoat.io";

// The built-in fallback strip is the original DaGoat.io sponsor destination.
constexpr const wchar_t* kDaGoat = L"https://dagoat.io";

// Kept as a source-compatible name for callers that still describe the built-in strip as the
// GoatProject lockup. New code should use kDaGoat so the online and fallback destinations cannot
// be confused.
constexpr const wchar_t* kGoatProject = kDaGoat;

// Game Optimizer now has its OWN dedicated repository, and the Star button points there.
// It used to open the GoatProject repository, which was a misdirection: this application is
// its own project, and asking someone to star a different codebase spends the one click a
// visitor was willing to give on the wrong repo. kGoatCam above is a separate destination and
// a separate button. Changing one of these two must never be read as changing the other.
//
// [A] the repository was NOT confirmed to exist at generation time - nothing in this session
// requested github.com, so "it resolves" is assumed and not measured. If it 404s, this is the
// only line to change: tools\gen-sponsor-html.py reads this header and refuses to generate if
// its own URL_GITHUB disagrees, so the two cannot drift apart.
constexpr const wchar_t* kGitHub = L"https://github.com/charlie754/Game-Optimizer-CPUs-Threads-Optimizer";

}  // namespace sponsor_url

// ---- Where the artwork comes from ------------------------------------------
// The online plate is filled by the verified JPEG in ob::Model, decoded from an IStream over
// those bytes. Its wordmark, signed headline/tagline/badge, viewfinder effects and 37x37 QR are
// all model fields; the static iPhone preview is local art and is not part of the signed record.
// Hovering the online QR opens the existing native QR magnifier (qr_magnifier.h).

// ---- The control -----------------------------------------------------------
// A single child window holding all three items in a row, so one timer, one back buffer and
// one hit test serve the whole strip. Sits directly above OK / Cancel / Apply. The GOAT Cam
// item is the headline and a plate; only the PLATE is a button (kSponsorGoat).
extern const wchar_t* kSponsorClass;

// Sent to the parent as WM_COMMAND when a button is activated, with the button id in
// LOWORD(wParam). The parent does not need to handle it - the control opens the URL itself -
// but it is there so the parent can log or suppress.
constexpr WORD SPN_CLICKED = 0x8103;

enum SponsorId { kSponsorKofi = 1, kSponsorGitHub = 2, kSponsorGoat = 3 };

void SponsorRegister(HINSTANCE hInst);

// Natural size at the given DPI, so the caller can lay out the footer without guessing. The
// height is the GDI fallback's QR-safe GOAT Cam band, Dp(84); the WebView2 strip keeps its own
// 76px CSS band. Settings uses this returned size when the fallback is selected.
SIZE SponsorMeasure(int dpi);

// ---- Motion constants, lifted from the stylesheet --------------------------
// Kept here rather than buried in the .cpp so the port can be diffed against the CSS.
namespace sponsor_motion {

// --gc-t-out 360ms. "Leaving is always faster than arriving" - the comment in the original
// stylesheet, and it is the thing that makes the interaction feel right. Do not equalise these.
constexpr int kOutMs        = 360;   // leaving

// The GOAT Cam plate (goatcam-banner.css, the :hover / :focus-visible rules).
constexpr int    kPushInMs    = 1200;   // .gc-photo__frame scale(1) -> scale(1.06), --gc-in
constexpr double kPushScale   = 0.06;
constexpr double kStageScale  = 0.05;   // the visible stage grows about its center on hover
constexpr int    kBracketInMs = 420;    // .gc-vf inset tightens 3px, --gc-in
constexpr int    kLockInMs    = 300;    // .gc-vf::after (the gold lock) opacity, linear ...
constexpr int    kLockDelayMs = 140;    // ... 140ms late
constexpr int    kLiftInMs    = 420;    // the lift, --gc-in
constexpr int    kAccentInMs  = 630;    // the tagline colour and the QR tile's caramel ring

// --gl-in: cubic-bezier(0.16, 1, 0.3, 1)  --gl-out: cubic-bezier(0.4, 0, 0.2, 1)
// Evaluate properly - a linear stand-in loses the overshoot-free snap that makes the original
// read as deliberate rather than sluggish.
double EaseIn(double t);    // the (0.16, 1, 0.3, 1) curve
double EaseOut(double t);   // the (0.4, 0, 0.2, 1) curve

}  // namespace sponsor_motion

// True when the user has asked Windows to minimise animation
// (SystemParametersInfoW SPI_GETCLIENTAREAANIMATION == FALSE). Re-read on
// WM_SETTINGCHANGE rather than cached forever.
bool AnimationsEnabled();

#if defined(CD_TESTS) || defined(CD_SPONSOR_HARNESS)
// Offscreen capture seams; they never ship in the production ABI.
void SponsorSetDpiForTest(int dpi);
void SponsorSetHoverForTest(HWND hwnd, bool hover);

struct SponsorHarnessLayout {
    RECT buttons[3]{};
    RECT plate{};
    RECT viewfinder{};
    RECT tile{};
    RECT text{};
    RECT copy{};
    RECT iphone{};
    RECT wordmark{};
    RECT tagline[2]{};
    int tagBaseline[2]{};
    int tagEmBaseline[2]{};
    bool taglineHasEm[2]{};
    bool taglineTruncated[2]{};
    RECT badge{};
    RECT stage{};
    RECT brackets{};
    RECT fallbackWordmark{};
    RECT fallbackTag{};
    POINT qrOrigin{};
    int module = 0;
};

bool SponsorGetLayoutForTest(HWND hwnd, SponsorHarnessLayout* out);
#endif

}  // namespace cd
