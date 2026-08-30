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
import io, re, os, sys, datetime

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
URL_GOAT   = "https://dagoat.io"

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
# THIS IS NOT THE PLUGIN'S PANEL ANY MORE. The plugin stacks Ko-fi, GitHub and the GOATPROJECT
# block in a single column, and its `.shell.is-open { width: 272px }` describes THAT shape.
# Game Optimizer lays the same four elements out as ONE SINGLE ROW - Ko-fi, Star-on-Github, the
# statement, then the GOATPROJECT lockup, left to right, all the same height (see
# GO_LAYOUT_CSS). That is much wider and much shorter than their column, so 272 is simply the
# wrong number for this page, and reading it out of their stylesheet - which is what this script
# used to do - would be a confident way to be wrong.
#
# The plugin's value is still read, as PLUGIN_CSS_W, and PRINTED FOR PROVENANCE ONLY. It is
# used for nothing. Keeping the read means a change on their side still shows up in this
# script's output instead of passing unnoticed.
#
# Both numbers below are MEASURED against this exact page by tools\measure-panel.py, never
# estimated: both are sums of intrinsic sizes that only a layout engine can add up (where two
# lines of copy wrap, how wide "Star Project on Github" sets at 12px/600, how tall the lockup's
# flex row comes out).
#
# THE PANEL NO LONGER HAS A SHIPPED WIDTH. It FILLS the Settings window's content row - see
# GO_LAYOUT_CSS's `.shell.is-open{width:100%}` and settings.cpp's `spW = cw - 2 * GAP` - so
# there is no fixed number to write down any more, only a FLOOR:
#
#   SHELL_MIN_W  the narrowest host width at which the three groups still fit. Below it,
#                `.shell { overflow: hidden }` starts cutting the right-hand group off in
#                silence. This is what WM_GETMINMAXINFO's window minimum has to cover, and it
#                is emitted as kSponsorCssMinWidth. It is NOT the width the panel is drawn at.
#   SHELL_H      the panel's height, and it does NOT vary with the width: every item in the row
#                is the Ko-fi button's height and the row never wraps, so one number is right at
#                every window size. Emitted as kSponsorCssHeight.
#
# [M] MEASURED 2026-08-29 by tools\measure-panel.py, chrome-headless-shell 1234, BOTH passes:
#
#       pass 1  --measure, `.shell.is-open{width:max-content}`, host 1800px
#               .shell 848.83 x 75.375   -> ceil = 849 x 76
#               gaps as rendered: inter-group 20.00 and 20.00, statement<->lockup 8.00,
#               outer edges 0.00 / 0.00, top 13.00, bottom 13.00, height spread 0.000
#
#       pass 2  the shipping page, `width:100%`, swept 1000px down to 640px in 1px steps
#               first break at 848px (row-overflow) -> the floor is 849, identical to pass 1
#               every width below 848 also breaks, so the boundary is a boundary and not a dip
#
#     The arithmetic behind 848.83, so the number can be re-derived rather than re-measured:
#       .kofi 171.47 + .gh 175.36 + .go-item--copy 236 + GROUP_GAP 8 + .go-item--lock 190
#       = 780.83 of groups, + 2 * ROW_GAP 20 = 820.83 for the row,
#       + 26 (.body__pad's 13px a side) + 2 (the shell's 0.5px border, snapped to 1px a side)
#       = 848.83.
#     Height: the row is the Ko-fi button's 47.375, + 13 top (PAD_TOP - see request 4; it was
#       2 and the panel was 65 tall) + 13 bottom + 2 border = 75.375.
SHELL_MIN_W = 849
SHELL_H = 76

if SHELL_MIN_W < 200 or SHELL_H < 40:
    raise SystemExit("*** SHELL_MIN_W x SHELL_H = %d x %d is implausibly small - it looks like a "
                     "measurement that failed rather than a panel." % (SHELL_MIN_W, SHELL_H))

# ---------------------------------------------------------------------------------------------
# THE HEIGHT THE WHOLE ROW IS BUILT AROUND
# ---------------------------------------------------------------------------------------------
# The operator's instruction for this layout is "align all height same as Ko-fi button", so the
# Ko-fi button is the ruler and every other item is made to match it. `align-items: stretch` on
# .go-row does the matching - but stretch only ever makes the short items EQUAL to the TALLEST
# one, so it gives the wrong answer unless Ko-fi is genuinely the tallest. Two of the four are
# naturally taller than it and are brought down here; the third is brought down by a 1px change
# to a gap. tools\measure-panel.py checks all of that rather than trusting it.
#
# [M] MEASURED 2026-08-29, chrome-headless-shell 1234, on the pre-change page:
#       .kofi        175.36 x 47.375   <- the ruler. Its height is padding (9+9) plus the
#                                         two-line .kofi__label (12.5px/1.25 + 11px/1.25),
#                                         and does NOT depend on the button's width.
#       .gh          175.36 x 36.00    <- SHORTER; stretch raises it, nothing to do.
#       .goat__copy  244.00 x 48.06    <- TALLER by 0.69px. Three rendered lines
#                                         (14.69 x 3 = 44.07) plus .goat__copy's own 4px gap.
#       .goat-lockup 244.00 x 74.00    <- TALLER by 26.6px. 5.4em mark at font-size 10px = 54,
#                                         plus 10px padding top and bottom.
KOFI_H = 47.375

