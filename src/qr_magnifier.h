// Game Optimizer - the QR magnifier: the GOAT Cam App Store QR, enlarged, in a small popup.
//
// WHY A POPUP. The sponsor band is 76 px tall, and at the 2 px/module the band allows, the QR is
// small enough that a phone has to come close to scan it. SPEC section 7.4 asks for the QR to
// ENLARGE on hover to at least 4 px per module; it cannot grow inside a 76 px band, so it grows
// in a window of its own, just above the strip.
//
// THE RULES IT KEEPS - this program runs beside full-screen games:
//   * It NEVER ACTIVATES and NEVER TAKES FOCUS: WS_EX_NOACTIVATE, shown with SW_SHOWNOACTIVATE,
//     MA_NOACTIVATE on a click, and HTTRANSPARENT so the pointer passes straight through it.
//   * It is topmost only while it is shown, and it is shown only while the QR is hovered or
//     keyboard-focused. It hides on leave, on focus loss, and when the window that owns the strip
//     moves, resizes, hides, deactivates or is destroyed (a subclass on that top-level window,
//     installed only while the popup is up).
//   * It FAILS CLOSED: no popup if that subclass cannot be installed (nothing would take it down),
//     and none over an owner that is hidden or minimised. An identical request while it is up is
//     a no-op - no reposition, no fade restart.
//   * It stays on the monitor of the tile: above the tile when there is room in that monitor's
//     work area, else below it, and clamped inside it either way.
//   * The QR is the exact matrix (goatcam_qr.h), on whole pixels, at an integer module size.
//   * The fade (in ~200 ms, out ~120 ms; instant when Windows asks for no animation) runs a timer
//     only while it is fading. If no timer can be had, it lands at once instead of freezing.
#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace cd {

// Shows the magnifier for the QR tile at `tileScreenRect` (screen coordinates), sized for `dpi`.
// `owner` is the control the tile lives in; the popup is owned by its top-level window. Calling
// it again while it is shown for the same tile does nothing.
void QrMagnifierShow(HWND owner, const RECT& tileScreenRect, int dpi);

// Hides it (fading out when animations are on). Safe to call when it is not shown.
void QrMagnifierHide();

// Hides it NOW, with no fade - for a window that is moving, hiding or going away.
void QrMagnifierHideNow();

// Whether it is shown or fading in. A fade-out counts as hidden.
bool QrMagnifierIsShown();

// The popup window, or nullptr until the first Show. For capture tools and the offscreen harness.
HWND QrMagnifierWindow();

// Device px per QR module at `dpi`: max(4, MulDiv(4, dpi, 96)) - 4 at 100%, 5 at 125%, 6 at 150%.
int QrMagnifierModulePx(int dpi);

// The popup's size at `dpi`: the 37-module code (its 4-module quiet zone included) and a border.
SIZE QrMagnifierSize(int dpi);

// Where a popup of `size` goes for `tile`, inside `work` (a monitor work area), `gap` px away.
// Above when it fits, else below, else pinned inside `work`. Pure; exposed for the harness.
RECT QrMagnifierPlace(const RECT& tile, const RECT& work, SIZE size, int gap);

}  // namespace cd
