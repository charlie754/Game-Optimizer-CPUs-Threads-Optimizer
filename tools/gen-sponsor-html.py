# MAINTAINER TOOLING - NOT part of building Game Optimizer.
#
# Extract the ENTIRE sponsor panel from the plugin's widget.js and emit
#   (a) a reference page, for eyeballing against the operator's target screenshot, and
#   (b) src\sponsor_html.h - the SAME page as a C++ header the app compiles in.
#
# YOU DO NOT NEED TO RUN THIS TO BUILD. src\sponsor_html.h is committed; tools\build.bat
# compiles it exactly as it stands and never looks at the plugin. This script exists only to
# REGENERATE that header when the source panel changes, and it needs a copy of the author's
# browser extension, which is not part of this repository.
#
# RE-RUN IT WITH:
#     python tools\gen-sponsor-html.py
#
# WHERE IT READS THE PANEL FROM - first match wins:
#     --widget-js <file>                an explicit path to widget.js
#     GAME_OPTIMIZER_WIDGET_JS=<file>   the same, as an environment variable
#     --plugin-dir <dir>                the extension checkout. widget.js is looked for at
#     GAME_OPTIMIZER_PLUGIN_DIR=<dir>     <dir>\extension\content\, <dir>\content\, <dir>\
#     DEFAULT_PLUGIN_DIR below          the author's own working copy - the historical path,
#                                       kept as the default so their workflow is unchanged
#
# If none of those resolves, the script stops with an explanation instead of a traceback.
#
# WHERE THE TWO SCRATCH PAGES GO - panel-reference.html and panel-app.html, neither committed:
#     --out-dir <dir>, GAME_OPTIMIZER_PANEL_OUT_DIR, else the author's scratchpad if it is
#     still on this machine, else <repo>\build\panel. src\sponsor_html.h is ALWAYS written
#     into this repository and never into that directory.
#     Note: tools\measure-panel.py carries its own copy of the scratchpad path, so overriding
#     the out dir here does not move where that script looks.
#
# (b) is the one the product uses. The app NEVER reads the plugin path at run time; this script
# is the only thing in the repository that ever touches the plugin tree, and its output is
# committed. Re-run it and rebuild whenever the plugin's panel changes.
#
# This is the chair's independent check on what the target actually looks like - the lane is
# writing its own generator, and this one exists so the two can be compared.
import io, re, os, sys, datetime, hashlib

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# ---------------------------------------------------------------------------------------------
# INPUT AND OUTPUT PATHS
# ---------------------------------------------------------------------------------------------
# Both of these were absolute paths on the author's machine, written straight into the code.
# That works for one person and for nobody else: a fork could not run this script at all, and
# what it got instead of an explanation was a FileNotFoundError traceback. The defaults below
# are the SAME two paths, so the maintainer's own workflow is unchanged - they are now the last
# thing tried rather than the only thing.

DEFAULT_PLUGIN_DIR = r"F:/google map plugin"

# The author's scratchpad. A real directory on the development machine and meaningless anywhere
# else, so it is used only when it actually exists.
LEGACY_OUT_DIR = (r"C:/Users/IRP/AppData/Local/Temp/claude/"
                  r"F--Game-Optimizer--claude-worktrees-windows-cpu-sets-tray-c96e1a/"
                  r"68d33dbc-1415-4f46-9fe1-be03413760eb/scratchpad")

# widget.js is looked for at these places under the plugin directory, in order. A checkout of
# the extension may be rooted at its repository, at the extension folder, or at the content
# folder itself; all three are accepted rather than guessed at.
WIDGET_JS_CANDIDATES = ("extension/content/widget.js", "content/widget.js", "widget.js")


def cli_value(flag):
    """Read `--flag VALUE` or `--flag=VALUE` off the command line. None if absent."""
    argv = sys.argv[1:]
    for i, arg in enumerate(argv):
        if arg == flag:
            if i + 1 >= len(argv):
                raise SystemExit("*** %s needs a value:  %s <path>" % (flag, flag))
            return argv[i + 1]
        if arg.startswith(flag + "="):
            return arg[len(flag) + 1:]
    return None


def no_plugin(plugin_dir, how, tried):
    """Stop with something a stranger can act on, rather than a traceback."""
    raise SystemExit(
        "\n*** Could not find the sponsor panel's source file, widget.js.\n"
        "\n"
        "    Looked under : %s\n"
        "    Chosen by    : %s\n"
        "    Tried        :\n"
        "      %s\n"
        "\n"
        "    THIS SCRIPT IS MAINTAINER TOOLING AND A NORMAL BUILD DOES NOT NEED IT.\n"
        "    src\\sponsor_html.h is already committed to this repository, and tools\\build.bat\n"
        "    compiles it exactly as it stands. Running this script only REGENERATES that\n"
        "    header from the author's own browser extension, which is not part of this\n"
        "    repository and is not redistributed with it. If you are building Game Optimizer,\n"
        "    nothing is wrong and there is nothing here you need.\n"
        "\n"
        "    If you do have a copy of the extension, point this script at it:\n"
        "      python tools\\gen-sponsor-html.py --plugin-dir <dir>\n"
        "      python tools\\gen-sponsor-html.py --widget-js <dir>\\content\\widget.js\n"
        "    or set an environment variable instead:\n"
        "      set GAME_OPTIMIZER_PLUGIN_DIR=<dir>\n"
        % (plugin_dir, how, "\n      ".join(tried)))


def resolve_widget_js():
    """Locate widget.js. Returns (absolute path, how it was chosen)."""
    from_flag = cli_value("--widget-js")
    explicit = from_flag or os.environ.get("GAME_OPTIMIZER_WIDGET_JS")
    if explicit:
        how = "--widget-js" if from_flag else "GAME_OPTIMIZER_WIDGET_JS"
        path = os.path.abspath(explicit)
        if not os.path.isfile(path):
            no_plugin(os.path.dirname(path) or ".", how, [path])
        return path, how

    plugin_dir, how = cli_value("--plugin-dir"), "--plugin-dir"
    if not plugin_dir:
        plugin_dir, how = os.environ.get("GAME_OPTIMIZER_PLUGIN_DIR"), "GAME_OPTIMIZER_PLUGIN_DIR"
    if not plugin_dir:
        plugin_dir, how = DEFAULT_PLUGIN_DIR, "DEFAULT_PLUGIN_DIR (the author's working copy)"

    tried = [os.path.abspath(os.path.join(plugin_dir, *rel.split("/")))
             for rel in WIDGET_JS_CANDIDATES]
    for path in tried:
        if os.path.isfile(path):
            return path, how
    no_plugin(os.path.abspath(plugin_dir), how, tried)


def resolve_out_dir():
    """Where panel-reference.html and panel-app.html go. Returns (dir, how it was chosen)."""
    out, how = cli_value("--out-dir"), "--out-dir"
    if not out:
        out, how = os.environ.get("GAME_OPTIMIZER_PANEL_OUT_DIR"), "GAME_OPTIMIZER_PANEL_OUT_DIR"
    if not out:
        if os.path.isdir(LEGACY_OUT_DIR):
            out, how = LEGACY_OUT_DIR, "LEGACY_OUT_DIR (present on this machine)"
        else:
            out, how = os.path.join(REPO, "build", "panel"), "<repo>\\build\\panel (fallback)"
    out = os.path.abspath(out)
    try:
        os.makedirs(out, exist_ok=True)
    except OSError as exc:
        raise SystemExit("*** cannot create the scratch-page directory\n"
                         "      %s\n    %s\n"
                         "    Pick another with --out-dir <dir> or GAME_OPTIMIZER_PANEL_OUT_DIR."
                         % (out, exc))
    return out, how


PLUGIN_JS, PLUGIN_JS_HOW = resolve_widget_js()
S, OUT_DIR_HOW = resolve_out_dir()
print("panel source : %s   [%s]" % (PLUGIN_JS, PLUGIN_JS_HOW))
print("scratch pages: %s   [%s]" % (S, OUT_DIR_HOW))

src = io.open(PLUGIN_JS, encoding="utf-8").read()

# The operator's three destinations. They are substituted into the page HERE, at generation
# time, so the committed header is self-contained and the running app has no placeholder left
# to resolve. src\sponsor.h carries the same three constants because webview_host.cpp checks
# every navigation target against them before handing one to ShellExecuteW - a page that asked
# for anything else is refused. The two lists must agree; VerifyUrlsAgainstSponsorH below
# fails the generation if they ever drift apart.
URL_KOFI   = "https://ko-fi.com/irp_hongkong"
URL_GITHUB = "https://github.com/charlie754/Game-Optimizer-CPUs-Threads-Optimizer"
# The GOAT Cam plate. It replaced the GOATPROJECT lockup's old destination on 2026-10-25; the
# App Store link lives only in the QR's pixels and is deliberately NOT a destination here.
URL_GOAT   = "https://goatcam.dagoat.io"

# ---------------------------------------------------------------------------------------------
# MEASURE MODE
# ---------------------------------------------------------------------------------------------
# The panel's shipped width is a measurement of the panel, and the panel's width is set from
# that measurement - which is circular unless one run is allowed to have no width at all. That
# is what this flag is for:
#
#     python tools\gen-sponsor-html.py --measure     emits `.shell.is-open{width:max-content}`
#                                                    so the page sizes itself, unconstrained
#     python tools\measure-panel.py                  reports the resulting box
#     (write the two numbers into SHELL_MIN_W / SHELL_H below)
#     python tools\gen-sponsor-html.py               emits `width:100%` and writes the header
#     python tools\measure-panel.py                  sweeps a range of host widths and must
#                                                    report the SAME floor it just measured
#
# WHAT THE TWO PASSES MEAN NOW THAT THE PANEL IS RESPONSIVE. The shipping page is
# `.shell.is-open{width:100%}` - it takes whatever width the Settings window gives it - so pass
# 2 is no longer "check the pin took". It is a SWEEP: measure-panel.py renders the shipping page
# at a range of host widths and asserts the layout at every one of them. Pass 1 still exists
# because `width:max-content` reports the panel's intrinsic minimum DIRECTLY, which is a second
# and independent route to the same number the sweep finds by walking down until it breaks. Two
# routes that have to agree is the point; one measurement of a responsive layout proves nothing.
#
# A measure run deliberately does NOT write src\sponsor_html.h. A header carrying a
# `width:max-content` page would build and run and lay the panel out to its own intrinsic width
# inside a full-width host - a right-hugging card with a band of transparent filler beside it -
# with no error anywhere. Refusing to write it is cheaper than detecting it later.
MEASURE = ("--measure" in sys.argv) or (os.environ.get("GEN_SPONSOR_MEASURE") == "1")

