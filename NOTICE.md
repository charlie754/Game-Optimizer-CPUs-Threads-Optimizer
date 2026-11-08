# Notices and attribution

## 2026-11-07 — 0.6.1 sponsor strip compatibility fix

The Ko-fi and Star controls retain the 0.5.9 markup, CSS, and geometry. The GOAT Cam caption is
right-aligned, and the banner stage grows on hover while keeping its viewfinder content inside the
plate in both the WebView and GDI fallback renderers.

## 2026-11-07 — signed sponsor content

The sponsor strip now loads signed promotional content from `goatcam.dagoat.io` when available
and falls back to the built-in Goat Project strip. The compiled-in public key has keyId
`220c87522a4b7cbf`. What is requested, when, and how to stop it is described in `README.md`.

**This supersedes the two "Nothing is fetched" statements in the older sections below.** The
coffee photo is no longer compiled into the app: it arrives as part of the signed online
banner. The only image still compiled in for the banner is a small preview of the GOAT Cam
iPhone app's screen, shown inside the phone outline; it is the operator's own work. The
CC0 coffee photo (credited below) is still in this repository as a test fixture under
`tests\banner-vectors\photos\`.

## CPUSetSetter — the inspiration, not a dependency

Game Optimizer was inspired by **[CPUSetSetter](https://github.com/SimonvBez/CPUSetSetter)** by
SimonvBez, which demonstrated that Windows CPU Sets are the right API for steering games onto a
particular CCD.

**Licence check performed 2026-08-28.** Fetched from
`raw.githubusercontent.com/SimonvBez/CPUSetSetter/master/LICENSE`, which reads:

> MIT License
>
> Copyright (c) 2025 Simon

MIT permits forking, derivative works and redistribution, including in closed form, provided
the copyright notice and permission notice accompany any substantial portion of the original.
**So forking was permitted.** It was not chosen.

## Why this is a reimplementation rather than a fork

1. The feature set diverges. Profiles pairing a game with a "heavy apps" list, transitive
   descendant tracking, a foreground CPU-percentage rule and a crash-restore journal are not
   the reference project's model.
2. CPUSetSetter is C# and requires the .NET Desktop Runtime 10. Game Optimizer is a single native
   exe with no *required* runtime dependency — WebView2 is optional and only decorates the
   sponsor panel, and the app starts and works without it — which is a real distribution
   difference and is unreachable while forking a C# codebase.
3. Nothing needed copying. The shared substrate is the Windows CPU Sets API, which is
   Microsoft's, not either project's.

**No code from CPUSetSetter appears in this repository.** Every file here was written against
Microsoft's documentation, the Windows SDK headers on disk, and measurements taken on the
development machine.

## If that ever changes

Reproducing the notice above is currently a **courtesy**, because no code is reused. The moment
any code is taken from CPUSetSetter, MIT makes reproducing its copyright and permission notice
**mandatory** in this repository and in any binary distribution.

This paragraph exists so a future contributor cannot get that wrong by assuming the attribution
already on this page is sufficient for a situation it was not written for.

## FreeToken — visual language, and nothing else

Game Optimizer's dark card-based interface — the left sidebar rail, rounded surfaces, stat
cards, ring gauges, pill badges, and the use of monospace for anything numeric or technical —
is modelled on **[FreeToken Desktop](https://github.com/FlashML-org/FreeToken)** by FlashML,
which is licensed **Apache-2.0**.

**What was taken:** a visual approach. Layout conventions, the dark palette structure, and the
sans-versus-monospace contrast.

**What was not taken:** any source code, any asset, the logo, the name, or any branding.
FreeToken is a Python Mixture-of-Experts serving engine; Game Optimizer is a native C++/Win32
tray app. They share no code and could not — there is no common runtime, framework or file.
The application UI is drawn by `src\theme.cpp` with plain GDI, written for this project. (The
sponsor strip in the Settings window is the exception, and it is the author's own work — see
below — not FreeToken's.)

Because no Apache-2.0 material is redistributed, that licence imposes no obligation on this
repository. **The credit above is given because it is deserved, not because it is required.**
If any FreeToken code or asset is ever incorporated, Apache-2.0 §4 then applies in full —
retain the licence and NOTICE, state changes, and preserve attribution notices.

Colour values, spacing and typography were chosen for this app and are recorded as design
tokens in `src\theme.h`; they are not copied from FreeToken's stylesheets.

## Microsoft WebView2 loader — a vendored binary

`third_party\webview2\x64\WebView2Loader.dll` is **Microsoft's redistributable WebView2 loader**.
(A downloaded release carries the same file as top-level `WebView2Loader.dll` beside
`GameOptimizer.exe`.)
It is the small stub whose only job is to find the installed Edge WebView2 runtime and export
`CreateCoreWebView2EnvironmentWithOptions`. It contains no browser; the browser is the
separately-installed WebView2 Runtime, which this repository does not redistribute.

**What it is used for.** The sponsor strip in the Settings window, and nothing else. See the
WebView2 section of `README.md`.

**Where this copy came from.** It was taken from the `webview2-com-sys` crate's vendored copy
on the development machine, at

```
D:\cargo\registry\src\index.crates.io-1949cf8c6b5b557f\webview2-com-sys-0.38.2\x64\WebView2Loader.dll
```

That crate vendors Microsoft's own redistributable unmodified; the bytes here are a byte-for-byte
copy of that file.

| | |
|---|---|
| Version | 1.0.3650.58 — the loader's PE file version, and the version of the `Microsoft.Web.WebView2` package whose x64 loader hashes to the same SHA-256 |
| Size | 160,320 bytes |
| SHA-256 | `8427b1fc58ec707813e5c0a51eb5d69397bb333250a7b891be4d3b123f1e0f1c` |
| Recorded | 2026-08-29 |

**Licence.** Microsoft distributes the WebView2 SDK, including this loader, for redistribution
with applications that use WebView2, under the licence terms Microsoft publishes for the
`Microsoft.Web.WebView2` SDK package. That licence text is reproduced in this repository and in
the shipped package as `third_party\webview2\LICENSE.txt` and
`third_party\webview2\NOTICE.txt`.
`third_party\webview2\README.md` records which package version this binary came from, shows
that it is byte-identical to Microsoft's published copy, and links to Microsoft's terms for it.
Anyone shipping a build of this repository should read those terms and satisfy them, rather
than treating this paragraph as a licence review.

**How to remove it.** Delete `third_party\webview2\` and rebuild. In a downloaded release, the
DLL is the top-level `WebView2Loader.dll` beside `GameOptimizer.exe`; deleting that DLL removes
the dependency, and the app then falls back to the GDI sponsor strip. `tools\build.bat` prints a
warning instead of failing, and the app falls back to the GDI sponsor strip at run time. No
source change is needed, because the DLL is loaded with `LoadLibraryW` and is not in the exe's
import table.

## The sponsor panel's HTML, CSS and SVG — the operator's own work

`src\sponsor_html.h` is a **generated file**. Its content — the panel stylesheet, the Ko-fi cup,
the GitHub star, and the **GOAT Cam** banner (its stylesheet, its headline, the "GOAT Cam"
wordmark and tagline, the viewfinder, and the App Store QR code) — is **the operator's own work**,
taken from their browser extension, the *google map plugin*, on this machine:

```
F:\google map plugin\extension\content\widget.js
```

The current header was generated on 2026-10-26 (banner v2) from the extension's
`goatcam-banner` worktree,
`F:\google map plugin\.claude\worktrees\goatcam-banner\extension\content\widget.js`. In that
file, `GOAT_LOCKUP_CSS` is a comment-stripped copy of the GOAT Cam repository's master
stylesheet, `brand\goatcam-banner\goatcam-banner.css`. Game Optimizer draws it as the master's
`goat--strip` variant.

The page also runs one script of the banner's, also the operator's own work:
`goatcam-qr-bridge.js` from the same GOAT Cam folder, inlined verbatim. While the pointer is on
the QR it reports the tile's position to the app, which shows its own enlarged QR in a native
popup.

Apart from the photo described below, it is **not third-party material**, and it is not
licensed from anyone: the author of this repository is the author of that panel. It is recorded
here because a generated file whose content came from another project should say where it came
from, not because a licence requires it.

**The banner's photo is third-party, and it is public domain.** The page embeds the same CC0
photo as the GDI strip (*White cup and MacBook* by Alex Knight; licence and evidence in the next
section) as a `data:image/webp` URI. It is the GOAT Cam site's own
`site\public\assets\hero-coffee-800.webp`, 800 × 498, 21,038 bytes, the same uncropped photo as
the GDI strip's JPEG. The embedded bytes are identical to that file.

**Nothing is fetched.** The photo is a data URI and the QR is inline SVG. The generator
refuses to write a page that carries any absolute URL other than the three destinations (Ko-fi,
the GitHub repository and `https://goatcam.dagoat.io`).