# The lockup is the one item that cannot simply be told to be shorter: its height is its 5.4em
# mark plus 10px of padding a side, and forcing the box down without shrinking the mark would
# push the mark under `overflow: clip` and cut the seal in half IN SILENCE - clip does not
# report scrollHeight, so no overflow check could ever see it.
#
# So it is scaled by the knob the plugin itself documents as the scale knob - "everything
# derives from font-size" - and nothing else about it is touched. Solving
# 5.4 * font-size + 10 + 10 = KOFI_H gives:
LOCK_FS = round((KOFI_H - 20.0) / 5.4, 3)      # 5.069px
if not 3.0 < LOCK_FS < 10.0:
    raise SystemExit("*** LOCK_FS came out %.3fpx, which is not a plausible lockup scale - the "
                     "KOFI_H measurement or the plugin's mark height has changed." % LOCK_FS)

# .goat__copy's own `gap: 4px` between its two lines puts the statement 0.69px OVER the Ko-fi
# button, which would make the copy the tallest item and quietly promote it to being the ruler.
# 3px puts it 0.31px under, so Ko-fi stays the tallest and the row height really is the Ko-fi
# height. This is set on OUR wrapper's descendant, not on their rule.
COPY_GAP = 3

# The statement's width, and it is NOT free. The copy fits the Ko-fi height only while it renders
# in THREE lines, and how many lines it renders in is a step function of this number.
#
# [M] MEASURED 2026-08-29 by sweeping .go-item--copy from 150px to 260px and reading
#     .goat__copy's height at every px (the two lines are 441.28px and 204.19px unwrapped):
#         150..154 -> 91.13px   line one in four lines, line two in two
#         155..204 -> 76.44px   line one in three,      line two in two
#         205..224 -> 61.75px   line one in three,      line two in one
#         225..260 -> 47.06px   line one in TWO,        line two in one   <- the only one that
#                                                                            fits KOFI_H
#     So 225 is the floor, exactly, and there is no width below it that works at this font size.
#     236 is that floor plus 11px, about two characters at 10.5px, so a small font-metric change
#     cannot silently push the copy to 61.75px and quietly make it the tallest item in the row.
COPY_W = 236

# The lockup's width, at LOCK_FS. Measured, not derived: it is the sum of the mark's box
# (0.88 x 5.4em, the 220/250 viewBox aspect), the 0.65em gap, and the wordmark - and the
# wordmark does NOT scale with font-size, because .goat .goat-lockup pins --gl-name-min: 13px
# and --gl-tag-min: 7px and both sit on their floor here. The tag, "The People's Compute
# Commons" at 7px/0.28em tracking and `white-space: nowrap`, is what actually sets this number.
# TO RE-DERIVE: put `width:max-content` in the .go-item--lock rule below, run measure-panel.py,
# read .go-item--lock, ceil it, put the number back.
LOCK_W = 190

# ---------------------------------------------------------------------------------------------
# THE TWO GAPS, AND WHY THERE HAVE TO BE TWO OF THEM
# ---------------------------------------------------------------------------------------------
# The row is no longer four equal items with one gutter repeated three times. The operator's
# grouping is THREE groups, and the third one has two things in it:
#
#     [ Ko-fi ]        [ Star Project on Github ]        [ statement   GOATPROJECT ]
#     \_ group 1 _/    \________ group 2 ______/         \_________ group 3 _______/
#
# "Statement and GoatProject as 1 group" is a claim the LAYOUT has to make, because nothing else
# in the page says it. The only thing that groups two items visually is that the gap inside the
# pair is smaller than the gaps around it - so these two numbers are not interchangeable, and
# GROUP_GAP < ROW_GAP is a hard requirement. It is asserted here, and re-asserted against the
# RENDERED boxes at every swept width by tools\measure-panel.py, because a CSS declaration is
# not evidence that the rendered gap came out that way.
#
# ROW_GAP IS A FLOOR, NOT THE GAP YOU SEE. `.go-row` is `justify-content: space-between`, so
# flexbox reserves ROW_GAP between the groups and then hands ALL the remaining width out equally
# between those same two gaps. At the narrowest allowed window the inter-group gap IS ROW_GAP;
# at every wider one it is larger, and it grows with the window - which is what "relative
# distance" asks for. GROUP_GAP never changes, so the wider the window the more plainly the pair
# reads as one group.
GROUP_GAP = 8      # statement <-> GOATPROJECT lockup. Tight on purpose - it IS the grouping.
ROW_GAP = 20       # the FLOOR for each of the two gaps between the three groups.

if not GROUP_GAP < ROW_GAP:
    raise SystemExit(
        "*** GROUP_GAP = %d is not smaller than ROW_GAP = %d, so the statement and the lockup "
        "would not read as one group - which is the whole of the operator's second request. "
        "tools\\measure-panel.py asserts the rendered version of this at every width and would "
        "fail too; refusing here is only the earlier of the two." % (GROUP_GAP, ROW_GAP))