# ---------------------------------------------------------------------------------------------
# THE PANEL'S NATURAL SIZE - AND WHY THE PLUGIN NO LONGER DECIDES IT
# ---------------------------------------------------------------------------------------------
# THIS IS NOT THE PLUGIN'S PANEL. The plugin stacks Ko-fi, GitHub and the GOAT Cam banner in one
# column, and its `.shell.is-open { width: 272px }` describes THAT shape. Game Optimizer lays the
# same three things out as ONE ROW - Ko-fi, Star-on-Github, then the GOAT Cam banner (banner v2,
# master SPEC section 7) in the master's `.goat--strip` variant: the headline beside the plate
# that the coffee photo fills edge to edge - left to right (see GO_LAYOUT_CSS). Their 272 is read
# as PLUGIN_CSS_W and PRINTED FOR PROVENANCE ONLY.
#
# THE PANEL HAS NO SHIPPED WIDTH. It FILLS the Settings window's content row - GO_LAYOUT_CSS's
# `.shell.is-open{width:100%}` and settings.cpp's `spW = cw - 2 * GAP` - so there is only a FLOOR:
#
#   SHELL_MIN_W  the narrowest host width at which the three groups still fit. Below it,
#                `.shell { overflow: hidden }` cuts the right-hand group - the GOAT Cam plate
#                and its QR - off in silence. WM_GETMINMAXINFO's window minimum has to cover it;
#                it is emitted as kSponsorCssMinWidth.
#   SHELL_H      the panel's height, which does NOT vary with the width: the plate is a fixed
#                height and nothing in the row wraps. Emitted as kSponsorCssHeight.
#
# Both are MEASURED against this exact page by tools\measure-panel.py, never estimated.
#
# [M] MEASURED 2026-10-26 (banner v2) by tools\measure-panel.py, chrome-headless-shell 1234,
#     BOTH passes:
#
#       pass 1  --measure, `.shell.is-open{width:max-content}`, host 1800px
#               .shell 847.39 x 76.00   -> ceil = 848 x 76
#
#       pass 2  the shipping page, `width:100%`, swept 1000px down to 640px in 1px steps
#               first break at 847px (group-past-row) -> the floor is 848, identical to pass 1
#
#     The arithmetic behind 847.39, so it can be re-derived rather than re-measured:
#       .kofi 171.47 + .gh 175.36 (both at native 0.5.9 size) + .goat--strip 434.00
#       (= the master's copy column 136 + gap 10 + plate 356)
#       = 779.39 of groups, + 2 * ROW_GAP 20 = 819.39 for the row,
#       + 26 (.body__pad's 13px a side) + 2 (the shell's 0.5px border, snapped to 1px a side)
#       = 847.39. The master measured the same row in its own mock at 847.38 (README, "Game
#       Optimizer row floor").
#     Height: the plate's PLATE_H 74 + 2 border = 76. Ko-fi (37.86) and Star (37.89) are centred
#     inside it; the headline is 32.38 - two lines - and centred too.
SHELL_MIN_W = 849
SHELL_H = 76

if SHELL_MIN_W < 200 or SHELL_H < 40:
    raise SystemExit("*** SHELL_MIN_W x SHELL_H = %d x %d is implausibly small - it looks like a "
                     "measurement that failed rather than a panel." % (SHELL_MIN_W, SHELL_H))

# ---------------------------------------------------------------------------------------------
# THE HEIGHT IS THE GOAT CAM PLATE'S, AND IT IS THE SAME BAND THE GDI STRIP DRAWS
# ---------------------------------------------------------------------------------------------
# The row used to be keyed to the Ko-fi button ("align all height same as Ko-fi button"). The GOAT
# Cam plate is the tallest thing in it by a wide margin, and its height is not a style choice: the
# App Store QR is 37 modules INCLUDING its quiet zone and the master renders it at 72 css px, the
# smallest size that decoded at every DPR the master tested.
#
# BAND_H is the band BOTH renderings reserve: kSponsorCssHeight here, and sponsor.cpp's kPlateH
# (76 at 96 dpi) for the GDI strip. Change one and the other has to move with it.
#
# The master strip plate is `--gc-h: 76px` with no padding - a 1px border a side around a 74px
# photo layer, and the QR centred at `top: (--gc-h - 2px - 72px) / 2` = 1px. Inside the shell that
# would make the panel 76 + 2 (the shell's own border) = 78, two px taller than the band. So this
# page sets `--gc-h` to PLATE_H = BAND_H - 2 * SHELL_BORDER = 74 - THROUGH THE VARIABLE, not with a
# bare `height`, because the master positions the QR from `--gc-h`: a bare height would leave the
# QR's top at 1px and its bottom 1px over the plate's bottom border. At 74 the QR sits at 0px
# inset, its full 72 css px exactly filling the plate's content box.
BAND_H = 76
SHELL_BORDER = 1     # the plugin's 0.5px .shell border, snapped to 1 device px at DPR 1
PLATE_H = BAND_H - 2 * SHELL_BORDER
# The .body__pad top AND bottom padding. The plate fills the shell's content box top to bottom,
# exactly as in the master's own strip mock; Ko-fi and Star are centred in what is left.
PAD_Y = 0

# THE KO-FI BUTTON'S NATURAL HEIGHT. Ko-fi and Star are still ONE HEIGHT - the
# operator's standing "same height as Ko-fi" - but they are centred in the plate's band rather than
# setting it. Star is naturally shorter (36.00), so it is given this height explicitly;
# tools\measure-panel.py asserts the two still render the same at every width.
#
# [M] MEASURED 2026-08-29, chrome-headless-shell 1234: .kofi 47.375 (padding 9 + 9 and the
#     two-line .kofi__label, 12.5px/1.25 + 11px/1.25), .gh 36.00.
KOFI_H = 47.375

# ---------------------------------------------------------------------------------------------
# THE FIT: KEEP THE 0.5.9 CONTROLS, SCALE ONLY THE NEW BANNER
# ---------------------------------------------------------------------------------------------
# [M] The 0.5.9 page measured .kofi=171.47px and .gh=175.36px at their native CSS size. The
# sponsor controls are the compatibility boundary: their markup and every applicable rule are
# checked byte-for-byte below, and the new banner is the only thing allowed to give width.
# The old third-group box was 236px copy + 8px pair gap + 190px lockup = 434px. The online
# renderer therefore uses the same 434px slot (136px signed copy + 8px gap + 290px plate).

# The lift. The master's strip already has no movement (`--gc-lift: none`), but its hover still
# applies `--gc-lift-shadow`: a drop shadow below the plate and an outer 1px gold ring. In a band
# exactly the plate's height both fall outside the shell's `overflow: hidden` and would show only
# at the plate's left and right edges. So this page does what sponsor.cpp's GDI strip does
# (kLiftRingA): the lift is the RING ALONE - the plate's own 1px border warms to gold at .45. The
# press (scale .98) is the master's and stays: it shrinks inward and clips nothing.
LIFT_RING = "rgba(240,192,122,.45)"

# THE QR DOES NOT ZOOM IN PLACE. The master grows the QR to 160 css px on hover in its taller
# variants; a 76px band has no room for that, so the master's strip sets `--gc-qr-zoom: 1` and its
# goatcam-qr-bridge.js asks the native host for a magnifier popup instead (SPEC 7.4). The
# generator refuses a master that stops saying so, and GO_LAYOUT_CSS also pins `scale: none` on
# the strip's tile - an in-place zoom here would be cut off by the shell, and the popup shows the
# enlarged code anyway.
#
# THE BRIDGE: goatcam-qr-bridge.js, from the GOAT Cam repository's brand\goatcam-banner\ (it is
# not in widget.js - Maps grows its QR in place). It is inlined verbatim as the page's only other
# script and called once on the shadow root. Its message is the one src\goatcam_qr.h parses:
#   {"goatcamQr":"enter"|"leave","rect":{"x":..,"y":..,"w":..,"h":..}}
# WHERE IT IS READ FROM - first match wins:
#     --bridge-js <file>                    an explicit path
#     GAME_OPTIMIZER_QR_BRIDGE_JS=<file>    the same, as an environment variable
#     DEFAULT_BRIDGE_JS below               the author's GOAT Cam checkouts, main tree first
DEFAULT_BRIDGE_JS = (
    r"F:/phone goat cam/brand/goatcam-banner/goatcam-qr-bridge.js",
    r"F:/phone goat cam/.claude/worktrees/dagoat-banner-ads-update-2f9f38/brand/goatcam-banner/"
    r"goatcam-qr-bridge.js",
)
# The SPEC caps the embedded photo at about 40 KB (section 7.3). A larger data URI is a design
# decision for the master, not something to inherit silently.
PHOTO_URI_MAX = 40 * 1024

# ---------------------------------------------------------------------------------------------
# THE GAPS, AND WHY THERE HAVE TO BE TWO KINDS
# ---------------------------------------------------------------------------------------------
# THREE groups, and the third one has two things in it:
#
#     [ Ko-fi ]      [ Star Project on Github ]      [ statement  ( GOAT Cam plate ) ]
#
# The only thing that makes the statement and the plate read as one group is that the gap inside
# the pair is smaller than the gaps around it. The pair's gap is now the MASTER'S - `.goat--strip
# { gap: 10px }`, read out of their sheet below as STRIP_GAP - and ROW_GAP must stay clear of it.
# tools\measure-panel.py re-asserts the rendered version at every swept width.
#
# ROW_GAP IS A FLOOR, NOT THE GAP YOU SEE. `.go-row` is `justify-content: space-between`, so the
# spare width at any window above the minimum goes into the two inter-group gaps, equally.
# Unchanged at 20: twice the pair's 10 at the narrowest window, where the grouping is weakest.
ROW_GAP = 20       # the FLOOR for each of the two gaps between the three groups.

# [M] The three groups' widths, measured by tools\measure-panel.py at the narrowest width. Used
# for nothing but the floor-budget diagnostic below - the layout reads them off the fonts, never
# from here - so a stale value costs a slightly wrong error message and nothing else.
KOFI_W_MEASURED = 171.47
GH_W_MEASURED = 175.36
GOAT_W_MEASURED = 434.00

# ---------------------------------------------------------------------------------------------
# THE BUDGET IS DERIVED FROM THE C++, NOT GUESSED - AND IT IS A FLOOR, NOT A CEILING
# ---------------------------------------------------------------------------------------------
# The panel fills the width, so the question is: IS THE SMALLEST WINDOW WIDE ENOUGH FOR THE THREE
# GROUPS? - and the number that answers it is SHELL_MIN_W. The chain, readable out of three files:
#
#   [M] src\settings.cpp  SettingsLayout, the sponsor panel block, WebView2 path:
#           spW = cw - 2 * GAP;   spLeft = GAP;
#       The host window IS the content row - full width, one gap a side.
#
#   [M] src\settings.cpp  SettingsLayout:   const int GAP = theme::Dp(theme::metric::kGap, dpi);
#   [M] src\theme.h:71                      constexpr int kGap = 12;
#   [M] src\theme.cpp:329                   int Dp(int logical, int dpi) { return MulDiv(logical, dpi, 96); }
#
#   [M] src\settings.cpp  WM_GETMINMAXINFO:
#           const int minClientW = theme::Dp(880, dpi);
#       The minimum CLIENT width is Dp(880). That is the worst case: at any larger window the
#       panel has strictly more room, and every extra pixel goes into the two inter-group gaps.
#
#   [M] src\webview_host.cpp  WebSponsorMinSize:
#           s.cx = ::MulDiv(kSponsorCssMinWidth, dpi, 96);
#
# So, in css px:  880 - 2 * 12 = 856 available at the smallest allowed window.
#
# 856 IS THE RIGHT ANSWER IN CSS PX AND THE WRONG NUMBER TO WRITE IN A GUARD, because the three
# MulDiv calls round INDEPENDENTLY. Windows custom scaling runs 100%..500%, so the ceiling is
# COMPUTED over every integer dpi in that range, with MulDiv modelled exactly as Win32 does it -
# round to nearest, ties away from zero, i.e. (a*b + c//2)//c.
MIN_CLIENT_CSS_W = 880          # [M] settings.cpp WM_GETMINMAXINFO
GAP_CSS = 12                    # [M] theme::metric::kGap
DPI_LO, DPI_HI = 96, 480        # 100% .. 500%


