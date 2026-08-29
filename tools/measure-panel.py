# Measure the sponsor panel's NATURAL SIZE by rendering it, and print the numbers.
#
# RUN IT WITH:
#     python tools\gen-sponsor-html.py --measure    (writes panel-app.html at width:max-content)
#     python tools\measure-panel.py                 (read .shell, write it into SHELL_W/SHELL_H)
#     python tools\gen-sponsor-html.py              (writes the header, pinned to those numbers)
#     python tools\measure-panel.py                 (must now report exactly SHELL_W x SHELL_H)
#
# WHY THIS EXISTS
# ---------------
# src\sponsor_html.h has to tell the Win32 layout how much room to reserve for the panel, and
# getting that number wrong is not a cosmetic problem: reserve too little and the panel is
# clipped, reserve too much and a band of dead space opens above the footer.
#
# BOTH numbers are now measured here. The width used to be read straight out of the plugin's
# `.shell.is-open { width: 272px }` rule, and that stopped being right when Game Optimizer
# relaid the panel out as ONE SINGLE ROW OF FOUR - Ko-fi, Star Project on Github, the statement,
# then the GOATPROJECT lockup, left to right. 272px describes the plugin's ONE-COLUMN stack.
# This page is a different shape, so its width is a measurement like its height already was, and
# gen-sponsor-html.py's --measure flag exists to break the circularity: it emits
# `width:max-content` so the page can report the size it actually wants.
#
# WHAT IT CHECKS, BEYOND THE BOX
# ------------------------------
# 1. EQUAL HEIGHT, which is the operator's actual requirement for this layout - "align all
#    height same as Ko-fi button". Two separate things have to be true and only one of them is
#    obvious:
#
#      (a) the four items are all the same height. `align-items: stretch` on .go-row does this
#          for free, so on its own this check is nearly vacuous - it passes whatever the height
#          turns out to be.
#      (b) that shared height is the KO-FI BUTTON'S OWN height. stretch equalises to the
#          TALLEST item, so if the statement or the lockup is even 1px taller than Ko-fi then
#          Ko-fi is the one being stretched and the ruler has silently changed. Nothing about
#          the rendered panel looks different when this goes wrong; the row is simply keyed to
#          the wrong element from then on.
#
#    (b) is measured by turning stretch off for one layout pass - `align-self: flex-start` on
#    every row child - reading each item's NATURAL height, and putting it back. Ko-fi must come
#    out the tallest, and the stretched height must equal Ko-fi's natural height.
#
# 2. OVERFLOW. A row can be wider than the shell that holds it, and `.shell { overflow: hidden }`
#    will CLIP it in silence. A clipped Ko-fi button still renders, still takes clicks on the
#    part you can see, and looks like a styling choice. So every probed element is asked for
#    scrollWidth vs clientWidth (and scrollHeight vs clientHeight) and any overflow is a FAILURE.
#
#    .goat-lockup IS DELIBERATELY EXCLUDED FROM THAT VERDICT and always has been. It reports
#    scrollWidth over clientWidth - 273 against 244 in the old two-group layout, 213 against 190
#    here - because of the plugin's own meteor layer, `position: absolute; inset: -22% -12%`,
#    which is deliberately larger than its box and sits behind `overflow: clip`. It reads the
#    same way in the plugin's own vertical stack and is not a symptom of anything this script
#    does. The same is true of its scrollHeight, for the -22% on the vertical axis.
#
# 3. WHAT `overflow: clip` HIDES, which is the reason 2 is not enough. clip does not create a
#    scroll container, so an element clipped by it reports NO scrollWidth or scrollHeight
#    overflow at all - the check in 2 is blind to it by construction. Three things inside the
#    lockup can be cut off with no trace: the 5.4em mark, the wordmark block, and the two nowrap
#    strings inside the wordmark. Those are checked directly, against the boxes that contain
#    them, rather than through a scroll metric that cannot see them.
#
# HOW, WITHOUT INSTALLING ANYTHING
# --------------------------------
# There is no Playwright and no websocket module for Python on this machine, so there is no
# CDP client to ask for layout metrics. Chrome's own --dump-dom prints the serialised DOM
# after scripts have run, which IS a channel back: the probe page measures itself and writes
# the answer into an element, and --dump-dom hands it over. No download, no new dependency.
#
# The panel lives in a shadow root and shadow content does not serialise - which does not
# matter, because only the numbers need to come back, and they are put in the light DOM.
import io, json, math, os, re, subprocess, sys, tempfile