# ---------------------------------------------------------------------------------------------
# THE GAP ABOVE THE ROW - REQUEST 4, AND ITS CAUSE
# ---------------------------------------------------------------------------------------------
# [M] The plugin's own rule, widget.js:190:   .body__pad { padding: 2px 13px 13px; }
#     TWO px at the top against THIRTEEN at the bottom. In their vertical stack that is right -
#     a `.rule` hairline sits immediately above it and the 2px is the clearance under that rule
#     - but this page has no `.rule`, so the row sits 2px below the shell's border while
#     carrying 13px beneath it, and the buttons read as jammed against the panel's top edge.
#
# The fix is ONE LONGHAND in our own block, appended after their rules: `padding-top` only, so
# their sides and their bottom are untouched and not one byte of theirs is edited. The panel
# grows by PAD_TOP - 2 px, and that is the whole of SHELL_H's growth this round.
#
# The number is checked against THEIRS at generation time - see the `.body__pad` parse further
# down - rather than typed here and left to drift.
PAD_TOP = 13

# [M] The two buttons' intrinsic widths, measured by tools\measure-panel.py. Used for nothing
# but the floor-budget diagnostic below - the layout reads them off the font, never from here -
# so a stale value costs a slightly wrong error message and nothing else.
KOFI_W_MEASURED = 171.47
GH_W_MEASURED = 175.36

# ---------------------------------------------------------------------------------------------
# THE BUDGET IS DERIVED FROM THE C++, NOT GUESSED - AND IT IS NOW A FLOOR, NOT A CEILING
# ---------------------------------------------------------------------------------------------
# While the panel was a fixed 831px the question was "is it too WIDE for the smallest window?".
# It fills the width now, so that question cannot be asked at all: whatever the window is, the
# panel is the content row. The question that replaces it is the mirror image - IS THE SMALLEST
# WINDOW WIDE ENOUGH FOR THE THREE GROUPS? - and the number that answers it is SHELL_MIN_W.
#
# The chain is readable straight out of three files, and the point of writing it down here is
# that the next reader can CHECK it instead of trusting this comment:
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
#           RECT need = { 0, 0, theme::Dp(880, dpi), needH };
#       The minimum CLIENT width is Dp(880). That is the worst case: at any larger window the
#       panel has strictly more room, and every extra pixel goes into the two inter-group gaps.
#
#   [M] src\webview_host.cpp  WebSponsorMinSize:
#           s.cx = ::MulDiv(kSponsorCssMinWidth, dpi, 96);
#       The floor's device width is kSponsorCssMinWidth - this file's SHELL_MIN_W - scaled by the
#       SAME MulDiv the 880 and the 12 go through, which is why the budget can be stated in
#       css px at all.
#
# So, in css px:  880 - 2 * 12 = 856 available at the smallest allowed window.
#
# 856 IS THE RIGHT ANSWER IN CSS PX AND THE WRONG NUMBER TO WRITE IN A GUARD, because the three
# MulDiv calls round INDEPENDENTLY and the css-px arithmetic does not survive that at every DPI.
# Windows custom scaling runs 100%..500%, so dpi is not restricted to the 96/120/144/168 set: a
# width that is correct in css px can overrun by a pixel at dpi 100 or 108 and look perfect on
# every machine anyone tested on.
#
# So the ceiling is COMPUTED here rather than asserted, over every integer dpi in that range,
# with MulDiv modelled exactly as Win32 does it - round to nearest, ties away from zero, i.e.
# (a*b + c//2)//c. Computing it beats writing the answer down: when kGap or the 880 changes,
# this number follows instead of going quietly stale.
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

