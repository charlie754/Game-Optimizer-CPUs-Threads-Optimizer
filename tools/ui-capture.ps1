# Game Optimizer - repeatable UI capture harness.
#
# The bare identifiers below - the process name and every window class - are NOT display text
# and must stay unspaced: they are matched against what the app actually registers.
#
# Why this exists: the Settings window shipped with a scroll repaint bug and a core map that
# was never created at all. The build was clean, 336 unit tests passed and Gate B was 4/4,
# because none of them draws a pixel. Both defects were found by a human looking at the
# window. This script makes that observation repeatable.
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\ui-capture.ps1 -OutDir <dir> -Tag before
#
# TWO INSTRUMENT BUGS ALREADY BIT ONCE EACH. Do not reintroduce them.
#
# 1. CharSet. Every P/Invoke below declares CharSet=CharSet.Unicode. Without it .NET marshals
#    StringBuilder as ANSI into a ...W function and every class name comes back truncated to
#    its first character - which produced a convincing false report of a wide/ANSI bug in the
#    app when the app was correct.
#
# 2. PrintWindow, not CopyFromScreen. Graphics.CopyFromScreen copies whatever pixels are at
#    those screen coordinates. If the target window is not on top it silently captures the
#    user's other windows instead - it captured a browser once and that image then had to be
#    deleted. PrintWindow(PW_RENDERFULLCONTENT) asks the window to render ITSELF, so z-order,
#    occlusion and focus are irrelevant, and it can never capture anyone else's content.

param(
    [string]$OutDir = "$env:TEMP\gameoptimizer-ui",
    [string]$Tag    = "run",
    # The Settings window has four tabs since v0.5.6. The tab bar stops at the last tab instead of
    # wrapping, so a larger number only repeats the last capture.
    [int]$Pages     = 4,
    # The binary whose running instance is captured. Only a GameOptimizer process started from exactly this path is
    # used: an installed release of another version would otherwise be captured and labelled as this build.
    [string]$ExePath = (Join-Path $PSScriptRoot '..\build\GameOptimizer.exe')
)

$ErrorActionPreference = 'Stop'

# NO RUN ENDS 0 WITH A FAILURE IN IT (v0.5.6). A capture PrintWindow refused printed nothing, a page whose heading could
# not be read printed "UNKNOWN", a missing page selector printed a line - and each run still exited 0. Each now counts,
# and the run ends "RESULT: FAIL=<n>", exiting 1 when n > 0. An error stops the run: the trap says what it was and exits
# 1, which is also what an uncaught Stop error does under -File (measured with a probe of that shape).
# Exit codes 2-5 below keep their meaning: the app, its message window or Settings could not be found.
$failures = 0
function Failure([string]$what) { Write-Output "FAIL  $what"; $script:failures++ }
trap { Write-Output "FAIL  a PowerShell error stopped the capture: $($_.Exception.Message) (line $($_.InvocationInfo.ScriptLineNumber))"; Write-Output "RESULT: FAIL=$($script:failures + 1)"; exit 1 }

# The tab labels in bar order. The tab bar itself has no window text, but each page's heading reads
# exactly its tab's label, so the page on screen is proven by which heading is visible - never by
# counting key presses. Entering GPU Assignment refreshes that tab, and if a GPU change was left
# unfinished it raises a notice this harness never answers.
$TabLabels = @('Profiles', 'CPU Core Map', 'GPU Assignment', 'Setting')
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