S = (r"C:/Users/IRP/AppData/Local/Temp/claude/"
     r"F--Game-Optimizer--claude-worktrees-windows-cpu-sets-tray-c96e1a/"
     r"68d33dbc-1415-4f46-9fe1-be03413760eb/scratchpad")

BROWSERS = [
    os.path.expandvars(r"%LOCALAPPDATA%\ms-playwright\chromium_headless_shell-1234"
                       r"\chrome-headless-shell-win64\chrome-headless-shell.exe"),
    r"C:\Program Files\Google\Chrome\Application\chrome.exe",
    r"C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe",
]

# Every selector the probe reports. `.shell` is the answer; the rest exist so that when the
# answer is wrong it is obvious WHICH item is the wrong size, instead of a single number that is
# off by an amount nobody can attribute.
#
#   .shell              the card the whole panel is drawn inside - this is SHELL_W x SHELL_H
#   .body__pad          the shell's content box, after its 13px side padding
#   .go-row             the single flex row (Game Optimizer's wrapper, not the plugin's)
#   .kofi .gh           items 1 and 2, the two buttons
#   .go-item--copy      item 3's wrapper - the two statement lines
#   .go-item--lock      item 4's wrapper - the GOATPROJECT lockup. Also carries the plugin's own
#                       `goat` class, because seven of their rules are `.goat .goat-lockup...`
#   .goat__copy         the statement itself, inside item 3
#   .goat-lockup        the lockup <a> itself, inside item 4
#   .gh__inner          the dark pill inside .gh. It must fill .gh once .gh is stretched, or a
#                       band of bare #262626 opens under the label
#   .goat-lockup__mark  the seal. 5.4em tall, so it is what the lockup's font-size buys
#   .goat-lockup__word  the wordmark column: name over tagline
#   .goat-lockup__name  "Goatproject", white-space: nowrap
#   .goat-lockup__tag   "The People's Compute Commons", white-space: nowrap - the widest thing
#                       in the lockup and therefore what sets the whole item's width
SELECTORS = [".shell", ".body__pad", ".go-row",
             ".kofi", ".gh", ".go-item--copy", ".go-item--lock",
             ".goat__copy", ".goat-lockup",
             ".gh__inner", ".goat-lockup__mark", ".goat-lockup__word",
             ".goat-lockup__name", ".goat-lockup__tag"]

# The four items of the row, in the order they are laid out. This list IS the operator's
# requirement: these four must all be the same height, and that height must be .kofi's.
ITEMS = [".kofi", ".gh", ".go-item--copy", ".go-item--lock"]
RULER = ".kofi"

# The layout MUST contain these. A page without .go-item--copy is a pre-change page - the
# two-group layout, or the plugin's vertical stack - and measuring it would hand back a width
# for a panel the app no longer ships.
REQUIRED = [".shell", ".go-row", ".kofi", ".gh", ".go-item--copy", ".go-item--lock"]

# Overflow is checked on these. .shell is here because it is the element that actually carries
# `overflow: hidden` and therefore the one that does the clipping; .go-row because that is where
# a too-wide item shows up first; the two wrappers because they are this script's own boxes and
# a mis-set width shows up there before anywhere else.
#
# .goat-lockup IS NOT HERE ON PURPOSE - see the header. Its scroll overflow is the plugin's
# meteor layer and is pre-existing.
OVERFLOW_CHECK = [".shell", ".body__pad", ".go-row", ".go-item--copy", ".go-item--lock",
                  ".kofi", ".gh"]

# Elements whose computed padding the probe reports, so a content-box fit can be checked.
# clientWidth/clientHeight are the PADDING box, not the content box, so the padding has to come
# back separately or every fit check inside a padded element is wrong by 20px.
PAD_OF = [".goat-lockup", ".go-item--copy", ".gh"]

# Sub-pixel tolerance. Layout arithmetic here lands on thirds of a pixel (14.69, 47.375), and a
# difference under 1px cannot be seen and cannot clip a glyph.
TOL = 1.0

