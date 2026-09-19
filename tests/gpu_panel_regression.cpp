// Exercises the actual panel implementation with in-memory preference I/O.
// Windows are hidden; no operator registry values, config, or restore files are changed.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdio>
#include <map>
#include "config.h"
#include "gpu_edit.h"
#include "gpu_policy.h"
#include "gpu_pref.h"
#include "gpu_rows.h"
#include "procwatch.h"
#include "theme.h"
#include "util.h"

namespace {
std::map<std::wstring, std::wstring> preferences;
bool complete = true;
bool failPlan = false;
int checks = 0, failures = 0;
const std::wstring mainKey = L"10DE&2B85&53021462", bgKey = L"10DE&2684&40BF1458";
const std::wstring oldPath = L"C:\\GameOptimizerPanelTest\\Chat\\app-1.0\\chat.exe";
const std::wstring newPath = L"C:\\GameOptimizerPanelTest\\Chat\\app-2.0\\chat.exe";
void Check(bool ok, const char* label) {
    ++checks;
    if (!ok) ++failures;
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", label);
}
bool TestRead(const std::wstring& path, std::wstring& out, bool* unreadable = nullptr) {
    if (unreadable) *unreadable = false;
    const auto it = preferences.find(path);
    out = it == preferences.end() ? L"" : it->second;
    return it != preferences.end();
}
std::vector<cd::GpuPreferenceEntry> TestEntries(bool* all = nullptr) {
    if (all) *all = complete;
    std::vector<cd::GpuPreferenceEntry> result;
    for (const auto& p : preferences) result.push_back(cd::MakeGpuPreferenceEntry(p.first, p.second, false));
    return result;
}
bool TestAdapters(std::vector<cd::GpuAdapter>& out, std::wstring* error = nullptr) {
    out.clear();
    cd::GpuAdapter a; a.name = L"Main test GPU"; a.adapterKey = mainKey; a.hasDisplay = true;
    out.push_back(a);
    a.name = L"Background test GPU"; a.adapterKey = bgKey; a.hasDisplay = false;
    out.push_back(a);
    if (error) error->clear();
    return true;
}
cd::GuardedWriteResult TestWrite(const std::wstring& path, bool present, const std::wstring& before,
                                bool remove, const std::wstring& value, unsigned long* error = nullptr) {
    std::wstring live;
    if (error) *error = 0;
    if (TestRead(path, live) != present || live != before) return cd::GuardedWriteResult::Changed;
    if (remove) preferences.erase(path); else preferences[path] = value;
    return cd::GuardedWriteResult::Written;
}
bool TestRecord(cd::GpuRestoreJournal&, const cd::GpuPreferenceBefore&) { return true; }
std::vector<std::wstring> TestUnfinished(const std::wstring&) { return {}; }
std::wstring TestConfigDir() { return L"C:\\GameOptimizerPanelTest"; }
void TestLog(const wchar_t*, ...) {}
int WINAPI TestMessage(HWND, LPCWSTR, LPCWSTR, UINT flags) { return (flags & MB_YESNO) ? IDNO : IDOK; }
DWORD WINAPI TestAttributes(LPCWSTR path) {
    if (std::wstring(path).find(L"C:\\GameOptimizerPanelTest\\") == 0) return FILE_ATTRIBUTE_NORMAL;
    return ::GetFileAttributesW(path);
}
HWND WINAPI TestCreate(DWORD ex, LPCWSTR cls, LPCWSTR text, DWORD style, int x, int y, int w, int h,
                       HWND parent, HMENU menu, HINSTANCE instance, LPVOID param) {
    if (failPlan && reinterpret_cast<UINT_PTR>(cls) > 0xffff && wcscmp(cls, L"STATIC") == 0) {
        failPlan = false;
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return nullptr;
    }
    return ::CreateWindowExW(ex, cls, text, style, x, y, w, h, parent, menu, instance, param);
}
}

