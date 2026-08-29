# WebView2Loader.dll — a Microsoft redistributable, vendored here

`x64\WebView2Loader.dll` in this directory is **Microsoft's redistributable WebView2 loader**.
It is not Game Optimizer's code and not the author's work. It is checked in so the app can be
built and run without fetching anything.

## What it is

A loader stub. Its whole job is to find the Microsoft Edge WebView2 Runtime installed on the
machine and export `CreateCoreWebView2EnvironmentWithOptions`, so an application can ask that
runtime for a browser control.

**It contains no browser engine.** The engine is the separately installed WebView2 Runtime,
which this repository does not contain and does not redistribute. If that runtime is absent the
loader finds nothing — which is one reason Game Optimizer treats the whole component as
optional. When it fails, the Settings window shows the GDI-drawn sponsor strip instead and the
app carries on. See the WebView2 section of `README.md` at the repository root.

## What Game Optimizer uses it for

The sponsor strip at the bottom of the Settings window, and nothing else.

It is loaded with `LoadLibraryW` at run time and is deliberately **not** in the exe's import
table, so a missing or deleted copy can never stop the app starting. `tools\build.bat` copies it
next to `GameOptimizer.exe` if it is here and prints a warning if it is not; it never fails the
build over it. Deleting this whole directory and rebuilding is a supported thing to do.

Because the build copies it next to the exe, **a distributed build of Game Optimizer contains
this DLL** — which is why the licence section below matters rather than being a formality.

## Which build this is, and where it came from

| | |
|---|---|
| File | `third_party\webview2\x64\WebView2Loader.dll` |
| Size | 160,320 bytes |
| SHA-256 | `8427b1fc58ec707813e5c0a51eb5d69397bb333250a7b891be4d3b123f1e0f1c` |
| PE file version | 1.0.3650.58 |
| PE company / description | Microsoft Corporation — "Microsoft Edge Embedded Browser WebView Loader" |
| Microsoft SDK package | `Microsoft.Web.WebView2` 1.0.3650.58 |
| Recorded | 2026-08-29 |

The copy here was taken from the `webview2-com-sys` crate's vendored binaries on the development
machine — `webview2-com-sys-0.38.2`, `x64\WebView2Loader.dll`. The exact path is recorded in
`NOTICE.md`.

That is provenance by hearsay, so it was checked against what Microsoft actually publishes. The
`Microsoft.Web.WebView2` 1.0.3650.58 package was downloaded from nuget.org, and both copies of
the 64-bit loader inside it — `build\native\x64\WebView2Loader.dll` and
`runtimes\win-x64\native\WebView2Loader.dll` — hash to the SHA-256 above. They are
**byte-identical to the file in this directory**. So this is Microsoft's published
redistributable at that exact version, unmodified, and the crate was only the delivery route.

The crate ships no licence file for the binary at all; its own MIT licence covers the Rust
bindings it publishes, not Microsoft's DLL.

## Licence

Use and redistribution of this binary are governed by **Microsoft's licence terms for the
WebView2 SDK**. Not by Game Optimizer's MIT licence, and not by the `webview2-com-sys` crate's.

🔴 **THEY ARE NOW REPRODUCED HERE, AND THEY HAD TO BE — see `LICENSE.txt` beside this file.**

Microsoft's terms for this package are **not** a "MICROSOFT SOFTWARE LICENSE TERMS" EULA, which
is what a licence link would have quietly implied. They are **BSD-3-Clause-shaped**, and the
second condition is explicit:

> *Redistributions in binary form must reproduce the above copyright notice, this list of
> conditions and the following disclaimer in the documentation and/or other materials provided
> with the distribution.*

**This repository redistributes the binary**, and `tools\build.bat` copies it next to
`GameOptimizer.exe`, so any release build carries it too. On a plain reading that condition is
triggered, and **a hyperlink does not discharge it.** So Microsoft's own files are committed
here verbatim — copied, never retyped:

| file | bytes | sha256 |
|---|---|---|
| `LICENSE.txt` | 1,487 | `0af8f1b807512aae39c2ac1aa4d0cae65cabecb6fd554b8439a5162a0d6eca55` |
| `NOTICE.txt` | 3,894 | from the same package |

**Honest limit:** what was verified is the licence the NuGet package itself declares for version
**1.0.3650.58**. Whether a separate Microsoft SDK agreement additionally applies has **not** been
established. **[A]**

The paragraph below is kept because its reasoning still stands for anything else vendored here.

**A licence must never be reproduced from memory.** That is how a repository
ends up shipping the wrong one, so this file links to the authoritative source instead:

- The licence for the exact package this file came from —
  <https://www.nuget.org/packages/Microsoft.Web.WebView2/1.0.3650.58/license>
- The package itself — <https://www.nuget.org/packages/Microsoft.Web.WebView2>
- WebView2 documentation — <https://learn.microsoft.com/en-us/microsoft-edge/webview2/>
- Distribution guidance — <https://learn.microsoft.com/en-us/microsoft-edge/webview2/concepts/distribution>

That SDK package carries its own `LICENSE.txt` (1,487 bytes) and `NOTICE.txt` (3,894 bytes).
**Neither is copied into this repository.**

This page is provenance, not a licence review, and it is not legal advice. If you redistribute a
build that includes this DLL, read Microsoft's terms at the first link and satisfy them yourself
rather than relying on anything written here.

Every link above was fetched on 2026-08-29 and returned HTTP 200.