# The constraint the Settings window imposes, DERIVED from the C++ rather than guessed - the
# same derivation gen-sponsor-html.py carries at SHELL_W_CEILING, and both must agree:
#
#   [M] src\settings.cpp   SettingsLayout:  if (spW > cw - 2 * GAP) spW = cw - 2 * GAP;
#                          and              const int GAP = theme::Dp(theme::metric::kGap, dpi);
#   [M] src\theme.h:71     constexpr int kGap = 12;
#   [M] src\theme.cpp:329  int Dp(int logical, int dpi) { return MulDiv(logical, dpi, 96); }
#   [M] src\settings.cpp   WM_GETMINMAXINFO:  RECT need = { 0, 0, theme::Dp(860, dpi), needH };
#   [M] src\webview_host.cpp:666  s.cx = ::MulDiv(kSponsorCssWidth, dpi, 96);
#
# 860 - 2 * 12 = 836 css px available at the SMALLEST allowed window. 836 is not the ceiling
# though: the three MulDiv calls round independently, and at 836 the panel overruns by 1px at
# 144 of the 385 integer DPI values from 96 to 480 - all of them custom-scaling DPIs, so a
# machine anyone actually tested on would look fine. 835 is the largest width that is safe at
# every one, so 835 is the number.
SHELL_W_CEILING = 835


def find_browser():
    for b in BROWSERS:
        if os.path.exists(b):
            return b
    raise SystemExit("*** no headless browser found; tried:\n  " + "\n  ".join(BROWSERS))


page_path = os.path.join(S, "panel-app.html")
if not os.path.exists(page_path):
    raise SystemExit("*** %s is missing - run tools\\gen-sponsor-html.py first" % page_path)

page = io.open(page_path, encoding="utf-8").read()

# The probe: the app page, plus a measuring script appended AFTER the app's own script has
# attached the shadow root and injected the panel. It reads the real laid-out boxes and writes
# them, as JSON, where --dump-dom will show them.
#
# The viewport is deliberately much larger than the panel, so nothing is constrained by it and
# what comes back is the panel's own intrinsic size rather than the window's. That matters more
# than it used to: under --measure the shell is `width:max-content`, so a narrow viewport would
# silently become the answer.
#
# It measures SYNCHRONOUSLY. requestAnimationFrame was tried first and never fired: under
# --virtual-time-budget in the headless shell nothing paints, so a rAF chain never resolves
# and --dump-dom prints a page with no answer in it - which looks exactly like a page whose
# script threw. getBoundingClientRect forces layout on the spot and needs no frame.
#
# It then measures AGAIN once webfonts have settled and overwrites the first answer, because
# where the copy wraps - and therefore whether it still fits the Ko-fi height - depends on the
# resolved font. Both paths write the same element, so whichever ran last is what --dump-dom
# reports.
#
# THE NATURAL PASS IS LAST AND IT PUTS ITSELF BACK. `align-self: flex-start` is injected into
# the shadow root as its own <style>, the four items are re-read, and the style is removed
# again - so the boxes reported above it are the real, stretched ones and the naturals are a
# separate map. Doing it the other way round would report a panel nobody ships.
probe_js = (
    "var SEL=" + json.dumps(SELECTORS) + ";"
    "var ITEMS=" + json.dumps(ITEMS) + ";"
    "var PADOF=" + json.dumps(PAD_OF) + ";"
    "function M(tag){"
    "var sh=document.getElementById('h').shadowRoot;"
    "var o={tag:tag,box:{},flow:{},pad:{},natural:{}};"
    "SEL.forEach(function(s){"
    "var e=sh.querySelector(s);"
    "if(!e){o.box[s]=null;return;}"
    "var b=e.getBoundingClientRect();"
    "o.box[s]=[b.width,b.height];"
    "o.flow[s]=[e.scrollWidth,e.clientWidth,e.scrollHeight,e.clientHeight];"
    "});"
    "PADOF.forEach(function(s){"
    "var e=sh.querySelector(s);"
    "if(!e){o.pad[s]=null;return;}"
    "var c=getComputedStyle(e);"
    "o.pad[s]=[parseFloat(c.paddingTop)||0,parseFloat(c.paddingBottom)||0,"
    "parseFloat(c.paddingLeft)||0,parseFloat(c.paddingRight)||0];"
    "});"
    "var st=document.createElement('style');"
    "st.textContent='.go-row>*{align-self:flex-start!important}"
    ".go-item--lock{align-items:center!important}';"
    "sh.appendChild(st);"
    "ITEMS.forEach(function(s){"
    "var e=sh.querySelector(s);"
    "o.natural[s]=e?e.getBoundingClientRect().height:null;"
    "});"
    "st.remove();"
    "var d=document.getElementById('measured');"
    "if(!d){d=document.createElement('div');d.id='measured';document.body.appendChild(d);}"
    "d.textContent=JSON.stringify(o);"
    "}"
    "M('sync');"
    "if(document.fonts&&document.fonts.ready){document.fonts.ready.then(function(){M('fonts');});}"
    "setTimeout(function(){M('settled');},1200);"
)
probe = page + "<script>" + probe_js + "</script>"