**Why it is copied rather than re-implemented.** The panel was first hand-ported into GDI
drawing code. Re-expressing CSS transitions, cubic-bezier easing, blur filters, a radial
`mask-image` and nine animated meteors as drawing calls is lossy by construction, and the
result was rejected for not matching. So the elements are now copied verbatim and rendered by
the engine they were written for. Nothing of the original is rewritten — not one selector,
not one path.

**How to re-generate it.**

```
python tools\gen-sponsor-html.py
python tools\gen-sponsor-html.py --plugin-dir <extension checkout>
```

Use the second form while the GOAT Cam banner exists only in a worktree of the extension. The
generator refuses an extension whose `widget.js` still carries the GOATPROJECT banner, or the v1
GOAT Cam banner's ask line or DaGoat lockup. It reads `goatcam-qr-bridge.js` from the GOAT Cam
checkout (`--bridge-js <file>` to point elsewhere).

That is the only thing in this repository that ever reads the plugin path. **The application
never does**: the generated header is compiled in, and the app never reads the plugin checkout.
(Since 0.6.0 it does keep a banner cache in `%LOCALAPPDATA%\GameOptimizer\banner` — see the
2026-11-07 entry at the top.) The generator also fails loudly if the three destination URLs in the
page ever disagree with the constants in `src\sponsor.h`.

