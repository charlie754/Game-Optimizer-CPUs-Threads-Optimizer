// Game Optimizer - the owner-drawn core map control.
//
// This control is the whole mitigation for "the app guessed the wrong CCD": if the user can
// SEE which logical processors belong to which cache domain, which two cells are the same
// physical core, and which cores Windows has parked, then a wrong guess is a thirty-second
// fix instead of a support ticket. Legibility beats polish everywhere in this file.
//
// Layout, top to bottom:
//   [processor group header]         only above 64 logical processors
//     [panel header]                 "Cache group - 16 logical processors - 96 MB L3"
//       [cell][cell]  [cell][cell]   one cell per LP, SMT siblings framed together
//     ...
//   [legend]                         the four visual states, spelled out
//   -----------------------------    fixed strip, never scrolls
//   [readout]                        "LP 3 - core 2 - L3 96 MB - parked: no"
//
// Every state is distinguishable WITHOUT colour: not-selected is an outline on a raised
// surface, selected is a fill plus a doubled ring, parked adds a diagonal hatch,
// excluded/absent is dimmed AND struck through. A colour-blind user loses nothing.
//
// Colours, radii and fonts all come from theme.h; nothing here reaches for a system colour,
// because the surrounding dashboard is dark and COLOR_WINDOW is not.
#include "ui.h"
#include "theme.h"
#include "topology.h"
#include "util.h"

#include <windowsx.h>

#include <algorithm>
#include <cstdio>
#include <map>
#include <new>
#include <set>
#include <string>
#include <vector>

namespace cd {

const wchar_t* kCoreMapClass = L"GameOptimizerCoreMap";

namespace {

// windows.h leaves the min/max macros in place here, so std::min/std::max are unusable.
inline int IMin(int a, int b) { return a < b ? a : b; }
inline int IMax(int a, int b) { return a > b ? a : b; }

constexpr UINT_PTR kTimerParked = 1;      // ~2 s parked-flag refresh while visible
constexpr UINT     kParkedPeriodMs = 2000;

// ---------------------------------------------------------------------------
// Per-window state
// ---------------------------------------------------------------------------

struct MapCell {
    ULONG lp = 0;             // LogicalProcessorIndex; meaningless when !present
    ULONG coreIndex = 0;
    bool  present = false;    // false => a placeholder for a core with fewer SMT siblings
    int   panel = -1;
    int   row = 0;            // global reading-order row, for arrow navigation
    RECT  r{};
};

struct MapPanel {
    std::wstring header;
    RECT headerRect{};
    bool selectable = true;                  // false for the "Unassigned" catch-all panel
    std::vector<std::vector<int>> units;     // one inner vector per physical core
};

struct GroupHeader {
    std::wstring text;
    RECT r{};
};

struct State {
    const Topology* topo = nullptr;          // borrowed; the caller guarantees the lifetime

    std::map<ULONG, bool> parked;            // LogicalProcessorIndex -> live Parked flag
    std::set<ULONG>       sel;               // selected LogicalProcessorIndex values
    bool editable = false;

    int   dpi = 96;
    // No HFONTs are owned here: every face comes from theme::GetFont, which caches one
    // handle per (Font, dpi) for the whole process. Owning a second copy per control is how
    // a long-running tray app leaks its GDI handle quota.

    std::vector<MapCell>     cells;          // reading order
    std::vector<MapPanel>    panels;
    std::vector<GroupHeader> groups;
    RECT  legendRect{};
    int   contentH = 0;
    int   rowCount = 0;

    int  scrollY = 0;
    bool scrollShown = false;
    bool laying = false;

    int  focus = -1;
    int  hover = -1;
    bool tracking = false;
    bool timerOn = false;