tmp = os.path.join(tempfile.gettempdir(), "panel-measure-probe.html")
io.open(tmp, "w", encoding="utf-8").write(probe)

browser = find_browser()
print("browser: %s" % browser)
print("page   : %s (%d bytes)" % (page_path, len(page)))
cmd = [browser, "--headless", "--disable-gpu", "--no-sandbox",
       "--window-size=1200,1200", "--virtual-time-budget=4000",
       "--dump-dom", "file:///" + tmp.replace("\\", "/")]
res = subprocess.run(cmd, capture_output=True, text=True, timeout=120)
dom = res.stdout or ""

# The <div id="measured"> is the only element with that id, and JSON contains no "<", so a
# non-greedy read to the next "<" is exact. The probe's own source is also in the dumped DOM -
# matching on the id's SERIALISED form (double quotes) and not on the script text is what keeps
# this from reading its own source back.
m = re.search(r'<div id="measured">([^<]*)</div>', dom)
if not m:
    sys.stderr.write(res.stderr[-2000:] if res.stderr else "(no stderr)\n")
    raise SystemExit("*** the probe never reported a size - the page did not run")

data = json.loads(m.group(1))
box, flow, pad, natural, when = (data["box"], data["flow"], data["pad"],
                                 data["natural"], data["tag"])

# Whether this page is a --measure page is readable straight off it, and worth printing: a run
# that measured the max-content page and a run that measured the pinned page look identical in
# their numbers when everything is right, and completely different when it is not.
#
# MATCH THE WHOLE DECLARATION, not just `width:max-content`. Other rules in the same block carry
# width:max-content and always will - `.go-row>.kofi` and `.go-row>.gh` both do - so a substring
# test on that alone calls EVERY page a measure page. It did, and it printed that under a correct
# set of numbers, which is exactly the shape of wrong that survives a glance.
_shell_w = re.search(r"\.shell\.is-open\{width:([^}]*)\}", page)
if not _shell_w:
    mode = "UNKNOWN - no `.shell.is-open{width:...}` rule in this page at all"
elif _shell_w.group(1) == "max-content":
    mode = "max-content  [--measure page: this reports what the panel WANTS]"
else:
    mode = "%s  [shipping page: this reports what the panel WAS GIVEN]" % _shell_w.group(1)
print("shell   : %s" % mode)
print("reported at: %s" % when)
print("")

failures = []

for s in REQUIRED:
    if box.get(s) is None:
        failures.append("FAILURE: %s is not in this page. That means panel-app.html predates the "
                        "single-row layout - regenerate it with tools\\gen-sponsor-html.py." % s)

print("  %-20s %-20s %s" % ("element", "border box (w x h)", "scrollW/clientW  scrollH/clientH"))
print("  %-20s %-20s %s" % ("-" * 20, "-" * 20, "-" * 33))
for s in SELECTORS:
    b = box.get(s)
    if b is None:
        print("  %-20s %-20s %s" % (s, "absent", ""))
        continue
    f = flow.get(s) or [0, 0, 0, 0]
    print("  %-20s %-20s %5d/%-5d    %5d/%-5d"
          % (s, "%.2f x %.2f" % (b[0], b[1]), f[0], f[1], f[2], f[3]))

# ---------------------------------------------------------------------------------------------
# THE EQUAL-HEIGHT CHECK - the operator's requirement, machine-checked
# ---------------------------------------------------------------------------------------------
print("")
print("EQUAL HEIGHT - all four items must match the Ko-fi button")
print("  %-20s %-14s %-14s" % ("item", "as laid out", "natural"))
print("  %-20s %-14s %-14s" % ("-" * 20, "-" * 14, "-" * 14))
_laid, _nat = {}, {}
for s in ITEMS:
    b, n = box.get(s), natural.get(s)
    print("  %-20s %-14s %-14s"
          % (s,
             "absent" if b is None else "%.3f" % b[1],
             "absent" if n is None else "%.3f" % n))
    if b is not None:
        _laid[s] = b[1]
    if n is not None:
        _nat[s] = n