# THE GUARD STAYS, POINTING THE OTHER WAY. A floor past the budget would genuinely clip -
# silently, at the panel's own RIGHT edge, on the machines least likely to be tested - so it must
# still refuse, and refusing by writing no header is the safe failure.
if SHELL_MIN_W > SHELL_W_CEILING and not MEASURE and os.environ.get("GEN_SPONSOR_ALLOW_WIDE") != "1":
    raise SystemExit(
        "*** SHELL_MIN_W = %d is past the %dpx budget, so at the SMALLEST allowed window the\n"
        "    three groups DO NOT FIT and `.shell { overflow: hidden }` would cut the right-hand\n"
        "    group - the GOATPROJECT lockup - off, silently, with no error anywhere.\n"
        "    DO NOT SHIP THIS - src%ssponsor_html.h has NOT been rewritten and still carries the\n"
        "    previous layout, which is the safe state.\n"
        "\n"
        "    Where the budget comes from: the minimum CLIENT width is theme::Dp(%d) and the panel\n"
        "    is that less one theme::metric::kGap = %d a side, so %d css px are available - but\n"
        "    the three MulDiv calls round independently, so the largest width that is safe at\n"
        "    EVERY integer dpi from %d to %d is %d. That number is COMPUTED above, not typed in.\n"
        "\n"
        "    What this page's floor is made of [M], measured, not estimated:\n"
        "      .kofi           %6.2f  intrinsic - 'Support me on Ko-fi' 12.5px/600\n"
        "      .gh             %6.2f  intrinsic - 'Star Project on Github' 12px/600, nowrap\n"
        "      .go-item--copy  %6d  pinned; 225 is its measured minimum (see COPY_W)\n"
        "      .go-item--lock  %6d  pinned; set by the nowrap tagline at 7px / 0.28em\n"
        "      group gap       %6d  statement <-> lockup, once - GROUP_GAP\n"
        "      row gaps        %6d  the TWO inter-group gaps at their floor, %dpx each\n"
        "      shell chrome     28.00  .body__pad %dpx a side + the 0.5px border, snapped\n"
        "\n"
        "    THE WAYS OUT, and every one of them is a design decision rather than a rounding one:\n"
        "      * raise the window minimum - settings.cpp's WM_GETMINMAXINFO and MIN_CLIENT_CSS_W\n"
        "        here must move TOGETHER, or this guard is measuring a window that does not\n"
        "        exist;\n"
        "      * cut ROW_GAP. It is only the FLOOR for the inter-group gaps, so a smaller value\n"
        "        costs nothing at any window wider than the minimum - but it must stay clear of\n"
        "        GROUP_GAP or the grouping stops reading;\n"
        "      * make an item narrower. COPY_W's own floor is 225, and the lockup's tagline\n"
        "        --gl-tag-min is the only other compressible number.\n"
        "\n"
        "    Set GEN_SPONSOR_ALLOW_WIDE=1 only to generate a page for measuring; it still\n"
        "    refuses to write the header."
        % (SHELL_MIN_W, SHELL_W_CEILING, chr(92), MIN_CLIENT_CSS_W, GAP_CSS,
           MIN_CLIENT_CSS_W - 2 * GAP_CSS, DPI_LO, DPI_HI, SHELL_W_CEILING,
           KOFI_W_MEASURED, GH_W_MEASURED, COPY_W, LOCK_W, GROUP_GAP,
           2 * ROW_GAP, ROW_GAP, PAD_TOP))