    int S(int v) const { return MulDiv(v, dpi, 96); }
};

State* GetState(HWND h) {
    if (!h) return nullptr;
    return reinterpret_cast<State*>(GetWindowLongPtrW(h, GWLP_USERDATA));
}

// ---------------------------------------------------------------------------
// DPI
// ---------------------------------------------------------------------------

UINT QueryDpi(HWND h) {
    typedef UINT(WINAPI * PfnGetDpiForWindow)(HWND);
    static PfnGetDpiForWindow pfn = nullptr;
    static bool tried = false;
    if (!tried) {
        tried = true;
        HMODULE mod = GetModuleHandleW(L"user32.dll");
        if (mod) {
            pfn = reinterpret_cast<PfnGetDpiForWindow>(
                      reinterpret_cast<void*>(GetProcAddress(mod, "GetDpiForWindow")));
        }
    }
    if (pfn) {
        UINT d = pfn(h);
        if (d >= 48) return d;
    }
    UINT out = 96;
    HDC dc = GetDC(h);
    if (dc) {
        int lx = GetDeviceCaps(dc, LOGPIXELSX);
        if (lx > 0) out = static_cast<UINT>(lx);
        ReleaseDC(h, dc);
    }
    return out;
}

// ---------------------------------------------------------------------------
// Text helpers
// ---------------------------------------------------------------------------

std::wstring FormatL3(ULONGLONG bytes) {
    if (bytes == 0) return std::wstring();
    ULONGLONG kb = bytes / 1024ULL;
    wchar_t buf[64];
    if (kb >= 1024ULL) swprintf_s(buf, L"%llu MB", kb / 1024ULL);
    else               swprintf_s(buf, L"%llu KB", kb);
    return std::wstring(buf);
}

// Display-only names for the LLC domains, mirroring the decision table documented in
// topology.h. Nothing downstream depends on these strings.
std::vector<std::wstring> DomainLabels(const Topology& t) {
    const size_t n = t.domains.size();
    std::vector<std::wstring> out(n);
    if (n == 0) return out;
    if (n == 1) { out[0] = L"All cores"; return out; }

    if (t.kind == TopologyKind::IntelHybrid) {
        std::vector<int> cls(n, 0);
        int hi = -1, lo = 1000;
        for (size_t i = 0; i < n; ++i) {
            int best = 0;
            for (size_t j = 0; j < t.domains[i].lps.size(); ++j) {
                const CpuSetEntry* e = FindByLp(t, t.domains[i].lps[j]);
                if (e && static_cast<int>(e->EfficiencyClass) > best) {
                    best = static_cast<int>(e->EfficiencyClass);
                }
            }
            cls[i] = best;
            if (best > hi) hi = best;
            if (best < lo) lo = best;
        }
        for (size_t i = 0; i < n; ++i) {
            if (cls[i] == hi)      out[i] = L"P-cores";
            else if (cls[i] == lo) out[i] = L"E-cores";
            else {
                wchar_t b[48];
                swprintf_s(b, L"Class %d cores", cls[i]);
                out[i] = b;
            }
        }
        return out;
    }

    if (t.kind == TopologyKind::AmdAsymmetricCache) {
        ULONGLONG biggest = 0;
        size_t bigAt = 0;
        for (size_t i = 0; i < n; ++i) {
            if (t.domains[i].l3Bytes > biggest) { biggest = t.domains[i].l3Bytes; bigAt = i; }
        }
        int freqN = 0;
        for (size_t i = 0; i < n; ++i) {
            if (i == bigAt) { out[i] = L"Cache group"; continue; }
            ++freqN;
            if (freqN == 1) out[i] = L"Freq group";
            else {
                wchar_t b[48];
                swprintf_s(b, L"Freq %d group", freqN);
                out[i] = b;
            }
        }
        return out;
    }

    if (t.kind == TopologyKind::MultiCcdSymmetric) {
        // Ordered by the domain's lowest logical processor index, matching mask naming.
        std::vector<size_t> order(n);
        for (size_t i = 0; i < n; ++i) order[i] = i;
        std::sort(order.begin(), order.end(), [&t](size_t a, size_t b) {
            ULONG la = t.domains[a].lps.empty() ? 0xFFFFFFFFu : t.domains[a].lps.front();
            ULONG lb = t.domains[b].lps.empty() ? 0xFFFFFFFFu : t.domains[b].lps.front();
            return la < lb;
        });
        for (size_t k = 0; k < order.size(); ++k) {
            wchar_t b[48];
            swprintf_s(b, L"CCD%d", static_cast<int>(k));
            out[order[k]] = b;
        }
        return out;
    }

    for (size_t i = 0; i < n; ++i) {
        wchar_t b[48];
        swprintf_s(b, L"Domain %lu", static_cast<unsigned long>(t.domains[i].index));
        out[i] = b;
    }
    return out;
}

bool IsParked(const State* st, ULONG lp) {
    std::map<ULONG, bool>::const_iterator it = st->parked.find(lp);
    return it != st->parked.end() && it->second;
}

// The domain that owns an LP, or SIZE_MAX.
size_t DomainOfLp(const Topology& t, ULONG lp) {
    for (size_t i = 0; i < t.domains.size(); ++i) {
        const std::vector<ULONG>& v = t.domains[i].lps;
        for (size_t j = 0; j < v.size(); ++j) {
            if (v[j] == lp) return i;
        }
    }
    return static_cast<size_t>(-1);
}

// ---------------------------------------------------------------------------
// Layout
// ---------------------------------------------------------------------------

struct Metrics {
    int pad, cellW, cellH, innerGap, unitGap, rowGap;
    int headerH, groupHeaderH, afterHeader, panelBottom;
    int legendLineH, readoutH, indent;
};

// Only the constants move here; the packing algorithm below is untouched. The values are
// the card language's spacing: cells sit a card's padding in from the card edge, SMT
// siblings are Dp(2) apart inside one shared border, and panelBottom is the gap BETWEEN
// two cards, so it has to clear the few pixels each card grows past its content.
Metrics MakeMetrics(const State* st) {
    Metrics m;
    m.pad          = st->S(12);
    m.cellW        = st->S(34);
    m.cellH        = st->S(30);
    m.innerGap     = st->S(2);    // between SMT siblings, inside their shared frame
    m.unitGap      = st->S(12);   // between physical cores
    m.rowGap       = st->S(10);
    m.headerH      = st->S(24);
    m.groupHeaderH = st->S(24);
    m.afterHeader  = st->S(8);
    m.panelBottom  = st->S(22);   // card bottom overhang + the gap to the next card
    m.legendLineH  = st->S(20);
    m.readoutH     = st->S(28);
    m.indent       = st->S(14);   // card padding: cells and header text share this inset
    return m;
}

void BuildPanel(State* st, const Metrics& m, int clientW, int& y, int& row,
                const std::wstring& header, bool selectable,
                const std::vector<ULONG>& lps) {
    MapPanel p;
    p.header = header;
    p.selectable = selectable;
    p.headerRect.left = m.pad;
    p.headerRect.top = y;
    p.headerRect.right = IMax(m.pad + 1, clientW - m.pad);
    p.headerRect.bottom = y + m.headerH;
    y += m.headerH + m.afterHeader;

    // Group the panel's LPs by CoreIndex so SMT siblings end up adjacent, keeping the
    // cores in ascending lowest-LP order.
    std::vector<ULONG> coreOrder;
    std::map<ULONG, std::vector<ULONG>> byCore;
    for (size_t i = 0; i < lps.size(); ++i) {
        const CpuSetEntry* e = FindByLp(*st->topo, lps[i]);
        ULONG ci = e ? e->CoreIndex : lps[i];
        if (byCore.find(ci) == byCore.end()) coreOrder.push_back(ci);
        byCore[ci].push_back(lps[i]);
    }
    size_t maxUnit = 1;
    for (size_t i = 0; i < coreOrder.size(); ++i) {
        size_t sz = byCore[coreOrder[i]].size();
        if (sz > maxUnit) maxUnit = sz;
    }

    const int unitW = static_cast<int>(maxUnit) * m.cellW +
                      (static_cast<int>(maxUnit) - 1) * m.innerGap;
    const int startX = m.pad + m.indent;
    const int right = IMax(startX + unitW, clientW - m.pad);

    int x = startX;
    bool anyOnRow = false;
    const int panelIdx = static_cast<int>(st->panels.size());

    for (size_t u = 0; u < coreOrder.size(); ++u) {
        if (anyOnRow && x + unitW > right) {
            x = startX;
            y += m.cellH + m.rowGap;
            ++row;
            anyOnRow = false;
        }
        std::vector<ULONG>& members = byCore[coreOrder[u]];
        std::sort(members.begin(), members.end());

        std::vector<int> unitCells;
        for (size_t k = 0; k < maxUnit; ++k) {
            MapCell c;
            c.panel = panelIdx;
            c.row = row;
            c.coreIndex = coreOrder[u];
            if (k < members.size()) {
                c.present = true;
                c.lp = members[k];
            }
            c.r.left = x + static_cast<int>(k) * (m.cellW + m.innerGap);
            c.r.top = y;
            c.r.right = c.r.left + m.cellW;
            c.r.bottom = y + m.cellH;
            unitCells.push_back(static_cast<int>(st->cells.size()));
            st->cells.push_back(c);
        }
        p.units.push_back(unitCells);
        x += unitW + m.unitGap;
        anyOnRow = true;
    }

    if (anyOnRow) { y += m.cellH; ++row; }
    y += m.panelBottom;
    st->panels.push_back(p);
}

void Layout(HWND h, State* st, int depth) {
    if (st->laying) return;
    st->laying = true;

    st->cells.clear();
    st->panels.clear();
    st->groups.clear();
    st->contentH = 0;
    st->rowCount = 0;
    SetRectEmpty(&st->legendRect);

    RECT rc;
    GetClientRect(h, &rc);
    const Metrics m = MakeMetrics(st);
    const int clientW = rc.right;
    const int viewH = IMax(0, rc.bottom - m.readoutH);

    if (!st->topo || st->topo->entries.empty()) {
        st->scrollY = 0;
        if (st->scrollShown) { st->scrollShown = false; ShowScrollBar(h, SB_VERT, FALSE); }
        st->laying = false;
        return;
    }

    const Topology& t = *st->topo;
    const std::vector<std::wstring> labels = DomainLabels(t);
    const bool byGroup = t.totalLogicalProcessors > 64;

    std::vector<USHORT> groupIds;
    for (size_t i = 0; i < t.entries.size(); ++i) {
        USHORT g = t.entries[i].Group;
        if (std::find(groupIds.begin(), groupIds.end(), g) == groupIds.end()) {
            groupIds.push_back(g);
        }
    }
    std::sort(groupIds.begin(), groupIds.end());
    if (groupIds.empty()) groupIds.push_back(0);

    int y = m.pad;
    int row = 0;

    const size_t passes = byGroup ? groupIds.size() : 1;
    for (size_t gi = 0; gi < passes; ++gi) {
        if (byGroup) {
            GroupHeader gh;
            wchar_t b[64];
            swprintf_s(b, L"Processor group %u", static_cast<unsigned>(groupIds[gi]));
            gh.text = b;
            gh.r.left = m.pad;
            gh.r.top = y;
            gh.r.right = IMax(m.pad + 1, clientW - m.pad);
            gh.r.bottom = y + m.groupHeaderH;
            st->groups.push_back(gh);
            y += m.groupHeaderH + m.afterHeader;
        }

        std::set<ULONG> covered;
        for (size_t d = 0; d < t.domains.size(); ++d) {
            std::vector<ULONG> lps;
            for (size_t k = 0; k < t.domains[d].lps.size(); ++k) {
                ULONG lp = t.domains[d].lps[k];
                const CpuSetEntry* e = FindByLp(t, lp);
                if (!e) continue;
                if (byGroup && e->Group != groupIds[gi]) continue;
                lps.push_back(lp);
                covered.insert(lp);
            }
            if (lps.empty()) continue;
            std::sort(lps.begin(), lps.end());

            std::wstring head = (d < labels.size() && !labels[d].empty())
                                    ? labels[d] : std::wstring(L"Cache group");
            wchar_t cnt[64];
            swprintf_s(cnt, L" - %d logical processors", static_cast<int>(lps.size()));
            head += cnt;
            std::wstring l3 = FormatL3(t.domains[d].l3Bytes);
            if (!l3.empty()) { head += L" - "; head += l3; head += L" L3"; }

            BuildPanel(st, m, clientW, y, row, head, true, lps);
        }

        // Anything the classifier left out of every domain still exists on the machine.
        // Show it, greyed, rather than silently dropping cores off the map.
        std::vector<ULONG> orphans;
        for (size_t i = 0; i < t.entries.size(); ++i) {
            const CpuSetEntry& e = t.entries[i];
            if (byGroup && e.Group != groupIds[gi]) continue;
            if (covered.find(e.LogicalProcessorIndex) != covered.end()) continue;
            orphans.push_back(e.LogicalProcessorIndex);
        }
        if (!orphans.empty()) {
            std::sort(orphans.begin(), orphans.end());
            wchar_t b[96];
            swprintf_s(b, L"Unassigned - %d logical processors - no cache domain",
                       static_cast<int>(orphans.size()));
            BuildPanel(st, m, clientW, y, row, b, false, orphans);
        }
    }

    st->rowCount = row;

    st->legendRect.left = m.pad;
    st->legendRect.top = y;
    st->legendRect.right = IMax(m.pad + 1, clientW - m.pad);
    st->legendRect.bottom = y + m.legendLineH * 5 + st->S(6);
    y = st->legendRect.bottom + m.pad;

    st->contentH = y;

    const int maxScroll = IMax(0, st->contentH - viewH);
    if (st->scrollY > maxScroll) st->scrollY = maxScroll;
    if (st->scrollY < 0) st->scrollY = 0;

    SCROLLINFO si;
    ZeroMemory(&si, sizeof(si));
    si.cbSize = sizeof(si);
    si.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
    si.nMin = 0;
    si.nMax = st->contentH > 0 ? st->contentH - 1 : 0;
    si.nPage = static_cast<UINT>(viewH > 0 ? viewH : 1);
    si.nPos = st->scrollY;
    SetScrollInfo(h, SB_VERT, &si, TRUE);

    const bool need = maxScroll > 0;
    if (need != st->scrollShown) {
        st->scrollShown = need;
        ShowScrollBar(h, SB_VERT, need ? TRUE : FALSE);
        if (depth == 0) {
            st->laying = false;
            Layout(h, st, 1);   // the scroll bar changed the client width; redo once
            return;
        }
    }
    st->laying = false;
}

// ---------------------------------------------------------------------------
// Painting
// ---------------------------------------------------------------------------

using theme::Font;

int CellRadius(int dpi) { return theme::Dp(6, dpi); }

// A hollow rounded rectangle. RoundRect strokes with the current pen and fills with the
// current brush, so NULL_BRUSH leaves whatever is already underneath; PS_INSIDEFRAME keeps
// a thick pen inside `r` instead of straddling its edge.
void FrameRoundRect(HDC dc, const RECT& r, int radius, COLORREF colour, int thickness) {
    if (r.right <= r.left || r.bottom <= r.top) return;
    HPEN pen = CreatePen(PS_INSIDEFRAME, thickness > 0 ? thickness : 1, colour);
    if (!pen) return;
    HGDIOBJ oldPen = SelectObject(dc, pen);
    HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));
    RoundRect(dc, r.left, r.top, r.right, r.bottom, radius * 2, radius * 2);
    SelectObject(dc, oldBrush);
    SelectObject(dc, oldPen);
    DeleteObject(pen);
}

