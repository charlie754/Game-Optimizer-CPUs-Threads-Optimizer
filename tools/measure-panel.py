# Measure the sponsor panel BY RENDERING IT, AT MANY WIDTHS, and fail loudly on any violation.
#
# RUN IT WITH:
#     python tools\gen-sponsor-html.py --measure    (writes panel-app.html at width:max-content)
#     python tools\measure-panel.py                 (reports the intrinsic box -> SHELL_MIN_W/H)
#     python tools\gen-sponsor-html.py              (writes the header; the page is width:100%)
#     python tools\measure-panel.py                 (SWEEPS the shipping page and must agree)
#
# WHY THIS EXISTS, AND WHY IT IS NOT ONE MEASUREMENT ANY MORE
# ----------------------------------------------------------
# The panel used to be a fixed 831 x 65 css px card, so one measurement settled it: render it
# once, read `.shell`, write the two numbers into the generator. THAT IS NO LONGER TRUE. The
# panel now FILLS the Settings window's content row - `.shell.is-open { width: 100% }`, and
# settings.cpp hands the host `cw - 2 * GAP` at `spLeft = GAP` - so its width is whatever the
# user's window is, and a single measurement describes exactly one window size out of thousands.
#
# A RESPONSIVE LAYOUT WITH A SINGLE-WIDTH TEST IS THE SHAPE OF THING THAT LOOKS VERIFIED AND IS
# NOT. So this script renders the SAME page at a range of host widths and asserts, at EVERY one:
#
#   1. nothing overflows and the three groups do not collide;
#   2. the GOAT Cam plate is the tallest item and sets the row's height; Ko-fi and Star are one
#      height with each other ("same height as Ko-fi"), and every item is centred in the row;
#   3. the gap between the statement and the GOAT Cam plate is STRICTLY SMALLER than the gaps
#      between the three groups - which is the acceptance test for "statement and banner as 1
#      group", and is not something anyone can eyeball reliably at 10px against 20px;
#   4. the gap above the row equals the gap below it - the acceptance test for "buttons too
#      close to panel upside edge". Both are 0 now: the plate fills the band;
#   5. nothing on the plate runs off it: the 72px App Store QR at full size, the photo layer,
#      the viewfinder, the wordmark - and the wordmark and viewfinder stay clear of the QR.
#
# and then SWEEPS DOWNWARD to find the width at which it FIRST breaks. That width plus one is
# SHELL_MIN_W / kSponsorCssMinWidth: the floor WM_GETMINMAXINFO has to keep the window above.
# It is derived here rather than reasoned about, because the sum of four intrinsic widths and
# two gaps is exactly the kind of arithmetic that is right on paper and one pixel out in a
# layout engine.
#
# TWO INDEPENDENT ROUTES TO THE SAME FLOOR, AND THEY HAVE TO AGREE. The `--measure` page is
# `width: max-content`, which reports the panel's intrinsic minimum DIRECTLY; the sweep finds it
# by walking down until something breaks. Neither is checked against the other automatically -
# they are two passes of a manual loop - but they are two different mechanisms and a
# disagreement between them means one of them is measuring the wrong thing.
#
# WHAT IT ALSO CHECKS, BEYOND THE BOX
# -----------------------------------
# 1. THE HEIGHT CONTRACT. The row used to be keyed to the Ko-fi button. The GOAT Cam plate is
#    now the ruler - it is 74 css px because its App Store QR is 72 - and the band it sets is
#    kSponsorCssHeight, the same band the GDI strip reserves. Three things have to be true:
#
#      (a) the plate is the TALLEST item at its NATURAL height. If the headline ever wraps to
#          more lines than the plate's height holds (v1's two-line copy did at five lines, 76.44px,
#          under a 146px column; banner v2's one line takes two in its 136px column, 32.38px) it
#          outgrows the plate, the row grows past the band, and the host cuts the panel's top
#          off. This is the check that catches it.
#      (b) Ko-fi and Star are the same height as each other - the operator's standing "same
#          height as Ko-fi" - now that they no longer set the row.
#      (c) every item is CENTRED in the row: `align-items: center` does that, and this says it
#          happened.
#
#    Natural heights are read by turning alignment off for one layout pass - `align-self:
#    flex-start` on every row child and every child of the banner - and putting it back.
#
# 2. OVERFLOW. A row can be wider than the shell that holds it, and `.shell { overflow: hidden }`
#    will CLIP it in silence. A clipped button still renders, still takes clicks on the part you
#    can see, and looks like a styling choice. So every probed element is asked for scrollWidth
#    vs clientWidth (and scrollHeight vs clientHeight) and any overflow is a FAILURE.
#
#    Banner v2's plate CLIPS NOTHING - the photo is clipped in its own layer, .gc-photo - so the
#    plate itself is in that verdict now (it was excluded in v1, whose meteor layer overran it by
#    design). .gc-photo is the one box left out: its frame is a cover crop, larger than the layer
#    on purpose, and the layer's own `overflow: hidden` is what trims it.
#
# 3. WHAT A SCROLL METRIC CANNOT SAY, which is why 2 is not enough. An absolutely positioned
#    child that runs past its parent shows in scrollWidth only on some axes and some ancestors,
#    and a child that merely overlaps a sibling shows nowhere. So the QR, the photo layer, the
#    viewfinder and the wordmark are checked directly against the plate's padding box, the
#    wordmark and viewfinder against the QR, and the QR must come out at its full 72 css px,
#    because a squeezed code is a code that does not scan.
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
#
# ALL THE WIDTHS ARE MEASURED IN ONE PAGE LOAD, by setting an inline width on the shadow HOST
# (#h) and re-reading the boxes. That is the same thing the app does - the host window IS the
# panel's width - and it beats one browser launch per width by two orders of magnitude, which is
# what makes a 300-step sweep affordable at all.
import io, json, math, os, re, subprocess, sys, tempfile

S = (r"C:/Users/IRP/AppData/Local/Temp/claude/"
     r"F--Game-Optimizer--claude-worktrees-windows-cpu-sets-tray-c96e1a/"
     r"68d33dbc-1415-4f46-9fe1-be03413760eb/scratchpad")

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

BROWSERS = [
    os.path.expandvars(r"%LOCALAPPDATA%\ms-playwright\chromium_headless_shell-1234"
                       r"\chrome-headless-shell-win64\chrome-headless-shell.exe"),
    r"C:\Program Files\Google\Chrome\Application\chrome.exe",
    r"C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe",
]