The panel's natural size is measured rather than estimated, by `tools\measure-panel.py`.

## The GOAT Cam banner in the GDI sponsor strip — and its coffee photo

When WebView2 is unavailable, the Settings window draws the sponsor strip itself
(`src\sponsor.cpp`). Its third item is the **GOAT Cam** banner: a headline beside a plate that
a coffee photo fills edge to edge, with the GOAT Cam wordmark, a viewfinder and an App Store QR
code; hovering the QR enlarges it in a small popup (`src\qr_magnifier.cpp`). Its design, its
copy and the QR matrix are **the operator's own work**, from the GOAT Cam repository
(`brand\goatcam-banner\`, SPEC section 7), and its drawing code was lifted from the operator's
own MacFanControl port of the same banner. None of it is third-party material.

**The photo is third-party, and it is public domain.** `src\goatcam-coffee.jpg` is embedded in
the executable as the resource `GOATCAM_COFFEE`:

| | |
|---|---|
| Title | White cup and MacBook |
| Photographer | Alex Knight — https://unsplash.com/@agkdesign |
| Source page | https://unsplash.com/photos/j4uuKnN43_M |
| Licence | Creative Commons CC0 1.0 Universal Public Domain Dedication — https://creativecommons.org/publicdomain/zero/1.0/ |
| Licence evidence | Unsplash published under CC0 until 5 June 2017; an archived copy of the page from 2017-04-25 names the photographer and shows the image: https://web.archive.org/web/20170425102516/https://unsplash.com/photos/j4uuKnN43_M — Wikimedia Commons records the same licence: https://commons.wikimedia.org/wiki/File:White_cup_and_MacBook_(Unsplash).jpg |
| Changes | resized to 800 px wide, a warm colour grade, all embedded metadata removed — this is the GOAT Cam site's own `site\public\assets\hero-coffee-800.jpg`, byte for byte, not re-cropped |
| This file | 800 × 498 JPEG, 41,289 bytes, SHA-256 `e84ccbe2459a621955d699130789191715a77364754cb52097331238a7fed819` |

These facts are taken from the GOAT Cam repository's
`brand\goatcam-banner\assets\CREDITS.txt`, not re-verified against the source pages here. CC0
does not require attribution; the photographer is credited anyway, with thanks.

**Nothing is fetched.** The photo is compiled in; drawing the strip opens no file and no URL.

## NVIDIA NVAPI — struct layouts and interface ids, not code

`src\gpu_cuda.cpp` talks to the NVIDIA display driver's settings database (DRS) to set which GPU
CUDA may use for one application. To do that it must agree with the driver's ABI: the layout of
`NVDRS_SETTING`, `NVDRS_SETTING_VALUES`, `NVDRS_PROFILE` and `NVDRS_APPLICATION`, the numeric
interface ids that `nvapi_QueryInterface` answers to, and the setting id `CUDA_EXCLUDED_GPUS_ID`
(0x10354FF8).

**Where those facts come from:** NVIDIA's own public headers at
[github.com/NVIDIA/nvapi](https://github.com/NVIDIA/nvapi) (`nvapi.h`,
`NvApiDriverSettings.h`), which are licensed **MIT**:

> Copyright (c) NVIDIA Corporation. All rights reserved.
>
> Permission is hereby granted, free of charge, to any person obtaining a copy of this software
> and associated documentation files (the "Software"), to deal in the Software without
> restriction, including without limitation the rights to use, copy, modify, merge, publish,
> distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the
> Software is furnished to do so, subject to the following conditions:
>
> The above copyright notice and this permission notice shall be included in all copies or
> substantial portions of the Software.

**What was taken:** the ABI facts above — structure layouts, field order, version constants and
interface ids. They are re-declared in `src\gpu_cuda.cpp` in this project's own style, with a
`static_assert` on every structure size, rather than vendored: **no NVIDIA header file is present
in this repository and none is included by the build.** Nothing links against `nvapi.lib`;
`nvapi64.dll` is opened at run time with `LoadLibraryEx` and is entirely optional, so the
application starts and works on a machine with no NVIDIA driver at all.

The notice above is reproduced because the ABI description is a substantial enough borrowing that
reproducing it is the safe reading of the licence, not because any NVIDIA source file was copied.

## Other third-party code in this repository

**None.** Apart from the WebView2 loader and the NVAPI ABI facts described above, Game Optimizer
links only against the Windows SDK import
libraries shipped with the platform toolchain (`user32`, `shell32`, `gdi32`, `advapi32`,
`comctl32`, `ole32`, `shlwapi`, `psapi`, `comdlg32`, `msimg32`, and `gdiplus`, delay-loaded
for the GDI sponsor strip). There is no vendored source and no package manager. The only bundled
image is the CC0 coffee photo described above. It ships twice: as a JPEG resource for the GDI
strip, and as a webp data URI inside the generated web page. The tray icons are drawn with GDI
at runtime.

## The sponsor strip's markup and stylesheets

`src\sponsor_html.h` is generated by `tools\gen-sponsor-html.py`. Nearly all of it comes from the
extension's `widget.js`; the bridge comes from the GOAT Cam folder. It contains these, verbatim:

- the panel's CSS template;
- `GOAT_LOCKUP_CSS`, the master GOAT Cam stylesheet;
- the Ko-fi and Star buttons, with their cup and star SVGs;
- the GOAT Cam banner's markup: the headline, and the plate with the photo, the wordmark and
  the QR;
- `goatcam-qr-bridge.js`, the banner's QR-magnifier bridge.

Two things are not verbatim:

- Game Optimizer's own layout rules, appended after theirs;
- one rewritten class attribute, `goat goat--panel goat--gated` becoming `goat goat--strip`.

All of it is the **operator's own** work, reproduced here with the author's instruction to do so.
The one exception is the CC0 photo described above. (The superseded `tools\gen-sponsor-html.ps1`
read four extension files, `options.css`, `options.html`, `goat-lockup-hover.css` and
`widget.js`. It exits without running and generates nothing.)