Add-Type -TypeDefinition @"
using System; using System.Runtime.InteropServices; using System.Text; using System.Drawing;
[StructLayout(LayoutKind.Sequential)] public struct CDRECT { public int L,T,R,B; }
public class CDUi {
  public delegate bool EnumProc(IntPtr h, IntPtr l);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr l);
  [DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr p, EnumProc cb, IntPtr l);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowTextW(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out CDRECT r);
  [DllImport("user32.dll")] public static extern IntPtr PostMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern IntPtr SendMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern IntPtr SetFocus(IntPtr h);
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr dc, uint flags);
  [DllImport("user32.dll")] public static extern IntPtr GetParent(IntPtr h);

  // The visible DIRECT Static children of `parent` whose text is one of `labels` - the page headings on screen.
  public static string[] VisibleHeadings(IntPtr parent, string[] labels) {
    var hits = new System.Collections.Generic.List<string>();
    EnumChildWindows(parent, (h,l) => {
      if (GetParent(h) != parent || !IsWindowVisible(h)) return true;
      var c = new StringBuilder(256); GetClassNameW(h, c, 256);
      if (!string.Equals(c.ToString(), "Static", StringComparison.OrdinalIgnoreCase)) return true;
      var t = new StringBuilder(256); GetWindowTextW(h, t, 256);
      if (Array.IndexOf(labels, t.ToString()) >= 0) hits.Add(t.ToString());
      return true; }, IntPtr.Zero);
    return hits.ToArray(); }

  public static IntPtr Find(uint pid, string cls) {
    IntPtr f = IntPtr.Zero;
    EnumWindows((h,l) => { uint p; GetWindowThreadProcessId(h, out p);
      if (p == pid) { var c = new StringBuilder(256); GetClassNameW(h, c, 256);
                      if (c.ToString() == cls) { f = h; return false; } }
      return true; }, IntPtr.Zero);
    return f; }

  public static IntPtr FindChild(IntPtr parent, string cls) {
    IntPtr f = IntPtr.Zero;
    EnumChildWindows(parent, (h,l) => { var c = new StringBuilder(256); GetClassNameW(h, c, 256);
      if (c.ToString() == cls) { f = h; return false; } return true; }, IntPtr.Zero);
    return f; }

  public static string Info(IntPtr h) {
    var c = new StringBuilder(256); GetClassNameW(h, c, 256);
    var t = new StringBuilder(256); GetWindowTextW(h, t, 256);
    CDRECT r; GetWindowRect(h, out r);
    return string.Format("{0} '{1}' {2}x{3} visible={4}", c, t, r.R-r.L, r.B-r.T, IsWindowVisible(h)); }

  // PW_RENDERFULLCONTENT = 2. The window renders itself; nothing else can end up in the image.
  public static bool Shot(IntPtr h, string path) {
    CDRECT r; GetWindowRect(h, out r);
    int w = r.R-r.L, ht = r.B-r.T;
    if (w < 1 || ht < 1) return false;
    using (var b = new Bitmap(w, ht)) {
      using (var g = Graphics.FromImage(b)) {
        IntPtr dc = g.GetHdc();
        bool ok = PrintWindow(h, dc, 2);
        g.ReleaseHdc(dc);
        if (!ok) return false;
      }
      b.Save(path, System.Drawing.Imaging.ImageFormat.Png);
    }
    return true; }
}
"@ -Language CSharp -ReferencedAssemblies System.Drawing

$all = @(Get-Process GameOptimizer -ErrorAction SilentlyContinue)
if ($all.Count -eq 0) { Write-Output "Game Optimizer is not running - start $ExePath first"; exit 2 }
# THE PROCESS IS CHOSEN BY ITS PATH, NEVER "THE FIRST ONE" (adversarial review, v0.5.6): an installed v0.5.5 with three
# tabs was running beside the build under test, and its captures would have carried this build's four tab labels.
$want = [IO.Path]::GetFullPath($ExePath)
$app = $all | Where-Object { $_.Path -and [string]::Equals([IO.Path]::GetFullPath($_.Path), $want, [StringComparison]::OrdinalIgnoreCase) } | Select-Object -First 1
if (-not $app) {
    Write-Output "no running Game Optimizer was started from $want - running instances:"
    foreach ($p in $all) { Write-Output "  pid $($p.Id)  $(if ($p.Path) { "$($p.Path)  version $((Get-Item -LiteralPath $p.Path).VersionInfo.FileVersion)" } else { '<path not readable>' })" }
    exit 5
}
$procId = [uint32]$app.Id
Write-Output "app pid=$procId  path=$($app.Path)  version=$((Get-Item -LiteralPath $app.Path).VersionInfo.FileVersion)"