# Every selector the probe reports. `.shell` is the panel; the rest exist so that when something
# is the wrong size it is obvious WHICH item is wrong, instead of a single number that is off by
# an amount nobody can attribute.
#
#   .shell              the card the whole panel is drawn inside - this is the panel's box
#   .body__pad          the shell's content box. The top/bottom gap check is measured against
#                       THIS, because its padding is the gap (0 and 0 now).
#   .go-row             the single flex row (Game Optimizer's wrapper, not the plugin's)
#   .kofi .gh           groups 1 and 2, the two buttons, drawn at the generator's BTN_ZOOM
#   .goat--strip        GROUP 3 - the master's GOAT Cam banner in its strip variant: the
#                       statement beside the plate, laid out by the master's own sheet
#   .goat__copy         the statement: two copy lines, wrapping to four in a 160px column
#   .goat-lockup        the PLATE - the espresso <a>, the only link in the banner, and the ruler
#   .gh__inner          the dark pill inside .gh. It must fill .gh, or a band of bare #262626
#                       opens under the label
#   .gc-photo           the coffee photo, grid column 1
#   .gc-vf              the viewfinder brackets, inset over the photo; must stay clear of the QR
#   .goat-lockup__word  the wordmark column: "GOAT Cam" over the two-line tagline
#   .goat-lockup__name  "GOAT Cam", white-space: nowrap
#   .goat-lockup__tag   "iPhone app" / "On the App Store", white-space: nowrap
#   .gc-qr              the App Store QR tile. Must be its full 72 css px, inside the plate
SELECTORS = [".shell", ".body__pad", ".go-row",
             ".kofi", ".gh", ".goat--strip", ".goat__copy", ".goat-lockup",
             ".gh__inner", ".gc-photo", ".gc-vf", ".goat-lockup__word",
             ".goat-lockup__name", ".goat-lockup__tag", ".gc-qr"]

# The four row-level items, in layout order. The PLATE is the ruler: its natural height is the
# row's height, and nothing else may be taller. Ko-fi and Star must match each other.
ITEMS = [".kofi", ".gh", ".goat__copy", ".goat-lockup"]
RULER = ".goat-lockup"
BUTTONS = (".kofi", ".gh")

# The QR's rendered size in css px. [M] The master sheet's `var(--gc-qr, 72px)`; the generator
# refuses a sheet where it is anything else. A tile that renders smaller has been squeezed.
QR_CSS = 72.0

# THE THREE GROUPS, in layout order. `.go-row` is `justify-content: space-between`, so these are
# the three things it spreads - and the gaps BETWEEN them are what must be larger than the gap
# INSIDE group 3.
GROUPS = [".kofi", ".gh", ".goat--strip"]
# The pair inside group 3. The gap between these two is the one that has to be tighter.
PAIR = (".goat__copy", ".goat-lockup")

# The layout MUST contain these. A page without .goat--strip is a pre-change page - the
# GOATPROJECT row, or the plugin's vertical stack - and measuring it would hand back numbers for
# a panel the app no longer ships.
REQUIRED = [".shell", ".body__pad", ".go-row", ".kofi", ".gh", ".goat--strip",
            ".goat__copy", ".goat-lockup", ".gc-qr"]

# Overflow is checked on these. .shell is here because it is the element that actually carries
# `overflow: hidden` and therefore the one that does the clipping; .go-row because that is where
# a too-wide group shows up first; the banner and its copy because a mis-set width shows up there
# before anywhere else.
#
# .goat-lockup IS HERE as of banner v2 - its plate clips nothing, so nothing may overrun it.
# .gc-photo is not, on purpose - see the header.
OVERFLOW_CHECK = [".shell", ".body__pad", ".go-row", ".goat--strip", ".goat-lockup",
                  ".goat__copy", ".kofi", ".gh"]

# Elements whose computed padding AND border the probe reports, so a fit against a padding box or
# a content box can be checked. clientWidth/clientHeight are the PADDING box, so these have to
# come back separately or every fit check inside a padded element is wrong.
PAD_OF = [".goat-lockup", ".gh", ".body__pad"]
# Sub-pixel tolerance. Layout arithmetic here lands on thirds of a pixel (14.69, 47.375), and a
# difference under 1px cannot be seen and cannot clip a glyph.
TOL = 1.0

# THE GROUPING MARGIN. "Strictly smaller" as a bare inequality would be satisfied by 19.6 against
# 20.0, which nobody can see - and a grouping nobody can see is not a grouping. So the in-group
# gap has to be smaller than the smallest inter-group gap by at least this much, which is the
# same "under 1px is invisible" reasoning as TOL.
GROUP_MARGIN = 1.0

# ---------------------------------------------------------------------------------------------
# THE CONSTRAINT THE SETTINGS WINDOW IMPOSES - DERIVED FROM THE C++, NOT GUESSED
# ---------------------------------------------------------------------------------------------
# The same derivation tools\gen-sponsor-html.py carries at MIN_CLIENT_CSS_W, and the two must
# agree. It is repeated rather than imported because this script has to be runnable against a
# page the generator did not just write.
#
#   [M] src\settings.cpp   SettingsLayout, WebView2 path:  spW = cw - 2 * GAP; spLeft = GAP;
#                          and                 const int GAP = theme::Dp(theme::metric::kGap, dpi);
#   [M] src\theme.h:71     constexpr int kGap = 12;
#   [M] src\theme.cpp:329  int Dp(int logical, int dpi) { return MulDiv(logical, dpi, 96); }
#   [M] src\settings.cpp   WM_GETMINMAXINFO:  RECT need = { 0, 0, theme::Dp(880, dpi), needH };
#   [M] src\webview_host.cpp  WebSponsorMinSize:  s.cx = ::MulDiv(kSponsorCssMinWidth, dpi, 96);
#
# 880 - 2 * 12 = 856 css px available at the SMALLEST allowed window. 856 is not the number to
# write in a guard, though: the three MulDiv calls round independently, so the largest floor that
# is safe at EVERY integer dpi from 96 to 480 is computed below rather than asserted.
MIN_CLIENT_CSS_W = 880
GAP_CSS = 12
DPI_LO, DPI_HI = 96, 480