// The parked hatch, clipped to the cell's rounded outline so it does not spill into the
// corners. Parked must remain readable WITHOUT colour - see the file header - so this is a
// texture over the fill, never a different fill.
void HatchRoundRect(HDC dc, const RECT& r, int radius, COLORREF colour) {
    if (r.right <= r.left || r.bottom <= r.top) return;
    HRGN rgn = CreateRoundRectRgn(r.left, r.top, r.right + 1, r.bottom + 1,
                                  radius * 2, radius * 2);
    if (!rgn) return;
    // SaveDC/RestoreDC is what puts the caller's clip back; without it the hatch clip would
    // leak into everything painted after this cell.
    const int saved = SaveDC(dc);
    if (saved != 0) {
        ExtSelectClipRgn(dc, rgn, RGN_AND);   // AND, so the content clip above still holds
        HBRUSH hatch = CreateHatchBrush(HS_FDIAGONAL, colour);
        if (hatch) {
            const int oldMode = SetBkMode(dc, TRANSPARENT);
            FillRect(dc, &r, hatch);
            SetBkMode(dc, oldMode);
            DeleteObject(hatch);
        }
        RestoreDC(dc, saved);
    }
    DeleteObject(rgn);
}

void FillSolid(HDC dc, const RECT& r, COLORREF colour) {
    HBRUSH br = CreateSolidBrush(colour);
    if (!br) return;
    FillRect(dc, &r, br);
    DeleteObject(br);
}

