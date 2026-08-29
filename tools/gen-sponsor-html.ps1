# =============================================================================
# SUPERSEDED 2026-08-29 - DO NOT RUN. Use:  python tools\gen-sponsor-html.py
# =============================================================================
# This script and gen-sponsor-html.py both wrote src\sponsor_html.h, and they did NOT produce
# the same page. Two generators for one committed file is a trap: whichever ran last won, and
# the header's own comment named the other one. It is kept for reference and disarmed.
#
# What this one produced, and why the .py replaced it:
#
#   * It REWROTE the plugin's stylesheet, renaming `:host` to `.gmdm-host` so the CSS would
#     work in a flat document. The .py attaches a REAL shadow root instead, exactly as the
#     plugin does, so not one selector of theirs is modified.
#   * It emitted {{URL_KOFI}} and {{URL_GITHUB}} placeholders that were never actually in the
#     markup - the Ko-fi and GitHub buttons are <button> elements with no href. The C++ then
#     "substituted" two placeholders that did not exist, so BOTH BUTTONS RENDERED PERFECTLY
#     AND DID NOTHING WHEN CLICKED. Measured. The .py bakes the URLs in and adds a click
#     listener on the shadow root, and fails loudly if they disagree with src\sponsor.h.
#   * It read four plugin files; the .py reads widget.js alone.
#
# Delete this file once nobody needs to look at what it used to do.
<#
  gen-sponsor-html.ps1 - build the sponsor strip's HTML from the PLUGIN'S OWN FILES.

  WHY THIS EXISTS
  ---------------
  The sponsor strip used to be a hand-port of 398 lines of CSS plus a large SVG into GDI
  drawing code. Re-implementing CSS transitions, cubic-bezier easing, blur filters, a radial
  mask-image and nine animated meteors in GDI is lossy by construction: no amount of iteration
  makes a re-implementation identical to the original. So the elements are now COPIED, not
  ported, and rendered by an embedded WebView2.

  This script is the copy step. It reads the four plugin files, extracts the stylesheet blocks
  and the two elements, and emits a C++ header holding the finished page. The header is
  committed, and the build uses the HEADER - the app never reads a path outside its own tree.

  RE-RUN IT WITH:
      powershell -NoProfile -ExecutionPolicy Bypass -File tools\gen-sponsor-html.ps1
  then rebuild. It rewrites src\sponsor_html.h in place and prints what it extracted.

  SOURCES (all four are the operator's own work):
      <plugin>\content\brand\goat-lockup-hover.css   the whole stylesheet, verbatim
      <plugin>\content\widget.js                     the <a class="goat-lockup"> element,
                                                     and the star SVG used by the GitHub button
      <plugin>\options\options.css                   the :root custom properties and .kofi rules
      <plugin>\options\options.html                  the Ko-fi <button> element

  WHAT THIS SCRIPT IS ALLOWED TO ADD
  ----------------------------------
  A single clearly-marked HOST BLOCK at the end of the <style>, and nothing else. The strip is
  a ~45px transparent band inside a Win32 card, not a browser tab, so it needs: no margins, no
  scrollbars, no background of its own, and one flex row. The two font sizes in that block are
  the host's scale knob and are substituted at RUNTIME (see the {{...}} placeholders below), so
  the C++ side owns the sizing and this file owns none of it.

  A tiny <script> is also emitted. It is behaviour, not style: it turns the plugin's Ko-fi
  <button> (which is a <button>, not a link) into something that navigates, so the host's
  NavigationStarting handler can cancel it and hand the URL to ShellExecuteW. Keeping the
  element a <button> is what lets it stay VERBATIM.

  PLACEHOLDERS the C++ host substitutes at runtime:
      {{URL_KOFI}} {{URL_GITHUB}} {{URL_GOAT}}   from src\sponsor.h (sponsor_url::*)
      {{LABEL_PX}} {{EM_PX}} {{GAP_PX}}          the scale knobs, in CSS px
#>

[CmdletBinding()]
param(
    [string]$Plugin = 'F:\google map plugin\extension',
    [string]$Out,
    # Optional: also write the finished page as a .html with the runtime placeholders
    # filled in, so the exact bytes the app will render can be opened in a browser.
    [string]$Preview
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

# SUPERSEDED - see the banner at the top of this file. It stops HERE rather than at the very
# top because a PowerShell script's param() block has to be the first executable statement,
# so an earlier guard turns the file into a parser error instead of a clear message.
Write-Host "gen-sponsor-html.ps1 is SUPERSEDED and will not run." -ForegroundColor Yellow
Write-Host "It rewrote the plugin's CSS and emitted URL placeholders that were never in the"
Write-Host "markup, so the Ko-fi and GitHub buttons rendered correctly and did nothing."
Write-Host ""
Write-Host "Use instead:  python tools\gen-sponsor-html.py"
exit 2

$repo = Split-Path -Parent $PSScriptRoot
if (-not $Out) { $Out = Join-Path $repo 'src\sponsor_html.h' }

$srcLockupCss  = Join-Path $Plugin 'content\brand\goat-lockup-hover.css'
$srcWidgetJs   = Join-Path $Plugin 'content\widget.js'
$srcOptionsCss = Join-Path $Plugin 'options\options.css'
$srcOptionsHtm = Join-Path $Plugin 'options\options.html'

foreach ($p in @($srcLockupCss, $srcWidgetJs, $srcOptionsCss, $srcOptionsHtm)) {
    if (-not (Test-Path -LiteralPath $p)) { throw "gen-sponsor-html: missing source file: $p" }
}

function Read-Text([string]$path) {
    (Get-Content -LiteralPath $path -Raw -Encoding UTF8) -replace "`r`n", "`n"
}

# ---------------------------------------------------------------------------
# A CSS block walker. Returns the top-level `selector { body }` rules in source
# order, comments and nested at-rule bodies skipped. DELIBERATELY not a CSS
# parser - it only has to be right about brace depth and /* */ comments, which
# is all these two stylesheets need.
# ---------------------------------------------------------------------------
function Get-TopLevelCssRules([string]$css) {
    $rules = New-Object System.Collections.ArrayList
    $i = 0
    $n = $css.Length
    $blockStart = 0        # where the current selector text began
    $depth = 0
    $braceAt = -1
    while ($i -lt $n) {
        if ($i + 1 -lt $n -and $css[$i] -eq '/' -and $css[$i + 1] -eq '*') {
            $end = $css.IndexOf('*/', $i + 2)
            if ($end -lt 0) { break }
            $i = $end + 2
            continue
        }
        $c = $css[$i]
        if ($c -eq '{') {
            if ($depth -eq 0) { $braceAt = $i }
            $depth++
        } elseif ($c -eq '}') {
            $depth--
            if ($depth -le 0) {
                $depth = 0
                if ($braceAt -ge 0) {
                    # The raw span carries any comment that preceded the rule, and that is kept
                    # in Text so the emitted stylesheet stays the author's. The SELECTOR used
                    # for matching has those comments stripped, or the first rule in a file
                    # would appear to be named after the file's own header comment.
                    $raw = $css.Substring($blockStart, $braceAt - $blockStart)
                    $sel = ($raw -replace '(?s)/\*.*?\*/', '').Trim()
                    $body = $css.Substring($braceAt, $i - $braceAt + 1)
                    [void]$rules.Add([pscustomobject]@{ Selector = $sel; Text = ($raw.Trim() + ' ' + $body) })
                }
                $braceAt = -1
                $blockStart = $i + 1
            }
        }
        $i++
    }
    return $rules
}

# ---------------------------------------------------------------------------
# Evaluate a JavaScript string-concatenation expression made only of
# single-quoted literals and known identifiers. Anything else is a hard error:
# a generator that silently drops a piece of somebody's markup is worse than
# one that stops.
# ---------------------------------------------------------------------------
function Invoke-JsConcat([string]$expr, [hashtable]$idents) {
    $out = New-Object System.Text.StringBuilder
    $i = 0
    $n = $expr.Length
    while ($i -lt $n) {
        $c = $expr[$i]
        if ($c -eq "'") {
            $i++
            while ($i -lt $n -and $expr[$i] -ne "'") {
                if ($expr[$i] -eq '\') {
                    $i++
                    if ($i -ge $n) { throw 'gen-sponsor-html: unterminated escape in JS literal' }
                    switch ($expr[$i]) {
                        'n' { [void]$out.Append("`n") }
                        't' { [void]$out.Append("`t") }
                        'r' { }
                        default { [void]$out.Append($expr[$i]) }
                    }
                } else {
                    [void]$out.Append($expr[$i])
                }
                $i++
            }
            if ($i -ge $n) { throw 'gen-sponsor-html: unterminated JS string literal' }
            $i++
        } elseif ($c -match '[A-Za-z_\$]') {
            $j = $i
            while ($j -lt $n -and $expr[$j] -match '[A-Za-z0-9_\$]') { $j++ }
            $name = $expr.Substring($i, $j - $i)
            if (-not $idents.ContainsKey($name)) {
                throw "gen-sponsor-html: unknown identifier '$name' in JS concatenation - refusing to guess"
            }
            [void]$out.Append([string]$idents[$name])
            $i = $j
        } elseif ($c -eq '+' -or $c -eq ' ' -or $c -eq "`t" -or $c -eq "`n" -or $c -eq '(' -or $c -eq ')') {
            $i++
        } else {
            throw "gen-sponsor-html: unexpected character '$c' in JS concatenation"
        }
    }
    return $out.ToString()
}

# ---------------------------------------------------------------------------
# 1. The lockup stylesheet - the whole file, verbatim.
# ---------------------------------------------------------------------------
$lockupCss = Read-Text $srcLockupCss
if ($lockupCss -notmatch '\.goat-lockup__meteors') {
    throw 'gen-sponsor-html: goat-lockup-hover.css does not contain the meteor layer - wrong file?'
}

# ---------------------------------------------------------------------------
# 2. options.css - the :root custom properties and every .kofi rule, verbatim.
# ---------------------------------------------------------------------------
$optionsCss = Read-Text $srcOptionsCss
$optRules = Get-TopLevelCssRules $optionsCss
$kept = New-Object System.Collections.ArrayList
foreach ($r in $optRules) {
    if ($r.Selector -eq ':root' -or $r.Selector -like '*.kofi*') { [void]$kept.Add($r.Text) }
}
if ($kept.Count -lt 6) {
    throw "gen-sponsor-html: expected at least 6 :root/.kofi rules in options.css, found $($kept.Count)"
}
$kofiCss = ($kept -join "`n`n")
if ($kofiCss -notmatch '--kofi:') { throw 'gen-sponsor-html: --kofi custom property not captured' }

# ---------------------------------------------------------------------------
# 3. options.html - the Ko-fi <button> element, verbatim except its data-url,
#    which becomes a placeholder so the URL keeps coming from src\sponsor.h.
# ---------------------------------------------------------------------------
$optionsHtml = Read-Text $srcOptionsHtm
$m = [regex]::Match($optionsHtml, '(?s)<button\b[^>]*class="kofi"[^>]*>.*?</button>')
if (-not $m.Success) { throw 'gen-sponsor-html: could not find the Ko-fi <button> in options.html' }
$kofiButton = $m.Value
$kofiButton = [regex]::Replace($kofiButton, 'data-url="[^"]*"', 'data-url="{{URL_KOFI}}"')
if ($kofiButton -notmatch 'kofi__handle') { throw 'gen-sponsor-html: Ko-fi button lost its handle line' }

# ---------------------------------------------------------------------------
# 4. widget.js - the star SVG and the goat lockup element.
# ---------------------------------------------------------------------------
$widget = Read-Text $srcWidgetJs

$goatUrlM = [regex]::Match($widget, "const\s+GOAT_URL\s*=\s*'([^']*)'")
if (-not $goatUrlM.Success) { throw 'gen-sponsor-html: GOAT_URL not found in widget.js' }

$starM = [regex]::Match($widget, "(?s)const\s+SVG_STAR\s*=(.*?);\n")
if (-not $starM.Success) { throw 'gen-sponsor-html: SVG_STAR not found in widget.js' }
$svgStar = Invoke-JsConcat $starM.Groups[1].Value @{}
if ($svgStar -notmatch '^<svg ') { throw 'gen-sponsor-html: SVG_STAR did not evaluate to an <svg>' }

# The lockup lives on one line, as a concatenation of literals and GOAT_URL. The href becomes
# a placeholder: the runtime substitutes sponsor_url::kGoatProject, so the three URLs in
# src\sponsor.h stay the single source of truth.
$lockLine = $null
foreach ($line in ($widget -split "`n")) {
    if ($line -match "'<a class=""goat-lockup""") { $lockLine = $line; break }
}
if (-not $lockLine) { throw 'gen-sponsor-html: the goat-lockup element was not found in widget.js' }
$lockExpr = $lockLine.Trim()
$lockExpr = [regex]::Replace($lockExpr, '\+\s*$', '')
$lockup = Invoke-JsConcat $lockExpr @{ 'GOAT_URL' = '{{URL_GOAT}}' }
if ($lockup -notmatch '^<a class="goat-lockup"') { throw 'gen-sponsor-html: lockup did not evaluate to an <a>' }
if ($lockup -notmatch 'gl-meteor|goat-lockup__meteors') { throw 'gen-sponsor-html: lockup lost its meteor layer' }
if (-not $lockup.EndsWith('</a>')) { throw 'gen-sponsor-html: lockup element is not closed' }

# ---------------------------------------------------------------------------
# 5. The GitHub star button. Per the brief it is "styled to match the Ko-fi
#    one", so it IS the Ko-fi element - the same .kofi classes, the same
#    geometry - carrying the plugin's own star SVG and its own two label lines.
#    Nothing about it is newly styled; every rule it uses came out of
#    options.css above.
# ---------------------------------------------------------------------------
$ghButton = $kofiButton
$ghButton = [regex]::Replace($ghButton, '(?s)<svg class="kofi__icon".*?</svg>',
                             ($svgStar -replace '^<svg ', '<svg class="kofi__icon" '))
$ghButton = $ghButton.Replace('id="kofi"', 'id="gh"')
$ghButton = $ghButton.Replace('{{URL_KOFI}}', '{{URL_GITHUB}}')
$ghButton = $ghButton.Replace('Support me on Ko-fi', 'Star on GitHub')
$ghButton = $ghButton.Replace('@IRP_HongKong', 'D-A-G-O-A-T/dagoat')
if ($ghButton -notmatch 'Star on GitHub') { throw 'gen-sponsor-html: GitHub label substitution failed' }
if ($ghButton -match 'kofi__icon" viewBox="0 0 24 24" width="20"') {
    throw 'gen-sponsor-html: the Ko-fi cup was not replaced by the star'
}

# ---------------------------------------------------------------------------
# 6. Assemble the page.
# ---------------------------------------------------------------------------
$hostBlock = @'
/* ==========================================================================
   HOST BLOCK - the ONLY rules on this page that did not come from the plugin.
   The strip is a transparent band inside a Win32 card, not a browser tab: no
   margin, no scrollbars, no background of its own, and one right-aligned flex
   row. The three substituted values are the host's scale knob and are the only
   sizing this page does.

   They are expressed against 100vh rather than in fixed px ON PURPOSE. The
   viewport IS the strip: whatever height the host gives this window, the type
   scales to it. That makes the page correct whether or not the WebView2
   controller's rasterization scale happens to follow the monitor DPI - a fixed
   px value would be right on one of those two paths and wrong on the other,
   and the wrong one is invisible until somebody runs at 150%.
   ========================================================================== */
html,body{margin:0;overflow:hidden;background:transparent}
body{font-family:system-ui,-apple-system,"Segoe UI",sans-serif;font-size:{{LABEL_PX}};line-height:1.4}
.strip{display:flex;align-items:center;justify-content:flex-end;gap:{{GAP_PX}};height:100vh}
.goat-lockup{font-size:{{EM_PX}}}
'@

$script = @'
/* Behaviour, not style. The plugin's Ko-fi control is a <button>, which is why it could be
   copied verbatim - but a <button> does not navigate. This turns a click into a navigation
   the host's NavigationStarting handler then CANCELS, handing the URL to ShellExecuteW so
   the link opens in the user's real browser and never inside this view. */
document.addEventListener('click', function (e) {
  var el = e.target && e.target.closest ? e.target.closest('[data-url]') : null;
  if (!el) return;
  e.preventDefault();
  location.href = el.getAttribute('data-url');
}, true);
'@

$sb = New-Object System.Text.StringBuilder
[void]$sb.Append("<!doctype html>`n<html><head><meta charset=`"utf-8`">`n<style>`n")
[void]$sb.Append("/* ---- options.css: :root custom properties and the .kofi rules, verbatim ---- */`n")
[void]$sb.Append($kofiCss)
[void]$sb.Append("`n`n/* ---- brand/goat-lockup-hover.css, verbatim and entire ---- */`n")
[void]$sb.Append($lockupCss)
[void]$sb.Append("`n")
[void]$sb.Append($hostBlock)
[void]$sb.Append("</style></head>`n<body><div class=`"strip`">`n")
[void]$sb.Append($kofiButton)
[void]$sb.Append("`n")
[void]$sb.Append($ghButton)
[void]$sb.Append("`n")
[void]$sb.Append($lockup)
[void]$sb.Append("`n</div>`n<script>`n")
[void]$sb.Append($script)
[void]$sb.Append("`n</script></body></html>`n")
$page = $sb.ToString()

foreach ($ph in @('{{URL_KOFI}}', '{{URL_GITHUB}}', '{{URL_GOAT}}', '{{LABEL_PX}}', '{{EM_PX}}', '{{GAP_PX}}')) {
    if ($page -notmatch [regex]::Escape($ph)) { throw "gen-sponsor-html: placeholder $ph is missing from the page" }
}

# ---------------------------------------------------------------------------
# 7. Escape to C++ and write the header.
#
#    Non-ASCII is written as a \uXXXX universal character name so the header is
#    pure ASCII and does not depend on MSVC guessing the file's encoding. A UCN
#    below U+00A0 is ill-formed C++, so the handful of control characters that
#    can legitimately appear are written as their own escapes instead.
# ---------------------------------------------------------------------------
function ConvertTo-CppLiteralBody([string]$s) {
    $sb = New-Object System.Text.StringBuilder
    foreach ($ch in $s.ToCharArray()) {
        $code = [int][char]$ch
        if ($ch -eq '\') { [void]$sb.Append('\\') }
        elseif ($ch -eq '"') { [void]$sb.Append('\"') }
        elseif ($code -eq 9) { [void]$sb.Append('\t') }
        elseif ($code -ge 32 -and $code -le 126) { [void]$sb.Append($ch) }
        elseif ($code -ge 0xA0 -and $code -lt 0xD800) { [void]$sb.AppendFormat('\u{0:x4}', $code) }
        elseif ($code -gt 0xDFFF) { [void]$sb.AppendFormat('\u{0:x4}', $code) }
        else { throw ("gen-sponsor-html: character U+{0:X4} cannot be written as a C++ escape" -f $code) }
    }
    return $sb.ToString()
}

$lines = $page -split "`n"
$parts = New-Object System.Collections.ArrayList
for ($i = 0; $i -lt $lines.Count; $i++) {
    $body = ConvertTo-CppLiteralBody $lines[$i]
    if ($i -lt $lines.Count - 1) { $body += '\n' }
    if ($body -eq '') { continue }
    [void]$parts.Add('    L"' + $body + '",')
}

$stamp = (Get-Date).ToString('yyyy-MM-dd')
$header = New-Object System.Text.StringBuilder
[void]$header.Append(@"
// ===========================================================================
// GENERATED FILE - DO NOT EDIT BY HAND.
//
// Produced by tools\gen-sponsor-html.ps1 on $stamp from the plugin's OWN files:
//   $srcOptionsCss
//   $srcOptionsHtm
//   $srcLockupCss
//   $srcWidgetJs
//
// Re-run with:
//   powershell -NoProfile -ExecutionPolicy Bypass -File tools\gen-sponsor-html.ps1
//
// The app never reads those paths at runtime - it uses this header - so the
// binary does not depend on a directory outside its own tree. Every rule and
// both elements below are the plugin's, verbatim, apart from one clearly
// marked HOST BLOCK and one small behaviour script; see the generator's own
// header comment for why each exists.
//
// {{URL_KOFI}} {{URL_GITHUB}} {{URL_GOAT}} are substituted at runtime from
// cd::sponsor_url in src\sponsor.h. {{LABEL_PX}} {{EM_PX}} {{GAP_PX}} are the
// host's scale knobs, in CSS px.
// ===========================================================================
#pragma once
#include <string>

namespace cd {

// The page, split one C++ string literal per source line: MSVC caps a single
// literal at 65535 bytes (C2026) and this page is comfortably past that.
inline void AppendSponsorHtml(std::wstring& out) {
    static const wchar_t* const kParts[] = {

"@)
foreach ($p in $parts) { [void]$header.Append($p + "`n") }
[void]$header.Append(@"
    };
    for (size_t i = 0; i < sizeof(kParts) / sizeof(kParts[0]); ++i) out += kParts[i];
}

}  // namespace cd
"@)

$utf8NoBom = New-Object System.Text.UTF8Encoding($false)
[System.IO.File]::WriteAllText($Out, ($header.ToString() -replace "`r`n", "`r`n"), $utf8NoBom)

if ($Preview) {
    # The same numbers src\webview_host.cpp substitutes; see kLabelPx / kEmPx / kGapPx there.
    $prev = $page.Replace('{{URL_KOFI}}',   'https://ko-fi.com/irp_hongkong').
                  Replace('{{URL_GITHUB}}', 'https://github.com/D-A-G-O-A-T/dagoat').
                  Replace('{{URL_GOAT}}',   'https://dagoat.io').
                  Replace('{{LABEL_PX}}',   'calc(100vh * 0.22222)').
                  Replace('{{EM_PX}}',      'calc(100vh * 0.16872)').
                  Replace('{{GAP_PX}}',     'calc(100vh * 0.17778)')
    [System.IO.File]::WriteAllText($Preview, $prev, (New-Object System.Text.UTF8Encoding($false)))
    Write-Host "gen-sponsor-html: preview written to $Preview"
}

Write-Host "gen-sponsor-html: wrote $Out"
Write-Host ("  options.css rules kept : {0}" -f $kept.Count)
Write-Host ("  lockup css chars       : {0}" -f $lockupCss.Length)
Write-Host ("  kofi button chars      : {0}" -f $kofiButton.Length)
Write-Host ("  github button chars    : {0}" -f $ghButton.Length)
Write-Host ("  lockup element chars   : {0}" -f $lockup.Length)
Write-Host ("  page chars             : {0}" -f $page.Length)
Write-Host ("  string literals emitted: {0}" -f $parts.Count)
Write-Host ("  plugin GOAT_URL        : {0} (placeholdered)" -f $goatUrlM.Groups[1].Value)
