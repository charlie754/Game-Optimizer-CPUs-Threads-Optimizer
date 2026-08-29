// Game Optimizer - the animated sponsor strip: Ko-fi, GitHub star, GoatProject.
//
// PORTED FROM CSS, NOT INVENTED. The motion design comes from
//   F:\google map plugin\extension\content\brand\goat-lockup-hover.css
// and the Ko-fi button from
//   F:\google map plugin\extension\options\options.css
// Both are the operator's own work. Nothing is copied verbatim - CSS cannot be - but every
// duration, delay, easing curve and opacity stop below is taken from that file so the feel
// survives the port to GDI.
//
// ============================================================================
// THE ONE CONSTRAINT THAT OUTRANKS FIDELITY
// ============================================================================
// This application exists to SAVE CPU. A sponsor button that spins a 60 Hz repaint forever
// would burn a core's worth of tenths of a percent for decoration, in the one program whose
// entire purpose is not doing that. So:
//
//   * The animation clock runs ONLY when there is something to draw: the strip is visible,
//     AND (a button is hovered OR focused, OR a transition is still settling, OR the meteor
//     layer's opacity is above zero).
//   * When every button is at rest the timer is KILLED, not left ticking at a lower rate.
//   * In the CSS the meteors run continuously and hover merely fades them in. Here they run
//     only while they would actually be visible, because animating an invisible layer is
//     pure waste. Visually identical; measurably cheaper.
//
// Honour the system animation preference. Windows exposes it through
// SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION) - the native equivalent of the CSS
// @media (prefers-reduced-motion: reduce) block, which that stylesheet already implements.
// When animations are off: the cross-fade still happens, the travelling parts do not.
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

// [M] dagoat.io was fetched 2026-08-29 and loads: "D.A. G.O.A.T. - Decentralized Architecture,
// Global Orchestration & Aligned Technology", a community-owned compute commons.
constexpr const wchar_t* kGoatProject = L"https://dagoat.io";

// Game Optimizer now has its OWN dedicated repository, and the Star button points there.
// It used to open the GoatProject repository, which was a misdirection: this application is
// its own project, and asking someone to star a different codebase spends the one click a
// visitor was willing to give on the wrong repo. kGoatProject above is UNAFFECTED - the
// GOATPROJECT lockup still goes to dagoat.io, which is a separate destination and a separate
// button. Changing one of these two must never be read as changing the other.
//
// [A] the repository was NOT confirmed to exist at generation time - nothing in this session
// requested github.com, so "it resolves" is assumed and not measured. If it 404s, this is the
// only line to change: tools\gen-sponsor-html.py reads this header and refuses to generate if
// its own URL_GITHUB disagrees, so the two cannot drift apart.
constexpr const wchar_t* kGitHub = L"https://github.com/charlie754/Game-Optimizer-CPUs-Threads-Optimizer";

}  // namespace sponsor_url

// ---- Where the artwork comes from ------------------------------------------
// The GoatProject lockup is not redrawn by eye. Its complete vector geometry is in the
// operator's own extension at
//   F:\google map plugin\extension\content\widget.js   (the GOAT_LOCKUP_MARKUP string)
// and consists of: six head paths plus an eye circle in .gl-head; a 36x36 rounded seal
// (.gl-seal-glow behind .gl-seal-face); inside it a 100x100 viewBox holding the .gl-char
// path (the SimSun 羊 outline, baked to a path so no CJK font is needed) and the .gl-thumb
// silhouette; and a .gl-burst group at translate(53,0) with one .gl-ring circle r=12, six
// .gl-ray lines from r=10 to r=40 at -168/-138/-108/-76/-44/-14 degrees, and three .gl-ember
// circles. The outer viewBox is "46 -16 220 250".
//
// Port those paths; do not approximate them. A hand-drawn lookalike of somebody's brand mark
// is worse than no mark at all.

// ---- The control -----------------------------------------------------------
// A single child window holding all three buttons in a row, so one timer, one back buffer and
// one hit test serve the whole strip. Sits directly above OK / Cancel / Apply.
extern const wchar_t* kSponsorClass;

// Sent to the parent as WM_COMMAND when a button is activated, with the button id in
// LOWORD(wParam). The parent does not need to handle it - the control opens the URL itself -
// but it is there so the parent can log or suppress.
constexpr WORD SPN_CLICKED = 0x8103;

enum SponsorId { kSponsorKofi = 1, kSponsorGitHub = 2, kSponsorGoat = 3 };

void SponsorRegister(HINSTANCE hInst);

// Natural size at the given DPI, so the caller can lay out the footer without guessing.
SIZE SponsorMeasure(int dpi);

// ---- Motion constants, lifted from the stylesheet --------------------------
// Kept here rather than buried in the .cpp so the port can be diffed against the CSS.
namespace sponsor_motion {

// .goat-lockup: --gl-t-out 360ms, --gl-t-fade 450ms.
// "leaving is always faster than arriving" - the comment in the original, and it is the thing
// that makes the interaction feel right. Do not equalise these.
constexpr int kOutMs        = 360;   // leaving
constexpr int kBloomMs      = 690;   // seal warms + glow rises (transition 690ms)
constexpr int kFadeMs       = 450;   // glyph cross-fade
constexpr int kThumbDelayMs = 180;   // the thumbs-up starts 180ms into the cross-fade

// gl-ray: 690ms, six rays with staggered lags.
constexpr int kRayMs = 690;
constexpr int kRayLagMs[6] = { 720, 765, 738, 792, 752, 816 };

// gl-ring: 930ms, 705ms lag, scale 0.3 -> 1.9, opacity 0 -> 0.85 -> 0.
constexpr int kRingMs     = 930;
constexpr int kRingLagMs  = 705;

// gl-ember: 1350ms, three embers with their own lag and travel vector (px at font-size 20).
constexpr int kEmberMs = 1350;
constexpr int kEmberLagMs[3] = { 840, 975, 908 };
constexpr int kEmberDx[3]    = { -13, 15, 3 };
constexpr int kEmberDy[3]    = { -28, -21, -34 };

// gl-meteor: nine streaks, each with its own height, duration and (negative) delay so the
// field is already in motion on the first frame rather than starting together.
constexpr int kMeteorCount = 9;
constexpr double kMeteorLeftPct[9] = { 0.74, 0.52, 0.89, 0.33, 0.64, 0.18, 0.97, 0.44, 0.82 };
constexpr double kMeteorTopPct [9] = { 0.06, -0.04, 0.22, 0.12, 0.38, -0.08, -0.02, 0.30, 0.48 };
constexpr double kMeteorLenEm  [9] = { 5.2, 3.4, 4.6, 2.8, 3.9, 4.1, 3.1, 5.6, 2.6 };
constexpr int    kMeteorDurMs  [9] = { 2850, 3900, 3150, 4650, 3600, 4200, 3450, 2550, 5100 };
constexpr int    kMeteorDelayMs[9] = { 0, -480, -1110, -1650, -270, -1430, -2180, -870, -2700 };
constexpr double kMeteorAngleDeg = 34.0;   // long axis along the fall direction

// The travel, in em, from the keyframe: translate 3.2em,-4.8em -> -3.4em,5.1em.
constexpr double kMeteorFromX = 3.2, kMeteorFromY = -4.8;
constexpr double kMeteorToX = -3.4, kMeteorToY = 5.1;
// Opacity stops: 0% -> 0, 9% -> 1, 68% -> 1, 100% -> 0.
constexpr double kMeteorFadeIn = 0.09, kMeteorFadeOut = 0.68;

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

}  // namespace cd