void DrawStrike(HDC dc, const RECT& r, int dpi, COLORREF colour) {
    HPEN pen = CreatePen(PS_SOLID, theme::Dp(1, dpi), colour);
    if (!pen) return;
    HGDIOBJ old = SelectObject(dc, pen);
    const int y = (r.top + r.bottom) / 2;
    MoveToEx(dc, r.left + theme::Dp(6, dpi), y, nullptr);
    LineTo(dc, r.right - theme::Dp(6, dpi), y);
    SelectObject(dc, old);
    DeleteObject(pen);
}

// The four cell states in ONE place, so a cell and its legend swatch cannot drift apart.
//
// PAINT ORDER IS LOAD-BEARING. The hatch is a full-strength texture across the whole cell,
// so anything drawn before it is covered by it. The cell's logical-processor number is the
// one thing that must never be covered - a parked processor is exactly the one a user needs
// to identify, because a mask built from parked processors is silently ignored by Windows -
// so the number is NOT drawn here. This function paints fill, then hatch, then border, and
// the caller draws the number afterwards, on top of everything. See PaintContent, which also
// lifts the number to a higher-contrast colour once a hatch is under it.
void DrawCellFace(HDC dc, const RECT& r, int dpi, bool selected, bool parked, bool inert) {
    const theme::Palette& p = theme::P();
    const int rad = CellRadius(dpi);

    COLORREF fill   = p.cardBgAlt;
    COLORREF border = p.border;
    if (inert) {
        fill = p.inputBg;   border = p.border;
    } else if (selected) {
        fill = p.accent;    border = p.accentHover;
    }

    // (1) the rounded fill, on its own - no border yet, so the hatch cannot sit over it.
    theme::FillRoundRect(dc, r, rad, fill);

    // (2) the parked texture, at full strength, clipped to the same rounded outline.
    if (parked) HatchRoundRect(dc, r, rad, p.parkedHatch);

    // (3) the border last of the surface layers, so the hatch cannot nibble the outline.
    FrameRoundRect(dc, r, rad, border, 1);
    if (selected) {
        // Fill AND a doubled ring: selection survives greyscale and colour blindness.
        RECT inner = r;
        InflateRect(&inner, -1, -1);
        FrameRoundRect(dc, inner, rad > 1 ? rad - 1 : rad, p.accentHover, 1);
    }
    if (inert) DrawStrike(dc, r, dpi, p.textDim);
}

void DrawStateSample(HDC dc, State* st, RECT r, int kind) {
    // kind: 0 not selected, 1 selected, 2 parked, 3 excluded/absent
    DrawCellFace(dc, r, st->dpi, kind == 1, kind == 2, kind == 3);
}

// ---- Panel header decoration ----------------------------------------------
// The header string is built once in Layout as "<name> - <measurements>". Painting splits
// it so the name gets proportional UI type and the numbers get mono, which is the whole
// point of the type contrast: "16 logical processors - 96 MB L3" is columnar data.
void SplitHeader(const std::wstring& header, std::wstring& name, std::wstring& detail) {
    const size_t at = header.find(L" - ");
    if (at == std::wstring::npos) { name = header; detail.clear(); return; }
    name = header.substr(0, at);
    detail = header.substr(at + 3);
}

struct DomainTag {
    std::wstring text;
    COLORREF bg;
    COLORREF fg;
};

bool StartsWith(const std::wstring& s, const wchar_t* prefix) {
    const size_t n = wcslen(prefix);
    return s.size() >= n && wcsncmp(s.c_str(), prefix, n) == 0;
}

// Cache domain blue, frequency domain violet - so the two families separate at a glance.
// The pill still spells the family out, because hue alone is not an affordance.
DomainTag TagFor(const std::wstring& name, bool selectable) {
    const theme::Palette& p = theme::P();
    DomainTag t;
    t.fg = p.textOnAccent;
    t.bg = p.cacheDomain;
    if (!selectable) {
        t.text = L"no domain";
        t.bg = p.cardBgAlt;
        t.fg = p.textDim;
    } else if (StartsWith(name, L"Cache"))      { t.text = L"cache";  t.bg = p.cacheDomain; }
    else if (StartsWith(name, L"Freq"))         { t.text = L"freq";   t.bg = p.freqDomain; }
    else if (StartsWith(name, L"P-core"))       { t.text = L"perf";   t.bg = p.cacheDomain; }
    else if (StartsWith(name, L"E-core"))       { t.text = L"eff";    t.bg = p.freqDomain; }
    else if (StartsWith(name, L"Class"))        { t.text = L"class";  t.bg = p.freqDomain; }
    else if (StartsWith(name, L"CCD"))          { t.text = L"ccd";    t.bg = p.cacheDomain; }
    else if (StartsWith(name, L"All"))          { t.text = L"all";    t.bg = p.cacheDomain; }
    else                                        { t.text = L"domain"; t.bg = p.cacheDomain; }
    return t;
}

// The card behind one domain panel, derived at PAINT time from the layout's own rects, so
// the packing algorithm above stays untouched.
RECT PanelCardRect(const State* st, const MapPanel& panel) {
    RECT card = panel.headerRect;
    card.top -= st->S(6);
    card.bottom = panel.headerRect.bottom + st->S(6);
    bool any = false;
    RECT u = { 0, 0, 0, 0 };
    for (size_t i = 0; i < panel.units.size(); ++i) {
        for (size_t k = 0; k < panel.units[i].size(); ++k) {
            const int ci = panel.units[i][k];
            if (ci < 0 || ci >= static_cast<int>(st->cells.size())) continue;
            const RECT& cr = st->cells[ci].r;
            if (!any) { u = cr; any = true; continue; }
            if (cr.left   < u.left)   u.left = cr.left;
            if (cr.top    < u.top)    u.top = cr.top;
            if (cr.right  > u.right)  u.right = cr.right;
            if (cr.bottom > u.bottom) u.bottom = cr.bottom;
        }
    }
    if (any) card.bottom = u.bottom + st->S(10);
    return card;
}

