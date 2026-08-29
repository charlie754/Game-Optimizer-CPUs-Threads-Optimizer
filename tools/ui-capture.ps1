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
    [int]$Pages     = 4
)

$ErrorActionPreference = 'Stop'
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

$app = Get-Process GameOptimizer -ErrorAction SilentlyContinue | Select-Object -First 1
if (-not $app) { Write-Output "Game Optimizer is not running - start build\GameOptimizer.exe first"; exit 2 }
$procId = [uint32]$app.Id
Write-Output "app pid=$procId"

# The first-run wizard, if it is up, is worth a capture of its own before we dismiss it.
$wiz = [CDUi]::Find($procId, "GameOptimizerFirstRun")
if ($wiz -ne [IntPtr]::Zero) {
    Write-Output "wizard: $([CDUi]::Info($wiz))"
    if ([CDUi]::Shot($wiz, (Join-Path $OutDir "$Tag-wizard.png"))) { Write-Output "captured $Tag-wizard.png" }
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

if ([CDUi]::Shot($set, (Join-Path $OutDir "$Tag-1.png"))) { Write-Output "captured $Tag-1.png" }

# Walk the page selector. The UI moved from a left rail to a top tab bar, so try the tab bar
# first (RIGHT arrow) and fall back to the rail (DOWN arrow) - both notify their parent the
# same way, and keeping both means this harness still works on either layout.
$bar = [CDUi]::FindChild($set, "GameOptimizerTabBar")
$key = 0x27   # VK_RIGHT
if ($bar -eq [IntPtr]::Zero) {
    $bar = [CDUi]::FindChild($set, "GameOptimizerNav")
    $key = 0x28   # VK_DOWN
    if ($bar -ne [IntPtr]::Zero) { Write-Output "using the left rail (GameOptimizerNav)" }
} else { Write-Output "using the top tab bar (GameOptimizerTabBar)" }

if ($bar -eq [IntPtr]::Zero) {
    Write-Output "NO PAGE SELECTOR FOUND - only one page captured"
} else {
    for ($i = 2; $i -le $Pages; $i++) {
        [CDUi]::SetFocus($bar) | Out-Null
        [CDUi]::SendMessageW($bar, 0x0100, [IntPtr]$key, [IntPtr]0) | Out-Null      # WM_KEYDOWN
        Start-Sleep -Milliseconds 800
        if ([CDUi]::Shot($set, (Join-Path $OutDir "$Tag-$i.png"))) { Write-Output "captured $Tag-$i.png" }
    }
}

Write-Output "done - inspect the PNGs for ghosted controls, smeared text, clipped drawing or a blank core map"