def muldiv(a, b, c):
    """Win32 MulDiv for the non-negative case: round to nearest, ties away from zero."""
    return (a * b + c // 2) // c


def worst_margin(css_w):
    """The smallest DEVICE-px margin the panel has at the minimum window, over every DPI."""
    return min(muldiv(MIN_CLIENT_CSS_W, d, 96)
               - 2 * muldiv(GAP_CSS, d, 96)
               - muldiv(css_w, d, 96)
               for d in range(DPI_LO, DPI_HI + 1))


MIN_AVAIL_CSS_W = MIN_CLIENT_CSS_W - 2 * GAP_CSS          # 856
SHELL_W_CEILING = MIN_AVAIL_CSS_W
while SHELL_W_CEILING > 200 and worst_margin(SHELL_W_CEILING) < 0:
    SHELL_W_CEILING -= 1

# The widths the DETAILED checks run at, in css px. Every one of them is a real window:
#
#   MIN_AVAIL_CSS_W  856   the content row at the SMALLEST window the app allows - Dp(880) less
#                          one kGap a side. This is the worst case and the one that matters.
#   880                    a little above the minimum
#   976                    THE DEFAULT WINDOW. settings.cpp opens at MulDiv(1000, dpi, 96) client
#                          px wide, so the content row is 1000 - 2 * 12 = 976.
#   1200, 1400, 1600       maximised and wide-monitor cases. 1400 is the spec's wide case.
#
# The floor the sweep finds is appended to this list, so the narrowest width that is supposed to
# work gets the full battery too and not just the cheap scan verdict.
DETAIL_WIDTHS = [MIN_AVAIL_CSS_W, 880, 976, 1200, 1400, 1600]

# The downward sweep's range. It walks from SCAN_HI to SCAN_LO looking for the highest width at
# which anything breaks; the floor is that width + 1. The range is deliberately wider than the
# answer on both sides: a floor at the very edge of the range would mean the range is the thing
# being measured.
SCAN_HI, SCAN_LO = 1000, 640


def find_browser():
    for b in BROWSERS:
        if os.path.exists(b):
            return b
    raise SystemExit("*** no headless browser found; tried:\n  " + "\n  ".join(BROWSERS))


page_path = os.path.join(S, "panel-app.html")
if not os.path.exists(page_path):
    raise SystemExit("*** %s is missing - run tools\\gen-sponsor-html.py first" % page_path)

page = io.open(page_path, encoding="utf-8").read()

# Whether this page is a --measure page is readable straight off it, and worth printing: a run
# that measured the max-content page and a run that swept the shipping page look identical in
# their numbers when everything is right, and completely different when it is not.
#
# MATCH THE WHOLE DECLARATION, not just `width:max-content`. Other rules in the same block carry
# width:max-content and always will - `.go-row>.kofi` and `.go-row>.gh` both do - so a substring
# test on that alone calls EVERY page a measure page. It did, and it printed that under a correct
# set of numbers, which is exactly the shape of wrong that survives a glance.
_shell_w = re.search(r"\.shell\.is-open\{width:([^}]*)\}", page)
if not _shell_w:
    raise SystemExit("*** this page has no `.shell.is-open{width:...}` rule at all, so there is "
                     "no way to tell what it is supposed to be. Regenerate it with "
                     "tools\\gen-sponsor-html.py.")
SHELL_DECL = _shell_w.group(1)
MEASURE_PAGE = (SHELL_DECL == "max-content")

# ---------------------------------------------------------------------------------------------
# THE PROBE
# ---------------------------------------------------------------------------------------------
# The app page, plus a measuring script appended AFTER the app's own script has attached the
# shadow root and injected the panel. It reads the real laid-out boxes and writes them, as JSON,
# where --dump-dom will show them.
#
# It measures SYNCHRONOUSLY. requestAnimationFrame was tried first and never fired: under
# --virtual-time-budget in the headless shell nothing paints, so a rAF chain never resolves and
# --dump-dom prints a page with no answer in it - which looks exactly like a page whose script
# threw. getBoundingClientRect forces layout on the spot and needs no frame.
#
# It then measures AGAIN once webfonts have settled and overwrites the first answer, because
# where the copy wraps - and therefore whether it still fits the Ko-fi height - depends on the
# resolved font. Both paths write the same element, so whichever ran last is what --dump-dom
# reports.
#
# SETTING THE WIDTH IS AN INLINE STYLE ON #h, the shadow host. The page's own rule is
# `#h{position:absolute;left:0;right:0;bottom:0}`; an inline `left:0;right:auto;width:Wpx` beats
# it without !important and without touching the page's stylesheet. That models the app exactly:
# the host WINDOW's width is the panel's width.
#
# THE NATURAL PASS PUTS ITSELF BACK. `align-self: flex-start` is injected into the shadow root as
# its own <style>, the four items are re-read, and the style is removed again - so the boxes
# reported alongside it are the real, laid-out ones and the naturals are a separate map. Doing
# it the other way round would report a panel nobody ships. It targets `.go-row>*` AND
# `.goat--strip>*`, because the statement and the plate are one level deeper than the buttons
# and a selector that misses them would report `null` and silently skip the check.
probe_js = (
    "var SEL=" + json.dumps(SELECTORS) + ";"
    "var ITEMS=" + json.dumps(ITEMS) + ";"
    "var PADOF=" + json.dumps(PAD_OF) + ";"
    "var GROUPS=" + json.dumps(GROUPS) + ";"
    "var PAIR=" + json.dumps(list(PAIR)) + ";"
    "var DETAIL=" + json.dumps(DETAIL_WIDTHS) + ";"
    "var SCAN_HI=" + str(SCAN_HI) + ",SCAN_LO=" + str(SCAN_LO) + ";"
    "var MEASURE_PAGE=" + ("true" if MEASURE_PAGE else "false") + ";"
    "var TOL=" + repr(TOL) + ",GM=" + repr(GROUP_MARGIN) + ";"
    "var RULER=" + json.dumps(RULER) + ",BTNS=" + json.dumps(list(BUTTONS)) + ";"
    "var host=document.getElementById('h');"
    "var sh=host.shadowRoot;"
    "function q(s){return sh.querySelector(s);}"
    "function setW(w){"
    "if(w===null){host.style.width='';host.style.right='';host.style.left='';}"
    "else{host.style.left='0';host.style.right='auto';host.style.width=w+'px';}"
    "host.getBoundingClientRect();"
    "}"
    # One width, everything: boxes, scroll metrics, paddings, natural heights, gaps.
    "function detail(w){"
    "setW(w);"
    "var o={w:w,box:{},flow:{},pad:{},natural:{},gaps:{}};"
    "SEL.forEach(function(s){"
    "var e=q(s);"
    "if(!e){o.box[s]=null;return;}"
    "var b=e.getBoundingClientRect();"
    "o.box[s]=[b.width,b.height,b.left,b.top,b.right,b.bottom];"
    "o.flow[s]=[e.scrollWidth,e.clientWidth,e.scrollHeight,e.clientHeight];"
    "});"
    "PADOF.forEach(function(s){"
    "var e=q(s);"
    "if(!e){o.pad[s]=null;return;}"
    "var c=getComputedStyle(e);"
    "o.pad[s]=[parseFloat(c.paddingTop)||0,parseFloat(c.paddingBottom)||0,"
    "parseFloat(c.paddingLeft)||0,parseFloat(c.paddingRight)||0,"
    "parseFloat(c.borderTopWidth)||0,parseFloat(c.borderBottomWidth)||0,"
    "parseFloat(c.borderLeftWidth)||0,parseFloat(c.borderRightWidth)||0];"
    "});"
    # The gaps, measured off the rendered boxes rather than read out of the CSS. A `gap`
    # declaration is what was asked for; the distance between two boxes is what happened.
    "var g=[];for(var i=0;i<GROUPS.length;i++){var e=q(GROUPS[i]);g.push(e?e.getBoundingClientRect():null);}"
    "o.gaps.inter=[];"
    "for(var i=1;i<g.length;i++){o.gaps.inter.push((g[i]&&g[i-1])?(g[i].left-g[i-1].right):null);}"
    "var pa=q(PAIR[0]),pb=q(PAIR[1]);"
    "o.gaps.tight=(pa&&pb)?(pb.getBoundingClientRect().left-pa.getBoundingClientRect().right):null;"
    "var row=q('.go-row'),padb=q('.body__pad');"
    "o.gaps.outer_left=(row&&g[0])?(g[0].left-row.getBoundingClientRect().left):null;"
    "o.gaps.outer_right=(row&&g[g.length-1])?(row.getBoundingClientRect().right-g[g.length-1].right):null;"
    "if(row&&padb){var rb=row.getBoundingClientRect(),pb2=padb.getBoundingClientRect();"
    "o.gaps.top=rb.top-pb2.top;o.gaps.bottom=pb2.bottom-rb.bottom;}"
    "else{o.gaps.top=null;o.gaps.bottom=null;}"
    # Natural (unstretched) heights, then put it back.
    "var st=document.createElement('style');"
    "st.textContent='.go-row>*,.goat--strip>*{align-self:flex-start!important}';"
    "sh.appendChild(st);"
    "ITEMS.forEach(function(s){var e=q(s);o.natural[s]=e?e.getBoundingClientRect().height:null;});"
    "st.remove();"
    "return o;"
    "}"
    # The cheap per-width verdict the downward sweep uses. Same questions, no bookkeeping.
    "function broke(w){"
    "setW(w);"
    "var sl=q('.shell'),row=q('.go-row');"
    "if(!sl||!row)return 'missing';"
    "if(sl.scrollWidth>sl.clientWidth)return 'shell-overflow';"
    "if(row.scrollWidth>row.clientWidth)return 'row-overflow';"
    "var rb=row.getBoundingClientRect();"
    "var g=[];for(var i=0;i<GROUPS.length;i++){var e=q(GROUPS[i]);if(!e)return 'missing';g.push(e.getBoundingClientRect());}"
    # SUB-PIXEL, NOT TOL. scrollWidth is an INTEGER, so a row that needs 847.41px reports no
    # overflow at 847 and the sweep would call 847 the floor while the --measure route says 848.
    # [M] 2026-10-25: exactly that - 0.41px past the row at 847. The floor is a fit, not a
    # visibility judgement, so the last group may not pass the row's edge at all.
    "if(g[g.length-1].right>rb.right+0.01)return 'group-past-row';"
    "for(var i=1;i<g.length;i++){if(g[i].left<g[i-1].right-TOL)return 'groups-overlap';}"
    "var inter=[];for(var i=1;i<g.length;i++)inter.push(g[i].left-g[i-1].right);"
    "var pa=q(PAIR[0]),pb=q(PAIR[1]);"
    "if(!pa||!pb)return 'missing';"
    "var tight=pb.getBoundingClientRect().left-pa.getBoundingClientRect().right;"
    "var mn=Math.min.apply(null,inter);"
    "if(!(tight<mn-GM))return 'grouping-lost';"
    # The height contract, cheaply: Ko-fi and Star one height, nothing taller than the plate,
    # everything centred on the row.
    "var b0=q(BTNS[0]),b1=q(BTNS[1]),pl=q(RULER);"
    "if(!b0||!b1||!pl)return 'missing';"
    "if(Math.abs(b0.getBoundingClientRect().height-b1.getBoundingClientRect().height)>TOL)return 'buttons-differ';"
    "var ph=pl.getBoundingClientRect().height,rc=(rb.top+rb.bottom)/2;"
    "for(var i=0;i<ITEMS.length;i++){var e=q(ITEMS[i]);if(!e)return 'missing';"
    "var bb=e.getBoundingClientRect();"
    "if(bb.height>ph+TOL)return 'taller-than-plate';"
    "if(Math.abs((bb.top+bb.bottom)/2-rc)>TOL)return 'not-centred';}"
    "return null;"
    "}"
    "function M(tag){"
    "var o={tag:tag,widths:[],scan:null,shell_decl:" + json.dumps(SHELL_DECL) + "};"
    "if(MEASURE_PAGE){"
    # A max-content page ignores the host width by construction, so sweeping it would report the
    # same box N times and call that a verified responsive layout. One wide host, one reading.
    "o.widths.push(detail(1800));"
    "}else{"
    "var hi=null,firstBreak=null,nonmono=[];"
    "for(var w=SCAN_HI;w>=SCAN_LO;w--){"
    "var r=broke(w);"
    "if(r&&firstBreak===null){firstBreak=w;o.scan={first_break:w,reason:r};}"
    "else if(!r&&firstBreak!==null){nonmono.push(w);}"
    "}"
    "if(o.scan===null)o.scan={first_break:null,reason:null};"
    "o.scan.nonmonotonic=nonmono.slice(0,12);"
    "o.scan.nonmonotonic_count=nonmono.length;"
    "o.scan.hi=SCAN_HI;o.scan.lo=SCAN_LO;"
    "var list=DETAIL.slice();"
    "if(firstBreak!==null){list.push(firstBreak+1);list.push(firstBreak);}"
    "list.sort(function(a,b){return a-b;});"
    "for(var i=0;i<list.length;i++){if(i===0||list[i]!==list[i-1])o.widths.push(detail(list[i]));}"
    "}"
    "setW(null);"
    "var d=document.getElementById('measured');"
    "if(!d){d=document.createElement('div');d.id='measured';document.body.appendChild(d);}"
    "d.textContent=JSON.stringify(o);"
    "}"
    "M('sync');"
    "if(document.fonts&&document.fonts.ready){document.fonts.ready.then(function(){M('fonts');});}"
    "setTimeout(function(){M('settled');},1500);"
)
probe = page + "<script>" + probe_js + "</script>"

tmp = os.path.join(tempfile.gettempdir(), "panel-measure-probe.html")
io.open(tmp, "w", encoding="utf-8").write(probe)

browser = find_browser()
print("browser: %s" % browser)
print("page   : %s (%d bytes)" % (page_path, len(page)))
if MEASURE_PAGE:
    print("shell  : max-content  [--measure page: this reports what the panel WANTS, i.e. its")
    print("         intrinsic MINIMUM. The sweep is skipped - a max-content page ignores the")
    print("         host width, so sweeping it would report one box six times.]")
else:
    print("shell  : %s  [shipping page: this SWEEPS the host width and asserts at every one]"
          % SHELL_DECL)
print("budget : min client %d - 2 * %d = %d css px available at the smallest window; the largest"
      % (MIN_CLIENT_CSS_W, GAP_CSS, MIN_AVAIL_CSS_W))
print("         floor that is safe at every integer dpi %d..%d is %d" % (DPI_LO, DPI_HI,
                                                                        SHELL_W_CEILING))
cmd = [browser, "--headless", "--disable-gpu", "--no-sandbox",
       "--window-size=1900,1200", "--virtual-time-budget=8000",
       "--dump-dom", "file:///" + tmp.replace("\\", "/")]
res = subprocess.run(cmd, capture_output=True, text=True, timeout=300)
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
when = data["tag"]
runs = data["widths"]
scan = data.get("scan")
print("reported at: %s   (%d width(s) measured)" % (when, len(runs)))
print("")

failures = []
notes = []


def fail(msg):
    failures.append(msg)


# ---------------------------------------------------------------------------------------------
# THE SWEEP - where it first breaks, and therefore what the floor is
# ---------------------------------------------------------------------------------------------
FLOOR = None
if not MEASURE_PAGE:
    if scan is None:
        fail("FAILURE: the sweep did not run at all, so the floor is UNVERIFIED.")
    elif scan.get("first_break") is None:
        fail("FAILURE: nothing broke anywhere between %dpx and %dpx, so the floor is BELOW the "
             "swept range and this script has not measured it. Lower SCAN_LO and re-run - a "
             "floor outside the range means the range is what is being measured, not the panel."
             % (scan["lo"], scan["hi"]))
    else:
        FLOOR = scan["first_break"] + 1
        print("SWEEP  %dpx down to %dpx, 1px steps" % (scan["hi"], scan["lo"]))
        print("  first break at %dpx  (%s)" % (scan["first_break"], scan["reason"]))
        print("  -> the floor is %dpx. This is the number for SHELL_MIN_W / kSponsorCssMinWidth."
              % FLOOR)
        if scan.get("nonmonotonic_count"):
            fail("FAILURE: %d width(s) BELOW the first break came back clean (%s...). Narrower is "
                 "supposed to be strictly worse, so either the break is not the floor or the "
                 "check is not measuring what it claims to."
                 % (scan["nonmonotonic_count"],
                    ", ".join(str(x) for x in scan["nonmonotonic"])))
        else:
            print("  every width below it also breaks - the boundary is a boundary, not a dip.")
        if FLOOR > SHELL_W_CEILING:
            fail("FAILURE: the floor is %dpx but the smallest allowed window only offers %dpx "
                 "(the largest always-safe value at every dpi %d..%d). At the minimum window "
                 "size the three groups DO NOT FIT and `.shell{overflow:hidden}` cuts the "
                 "right-hand group off in silence. DO NOT SHIP THIS: either raise the window "
                 "minimum in settings.cpp's WM_GETMINMAXINFO (and MIN_CLIENT_CSS_W in BOTH this "
                 "script and tools\\gen-sponsor-html.py, together), or make the row narrower."
                 % (FLOOR, SHELL_W_CEILING, DPI_LO, DPI_HI))
        else:
            print("  budget: %dpx floor against a %dpx ceiling - %dpx of css slack, worst-case "
                  "%d device px over every dpi %d..%d."
                  % (FLOOR, SHELL_W_CEILING, SHELL_W_CEILING - FLOOR, worst_margin(FLOOR),
                     DPI_LO, DPI_HI))
        print("")

# ---------------------------------------------------------------------------------------------
# THE PER-WIDTH TABLE, AND THE FOUR ASSERTIONS AT EVERY ONE OF THEM
# ---------------------------------------------------------------------------------------------
print("PER-WIDTH CHECKS - every column is asserted, not just printed")
print("  %6s %9s %8s %8s %8s %8s %8s %7s %7s %7s %7s"
      % ("host", "shell", "gap1", "gap2", "tight", "outerL", "outerR", "top", "bottom", "plateH",
         "btnsH"))
print("  %6s %9s %8s %8s %8s %8s %8s %7s %7s %7s %7s"
      % ("-" * 6, "-" * 9, "-" * 8, "-" * 8, "-" * 8, "-" * 8, "-" * 8, "-" * 7, "-" * 7, "-" * 7,
         "-" * 7))

shell_h_seen = set()
for run in runs:
    w = run["w"]
    box, flow, pad = run["box"], run["flow"], run["pad"]
    gaps, natural = run["gaps"], run["natural"]
    tag = "w=%d" % w

    missing = [s for s in REQUIRED if box.get(s) is None]
    if missing:
        fail("FAILURE (%s): %s not in this page. That means panel-app.html predates the "
             "three-group layout - regenerate it with tools\\gen-sponsor-html.py."
             % (tag, ", ".join(missing)))
        continue

    inter = [g for g in gaps["inter"] if g is not None]
    tight = gaps["tight"]
    heights = [box[s][1] for s in ITEMS if box.get(s)]

    print("  %6d %9.2f %8.2f %8.2f %8.2f %8.2f %8.2f %7.2f %7.2f %7.2f %7.2f"
          % (w, box[".shell"][0], inter[0] if inter else -1,
             inter[1] if len(inter) > 1 else -1,
             -1 if tight is None else tight,
             gaps["outer_left"] if gaps["outer_left"] is not None else -1,
             gaps["outer_right"] if gaps["outer_right"] is not None else -1,
             gaps["top"] if gaps["top"] is not None else -1,
             gaps["bottom"] if gaps["bottom"] is not None else -1,
             box[RULER][1], box[BUTTONS[0]][1]))

    is_floor_probe = (FLOOR is not None and w == FLOOR - 1)
    if is_floor_probe:
        # This row is the KNOWN-BAD one, printed on purpose so the boundary is visible in the
        # table rather than asserted in prose. It is expected to violate, so it is not asserted.
        notes.append("w=%d is the first BREAKING width and is in the table as evidence of the "
                     "boundary; its violations are expected and are not counted." % w)
        continue

    # --- REQUEST 1: the panel fills the host it is given -------------------------------------
    if not MEASURE_PAGE and abs(box[".shell"][0] - w) > TOL:
        fail("FAILURE (%s): the panel is %.2fpx wide inside a %dpx host - it is NOT filling the "
             "window. `.shell.is-open` should be width:100%%; this page says `%s`."
             % (tag, box[".shell"][0], w, SHELL_DECL))

    # --- REQUEST 2a: the three groups do not collide and nothing overflows --------------------
    for s in OVERFLOW_CHECK:
        f = flow.get(s)
        if not f:
            continue
        sw, cw_, sh_, ch_ = f
        if sw > cw_:
            fail("FAILURE (%s): %s OVERFLOWS HORIZONTALLY - scrollWidth %d > clientWidth %d "
                 "(%dpx of content is being clipped, i.e. a button is cut off)."
                 % (tag, s, sw, cw_, sw - cw_))
        if sh_ > ch_:
            fail("FAILURE (%s): %s OVERFLOWS VERTICALLY - scrollHeight %d > clientHeight %d "
                 "(%dpx clipped)." % (tag, s, sh_, ch_, sh_ - ch_))
    for a, b in zip(GROUPS, GROUPS[1:]):
        if box[b][2] < box[a][4] - TOL:
            fail("FAILURE (%s): %s starts at %.2f, inside %s which ends at %.2f - the groups "
                 "OVERLAP by %.2fpx." % (tag, b, box[b][2], a, box[a][4], box[a][4] - box[b][2]))

    # --- REQUEST 2b: the outer edges sit on the panel's padding -------------------------------
    for name, v in (("left", gaps["outer_left"]), ("right", gaps["outer_right"])):
        if v is None or abs(v) > TOL:
            fail("FAILURE (%s): the %s outer edge of the row is %.2fpx from the panel's padding, "
                 "not 0. space-between is supposed to put the first and last group flush against "
                 "the content box." % (tag, name, -1 if v is None else v))

    # --- REQUEST 2c: THE GROUPING. the pair's gap must be tighter than the gaps around it ------
    if tight is None or len(inter) < 2:
        fail("FAILURE (%s): the gaps did not come back, so whether the statement and the plate "
             "read as one group is UNVERIFIED." % tag)
    elif not tight < min(inter) - GROUP_MARGIN:
        fail("FAILURE (%s): the statement<->plate gap is %.2fpx against inter-group gaps of "
             "%.2f and %.2f. It has to be STRICTLY SMALLER (by at least %.1fpx, or nobody can "
             "see it) or the two do not read as one group - which is the whole of the operator's "
             "second request." % (tag, tight, inter[0], inter[1], GROUP_MARGIN))

    # --- REQUEST 4: the gap above the row equals the gap below it -----------------------------
    if gaps["top"] is None or gaps["bottom"] is None:
        fail("FAILURE (%s): the top/bottom gaps did not come back, so whether the row is centred "
             "in the panel is UNVERIFIED." % tag)
    elif abs(gaps["top"] - gaps["bottom"]) > TOL:
        fail("FAILURE (%s): the gap ABOVE the row is %.2fpx and the gap BELOW it is %.2fpx - off "
             "by %.2f. The operator's fourth request is that they match, and the cause was the "
             "plugin's `.body__pad { padding: 2px 13px 13px }`. Check the `.body__pad` override "
             "in tools\\gen-sponsor-html.py's GO_LAYOUT_CSS."
             % (tag, gaps["top"], gaps["bottom"], abs(gaps["top"] - gaps["bottom"])))

    # --- THE HEIGHT CONTRACT: the plate is the ruler; Ko-fi and Star match; all centred -------
    nat = dict((s, natural[s]) for s in ITEMS if natural.get(s) is not None)
    if len(heights) != len(ITEMS) or len(nat) != len(ITEMS):
        fail("FAILURE (%s): only %d of the %d row items (and %d natural heights) could be "
             "measured, so the height contract is UNVERIFIED."
             % (tag, len(heights), len(ITEMS), len(nat)))
    else:
        tallest = max(nat, key=lambda k: nat[k])
        if nat[tallest] > nat[RULER] + TOL:
            fail("FAILURE (%s): %s is the TALLEST item at its natural height %.3fpx, over the "
                 "plate's %.3fpx. The row grows past the band and the host cuts the panel's top "
                 "off. If it is .goat__copy, the headline has wrapped to more lines than the "
                 "band holds - its column is too narrow for these fonts."
                 % (tag, tallest, nat[tallest], nat[RULER]))
        if abs(box[".go-row"][1] - box[RULER][1]) > TOL:
            fail("FAILURE (%s): the row is %.3fpx tall but the plate is %.3fpx - the row is keyed "
                 "to something other than the plate." % (tag, box[".go-row"][1], box[RULER][1]))
        b0, b1 = BUTTONS
        if abs(box[b0][1] - box[b1][1]) > TOL:
            fail("FAILURE (%s): %s is %.3fpx tall and %s is %.3fpx - the two buttons are not one "
                 "height." % (tag, b0, box[b0][1], b1, box[b1][1]))
        rc = (box[".go-row"][3] + box[".go-row"][5]) / 2.0
        for s in ITEMS:
            c_ = (box[s][3] + box[s][5]) / 2.0
            if abs(c_ - rc) > TOL:
                fail("FAILURE (%s): %s is centred at %.2f against the row's %.2f - off by %.2fpx."
                     % (tag, s, c_, rc, c_ - rc))

    # --- WHAT A SCROLL METRIC CANNOT SAY ------------------------------------------------------
    # Direct box comparisons, because a scroll metric can miss an absolute child and never sees an
    # overlap. (In v1 the plate was `overflow: clip`,
    # which does not create a scroll container, so content cut off inside it reports nothing at
    # all. These are direct box-against-box comparisons against the plate's PADDING box - the
    # clip edge - instead.
    lp = pad.get(".goat-lockup") or [0.0] * 8
    pl_ = box.get(".goat-lockup")

    def inside_plate(what, s, _tag=tag, _box=box, _pl=pl_, _lp=lp):
        bi = _box.get(s)
        if bi is None or _pl is None:
            fail("FAILURE (%s): %s - %s or the plate is missing, so the fit is UNVERIFIED."
                 % (_tag, what, s))
            return
        # padding box = border box less the border, in viewport coordinates
        L, T = _pl[2] + _lp[6], _pl[3] + _lp[4]
        R, B = _pl[4] - _lp[7], _pl[5] - _lp[5]
        over = max(L - bi[2], T - bi[3], bi[4] - R, bi[5] - B)
        if over > TOL:
            fail("FAILURE (%s): %s - %s reaches %.3fpx past the plate's clip edge (box %.2f,%.2f "
                 "- %.2f,%.2f against %.2f,%.2f - %.2f,%.2f), over the plate's border or past "
                 "it, where no scroll metric reliably shows it."
                 % (_tag, what, s, over, bi[2], bi[3], bi[4], bi[5], L, T, R, B))

    def fits(what, inner, outer, axis, _tag=tag, _box=box):
        bi, bo = _box.get(inner), _box.get(outer)
        if bi is None or bo is None:
            fail("FAILURE (%s): %s - %s or %s is missing, so the fit is UNVERIFIED."
                 % (_tag, what, inner, outer))
            return
        i_ = bi[0] if axis == "w" else bi[1]
        o_ = bo[0] if axis == "w" else bo[1]
        if i_ > o_ + TOL:
            fail("FAILURE (%s): %s - %s is %.3fpx against %.3fpx of room inside %s, so %.3fpx is "
                 "being cut off with no scroll metric to show it."
                 % (_tag, what, inner, i_, o_, outer, i_ - o_))

    inside_plate("the App Store QR is inside the plate", ".gc-qr")
    inside_plate("the photo layer is inside the plate", ".gc-photo")
    inside_plate("the viewfinder is inside the plate", ".gc-vf")
    inside_plate("the wordmark is inside the plate", ".goat-lockup__word")
    # Nothing may sit ON the QR: a bracket or a glyph over a module is a code that may not scan.
    _qb = box.get(".gc-qr")
    for _s in (".goat-lockup__word", ".gc-vf"):
        _b = box.get(_s)
        if _b is None or _qb is None:
            fail("FAILURE (%s): %s or the QR is missing, so whether they overlap is UNVERIFIED."
                 % (tag, _s))
        elif _b[4] > _qb[2] - 1.0 and _b[2] < _qb[4] and _b[5] > _qb[3] and _b[3] < _qb[5]:
            fail("FAILURE (%s): %s (right edge %.2f) runs into the QR (left edge %.2f) - it must "
                 "stay at least 1px clear." % (tag, _s, _b[4], _qb[2]))
    fits("'GOAT Cam' fits the wordmark", ".goat-lockup__name", ".goat-lockup__word", "w")
    fits("the nowrap tagline fits the wordmark", ".goat-lockup__tag", ".goat-lockup__word", "w")
    qb = box.get(".gc-qr")
    if qb is None:
        fail("FAILURE (%s): the QR tile is missing." % tag)
    elif abs(qb[0] - QR_CSS) > 0.5 or abs(qb[1] - QR_CSS) > 0.5:
        fail("FAILURE (%s): the App Store QR renders %.2f x %.2f css px, not %g x %g - a squeezed "
             "code is a code that may not scan." % (tag, qb[0], qb[1], QR_CSS, QR_CSS))

    # Not a fit but a FILL, and the opposite comparison. .gh is display:block in the plugin with
    # a 1px frame and one in-flow child that paints the dark pill; given the Ko-fi height as a
    # block, the frame grows and the pill does not, and a band of bare #262626 opens under the
    # label. GO_LAYOUT_CSS makes .gh a flex container so the pill stretches with it. This is the
    # check that says it worked - it is invisible to every overflow metric, because nothing
    # overflows: the button is simply the right size and half empty.
    gp = pad.get(".gh") or [0.0] * 8
    bg, bi_ = box.get(".gh"), box.get(".gh__inner")
    if bg is None or bi_ is None:
        fail("FAILURE (%s): .gh or .gh__inner is missing, so whether the pill fills the "
             "button is UNVERIFIED." % tag)
    else:
        # Compared on rendered boxes only, because .gh is ZOOMED and computed padding comes back
        # in its own unzoomed px. The plugin's frame is `padding: 1px` (gp[0] / gp[1]), so the
        # pill may sit at most that much inside a side at any zoom <= 1.
        frame = gp[0] + gp[1] + gp[4] + gp[5]
        if bg[1] - bi_[1] > frame + TOL:
            fail("FAILURE (%s): .gh__inner is %.3fpx inside a %.3fpx .gh, so %.3fpx of bare "
                 "#262626 frame is showing under the label (the frame itself is %.2fpx). .gh needs "
                 "to be a flex container for its pill to stretch with it."
                 % (tag, bi_[1], bg[1], bg[1] - bi_[1] - frame, frame))

    shell_h_seen.add(round(box[".shell"][1], 3))

# ---------------------------------------------------------------------------------------------
# THE HEIGHT IS ONE NUMBER, OR IT IS NOT A CONSTANT
# ---------------------------------------------------------------------------------------------
# kSponsorCssHeight is a single compile-time value, so the panel's height must not depend on how
# wide the window is. It does not, because the plate is a fixed height, the copy column a fixed
# width and the row never wraps - but "it does not" is an assumption until the sweep has measured
# it at both ends.
# WHAT THE FLOOR IS MADE OF. The sweep says where it breaks; this says WHY, which is the only
# thing that tells a reader which item to shrink when it breaks too high. Printed once, at the
# narrowest width measured, because the item widths are pinned and do not move with the window.
if runs:
    _first = runs[0]
    print("")
    print("ROW COMPOSITION at w=%d - what the floor is made of" % _first["w"])
    for s in (".kofi", ".gh", ".goat--strip", ".goat__copy", ".goat-lockup", ".gc-photo",
              ".gc-vf", ".goat-lockup__word", ".gc-qr", ".go-row",
              ".body__pad", ".shell"):
        _b = _first["box"].get(s)
        print("  %-20s %8s" % (s, "absent" if _b is None else "%.2f" % _b[0]))

# The heights themselves, once, so the assertions above have a printed witness: which item is
# which height as laid out, and what each one's natural height is.
if runs:
    _last = runs[-1]
    print("")
    print("ROW HEIGHT at w=%d - the plate is the ruler; Ko-fi and Star match; all centred"
          % _last["w"])
    print("  %-20s %-14s %-14s %-14s" % ("item", "as laid out", "natural", "centre - row"))
    print("  %-20s %-14s %-14s %-14s" % ("-" * 20, "-" * 14, "-" * 14, "-" * 14))
    _rb = _last["box"].get(".go-row")
    _rc = (_rb[3] + _rb[5]) / 2.0 if _rb else 0.0
    for s in ITEMS:
        _b, _n = _last["box"].get(s), _last["natural"].get(s)
        print("  %-20s %-14s %-14s %-14s"
              % (s, "absent" if _b is None else "%.3f" % _b[1],
                 "absent" if _n is None else "%.3f" % _n,
                 "absent" if _b is None else "%+.3f" % ((_b[3] + _b[5]) / 2.0 - _rc)))
    _q = _last["box"].get(".gc-qr")
    if _q:
        print("  .gc-qr               %.3f x %.3f   (must be %g x %g)" % (_q[0], _q[1], QR_CSS,
                                                                        QR_CSS))

print("")
if len(shell_h_seen) == 1:
    print("HEIGHT: %.3fpx at every width measured - kSponsorCssHeight is a constant, as the C++ "
          "assumes." % list(shell_h_seen)[0])
elif len(shell_h_seen) > 1:
    fail("FAILURE: the panel is %s px tall at different widths. kSponsorCssHeight is ONE "
         "compile-time constant, so a height that moves with the window means the host reserves "
         "the wrong rectangle at every size but one."
         % " / ".join("%.3f" % h for h in sorted(shell_h_seen)))

# ---------------------------------------------------------------------------------------------
# THE PLATE IS IN THE OVERFLOW VERDICT NOW, SAID OUT LOUD
# ---------------------------------------------------------------------------------------------
if runs:
    lf = runs[-1]["flow"].get(".goat-lockup")
    if lf:
        print("")
        print("no overflow on any of: %s" % ", ".join(OVERFLOW_CHECK))
        print("  the plate reads scrollWidth %d / clientWidth %d, scrollHeight %d / clientHeight %d:"
              % (lf[0], lf[1], lf[2], lf[3]))
        print("  banner v2 clips its photo in .gc-photo, so the plate itself has nothing to hide.")
        print("  .gc-photo alone is left out: its frame is a cover crop, larger than the layer on")
        print("  purpose. The QR, the viewfinder and the wordmark are checked directly, above.")

# ---------------------------------------------------------------------------------------------
# WHAT TO WRITE DOWN, AND WHETHER THE SHIPPED HEADER ALREADY SAYS IT
# ---------------------------------------------------------------------------------------------
print("")
if MEASURE_PAGE:
    b = runs[0]["box"][".shell"]
    w, h = int(math.ceil(b[0])), int(math.ceil(b[1]))
    print("MEASURED .shell = %d x %d css px, intrinsic  (reported at: %s)" % (w, h, when))
    print("")
    print("Write these into tools\\gen-sponsor-html.py:")
    print("    SHELL_MIN_W = %d" % w)
    print("    SHELL_H = %d" % h)
    print("then re-run WITHOUT --measure and run this script again - the sweep has to find the")
    print("same floor from the shipping page.")
    if w > SHELL_W_CEILING:
        fail("FAILURE: the intrinsic minimum is %dpx, past the %dpx the smallest allowed window "
             "can offer. At the minimum window size the row does not fit and the right-hand "
             "group is cut off silently. DO NOT SHIP THIS." % (w, SHELL_W_CEILING))
else:
    hh = sorted(shell_h_seen)
    h = int(math.ceil(hh[0])) if hh else 0
    print("MEASURED  floor = %s css px, height = %d css px  (reported at: %s)"
          % ("%d" % FLOOR if FLOOR else "UNKNOWN", h, when))
    print("")
    print("These are the two the generator must carry:")
    print("    SHELL_MIN_W = %s" % ("%d" % FLOOR if FLOOR else "UNKNOWN"))
    print("    SHELL_H = %d" % h)

    # And the closing check: the header the app actually compiles has to agree with what was
    # just measured. Without this the two-pass loop can be run, the numbers can be read, and the
    # header can quietly still carry the previous round's - which builds, runs, and reserves the
    # wrong rectangle.
    hdr_path = os.path.join(REPO, "src", "sponsor_html.h")
    try:
        hdr = io.open(hdr_path, encoding="utf-8").read()
    except OSError as exc:
        fail("FAILURE: src\\sponsor_html.h could not be read (%s), so whether the SHIPPED header "
             "agrees with these measurements is UNVERIFIED." % exc)
        hdr = None
    if hdr is not None:
        got = {}
        for name in ("kSponsorCssMinWidth", "kSponsorCssHeight"):
            mm = re.search(r"constexpr\s+int\s+%s\s*=\s*(\d+)\s*;" % name, hdr)
            got[name] = int(mm.group(1)) if mm else None
        print("")
        print("SHIPPED HEADER  src\\sponsor_html.h: kSponsorCssMinWidth = %s, kSponsorCssHeight "
              "= %s" % (got["kSponsorCssMinWidth"], got["kSponsorCssHeight"]))
        if re.search(r"constexpr\s+int\s+kSponsorCssWidth\b", hdr):
            fail("FAILURE: the shipped header still declares kSponsorCssWidth. That constant "
                 "described a FIXED-WIDTH panel and this one fills the window; a header carrying "
                 "it has not been regenerated. Run python tools\\gen-sponsor-html.py.")
        for name, want in (("kSponsorCssMinWidth", FLOOR), ("kSponsorCssHeight", h)):
            if got[name] is None:
                fail("FAILURE: src\\sponsor_html.h does not declare %s at all, so the C++ cannot "
                     "be reserving the right room. Run python tools\\gen-sponsor-html.py." % name)
            elif want is not None and got[name] != want:
                fail("FAILURE: src\\sponsor_html.h carries %s = %d but this page MEASURES %d. The "
                     "header the app compiles does not describe the page the app ships. Write the "
                     "measured numbers into tools\\gen-sponsor-html.py and re-run it."
                     % (name, got[name], want))
        if not failures:
            print("  -> the shipped header agrees with the rendered page.")

for n in notes:
    print("")
    print("NOTE: %s" % n)

if failures:
    print("")
    print("=" * 92)
    for line in failures:
        print(line)
    print("=" * 92)
    print("%d FAILURE(S). The panel does NOT behave; the numbers above are not safe to ship."
          % len(failures))
    sys.exit(1)

print("")
print("ALL CHECKS PASSED at every width measured: the panel fills its host, the three groups")
print("never collide or overflow, the statement<->plate gap is strictly tighter than the two")
print("inter-group gaps, the gap above the row equals the gap below it, the GOAT Cam plate is")
print("the tallest item and sets the row, Ko-fi and Star are one height, every item is centred,")
print("the App Store QR is its full %g css px inside the plate with nothing over it, nothing" % QR_CSS)
print("runs off the plate, and the floor is inside the %dpx the smallest allowed window can offer."
      % SHELL_W_CEILING)