void PaintContent(HDC dc, HWND h, State* st, const RECT& rc) {
    const Metrics m = MakeMetrics(st);
    const theme::Palette& pal = theme::P();
    const int dpi = st->dpi;
    const int dy = -st->scrollY;
    const bool hasFocus = (GetFocus() == h);

    SetBkMode(dc, TRANSPARENT);

    // Processor group headings sit on the app background, above the cards they introduce.
    for (size_t i = 0; i < st->groups.size(); ++i) {
        RECT r = st->groups[i].r;
        OffsetRect(&r, 0, dy);
        r.left += m.indent;
        theme::DrawText(dc, r, st->groups[i].text, Font::UiHeading, dpi, pal.textPrimary,
                        DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
    }

    // Panels, each one a card.
    for (size_t p = 0; p < st->panels.size(); ++p) {
        const MapPanel& panel = st->panels[p];

        RECT card = PanelCardRect(st, panel);
        OffsetRect(&card, 0, dy);
        theme::DrawCard(dc, card, dpi);

        // Header: name in proportional type, measurements in mono, family tag on the right.
        RECT hr = panel.headerRect;
        OffsetRect(&hr, 0, dy);
        hr.left += m.indent;                       // aligns with the first cell of a row
        hr.right = IMax(hr.left, hr.right - m.indent);

        std::wstring name, detail;
        SplitHeader(panel.header, name, detail);
        const DomainTag tag = TagFor(name, panel.selectable);

        int tagLeft = hr.right;
        if (!tag.text.empty()) {
            const SIZE ts = theme::MeasureText(dc, tag.text, Font::UiSmall, dpi);
            const int pw = IMin(hr.right - hr.left, ts.cx + theme::Dp(16, dpi));
            const int ph = IMin(hr.bottom - hr.top, ts.cy + theme::Dp(6, dpi));
            if (pw > 0 && ph > 0) {
                RECT pill;
                pill.right = hr.right;
                pill.left = hr.right - pw;
                pill.top = hr.top + ((hr.bottom - hr.top) - ph) / 2;
                pill.bottom = pill.top + ph;
                theme::DrawPill(dc, pill, tag.text, dpi, tag.bg, tag.fg);
                tagLeft = pill.left;
            }
        }

        RECT nr = hr;
        nr.right = IMax(nr.left, tagLeft - theme::Dp(10, dpi));
        theme::DrawText(dc, nr, name, Font::UiHeading, dpi,
                        panel.selectable ? pal.textPrimary : pal.textSecondary,
                        DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
        if (!detail.empty()) {
            const SIZE ns = theme::MeasureText(dc, name, Font::UiHeading, dpi);
            RECT dr = nr;
            dr.left = IMin(nr.right, nr.left + ns.cx + theme::Dp(10, dpi));
            if (dr.right > dr.left) {
                theme::DrawText(dc, dr, detail, Font::MonoBody, dpi, pal.textSecondary,
                                DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX |
                                DT_END_ELLIPSIS);
            }
        }

        for (size_t u = 0; u < panel.units.size(); ++u) {
            const std::vector<int>& unit = panel.units[u];
            if (unit.empty()) continue;

            // One rounded frame around the physical core: the cells inside it share a core,
            // which is the single most misread thing on this map.
            if (unit.size() > 1) {
                RECT ur = st->cells[unit.front()].r;
                for (size_t k = 1; k < unit.size(); ++k) {
                    const RECT& cr = st->cells[unit[k]].r;
                    if (cr.left < ur.left) ur.left = cr.left;
                    if (cr.top < ur.top) ur.top = cr.top;
                    if (cr.right > ur.right) ur.right = cr.right;
                    if (cr.bottom > ur.bottom) ur.bottom = cr.bottom;
                }
                InflateRect(&ur, theme::Dp(3, dpi), theme::Dp(3, dpi));
                OffsetRect(&ur, 0, dy);
                FrameRoundRect(dc, ur, CellRadius(dpi) + theme::Dp(3, dpi),
                               pal.borderStrong, 1);
            }

            for (size_t k = 0; k < unit.size(); ++k) {
                const MapCell& c = st->cells[unit[k]];
                RECT r = c.r;
                OffsetRect(&r, 0, dy);
                const bool live = c.present && panel.selectable;
                const bool sel = live && st->sel.find(c.lp) != st->sel.end();
                const bool parked = live && IsParked(st, c.lp);

                // Surface first (fill, hatch, border), THEN the number, so a parked cell
                // still says which logical processor it is.
                DrawCellFace(dc, r, dpi, sel, parked, !live);

                const std::wstring txt =
                    c.present ? std::to_wstring(c.lp) : std::wstring(L"-");
                // Over a hatch, textSecondary is only a step or two off the hatch itself and
                // the digits break up. The fix is a brighter ink, never a fainter hatch: the
                // hatch is the only non-colour cue that the processor is parked.
                COLORREF fg;
                if (!live)       fg = pal.textDim;
                else if (sel)    fg = pal.textOnAccent;
                else if (parked) fg = pal.textPrimary;
                else             fg = pal.textSecondary;
                theme::DrawText(dc, r, txt, Font::MonoSmall, dpi, fg,
                                DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

                if (hasFocus && st->focus == unit[k]) {
                    RECT fr = r;
                    InflateRect(&fr, -theme::Dp(2, dpi), -theme::Dp(2, dpi));
                    FrameRoundRect(dc, fr, IMax(1, CellRadius(dpi) - theme::Dp(2, dpi)),
                                   sel ? pal.textOnAccent : pal.accentHover, 1);
                }
            }
        }
    }

    // Legend, on its own card.
    RECT lr = st->legendRect;
    OffsetRect(&lr, 0, dy);
    RECT legendCard = lr;
    legendCard.top -= st->S(6);
    legendCard.bottom += st->S(6);
    theme::DrawCard(dc, legendCard, dpi);

    RECT title = lr;
    title.left += m.indent;
    title.right = IMax(title.left, title.right - m.indent);
    title.bottom = title.top + m.legendLineH;
    theme::DrawText(dc, title, L"Legend", Font::UiSmall, dpi, pal.textSecondary,
                    DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

    static const wchar_t* kLegend[4] = {
        L"Selected - filled and ringed",
        L"Not selected - outlined only",
        L"Parked by Windows - diagonal hatch (a mask over parked cores does nothing)",
        L"Excluded or absent - dimmed and struck through"
    };
    for (int i = 0; i < 4; ++i) {
        RECT row;
        row.left = lr.left + m.indent;
        row.top = lr.top + m.legendLineH * (i + 1);
        row.bottom = row.top + m.legendLineH;
        row.right = IMax(row.left, lr.right - m.indent);

        RECT sw;
        sw.left = row.left;
        sw.top = row.top + theme::Dp(3, dpi);
        sw.right = sw.left + theme::Dp(28, dpi);
        sw.bottom = row.bottom - theme::Dp(3, dpi);
        if (sw.bottom <= sw.top) sw.bottom = sw.top + 1;
        const int kind = (i == 0) ? 1 : (i == 1) ? 0 : (i == 2) ? 2 : 3;
        DrawStateSample(dc, st, sw, kind);

        RECT txt = row;
        txt.left = IMin(row.right, sw.right + theme::Dp(10, dpi));
        theme::DrawText(dc, txt, kLegend[i], Font::UiSmall, dpi, pal.textDim,
                        DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
    }

    (void)rc;
}

std::wstring BuildReadout(State* st) {
    if (!st->topo) return std::wstring(L"No topology");
    if (st->hover < 0 || st->hover >= static_cast<int>(st->cells.size())) {
        return std::wstring(L"Hover a cell for details");
    }
    const MapCell& c = st->cells[st->hover];
    if (!c.present) return std::wstring(L"Absent - no logical processor here");

    const CpuSetEntry* e = FindByLp(*st->topo, c.lp);
    ULONG core = e ? e->CoreIndex : c.coreIndex;
    ULONGLONG l3 = 0;
    size_t d = DomainOfLp(*st->topo, c.lp);
    if (d != static_cast<size_t>(-1)) l3 = st->topo->domains[d].l3Bytes;

    std::wstring l3s = FormatL3(l3);
    if (l3s.empty()) l3s = L"unknown";

    wchar_t buf[256];
    swprintf_s(buf, L"LP %lu - core %lu - L3 %s - parked: %s",
               static_cast<unsigned long>(c.lp),
               static_cast<unsigned long>(core),
               l3s.c_str(),
               IsParked(st, c.lp) ? L"yes" : L"no");
    return std::wstring(buf);
}

void OnPaint(HWND h, State* st) {
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(h, &ps);
    RECT rc;
    GetClientRect(h, &rc);
    const int w = IMax(1, rc.right);
    const int ht = IMax(1, rc.bottom);

    HDC mem = CreateCompatibleDC(hdc);
    HBITMAP bmp = CreateCompatibleBitmap(hdc, w, ht);
    HGDIOBJ oldBmp = SelectObject(mem, bmp);

    RECT all = { 0, 0, w, ht };
    theme::FillBackground(mem, all);
    SetBkMode(mem, TRANSPARENT);

    const theme::Palette& pal = theme::P();
    const Metrics m = MakeMetrics(st);
    const int viewH = IMax(0, ht - m.readoutH);

    if (!st->topo || st->topo->entries.empty()) {
        theme::DrawText(mem, all, L"No CPU topology available.", Font::UiBody, st->dpi,
                        pal.textDim, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    } else {
        int saved = SaveDC(mem);
        IntersectClipRect(mem, 0, 0, w, viewH);
        PaintContent(mem, h, st, rc);
        RestoreDC(mem, saved);
    }

    // The readout strip is fixed: it must stay visible however far the map is scrolled.
    RECT strip = { 0, viewH, w, ht };
    FillSolid(mem, strip, pal.sidebarBg);
    RECT rule = { 0, viewH, w, viewH + 1 };
    FillSolid(mem, rule, pal.border);

    // A live readout is technical data - LP, core, cache size - so it is set in mono. The
    // idle prompt is prose and is not.
    const bool technical =
        st->topo != nullptr &&
        st->hover >= 0 && st->hover < static_cast<int>(st->cells.size()) &&
        st->cells[st->hover].present;
    RECT tr = strip;
    tr.left += m.pad + m.indent;      // aligned with the card content above it
    tr.top += 1;
    const std::wstring readout = BuildReadout(st);
    theme::DrawText(mem, tr, readout,
                    technical ? Font::MonoBody : Font::UiSmall, st->dpi,
                    technical ? pal.textSecondary : pal.textDim,
                    DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);

    BitBlt(hdc, 0, 0, w, ht, mem, 0, 0, SRCCOPY);

    SelectObject(mem, oldBmp);
    DeleteObject(bmp);
    DeleteDC(mem);
    EndPaint(h, &ps);
}

// ---------------------------------------------------------------------------
// Hit testing and selection
// ---------------------------------------------------------------------------

bool InContentArea(HWND h, State* st, POINT p) {
    RECT rc;
    GetClientRect(h, &rc);
    const Metrics m = MakeMetrics(st);
    return p.y >= 0 && p.y < IMax(0, rc.bottom - m.readoutH);
}

int HitCell(State* st, POINT p) {
    POINT q = p;
    q.y += st->scrollY;
    for (size_t i = 0; i < st->cells.size(); ++i) {
        if (PtInRect(&st->cells[i].r, q)) return static_cast<int>(i);
    }
    return -1;
}

int HitHeader(State* st, POINT p) {
    POINT q = p;
    q.y += st->scrollY;
    for (size_t i = 0; i < st->panels.size(); ++i) {
        if (PtInRect(&st->panels[i].headerRect, q)) return static_cast<int>(i);
    }
    return -1;
}

void NotifyParent(HWND h) {
    HWND parent = GetParent(h);
    if (!parent) return;
    SendMessageW(parent, WM_COMMAND,
                 MAKEWPARAM(static_cast<WORD>(GetDlgCtrlID(h)), CMN_SELECTION_CHANGED),
                 reinterpret_cast<LPARAM>(h));
}

// Toggle as a set: all selected -> clear them all, otherwise select them all.
void ToggleCells(HWND h, State* st, const std::vector<int>& idxs) {
    std::vector<ULONG> live;
    for (size_t i = 0; i < idxs.size(); ++i) {
        int ci = idxs[i];
        if (ci < 0 || ci >= static_cast<int>(st->cells.size())) continue;
        const MapCell& c = st->cells[ci];
        if (!c.present) continue;
        if (c.panel < 0 || c.panel >= static_cast<int>(st->panels.size())) continue;
        if (!st->panels[c.panel].selectable) continue;
        live.push_back(c.lp);
    }
    if (live.empty()) return;

    bool allSel = true;
    for (size_t i = 0; i < live.size(); ++i) {
        if (st->sel.find(live[i]) == st->sel.end()) { allSel = false; break; }
    }
    for (size_t i = 0; i < live.size(); ++i) {
        if (allSel) st->sel.erase(live[i]);
        else        st->sel.insert(live[i]);
    }
    InvalidateRect(h, nullptr, FALSE);
    NotifyParent(h);
}

void EnsureVisible(HWND h, State* st, int cellIdx) {
    if (cellIdx < 0 || cellIdx >= static_cast<int>(st->cells.size())) return;
    RECT rc;
    GetClientRect(h, &rc);
    const Metrics m = MakeMetrics(st);
    const int viewH = IMax(1, rc.bottom - m.readoutH);
    const RECT& r = st->cells[cellIdx].r;
    int top = st->scrollY;
    if (r.top - m.pad < top) top = IMax(0, r.top - m.pad);
    if (r.bottom + m.pad > top + viewH) top = r.bottom + m.pad - viewH;
    const int maxScroll = IMax(0, st->contentH - viewH);
    if (top > maxScroll) top = maxScroll;
    if (top < 0) top = 0;
    if (top != st->scrollY) {
        st->scrollY = top;
        SCROLLINFO si;
        ZeroMemory(&si, sizeof(si));
        si.cbSize = sizeof(si);
        si.fMask = SIF_POS;
        si.nPos = st->scrollY;
        SetScrollInfo(h, SB_VERT, &si, TRUE);
        InvalidateRect(h, nullptr, FALSE);
    }
}

void MoveFocus(HWND h, State* st, int dir, bool vertical) {
    if (st->cells.empty()) return;
    if (st->focus < 0 || st->focus >= static_cast<int>(st->cells.size())) {
        st->focus = 0;
        EnsureVisible(h, st, st->focus);
        InvalidateRect(h, nullptr, FALSE);
        return;
    }
    if (!vertical) {
        int n = st->focus + dir;
        if (n < 0) n = 0;
        if (n >= static_cast<int>(st->cells.size())) n = static_cast<int>(st->cells.size()) - 1;
        st->focus = n;
    } else {
        const MapCell& cur = st->cells[st->focus];
        const int curX = (cur.r.left + cur.r.right) / 2;
        const int wantRow = cur.row + dir;
        int best = -1, bestD = 0;
        for (size_t i = 0; i < st->cells.size(); ++i) {
            if (st->cells[i].row != wantRow) continue;
            int cx = (st->cells[i].r.left + st->cells[i].r.right) / 2;
            int d = cx > curX ? cx - curX : curX - cx;
            if (best < 0 || d < bestD) { best = static_cast<int>(i); bestD = d; }
        }
        if (best >= 0) st->focus = best;
    }
    EnsureVisible(h, st, st->focus);
    InvalidateRect(h, nullptr, FALSE);
}

void ScrollBy(HWND h, State* st, int delta) {
    RECT rc;
    GetClientRect(h, &rc);
    const Metrics m = MakeMetrics(st);
    const int viewH = IMax(1, rc.bottom - m.readoutH);
    const int maxScroll = IMax(0, st->contentH - viewH);
    int np = st->scrollY + delta;
    if (np < 0) np = 0;
    if (np > maxScroll) np = maxScroll;
    if (np == st->scrollY) return;
    st->scrollY = np;
    SCROLLINFO si;
    ZeroMemory(&si, sizeof(si));
    si.cbSize = sizeof(si);
    si.fMask = SIF_POS;
    si.nPos = st->scrollY;
    SetScrollInfo(h, SB_VERT, &si, TRUE);
    InvalidateRect(h, nullptr, FALSE);
}

// ---------------------------------------------------------------------------
// Window procedure
// ---------------------------------------------------------------------------

LRESULT CALLBACK CoreMapProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    State* st = GetState(h);

    switch (msg) {
    case WM_NCCREATE: {
        State* s = new (std::nothrow) State();
        if (!s) return FALSE;
        SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(s));
        return DefWindowProcW(h, msg, wp, lp);
    }

    case WM_CREATE: {
        if (!st) return -1;
        // The dialog manager reads WS_TABSTOP at tab time, so setting it here makes the
        // control keyboard-reachable even if the creator forgot the style bit.
        SetWindowLongPtrW(h, GWL_STYLE, GetWindowLongPtrW(h, GWL_STYLE) | WS_TABSTOP);
        st->dpi = static_cast<int>(QueryDpi(h));
        Layout(h, st, 0);
        if (GetWindowLongW(h, GWL_STYLE) & WS_VISIBLE) {
            SetTimer(h, kTimerParked, kParkedPeriodMs, nullptr);
            st->timerOn = true;
        }
        return 0;
    }

    case WM_DESTROY: {
        if (st && st->timerOn) { KillTimer(h, kTimerParked); st->timerOn = false; }
        return 0;
    }

    case WM_NCDESTROY: {
        if (st) {
            // No fonts to free: the theme owns them and frees them in theme::Shutdown.
            delete st;
        }
        SetWindowLongPtrW(h, GWLP_USERDATA, 0);
        return DefWindowProcW(h, msg, wp, lp);
    }

    case WM_SHOWWINDOW: {
        if (!st) break;
        if (wp) {
            if (!st->timerOn) {
                SetTimer(h, kTimerParked, kParkedPeriodMs, nullptr);
                st->timerOn = true;
            }
        } else if (st->timerOn) {
            KillTimer(h, kTimerParked);
            st->timerOn = false;
        }
        return 0;
    }

    case WM_TIMER: {
        if (st && wp == kTimerParked) CoreMapRefreshParked(h);
        return 0;
    }

    case WM_SIZE: {
        if (st) {
            int d = static_cast<int>(QueryDpi(h));
            if (d != st->dpi) st->dpi = d;
            Layout(h, st, 0);
            InvalidateRect(h, nullptr, FALSE);
        }
        return 0;
    }

    case WM_SETTINGCHANGE:
    case WM_THEMECHANGED: {
        if (st) {
            st->dpi = static_cast<int>(QueryDpi(h));
            Layout(h, st, 0);
            InvalidateRect(h, nullptr, FALSE);
        }
        return 0;
    }

    case WM_ERASEBKGND:
        return 1;   // fully painted in WM_PAINT through a memory DC

    case WM_PAINT: {
        if (st) OnPaint(h, st);
        else {
            PAINTSTRUCT ps;
            BeginPaint(h, &ps);
            EndPaint(h, &ps);
        }
        return 0;
    }

    case WM_GETDLGCODE:
        return DLGC_WANTARROWS | DLGC_WANTCHARS;

    case WM_SETFOCUS:
    case WM_KILLFOCUS:
        InvalidateRect(h, nullptr, FALSE);
        return 0;

    case WM_VSCROLL: {
        if (!st) break;
        RECT rc;
        GetClientRect(h, &rc);
        const Metrics m = MakeMetrics(st);
        const int viewH = IMax(1, rc.bottom - m.readoutH);
        int step = m.cellH + m.rowGap;
        switch (LOWORD(wp)) {
        case SB_LINEUP:   ScrollBy(h, st, -step); break;
        case SB_LINEDOWN: ScrollBy(h, st, step); break;
        case SB_PAGEUP:   ScrollBy(h, st, -viewH); break;
        case SB_PAGEDOWN: ScrollBy(h, st, viewH); break;
        case SB_TOP:      ScrollBy(h, st, -st->contentH); break;
        case SB_BOTTOM:   ScrollBy(h, st, st->contentH); break;
        case SB_THUMBTRACK:
        case SB_THUMBPOSITION: {
            SCROLLINFO si;
            ZeroMemory(&si, sizeof(si));
            si.cbSize = sizeof(si);
            si.fMask = SIF_TRACKPOS;
            if (GetScrollInfo(h, SB_VERT, &si)) ScrollBy(h, st, si.nTrackPos - st->scrollY);
            break;
        }
        default: break;
        }
        return 0;
    }

    case WM_MOUSEWHEEL: {
        if (!st) break;
        const Metrics m = MakeMetrics(st);
        int delta = GET_WHEEL_DELTA_WPARAM(wp);
        ScrollBy(h, st, -(delta * (m.cellH + m.rowGap)) / WHEEL_DELTA);
        return 0;
    }

    case WM_MOUSEMOVE: {
        if (!st) break;
        if (!st->tracking) {
            TRACKMOUSEEVENT tme;
            ZeroMemory(&tme, sizeof(tme));
            tme.cbSize = sizeof(tme);
            tme.dwFlags = TME_LEAVE;
            tme.hwndTrack = h;
            if (TrackMouseEvent(&tme)) st->tracking = true;
        }
        POINT p = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        int hit = InContentArea(h, st, p) ? HitCell(st, p) : -1;
        if (hit != st->hover) {
            st->hover = hit;
            InvalidateRect(h, nullptr, FALSE);
        }
        return 0;
    }

    case WM_MOUSELEAVE: {
        if (!st) break;
        st->tracking = false;
        if (st->hover != -1) { st->hover = -1; InvalidateRect(h, nullptr, FALSE); }
        return 0;
    }

    case WM_LBUTTONDOWN: {
        if (!st) break;
        SetFocus(h);
        POINT p = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        if (!InContentArea(h, st, p)) return 0;

        int ci = HitCell(st, p);
        if (ci >= 0) {
            st->focus = ci;
            InvalidateRect(h, nullptr, FALSE);
            if (!st->editable) return 0;
            const MapCell& c = st->cells[ci];
            const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
            if (ctrl && c.panel >= 0 && c.panel < static_cast<int>(st->panels.size())) {
                const MapPanel& panel = st->panels[c.panel];
                for (size_t u = 0; u < panel.units.size(); ++u) {
                    if (std::find(panel.units[u].begin(), panel.units[u].end(), ci) !=
                        panel.units[u].end()) {
                        ToggleCells(h, st, panel.units[u]);
                        break;
                    }
                }
            } else {
                std::vector<int> one(1, ci);
                ToggleCells(h, st, one);
            }
            return 0;
        }

        int pi = HitHeader(st, p);
        if (pi >= 0) {
            if (!st->editable) return 0;
            std::vector<int> all;
            const MapPanel& panel = st->panels[pi];
            for (size_t u = 0; u < panel.units.size(); ++u) {
                for (size_t k = 0; k < panel.units[u].size(); ++k) {
                    all.push_back(panel.units[u][k]);
                }
            }
            ToggleCells(h, st, all);
        }
        return 0;
    }

    case WM_KEYDOWN: {
        if (!st) break;
        switch (wp) {
        case VK_LEFT:  MoveFocus(h, st, -1, false); return 0;
        case VK_RIGHT: MoveFocus(h, st, +1, false); return 0;
        case VK_UP:    MoveFocus(h, st, -1, true);  return 0;
        case VK_DOWN:  MoveFocus(h, st, +1, true);  return 0;
        case VK_HOME:  if (!st->cells.empty()) { st->focus = 0; EnsureVisible(h, st, 0);
                           InvalidateRect(h, nullptr, FALSE); } return 0;
        case VK_END:   if (!st->cells.empty()) {
                           st->focus = static_cast<int>(st->cells.size()) - 1;
                           EnsureVisible(h, st, st->focus);
                           InvalidateRect(h, nullptr, FALSE); } return 0;
        case VK_SPACE: {
            if (!st->editable) return 0;
            if (st->focus < 0 || st->focus >= static_cast<int>(st->cells.size())) return 0;
            const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
            const MapCell& c = st->cells[st->focus];
            if (ctrl && c.panel >= 0 && c.panel < static_cast<int>(st->panels.size())) {
                const MapPanel& panel = st->panels[c.panel];
                for (size_t u = 0; u < panel.units.size(); ++u) {
                    if (std::find(panel.units[u].begin(), panel.units[u].end(), st->focus) !=
                        panel.units[u].end()) {
                        ToggleCells(h, st, panel.units[u]);
                        break;
                    }
                }
            } else {
                std::vector<int> one(1, st->focus);
                ToggleCells(h, st, one);
            }
            return 0;
        }
        default: break;
        }
        break;
    }

    default:
        break;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

}  // namespace

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void CoreMapRegister(HINSTANCE hInst) {
    static bool registered = false;
    if (registered) return;

    WNDCLASSEXW wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    wc.lpfnWndProc = CoreMapProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;      // WM_ERASEBKGND is handled
    wc.lpszClassName = kCoreMapClass;

    if (RegisterClassExW(&wc) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS) {
        registered = true;
    } else {
        LogLine(L"CoreMapRegister: RegisterClassExW failed, gle=%lu",
                static_cast<unsigned long>(GetLastError()));
    }
}

void CoreMapSetTopology(HWND hMap, const Topology* t) {
    State* st = GetState(hMap);
    if (!st) return;
    st->topo = t;                    // borrowed - the caller guarantees it outlives us
    st->sel.clear();
    st->parked.clear();
    st->focus = -1;
    st->hover = -1;
    st->scrollY = 0;
    if (t) {
        for (size_t i = 0; i < t->entries.size(); ++i) {
            st->parked[t->entries[i].LogicalProcessorIndex] = t->entries[i].Parked;
        }
    }
    Layout(hMap, st, 0);
    InvalidateRect(hMap, nullptr, FALSE);
}

void CoreMapSetSelection(HWND hMap, const std::vector<ULONG>& ids) {
    State* st = GetState(hMap);
    if (!st) return;
    st->sel.clear();
    if (st->topo) {
        std::vector<ULONG> lps = LpsForIds(*st->topo, ids);
        for (size_t i = 0; i < lps.size(); ++i) st->sel.insert(lps[i]);
    }
    InvalidateRect(hMap, nullptr, FALSE);
}

std::vector<ULONG> CoreMapGetSelection(HWND hMap) {
    std::vector<ULONG> out;
    State* st = GetState(hMap);
    if (!st || !st->topo) return out;

    std::vector<ULONG> lps(st->sel.begin(), st->sel.end());
    std::sort(lps.begin(), lps.end());
    // Never assume Id == LP + 256; the offset is real on the reference machine but is not
    // architectural. Go through the topology's own mapping.
    out = IdsForLps(*st->topo, lps);
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

void CoreMapSetEditable(HWND hMap, bool editable) {
    State* st = GetState(hMap);
    if (!st) return;
    st->editable = editable;
    InvalidateRect(hMap, nullptr, FALSE);
}

void CoreMapRefreshParked(HWND hMap) {
    State* st = GetState(hMap);
    if (!st || !st->topo) return;

    Topology live;
    std::wstring err;
    if (!DetectTopology(live, &err)) return;   // transient; keep the last known flags

    bool changed = false;
    for (size_t i = 0; i < live.entries.size(); ++i) {
        const CpuSetEntry& e = live.entries[i];
        std::map<ULONG, bool>::iterator it = st->parked.find(e.LogicalProcessorIndex);
        if (it == st->parked.end()) {
            st->parked[e.LogicalProcessorIndex] = e.Parked;
            changed = true;
        } else if (it->second != e.Parked) {
            it->second = e.Parked;
            changed = true;
        }
    }
    if (changed) InvalidateRect(hMap, nullptr, FALSE);
}

}  // namespace cd
