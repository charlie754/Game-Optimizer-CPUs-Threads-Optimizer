# Notices and attribution

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
Every pixel here is drawn by `src\theme.cpp` with plain GDI, written for this project.

Because no Apache-2.0 material is redistributed, that licence imposes no obligation on this
repository. **The credit above is given because it is deserved, not because it is required.**
If any FreeToken code or asset is ever incorporated, Apache-2.0 §4 then applies in full —
retain the licence and NOTICE, state changes, and preserve attribution notices.

Colour values, spacing and typography were chosen for this app and are recorded as design
tokens in `src\theme.h`; they are not copied from FreeToken's stylesheets.

## Microsoft WebView2 loader — a vendored binary

`third_party\webview2\x64\WebView2Loader.dll` is **Microsoft's redistributable WebView2 loader**.
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
`Microsoft.Web.WebView2` SDK package. **That licence text is NOT reproduced anywhere in this
repository**, and no copy of the SDK's own `LICENSE.txt` or `NOTICE.txt` is present here.
`third_party\webview2\README.md` records which package version this binary came from, shows
that it is byte-identical to Microsoft's published copy, and links to Microsoft's terms for it.
Anyone shipping a build of this repository should read those terms and satisfy them, rather
than treating this paragraph as a licence review.

**How to remove it.** Delete `third_party\webview2\` and rebuild. `tools\build.bat` prints a
warning instead of failing, and the app falls back to the GDI sponsor strip at run time. No
source change is needed, because the DLL is loaded with `LoadLibraryW` and is not in the exe's
import table.

## The sponsor panel's HTML, CSS and SVG — the operator's own work

`src\sponsor_html.h` is a **generated file**. Its content — the panel stylesheet, the animated
GOATPROJECT lockup, the Ko-fi cup and the GitHub star — is **the operator's own work**, taken
from their browser extension, the *google map plugin*, on this machine:

```
F:\google map plugin\extension\content\widget.js
```

It is **not third-party material**, and it is not licensed from anyone: the author of this
repository is the author of that panel. It is recorded here because a generated file whose
content came from another project should say where it came from, not because a licence
requires it.

**Why it is copied rather than re-implemented.** The panel was first hand-ported into GDI
drawing code. Re-expressing CSS transitions, cubic-bezier easing, blur filters, a radial
`mask-image` and nine animated meteors as drawing calls is lossy by construction, and the
result was rejected for not matching. So the elements are now copied verbatim and rendered by
the engine they were written for. Nothing of the original is rewritten — not one selector,
not one path.

**How to re-generate it.**

```
python tools\gen-sponsor-html.py
```

That is the only thing in this repository that ever reads the plugin path. **The application
never does**: the generated header is compiled in, and the running app opens no file outside
its own installation. The generator also fails loudly if the three destination URLs in the
page ever disagree with the constants in `src\sponsor.h`.

The panel's natural size is measured rather than estimated, by `tools\measure-panel.py`.

## Other third-party code in this repository

**None.** Apart from the loader above, Game Optimizer links only against the Windows SDK import
libraries shipped with the platform toolchain (`user32`, `shell32`, `gdi32`, `advapi32`,
`comctl32`, `ole32`, `shlwapi`, `psapi`, `comdlg32`, `msimg32`). There is no vendored source,
no package manager, and no bundled image asset — the tray icons are drawn with GDI at runtime.

## The sponsor strip's markup and stylesheets

`src\sponsor_html.h` is generated by `tools\gen-sponsor-html.ps1` and contains, verbatim, the
`.kofi` rules and `:root` custom properties from `options.css`, the Ko-fi `<button>` element
from `options.html`, the whole of `brand\goat-lockup-hover.css`, and the
`<a class="goat-lockup">` element and star SVG from `widget.js` — all four files being the
**operator's own** browser extension. They are reproduced here with the author's instruction to
do so. Nothing in that generated file is third-party work.
