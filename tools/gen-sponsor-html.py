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
#     (write the two numbers into SHELL_W / SHELL_H below)
#     python tools\gen-sponsor-html.py               emits the fixed px and writes the header
#     python tools\measure-panel.py                  must now report exactly SHELL_W x SHELL_H
#
# A measure run deliberately does NOT write src\sponsor_html.h. A header carrying a
# `width:max-content` page would build and run and size the host window off a value the C++
# constants no longer describe - a wrong panel with no error anywhere. Refusing to write it is
# cheaper than detecting it later.
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
# estimated: neither one is written down anywhere, because both are the sum of intrinsic sizes
# that only a layout engine can add up (where two lines of copy wrap, how wide "Star Project on
# Github" sets at 12px/600, how tall the lockup's flex row comes out).
#
# [M] MEASURED 2026-08-29 by tools\measure-panel.py, chrome-headless-shell 1234,
#     --window-size=1200,1200 (a viewport far larger than the panel, so nothing constrains it):
#
#       pass 1  --measure, `.shell.is-open{width:max-content}`
#               .shell 830.83 x 64.38   .body__pad 828.83 x 62.38   .go-row 802.83 x 47.38
#               .kofi 171.47 x 47.38   .gh 175.36 x 47.38
#               .go-item--copy 236.00 x 47.38   .go-item--lock 190.00 x 47.38
#               -> ceil = 831 x 65
#
#       pass 2  pinned to the two constants below
#               .shell 831.00 x 64.38   -> ceil = 831 x 65, identical
#
#     The arithmetic: 171.47 + 175.36 + 236 + 190 + 30 (three 10px gaps) = 802.83 for the row,
#     + 26 (.body__pad's 13px a side) + 2 (the shell's 0.5px border, snapped to 1px a side)
#     = 830.83. Height: the row is the Ko-fi button's 47.38, + 15 (.body__pad is
#     `padding: 2px 13px 13px`, so 2 top and 13 bottom) + 2 border = 64.38.
SHELL_W = 831
SHELL_H = 65

if SHELL_W < 200 or SHELL_H < 40:
    raise SystemExit("*** SHELL_W x SHELL_H = %d x %d is implausibly small - it looks like a "
                     "measurement that failed rather than a panel." % (SHELL_W, SHELL_H))

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

# The gutter between the four items. 10px rather than the old 14: the row now has three gutters
# instead of one, and every px of them is px the Settings window has to find.
ROW_GAP = 10