if len(_laid) != len(ITEMS):
    failures.append("FAILURE: only %d of the %d row items could be measured, so the equal-height "
                    "requirement cannot be checked at all." % (len(_laid), len(ITEMS)))
else:
    _spread = max(_laid.values()) - min(_laid.values())
    if _spread > TOL:
        _worst = max(_laid, key=lambda k: _laid[k])
        _least = min(_laid, key=lambda k: _laid[k])
        failures.append(
            "FAILURE: the four row items are NOT the same height - they spread %.3fpx, from %s "
            "at %.3f to %s at %.3f. The operator's requirement for this layout is that all four "
            "match the Ko-fi button."
            % (_spread, _least, _laid[_least], _worst, _laid[_worst]))
    else:
        print("  -> all four within %.3fpx of each other (tolerance %.1f)" % (_spread, TOL))

    # And the harder half: the height they all share has to be KO-FI'S, not whatever happened to
    # be the tallest. stretch cannot tell the difference and neither can a screenshot.
    if RULER in _nat and len(_nat) == len(ITEMS):
        _tallest = max(_nat, key=lambda k: _nat[k])
        if _nat[_tallest] > _nat[RULER] + TOL:
            failures.append(
                "FAILURE: %s is the TALLEST item at its natural height %.3fpx, over %s's %.3fpx. "
                "align-items:stretch equalises to the tallest, so the row is keyed to %s and the "
                "Ko-fi button is the thing being stretched - the opposite of what was asked."
                % (_tallest, _nat[_tallest], RULER, _nat[RULER], _tallest))
        _common = max(_laid.values())
        if abs(_common - _nat[RULER]) > TOL:
            failures.append(
                "FAILURE: the row is %.3fpx tall but %s's own natural height is %.3fpx (off by "
                "%.3f). The four items match each other but they do not match the Ko-fi button."
                % (_common, RULER, _nat[RULER], abs(_common - _nat[RULER])))
        else:
            print("  -> and that height is %s's own natural %.3fpx (row %.3f)"
                  % (RULER, _nat[RULER], _common))
    else:
        failures.append("FAILURE: the natural (unstretched) heights did not come back, so "
                        "whether the shared height is the Ko-fi button's is UNVERIFIED.")

# ---------------------------------------------------------------------------------------------
# WHAT `overflow: clip` HIDES
# ---------------------------------------------------------------------------------------------
# The scroll-metric check below cannot see any of this: .goat-lockup is `overflow: clip`, which
# does not create a scroll container, so content cut off inside it reports nothing at all. These
# are direct box-against-box comparisons instead.
print("")
print("FIT INSIDE overflow:clip (scroll metrics are blind to these)")


def _fits(what, inner, outer, axis, outer_pad=(0.0, 0.0)):
    """inner's axis extent must fit outer's CONTENT box. clientW/H are the PADDING box."""
    bi, bo = box.get(inner), box.get(outer)
    if bi is None or bo is None:
        failures.append("FAILURE: %s - %s or %s is missing, so the fit is UNVERIFIED."
                        % (what, inner, outer))
        return
    i = bi[0] if axis == "w" else bi[1]
    o = (bo[0] if axis == "w" else bo[1]) - outer_pad[0] - outer_pad[1]
    ok = i <= o + TOL
    print("  %-46s %8.3f in %8.3f   %s" % (what, i, o, "ok" if ok else "*** CLIPPED ***"))
    if not ok:
        failures.append("FAILURE: %s - %s is %.3fpx against %.3fpx of room inside %s, so %.3fpx "
                        "is being cut off with no scroll metric to show it."
                        % (what, inner, i, o, outer, i - o))


_lp = pad.get(".goat-lockup") or [0.0, 0.0, 0.0, 0.0]
_cp = pad.get(".go-item--copy") or [0.0, 0.0, 0.0, 0.0]
_fits("the seal fits the lockup's height", ".goat-lockup__mark", ".goat-lockup", "h",
      (_lp[0], _lp[1]))
_fits("the wordmark fits the lockup's height", ".goat-lockup__word", ".goat-lockup", "h",
      (_lp[0], _lp[1]))
_fits("'Goatproject' fits the wordmark", ".goat-lockup__name", ".goat-lockup__word", "w")
_fits("the nowrap tagline fits the wordmark", ".goat-lockup__tag", ".goat-lockup__word", "w")
_fits("the statement fits its wrapper", ".goat__copy", ".go-item--copy", "h", (_cp[0], _cp[1]))