# The first-run wizard, if it is up, is worth a capture of its own before we dismiss it.
$wiz = [CDUi]::Find($procId, "GameOptimizerFirstRun")
if ($wiz -ne [IntPtr]::Zero) {
    Write-Output "wizard: $([CDUi]::Info($wiz))"
    if ([CDUi]::Shot($wiz, (Join-Path $OutDir "$Tag-wizard.png"))) { Write-Output "captured $Tag-wizard.png" }
    else { Failure "capture $Tag-wizard.png - PrintWindow produced no image" }
    [CDUi]::PostMessageW($wiz, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null   # WM_CLOSE
    Start-Sleep -Seconds 3
} else { Write-Output "no wizard on screen (config already has firstRunDone)" }

# IDM_SETTINGS = 40001 (IDM_STATUS = 40000 is the first tray menu id in src\ui.h).
$msgw = [CDUi]::Find($procId, "GameOptimizerMessageWindow")
if ($msgw -eq [IntPtr]::Zero) { Write-Output "no message window found"; exit 3 }
[CDUi]::PostMessageW($msgw, 0x0111, [IntPtr]40001, [IntPtr]::Zero) | Out-Null       # WM_COMMAND
Start-Sleep -Seconds 3

$set = [CDUi]::Find($procId, "GameOptimizerSettings")
if ($set -eq [IntPtr]::Zero) { Write-Output "Settings window did not open"; exit 4 }
Write-Output "settings: $([CDUi]::Info($set))"

# Each capture is labelled by the ONE visible page heading, read from the window - never by its position in the walk.
# A Settings window that was already open keeps the tab it was on, so a positional label could be wrong from the start.
function PageLabel([IntPtr]$window) {
    $hits = @([CDUi]::VisibleHeadings($window, [string[]]$TabLabels))
    if ($hits.Count -eq 1) { return $hits[0] }
    return "UNKNOWN - $($hits.Count) page headings visible"
}
# Captures Settings as $name, and counts a capture PrintWindow refused, a page no single heading names, or - when $want is
# given - any page but $want as a failure. THE PAGE A KEY PRESS REACHED IS CHECKED, NOT ONLY NAMED (fix round, v0.5.6): a
# tab bar that ignored WM_KEYDOWN gave four captures of one page, each labelled correctly, and the run exited 0.
function CapturePage([string]$name, [string]$want) {
    if (-not [CDUi]::Shot($set, (Join-Path $OutDir $name))) { Failure "capture $name - PrintWindow produced no image"; return }
    $label = PageLabel $set
    if ($label.StartsWith('UNKNOWN')) { Failure "capture $name - $label" }
    elseif ($want -and $label -cne $want) { Failure "capture $name shows '$label', expected '$want' - the page selector did not reach that page" }
    else { Write-Output "captured $name ($label)" }
}

# Walk the page selector. The UI moved from a left rail to a top tab bar, so try the tab bar
# first (RIGHT arrow) and fall back to the rail (DOWN arrow) - both notify their parent the
# same way, and keeping both means this harness still works on either layout.
$bar = [CDUi]::FindChild($set, "GameOptimizerTabBar")
$key = 0x27; $back = 0x25   # VK_RIGHT, VK_LEFT
if ($bar -eq [IntPtr]::Zero) {
    $bar = [CDUi]::FindChild($set, "GameOptimizerNav")
    $key = 0x28; $back = 0x26   # VK_DOWN, VK_UP
    if ($bar -ne [IntPtr]::Zero) { Write-Output "using the left rail (GameOptimizerNav)" }
} else { Write-Output "using the top tab bar (GameOptimizerTabBar)" }

# Back to the first page before the first capture. The selector stops at both ends, so enough presses land there
# from wherever it was.
if ($bar -ne [IntPtr]::Zero) {
    for ($i = 1; $i -lt [Math]::Max($Pages, $TabLabels.Count); $i++) {
        [CDUi]::SetFocus($bar) | Out-Null
        [CDUi]::SendMessageW($bar, 0x0100, [IntPtr]$back, [IntPtr]0) | Out-Null      # WM_KEYDOWN
        Start-Sleep -Milliseconds 300
    }
    Start-Sleep -Milliseconds 500
}

# Without a selector nothing was rewound, so the first page is only named, not checked.
CapturePage "$Tag-1.png" $(if ($bar -ne [IntPtr]::Zero) { $TabLabels[0] } else { '' })

if ($bar -eq [IntPtr]::Zero) {
    Failure "NO PAGE SELECTOR FOUND - only one page captured"
} else {
    for ($i = 2; $i -le $Pages; $i++) {
        [CDUi]::SetFocus($bar) | Out-Null
        [CDUi]::SendMessageW($bar, 0x0100, [IntPtr]$key, [IntPtr]0) | Out-Null      # WM_KEYDOWN
        Start-Sleep -Milliseconds 800
        # Past the last tab the bar stops, so the last label is the one expected again.
        CapturePage "$Tag-$i.png" ($TabLabels[[Math]::Min($i, $TabLabels.Count) - 1])
    }
}

Write-Output "done - inspect the PNGs for ghosted controls, smeared text, clipped drawing or a blank core map"
Write-Output "RESULT: FAIL=$failures"
exit ([int]($failures -gt 0))