# ---------------------------------------------------------------------------------------------
# 831 FITS - AND THE BUDGET IS DERIVED FROM THE C++, NOT GUESSED
# ---------------------------------------------------------------------------------------------
# An earlier revision of this file carried a 720px ceiling. It was an unmeasured guess and it was
# WRONG - it refused a layout that fits. The real budget is readable straight out of three files,
# and the point of writing the chain down here is that the next reader can CHECK it instead of
# trusting this comment:
#
#   [M] src\settings.cpp  SettingsLayout, the sponsor panel block:
#           if (spW > cw - 2 * GAP) spW = cw - 2 * GAP;
#       The panel is capped at the client width less one gap a side, and it clips at its own
#       left edge rather than pushing the footer. That expression IS the budget.
#
#   [M] src\settings.cpp  SettingsLayout:   const int GAP = theme::Dp(theme::metric::kGap, dpi);
#   [M] src\theme.h:71                      constexpr int kGap = 12;
#   [M] src\theme.cpp:329                   int Dp(int logical, int dpi) { return MulDiv(logical, dpi, 96); }
#
#   [M] src\settings.cpp  WM_GETMINMAXINFO:
#           RECT need = { 0, 0, theme::Dp(860, dpi), needH };
#       The minimum CLIENT width is Dp(860). This is the worst case: at any larger window the
#       panel has strictly more room.
#
#   [M] src\webview_host.cpp:666  WebSponsorNaturalSize:
#           s.cx = ::MulDiv(kSponsorCssWidth, dpi, 96);
#       The panel's device width is kSponsorCssWidth - this file's SHELL_W - scaled by the SAME
#       MulDiv the 860 and the 12 go through, which is why the budget can be stated in css px.
#
# So, in css px:  860 - 2 * 12 = 836 available, and SHELL_W = 831 fits with 5 to spare.
#
# ---------------------------------------------------------------------------------------------
# THE CEILING IS 835, NOT 836, AND THE MISSING PIXEL IS A REAL ONE
# ---------------------------------------------------------------------------------------------
# 836 is the right answer in css px and the WRONG number to write in a guard, because the three
# MulDiv calls round independently and the css-px arithmetic does not survive that at every DPI.
#
# [M] COMPUTED 2026-08-29 over every integer dpi from 96 to 480 (Windows custom scaling runs
#     100%..500%, so dpi is not restricted to the 96/120/144/168 set), with MulDiv modelled
#     exactly as Win32 does it - round to nearest, ties away from zero, i.e. (a*b + c//2)//c:
#
#         margin(dpi) = MulDiv(860,dpi,96) - 2*MulDiv(12,dpi,96) - MulDiv(SHELL_W,dpi,96)
#
#       SHELL_W = 831   margin >= 0 at ALL 385 dpi values. Worst case 4px, at dpi 100.
#       SHELL_W = 835   margin >= 0 at ALL 385 dpi values. The largest width for which that
#                       is true.
#       SHELL_W = 836   margin = -1 at 144 of the 385, starting at dpi 100, 101, 108, 109,
#                       110, 111. Those are custom-scaling DPIs, not the common ones - which is
#                       exactly why a ceiling of 836 would look correct on every machine anyone
#                       tested on and clip one pixel on somebody's.
#
#     At the four common scales, SHELL_W = 831 leaves, in DEVICE px:
#         100% (dpi 96)   panel 831   gap 12   cw 860    margin  5
#         125% (dpi 120)  panel 1039  gap 15   cw 1075   margin  6
#         150% (dpi 144)  panel 1247  gap 18   cw 1290   margin  7
#         175% (dpi 168)  panel 1454  gap 21   cw 1505   margin  9
#     The margin GROWS with DPI, because 5 css px scale up while the rounding error stays at
#     most 1px. The thin case is 100%, and even there nothing rounds it away.
#
# THE GUARD STAYS. A future layout past 835 would genuinely clip - silently, at the panel's own
# left edge, on the machines least likely to be tested - so it must still refuse, and refusing
# by writing no header is the safe failure.
SHELL_W_CEILING = 835
if SHELL_W > SHELL_W_CEILING and not MEASURE and os.environ.get("GEN_SPONSOR_ALLOW_WIDE") != "1":
    raise SystemExit(
        "*** SHELL_W = %d is past the %dpx ceiling, so this panel WOULD BE CLIPPED at its own\n"
        "    left edge by settings.cpp's `if (spW > cw - 2 * GAP) spW = cw - 2 * GAP;` at the\n"
        "    minimum window size - silently, with no error anywhere.\n"
        "    DO NOT SHIP THIS - src%ssponsor_html.h has NOT been rewritten and still carries\n"
        "    the previous layout, which is the safe state.\n"
        "\n"
        "    Where the ceiling comes from: the minimum CLIENT width is theme::Dp(860) and the\n"
        "    panel is capped at that less one theme::metric::kGap = 12 a side, so 836 css px is\n"
        "    available - but 836 itself rounds to a 1px overrun at 144 of the 385 integer DPI\n"
        "    values between 96 and 480, so the largest always-safe width is %d. See the block\n"
        "    at SHELL_W_CEILING for the full derivation and the numbers.\n"
        "\n"
        "    What this page's width is made of [M], measured, not estimated:\n"
        "      .kofi           171.47  intrinsic - 'Support me on Ko-fi' 12.5px/600\n"
        "      .gh             175.36  intrinsic - 'Star Project on Github' 12px/600, nowrap\n"
        "      .go-item--copy  %6.2f  %s\n"
        "      .go-item--lock  %6.2f  pinned; 188.50 is its own max-content, set by the\n"
        "                              nowrap tagline 'The People's Compute Commons'\n"
        "                              at 7px with 0.28em tracking\n"
        "      gutters         %6.2f  %d gaps of %dpx\n"
        "      shell chrome      28.00  .body__pad 13px a side + the 0.5px border, snapped\n"
        "    [M] The floor for THIS set of four items, each at its measured minimum, is\n"
        "        818.83px; dropping the lockup tagline's --gl-tag-min from 7px to 5px - the\n"
        "        only other compressible number - takes it to about 785. Anything below that\n"
        "        needs a design decision, not a rounding one: shrink the button type, or take\n"
        "        an item out of the row. Neither is a call this script gets to make.\n"
        "\n"
        "    Set GEN_SPONSOR_ALLOW_WIDE=1 only to generate a page for measuring; it still\n"
        "    refuses to write the header."
        % (SHELL_W, SHELL_W_CEILING, chr(92), SHELL_W_CEILING, COPY_W,
           "pinned; 225 is the measured minimum for three lines",
           LOCK_W, 3.0 * ROW_GAP, 3, ROW_GAP))