// Headers are already loaded. Redirect only this translation unit's OS boundary calls.
#define ReadGpuPreference TestRead
#define EnumerateGpuPreferenceEntries TestEntries
#define EnumerateGpuAdapters TestAdapters
#define GuardedWriteGpuPreference TestWrite
#define RecordGpuRestoreRow TestRecord
#define UnfinishedGpuRestores TestUnfinished
#define GetConfigDir TestConfigDir
#define LogLine TestLog
#define MessageBoxW TestMessage
#define GetFileAttributesW TestAttributes
#define CreateWindowExW TestCreate
#include "../src/gpuwindow.cpp"
#undef CreateWindowExW
#undef GetFileAttributesW
#undef MessageBoxW
#undef LogLine
#undef UnfinishedGpuRestores
#undef GetConfigDir
#undef RecordGpuRestoreRow
#undef GuardedWriteGpuPreference
#undef EnumerateGpuAdapters
#undef EnumerateGpuPreferenceEntries
#undef ReadGpuPreference

int main() {
    SetEnvironmentVariableW(L"GAME_OPTIMIZER_TEST_WRITE_DELAY_MS", nullptr);
    cd::Config cfg;
    cd::ProcessSnapshot snap;
    auto& processes = const_cast<std::map<DWORD, cd::ProcInfo>&>(snap.All());
    cd::ProcInfo proc; proc.pid = 70001; proc.name = L"chat.exe"; proc.fullPath = oldPath;
    processes[proc.pid] = proc;
    proc.pid = 70002; proc.fullPath = newPath; processes[proc.pid] = proc;
    HWND parent = CreateWindowExW(0, L"STATIC", L"Panel regression (hidden)", WS_OVERLAPPED, 0, 0, 800, 600,
                                  nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    HWND panel = cd::CreateGpuPanel(parent, 1905);
    if (!parent || !panel) { std::puts("FAIL hidden panel creation"); return 1; }
    cd::GpuState* st = cd::StateOf(panel);
    const auto reset = [&]() {
        preferences.clear(); complete = true; st->rows.clear(); st->targetKey.clear();
        cd::ActivateGpuPanel(panel, cfg, snap);
    };
    const auto index = [&](const std::wstring& path) {
        for (size_t i = 0; i < st->rows.size(); ++i) if (st->rows[i].r.exePath == path) return i;
        return st->rows.size();
    };

    reset();
    Check(st->rows.size() == 2 && st->targetKey == bgKey, "two sibling versions and background target");
    if (st->rows.size() != 2) return 1;
    cd::DoBulk(st, panel);
    Check(st->rows[index(newPath)].r.selected, "control: unpinned new version auto-selected");
    cd::EditPlan edit; edit.row = index(oldPath); edit.item.exePath = oldPath;
    cd::GpuRestoreJournal journal;
    cd::RunEdits(st, std::vector<cd::EditPlan>(1, edit), false, mainKey, journal);
    Check(!cd::IsMovable(st, st->rows[index(newPath)]), "R3-1: successful pin refreshes sibling evidence");
    Check(!st->rows[index(newPath)].r.selected, "R3-1: write read-back invalidates earlier automatic sibling tick");

    reset();
    preferences[oldPath] = cd::FormatPreferenceValue(mainKey);
    cd::DoBulk(st, panel);
    Check(!st->rows[index(newPath)].r.selected, "R3-1: Auto assign reads externally added sibling pin");

    reset(); cd::DoBulk(st, panel);
    cd::ActivateGpuPanel(panel, cfg, snap);
    Check(st->rows[index(newPath)].r.selected, "unchanged automatic selection survives refresh");
    preferences[oldPath] = cd::FormatPreferenceValue(mainKey);
    cd::LoadGpuData(st, cfg, snap);
    Check(!st->rows[index(newPath)].r.selected, "R3-2: tab refresh drops automatic tick after sibling pin");

    reset(); cd::DoBulk(st, panel);
    preferences[oldPath] = cd::FormatPreferenceValue(mainKey);
    std::vector<cd::EditPlan> ready;
    std::vector<std::wstring> refused;
    size_t changed = 0;
    cd::PrepareEdits(st, false, ready, refused, changed);
    Check(ready.empty() && !refused.empty(), "R3-1: Apply refuses automatic selections after pin change");

    reset(); cd::DoBulk(st, panel); complete = false;
    ready.clear(); refused.clear();
    cd::PrepareEdits(st, false, ready, refused, changed);
    Check(ready.empty(), "incomplete fresh scan refuses automatic writes");

    reset(); cd::DoBulk(st, panel);
    preferences[newPath] = cd::FormatPreferenceValue(mainKey);
    const std::wstring originalChoice = st->rows[index(newPath)].listedChoice;
    ready.clear(); refused.clear();
    cd::PrepareEdits(st, false, ready, refused, changed);
    Check(st->rows[index(newPath)].listedChoice == originalChoice,
          "fresh auto-policy read does not rebase listed consent onto a new GPU choice");
    Check(!st->rows[index(newPath)].r.selected, "own new main-GPU pin invalidates automatic tick");

    reset(); cd::DoBulk(st, panel);
    for (int item = 0; item < SendMessageW(st->hList, LB_GETCOUNT, 0, 0); ++item) {
        if (static_cast<size_t>(SendMessageW(st->hList, LB_GETITEMDATA, item, 0)) != index(newPath)) continue;
        SendMessageW(panel, WM_VKEYTOITEM, MAKEWPARAM(VK_SPACE, item), reinterpret_cast<LPARAM>(st->hList));
        SendMessageW(panel, WM_VKEYTOITEM, MAKEWPARAM(VK_SPACE, item), reinterpret_cast<LPARAM>(st->hList));
    }
    preferences[oldPath] = cd::FormatPreferenceValue(mainKey);
    ready.clear(); refused.clear();
    cd::PrepareEdits(st, false, ready, refused, changed);
    Check(ready.size() == 1 && ready[0].item.exePath == newPath, "Space untick/retick converts bulk tick to manual override");

    reset();
    st->rows[index(newPath)].r.selected = true; // explicit manual selection
    preferences[oldPath] = cd::FormatPreferenceValue(mainKey);
    ready.clear(); refused.clear();
    cd::PrepareEdits(st, false, ready, refused, changed);
    Check(ready.size() == 1 && ready[0].item.exePath == newPath, "manual sibling override remains available");
    cd::LoadGpuData(st, cfg, snap);
    Check(st->rows[index(newPath)].r.selected, "manual sibling override survives unchanged row refresh");

    reset();
    st->prefPairs.push_back(std::make_pair(oldPath, cd::UnreadableChoiceKey()));
    Check(cd::MainGpuPinCount(st) == 0, "R3-3: unreadable sibling preference is not a confirmed main-GPU pin");
    Check(cd::MainGpuPinCount(st, true) == 1, "R3-3: unreadable sibling counted separately as possible pin");
    Check(cd::FormatNothingToMoveLine(0, 1).find(L"could not be read") != std::wstring::npos,
          "R3-3: status explains uncertainty");
    st->rows[index(newPath)].system = true;
    Check(cd::MainGpuPinCount(st, true) == 0, "pin status excludes system rows");
    st->rows[index(newPath)].system = false;
    st->rows[index(newPath)].r.isProfileGame = true;
    Check(cd::MainGpuPinCount(st, true) == 0, "pin status excludes profile games");
    st->rows[index(newPath)].r.isProfileGame = false;
    st->rows[index(newPath)].listed = false;
    Check(cd::MainGpuPinCount(st, true) == 0, "pin status excludes unlisted rows");
    st->rows[index(newPath)].listed = true;
    st->prefPairs[0].second = mainKey;
    Check(cd::MainGpuPinCount(st) == 1 && cd::MainGpuPinCount(st, true) == 0,
          "confirmed sibling counted once without uncertainty");

    DestroyWindow(panel);
    failPlan = true;
    panel = cd::CreateGpuPanel(parent, 1905);
    Check(panel == nullptr, "R3-4: required control failure returns null for Settings fallback");
    if (panel) DestroyWindow(panel);
    panel = cd::CreateGpuPanel(parent, 1905);
    Check(panel != nullptr, "panel can be recreated after required-control failure");
    if (panel) DestroyWindow(panel);
    DestroyWindow(parent);
    std::printf("PANEL TOTAL %d PASSED %d FAILED %d\n", checks, checks - failures, failures);
    return failures ? 1 : 0;
}