# ---------------------------------------------------------------------------------------------
# THE ONLY CSS IN THIS PAGE THAT IS NOT THE PLUGIN'S
# ---------------------------------------------------------------------------------------------
# It is appended AFTER the plugin's rules inside the same <style>, so where a declaration
# collides the later one wins on cascade order alone - no !important, no rewritten selector,
# not one byte of theirs edited. Delete this block and the page falls straight back to their
# vertical stack.
#
#   .go-row            the flex context. ONE ROW, THREE GROUPS, left to right, and
#                      align-items:stretch so they all come out the same height - which is the
#                      operator's actual requirement for this layout. stretch equalises to the
#                      TALLEST item, so everything else in this block exists to make sure the
#                      tallest item is the Ko-fi button and not one of the other three.
#                      justify-content:space-between is what spreads the three groups across
#                      the panel: the outer edges sit on .body__pad's padding and ALL the spare
#                      width is handed to the two gaps between the groups, equally. The `gap`
#                      declaration is therefore a FLOOR, not the gap you see - see ROW_GAP.
#   .go-row>*          flex:0 0 auto - nothing grows, nothing shrinks. A shrinking item would
#                      rewrap the statement or clip the lockup's nowrap tagline, and the lockup
#                      is `overflow: clip`, which reports no scrollWidth and so cannot be
#                      caught. Growing is space-between's job here, not the items'.
#   .go-group          the third group's own flex row: the statement and the lockup, with a
#                      GROUP_GAP between them instead of a ROW_GAP. This wrapper is the whole of
#                      "Statement and GoatProject as 1 group" - without it there are four items
#                      and space-between spreads all four evenly, which says the opposite.
#   .go-row>.kofi      width:max-content. BOTH buttons are `width: 100%` in the plugin, which was
#   .go-row>.gh        right inside their column and is wrong as a direct child of the row: 100%
#                      of the row is the whole panel, so each button would try to be the full
#                      width. max-content gives each its own intrinsic width instead.
#                      margin-top:0 on both. The plugin's 4px and 8px were the gaps ABOVE each
#                      button in their vertical stack; in a row they are top-margins against
#                      nothing, and a top margin on a stretched flex item eats into its height.
#   .go-row>.gh        display:flex. .gh is `display:block` with a `padding:1px` frame and one
#                      in-flow child, .gh__inner, which paints the dark pill. Stretched to the
#                      Ko-fi height as a block, the frame grows and the pill does not - an 11px
#                      band of bare #262626 opens under the label. As a flex container the inner
#                      pill stretches with it and .gh__inner's own align-items:center re-centres
#                      the star and the label. (.gh__corner and .gh__sweep are position:absolute
#                      and are out of flow either way.)
#   .go-item           the two wrappers this script adds, so the statement and the lockup can be
#                      given a width and a height without one byte of the plugin's rules being
#                      touched. align-items:center so short content sits in the middle of the
#                      Ko-fi height rather than at the top.
#   .go-item--lock     ALSO CARRIES THE PLUGIN'S OWN `goat` CLASS, and that is load-bearing:
#                      SEVEN of their rules are written as `.goat .goat-lockup...`, including
#                      the one that sets font-size:10px, --gl-seal-scale, --gl-name-min, the
#                      background, the padding and the radius. Drop the .goat ancestor and the
#                      lockup silently falls back to `.goat-lockup { font-size: 20px }` with no
#                      background at all. The class is kept on OUR wrapper so their cascade still
#                      matches; every property of theirs it brings with it (display:block,
#                      width:100%, margin-top:16px) is overridden by the .go-item rules below,
#                      which are later in the same stylesheet.
#                      align-items:stretch, so the <a> fills the wrapper instead of leaving the
#                      wrapper the right height and the visible card 3px short.
#   .goat-lockup       font-size:LOCK_FS. See LOCK_FS - this is the plugin's own documented
#                      scale knob and it is the only way to bring a 74px lockup to the Ko-fi
#                      height without cutting the mark off under `overflow: clip`.
#   .goat__copy        margin:0 (their 0 0 8px was clearance under the statement in the column)
#                      and gap:COPY_GAP - see COPY_GAP.
#   .body__pad         padding-top ONLY. Their shorthand is `padding: 2px 13px 13px` - see
#                      PAD_TOP - and a longhand later in the same stylesheet replaces exactly
#                      that one component of it. Their sides and their bottom are untouched.
#   .shell.is-open     width:100% - the panel FILLS whatever host it is given, which is the
#                      whole of the operator's first request. Under --measure it is
#                      `max-content` instead, so the page can report its own intrinsic minimum.
#                      This is also the declaration that overrides the plugin's 272px.
GO_LAYOUT_CSS = """
.go-row{display:flex;flex-direction:row;align-items:stretch;justify-content:space-between;gap:%(rowgap)dpx}
.go-row>*{flex:0 0 auto}
.go-group{display:flex;flex-direction:row;align-items:stretch;gap:%(groupgap)dpx;min-width:0}
.go-group>*{flex:0 0 auto}
.go-row>.kofi{width:max-content;margin-top:0}
.go-row>.gh{width:max-content;margin-top:0;display:flex;align-items:stretch}
.go-row>.gh>.gh__inner{flex:1 1 auto}
.go-item{display:flex;align-items:center;box-sizing:border-box;min-width:0;margin:0}
.go-item--copy{width:%(copy)dpx}
.go-item--lock{width:%(lock)dpx;align-items:stretch}
.go-group>.go-item--lock .goat-lockup{font-size:%(lockfs)spx}
.go-group>.go-item .goat__copy{margin:0;gap:%(copygap)dpx;flex:1 1 auto;min-width:0}
.body__pad{padding-top:%(padtop)dpx}
.shell.is-open{width:%(shell)s}
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
cup = const_string("SVG_CUP")
star = const_string("SVG_STAR")
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

# ---- REQUEST 4'S CAUSE, READ OUT OF THEIR STYLESHEET RATHER THAN TYPED IN --------------------
# `.body__pad { padding: 2px 13px 13px }` - 2px at the top against 13px at the bottom - is why
# the row looked jammed against the panel's top edge. Our block sets `padding-top: PAD_TOP`; the
# check below says the number PAD_TOP has to match is still THEIRS, so a change on their side
# fails the generation instead of quietly un-centring the row.
_bp = re.search(r"\.body__pad\s*\{[^}]*?\bpadding:\s*([0-9.]+)px\s+([0-9.]+)px\s+([0-9.]+)px",
                css, re.S)
if not _bp:
    raise SystemExit("*** could not find the plugin's `.body__pad { padding: T S B }` rule, so "
                     "the top gap this page overrides cannot be checked against their bottom "
                     "one. Request 4 is a SYMMETRY requirement and it has just become "
                     "unverifiable - fix the parse rather than shipping it unchecked.")
PLUGIN_PAD_TOP = float(_bp.group(1))
PLUGIN_PAD_SIDE = float(_bp.group(2))
PLUGIN_PAD_BOTTOM = float(_bp.group(3))
print("plugin .body__pad padding = %g %g %g px  ->  our override sets padding-top to %dpx; "
      "their sides and bottom are untouched"
      % (PLUGIN_PAD_TOP, PLUGIN_PAD_SIDE, PLUGIN_PAD_BOTTOM, PAD_TOP))
if abs(PAD_TOP - PLUGIN_PAD_BOTTOM) > 0.001:
    raise SystemExit(
        "*** PAD_TOP = %d but the plugin's `.body__pad` BOTTOM padding is %gpx. The operator's\n"
        "    fourth request is that the gap ABOVE the row equals the gap BELOW it, so these two\n"
        "    have to be the same number. Set PAD_TOP = %g." % (PAD_TOP, PLUGIN_PAD_BOTTOM,
                                                              PLUGIN_PAD_BOTTOM))
SHELL_CHROME_W = 2.0 * PLUGIN_PAD_SIDE + 2.0     # their side padding + the 0.5px border, snapped


# The sponsor markup: from the Ko-fi button through the end of the .goat block.
m0 = src.index("'<button type=" + '"' + "button" + '"' + ' class="kofi">' + "'")
m1 = src.index("'</div>' +", src.index("data-goat-mark"))
frag = src[m0:m1 + len("'</div>' +")]

# Rebuild the concatenation: quoted literals in order, with the two SVG consts spliced back in
# at the exact points where the source concatenated them.
# Hand-scanned rather than regexed: a character class containing a backslash is exactly the
# kind of escaping fight that produces a wrong answer quietly.
SUBST = {"SVG_CUP": cup, "SVG_STAR": star,
         "KOFI_HANDLE": "@IRP_HongKong", "GOAT_URL": URL_GOAT}
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
        if frag.startswith(name, i):
            hit = name
            break
    if hit:
        pieces.append(SUBST[hit])
        i += len(hit)
        continue
    i += 1
markup = "".join(pieces)

print("css=%d lockup=%d cup=%d star=%d markup=%d" % (len(css), len(lock), len(cup), len(star), len(markup)))
for probe in ("class=\"kofi\"", "class=\"gh\"", "gh__sweep", "goat__copy", "goat-lockup",
              "Star Project on Github", "Help fight Cancer", "Earn GOAT"):
    print("  %-28s %s" % (probe, "FOUND" if probe in markup else "*** MISSING ***"))

# ---- REGROUP: one column -> ONE ROW OF THREE GROUPS ------------------------------------------
# The plugin emits these three siblings, in this order, all in one column:
#     <button class="kofi">   Ko-fi
#     <button class="gh">     Star Project on Github
#     <div class="goat">      two statement lines, then the GOATPROJECT lockup
#
# Game Optimizer wants the same four things in ONE ROW, spread across the panel as THREE GROUPS:
#
#     [ Ko-fi ]        [ Star Project on Github ]        [ statement   GOATPROJECT ]
#
# The first two are already siblings and cross over untouched. The other two are NOT siblings -
# they are both inside `<div class="goat">` - so that block is split one level further and its
# two children become items in their own right. They are then put back TOGETHER inside a new
# `.go-group` wrapper, which is what makes them the third group rather than the third and fourth
# items: `justify-content: space-between` spreads its own children evenly, so four children
# would space all four equally and say the opposite of what was asked.
#
# `.goat` therefore stops being a wrapper around a group... except as a CLASS, which is kept on
# the lockup's new wrapper because seven of the plugin's rules are written `.goat .goat-lockup...`
# and the lockup loses its font-size, its background, its padding and its scale variables without
# that ancestor. See GO_LAYOUT_CSS.
#
# THE SPLIT IS BY INDEX AND EVERY PIECE IS RE-EMITTED BYTE FOR BYTE. Nothing of the plugin's is
# reindented, re-quoted or rewritten: the wrappers go AROUND their elements exactly as
# GO_LAYOUT_CSS goes AFTER their rules. The guards below stop the generator dead if widget.js
# ever changes shape, rather than emitting a silently mis-sliced panel - a regroup that half
# worked would present as a CSS problem and cost a day to trace back to here.
A_GH = '<button type="button" class="gh">'
A_GOAT = '<div class="goat">'
if markup.count(A_GH) != 1:
    raise SystemExit("*** expected exactly ONE %s in the extracted markup, found %d. widget.js "
                     "has changed shape and this split would be wrong."
                     % (A_GH, markup.count(A_GH)))
if markup.count(A_GOAT) != 1:
    raise SystemExit("*** expected exactly ONE %s in the extracted markup, found %d. widget.js "
                     "has changed shape and this split would be wrong."
                     % (A_GOAT, markup.count(A_GOAT)))
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
if part_kofi + part_gh + part_goat != markup:
    raise SystemExit("*** the three slices do not reassemble into the original markup - bytes "
                     "were lost or duplicated in the split")

# ---- SECOND SPLIT: .goat -> statement + lockup, as two independent row items ------------------
# part_goat is exactly:  <div class="goat"> <div class="goat__copy">...</div> <a class="goat-
# lockup" ...>...</a> </div>. The lockup is ONE enormous <a> with the whole seal SVG nested
# inside it, so the only safe cut is at its opening tag and at part_goat's own final </div> -
# anything that tries to find the matching close by scanning is looking at hundreds of nested
# tags and will get it wrong.
A_COPY = '<div class="goat__copy">'
A_LOCK = '<a class="goat-lockup"'
for _anchor in (A_COPY, A_LOCK):
    if part_goat.count(_anchor) != 1:
        raise SystemExit("*** expected exactly ONE %s inside the .goat block, found %d. "
                         "widget.js has changed shape and this split would be wrong."
                         % (_anchor, part_goat.count(_anchor)))
_i_copy = part_goat.index(A_COPY)
_i_lock = part_goat.index(A_LOCK)
if not _i_copy < _i_lock:
    raise SystemExit("*** the .goat__copy block no longer precedes the .goat-lockup (index %d vs "
                     "%d). The plugin's order changed; this split is no longer valid."
                     % (_i_copy, _i_lock))
if part_goat[:_i_copy] != A_GOAT:
    raise SystemExit("*** the .goat block does not open straight onto %s - there is something "
                     "else in between: %r. This split would drop it."
                     % (A_COPY, part_goat[:_i_copy]))
# Trailing whitespace after the closing </div>, if the plugin ever grows any, is carried through
# rather than silently dropped.
_goat_body = part_goat.rstrip()
_goat_ws = part_goat[len(_goat_body):]
part_copy = _goat_body[_i_copy:_i_lock]
_lock_and_close = _goat_body[_i_lock:]
if not _lock_and_close.endswith("</div>"):
    raise SystemExit("*** the .goat block does not end with the </div> that closes it. It ends: "
                     "...%r - the split would leave the page malformed." % _lock_and_close[-60:])
part_lock = _lock_and_close[:-len("</div>")]
if not part_copy.endswith("</div>"):
    raise SystemExit("*** part_copy does not end with </div> - the cut landed inside the "
                     ".goat__copy block. It ends: ...%r" % part_copy[-60:])
if not part_lock.endswith("</a>"):
    raise SystemExit("*** part_lock does not end with </a> - the cut landed INSIDE the lockup's "
                     "<a> element, which carries the whole seal SVG. It ends: ...%r"
                     % part_lock[-60:])
if A_GOAT + part_copy + part_lock + "</div>" + _goat_ws != part_goat:
    raise SystemExit("*** the .goat slices do not reassemble into part_goat - bytes were lost or "
                     "duplicated in the second split")

# `goat` on the lockup's wrapper is NOT decoration - see the GO_LAYOUT_CSS note. Their seven
# `.goat .goat-lockup...` rules need that ancestor and the class is the whole of what they match.
W_COPY_OPEN = '<div class="go-item go-item--copy">'
W_LOCK_OPEN = '<div class="goat go-item go-item--lock">'
# THE GROUP WRAPPER IS NOT COSMETIC. It is the entire mechanism by which "Statement and
# GoatProject as 1 group" is expressed: it makes them ONE flex item of .go-row, so
# space-between has three things to spread instead of four, and it gives them their own
# GROUP_GAP instead of the ROW_GAP everything else gets.
W_GROUP_OPEN = '<div class="go-group go-group--goat">'
regrouped = ('<div class="go-row">'
             + part_kofi
             + part_gh
             + W_GROUP_OPEN
             + W_COPY_OPEN + part_copy + '</div>'
             + W_LOCK_OPEN + part_lock + '</div>'
             + '</div>'
             + _goat_ws +
             '</div>')
print("regrouped: kofi=%d gh=%d copy=%d lock=%d bytes into three groups -> %d bytes"
      % (len(part_kofi), len(part_gh), len(part_copy), len(part_lock), len(regrouped)))
for _probe in ('<div class="go-row">', W_GROUP_OPEN, W_COPY_OPEN, W_LOCK_OPEN):
    if regrouped.count(_probe) != 1:
        raise SystemExit("*** %s appears %d times in the regrouped markup, expected 1"
                         % (_probe, regrouped.count(_probe)))
if '<div class="go-col' in regrouped:
    raise SystemExit("*** the old two-group .go-col wrapper is still being emitted - this is a "
                     "half-applied single-row layout")
# THE GROUP MUST CONTAIN EXACTLY THOSE TWO, IN THAT ORDER, AND NOTHING ELSE. Counting the
# wrapper once (above) does not say what is inside it, and a group that opened before .gh - or
# closed before the lockup - would still count 1. This is the operator's second request stated
# as a byte comparison.
_want_group = (W_COPY_OPEN + part_copy + '</div>' + W_LOCK_OPEN + part_lock + '</div>' + '</div>')
_g0 = regrouped.index(W_GROUP_OPEN) + len(W_GROUP_OPEN)
if regrouped[_g0:_g0 + len(_want_group)] != _want_group:
    raise SystemExit("*** the statement and the lockup are not the two and only children of %s, "
                     "so they will not read as one group. What follows the wrapper is: ...%r"
                     % (W_GROUP_OPEN, regrouped[_g0:_g0 + 80]))
# Every byte of the plugin's own markup still has to be present, unmodified and in the original
# order - the wrappers may only be ADDED around it. Two checks, because either one alone can be
# satisfied by a wrong page: the first says nothing of theirs was rewritten, the second says
# nothing extra was slipped in between.
_pos = 0
for _name, _piece in (("part_kofi", part_kofi), ("part_gh", part_gh),
                      ("part_copy", part_copy), ("part_lock", part_lock)):
    _at = regrouped.find(_piece, _pos)
    if _at < 0:
        raise SystemExit("*** %s is not present in the regrouped markup byte for byte, or is out "
                         "of order - the plugin's own element was rewritten, not wrapped" % _name)
    _pos = _at + len(_piece)
_added = (len('<div class="go-row">') + len(W_GROUP_OPEN) + len(W_COPY_OPEN) + len(W_LOCK_OPEN)
          + 4 * len("</div>"))
_want = (len(part_kofi) + len(part_gh) + len(part_copy) + len(part_lock)
         + len(_goat_ws) + _added)
if len(regrouped) != _want:
    raise SystemExit("*** the regrouped markup is %d bytes but the four plugin pieces plus this "
                     "script's %d bytes of wrapper come to %d - something was inserted or lost"
                     % (len(regrouped), _added, _want))


# THE PANEL CSS IS WRITTEN FOR A SHADOW DOM. `:host` appears twice near the top of the CSS
# template and carries the font stack AND every custom property, including --kofi. In a flat
# document `:host` matches nothing, so every var() resolves to nothing and you get an unfilled
# Ko-fi button, no star colour and a serif fallback - which is exactly what the first render
# produced. So attach a real shadow root, the way the plugin itself does, instead of rewriting
# their stylesheet. Nothing of theirs is modified.
# Our own rules go LAST inside the same <style>, after the plugin's CSS and after the lockup's,
# so cascade order alone decides the collisions. Under --measure the shell is width:max-content
# so the page can report its own intrinsic minimum; otherwise it is `width:100%` and takes
# whatever the host gives it.
go_css = GO_LAYOUT_CSS % {"rowgap": ROW_GAP, "groupgap": GROUP_GAP,
                          "copy": COPY_W, "lock": LOCK_W,
                          "lockfs": ("%.3f" % LOCK_FS).rstrip("0").rstrip("."),
                          "copygap": COPY_GAP, "padtop": PAD_TOP,
                          "shell": "max-content" if MEASURE else "100%"}
print("layout override: three groups, space-between; inter-group gap FLOOR %dpx, in-group gap "
      "%dpx; .go-item--copy %dpx (line gap %dpx), .go-item--lock %dpx (lockup font-size %spx, "
      "from KOFI_H %.3f); .body__pad padding-top -> %dpx; .shell.is-open width -> %s"
      % (ROW_GAP, GROUP_GAP, COPY_W, COPY_GAP, LOCK_W,
         ("%.3f" % LOCK_FS).rstrip("0").rstrip("."), KOFI_H, PAD_TOP,
         "max-content  [MEASURE MODE]" if MEASURE else "100%  [fills the host]"))

payload = "<style>" + css + chr(10) + lock + chr(10) + go_css + "</style>" + \
          "<div class='shell is-open'><div class='body__pad'>" + regrouped + "</div></div>"
js = ("var host=document.getElementById('h');"
      "var r=host.attachShadow({mode:'open'});"
      "r.innerHTML=document.getElementById('p').textContent;")
html = ("<!doctype html><meta charset='utf-8'>"
        "<style>html,body{margin:0;padding:14px;overflow:hidden;background:#0F1115}</style>"
        "<body><div id='h'></div>"
        "<script type='text/plain' id='p'>" + payload + "</script>"
        "<script>" + js + "</script>")
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
# that would disappear is the RIGHT-hand group - the GOATPROJECT lockup - behind
# `.shell { overflow: hidden }`. Nothing degrades gracefully any more; what prevents it is
# keeping the window above kSponsorCssMinWidth, which is exactly what WM_GETMINMAXINFO and this
# script's own floor guard are for. It is a guarantee now rather than a soft landing.
app_html = ("<!doctype html><meta charset='utf-8'>"
            "<style>html,body{margin:0;padding:0;overflow:hidden;background:transparent}"
            "#h{position:absolute;left:0;right:0;bottom:0}</style>"
            "<body><div id='h'></div>"
            "<script type='text/plain' id='p'>" + payload + "</script>"
            "<script>" + app_js + "</script>")

# Written out as a file as well, so the size measurement below (and any future one) runs
# against exactly the bytes the app compiles in, rather than a near-neighbour.
io.open(os.path.join(S, "panel-app.html"), "w", encoding="utf-8").write(app_html)
print("wrote panel-app.html (%d bytes)" % len(app_html))

for probe in (URL_KOFI, URL_GITHUB, URL_GOAT):
    if probe not in app_html:
        raise SystemExit("*** %s never reached the page - link substitution failed" % probe)
print("  all three operator URLs are in the page")


def VerifyUrlsAgainstSponsorH():
    """The page's destinations and webview_host.cpp's allow-list must be the same three.

    webview_host.cpp refuses to ShellExecute a target that is not one of sponsor.h's
    constants. If this script ever emitted a URL that header does not carry, the button would
    render, navigate, be cancelled - and then silently do nothing. That is a failure with no
    symptom, so it is caught here instead of in the field.
    """
    path = os.path.join(REPO, "src", "sponsor.h")
    text = io.open(path, encoding="utf-8").read()
    for name, url in (("kKofi", URL_KOFI), ("kGitHub", URL_GITHUB),
                      ("kGoatProject", URL_GOAT)):
        want = 'L"%s"' % url
        if want not in text:
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
A("// webview_host.cpp refuses to open a target that header does not list.")
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
A("// is `.shell.is-open { width: 100%% }` - so there is no shipped width to carry here. What")
A("// the C++ needs instead is the NARROWEST host the three groups still fit inside, because")
A("// that is the number WM_GETMINMAXINFO has to keep the window above. Below it the row")
A("// overflows its end edge and `.shell { overflow: hidden }` cuts the right-hand group - the")
A("// GOATPROJECT lockup - off in silence.")
A("//")
A("// kSponsorCssHeight is the panel's height, and it does NOT vary with the width: every item")
A("// in the row is the Ko-fi button's height and the row never wraps, so one number is right")
A("// at every window size.")
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
