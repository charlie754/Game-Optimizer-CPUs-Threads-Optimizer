// Game Optimizer - the sponsor strip, rendered by an embedded WebView2.
//
// ============================================================================
// WHY A BROWSER IS IN A PROGRAM WHOSE JOB IS TO SAVE CPU
// ============================================================================
// The three sponsor buttons come from the operator's own browser extension. They were first
// hand-ported into GDI: 398 lines of CSS and a large SVG re-expressed as drawing code. That
// approach is lossy BY CONSTRUCTION - CSS transitions, cubic-bezier easing, blur filters, a
// radial mask-image and nine animated meteors cannot be reproduced exactly - and it was
// rejected twice for not matching. So the elements are now COPIED, and the engine that
// renders them is the engine they were written for. The pixels are the plugin's pixels.
//
// That is only acceptable under four rules, and all four are enforced here rather than
// documented and forgotten:
//
//   1. LAZY. Nothing web-related exists until the Settings window is created, and everything
//      is destroyed when it closes. While the app sits in the tray - which is nearly all the
//      time - there is no browser, no user-data folder open, and no extra process.
//   2. NEVER ON THE STARTUP PATH. The tray, the engine and the first-run wizard do not call
//      into this file. LoadLibraryW on the loader happens on the first Settings open.
//   3. THE GDI STRIP IS THE FALLBACK AND IT STAYS. If the loader DLL is missing, the runtime
//      is absent, or environment or controller creation fails for any reason at all, the
//      caller shows cd::kSponsorClass in the same rectangle. The app never loses its sponsor
//      strip and never fails to start because of this feature.
//   4. LINKS LEAVE. NewWindowRequested and NavigationStarting are both cancelled and the URL
//      is handed to ShellExecuteW, so a link opens in the user's real browser and never
//      inside this view. The three destinations are the constants in sponsor.h and nothing
//      else is ever opened.
//
// There is no WebView2.h on the build machine, so the handful of COM interfaces this needs
// are declared in the .cpp. Their IIDs and - far more importantly - their vtable METHOD ORDER
// were read out of Microsoft's own generated bindings rather than reconstructed from memory:
//   D:\cargo\registry\src\index.crates.io-1949cf8c6b5b557f\webview2-com-sys-0.38.2\src\bindings.rs
// A wrong IID or a method declared out of order is a crash, not a compile error.
#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace cd {

// Opaque. Created by WebSponsorCreate, freed by WebSponsorDestroy, and by nothing else.
struct WebSponsor;

// The panel's MINIMUM width and its height, in DEVICE pixels at `dpi`.
//
// cy IS THE HEIGHT TO RESERVE. cx IS NOT THE WIDTH TO USE. The two halves of this SIZE are
// different kinds of number and the name says so - it was WebSponsorNaturalSize until the panel
// stopped having a natural width.
//
// The panel is ONE ROW OF THREE GROUPS - Ko-fi, the GitHub pill, and the disease-research copy
// beside the GOATPROJECT lockup - and it FILLS whatever width it is given
// (`.shell.is-open { width: 100% }`). So there is no natural width any more, only a floor:
// kSponsorCssMinWidth, the narrowest host at which the three groups still fit. Below it the row
// overflows its end edge and `.shell { overflow: hidden }` cuts the right-hand group off in
// silence, which is why WM_GETMINMAXINFO has to keep the window above it.
//
// The height does NOT vary with the width - every item in the row is the Ko-fi button's height
// and the row never wraps - so one constant is right at every window size.
//
// Both figures come from the generated header, where they were MEASURED by rendering the page
// at a range of widths (tools\measure-panel.py), so they track the plugin rather than a
// constant typed here. The caller reserves cy of height, gives the panel the full content width,
// and passes that rectangle to WebSponsorCreate and WebSponsorMove.
//
// Available WITHOUT creating anything: it reads two compile-time constants and does no
// LoadLibrary, so the layout can ask for it on any code path, including one where WebView2
// will turn out to be unavailable.
SIZE WebSponsorMinSize(int dpi);

// Called on the UI thread when creation finishes, exactly once per successful
// WebSponsorCreate. ok == false means WebView2 could not be used and the caller MUST show
// the GDI strip instead.
typedef void (*WebSponsorReadyFn)(void* user, bool ok);

// Begins creating a WebView2-hosted sponsor strip as a child of `parent`, at `rc` in the
// parent's client coordinates.
//
// Returns nullptr when the attempt could not even be STARTED - no loader DLL, no export, no
// host window. That is a synchronous failure and `ready` is never called; show the GDI strip
// immediately.
//
// A non-null result means creation is IN FLIGHT. WebView2's environment and controller are
// both created asynchronously, so the verdict arrives later through `ready`, on the UI
// thread, while the message loop runs. The returned object is valid immediately - it can be
// moved and destroyed before the verdict arrives.
WebSponsor* WebSponsorCreate(HWND parent, const RECT& rc, WebSponsorReadyFn ready, void* user);

// Move/resize. Safe at any time, including before the controller exists.
void WebSponsorMove(WebSponsor* w, const RECT& rc);

// Closes the controller, destroys the host window and releases every interface. Safe while
// creation is still in flight: the pending completion handlers observe the closed state and
// tear themselves down instead of touching freed memory.
void WebSponsorDestroy(WebSponsor* w);

// The host child window, or nullptr. Never assume it is visible - it is shown only once the
// page is actually rendering, so a failed creation never leaves a hole where the strip was.
HWND WebSponsorWindow(const WebSponsor* w);

}  // namespace cd