# ---------------------------------------------------------------------------------------------
# THE ONLY CSS IN THIS PAGE THAT IS NOT THE PLUGIN'S
# ---------------------------------------------------------------------------------------------
# It is appended AFTER the plugin's rules inside the same <style>, so where a declaration
# collides the later one wins on cascade order alone - no !important, no rewritten selector,
# not one byte of theirs edited. Delete this block and the page falls straight back to their
# vertical stack.
#
#   .go-row            the flex context. ONE ROW, four items, left to right, and
#                      align-items:stretch so they all come out the same height - which is the
#                      operator's actual requirement for this layout. stretch equalises to the
#                      TALLEST item, so everything else in this block exists to make sure the
#                      tallest item is the Ko-fi button and not one of the other three.
#   .go-row>*          flex:0 0 auto - nothing grows, nothing shrinks. A shrinking item would
#                      rewrap the statement or clip the lockup's nowrap tagline, and the lockup
#                      is `overflow: clip`, which reports no scrollWidth and so cannot be caught.
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
#   .shell.is-open     the width, from SHELL_W - or `max-content` under --measure. This is the
#                      declaration that overrides the plugin's 272px.
GO_LAYOUT_CSS = """
.go-row{display:flex;flex-direction:row;align-items:stretch;gap:%(gap)dpx}
.go-row>*{flex:0 0 auto}
.go-row>.kofi{width:max-content;margin-top:0}
.go-row>.gh{width:max-content;margin-top:0;display:flex;align-items:stretch}
.go-row>.gh>.gh__inner{flex:1 1 auto}
.go-item{display:flex;align-items:center;box-sizing:border-box;min-width:0;margin:0}
.go-item--copy{width:%(copy)dpx}
.go-item--lock{width:%(lock)dpx;align-items:stretch}
.go-row>.go-item--lock .goat-lockup{font-size:%(lockfs)spx}
.go-row>.go-item .goat__copy{margin:0;gap:%(copygap)dpx;flex:1 1 auto;min-width:0}
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
# longer theirs - see SHELL_W. The read is kept so a change on their side appears in this
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
print("plugin .shell.is-open = %dpx  (PROVENANCE ONLY - unused; this page ships SHELL_W = %dpx)"
      % (PLUGIN_CSS_W, SHELL_W))


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

# ---- REGROUP: one column -> ONE ROW OF FOUR --------------------------------------------------
# The plugin emits these three siblings, in this order, all in one column:
#     <button class="kofi">   Ko-fi
#     <button class="gh">     Star Project on Github
#     <div class="goat">      two statement lines, then the GOATPROJECT lockup
#
# Game Optimizer wants the same FOUR things in ONE ROW, left to right:
#     [ Ko-fi ] [ Star Project on Github ] [ statement ] [ GOATPROJECT lockup ]
#
# The first two are already siblings and cross over untouched. The other two are NOT siblings -
# they are both inside `<div class="goat">` - so that block is split one level further and its
# two children become row items in their own right. `.goat` therefore stops being a wrapper
# around a group... except as a CLASS, which is kept on the lockup's new wrapper because seven of
# the plugin's rules are written `.goat .goat-lockup...` and the lockup loses its font-size, its
# background, its padding and its scale variables without that ancestor. See GO_LAYOUT_CSS.
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
regrouped = ('<div class="go-row">'
             + part_kofi
             + part_gh
             + W_COPY_OPEN + part_copy + '</div>'
             + W_LOCK_OPEN + part_lock + '</div>'
             + _goat_ws +
             '</div>')
print("regrouped: kofi=%d gh=%d copy=%d lock=%d bytes into one row -> %d bytes"
      % (len(part_kofi), len(part_gh), len(part_copy), len(part_lock), len(regrouped)))
for _probe in ('<div class="go-row">', W_COPY_OPEN, W_LOCK_OPEN):
    if regrouped.count(_probe) != 1:
        raise SystemExit("*** %s appears %d times in the regrouped markup, expected 1"
                         % (_probe, regrouped.count(_probe)))
if '<div class="go-col' in regrouped:
    raise SystemExit("*** the old two-group .go-col wrapper is still being emitted - this is a "
                     "half-applied single-row layout")
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
_added = len('<div class="go-row">') + len(W_COPY_OPEN) + len(W_LOCK_OPEN) + 3 * len("</div>")
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
# so the page can report its own natural size; otherwise it is pinned to SHELL_W.
go_css = GO_LAYOUT_CSS % {"gap": ROW_GAP, "copy": COPY_W, "lock": LOCK_W,
                          "lockfs": ("%.3f" % LOCK_FS).rstrip("0").rstrip("."),
                          "copygap": COPY_GAP,
                          "shell": "max-content" if MEASURE else "%dpx" % SHELL_W}
print("layout override: one row, gap %dpx; .go-item--copy %dpx (line gap %dpx), "
      ".go-item--lock %dpx (lockup font-size %spx, from KOFI_H %.3f); .shell.is-open width -> %s"
      % (ROW_GAP, COPY_W, COPY_GAP, LOCK_W, ("%.3f" % LOCK_FS).rstrip("0").rstrip("."), KOFI_H,
         "max-content  [MEASURE MODE]" if MEASURE else "%dpx" % SHELL_W))

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
# THE PANEL IS ANCHORED BOTTOM-RIGHT, not top-left, and that is what decides which edge gets
# cut when the window is too small for it. The host rectangle is normally the panel's exact
# natural size, so at every ordinary size the two are identical - it only matters at the
# extreme. There the layout clips the panel rather than moving the OK/Cancel/Apply row, and
# anchoring it to the bottom means the part that disappears is the TOP of the stack (the Ko-fi
# button) while the GOATPROJECT lockup at the bottom stays put and stays aligned with Apply.
# Anchored top-left the opposite happens and the lockup is the first thing to vanish.
app_html = ("<!doctype html><meta charset='utf-8'>"
            "<style>html,body{margin:0;padding:0;overflow:hidden;background:transparent}"
            "#h{position:absolute;right:0;bottom:0}</style>"
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


# The panel's natural size is SHELL_W x SHELL_H, declared at the top of this file next to the
# measure-mode flag that produced it. It used to be computed here - width scraped out of the
# plugin's `.shell.is-open` rule, height measured - and that is no longer true of the width:
# this page is a SINGLE ROW OF FOUR and the plugin's 272px describes their one-column stack.
# BOTH numbers are now measured, both live up there, and PLUGIN_CSS_W (also up there) keeps the
# plugin's value visible as provenance without letting it decide anything.
#
# Both are .shell's BORDER BOX. The card's soft drop shadow (--glass-shadow: 0 8px 22px) falls
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
A("// The panel's natural size in CSS pixels. The host reserves this much room and scales it")
A("// by the monitor's DPI; WebView2 rasterises CSS px at that same scale.")
A("//")
A("// BOTH numbers are MEASURED against this exact page by tools\\measure-panel.py - neither is")
A("// read out of the plugin's stylesheet, and the width USED TO BE. That changed when the")
A("// panel became a single row of four: the plugin's `.shell.is-open { width: %dpx }`" % PLUGIN_CSS_W)
A("// describes their ONE-COLUMN stack, so for this page it is the wrong number. It is still")
A("// read at generation time and printed, as provenance only.")
A("//")
A("// Re-measure with:  python tools\\gen-sponsor-html.py --measure")
A("//                   python tools\\measure-panel.py")
A("//                   (write the result into SHELL_W / SHELL_H, re-run without --measure,")
A("//                    then measure once more - it must come back identical.)")
A("constexpr int kSponsorCssWidth  = %d;" % SHELL_W)
A("constexpr int kSponsorCssHeight = %d;" % SHELL_H)
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
    # a page whose real width no longer matches kSponsorCssWidth, and nothing downstream would
    # notice: the host would reserve one rectangle and the panel would lay out to another.
    print("MEASURE MODE - src\\sponsor_html.h was NOT written and is unchanged.")
    print("  panel-app.html is the max-content page; measure it, write SHELL_W / SHELL_H,")
    print("  then re-run WITHOUT --measure to regenerate the header.")
elif SHELL_W > SHELL_W_CEILING:
    # Reached only via GEN_SPONSOR_ALLOW_WIDE=1, whose whole purpose is to produce a PAGE that
    # can be measured at the pinned width - pass 2 of the two-pass loop. It is deliberately NOT
    # a way to ship an over-budget panel: the header is refused here as well, so the only route
    # to shipping one is to change SHELL_W_CEILING on purpose, which is a decision with a name
    # on it rather than an environment variable somebody exported once.
    print("OVER THE %dpx CEILING (SHELL_W = %d) - src\\sponsor_html.h was NOT written and is"
          % (SHELL_W_CEILING, SHELL_W))
    print("  unchanged; it still carries the previous layout, which is the safe state.")
    print("  panel-app.html IS the pinned single-row page, so tools\\measure-panel.py can")
    print("  measure it - that is the only thing GEN_SPONSOR_ALLOW_WIDE is for.")
else:
    io.open(out_path, "w", encoding="utf-8", newline="\r\n").write(hdr_text)
    print("wrote src\\sponsor_html.h (%d bytes, %d literals, natural size %dx%d css px)"
          % (len(hdr_text), len(chunks(app_html, CHUNK)), SHELL_W, SHELL_H))