# Not a fit but a FILL, and the opposite comparison. .gh is display:block in the plugin with a
# 1px frame and one in-flow child that paints the dark pill; stretched to the Ko-fi height as a
# block, the frame grows and the pill does not, and an 11px band of bare #262626 opens under the
# label. GO_LAYOUT_CSS makes .gh a flex container so the pill stretches with it. This is the
# check that says it worked - it is invisible to every overflow metric, because nothing
# overflows: the button is simply the right size and half empty.
_gp = pad.get(".gh") or [0.0, 0.0, 0.0, 0.0]
_bg, _bi = box.get(".gh"), box.get(".gh__inner")
if _bg is None or _bi is None:
    failures.append("FAILURE: .gh or .gh__inner is missing, so whether the pill fills the "
                    "stretched button is UNVERIFIED.")
else:
    _room = _bg[1] - _gp[0] - _gp[1]
    _ok = _bi[1] >= _room - TOL
    print("  %-46s %8.3f of %8.3f   %s"
          % ("the dark pill fills the stretched .gh", _bi[1], _room,
             "ok" if _ok else "*** HALF EMPTY ***"))
    if not _ok:
        failures.append("FAILURE: .gh__inner is %.3fpx inside a %.3fpx .gh content box, so "
                        "%.3fpx of bare #262626 frame is showing under the label. .gh needs to "
                        "be a flex container for its pill to stretch with it."
                        % (_bi[1], _room, _room - _bi[1]))

print("")
for s in OVERFLOW_CHECK:
    f = flow.get(s)
    if not f:
        continue
    sw, cw, sh, ch = f
    if sw > cw:
        failures.append("FAILURE: %s OVERFLOWS HORIZONTALLY - scrollWidth %d > clientWidth %d "
                        "(%dpx of content is being clipped, i.e. a button is cut off)."
                        % (s, sw, cw, sw - cw))
    if sh > ch:
        failures.append("FAILURE: %s OVERFLOWS VERTICALLY - scrollHeight %d > clientHeight %d "
                        "(%dpx clipped)." % (s, sh, ch, sh - ch))

_lf = flow.get(".goat-lockup")
if _lf:
    print("no overflow on any of: %s" % ", ".join(OVERFLOW_CHECK))
    print("  .goat-lockup is EXCLUDED from that verdict on purpose: it reports scrollWidth %d "
          "against" % _lf[0])
    print("  clientWidth %d and scrollHeight %d against clientHeight %d, which is the PLUGIN'S "
          "OWN" % (_lf[1], _lf[2], _lf[3]))
    print("  meteor layer - `position:absolute; inset:-22% -12%` behind `overflow:clip`, "
          "deliberately")
    print("  larger than its box. It reads the same in the plugin's vertical stack. The things "
          "that")
    print("  clip WOULD hide are checked directly, above.")

shell = box.get(".shell")
if shell:
    w, h = int(math.ceil(shell[0])), int(math.ceil(shell[1]))
    print("")
    print("MEASURED .shell = %d x %d css px  (reported at: %s)" % (w, h, when))
    print("")
    print("Write these into tools\\gen-sponsor-html.py:")
    print("    SHELL_W = %d" % w)
    print("    SHELL_H = %d" % h)
    if w > SHELL_W_CEILING:
        _row = box.get(".go-row")
        _parts = "  ".join("%s %.2f" % (s, box[s][0]) for s in ITEMS if box.get(s))
        failures.append(
            "FAILURE: .shell measured %dpx wide, past the %dpx ceiling. At the minimum window "
            "size settings.cpp caps the panel at `cw - 2 * GAP` and CLIPS the overrun at the "
            "panel's own left edge, silently. DO NOT SHIP THIS.\n"
            "         The row is %.2fpx of that; the four items are: %s.\n"
            "         Reaching the ceiling is a design decision - shrink type, or take an item "
            "out of the row - not a rounding one."
            % (w, SHELL_W_CEILING, (_row[0] if _row else -1), _parts))

if failures:
    print("")
    print("=" * 90)
    for line in failures:
        print(line)
    print("=" * 90)
    print("%d FAILURE(S). The panel does NOT fit; the numbers above are not safe to ship."
          % len(failures))
    sys.exit(1)

print("")
print("ALL CHECKS PASSED: four items equal height, that height is the Ko-fi button's, nothing")
print("clipped inside overflow:clip, no scroll overflow, and .shell is within the %dpx ceiling."
      % SHELL_W_CEILING)