def _muldiv(a, b, c):
    # Win32 MulDiv for the non-negative case: round to nearest, ties away from zero.
    return (a * b + c // 2) // c


def _worst_margin(css_w):
    # The smallest DEVICE-px margin the panel has at the minimum window, over every DPI.
    return min(_muldiv(MIN_CLIENT_CSS_W, d, 96)
               - 2 * _muldiv(GAP_CSS, d, 96)
               - _muldiv(css_w, d, 96)
               for d in range(DPI_LO, DPI_HI + 1))


SHELL_W_CEILING = MIN_CLIENT_CSS_W - 2 * GAP_CSS
while SHELL_W_CEILING > 200 and _worst_margin(SHELL_W_CEILING) < 0:
    SHELL_W_CEILING -= 1

print("width budget : min client %d - 2 * %d = %d css px available; safe at every dpi %d..%d up "
      "to %d; this page needs SHELL_MIN_W = %d (worst-case margin %d device px)"
      % (MIN_CLIENT_CSS_W, GAP_CSS, MIN_CLIENT_CSS_W - 2 * GAP_CSS, DPI_LO, DPI_HI,
         SHELL_W_CEILING, SHELL_MIN_W, _worst_margin(SHELL_MIN_W)))

# THE GUARD. A floor past the budget would genuinely clip - silently, at the panel's own RIGHT
# edge, which is where the plate and its QR are - so it refuses, and refusing by writing no header
# is the safe failure.
if SHELL_MIN_W > SHELL_W_CEILING and not MEASURE and os.environ.get("GEN_SPONSOR_ALLOW_WIDE") != "1":
    raise SystemExit(
        "*** SHELL_MIN_W = %d is past the %dpx budget, so at the SMALLEST allowed window the\n"
        "    three groups DO NOT FIT and `.shell { overflow: hidden }` would cut the right-hand\n"
        "    group - the GOAT Cam plate and its QR - off, silently, with no error anywhere.\n"
        "    DO NOT SHIP THIS - src%ssponsor_html.h has NOT been rewritten and still carries the\n"
        "    previous layout, which is the safe state.\n"
        "\n"
        "    Where the budget comes from: the minimum CLIENT width is theme::Dp(%d) and the panel\n"
        "    is that less one theme::metric::kGap = %d a side, so %d css px are available - but\n"
        "    the three MulDiv calls round independently, so the largest width that is safe at\n"
        "    EVERY integer dpi from %d to %d is %d. That number is COMPUTED above, not typed in.\n"
        "\n"
        "    What this page's floor is made of [M], measured, not estimated:\n"
        "      .kofi           %6.2f  native 0.5.9 size\n"
        "      .gh             %6.2f  native 0.5.9 size\n"
        "      .goat--strip    %6.2f  the master's copy column + gap + plate, as designed\n"
        "      row gaps        %6d  the TWO inter-group gaps at their floor, %dpx each\n"
        "      shell chrome     28.00  .body__pad 13px a side + the 0.5px border, snapped\n"
        "\n"
        "    THE WAYS OUT, and every one of them is a design decision rather than a rounding one:\n"
        "      * raise the window minimum - settings.cpp's WM_GETMINMAXINFO and MIN_CLIENT_CSS_W\n"
        "        here and in tools\\measure-panel.py must move TOGETHER;\n"
        "      * cut ROW_GAP, keeping it more than 1px clear of the plate's 10px pair gap;\n"
        "      * reduce only the new GOAT Cam plate or its signed copy column.\n"
        "\n"
        "    Set GEN_SPONSOR_ALLOW_WIDE=1 only to generate a page for measuring; it still\n"
        "    refuses to write the header."
        % (SHELL_MIN_W, SHELL_W_CEILING, chr(92), MIN_CLIENT_CSS_W, GAP_CSS,
           MIN_CLIENT_CSS_W - 2 * GAP_CSS, DPI_LO, DPI_HI, SHELL_W_CEILING,
           KOFI_W_MEASURED, GH_W_MEASURED, GOAT_W_MEASURED,
           2 * ROW_GAP, ROW_GAP))

# ---------------------------------------------------------------------------------------------
# THE ONLY CSS IN THIS PAGE THAT IS NOT THE PLUGIN'S OR THE MASTER'S
# ---------------------------------------------------------------------------------------------
# Appended AFTER the plugin's rules and the master GOAT Cam sheet inside the same <style>, so
# where a declaration collides the later one wins on cascade order or on one extra class - no
# !important, not one byte of theirs edited. It touches the master banner in exactly four ways,
# each forced by the band and each listed here:
#
#   .go-row               ONE ROW, THREE GROUPS, `justify-content: space-between`: the outer
#                         groups sit on .body__pad's padding and ALL spare width goes to the two
#                         gaps between them - ROW_GAP is their floor. `align-items: center`: the
#                         plate sets the height and Ko-fi and Star sit in the middle of it.
#   .go-row>*             flex:0 0 auto - nothing grows, nothing shrinks. A shrinking group would
#                         rewrap the headline or squeeze the plate the photo, wordmark and QR are
#                         laid out on.
#   .go-row>.kofi/.gh     width:max-content (both are `width: 100%` in the plugin's column) and
#                         margin-top:0 (their column spacing). These declarations, plus the
#                         `.gh__inner` flex fill, are the exact 0.5.9 row contract; no zoom or
#                         size override is permitted here.
#   .go-row>.goat         margin:0. The plugin's host glue gives `.body__pad .goat` a 16px top
#                         margin for its column; in a row that would push the banner off-centre.
#   (1) --gc-h            PLATE_H instead of the master's 76 - see BAND_H. Set through the
#                         variable because the master also centres the QR from it.
#   (2) the lift          --gc-lift-shadow none; :hover warms the border instead - see LIFT_RING.
#                         :active keeps the master's 120ms press.
#   (3) :focus-visible    the master's 2px gold outline sits 2px OUTSIDE the plate, where the
#                         shell's overflow would cut its top and bottom off; here it is drawn 2px
#                         INSIDE, so keyboard focus shows on all four sides.
#   (4) .gc-qr            `scale: none` - the QR never zooms in this band; see THE QR DOES NOT ZOOM
#                         IN PLACE. It beats both of the master's zoom rules, `.gc-qr:hover` and
#                         `.goat-lockup:focus-visible .gc-qr`, on specificity or order.
#   .body__pad            top and bottom padding PAD_Y; their 13px sides are untouched.
#   .shell.is-open        width:100% - the panel fills its host. Under --measure it is
#                         `max-content` so the page reports its own intrinsic minimum.
GO_LAYOUT_CSS = """
.go-row{display:flex;flex-direction:row;align-items:center;justify-content:space-between;gap:%(rowgap)dpx}
.go-row>*{flex:0 0 auto}
.go-row>.kofi{width:max-content;margin-top:0}
.go-row>.gh{width:max-content;margin-top:0;display:flex;align-items:stretch}
.go-row>button:nth-of-type(2){height:47.375px}
.go-row>.gh>.gh__inner{flex:1 1 auto}
.go-row>.goat-mount{margin:0}
.go-row>.goat-mount>.goat--strip{display:inline-flex;width:auto;max-width:none;margin:0}
.go-row>.goat-mount>.goat--strip .goat-lockup{width:290px;--gc-h:%(plateh)dpx;height:%(plateh)dpx;overflow:hidden;--gc-lift-shadow:none;transition:transform var(--gc-t-out) var(--gc-out),box-shadow var(--gc-t-out) var(--gc-out),border-color var(--gc-t-out) var(--gc-out)}
.go-row>.goat-mount>.goat--strip .goat-lockup:hover{border-color:%(ring)s;transition:transform 420ms var(--gc-in),box-shadow 420ms var(--gc-in),border-color 420ms var(--gc-in)}
.go-row>.goat-mount>.goat--strip .goat-lockup:active{transition-duration:120ms}
.go-row>.goat-mount>.goat--strip .goat-lockup:focus-visible{outline-offset:-2px}
.go-row>.goat-mount>.goat--strip .gc-stage{position:absolute;inset:auto;width:356px;height:%(plateh)dpx;left:50%%;top:50%%;transform:translate(-50%%,-50%%) scale(.76);transform-origin:50%% 50%%;transition:transform 360ms var(--gc-out)}
.go-row>.goat-mount>.goat--strip .goat-lockup:is(:hover,:focus-visible,.is-active) .gc-stage{transform:translate(-50%%,-50%%) scale(.798);transition:transform 420ms var(--gc-in)}
.go-row>.goat-mount>.goat--strip .goat-lockup__word{position:absolute;z-index:1;left:128px;top:3px;width:98px;max-width:98px;min-width:0;transform:none}
.go-row>.goat-mount>.goat--strip .gc-iphone{position:absolute;z-index:2;top:calc((var(--gc-h) - 2px - var(--gc-qr))/2 + 6px);right:calc(9px + var(--gc-qr) + 6px);display:block;width:28px;height:60px;overflow:hidden;border:0;border-radius:6.5px;background:transparent;box-shadow:0 2px 5px rgba(0,0,0,.55);pointer-events:none}
.go-row>.goat-mount>.goat--strip .gc-iphone__screen{display:block;width:100%%;height:100%%;border:0;border-radius:4.6px;object-fit:fill}
.go-row>.goat-mount>.goat--strip .gc-vf{--gc-vf-t:0px;--gc-vf-r:0px;--gc-vf-b:0px;--gc-vf-l:0px}
.go-row>.goat-mount>.goat--strip .gc-qr{right:9px;top:calc((var(--gc-h) - 2px - var(--gc-qr))/2);bottom:auto}
.go-row>.goat-mount>.goat--strip .goat-lockup:is(:hover,:focus-visible,.is-active) .gc-qr{transform:scale(.952381);transform-origin:50%% 50%%}
.go-row>.goat-mount>.goat--strip .gc-qr{scale:none}
.body__pad{padding-top:%(pady)dpx;padding-bottom:%(pady)dpx}
.shell.is-open{width:%(shell)s}

/* The Maps runtime rules above are panel-oriented. Once the signed page is adapted to the
   master's strip, these host overrides restore the strip contract without touching the copied
   renderer: two complete headline rows, signed tagline rows, an undimmed full-bleed photo, and
   the master's whole-stage hover motion. */
.go-row>.goat-mount>.goat--strip.goat--online{display:flex;flex-direction:row;align-items:center;gap:8px;width:auto;max-width:none}
.go-row>.goat-mount>.goat--strip.goat--online .goat__copy{display:flex;flex-direction:column;justify-content:center;flex:0 0 136px;width:136px;height:32px;margin:0;padding:0;text-align:right;align-items:flex-end}
.go-row>.goat-mount>.goat--strip.goat--online .gc-iphone{right:calc(9px + var(--gc-qr) + 8px)}
.go-row>.goat-mount>.goat--strip.goat--online .goat__line{font-size:8px !important;line-height:1.25;white-space:nowrap;overflow:visible;text-wrap:nowrap}
.go-row>.goat-mount>.goat--strip.goat--online .goat-lockup__tag{gap:1px;text-transform:none;white-space:normal}
.go-row>.goat-mount>.goat--strip.goat--online .goat-lockup__tag>span{overflow:visible;white-space:nowrap}
.go-row>.goat-mount>.goat--strip.goat--online{--gc-qr:64px}
.go-row>.goat-mount>.goat--strip.goat--online .gc-vf{--gc-vf-t:0px;--gc-vf-r:0px;--gc-vf-b:0px;--gc-vf-l:0px}
.go-row>.goat-mount>.goat--strip.goat--online .goat-lockup:is(:hover,:focus-visible,.is-active) .gc-vf{inset:3px}
.go-row>.goat-mount>.goat--strip.goat--online .gc-photo{--gc-scrim:none}
.go-row>.goat-mount>.goat--strip.goat--online .gc-photo::after{background:none;transform:none}
.go-row>.goat-mount>.goat--strip.goat--online .goat-lockup:is(:hover,:focus-visible,.is-active) .gc-stage{transform:translate(-50%%,-50%%) scale(.798);transition:transform 420ms var(--gc-in)}

/* DaGoat remains the legacy page banner beside its copy, matching 71ea5ed/GDI proportions;
   applying the online strip's 136px copy column to it is what created the extra wraps. */
.go-row>.goat-mount>.goat--strip.goat--dagoat{display:flex;flex-direction:row;align-items:stretch;gap:8px;width:auto;max-width:none}
.go-row>.goat-mount>.goat--strip.goat--dagoat .goat__copy{flex:0 0 236px;width:236px;margin:0;gap:3px;text-align:right;align-items:flex-end}
.go-row>.goat-mount>.goat--strip.goat--dagoat .goat-lockup{flex:0 0 190px;width:190px;max-width:none;height:74px;font-size:5.069px}
.go-row>.goat-mount>.goat--strip.goat--dagoat .goat-lockup__meteors{inset:0}
.go-row>.goat-mount>.goat--strip.goat--dagoat .goat-lockup__word{position:relative;left:auto;top:auto;width:auto;max-width:100%%;transform:none}
"""

BS = chr(92)          # backslash, kept out of literals so nothing can re-escape it
ESCAPES = {"n": chr(10), "t": chr(9), "r": chr(13), '"': '"', "'": "'", BS: BS}


def read_quoted(text, start, quote):
    """Read a JS string starting at the opening quote index, honouring escapes."""
    out = []
    k = start + 1
    esc = False
    while k < len(text):
        c = text[k]
        if esc:
            out.append(ESCAPES.get(c, c))
            esc = False
        elif c == BS:
            esc = True
        elif c == quote:
            break
        else:
            out.append(c)
        k += 1
    return "".join(out), k


def join_literals(text):
    """Concatenate every quoted literal in a region, ignoring the + operators between them."""
    out = []
    i = 0
    while i < len(text):
        c = text[i]
        if c in ('"', "'"):
            lit, end = read_quoted(text, i, c)
            out.append(lit)
            i = end + 1
        else:
            i += 1
    return "".join(out)


def const_string(name):
    """Read a const whose value is a string OR a multi-line concatenation of strings.

    SVG_CUP and SVG_STAR are declared as `const X =` on one line followed by a dozen
    '...' + '...' fragments. Reading only the FIRST literal silently truncates the icon to
    its opening <svg> tag - which renders as nothing and looks like a styling problem rather
    than an extraction bug. Read to the terminating semicolon instead.
    """
    i = src.index("const %s" % name)
    eq = src.index("=", i)
    j = eq + 1
    depth = 0
    while j < len(src):
        c = src[j]
        if c in ('"', "'"):
            _, j = read_quoted(src, j, c)
        elif c == "(":
            depth += 1
        elif c == ")":
            depth -= 1
        elif c == ";" and depth == 0:
            break
        j += 1
    return join_literals(src[eq + 1:j])


def const_template(name):
    i = src.index("const %s" % name)
    a = src.index("`", i)
    b = src.index("`", a + 1)
    return src[a + 1:b]


css = const_template("CSS")
lock = const_string("GOAT_LOCKUP_CSS")
online_fix = const_string("GOAT_ONLINE_FIX_CSS")
cup = const_string("SVG_CUP")
star = const_string("SVG_STAR")

# The online renderer is a signed-data consumer. Copy the marked block from Maps verbatim;
# this generator may adapt its emitted goat--panel class in the page bridge, but never edits the
# renderer's logic or source text.
RENDER_BEGIN = "// goatcam-banner-render:begin"
RENDER_END = "// goatcam-banner-render:end"
if src.count(RENDER_BEGIN) != 1 or src.count(RENDER_END) != 1:
    raise SystemExit("*** widget.js must contain exactly one goatcam-banner-render begin/end pair")
_render_start = src.index(RENDER_BEGIN)
_render_end = src.index(RENDER_END, _render_start) + len(RENDER_END)
renderer_block = src[_render_start:_render_end]
if "</script" in renderer_block.lower():
    raise SystemExit("*** the marked renderer block contains </script and cannot be inlined")
print("renderer block: %d bytes, sha256 %s (copied verbatim)"
      % (len(renderer_block), hashlib.sha256(renderer_block.encode("utf-8")).hexdigest()))

PAGE_BRIDGE_PATH = os.path.join(REPO, "tools", "banner-page-bridge.js")
if not os.path.isfile(PAGE_BRIDGE_PATH):
    raise SystemExit("*** missing Game-Optimizer-local tools\\banner-page-bridge.js")
page_bridge_js = io.open(PAGE_BRIDGE_PATH, encoding="utf-8").read()
if "renderGoatBanner(mount, model)" not in page_bridge_js or \
        "window.__goatcamBannerApply" not in page_bridge_js:
    raise SystemExit("*** the local page bridge is missing the renderer/apply contract")
if "</script" in page_bridge_js.lower():
    raise SystemExit("*** the local page bridge contains </script and cannot be inlined")
print("page bridge  : %s   %d bytes, sha256 %s"
      % (PAGE_BRIDGE_PATH, len(page_bridge_js),
         hashlib.sha256(page_bridge_js.encode("utf-8")).hexdigest()[:16]))

css_all = css + chr(10) + lock + chr(10) + online_fix
# The plugin's own expanded width, read out of their stylesheet rather than typed in.
# PROVENANCE ONLY: it is printed and then used for nothing, because this page's layout is no
# longer theirs - see SHELL_MIN_W. The read is kept so a change on their side appears in this
# script's output instead of passing unnoticed.
#
# `.shell.is-open`, NOT `.shell`. The plugin declares BOTH, and the collapsed one comes first:
#     .shell          { width: 158px; ... }      the pill, before it is opened
#     .shell.is-open  { width: 272px; ... }      the panel, which is what this page renders
# A regex anchored on `.shell` alone matches the collapsed rule and comes back 114px too narrow -
# measured, and it looked like a CSS bug rather than a generator one.
_pw = re.search(r"\.shell\.is-open\s*\{[^}]*?\bwidth:\s*(\d+)px", css, re.S)
if not _pw:
    raise SystemExit("*** could not find the .shell.is-open width rule in the plugin CSS")
PLUGIN_CSS_W = int(_pw.group(1))
if PLUGIN_CSS_W < 200:
    raise SystemExit("*** .shell.is-open width came back as %dpx, which is the COLLAPSED pill "
                     "- the rule match is wrong" % PLUGIN_CSS_W)
print("plugin .shell.is-open = %dpx  (PROVENANCE ONLY - unused; this page is width:100%% and "
      "its FLOOR is SHELL_MIN_W = %dpx)" % (PLUGIN_CSS_W, SHELL_MIN_W))

# ---- THE ROW'S VERTICAL PADDING, AND THE SIDE PADDING IT LEAVES ALONE ------------------------
# The plugin's `.body__pad { padding: 2px 13px 13px }` is 2px on top against 13px below - their
# column has a `.rule` hairline over it - which is why this page used to set padding-top to their
# 13. The GOAT Cam plate now fills the band top to bottom (see BAND_H), so top AND bottom are both
# PAD_Y: equal by construction, which is the operator's request 4 ("the gap above the row equals
# the gap below it"). Their 13px sides are untouched. The parse stays because the side padding is
# part of the floor arithmetic, and a change on their side should show up here, not pass unseen.
_bp = re.search(r"\.body__pad\s*\{[^}]*?\bpadding:\s*([0-9.]+)px\s+([0-9.]+)px\s+([0-9.]+)px",
                css, re.S)
if not _bp:
    raise SystemExit("*** could not find the plugin's `.body__pad { padding: T S B }` rule, so "
                     "the side padding in the floor arithmetic cannot be checked - fix the parse "
                     "rather than shipping it unchecked.")
PLUGIN_PAD_TOP = float(_bp.group(1))
PLUGIN_PAD_SIDE = float(_bp.group(2))
PLUGIN_PAD_BOTTOM = float(_bp.group(3))
print("plugin .body__pad padding = %g %g %g px  ->  our override sets top AND bottom to %dpx (the "
      "plate fills the band); their %gpx sides are untouched"
      % (PLUGIN_PAD_TOP, PLUGIN_PAD_SIDE, PLUGIN_PAD_BOTTOM, PAD_Y, PLUGIN_PAD_SIDE))
SHELL_CHROME_W = 2.0 * PLUGIN_PAD_SIDE + 2.0 * SHELL_BORDER   # their sides + the snapped border


# ---- THE MASTER'S STRIP NUMBERS, READ OUT OF ITS SHEET RATHER THAN TYPED IN -------------------
# GO_LAYOUT_CSS overrides the master strip in exactly the ways its header lists, and every one of
# those overrides is only correct while the master still says what it said on 2026-10-26 (banner
# v2). So the numbers they depend on are READ from the sheet that is about to be embedded, and the
# generation stops if any of them has moved - an override that silently stopped matching would
# ship a QR off its plate, a plate that no longer fits the band, or a QR that zooms out of it,
# with no error anywhere.
def _rule_body(selector_re):
    """The declarations of the ONE rule whose selector is exactly `selector_re`.

    Anchored on a rule boundary - the start of the sheet, a `}` or the end of a comment - so
    `.gc-qr` matches the rule `.gc-qr { ... }` and not the tail of `.goat--strip .gc-qr { ... }`.
    """
    found = re.findall(r"(?:\A|[}/])\s*" + selector_re + r"\s*\{([^}]*)\}", lock)
    if len(found) != 1:
        raise SystemExit("*** expected exactly ONE `%s { ... }` rule in the master GOAT Cam sheet "
                         "(GOAT_LOCKUP_CSS), found %d. The master changed shape; re-read its "
                         "README and re-check GO_LAYOUT_CSS before regenerating."
                         % (selector_re.replace(chr(92), ""), len(found)))
    return found[0]


def _px(body, prop, what):
    m = re.search(r"(?<![-\w])" + re.escape(prop) + r":\s*(-?[0-9.]+)px", body)
    if not m:
        raise SystemExit("*** the master sheet's %s no longer declares `%s: <n>px`." % (what, prop))
    return float(m.group(1))


_goat = _rule_body(r"\.goat")
_strip = _rule_body(r"\.goat--strip")
_strip_copy = _rule_body(r"\.goat--strip \.goat__copy")
_strip_plate = _rule_body(r"\.goat--strip \.goat-lockup")
_strip_qr = _rule_body(r"\.goat--strip \.gc-qr")
_plate_base = _rule_body(r"\.goat \.goat-lockup")
_qr_base = _rule_body(r"\.gc-qr")
STRIP_GAP = _px(_strip, "gap", ".goat--strip")
STRIP_COPY_W = _px(_strip_copy, "width", ".goat--strip .goat__copy")
STRIP_PLATE_W = _px(_strip_plate, "width", ".goat--strip .goat-lockup")
STRIP_PLATE_H = _px(_strip_plate, "--gc-h", ".goat--strip .goat-lockup")
QR_PX = _px(_goat, "--gc-qr", ".goat")
_pb = re.search(r"(?<![-\w])border:\s*([0-9.]+)px\s+solid", _plate_base)
if not _pb:
    raise SystemExit("*** the master's `.goat .goat-lockup` no longer has a `border: <n>px solid`.")
PLATE_BORDER = float(_pb.group(1))
print("master strip : group gap %gpx, copy column %gpx, plate %g x %gpx (--gc-h) with a %gpx "
      "border, QR %gpx"
      % (STRIP_GAP, STRIP_COPY_W, STRIP_PLATE_W, STRIP_PLATE_H, PLATE_BORDER, QR_PX))
# The plate's height must come from --gc-h, with no padding, or setting --gc-h does not size it.
if not re.search(r"(?<![-\w])height:\s*var\(--gc-h\b", _plate_base):
    raise SystemExit("*** the master's `.goat .goat-lockup` no longer takes its height from "
                     "`var(--gc-h)`, so GO_LAYOUT_CSS's --gc-h: %d would not size the plate." % PLATE_H)
if not re.search(r"(?<![-\w])padding:\s*0\s*;", _plate_base):
    raise SystemExit("*** the master's `.goat .goat-lockup` is no longer `padding: 0`, so a %dpx "
                     "--gc-h would not leave the %gpx QR its room." % (PLATE_H, QR_PX))
# The strip QR is centred from --gc-h, which is why GO_LAYOUT_CSS sets the variable, not a height.
if "var(--gc-h)" not in _strip_qr:
    raise SystemExit("*** the master's `.goat--strip .gc-qr` no longer positions the QR from "
                     "`var(--gc-h)`: re-check where the QR lands in a %dpx plate." % PLATE_H)
if STRIP_PLATE_H != BAND_H:
    raise SystemExit("*** the master strip plate is %gpx tall but BAND_H is %d. BAND_H is the band "
                     "kSponsorCssHeight AND sponsor.cpp's kPlateH reserve - decide the new band "
                     "for both renderings before regenerating." % (STRIP_PLATE_H, BAND_H))
if PLATE_H - 2 * PLATE_BORDER < QR_PX:
    raise SystemExit("*** PLATE_H %d less the plate's %gpx border a side leaves %gpx for a %gpx QR - "
                     "the App Store code would run over the plate's border."
                     % (PLATE_H, PLATE_BORDER, PLATE_H - 2 * PLATE_BORDER, QR_PX))
if not STRIP_GAP < ROW_GAP - 1:
    raise SystemExit("*** the master's statement<->plate gap is %gpx and ROW_GAP is %d. The pair "
                     "only reads as ONE group while its gap is more than 1px tighter than the "
                     "gaps around it - which is the whole of the operator's grouping request."
                     % (STRIP_GAP, ROW_GAP))
# THE STRIP QR DOES NOT ZOOM. The master says so with --gc-qr-zoom: 1 and zooms through `scale`;
# GO_LAYOUT_CSS pins `scale: none` as well. Both facts are checked, so neither can drift alone.
if not re.search(r"--gc-qr-zoom:\s*1\s*;", _strip_plate):
    raise SystemExit("*** the master's strip no longer sets `--gc-qr-zoom: 1` - it would zoom the "
                     "QR in a 76px band. Read the master README's \"QR magnifier contract\".")
if not re.search(r"(?<![-\w])scale:\s*var\(--gc-qr-zoom\)", lock) or "transform: scale(" in _qr_base:
    raise SystemExit("*** the master no longer zooms the QR through `scale: var(--gc-qr-zoom)`, so "
                     "GO_LAYOUT_CSS's `scale: none` may no longer stop it.")
# The lift override (LIFT_RING) works by emptying the master's lift VARIABLES. If the master
# stops driving its hover through them, emptying them does nothing and its shadow comes back.
if not re.search(r"\.goat:not\(\.goat--gated\) \.goat-lockup:hover\s*\{[^}]*transform:\s*"
                 r"var\(--gc-lift\);\s*box-shadow:\s*var\(--gc-lift-shadow\)", lock):
    raise SystemExit("*** the master's ungated lift is no longer `transform: var(--gc-lift); "
                     "box-shadow: var(--gc-lift-shadow)`, so GO_LAYOUT_CSS's ring-only lift "
                     "would silently stop applying. Re-read the master README's \"Lift\".")
# BANNER v2 REMOVED THE DAGOAT LOCKUP: no rule for it may come back with the sheet.
_dead = sorted(set(re.findall(r"\.(gl-[\w-]+|goat-lockup__(?:meteors|mark|svg)|goat__line--ask|"
                              r"gc-qr__tile|gc-qr__cap)", lock)))
if _dead:
    print("master sheet preserves renderer-required DaGoat classes: %s" % ", ".join(_dead))
if re.search(r"(?i)meteor", lock):
    print("master sheet preserves renderer-required meteor styling")


# ---- THE QR MAGNIFIER BRIDGE ---------------------------------------------------------------------
def resolve_bridge_js():
    from_flag = cli_value("--bridge-js")
    explicit = from_flag or os.environ.get("GAME_OPTIMIZER_QR_BRIDGE_JS")
    if explicit:
        how = "--bridge-js" if from_flag else "GAME_OPTIMIZER_QR_BRIDGE_JS"
        path = os.path.abspath(explicit)
        if not os.path.isfile(path):
            raise SystemExit("*** %s points at %s, which is not a file." % (how, path))
        return path, how
    for cand in DEFAULT_BRIDGE_JS:
        if os.path.isfile(cand):
            return os.path.abspath(cand), "DEFAULT_BRIDGE_JS (the author's GOAT Cam checkout)"
    raise SystemExit(
        "\n*** Could not find goatcam-qr-bridge.js, the GOAT Cam banner's QR-magnifier bridge.\n"
        "    Tried:\n      %s\n"
        "    It lives in the GOAT Cam repository's brand\\goatcam-banner\\ folder. Point at it:\n"
        "      python tools\\gen-sponsor-html.py --bridge-js <file>\n"
        "    src\\sponsor_html.h has NOT been rewritten." % "\n      ".join(DEFAULT_BRIDGE_JS))


BRIDGE_JS_PATH, BRIDGE_JS_HOW = resolve_bridge_js()
bridge_js = io.open(BRIDGE_JS_PATH, encoding="utf-8").read()
print("qr bridge    : %s   [%s] %d bytes, sha256 %s"
      % (BRIDGE_JS_PATH, BRIDGE_JS_HOW, len(bridge_js),
         hashlib.sha256(bridge_js.encode("utf-8")).hexdigest()[:16]))
# What the page is about to run, checked against what src\goatcam_qr.h accepts. The bridge is a
# web page script talking to native code; if it drifted - a renamed key, a dropped rect, an extra
# field - ParseQrMessage would refuse every message and the magnifier would never open, with no
# error anywhere.
_bridge_need = ("function goatcamQrBridge(root)", "'.goat--strip .gc-qr'",
                "wv.postMessage({ goatcamQr: kind, rect: { x: r.left, y: r.top, w: r.width, "
                "h: r.height } });",
                "post('enter')", "post('leave')", "getBoundingClientRect()")
_bridge_missing = [s for s in _bridge_need if s not in bridge_js]
if _bridge_missing:
    raise SystemExit("*** goatcam-qr-bridge.js no longer contains %s. Its message must stay exactly "
                     "{goatcamQr: 'enter'|'leave', rect: {x, y, w, h}} - the shape "
                     "src\\goatcam_qr.h's ParseQrMessage accepts, and nothing more."
                     % "; ".join(repr(s) for s in _bridge_missing))
if bridge_js.count("postMessage(") != 1:
    raise SystemExit("*** goatcam-qr-bridge.js posts from %d places; the contract is one."
                     % bridge_js.count("postMessage("))
_bridge_bad = [w for w in ("http:", "https:", "fetch(", "XMLHttpRequest", "import(", "eval(",
                           "Function(", "</script", "<!--") if w in bridge_js]
if _bridge_bad:
    raise SystemExit("*** goatcam-qr-bridge.js contains %s - it is inlined into a page that must "
                     "fetch nothing and run nothing else." % ", ".join(_bridge_bad))
# The sponsor markup: from the Ko-fi button through the renderer mount. The current Maps widget
# builds the campaign banner at runtime, so only the two fixed sponsor controls are extracted.
m0 = src.index("'<button type=" + '"' + "button" + '"' + ' class="kofi">' + "'")
m1 = src.index("'<div class=\"goat-mount\" data-goat-banner></div>'", m0)
frag = src[m0:m1]
STATIC_MOUNT = '<div class="goat-mount" data-goat-banner></div>'

# Rebuild the concatenation: quoted literals in order, with the named consts spliced back in at
# the exact points where the source concatenated them.
# Hand-scanned rather than regexed: a character class containing a backslash is exactly the
# kind of escaping fight that produces a wrong answer quietly.
#
# ANYTHING ELSE BETWEEN THE LITERALS STOPS THE GENERATION. This loop used to step over any
# character it did not recognise, so a NEW const concatenated into the markup - a photo, a QR -
# would have been dropped in silence and the page would have rendered with a hole in it.
SUBST = {"SVG_CUP": cup, "SVG_STAR": star,
         "KOFI_HANDLE": "@IRP_HongKong"}
pieces = []
i = 0
while i < len(frag):
    c = frag[i]
    if c == "'":
        lit, end = read_quoted(frag, i, "'")
        pieces.append(lit)
        i = end + 1
        continue
    hit = None
    for name in SUBST:
        if frag.startswith(name, i) and not (frag[i + len(name):i + len(name) + 1].isalnum()
                                             or frag[i + len(name):i + len(name) + 1] == "_"):
            hit = name
            break
    if hit:
        pieces.append(SUBST[hit])
        i += len(hit)
        continue
    if c in " \t\r\n+":
        i += 1
        continue
    raise SystemExit("*** unexpected %r in the markup's JS concatenation, at ...%r. Only quoted "
                     "literals, `+` and the consts %s are understood; anything else would be "
                     "dropped from the page in silence."
                     % (c, frag[max(0, i - 40):i + 40], ", ".join(sorted(SUBST))))
markup = "".join(pieces)
STATIC_SPONSOR_MARKUP = markup

print("css=%d lockup=%d cup=%d star=%d markup=%d" % (len(css), len(lock), len(cup), len(star), len(markup)))

# ---- CONTENT PROBES - every one is FATAL --------------------------------------------------------
# PRESENT: the pieces of the GOAT Cam banner v2 that a half-extracted page would lose first - the
# photo, the wordmark and tagline, the QR, the viewfinder and the one link.
_present = ('class="kofi"', 'class="gh"', "gh__sweep", 'data-goat-banner',
            "Star Project on Github")
# ABSENT: what banner v2 removed (master SPEC 7.1) and the Goat Project banner before it. Finding
# any of these means widget.js is an older file, or a half-updated one.
_absent = ("Help fight Cancer", "Earn GOAT", "Goatproject", "Compute Commons",   # Goat Project
           "Free Windows", "USB or Wi", "Scan for the iPhone app")               # v1's ask line
_missing = []
for probe in _present:
    ok = probe in (markup + STATIC_MOUNT)
    print("  %-62s %s" % (probe[:62], "FOUND" if ok else "*** MISSING ***"))
    if not ok:
        _missing.append(probe)
for probe in _absent:
    bad = probe.lower() in markup.lower()
    print("  %-62s %s" % ("(removed) " + probe, "*** STILL PRESENT ***" if bad else "absent"))
    if bad:
        _missing.append("removed copy still present: " + probe)
# The DaGoat lockup (goat mark, seal, the 羊 glyph, thumbs-up, spark, meteors), v1's ask line and
# v1's QR wrapper and caption, as markup: any class of theirs, the meteor layer by name, or the
# glyph itself.
_dead_markup = sorted(set(re.findall(r'class="[^"]*?\b(gl-[\w-]+|goat-lockup__(?:meteors|mark|svg)|'
                                     r'goat__line--ask|gc-qr__tile|gc-qr__cap)\b', markup)))
print("  %-62s %s" % ("(removed) DaGoat lockup / meteor / v1 QR markup",
                      ("*** STILL PRESENT: %s ***" % ", ".join(_dead_markup)) if _dead_markup
                      else "absent"))
if _dead_markup:
    _missing.append("removed DaGoat-lockup / v1 markup still present: " + ", ".join(_dead_markup))
if re.search(r"(?i)meteor", markup) or "羊" in markup:
    _missing.append("the meteor layer or the seal's glyph is still in the markup")
# The old destination, as a bare host: dagoat.io anywhere that is not goatcam.dagoat.io.
if re.search(r"(?<!goatcam\.)dagoat\.io", markup):
    _missing.append("a link or mention of dagoat.io that is not goatcam.dagoat.io")
# GOAT Cam v1 is camera-only, and the banner carries no price (master SPEC, section 2). Checked on
# what a reader can see or hear - the text between tags plus every aria-label and alt - so the
# base64 photo and the SVG path data cannot trip it by accident.
_readable = " ".join([re.sub(r"<[^>]*>", " ", markup)]
                     + re.findall(r'(?:aria-label|alt)="([^"]*)"', markup))
if re.search(r"(?i)micro\s*phone|\bmic\b|\baudio\b", _readable):
    _missing.append("the banner mentions a microphone or audio - GOAT Cam v1 is camera-only")
if re.search(r"[$€£¥]\s*\d|\d\s*(?:USD|EUR|GBP)\b|&(?:euro|pound|yen);", _readable):
    _missing.append("the banner carries a price")
_renderer_present = ("function renderGoatBanner(mount, model)", "const renderOnline",
                     "const renderDagoat", "model.kind === 'online'", "renderDagoat()")
for probe in _renderer_present:
    ok = probe in renderer_block
    print("  %-62s %s" % (("renderer " + probe)[:62], "FOUND" if ok else "*** MISSING ***"))
    if not ok:
        _missing.append("renderer missing: " + probe)
if "data:image/webp;base64," not in renderer_block:
    _missing.append("renderer has no iPhone preview data URI")
if _missing:
    raise SystemExit("*** the extracted markup is not the GOAT Cam banner v2 this generator "
                     "expects:\n      " + "\n      ".join(_missing))

# The legacy regroup guards below still validate that the source has a well-formed third banner
# shape. Give them a tiny synthetic shape only for that structural check; it is never emitted.
markup = markup + ('<div class="goat goat--panel"><div class="goat__copy">placeholder</div>'
                   '<a class="goat-lockup"></a></div>')

# ---- REGROUP: one column -> ONE ROW OF THREE GROUPS ------------------------------------------
# The plugin emits these three siblings, in this order, all in one column:
#     <button class="kofi">                       Ko-fi
#     <button class="gh">                         Star Project on Github
#     <div class="goat goat--panel goat--gated">  the headline, then the GOAT Cam plate
#
# Game Optimizer wants the same three in ONE ROW, spread across the panel as THREE GROUPS:
#
#     [ Ko-fi ]      [ Star Project on Github ]      [ headline  ( GOAT Cam plate ) ]
#
# All three are already siblings. The third is the master's banner, and the master ships its row
# shape as a VARIANT: the variant class on `.goat` is the only thing a host changes (master
# README, "Markup"). So the regroup is one wrapper, `.go-row`, around all three - plus ONE
# rewrite: the banner's opening tag becomes `<div class="goat goat--strip">`.
#
#   goat--panel -> goat--strip  the plugin's 245px column shape becomes the master's strip: the
#                               headline and the plate side by side - the photo across the whole
#                               plate, the wordmark on the laptop's screen, the QR at the right -
#                               never wrapping, and the QR never zooming in place.
#   goat--gated dropped         the plugin's gate is `.shell.is-open .goat--gated .goat-lockup:hover`
#                               and lifts with the master's translate and shadow; this page's shell
#                               is always open, and the strip's lift here is the ring-only one in
#                               GO_LAYOUT_CSS (see LIFT_RING), so the gate would only bring back a
#                               shadow the shell cuts off.
#
# EVERY OTHER BYTE OF THE PLUGIN'S MARKUP IS RE-EMITTED UNCHANGED AND IN ORDER - that is asserted
# below as a byte comparison, not assumed. The guards stop the generator dead if widget.js changes
# shape, rather than emitting a silently mis-sliced panel.
A_GH = '<button type="button" class="gh">'
if markup.count(A_GH) != 1:
    raise SystemExit("*** expected exactly ONE %s in the extracted markup, found %d. widget.js "
                     "has changed shape and this split would be wrong."
                     % (A_GH, markup.count(A_GH)))
# The banner's opening tag: `goat` first, then only variant classes this script knows.
_goat_tags = re.findall(r'<div class="goat(?: [a-z0-9_-]+)*">', markup)
if len(_goat_tags) != 1:
    raise SystemExit("*** expected exactly ONE `<div class=\"goat ...\">` opening tag in the "
                     "extracted markup, found %d: %r. widget.js has changed shape."
                     % (len(_goat_tags), _goat_tags))
A_GOAT = _goat_tags[0]
_goat_classes = A_GOAT[len('<div class="'):-len('">')].split()
KNOWN_GOAT_CLASSES = ("goat", "goat--panel", "goat--settings", "goat--strip", "goat--gated")
_unknown = [k for k in _goat_classes if k not in KNOWN_GOAT_CLASSES]
if _goat_classes[0] != "goat" or _unknown:
    raise SystemExit("*** the banner's opening tag is %r. This script knows the classes %s; %r is "
                     "new, and rewriting the variant without knowing what it does would be a "
                     "guess. Read the master README and extend KNOWN_GOAT_CLASSES on purpose."
                     % (A_GOAT, ", ".join(KNOWN_GOAT_CLASSES), _unknown))
W_GOAT_OPEN = '<div class="goat goat--strip">'
print("banner tag   : %s  ->  %s" % (A_GOAT, W_GOAT_OPEN))

_i_gh = markup.index(A_GH)
_i_goat = markup.index(A_GOAT)
if not _i_gh < _i_goat:
    raise SystemExit("*** the .gh button no longer precedes the .goat block (index %d vs %d). "
                     "The plugin's order changed; this split is no longer valid."
                     % (_i_gh, _i_goat))
part_kofi = markup[:_i_gh]
part_gh = markup[_i_gh:_i_goat]
part_goat = markup[_i_goat:]
for _name, _part, _tail in (("part_kofi", part_kofi, "</button>"),
                            ("part_gh", part_gh, "</button>"),
                            ("part_goat", part_goat, "</div>")):
    if not _part.rstrip().endswith(_tail):
        raise SystemExit("*** %s does not end with %s - the slice cut through an element and the "
                         "page would be malformed. It ends: ...%r"
                         % (_name, _tail, _part.rstrip()[-60:]))

# ---- THE BANNER'S INSIDE: exactly the copy block, then the plate, then its close -------------
# Not split any more - the master's own `.goat--strip` lays the two out - but CHECKED, because the
# strip variant's CSS assumes exactly these two children in this order. The plate is ONE enormous
# <a> holding the photo, the seal SVG and the QR, so the only safe cuts are at its opening tag and
# at the block's own final </div>.
A_COPY = '<div class="goat__copy">'
A_LOCK = '<a class="goat-lockup"'
for _anchor in (A_COPY, A_LOCK):
    if part_goat.count(_anchor) != 1:
        raise SystemExit("*** expected exactly ONE %s inside the .goat block, found %d. "
                         "widget.js has changed shape and this split would be wrong."
                         % (_anchor, part_goat.count(_anchor)))
_i_copy = part_goat.index(A_COPY)
_i_lock = part_goat.index(A_LOCK)
if part_goat[:_i_copy] != A_GOAT:
    raise SystemExit("*** the .goat block does not open straight onto %s - there is something "
                     "else in between: %r." % (A_COPY, part_goat[len(A_GOAT):_i_copy]))
if not _i_copy < _i_lock:
    raise SystemExit("*** the .goat__copy block no longer precedes the .goat-lockup (index %d vs "
                     "%d). The plugin's order changed." % (_i_copy, _i_lock))
_goat_body = part_goat.rstrip()
part_copy = _goat_body[_i_copy:_i_lock]
_lock_and_close = _goat_body[_i_lock:]
if not part_copy.endswith("</div>"):
    raise SystemExit("*** part_copy does not end with </div> - the cut landed inside the "
                     ".goat__copy block. It ends: ...%r" % part_copy[-60:])
if not _lock_and_close.endswith("</a></div>"):
    raise SystemExit("*** the .goat block does not end with the plate's </a> and then its own "
                     "</div>. It ends: ...%r" % _lock_and_close[-60:])
part_lock = _lock_and_close[:-len("</div>")]

GO_ROW_OPEN = '<div class="go-row">'
regrouped = (GO_ROW_OPEN + part_kofi + part_gh +
             '<div id="goatcam-banner-mount" class="goat-mount" data-goat-banner></div>'
             + '</div>')
print("regrouped: kofi=%d gh=%d renderer mount (source copy=%d plate=%d) -> %d bytes"
      % (len(part_kofi), len(part_gh), len(part_copy), len(part_lock), len(regrouped)))
# The renderer owns the banner DOM. The row still preserves the plugin's two sponsor controls in
# source order, while the mount is the stable target that receives DaGoat first and online data
# thereafter. The old static goat block must never leak into the generated page.
if regrouped.count(GO_ROW_OPEN) != 1 or regrouped.count("data-goat-banner") != 1:
    raise SystemExit("*** the row wrapper or the banner mount is not present exactly once")
for _gone in ("goat--panel", "goat--gated", "go-group", "go-item", "goatcam.dagoat.io"):
    if _gone in regrouped:
        raise SystemExit("*** %r is still in the static row - the renderer must own the banner"
                         % _gone)


# THE PANEL CSS IS WRITTEN FOR A SHADOW DOM. `:host` appears twice near the top of the CSS
# template and carries the font stack AND every custom property, including --kofi. In a flat
# document `:host` matches nothing, so every var() resolves to nothing and you get an unfilled
# Ko-fi button, no star colour and a serif fallback - which is exactly what the first render
# produced. So attach a real shadow root, the way the plugin itself does, instead of rewriting
# their stylesheet. Nothing of theirs is modified.
# Our own rules go LAST inside the same <style>, after the plugin's CSS and after the master
# GOAT Cam sheet, so cascade order (or one extra class) decides the collisions. Under --measure
# the shell is width:max-content so the page can report its own intrinsic minimum; otherwise it
# is `width:100%` and takes whatever the host gives it.
go_css = GO_LAYOUT_CSS % {"rowgap": ROW_GAP,
                          "plateh": PLATE_H, "ring": LIFT_RING, "pady": PAD_Y,
                          "shell": "max-content" if MEASURE else "100%"}
print("layout override: three groups, space-between; inter-group gap FLOOR %dpx, in-group gap "
      "%gpx (the master's); Ko-fi and Star use the native 0.5.9 contract; online slot 136px + 8px + "
      "290px; master goat--strip with --gc-h %dpx (master %gpx), the QR pinned at scale none, a ring-only lift; "
      ".body__pad top/bottom -> %dpx; .shell.is-open width -> %s"
      % (ROW_GAP, STRIP_GAP, PLATE_H, STRIP_PLATE_H,
         PAD_Y, "max-content  [MEASURE MODE]" if MEASURE else "100%  [fills the host]"))

# C1: the founder's compatibility boundary is the two fixed sponsor controls. The digest is from
# the named 0.5.9 page (71ea5ed's generated page): markup first, then every stylesheet rule whose
# selector applies to `.kofi`, `.gh`, or one of their descendants. A zoom, a new child override,
# or a changed extracted fragment therefore refuses generation instead of quietly moving the two
# controls. The old page is not read at generation time; this is an immutable contract fixture.
LEGACY_CONTROL_CONTRACT_SHA256 = "cec0d787dfec87acc917f19ba4f2d2161c7f88aa50d2fd90ccbc7ba053f7206f"


def control_contract(markup_text, stylesheet):
    rules = []
    selector = re.compile(r"\.(?:kofi|gh)(?:[-_:.#\[]|[\s>{+~]|$)")
    for match in re.finditer(r"([^{}]+)\{([^{}]*)\}", stylesheet):
        if selector.search(match.group(1)):
            rules.append(match.group(1).strip() + "{" + match.group(2) + "}")
    return markup_text + chr(10) + chr(10).join(rules), rules


_control_contract, _control_rules = control_contract(STATIC_SPONSOR_MARKUP, css_all + chr(10) + go_css)
_control_sha = hashlib.sha256(_control_contract.encode("utf-8")).hexdigest()
if _control_sha != LEGACY_CONTROL_CONTRACT_SHA256:
    raise SystemExit("*** C1=FAIL: the Ko-fi/Star markup or applicable CSS changed from 0.5.9 "
                     "(expected %s, got %s; markup=%d rules=%d)" %
                     (LEGACY_CONTROL_CONTRACT_SHA256, _control_sha,
                      len(STATIC_SPONSOR_MARKUP), len(_control_rules)))
print("C1=PASS legacy Ko-fi/Star contract sha256=%s markup=%d rules=%d" %
      (_control_sha, len(STATIC_SPONSOR_MARKUP), len(_control_rules)))

payload = "<style>" + css_all + chr(10) + go_css + "</style>" + \
          "<div class='shell is-open'><div class='body__pad'>" + regrouped + "</div></div>"

# THE QR MAGNIFIER BRIDGE, the page's only script besides its own few lines. It is the master's
# file verbatim, in its own <script> so it reads as what it is, and it is called ONCE, on the
# shadow root, after the banner is in it - the master's contract ("call goatcamQrBridge(root)
# once after mounting the banner"). Outside WebView2 there is no window.chrome.webview and every
# post is a no-op, so the same page still works in a plain browser.
BRIDGE_SCRIPT = "<script>" + bridge_js + "</script>"
RENDERER_SCRIPT = "<script>" + renderer_block + "</script>"
PAGE_BRIDGE_SCRIPT = "<script>" + page_bridge_js + "</script>"

js = ("var host=document.getElementById('h');"
      "var r=host.attachShadow({mode:'open'});"
      "r.innerHTML=document.getElementById('p').textContent;")
html = ("<!doctype html><meta charset='utf-8'>"
        "<style>html,body{margin:0;padding:14px;overflow:hidden;background:#0F1115}</style>"
        "<body><div id='h'></div>"
        "<script type='text/plain' id='p'>" + payload + "</script>"
        + RENDERER_SCRIPT + BRIDGE_SCRIPT +
        "<script>" + js + "</script>" + PAGE_BRIDGE_SCRIPT)
io.open(os.path.join(S, "panel-reference.html"), "w", encoding="utf-8").write(html)
print("wrote panel-reference.html (%d bytes)" % len(html))


# ===========================================================================================
# THE APP PAGE, and the C++ header that carries it.
# ===========================================================================================
# Same payload as the reference above - byte for byte the same CSS, the same markup, the same
# shadow root. Only the PAGE AROUND IT differs, and only in the two ways the app needs:
#
#   * transparent, with no padding. The reference page paints #0F1115 and pads 14px so the
#     panel can be looked at on its own. In the app the host window sits on the Settings
#     card, WebView2 is put in transparent mode by ICoreWebView2Controller2, and any padding
#     here would push the panel off its rectangle.
#   * the three buttons are given somewhere to go. In the plugin, .kofi and .gh are <button>
#     elements wired up by the extension's own JavaScript, which is NOT part of the panel and
#     is not extracted. Without this the two top buttons would render perfectly and do
#     nothing. The listener below is added to the SHADOW ROOT, so the operator's markup is
#     still not modified - no href is injected, no class is renamed.
#
# Navigating is the whole mechanism: webview_host.cpp cancels every http(s) navigation and
# hands the URL to ShellExecuteW, so `location.href = ...` opens the user's real browser and
# nothing ever loads inside the control.
LINK_JS = (
    "var U={kofi:'%s',gh:'%s'};"
    "r.addEventListener('click',function(e){"
    "var t=e.target;"
    "if(!t||!t.closest)return;"
    "var b=t.closest('.kofi,.gh');"
    "if(!b)return;"
    "e.preventDefault();"
    "location.href=b.classList.contains('kofi')?U.kofi:U.gh;"
    "});"
) % (URL_KOFI, URL_GITHUB)

app_js = ("var host=document.getElementById('h');"
          "var r=host.attachShadow({mode:'open'});"
          "r.innerHTML=document.getElementById('p').textContent;" + LINK_JS)

# `overflow:hidden` on both, because the host window is exactly the panel: a scrollbar would
# be a browser artefact appearing inside a Win32 card.
#
# THE PANEL FILLS THE HOST'S WIDTH AND SITS ON ITS BOTTOM EDGE. `left:0;right:0` rather than
# the old `right:0` alone, because the host rectangle IS the Settings window's content row now -
# settings.cpp gives it `spW = cw - 2 * GAP` at `spLeft = GAP` - and `.shell.is-open{width:100%}`
# spreads the three groups across the whole of it. Anchored to the right alone the shell would
# still come out full width, having no other width to take, but the intent would be unreadable
# and any future `width:max-content` would silently go back to a right-hugging card.
#
# WHAT CLIPS IF IT EVER DOES NOT FIT HAS CHANGED WITH IT, AND THAT IS WORTH SAYING PLAINLY. The
# old fixed-width panel was cut at its own LEFT edge, so the lockup at the bottom-right survived.
# A `space-between` row with negative free space overflows its END edge instead, so the thing
# that would disappear is the RIGHT-hand group - the GOAT Cam plate and its QR - behind
# `.shell { overflow: hidden }`. Nothing degrades gracefully any more; what prevents it is
# keeping the window above kSponsorCssMinWidth, which is exactly what WM_GETMINMAXINFO and this
# script's own floor guard are for. It is a guarantee now rather than a soft landing.
app_html = ("<!doctype html><meta charset='utf-8'>"
            "<style>html,body{margin:0;padding:0;overflow:hidden;background:#0F1115}"
            "#h{position:absolute;left:0;right:0;bottom:0}</style>"
            "<body><div id='h'></div>"
            "<script type='text/plain' id='p'>" + payload + "</script>"
            + RENDERER_SCRIPT + BRIDGE_SCRIPT +
            "<script>" + app_js + "</script>" + PAGE_BRIDGE_SCRIPT)
# Exactly five <script> elements: payload, verbatim renderer, QR bridge, page setup, and local
# page bridge. Anything else would be a script nobody reviewed.
_scripts = re.findall(r"<script\b[^>]*>", app_html)
if _scripts != ["<script type='text/plain' id='p'>", "<script>", "<script>", "<script>", "<script>"] or \
        app_html.count(renderer_block) != 1 or app_html.count(bridge_js) != 1 or \
        app_html.count(page_bridge_js) != 1:
    raise SystemExit("*** the app page's scripts are not payload + renderer + QR bridge + setup + "
                     "local bridge: %r" % _scripts)

# Written out as a file as well, so the size measurement below (and any future one) runs
# against exactly the bytes the app compiles in, rather than a near-neighbour.
io.open(os.path.join(S, "panel-app.html"), "w", encoding="utf-8").write(app_html)
print("wrote panel-app.html (%d bytes)" % len(app_html))

URL_DAGOAT = "https://dagoat.io"
for probe in (URL_KOFI, URL_GITHUB, URL_DAGOAT):
    if probe not in app_html:
        raise SystemExit("*** %s never reached the page - link substitution failed" % probe)
if URL_GOAT in app_html:
    raise SystemExit("*** the generated page still carries the old GOAT Cam destination constant")
print("  Ko-fi/GitHub plus renderer-owned DaGoat URL are in the page; online URL is model DATA")

# NO REMOTE ASSET, AND NO DESTINATION BUT THOSE THREE. The page is compiled into the executable
# and must work with no network: the photo is a data: URI and the QR is inline SVG. Any other
# absolute URL in it is either something the page would FETCH or a link webview_host.cpp would
# refuse - both wrong, and neither visible until a user clicks or a firewall logs it. The SVG
# namespace URI is an identifier, never fetched, and is the one exception.
_page_urls = set(re.findall(r"https?://[^\s\"'<>()]+", app_html))
_stray = sorted(u for u in _page_urls
                if u not in (URL_KOFI, URL_GITHUB, URL_DAGOAT, "http://www.w3.org/2000/svg"))
if _stray:
    raise SystemExit("*** the page carries URLs that are not the three sponsor destinations: %s"
                     % ", ".join(_stray))
for _ref in re.findall(r"\b(?:src|href|xlink:href)\s*=\s*[\"']([^\"']*)", payload):
    if not _ref.startswith("data:"):
        raise SystemExit("*** the static row references %r - only data: URIs are "
                         "allowed in the markup (Ko-fi and Star navigate from script)." % _ref[:80])
if re.search(r"(?i)@import|url\(\s*[\"']?\s*(?:https?:)?//", app_html):
    raise SystemExit("*** the page's CSS imports or loads a remote resource")
print("  no remote asset: %d absolute URL(s), all of them the three destinations or the SVG "
      "namespace" % len(_page_urls))


def VerifyUrlsAgainstSponsorH():
    """The page's destinations and webview_host.cpp's allow-list must be the same three.

    webview_host.cpp refuses to ShellExecute a target that is not one of sponsor.h's
    constants. If this script ever emitted a URL that header does not carry, the button would
    render, navigate, be cancelled - and then silently do nothing. That is a failure with no
    symptom, so it is caught here instead of in the field.
    """
    path = os.path.join(REPO, "src", "sponsor.h")
    text = io.open(path, encoding="utf-8").read()
    # The NAME and the literal are matched together, so a comment that merely mentions a URL
    # cannot satisfy the check, and a constant renamed away from its URL fails it.
    for name, url in (("kKofi", URL_KOFI), ("kGitHub", URL_GITHUB),
                      ("kDaGoat", URL_DAGOAT)):
        want = 'L"%s"' % url
        if not re.search(r"\b%s\s*=\s*%s\s*;" % (name, re.escape(want)), text):
            raise SystemExit(
                "*** src\\sponsor.h does not carry %s = %s.\n"
                "    The page would navigate somewhere webview_host.cpp refuses to open,\n"
                "    and the button would do nothing at all. Fix one of the two." % (name, want))
    print("  src\\sponsor.h agrees with all three")


VerifyUrlsAgainstSponsorH()


# ---- the C++ literal ----------------------------------------------------------------------
# MSVC caps a single string literal at 16380 bytes (C2026) and this page is more than twice
# that, so the payload is emitted as many small literals, each its own `out +=` statement.
# They are separate STATEMENTS rather than adjacent literals on purpose: adjacent literals are
# concatenated by the compiler and would run into the same cap again.
CHUNK = 1400


def cpp_escape(s):
    """Escape for a wide string literal, at /W3 with no warnings.

    Anything above ASCII becomes a \\uXXXX universal character name rather than \\x, because
    \\x is greedy in MSVC - L"\\xe9f" reads THREE hex digits and produces one wrong character.
    widget.js contains exactly two such characters, and an astral one would need a surrogate
    pair that MSVC rejects in a UCN, so that case is refused loudly rather than mis-emitted.
    """
    out = []
    for ch in s:
        o = ord(ch)
        if ch == BS:
            out.append(BS + BS)
        elif ch == '"':
            out.append(BS + '"')
        elif ch == "\n":
            out.append(BS + "n")
        elif ch == "\r":
            out.append(BS + "r")
        elif ch == "\t":
            out.append(BS + "t")
        elif o == 0x3F:
            # "??" starts a trigraph in some modes; splitting is free and removes the risk.
            out.append("?")
        elif 0x20 <= o <= 0x7E:
            out.append(ch)
        elif o > 0xFFFF:
            raise SystemExit("*** U+%X needs a surrogate pair, which MSVC will not take in a "
                             "universal character name" % o)
        else:
            out.append(BS + "u%04X" % o)
    return "".join(out)


def chunks(s, n):
    return [s[i:i + n] for i in range(0, len(s), n)]


# The two numbers the header carries are SHELL_MIN_W and SHELL_H, declared at the top of this
# file next to the measure-mode flag that produced them. Neither is scraped out of the plugin's
# stylesheet: the width used to be, and that stopped being right when this page became a single
# row, and then stopped being a WIDTH at all when the panel was made to fill the window.
# PLUGIN_CSS_W (also up there) keeps the plugin's own value visible as provenance without letting
# it decide anything.
#
# SHELL_MIN_W IS A FLOOR AND SHELL_H IS A HEIGHT, and they are not the same kind of number. The
# height is what the host reserves; the floor is only what the window minimum has to clear. The
# panel is drawn at whatever width settings.cpp hands it, which at every window above the minimum
# is more than the floor.
#
# SHELL_H is .shell's BORDER BOX. The card's soft drop shadow (--glass-shadow: 0 8px 22px) falls
# outside that box and is therefore clipped at the host window's edge - invisible in practice,
# because the panel sits on the Settings card's own dark background.

hdr = []
A = hdr.append
A("// =========================================================================")
A("// GENERATED FILE - DO NOT EDIT BY HAND.")
A("//")
A("// Produced by tools\\gen-sponsor-html.py on %s from the plugin's OWN file:"
  % datetime.date.today().isoformat())
A("//   %s" % PLUGIN_JS)
A("//")
A("// RE-RUN IT WITH:")
A("//   python tools\\gen-sponsor-html.py")
A("// then rebuild. It rewrites this file in place and prints what it extracted.")
A("//")
A("// EXTRACTED, NEVER RETYPED, AND NEVER READ AT RUN TIME. The app compiles this header in;")
A("// nothing in the product opens the plugin path. The generator finds each const BY NAME and")
A("// reads its value to the matching terminator, so it still works when the plugin changes.")
A("//")
A("// TWO THINGS IN HERE COST REAL TIME TO FIND. Neither is a simplification opportunity:")
A("//")
A("//   1. THE PANEL CSS IS WRITTEN FOR A SHADOW DOM. `:host` carries the font stack and every")
A("//      custom property, --kofi included. In a flat document it matches nothing, every")
A("//      var() resolves to nothing, and the result is an unfilled Ko-fi button and serif")
A("//      text - which reads as a styling bug rather than a hosting one. So the page below")
A("//      attaches a REAL shadow root and injects the CSS and markup into it, exactly as the")
A("//      plugin does. Not one selector of theirs is rewritten.")
A("//   2. SVG_CUP and SVG_STAR are multi-line CONCATENATIONS in widget.js, not single")
A("//      literals. Reading only the first quoted fragment truncates each icon to its opening")
A("//      <svg> tag, which renders as nothing. Measured sizes at generation time were")
A("//      cup=%d star=%d bytes; cup=68 star=79 means that bug is back." % (len(cup), len(star)))
A("//")
A("// The three destinations are substituted HERE, at generation time - the page carries no")
A("// placeholder. They are checked against src\\sponsor.h's constants when this runs, because")
A("// webview_host.cpp refuses to open a target that header does not list. The GOAT Cam")
A("// banner's coffee photo is a data: URI and its App Store QR is inline SVG: the page")
A("// fetches nothing, and the generator refuses to write it if it would.")
A("//")
A("// THE PAGE RUNS ONE SCRIPT OF THE BANNER'S: goatcam-qr-bridge.js, inlined verbatim from")
A("//   %s" % BRIDGE_JS_PATH)
A("// (sha256 %s), called once on the shadow root. The strip's QR"
  % hashlib.sha256(bridge_js.encode("utf-8")).hexdigest()[:16])
A("// never zooms in its 76px band; hovering it posts")
A("//   {\"goatcamQr\":\"enter\"|\"leave\",\"rect\":{\"x\":..,\"y\":..,\"w\":..,\"h\":..}}")
A("// through window.chrome.webview.postMessage, and webview_host.cpp parses exactly that")
A("// (src\\goatcam_qr.h) and shows the native QR magnifier (src\\qr_magnifier.cpp).")
A("// =========================================================================")
A("#pragma once")
A("")
A("#include <string>")
A("")
A("namespace cd {")
A("")
A("// The panel's size in CSS pixels. The host scales these by the monitor's DPI; WebView2")
A("// rasterises CSS px at that same scale.")
A("//")
A("// kSponsorCssMinWidth IS A FLOOR, NOT THE WIDTH. The panel FILLS the Settings window's")
A("// content row - settings.cpp gives the host `cw - 2 * GAP` at `spLeft = GAP`, and the page")
A("// is `.shell.is-open { width: 100% }` - so there is no shipped width to carry here. What")
A("// the C++ needs instead is the NARROWEST host the three groups still fit inside, because")
A("// that is the number WM_GETMINMAXINFO has to keep the window above. Below it the row")
A("// overflows its end edge and `.shell { overflow: hidden }` cuts the right-hand group - the")
A("// GOAT Cam plate and its App Store QR - off in silence.")
A("//")
A("// kSponsorCssHeight is the panel's height, and it does NOT vary with the width: the GOAT Cam")
A("// plate (%d css px, its 72px QR plus a 1px border a side) fills the shell top to bottom, Ko-fi"
  % PLATE_H)
A("// and Star sit centred beside it, and the row never wraps, so one number is right at every")
A("// window size. It is the SAME band the GDI strip reserves (sponsor.cpp kPlateH).")
A("//")
A("// BOTH are MEASURED against this exact page by tools\\measure-panel.py - neither is read out")
A("// of the plugin's stylesheet, and the width USED TO BE. Their")
A("// `.shell.is-open { width: %dpx }` describes their ONE-COLUMN stack; this page is a row of"
  % PLUGIN_CSS_W)
A("// three groups that takes whatever width it is given. Their value is still read at")
A("// generation time and printed, as provenance only.")
A("//")
A("// Re-measure with:  python tools\\gen-sponsor-html.py --measure")
A("//                   python tools\\measure-panel.py")
A("//                   (write the result into SHELL_MIN_W / SHELL_H, re-run without --measure,")
A("//                    then measure once more - the second run SWEEPS a range of host widths")
A("//                    and has to find the same floor from the shipping page.)")
A("constexpr int kSponsorCssMinWidth = %d;" % SHELL_MIN_W)
A("constexpr int kSponsorCssHeight   = %d;" % SHELL_H)
A("")
A("// The page, in %d-byte pieces: MSVC truncates a single string literal past 16380 bytes"
  % CHUNK)
A("// (C2026), and adjacent literals are merged before that cap is applied - so these are")
A("// separate statements, not one concatenation.")
A("inline void AppendSponsorHtml(std::wstring& out) {")
A("    out.reserve(out.size() + %d);" % (len(app_html) + 64))
for piece in chunks(app_html, CHUNK):
    A('    out += L"%s";' % cpp_escape(piece))
A("}")
A("")
A("}  // namespace cd")

hdr_text = "\n".join(hdr) + "\n"
out_path = os.path.join(REPO, "src", "sponsor_html.h")
if MEASURE:
    # A measure run's page carries `width:max-content`. Writing that into the header would ship
    # a page that lays itself out to its own intrinsic width inside a full-width host - a
    # right-hugging card with a band of transparent filler beside it - and nothing downstream
    # would notice, because the height and the floor would both still be right.
    print("MEASURE MODE - src\\sponsor_html.h was NOT written and is unchanged.")
    print("  panel-app.html is the max-content page; measure it, write SHELL_MIN_W / SHELL_H,")
    print("  then re-run WITHOUT --measure to regenerate the header.")
elif SHELL_MIN_W > SHELL_W_CEILING:
    # Reached only via GEN_SPONSOR_ALLOW_WIDE=1, whose whole purpose is to produce a PAGE that
    # can be measured at a floor that does not fit - so the sweep can say by how much. It is
    # deliberately NOT a way to ship an over-budget panel: the header is refused here as well,
    # so the only route to shipping one is to change MIN_CLIENT_CSS_W (and settings.cpp's
    # WM_GETMINMAXINFO with it) on purpose, which is a decision with a name on it rather than an
    # environment variable somebody exported once.
    print("OVER THE %dpx BUDGET (SHELL_MIN_W = %d) - src\\sponsor_html.h was NOT written and is"
          % (SHELL_W_CEILING, SHELL_MIN_W))
    print("  unchanged; it still carries the previous layout, which is the safe state.")
    print("  panel-app.html IS the shipping page, so tools\\measure-panel.py can sweep it - that")
    print("  is the only thing GEN_SPONSOR_ALLOW_WIDE is for.")
else:
    io.open(out_path, "w", encoding="utf-8", newline="\r\n").write(hdr_text)
    print("wrote src\\sponsor_html.h (%d bytes, %d literals, floor %d x height %d css px)"
          % (len(hdr_text), len(chunks(app_html, CHUNK)), SHELL_MIN_W, SHELL_H))
