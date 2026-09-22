// Exercises the actual panel implementation with in-memory preference I/O.
// Windows are hidden; no operator registry values, config, or restore files are changed.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdio>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>
#include "config.h"
#include "gpu_cuda.h"
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
// The background GPU TestAdapters reports. A test makes it an AMD card to prove that a non-NVIDIA
// target leaves CUDA alone and says nothing (founder decision 18).
const std::wstring amdKey = L"1002&164E&164E1002";
std::wstring bgAdapterKey = bgKey;
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
// The machine TestAdapters reports: two different cards (the default), ONE card, or two cards reporting the same
// adapter key. The last two are the ones the plan refuses, so the panel has no target and no main GPU.
enum class AdapterShape { TwoCards, OneCard, TwinCards };
AdapterShape adapterShape = AdapterShape::TwoCards;
bool TestAdapters(std::vector<cd::GpuAdapter>& out, std::wstring* error = nullptr) {
    out.clear();
    cd::GpuAdapter a; a.name = L"Main test GPU"; a.adapterKey = mainKey; a.hasDisplay = true;
    out.push_back(a);
    if (adapterShape == AdapterShape::TwinCards) {
        a.hasDisplay = false;
        out.push_back(a);
    } else if (adapterShape == AdapterShape::TwoCards) {
        a.name = L"Background test GPU"; a.adapterKey = bgAdapterKey; a.hasDisplay = false;
        out.push_back(a);
    }
    if (error) error->clear();
    return true;
}
// Run inside TestWrite after its own comparison, just before stillAllowed is evaluated: where another program's write
// lands in the sibling-pin race v0.5.7 closes. Empty: nothing happens.
std::function<void()> duringWrite;
// GuardedWriteGpuPreference's order, in memory: compare, (another program), the caller's check, then the change.
cd::GuardedWriteResult TestWrite(const std::wstring& path, bool present, const std::wstring& before,
                                bool remove, const std::wstring& value, unsigned long* error = nullptr,
                                cd::GuardedWriteHook = nullptr, void* = nullptr,
                                cd::GuardedWriteCheck stillAllowed = nullptr, void* checkContext = nullptr) {
    std::wstring live;
    if (error) *error = 0;
    if (TestRead(path, live) != present || live != before) return cd::GuardedWriteResult::Changed;
    if (duringWrite) duringWrite();
    if (stillAllowed && !stillAllowed(checkContext)) return cd::GuardedWriteResult::NoLongerAllowed;
    // [M] txprobe2: a write to this same value after the comparison makes the commit fail with 6704 - never lost here either
    if (TestRead(path, live) != present || live != before) {
        if (error) *error = 6704;
        return cd::GuardedWriteResult::NotCommitted;
    }
    if (remove) preferences.erase(path); else preferences[path] = value;
    return cd::GuardedWriteResult::Written;
}
bool TestRecord(cd::GpuRestoreJournal&, const cd::GpuPreferenceBefore&) { return true; }
// No restore file on disk: Apply is driven end to end here, and the real BeginGpuRestore would write into TestConfigDir.
bool TestBegin(const std::wstring& dir, const std::vector<cd::GpuPreferenceBefore>&, cd::GpuRestoreJournal& journal) {
    journal = cd::GpuRestoreJournal();
    journal.pendingPath = dir + L"\\gpu-preferences-before-panel-test.pending";
    journal.regPath = dir + L"\\gpu-preferences-before-panel-test.reg";
    return true;
}
bool TestFinish(const cd::GpuRestoreJournal&) { return true; }
std::vector<std::wstring> TestUnfinished(const std::wstring&) { return {}; }
std::wstring TestConfigDir() { return L"C:\\GameOptimizerPanelTest"; }
void TestLog(const wchar_t*, ...) {}
// Every message box's text, in order; a Yes/No question gets `yesNoAnswer` (No unless a test says otherwise).
std::vector<std::wstring> messages;
int yesNoAnswer = IDNO;
int WINAPI TestMessage(HWND, LPCWSTR text, LPCWSTR, UINT flags) {
    messages.push_back(text ? std::wstring(text) : std::wstring());
    return (flags & MB_YESNO) ? yesNoAnswer : IDOK;
}
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

// ---- A whole NVIDIA driver in memory (v0.5.8) ---------------------------------------------------
//
// The panel's CUDA half is driven end to end through this: no nvapi64.dll is opened, no driver session
// exists, and NOTHING of the operator's real NVIDIA settings is read or written. It is the same seam the
// rest of this file uses for the registry - the OS boundary is a function object, substituted here.
const std::wstring kId5090 = L"id,2.0:2B8510DE,00000100,GF - (432,2,161,32607) @ (0)";
const std::wstring kId4090 = L"id,2.0:268410DE,00000300,GF - (400,2,161,23028) @ (0)";
// 🔴 THE SESSION AND THE DATABASE ARE TWO MAPS, because NvAPI_DRS_SaveSettings writes the WHOLE open
// session rather than one setting. A fake that committed each write on its own could not show the
// failure the Council found (F6): a row whose own save failed is still in the session, and the NEXT
// row's save puts it in the database with no restore record naming it.
struct FakeCuda {
    bool available = true;
    cd::CudaRefusal openRefusal = cd::CudaRefusal::NoNvidiaDriver;
    int opened = 0, saves = 0, creates = 0, closes = 0, deletes = 0, writes = 0;
    // What CreateApplication answers. 🔴 IT IS ASKED AFTER THE PROFILE IS MADE, which is where the real
    // -167 arrives - so a test that sets it also exercises E7's clean-up of the profile made moments before.
    cd::CudaCreate createAnswer = cd::CudaCreate::Created;
    std::map<std::wstring, cd::CudaProfile> byExe;    // lower-case exe path -> its profile
    std::set<std::wstring> profiles;                  // every profile name the database holds, empty ones too
    std::map<std::wstring, std::wstring> settings;    // THE SESSION: profile name -> 0x10354FF8
    // 🔴 R5-1: settings on a profile that this product did NOT write - what a user sets in NVIDIA Control
    // Panel, which NVIDIA keeps in the same profile. Deleting the profile would take them with it.
    std::map<std::wstring, size_t> otherSettings;     // profile name -> settings we did not write
    // 🔴 R6-1: profiles whose CUDA value READS but is not that profile's own - NvAPI_DRS_GetSetting
    // answering from NVIDIA's general settings. They enumerate WITHOUT 0x10354FF8.
    std::set<std::wstring> inherited;
    bool settingsListFails = false;   // R6-1: NvAPI_DRS_EnumSettings refuses
    bool noEnumSettings = false;      // R6-2: this driver does not offer it, so the operation is EMPTY
    bool noDelete = false;            // R6-2: nor the delete calls, so THAT operation is EMPTY too
    std::map<std::wstring, std::wstring> saved;       // THE DATABASE: only what a save really committed
    std::vector<std::wstring> written;                // "<profile>=<value>" or "<profile>=CLEAR"
    bool lookupFails = false;    // the driver will not say whether it has a profile for the application
    bool nameLookupFails = false;      // R5-2: FindProfileByName answers Failed
    bool nameAppsIncomplete = false;   // R5-2: a by-name membership the driver would not finish listing
    bool readFails = false;      // the driver will not say what the setting is now
    bool saveFails = false;      // every save refuses
    bool deleteFails = false;    // R5-4: the driver offers the delete and refuses it
    int failSaveNumber = 0;      // or only the Nth one does, counting from 1
    int failWriteNumber = 0;     // the Nth write refuses, counting from 1 - a rollback's write is a write
    bool twinCards = false;      // two cards reporting one adapter key, which cannot be told apart
    // The background card is the only NVIDIA GPU the driver lists, so the target implies NVIDIA's own
    // "nothing is excluded" instead of an id string - a second, different value for the SAME target.
    bool oneCard = false;
};
FakeCuda cuda;
// THE ONE RECORD, in memory (gpu_cuda.h, D1): what ReadCudaRecord answers, and what the panel leaves in it.
cd::CudaRecord cudaRecord;
bool cudaRecordFails = false;   // every write to the record refuses
int cudaSaved = 0, cudaForgot = 0;

void ResetCuda() {
    cuda = FakeCuda();
    cuda.settings.clear();
    cudaRecord = cd::CudaRecord();
    cudaRecord.state = cd::CudaRecordState::Ok;
    cudaRecord.path = L"C:\\GameOptimizerPanelTest\\gpu-cuda-record.txt";
    cudaRecordFails = false;
    cudaSaved = 0;
    cudaForgot = 0;
}

// One line in the record, as ReadCudaRecord would have parsed it.
void AddCudaRecord(const cd::CudaRecordRow& row) { cudaRecord.rows.push_back(row); }

// NVDRS_PROFILE::numOfSettings as the driver reports it: our own CUDA setting plus anybody else's (R5-1).
size_t SettingsOn(const std::wstring& profileName) {
    size_t n = cuda.settings.count(profileName) != 0 ? 1u : 0u;
    const auto it = cuda.otherSettings.find(profileName);
    if (it != cuda.otherSettings.end()) n += it->second;
    return n;
}

// 🔴 R6-1: what NvAPI_DRS_EnumSettings would list - the profile's OWN setting ids. Not the same thing as
// the count above, which is exactly why round 6 exists.
std::vector<unsigned long> SettingIdsOn(const std::wstring& profileName) {
    std::vector<unsigned long> ids;
    if (cuda.settings.count(profileName) != 0 && cuda.inherited.count(profileName) == 0)
        ids.push_back(cd::CudaSettingId());
    const auto it = cuda.otherSettings.find(profileName);
    const size_t n = it != cuda.otherSettings.end() ? it->second : 0;
    for (size_t k = 0; k < n; ++k) ids.push_back(0x20D0F3E6ul + static_cast<unsigned long>(k));
    return ids;
}

cd::CudaOps TestMakeCudaOps() {
    ++cuda.opened;
    cd::CudaOps ops;
    ops.available = cuda.available;
    ops.openRefusal = cuda.openRefusal;
    ops.listGpus = []() {
        std::vector<cd::NvidiaGpu> gpus;
        if (!cuda.oneCard) {
            cd::NvidiaGpu a; a.adapterKey = cuda.twinCards ? bgKey : mainKey; a.busId = 1; gpus.push_back(a);
        }
        cd::NvidiaGpu b; b.adapterKey = bgKey; b.busId = 3; gpus.push_back(b);
        return gpus;
    };
    ops.listIds = []() {
        std::vector<std::wstring> ids;
        ids.push_back(L"autoselect");
        ids.push_back(kId5090);
        ids.push_back(kId4090);
        return ids;
    };
    ops.findProfileForExe = [](const std::wstring& exePath, cd::CudaProfile& profile) {
        profile = cd::CudaProfile();
        if (cuda.lookupFails) return cd::CudaLookup::Failed;
        const auto it = cuda.byExe.find(cd::ToLower(exePath));
        if (it == cuda.byExe.end()) return cd::CudaLookup::Absent;
        profile = it->second;
        profile.numSettings = SettingsOn(profile.profileName);   // R5-1, as GetProfileInfo reports it
        return cd::CudaLookup::Found;
    };
    // 🔴 R5-2: what the driver says about a profile asked for BY NAME - the seam the adopt ruling reaches.
    ops.findProfileByName = [](const std::wstring& profileName, cd::CudaProfile& profile) {
        profile = cd::CudaProfile();
        if (cuda.nameLookupFails || profileName.empty()) return cd::CudaLookup::Failed;
        if (cuda.profiles.count(profileName) == 0) return cd::CudaLookup::Absent;
        profile.profileName = profileName;
        size_t apps = 0;
        for (auto it = cuda.byExe.begin(); it != cuda.byExe.end(); ++it) {
            if (!cd::IEquals(it->second.profileName, profileName)) continue;
            apps += 1 + it->second.otherApps;
            if (it->second.isPredefined) profile.isPredefined = true;
        }
        profile.otherApps = apps;                  // nothing is excluded: none of them can be ours
        profile.appsComplete = !cuda.nameAppsIncomplete;
        profile.numSettings = SettingsOn(profileName);
        return cd::CudaLookup::Found;
    };
    ops.readSetting = [](const std::wstring& profile, std::wstring& value) {
        value.clear();
        if (cuda.readFails) return cd::CudaRead::Failed;
        const auto it = cuda.settings.find(profile);
        if (it == cuda.settings.end()) return cd::CudaRead::Absent;
        value = it->second;
        return cd::CudaRead::Value;
    };
    ops.writeSetting = [](const std::wstring& profile, bool clear, const std::wstring& value) {
        ++cuda.writes;
        if (cuda.writes == cuda.failWriteNumber) return false;
        cuda.written.push_back(profile + L"=" + (clear ? std::wstring(L"CLEAR") : value));
        if (clear) cuda.settings.erase(profile);
        else cuda.settings[profile] = value;
        return true;
    };
    // The live one looks the name up, makes the profile only when there is none, refuses to touch one it
    // was not told it may adopt (E1), and takes a profile it just made away again when the application
    // will not go into it (E7).
    // 🔴 R6-12: `adoptName` is the entry the caller ruled on, EMPTY when there is none - and the live one
    // opens exactly that name because NvAPI_DRS_FindProfileByName has case ([M] probe S23).
    ops.createProfileForExe = [](const std::wstring& exePath, const std::wstring& adoptName,
                                 cd::CudaProfile& made) {
        ++cuda.creates;
        const bool reuseNamed = !adoptName.empty();
        const std::wstring name = reuseNamed ? adoptName : cd::CudaProfileNameFor(exePath);
        const bool exists = cuda.profiles.count(name) != 0;
        if (exists && !reuseNamed) return cd::CudaCreate::NameTaken;
        bool created = false;
        if (!exists) {
            cuda.profiles.insert(name);
            created = true;
        }
        // 🔴 `made` IS FILLED EVEN WHEN THE ANSWER IS NOT Created, exactly as the live one fills it: a
        // profile this call made is a change to the driver whatever the application call answered, and
        // taking it away again is the CALLER's decision (E7).
        made = cd::CudaProfile();
        made.profileName = name;
        made.appEntry = cd::CudaAppKeyFor(exePath);
        made.createdNow = created;
        if (cuda.createAnswer != cd::CudaCreate::Created) return cuda.createAnswer;
        cuda.byExe[cd::ToLower(exePath)] = made;
        return cd::CudaCreate::Created;
    };
    // The live one re-reads the profile and refuses unless all four guards hold (gpu_cuda.cpp,
    // DeleteOwnProfile); this one answers the same way from the maps above - including the fourth guard,
    // that the ONE application it covers is the entry it was handed.
    ops.deleteProfile = [](const std::wstring& profileName, const std::wstring& appEntry) {
        ++cuda.deletes;
        if (cuda.deleteFails) return false;
        if (!cd::IsCudaProfileWeMade(profileName) || appEntry.empty()) return false;
        if (cuda.profiles.count(profileName) == 0) return false;
        size_t apps = 0, others = 0;
        bool predefined = false;
        for (auto it = cuda.byExe.begin(); it != cuda.byExe.end(); ++it) {
            if (it->second.profileName != profileName) continue;
            ++apps;
            others += it->second.otherApps;
            if (!cd::IEquals(it->second.appEntry, appEntry)) ++others;
            if (it->second.isPredefined) predefined = true;
        }
        if (predefined || apps > 1 || others > 0) return false;
        // 🔴 R5-1 / R6-1, GUARD 5, as gpu_cuda.cpp's DeleteOwnProfile now refuses it too: the entry's own
        // setting IDS are enumerated and nothing this product did not write may be among them.
        if (cuda.settingsListFails || cuda.noEnumSettings) return false;
        const std::vector<unsigned long> ids = SettingIdsOn(profileName);
        for (size_t i = 0; i < ids.size(); ++i)
            if (ids[i] != cd::CudaSettingId()) return false;
        for (auto it = cuda.byExe.begin(); it != cuda.byExe.end();) {
            if (it->second.profileName == profileName) cuda.byExe.erase(it++);
            else ++it;
        }
        cuda.settings.erase(profileName);
        cuda.profiles.erase(profileName);
        return true;
    };
    // 🔴 R6-1: the setting ids, which is what the delete decision is taken on.
    ops.listSettingIds = [](const std::wstring& profileName, std::vector<unsigned long>& ids) {
        ids.clear();
        if (cuda.settingsListFails) return false;
        ids = SettingIdsOn(profileName);
        return true;
    };
    ops.save = []() {
        ++cuda.saves;
        if (cuda.saveFails || cuda.saves == cuda.failSaveNumber) return false;
        cuda.saved = cuda.settings;   // the whole session, which is what the driver's own save does
        return true;
    };
    ops.close = []() { ++cuda.closes; };
    // 🔴 R6-2: THE SAME ONE GATE MakeCudaOps USES. Every operation above is installed as though its entry
    // points resolved, and this takes away the ones whose entry points did not - so what this file drives
    // is the product's own wiring rather than an `if` of its own.
    cd::CudaDriverCalls calls;
    calls.deleteProfile = !cuda.noDelete;
    calls.deleteApplication = !cuda.noDelete;
    calls.enumSettings = !cuda.noEnumSettings;
    cd::ApplyCudaDriverCalls(ops, calls);
    return ops;
}

cd::CudaRecord TestReadCudaRecord(const std::wstring&) { return cudaRecord; }

// The rewrite a real write does, without the file: the test's own copy moves with it, so a SECOND Apply
// or Remove in the same test reads what the first one left behind.
bool TestSetCudaRecordRows(cd::CudaRecord& record, const std::vector<cd::CudaRecordRow>& rows) {
    if (cudaRecordFails || record.state == cd::CudaRecordState::Unreadable) return false;
    record.rows = rows;
    record.state = cd::CudaRecordState::Ok;
    cudaRecord = record;
    return true;
}

bool TestSaveCudaRecordRow(cd::CudaRecord& record, cd::CudaRecordRow row) {
    ++cudaSaved;
    if (record.state == cd::CudaRecordState::Unreadable) return false;
    if (row.when.empty()) row.when = L"2026-09-20 12:00:00";
    if (!cd::CudaRecordRowIsWritable(row)) return false;
    return TestSetCudaRecordRows(record, cd::CudaRowsWith(record.rows, row));
}

bool TestForgetCudaRecordRow(cd::CudaRecord& record, const std::wstring& appEntry) {
    ++cudaForgot;
    if (record.state != cd::CudaRecordState::Ok) return false;
    const std::vector<cd::CudaRecordRow> kept = cd::CudaRowsWithout(record.rows, appEntry);
    if (kept.size() == record.rows.size()) return true;
    return TestSetCudaRecordRows(record, kept);
}
}

// Headers are already loaded. Redirect only this translation unit's OS boundary calls.
#define ReadGpuPreference TestRead
#define EnumerateGpuPreferenceEntries TestEntries
#define EnumerateGpuAdapters TestAdapters
#define GuardedWriteGpuPreference TestWrite
#define MakeCudaOps TestMakeCudaOps
#define ReadCudaRecord TestReadCudaRecord
#define SaveCudaRecordRow TestSaveCudaRecordRow
#define ForgetCudaRecordRow TestForgetCudaRecordRow
#define SetCudaRecordRows TestSetCudaRecordRows
#define RecordGpuRestoreRow TestRecord
#define BeginGpuRestore TestBegin
#define FinishGpuRestore TestFinish
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
#undef FinishGpuRestore
#undef BeginGpuRestore
#undef RecordGpuRestoreRow
#undef SetCudaRecordRows
#undef ForgetCudaRecordRow
#undef SaveCudaRecordRow
#undef ReadCudaRecord
#undef MakeCudaOps
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
        adapterShape = AdapterShape::TwoCards;
        duringWrite = nullptr; yesNoAnswer = IDNO; messages.clear();
        ResetCuda();
        cd::ActivateGpuPanel(panel, cfg, snap);
    };
    const auto index = [&](const std::wstring& path) {
        for (size_t i = 0; i < st->rows.size(); ++i) if (st->rows[i].r.exePath == path) return i;
        return st->rows.size();
    };
    // THE REAL MESSAGE PATHS: a button's click as Windows sends it, and a list line's Space and double-click.
    const auto press = [&](int id, HWND button) {
        SendMessageW(panel, WM_COMMAND, MAKEWPARAM(id, BN_CLICKED), reinterpret_cast<LPARAM>(button));
    };
    const auto lineOf = [&](const std::wstring& path) {
        const int count = static_cast<int>(SendMessageW(st->hList, LB_GETCOUNT, 0, 0));
        for (int item = 0; item < count; ++item)
            if (static_cast<size_t>(SendMessageW(st->hList, LB_GETITEMDATA, item, 0)) == index(path)) return item;
        return -1;
    };
    const auto space = [&](const std::wstring& path) {
        SendMessageW(panel, WM_VKEYTOITEM, MAKEWPARAM(VK_SPACE, lineOf(path)), reinterpret_cast<LPARAM>(st->hList));
    };
    const auto doubleClick = [&](const std::wstring& path) {
        SendMessageW(st->hList, LB_SETCURSEL, static_cast<WPARAM>(lineOf(path)), 0);
        SendMessageW(panel, WM_COMMAND, MAKEWPARAM(cd::IDC_GPU_LIST, LBN_DBLCLK), reinterpret_cast<LPARAM>(st->hList));
    };
    const auto statusText = [&]() {
        const int n = GetWindowTextLengthW(st->hStatus);
        std::wstring text(static_cast<size_t>(n > 0 ? n : 0) + 1, L'\0');
        text.resize(static_cast<size_t>(GetWindowTextW(st->hStatus, &text[0], n + 1)));
        return text;
    };
    const auto has = [](const std::wstring& text, const std::wstring& part) { return text.find(part) != std::wstring::npos; };
    const std::wstring autoRecheckRefusal = L"its automatic selection is no longer safe";
    const std::wstring raceRefusal = cd::GuardedReason(cd::GuardedWriteResult::NoLongerAllowed, 0);

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
    {
        // R4f (v0.5.7): refused by PrepareEdits' AUTOMATIC recheck, by name - not by the listed-choice comparison after it,
        // which would untick this row too. Without the recheck the older version, whose own value did not change, is ready.
        const std::wstring own = cd::Describe(st->rows[index(newPath)]) + L" - ";
        bool byRecheck = false, byListedChoice = false;
        for (size_t k = 0; k < refused.size(); ++k) {
            if (refused[k].compare(0, own.size(), own) == 0 && has(refused[k], autoRecheckRefusal)) byRecheck = true;
            if (has(refused[k], L"changed after the list was shown")) byListedChoice = true;
        }
        Check(byRecheck && !byListedChoice && ready.empty(),
              "own new main-GPU pin is refused by the automatic recheck, with its refusal text");
    }

    // R4c (v0.5.7): THE SIBLING-PIN RACE. Auto assign ticks the newer version; the older version is unticked by hand and has
    // no value, so PrepareEdits finds the newer one still eligible. Another program pins the older version to the main GPU
    // inside the newer one's guarded write, after its own comparison. Apply is pressed, and answered Yes.
    reset(); press(cd::IDC_GPU_BULK, st->hBulk);
    space(oldPath);
    Check(st->rows[index(newPath)].r.selected && st->rows[index(newPath)].autoSelected &&
          !st->rows[index(oldPath)].r.selected, "race setup: newer version ticked by Auto assign, older unticked");
    bool pinnedMidWrite = false;
    duringWrite = [&]() {
        if (pinnedMidWrite) return;
        pinnedMidWrite = true;
        preferences[oldPath] = cd::FormatPreferenceValue(mainKey);
    };
    yesNoAnswer = IDYES;
    press(cd::IDC_GPU_APPLY, st->hApply);
    duringWrite = nullptr; yesNoAnswer = IDNO;
    Check(pinnedMidWrite, "R1 race: the sibling was pinned inside the newer version's guarded write");
    Check(preferences.count(newPath) == 0, "R1 race: sibling pinned during the write is not overwritten");
    {
        const std::wstring result = messages.empty() ? std::wstring() : messages.back();
        const size_t notWritten = result.find(L"Not written:");
        const size_t line = result.find(cd::Describe(st->rows[index(newPath)]) + L" - " + raceRefusal);
        Check(messages.size() == 2 && has(result, L"0 of 1 assigned") && notWritten != std::wstring::npos &&
                  line != std::wstring::npos && line > notWritten && !has(result, autoRecheckRefusal),
              "R1 race: the result lists it as not written, with the in-write refusal");
    }
    Check(!st->rows[index(newPath)].r.selected && !st->rows[index(newPath)].autoSelected,
          "R1 race: the refused automatic tick is gone once its sibling holds a main-GPU pin");

    // CONTROL: the same race against a HAND tick - the user's override - is written, and not checked.
    reset();
    space(newPath);
    Check(st->rows[index(newPath)].r.selected && !st->rows[index(newPath)].autoSelected, "control setup: hand tick");
    pinnedMidWrite = false;
    duringWrite = [&]() {
        if (pinnedMidWrite) return;
        pinnedMidWrite = true;
        preferences[oldPath] = cd::FormatPreferenceValue(mainKey);
    };
    yesNoAnswer = IDYES;
    press(cd::IDC_GPU_APPLY, st->hApply);
    duringWrite = nullptr; yesNoAnswer = IDNO;
    Check(pinnedMidWrite && preferences.count(newPath) == 1 &&
              cd::PreferenceChoiceKey(preferences[newPath]) == bgKey && preferences[oldPath] == cd::FormatPreferenceValue(mainKey),
          "R1 control: a hand tick raced the same way is written (override kept)");
    Check(!messages.empty() && has(messages.back(), L"1 of 1 assigned") && !has(messages.back(), raceRefusal),
          "R1 control: the result says it was assigned");

    // R4d (v0.5.7): Auto assign's fresh read reaches the row. A listed application with no assignment is pinned to the main
    // GPU behind the tab's back; Auto assign is pressed, ticks nothing, and the status line must stop calling it movable.
    reset();
    const std::wstring listedBefore = st->rows[index(newPath)].listedChoice;
    Check(has(statusText(), L"can be moved to the GPU chosen above"), "R2 setup: both versions listed as movable");
    preferences[newPath] = cd::FormatPreferenceValue(mainKey);
    press(cd::IDC_GPU_BULK, st->hBulk);
    const std::wstring status = statusText();
    Check(has(status, L"pinned to the main GPU") && !has(status, L"can be moved to the GPU chosen above"),
          "R2: the status control says pinned to the main GPU, not movable, after Auto assign reads a new pin");
    Check(st->rows[index(newPath)].r.assignedKey == mainKey && st->rows[index(newPath)].listedChoice == listedBefore,
          "R2: the fresh read refreshes assignedKey and leaves listedChoice alone");
    Check(!IsWindowEnabled(st->hBulk) && !st->rows[index(newPath)].r.selected && !st->rows[index(oldPath)].r.selected,
          "R2: nothing ticked, and Auto assign is off with nothing left to move");
    Check(cd::ReadAutoChoices(st).size() == st->rows.size(), "Auto assign's answer has one entry per row");

    // R4e (v0.5.7): a double-click - LBN_DBLCLK through the panel - converts an automatic tick to a manual one, as Space does.
    reset(); press(cd::IDC_GPU_BULK, st->hBulk);
    doubleClick(newPath);
    Check(!st->rows[index(newPath)].r.selected, "double-click unticks the automatic tick");
    doubleClick(newPath);
    Check(st->rows[index(newPath)].r.selected && !st->rows[index(newPath)].autoSelected,
          "double-click untick/retick converts bulk tick to manual override");
    preferences[oldPath] = cd::FormatPreferenceValue(mainKey);
    ready.clear(); refused.clear();
    cd::PrepareEdits(st, false, ready, refused, changed);
    Check(ready.size() == 1 && ready[0].item.exePath == newPath && !ready[0].item.automatic,
          "double-clicked row is prepared as a hand tick despite the sibling pin");

    // R3 (v0.5.7): an untick takes the tick's origin with it - Deselect all ("Clear selection" until v0.5.9) leaves no
    // row marked as Auto assign's.
    reset(); press(cd::IDC_GPU_BULK, st->hBulk);
    press(cd::IDC_GPU_CLEARSEL, st->hClearSel);
    Check(!st->rows[index(newPath)].r.selected && !st->rows[index(newPath)].autoSelected &&
              !st->rows[index(oldPath)].r.selected && !st->rows[index(oldPath)].autoSelected,
          "R3: Deselect all unticks and clears automatic origin");

    // ---- v0.5.9: Select all -----------------------------------------------------------------------------------------
    //
    // FOUR MORE ROWS, ONE PER RULE, because this button's whole job is WHICH ROWS IT LEAVES ALONE, and the two sibling
    // rows the rest of this file uses cannot show that. They join the process snapshot and the config for this block
    // only and are taken out again at its end, so every later test sees the list it was written against.
    {
        const std::wstring ordinaryPath = L"C:\\GameOptimizerPanelTest\\Editor\\editor.exe";
        const std::wstring windowsPath = L"C:\\Windows\\System32\\panelsystemtest.exe";
        const std::wstring gamePath = L"C:\\GameOptimizerPanelTest\\Game\\mygame.exe";
        const std::wstring pinnedPath = L"C:\\GameOptimizerPanelTest\\Pinned\\pinned.exe";
        cd::ProcInfo extra;
        extra.pid = 70003; extra.name = L"editor.exe"; extra.fullPath = ordinaryPath; processes[extra.pid] = extra;
        extra.pid = 70004; extra.name = L"panelsystemtest.exe"; extra.fullPath = windowsPath; processes[extra.pid] = extra;
        extra.pid = 70005; extra.name = L"mygame.exe"; extra.fullPath = gamePath; processes[extra.pid] = extra;
        extra.pid = 70006; extra.name = L"pinned.exe"; extra.fullPath = pinnedPath; processes[extra.pid] = extra;
        cd::Profile profile;
        profile.name = L"SelectAllTest";
        profile.game = L"mygame.exe";
        cfg.profiles.push_back(profile);

        // A live main-GPU pin on pinned.exe, and one on the OLD sibling so the new one inherits the doubt.
        reset();
        preferences[pinnedPath] = cd::FormatPreferenceValue(mainKey);
        preferences[oldPath] = cd::FormatPreferenceValue(mainKey);
        Check(st->rows.size() == 6, "v0.5.9: six rows for the Select all cases");
        press(cd::IDC_GPU_SELALL, st->hSelAll);
        Check(st->rows[index(ordinaryPath)].r.selected,
              "v0.5.9: Select all ticks an ordinary listed application");
        Check(!st->rows[index(ordinaryPath)].autoSelected,
              "🔴 v0.5.9: and every tick it makes is a HAND tick, never Auto assign's");
        Check(!st->rows[index(windowsPath)].r.selected,
              "v0.5.9: Select all leaves a Windows image alone");
        Check(!st->rows[index(gamePath)].r.selected,
              "v0.5.9: Select all leaves a game the user has a profile for alone");
        Check(!st->rows[index(pinnedPath)].r.selected,
              "v0.5.9: Select all leaves an application pinned to the main GPU alone");
        Check(!st->rows[index(newPath)].r.selected && !st->rows[index(oldPath)].r.selected,
              "🔴 v0.5.9: and leaves one whose sibling install may hold such a pin alone");

        // THE TICK IS PREPARED AS A HAND TICK, which is what makes the rule above load-bearing: Apply's v0.5.7
        // re-checks are skipped for it, so nothing downstream would catch a row Select all should not have ticked.
        ready.clear(); refused.clear();
        cd::PrepareEdits(st, false, ready, refused, changed);
        Check(ready.size() == 1 && ready[0].item.exePath == ordinaryPath && !ready[0].item.automatic,
              "🔴 v0.5.9: a Select all tick reaches Apply as a hand tick, not an automatic one");

        // IT ADDS TO THE SELECTION AND NEVER CLEARS ONE: a row ticked by hand that the rule would not tick is the
        // user's own choice and survives the click.
        reset();
        preferences[pinnedPath] = cd::FormatPreferenceValue(mainKey);
        space(windowsPath);
        press(cd::IDC_GPU_SELALL, st->hSelAll);
        Check(st->rows[index(windowsPath)].r.selected,
              "v0.5.9: Select all does not untick a Windows image the user ticked by hand");

        // ENABLED STATES, both buttons, and Deselect all still clears everything.
        reset();
        preferences[pinnedPath] = cd::FormatPreferenceValue(mainKey);
        preferences[oldPath] = cd::FormatPreferenceValue(mainKey);
        cd::SyncButtons(st);
        Check(IsWindowEnabled(st->hSelAll) && !IsWindowEnabled(st->hClearSel),
              "v0.5.9: with rows to tick and nothing ticked, Select all is lit and Deselect all is grey");
        press(cd::IDC_GPU_SELALL, st->hSelAll);
        Check(!IsWindowEnabled(st->hSelAll) && IsWindowEnabled(st->hClearSel),
              "v0.5.9: once every eligible row is ticked Select all greys, and Deselect all lights");
        press(cd::IDC_GPU_CLEARSEL, st->hClearSel);
        Check(!st->rows[index(ordinaryPath)].r.selected && !st->rows[index(ordinaryPath)].autoSelected &&
                  IsWindowEnabled(st->hSelAll) && !IsWindowEnabled(st->hClearSel),
              "v0.5.9: Deselect all unticks everything, clears the origin, and the pair swaps back");

        // 🔴 NO TARGET, NO SELECT ALL (pre-publish review of v0.5.9). One GPU - the commonest machine - or two cards
        // reporting one adapter key: the plan refuses, so there is no target AND no main GPU. IsMainGpuPin is then
        // false for every row, and a preference the user set in Windows' Graphics settings reads as an ordinary row.
        // Select all must be grey, and a click that reaches it anyway must tick nothing - otherwise Remove assignment
        // lights up and one Yes deletes those preferences in bulk.
        const AdapterShape noTargetShapes[2] = {AdapterShape::OneCard, AdapterShape::TwinCards};
        const std::string noTargetNames[2] = {"one GPU", "two cards reporting one adapter key"};
        for (int k = 0; k < 2; ++k) {
            const std::string on = " (" + noTargetNames[k] + ")";
            reset();
            adapterShape = noTargetShapes[k];
            preferences[pinnedPath] = cd::FormatPreferenceValue(mainKey);   // the user's own Windows preference
            cd::ActivateGpuPanel(panel, cfg, snap);
            Check(st->targetKey.empty() && st->plan.gameKey.empty() && !IsWindowEnabled(st->hApply),
                  ("v0.5.9 control: the plan refuses, so there is no target, no main GPU and no Apply" + on).c_str());
            Check(cd::SelectablePendingCount(st) > 0 && !st->rows[index(pinnedPath)].r.assignedKey.empty(),
                  ("v0.5.9 control: rows the row rule alone WOULD tick, the user's preference among them" + on).c_str());
            Check(!IsWindowEnabled(st->hSelAll),
                  ("🔴 v0.5.9: with no target Select all is grey, exactly as Apply is" + on).c_str());
            press(cd::IDC_GPU_SELALL, st->hSelAll);
            bool anyTicked = false;
            for (size_t i = 0; i < st->rows.size(); ++i) anyTicked = anyTicked || st->rows[i].r.selected;
            Check(!anyTicked && !IsWindowEnabled(st->hRemove),
                  ("🔴 v0.5.9: and a click that reaches it ticks nothing, so Remove stays grey" + on).c_str());
        }

        processes.erase(70003); processes.erase(70004); processes.erase(70005); processes.erase(70006);
        cfg.profiles.pop_back();
        reset();
        Check(st->rows.size() == 2, "v0.5.9: the Select all rows are gone again");
    }

    // ---- v0.5.9: THE BUTTON ROW FITS, MEASURED ON THE REAL CONTROLS ------------------------------------------------
    //
    // 🔴 THE CHECK THIS HARNESS HAS NEVER HAD. Every other assertion in this file reads state; none reads GEOMETRY, and
    // the button row had no overflow guard and no test. [M] Adding a fourth Dp(116) button to the three that were
    // there needed 124 px of the 96 free at the panel's MINIMUM width (824 px at 96 dpi) - 28 px of overlap at 96 dpi
    // and 62 at 192 - while fitting with 92 px to spare at the DEFAULT width, which is why looking at the panel would
    // have passed it.
    {
        RECT before;
        GetWindowRect(panel, &before);
        MapWindowPoints(nullptr, parent, reinterpret_cast<POINT*>(&before), 2);
        const int dpi = st->dpi;
        const int GT = cd::theme::Dp(8, dpi);
        // Settings' own minimum client width less the page gap and the card padding either side - the narrowest the
        // panel is ever laid out at (settings.cpp: Dp(880), theme::metric::kGap, theme::metric::kCardPad).
        const int minPanelW = cd::theme::Dp(880, dpi) - 2 * cd::theme::Dp(12, dpi) - 2 * cd::theme::Dp(16, dpi);
        const int panelH = cd::theme::Dp(520, dpi);

        // Every control of the row, left to right as it is laid out, in the panel's own client coordinates.
        HWND row[6] = { st->hBulk, st->hSelAll, st->hClearSel, st->hRemove, st->hApply, st->hClose };
        const auto measure = [&](int width, RECT* out) {
            MoveWindow(panel, 0, 0, width, panelH, TRUE);
            for (int i = 0; i < 6; ++i) {
                GetWindowRect(row[i], &out[i]);
                MapWindowPoints(nullptr, panel, reinterpret_cast<POINT*>(&out[i]), 2);
            }
        };
        const auto noOverlap = [](const RECT* r) {
            for (int i = 0; i + 1 < 6; ++i)
                for (int j = i + 1; j < 6; ++j)
                    if (r[i].right > r[j].left && r[j].right > r[i].left) return false;
            return true;
        };

        RECT r[6];
        measure(minPanelW, r);
        Check(r[3].right <= r[4].left - GT,
              "🔴 v0.5.9 layout: at the panel's minimum width the left group ends a full gap before Apply");
        Check(noOverlap(r), "🔴 v0.5.9 layout: at the minimum width no two buttons of the row overlap");
        // WITHOUT THIS THE TWO ABOVE PASS ON A TRUNCATED ROW. The hard clamp cuts a button that does not fit rather
        // than letting it overlap, so an arithmetic that stopped sharing the remainder would still look clean: the
        // tell is that Remove would come out narrower than Auto assign. The two wide buttons always share one width
        // and the two selection buttons share another.
        Check(r[0].right - r[0].left == r[3].right - r[3].left,
              "🔴 v0.5.9 layout: Auto assign and Remove assignment keep the SAME width - neither is cut to fit");
        Check(r[1].right - r[1].left == r[2].right - r[2].left &&
                  r[1].right - r[1].left >= cd::theme::Dp(76, dpi),
              "🔴 v0.5.9 layout: Select all and Deselect all share one width, above the readable floor");

        // AND A WIDTH NARROWER THAN ANYTHING SETTINGS WILL HAND US, where the readable floor wins and only the hard
        // clamp is left. Captions ellipsize (theme::DrawButton); buttons do not slide under Apply.
        measure(cd::theme::Dp(400, dpi), r);
        Check(r[3].right <= r[4].left - GT,
              "🔴 v0.5.9 layout: below every real width the hard clamp still keeps the left group off Apply");
        Check(noOverlap(r), "🔴 v0.5.9 layout: and no two buttons of the row overlap there either");

        MoveWindow(panel, before.left, before.top, before.right - before.left, before.bottom - before.top, TRUE);
    }

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


    // ---- v0.5.8: GPU Assignment also sets the application's CUDA GPU ----------------------------
    //
    // Driven through the real Apply and Remove, against the in-memory driver above. Every case here is
    // one the ledger names, and each one checks BOTH halves: what the driver was told, and what the
    // dialogs said about it.
    //
    // 🔴 ROUND 3 NARROWED THE FEATURE RATHER THAN GUARDING THE UNDO A THIRD TIME
    // (E1-E9; founder decision 22). NVIDIA's CUDA setting
    // belongs to a PROFILE and a profile can cover many applications, so this product now writes only on
    // an entry it created itself, for one application - and the undo is taking that entry away again.
    const std::wstring madeProfile = cd::CudaProfileNameFor(newPath);
    const std::wstring madeEntry = cd::CudaAppKeyFor(newPath);
    const auto line = [&](const std::wstring& profile, const std::wstring& entry, const std::wstring& lastWrote) {
        cd::CudaRecordRow row;
        row.profileName = profile;
        row.appEntry = entry;
        row.lastWrote = lastWrote;
        row.when = L"2026-09-20 12:00:00";
        return row;
    };
    // The driver already keeps `profile` for `exe`, under the application entry `entry`.
    const auto own = [&](const std::wstring& exe, const std::wstring& profile, const std::wstring& entry,
                         bool predefined, size_t otherApps) {
        cd::CudaProfile p;
        p.profileName = profile;
        p.appEntry = entry;
        p.isPredefined = predefined;
        p.otherApps = otherApps;
        cuda.byExe[cd::ToLower(exe)] = p;
        cuda.profiles.insert(profile);
    };
    const auto removeReady = [&](const std::wstring& path) {
        preferences[path] = cd::FormatPreferenceValue(bgKey);
        cd::ActivateGpuPanel(panel, cfg, snap);
        space(path);
        yesNoAnswer = IDYES;
    };

    // E1 FIRST CASE: NVIDIA has no entry for it, so one of ours is made, written, recorded and saved -
    // and the record gets ONE line, which is the entry, the value and the time (E2).
    reset();
    space(newPath);
    yesNoAnswer = IDYES;
    press(cd::IDC_GPU_APPLY, st->hApply);
    Check(preferences.count(newPath) == 1 && cd::PreferenceChoiceKey(preferences[newPath]) == bgKey,
          "CUDA: the GPU preference itself was still written");
    Check(cuda.settings.size() == 1 && cuda.settings[madeProfile] == kId5090 && cuda.saves == 1,
          "E1: an NVIDIA target excludes the other NVIDIA card from CUDA, and saves once");
    Check(cuda.creates == 1 && cuda.closes == 1, "E1: one entry made for it, and the driver session closed");
    Check(cudaRecord.rows.size() == 1 && cudaRecord.rows[0].profileName == madeProfile &&
              cudaRecord.rows[0].appEntry == madeEntry && cudaRecord.rows[0].lastWrote == kId5090 &&
              !cudaRecord.rows[0].when.empty(),
          "E2: one line for it - the entry, the value written, and when");
    Check(messages.size() == 2 &&
              has(messages[0], L"This also asks NVIDIA to use Background test GPU for the CUDA work of any of "
                               L"them that use CUDA.") &&
              has(messages[0], L"is left to NVIDIA Control Panel"),
          "E9: the question says which GPU CUDA is asked for, and what it will not do");
    Check(has(messages.back(), L"1 of them will use Background test GPU for CUDA as well.") &&
              has(messages.back(), cudaRecord.path),
          "E9: the result says what CUDA was set to, and where the record is");

    // A SECOND Apply on our own entry moves the line's value and makes nothing new.
    preferences.erase(newPath);
    cd::ActivateGpuPanel(panel, cfg, snap);
    space(newPath);
    yesNoAnswer = IDYES;
    press(cd::IDC_GPU_APPLY, st->hApply);
    Check(cuda.creates == 1 && cudaRecord.rows.size() == 1,
          "E2: a second Apply on our own entry writes one line, not two");

    // 🔴 E1 THIRD CASE, AND THE HEART OF v0.5.8: NVIDIA'S OWN ENTRY IS NOT WRITTEN. The Windows GPU
    // assignment still happens; the result names the entry, counts the other programs and sends the user
    // to NVIDIA Control Panel.
    reset();
    own(newPath, L"Google Chrome", L"chat.exe", true, 2);
    cuda.settings[L"Google Chrome"] = kId4090;
    space(newPath);
    yesNoAnswer = IDYES;
    press(cd::IDC_GPU_APPLY, st->hApply);
    Check(preferences.count(newPath) == 1 && cuda.creates == 0 && cuda.written.empty() && cuda.saves == 0 &&
              cuda.settings[L"Google Chrome"] == kId4090 && cudaRecord.rows.empty(),
          "E1: a profile NVIDIA owns is never written on, and the GPU assignment still happens");
    Check(has(messages.back(), L"Which GPU CUDA uses was not changed for:") &&
              has(messages.back(), L"\"Google Chrome\"") &&
              has(messages.back(), L"2 other programs as well") &&
              has(messages.back(), L"NVIDIA Control Panel"),
          "E9: and the result names the entry, counts its programs and says where to set it");
    Check(!has(messages.back(), L"1 of them will use Background test GPU for CUDA as well."),
          "E1: and it claims no CUDA change for that row");

    // E1: an entry the USER made, with a second member - not predefined, and still not ours.
    reset();
    own(newPath, L"My own profile", madeEntry, false, 1);
    space(newPath);
    yesNoAnswer = IDYES;
    press(cd::IDC_GPU_APPLY, st->hApply);
    Check(cuda.written.empty() && cuda.creates == 0 && cudaRecord.rows.empty(),
          "E1: an entry with another program on it is not written on either");
    Check(has(messages.back(), L"\"My own profile\"") && has(messages.back(), L"1 other program as well"),
          "E9: one other program is counted in the singular");

    // E1: an entry carrying OUR OWN NAME that no line of ours claims - a run that died, or a user's hand.
    reset();
    cuda.profiles.insert(madeProfile);
    space(newPath);
    yesNoAnswer = IDYES;
    press(cd::IDC_GPU_APPLY, st->hApply);
    Check(preferences.count(newPath) == 1 && cuda.written.empty() && cuda.byExe.empty() &&
              cuda.profiles.count(madeProfile) == 1 && cudaRecord.rows.empty(),
          "E1: an entry with our name that no record claims is left completely alone");
    Check(has(messages.back(), cd::CudaRefusalReason(cd::CudaRefusal::ProfileNameTaken)),
          "E1: and the result says the name is taken by something it did not make");

    // E1: a membership the driver would not finish listing - no count is claimed for it.
    reset();
    own(newPath, L"Microsoft Edge Beta", madeEntry, false, 1);
    cuda.byExe[cd::ToLower(newPath)].appsComplete = false;
    space(newPath);
    yesNoAnswer = IDYES;
    press(cd::IDC_GPU_APPLY, st->hApply);
    Check(preferences.count(newPath) == 1 && cuda.written.empty() && cuda.saves == 0,
          "E1: an entry whose members could not all be listed is never written on");
    Check(has(messages.back(), cd::CudaRefusalReason(cd::CudaRefusal::MembershipUnknown)) &&
              !has(messages.back(), L"other programs as well"),
          "E1: and half a count is never shown as if it were the whole one");

    // E1 / E7: -167 arrives after the profile was made, so the profile goes again and nothing is left.
    reset();
    cuda.createAnswer = cd::CudaCreate::AlreadyInUse;
    space(newPath);
    yesNoAnswer = IDYES;
    press(cd::IDC_GPU_APPLY, st->hApply);
    Check(preferences.count(newPath) == 1 && cuda.settings.empty() && cuda.saves == 0 && cudaRecord.rows.empty(),
          "E1: a name another entry claims writes no CUDA setting and never fails the GPU assignment");
    Check(cuda.profiles.empty() && cuda.deletes == 1,
          "E7: and the profile made moments before is taken away again, so none is left behind");
    Check(has(messages.back(), L"Which GPU CUDA uses was not changed for:") &&
              has(messages.back(), cd::CudaRefusalReason(cd::CudaRefusal::NameAlreadyInUse)),
          "E1: and the result says so, by name, in one sentence");

    // E5 / the record: a line that cannot be written takes the change back out and stops the CUDA half.
    reset();
    cudaRecordFails = true;
    space(newPath);
    space(oldPath);
    yesNoAnswer = IDYES;
    press(cd::IDC_GPU_APPLY, st->hApply);
    Check(preferences.count(newPath) == 1 && preferences.count(oldPath) == 1,
          "setup: both GPU preferences were still written");
    Check(cuda.settings.empty() && cuda.saves == 0 && cuda.byExe.empty() && cuda.profiles.empty() &&
              cudaRecord.rows.empty(),
          "a CUDA change whose line could not be written is taken back out, and never saved");
    Check(has(messages.back(), cd::CudaRefusalReason(cd::CudaRefusal::NotRecorded)) &&
              has(messages.back(), L"not tried: NVIDIA's settings could not be finished for an application "
                                   L"before it"),
          "the result says why, and that the CUDA half stopped");

    // A row whose GPU preference did NOT land is never given a CUDA setting.
    reset();
    space(newPath);
    {
        bool bumped = false;
        duringWrite = [&]() {
            if (bumped) return;
            bumped = true;
            preferences[newPath] = L"AppStatus=1;";   // another program, inside the guarded write
        };
        yesNoAnswer = IDYES;
        press(cd::IDC_GPU_APPLY, st->hApply);
        duringWrite = nullptr;
        Check(bumped && cuda.settings.empty() && cuda.saves == 0 && cudaRecord.rows.empty(),
              "a refused GPU write drags no CUDA change with it");
    }

    // Decision 18: a target that is not an NVIDIA card does nothing, says nothing, and NVIDIA's own
    // settings are never even opened.
    bgAdapterKey = amdKey;
    reset();
    Check(st->targetKey == amdKey, "setup: the picker's target is the AMD GPU");
    space(newPath);
    yesNoAnswer = IDYES;
    press(cd::IDC_GPU_APPLY, st->hApply);
    Check(preferences.count(newPath) == 1 && cuda.opened == 0 && cuda.settings.empty() && cuda.creates == 0,
          "a non-NVIDIA target never opens NVIDIA's settings at all");
    Check(messages.size() == 2 && !has(messages[0], L"CUDA") && !has(messages[1], L"CUDA"),
          "and nothing about CUDA is said, in the question or the result");
    bgAdapterKey = bgKey;

    // No NVIDIA driver at all.
    reset();
    cuda.available = false;
    cuda.openRefusal = cd::CudaRefusal::NoNvidiaDriver;
    space(newPath);
    yesNoAnswer = IDYES;
    press(cd::IDC_GPU_APPLY, st->hApply);
    Check(preferences.count(newPath) == 1 && cuda.settings.empty(),
          "with no NVIDIA driver the GPU assignment still happens");
    Check(!has(messages[0], L"CUDA"), "the question promises nothing it cannot do");
    Check(has(messages.back(), cd::CudaRefusalReason(cd::CudaRefusal::NoNvidiaDriver)),
          "and the result says it once, for the whole run");

    // The check box off - nothing CUDA-related happens, and the driver is not even opened.
    cfg.setCudaGpu = false;
    reset();
    space(newPath);
    yesNoAnswer = IDYES;
    press(cd::IDC_GPU_APPLY, st->hApply);
    Check(preferences.count(newPath) == 1 && cuda.opened == 0 && cuda.settings.empty(),
          "with the setting off, NVIDIA is never asked anything");
    Check(!has(messages[0], L"CUDA") && !has(messages[1], L"CUDA"),
          "and nothing about CUDA is said either");
    cfg.setCudaGpu = true;

    // ---- E3: Remove takes the entry away, and only while it is still ours ------------------------
    //
    // 🔴 THE ROUND-2 BLOCKER, THROUGH THE REAL APPLY AND REMOVE. Assign to one NVIDIA card, then to the
    // other, then Remove. The per-run journal compared the driver against what the FIRST Apply wrote, saw
    // the SECOND Apply's value, and announced that somebody else had changed it.
    reset();
    space(newPath);
    yesNoAnswer = IDYES;
    press(cd::IDC_GPU_APPLY, st->hApply);
    Check(cuda.settings[madeProfile] == kId5090 && cudaRecord.rows.size() == 1,
          "setup: the first Apply made our entry and recorded the value it wrote");
    // The machine loses its other NVIDIA card, so the same target now means NVIDIA's own "nothing is
    // excluded" rather than an id - a second, different value of ours for the same target.
    preferences.erase(newPath);
    cuda.oneCard = true;
    cd::ActivateGpuPanel(panel, cfg, snap);
    space(newPath);
    yesNoAnswer = IDYES;
    press(cd::IDC_GPU_APPLY, st->hApply);
    Check(cuda.settings[madeProfile] == cd::CudaNoneValue() && cudaRecord.rows.size() == 1 &&
              cudaRecord.rows[0].lastWrote == cd::CudaNoneValue(),
          "E2: a second Apply moves the line's value, and there is still exactly one line");
    removeReady(newPath);
    press(cd::IDC_GPU_REMOVE, st->hRemove);
    Check(cuda.settings.empty() && cuda.profiles.empty() && cuda.byExe.empty() && cudaRecord.rows.empty(),
          "E3: Remove after two Applies takes the whole entry away and spends the line");
    Check(preferences.count(newPath) == 0, "E3: and the Windows assignment goes with it");
    Check(has(messages.back(), L"Which GPU CUDA uses was put back for 1 of them.") &&
              !has(messages.back(), cd::CudaRefusalReason(cd::CudaRefusal::ChangedSinceWritten)),
          "E3: and it is not reported as a change somebody else made");
    // The question is the message before the result: this test has already asked two Apply questions.
    Check(messages.size() >= 2 &&
              has(messages[messages.size() - 2],
                  L"takes away the NVIDIA settings entry Game Optimizer made for 1 of them"),
          "E9: and the question said so before it was answered");

    // 🔴 E3 SECOND CASE + E4: a value somebody else changed IS a conflict - nothing is written, the line
    // stays, AND THE WINDOWS ASSIGNMENT STAYS TOO, so Remove assignment can be tried again.
    reset();
    own(newPath, madeProfile, madeEntry, false, 0);
    AddCudaRecord(line(madeProfile, madeEntry, kId5090));
    cuda.settings[madeProfile] = cd::CudaNoneValue();   // changed in NVIDIA's own panel since
    removeReady(newPath);
    press(cd::IDC_GPU_REMOVE, st->hRemove);
    Check(cuda.settings[madeProfile] == cd::CudaNoneValue() && cuda.deletes == 0 && cudaRecord.rows.size() == 1,
          "E3: a setting somebody else changed since is left alone, and the line is KEPT");
    Check(preferences.count(newPath) == 1,
          "🔴 E4: and the GPU assignment is NOT removed, so Remove assignment is still lit");
    Check(has(messages.back(), cd::CudaRefusalReason(cd::CudaRefusal::ChangedSinceWritten)) &&
              has(messages.back(), L"their GPU assignment was left in place and Remove assignment can try "
                                   L"again"),
          "E4: and the result says both halves of that");

    // E3 second case, at the identity level: the entry the line names is not the one the driver answers
    // with any more. Nothing is written, and the assignment stays.
    reset();
    own(newPath, L"Google Chrome", L"chat.exe", true, 0);
    AddCudaRecord(line(madeProfile, madeEntry, kId5090));
    cuda.settings[madeProfile] = kId5090;
    removeReady(newPath);
    press(cd::IDC_GPU_REMOVE, st->hRemove);
    Check(cuda.deletes == 0 && cudaRecord.rows.size() == 1 && preferences.count(newPath) == 1,
          "E3: an entry that is no longer the recorded one is never taken away, and the row is left alone");
    Check(has(messages.back(), cd::CudaRefusalReason(cd::CudaRefusal::RecordedProfileChanged)),
          "E3: and the result says the settings entry changed");

    // E3 THIRD CASE: the entry is already gone. The line is dropped and it is not reported as a failure.
    reset();
    AddCudaRecord(line(madeProfile, madeEntry, kId5090));
    removeReady(newPath);
    press(cd::IDC_GPU_REMOVE, st->hRemove);
    Check(cuda.written.empty() && cuda.saves == 0 && cuda.deletes == 0 && cudaRecord.rows.empty(),
          "E3: an entry that is gone needs nothing done, and the line is spent anyway");
    Check(preferences.count(newPath) == 0, "E3: and that row's assignment is removed normally");
    Check(has(messages.back(), L"no longer has NVIDIA settings of its own") &&
              !has(messages.back(), L"was not put back for"),
          "E3: and it is said as 'nothing to put back', not as a failure");

    // E6 + E4 AT THE LEVEL OF THE WHOLE RUN: a record nobody could read may name the very application
    // being removed, so NOTHING is removed - and the question says so before Yes.
    reset();
    cudaRecord.state = cd::CudaRecordState::Unreadable;
    cuda.settings[madeProfile] = kId5090;
    removeReady(newPath);
    press(cd::IDC_GPU_REMOVE, st->hRemove);
    Check(preferences.count(newPath) == 1 && cuda.opened == 0 && cuda.written.empty(),
          "🔴 E4: with the note unreadable, no GPU assignment is removed at all");
    Check(has(messages[0], L"Nothing is removed at all") &&
              has(messages[0], cd::CudaRefusalReason(cd::CudaRefusal::RecordUnreadable)),
          "E4: and the question says that BEFORE it is answered");
    // 🔴 R5-6: a user told "nothing can be removed" has to be told WHERE the note is and what to clear.
    Check(has(messages[0], L"C:\\GameOptimizerPanelTest") && has(messages[0], L"gpu-cuda-before-*.txt") &&
              has(messages[0], cd::CudaRecordFileName()),
          "R5-6: and it names the configuration folder and the older per-run file pattern");
    Check(has(messages.back(), cd::CudaRefusalReason(cd::CudaRefusal::RecordUnreadable)) &&
              has(messages.back(), L"so no GPU assignment was removed either") &&
              !has(messages.back(), L"never had their CUDA GPU changed here"),
          "E6: an unreadable note is said out loud, not read as 'there was none'");

    // E6 on Apply: it will not write over a note it could not read, and the GPU assignment still happens.
    reset();
    cudaRecord.state = cd::CudaRecordState::Unreadable;
    space(newPath);
    yesNoAnswer = IDYES;
    press(cd::IDC_GPU_APPLY, st->hApply);
    Check(preferences.count(newPath) == 1 && cuda.opened == 0 && cuda.settings.empty(),
          "E6: Apply will not write over a note it could not read, and the GPU assignment still happens");
    Check(has(messages.back(), cd::CudaRefusalReason(cd::CudaRefusal::RecordUnreadable)),
          "E6: and says why");

    // E4 at run level, the other way: something of ours IS recorded and there is no driver to undo it
    // with, so again nothing is removed.
    reset();
    AddCudaRecord(line(madeProfile, madeEntry, kId5090));
    cuda.available = false;
    cuda.openRefusal = cd::CudaRefusal::NoNvidiaDriver;
    removeReady(newPath);
    press(cd::IDC_GPU_REMOVE, st->hRemove);
    Check(preferences.count(newPath) == 1 && cudaRecord.rows.size() == 1,
          "E4: with something of ours recorded and no driver, nothing is removed and the line is kept");
    Check(has(messages[0], L"Nothing is removed at all"),
          "E4: and the question says so for that reason too");

    // 🔴 R6-3 REPLACED WHAT THIS TEST USED TO ENCODE. It asserted `cuda.opened == 0` - with nothing of
    // ours recorded, NVIDIA was never asked anything - and that silence was the defect: the session is
    // what would have found an entry this product made for a ticked row. The session is now opened to
    // LOOK, and when it finds nothing of ours the run is as silent as it ever was: nothing written,
    // nothing refused, not one word about CUDA in the question or the result.
    reset();
    cuda.settings[L"Someone Else"] = kId5090;
    removeReady(newPath);
    press(cd::IDC_GPU_REMOVE, st->hRemove);
    Check(preferences.count(newPath) == 0 && cuda.opened == 1 && cuda.written.empty() && cuda.deletes == 0,
          "R6-3: with nothing of ours recorded, NVIDIA is LOOKED at and nothing is written");
    Check(!has(messages[0], L"CUDA") && !has(messages.back(), L"CUDA"),
          "R6-3: and with no entry of ours there, nothing about CUDA is said either");

    // But a ticked row the record DOES name is opened, and the ones it does not are counted and said.
    // The entry is this copy of chat.exe and not its sibling version, because our entry is a full path.
    reset();
    own(newPath, madeProfile, madeEntry, false, 0);
    cuda.settings[madeProfile] = kId5090;
    AddCudaRecord(line(madeProfile, madeEntry, kId5090));
    preferences[newPath] = cd::FormatPreferenceValue(bgKey);
    preferences[oldPath] = cd::FormatPreferenceValue(bgKey);
    cd::ActivateGpuPanel(panel, cfg, snap);
    space(newPath);
    space(oldPath);
    yesNoAnswer = IDYES;
    press(cd::IDC_GPU_REMOVE, st->hRemove);
    Check(cuda.profiles.empty() && cudaRecord.rows.empty(),
          "Remove: the row the record names has its entry taken away");
    Check(has(messages.back(), L"1 of them never had their CUDA GPU changed here"),
          "Remove: and the ticked row it does not name is counted and said");

    // 🔴 THE CHECK BOX GATES APPLY'S WRITES, NOT REMOVE'S UNDO. Assign with it ticked, clear it ("stop
    // touching CUDA"), then Remove: before this, the GPU preference went and the entry this product made
    // stayed - and with nothing assigned any more Remove was greyed out.
    cfg.setCudaGpu = false;
    reset();
    own(newPath, madeProfile, madeEntry, false, 0);
    AddCudaRecord(line(madeProfile, madeEntry, kId5090));
    cuda.settings[madeProfile] = kId5090;
    removeReady(newPath);
    press(cd::IDC_GPU_REMOVE, st->hRemove);
    Check(preferences.count(newPath) == 0 && cuda.profiles.empty() && cudaRecord.rows.empty(),
          "with the check box off, Remove still takes away what this product made");
    Check(has(messages[0], L"takes away the NVIDIA settings entry Game Optimizer made for 1 of them") &&
              has(messages[0], L"whether or not \"Also set which GPU CUDA uses\" is ticked"),
          "and the question says so before it is answered");
    Check(has(messages.back(), L"Which GPU CUDA uses was put back for 1 of them."),
          "and the result reports it like any other undo");

    // With the box off and nothing of ours recorded, the word CUDA does not appear at all. R6-3's look
    // runs here too - the box has never gated Remove - and it finds nothing, so nothing is said.
    cfg.setCudaGpu = false;
    reset();
    removeReady(newPath);
    press(cd::IDC_GPU_REMOVE, st->hRemove);
    Check(preferences.count(newPath) == 0 && cuda.written.empty() && cuda.deletes == 0,
          "with the box off and nothing of ours recorded, Remove writes nothing at all");
    Check(!has(messages[0], L"CUDA") && !has(messages.back(), L"CUDA"),
          "and says nothing about CUDA, in the question or the result");
    cfg.setCudaGpu = true;

    // The Apply question may NOT promise a CUDA change for a row NVIDIA has no settings entry for.
    reset();
    space(newPath);
    yesNoAnswer = IDNO;
    press(cd::IDC_GPU_APPLY, st->hApply);
    Check(has(messages[0], L"This also asks NVIDIA to use") &&
              has(messages[0], L"The result says which ones changed") &&
              !has(messages[0], L"will be set to use"),
          "E9: Apply's question asks rather than promises, and points at the result");

    // ---- The findings about what the code DOES, from all three Council rounds -------------------
    //
    // The driver is a place other people write too, so nothing here may treat "we could not look" as
    // "there was nothing there".

    // A CUDA setting on OUR OWN entry that could not be READ is never written over.
    reset();
    own(newPath, madeProfile, madeEntry, false, 0);
    AddCudaRecord(line(madeProfile, madeEntry, kId4090));
    cuda.settings[madeProfile] = kId4090;
    cuda.readFails = true;
    space(newPath);
    yesNoAnswer = IDYES;
    press(cd::IDC_GPU_APPLY, st->hApply);
    Check(preferences.count(newPath) == 1 && cuda.written.empty() && cuda.saves == 0 &&
              cuda.settings[madeProfile] == kId4090,
          "a CUDA setting that could not be read is left alone");
    Check(has(messages.back(), cd::CudaRefusalReason(cd::CudaRefusal::CouldNotRead)),
          "and the result says which application, and why");

    // A LIVE value the driver could not have written counts as could-not-read.
    reset();
    own(newPath, madeProfile, madeEntry, false, 0);
    AddCudaRecord(line(madeProfile, madeEntry, kId4090));
    cuda.settings[madeProfile] = L"autoselect";
    space(newPath);
    yesNoAnswer = IDYES;
    press(cd::IDC_GPU_APPLY, st->hApply);
    Check(cuda.written.empty() && cuda.saves == 0 && cuda.settings[madeProfile] == L"autoselect",
          "a previous value that is not a legal driver value refuses the row");
    Check(has(messages.back(), cd::CudaRefusalReason(cd::CudaRefusal::CouldNotRead)),
          "and it is said as could-not-read");

    // The same on Remove - a read that fails must not be taken as "it is still ours to take away".
    reset();
    own(newPath, madeProfile, madeEntry, false, 0);
    AddCudaRecord(line(madeProfile, madeEntry, kId5090));
    cuda.settings[madeProfile] = kId5090;
    cuda.readFails = true;
    removeReady(newPath);
    press(cd::IDC_GPU_REMOVE, st->hRemove);
    Check(preferences.count(newPath) == 1 && cuda.deletes == 0 && cudaRecord.rows.size() == 1,
          "a failed read on Remove takes nothing away, spends no line and keeps the assignment");
    Check(has(messages.back(), cd::CudaRefusalReason(cd::CudaRefusal::CouldNotRead)),
          "and the result says so on Remove too");

    // Two cards reporting one adapter key cannot be told apart, so the run refuses instead of writing
    // NVIDIA's "nothing is excluded" - which is the opposite of what was asked for.
    reset();
    cuda.twinCards = true;
    space(newPath);
    yesNoAnswer = IDYES;
    press(cd::IDC_GPU_APPLY, st->hApply);
    Check(preferences.count(newPath) == 1 && cuda.written.empty() && cuda.creates == 0 && cuda.saves == 0,
          "two identical cards write no CUDA setting at all, and the GPU assignment still happens");
    Check(has(messages.back(), cd::CudaRefusalReason(cd::CudaRefusal::AmbiguousIdenticalCards)) &&
              !has(messages[0], L"CUDA"),
          "the result says it once for the whole run, and the question promised nothing");

    // NvAPI saves the WHOLE session, so a row whose save failed has to leave the session too - or the
    // next row's save commits a change no line names. And the line goes with it.
    reset();
    cuda.failSaveNumber = 1;
    space(newPath);
    space(oldPath);
    yesNoAnswer = IDYES;
    press(cd::IDC_GPU_APPLY, st->hApply);
    Check(preferences.count(newPath) == 1 && preferences.count(oldPath) == 1 && cuda.saves == 2,
          "setup: both GPU preferences were written, and each row saved on its own");
    Check(cuda.saved.size() == 1 && cuda.settings.size() == 1 && cuda.byExe.size() == 1 && cuda.deletes == 1,
          "the row whose save failed is out of the session, so the next save cannot commit it");
    Check(cudaRecord.rows.size() == 1 && cuda.saved.count(cudaRecord.rows[0].profileName) == 1,
          "and the one line left in the record is exactly the row that landed");
    Check(has(messages.back(), cd::CudaRefusalReason(cd::CudaRefusal::NotSaved)),
          "the row that did not land is named in the result");

    // 🔴 E5: A ROLLBACK THAT ITSELF FAILS STOPS THE WHOLE RUN - not just the CUDA half. The row after it
    // gets neither a CUDA setting nor a Windows GPU preference, which is what makes the result's own
    // sentence true.
    reset();
    cuda.saveFails = true;
    cuda.failWriteNumber = 2;   // the row's own write lands; the rollback's write does not
    space(newPath);
    space(oldPath);
    yesNoAnswer = IDYES;
    press(cd::IDC_GPU_APPLY, st->hApply);
    Check(preferences.size() == 1,
          "🔴 E5: the application after the failed rollback gets NO Windows GPU preference either");
    Check(cuda.saves == 1 && cuda.settings.size() == 1 && cuda.saved.empty(),
          "E5: and its CUDA half is not attempted at all");
    Check(has(messages.back(), L"was changed and could NOT be put back for:") &&
              has(messages.back(), L"Nothing after that was tried, for the GPU setting or for CUDA.") &&
              has(messages.back(), cudaRecord.path),
          "E5: the result says so plainly, names the application and points at the record");
    Check(has(messages.back(), L"Not tried:") &&
              has(messages.back(), L"which GPU CUDA uses was changed for an application before it"),
          "E5: and the row it stopped before is named in the GPU half too");

    // A note holding something that is not a CUDA setting is refused before the driver is asked.
    reset();
    own(newPath, madeProfile, madeEntry, false, 0);
    AddCudaRecord(line(madeProfile, madeEntry, L"whatever was in that line"));
    cuda.settings[madeProfile] = kId5090;
    removeReady(newPath);
    press(cd::IDC_GPU_REMOVE, st->hRemove);
    Check(cuda.deletes == 0 && cuda.settings[madeProfile] == kId5090 && preferences.count(newPath) == 1,
          "a damaged record value is never handed to the driver, and the row is left alone");
    Check(has(messages.back(), cd::CudaRefusalReason(cd::CudaRefusal::RecordUnusable)),
          "and the result says the record is the problem");

    // A profile lookup the driver would not answer is NOT "NVIDIA has no settings entry for this program
    // yet". The second does not stop the row - it MAKES an entry and writes on it.
    reset();
    cuda.lookupFails = true;
    space(newPath);
    yesNoAnswer = IDYES;
    press(cd::IDC_GPU_APPLY, st->hApply);
    Check(preferences.count(newPath) == 1 && cuda.creates == 0 && cuda.written.empty() && cuda.saves == 0 &&
              cuda.settings.empty() && cudaRecord.rows.empty(),
          "a lookup the driver would not answer makes no entry, writes nothing and records nothing");
    Check(has(messages.back(), cd::CudaRefusalReason(cd::CudaRefusal::CouldNotLookUp)) &&
              !has(messages.back(), L"will use Background test GPU for CUDA as well"),
          "the result says which application and why, and claims no CUDA change");

    reset();
    space(newPath);
    yesNoAnswer = IDYES;
    press(cd::IDC_GPU_APPLY, st->hApply);
    Check(cuda.creates == 1 && cuda.settings[madeProfile] == kId5090 && cudaRecord.rows.size() == 1,
          "and the driver ANSWERING that it has no entry still means 'make one'");

    // ---- Round 5: what the fourth Council round found, through the real Apply and Remove -------------
    //
    // 🔴 R5-3, AND IT IS THE ONE THE OPERATOR WOULD HAVE MET FIRST (Council round 4, BOTH seats). A row
    // whose Windows GPU pin ALREADY equals what Apply intends is AlreadyDone - which is every application
    // v0.5.6 and v0.5.7 pinned - and the CUDA half was gated on Done, so the whole feature did nothing at
    // all for them, in silence.
    reset();
    preferences[newPath] = cd::FormatPreferenceValue(bgKey);
    cd::ActivateGpuPanel(panel, cfg, snap);
    space(newPath);
    yesNoAnswer = IDYES;
    press(cd::IDC_GPU_APPLY, st->hApply);
    Check(cuda.creates == 1 && cuda.settings[madeProfile] == kId5090 && cuda.saves == 1 &&
              cudaRecord.rows.size() == 1,
          "🔴 R5-3: a row already pinned to the target still has its CUDA GPU set");
    // 🔴 R6-4: AND THE HEADING NOW SAYS WHICH SETTING IT MEANS. "Already set, nothing written" read as
    // "nothing happened for any of these" on exactly the machine where it covers every row - one
    // upgrading from v0.5.6 or v0.5.7, where the CUDA half below it is doing all the work.
    Check(has(messages.back(), L"Windows' own GPU preference was already set, nothing written:") &&
              has(messages.back(), L"1 of them will use Background test GPU for CUDA as well."),
          "R5-3/R6-4: the result counts that row in both halves, and says which setting was already set");
    // 🔴 R6-4, AND IT IS WHAT AN UPGRADE FROM v0.5.7 ACTUALLY HITS: t.done is 0 here - not one registry
    // value was written - and the restart paragraph must still be printed, because the CUDA setting DID
    // change and NVIDIA obeys it at the application's next launch.
    Check(has(messages.back(), L"0 of 1 assigned to Background test GPU.") &&
              has(messages.back(), L"Restart those applications for the change to take effect.") &&
              has(messages.back(), L"The same is true of which GPU CUDA uses"),
          "🔴 R6-4: a row that wrote NO registry value still gets the restart instruction");

    // And the half of R5-3 that did NOT change: a row whose GPU write was refused drags no CUDA change.
    reset();
    preferences[newPath] = cd::FormatPreferenceValue(mainKey);
    cd::ActivateGpuPanel(panel, cfg, snap);
    space(newPath);
    {
        bool bumped = false;
        duringWrite = [&]() {
            if (bumped) return;
            bumped = true;
            preferences[newPath] = L"AppStatus=1;";   // another program, inside the guarded write
        };
        yesNoAnswer = IDYES;
        press(cd::IDC_GPU_APPLY, st->hApply);
        duringWrite = nullptr;
        Check(bumped && cuda.settings.empty() && cuda.creates == 0 && cudaRecord.rows.empty(),
              "R5-3: a refused GPU write still drags no CUDA change, pinned or not");
    }

    // 🔴 R5-1: AN ENTRY HOLDING SETTINGS THIS PRODUCT DID NOT WRITE IS NEVER DELETED. NVIDIA keeps every
    // per-application setting in ONE profile, so the entry Game Optimizer made is also where the user's own
    // "Vertical sync" for that program lives. Remove used to delete the whole profile and take it with it.
    reset();
    own(newPath, madeProfile, madeEntry, false, 0);
    AddCudaRecord(line(madeProfile, madeEntry, kId5090));
    cuda.settings[madeProfile] = kId5090;
    cuda.otherSettings[madeProfile] = 1;              // the user's own setting, in the same entry
    removeReady(newPath);
    press(cd::IDC_GPU_REMOVE, st->hRemove);
    Check(cuda.profiles.count(madeProfile) == 1 && cuda.byExe.count(cd::ToLower(newPath)) == 1 &&
              cuda.otherSettings[madeProfile] == 1 && cuda.settings.count(madeProfile) == 0 &&
              cuda.deletes == 0,
          "🔴 R5-1: the user's own settings survive - only Game Optimizer's own CUDA setting is taken out");
    Check(preferences.count(newPath) == 0, "R5-1: and it is a successful restore, so the row is removed");
    // 🔴 R6-7: AND THE LINE IS KEPT, WITH THE VALUE EMPTIED. Spending it left the entry stranded - still
    // there, still carrying this product's name, and with no note claiming it every future Apply answered
    // "NVIDIA manages this one", forever.
    Check(cudaRecord.rows.size() == 1 && cudaRecord.rows[0].profileName == madeProfile &&
              cudaRecord.rows[0].appEntry == madeEntry &&
              cudaRecord.rows[0].lastWrote == cd::CudaNothingWritten(),
          "🔴 R6-7: the note KEEPS the line for an entry it left standing, with the value emptied");
    Check(has(messages.back(), L"Which GPU CUDA uses was put back for 1 of them.") &&
              has(messages.back(), L"the entry itself was left standing") &&
              has(messages.back(), L"so assigning that application again can use it"),
          "R5-1/R6-7: and the result says which restore happened and that the entry can be used again");

    // 🔴 R6-7, THE SECOND HALF, AND IT IS THE WHOLE POINT: A LATER APPLY OWNS THAT ENTRY AGAIN. Before
    // round 6 this row was refused for ever - the entry existed, carried our name, and no line claimed it.
    preferences.erase(newPath);
    cd::ActivateGpuPanel(panel, cfg, snap);
    space(newPath);
    yesNoAnswer = IDYES;
    press(cd::IDC_GPU_APPLY, st->hApply);
    Check(cuda.settings[madeProfile] == kId5090 && cuda.otherSettings[madeProfile] == 1 &&
              cuda.creates == 0 && cudaRecord.rows.size() == 1 &&
              cudaRecord.rows[0].lastWrote == kId5090,
          "🔴 R6-7: a later Apply owns that entry again, and the line carries a value once more");
    // 🔴 NOT CudaRefusalReason(NvidiaManagesIt) - THAT IS THE EMPTY STRING, and `has(msg, L"")` is true of
    // every message ever written, so the check would have passed on a refused row. The sentence
    // FormatCudaNvidiaManagesLine really builds is what to look for.
    Check(!has(messages.back(), L"NVIDIA keeps which GPU CUDA uses for it in its own settings entry") &&
              has(messages.back(), L"1 of them will use Background test GPU for CUDA as well."),
          "R6-7: and it is not refused as one NVIDIA manages");

    // 🔴 R6-7, THE THIRD CASE: a Remove that MEETS such a line. Nothing to take away, nothing failed,
    // nothing blocked - and the line stays.
    reset();
    own(newPath, madeProfile, madeEntry, false, 0);
    cuda.otherSettings[madeProfile] = 1;              // the user's own setting is still in it
    AddCudaRecord(line(madeProfile, madeEntry, cd::CudaNothingWritten()));
    removeReady(newPath);
    press(cd::IDC_GPU_REMOVE, st->hRemove);
    Check(preferences.count(newPath) == 0 && cuda.deletes == 0 && cuda.written.empty() &&
              cuda.otherSettings[madeProfile] == 1 && cuda.profiles.count(madeProfile) == 1,
          "🔴 R6-7: a line holding nothing of ours takes nothing away and blocks nothing");
    Check(cudaRecord.rows.size() == 1 && cudaRecord.rows[0].lastWrote == cd::CudaNothingWritten(),
          "R6-7: and the line is KEPT - it goes only when the entry itself does");
    Check(has(messages.back(), L"already had nothing of Game Optimizer's in its NVIDIA settings entry") &&
              !has(messages.back(), L"never had their CUDA GPU changed here") &&
              !has(messages.back(), L"was not put back for"),
          "R6-7: and it is said as 'nothing to put back', not as a failure and not as one we never touched");
    // and the question does not promise to take anything away for it
    Check(!has(messages[0], L"takes away the NVIDIA settings entry"),
          "R6-7: the question promises no undo for a line that has nothing to undo");

    // 🔴 R5-2: THE ADOPT PATH PASSES THE OWNERSHIP CHECK TOO. An entry carrying our own name that ANOTHER
    // program has joined since is refused, named and counted - it used to be written on as ours alone.
    reset();
    own(L"C:\\GameOptimizerPanelTest\\Other\\other.exe", madeProfile,
        L"c:/gameoptimizerpaneltest/other/other.exe", false, 0);
    AddCudaRecord(line(madeProfile, madeEntry, kId5090));
    space(newPath);
    yesNoAnswer = IDYES;
    press(cd::IDC_GPU_APPLY, st->hApply);
    Check(preferences.count(newPath) == 1 && cuda.creates == 0 && cuda.written.empty() && cuda.saves == 0,
          "🔴 R5-2: an entry of our name another program joined is never written on");
    Check(has(messages.back(), madeProfile) && has(messages.back(), L"1 other program as well") &&
              has(messages.back(), L"NVIDIA Control Panel"),
          "R5-2: and the refusal names it and states the count it MEASURED");

    // 🔴 R5-4: A CLEAN-UP THAT FAILED IS UNRESOLVED - the profile this row made is still in the open
    // session, and NvAPI_DRS_SaveSettings commits the whole session, so the run stops the way E5 stops it.
    reset();
    cuda.saveFails = true;       // the row's own save refuses, so the row rolls back
    cuda.deleteFails = true;     // and the profile it made will not go again
    space(newPath);
    space(oldPath);
    yesNoAnswer = IDYES;
    press(cd::IDC_GPU_APPLY, st->hApply);
    Check(preferences.size() == 1,
          "🔴 R5-4: the application after a failed clean-up gets NO Windows GPU preference either");
    Check(cuda.saves == 1 && cuda.saved.empty(),
          "R5-4: and nothing was committed, so no later save can carry the leftover");
    Check(has(messages.back(), L"was changed and could NOT be put back for:") &&
              has(messages.back(), L"Nothing after that was tried, for the GPU setting or for CUDA."),
          "R5-4: and the result says so plainly");

    // 🔴 R5-5: REMOVE MEETS AN ENTRY OF OUR OWN NAME THAT NO NOTE CLAIMS. It is SAID and nothing else: the
    // removal is not blocked (E4's trap) and the entry is not deleted (E1). Chair decision, round 5.
    reset();
    {
        const std::wstring oldProfile = cd::CudaProfileNameFor(oldPath);
        const std::wstring oldEntry = cd::CudaAppKeyFor(oldPath);
        own(oldPath, oldProfile, oldEntry, false, 0);
        cuda.settings[oldProfile] = kId5090;
        AddCudaRecord(line(oldProfile, oldEntry, kId5090));       // this one IS ours on the record
        own(newPath, madeProfile, madeEntry, false, 0);           // and this one is not
        cuda.settings[madeProfile] = kId4090;
        preferences[newPath] = cd::FormatPreferenceValue(bgKey);
        preferences[oldPath] = cd::FormatPreferenceValue(bgKey);
        cd::ActivateGpuPanel(panel, cfg, snap);
        space(newPath);
        space(oldPath);
        yesNoAnswer = IDYES;
        press(cd::IDC_GPU_REMOVE, st->hRemove);
        Check(preferences.count(newPath) == 0 && preferences.count(oldPath) == 0,
              "🔴 R5-5: the removal is NOT blocked by an entry no note claims");
        Check(cuda.profiles.count(madeProfile) == 1 && cuda.settings[madeProfile] == kId4090,
              "🔴 R5-5: and that entry is NOT deleted either - no record claims it");
        Check(has(messages.back(), L"Nothing was done to those entries:") &&
                  has(messages.back(), madeProfile) && has(messages.back(), L"NVIDIA Control Panel") &&
                  !has(messages.back(), L"never had their CUDA GPU changed here"),
              "R5-5: and the result names it instead of counting it as one this never touched");
    }

    // 🔴 R6-3: THE COMMONEST WAY TO A LEFTOVER, AND UNTIL ROUND 6 IT WAS SILENT. The user deletes
    // gpu-cuda-record.txt - a text file in their configuration folder whose own header says it is only a
    // note - and presses Remove. NO ticked row then has a line, so the driver session was never opened at
    // all: the Windows pin went, the NVIDIA settings entry this product made stayed behind for good, and
    // not one word was said. R5-5's sentence existed for exactly this and could only be reached while some
    // OTHER ticked row still had a line of its own.
    reset();
    cudaRecord.rows.clear();                       // the note is gone entirely
    own(newPath, madeProfile, madeEntry, false, 0);
    cuda.settings[madeProfile] = kId5090;
    removeReady(newPath);
    press(cd::IDC_GPU_REMOVE, st->hRemove);
    Check(preferences.count(newPath) == 0,
          "🔴 R6-3: with NO note at all, the removal still happens - it is never blocked");
    Check(cuda.opened == 1 && cuda.deletes == 0 && cuda.written.empty() &&
              cuda.profiles.count(madeProfile) == 1 && cuda.settings[madeProfile] == kId5090,
          "🔴 R6-3: the session is opened to LOOK, and the entry no note claims is left exactly alone");
    Check(has(messages.back(), L"Nothing was done to those entries:") &&
              has(messages.back(), madeProfile) && has(messages.back(), L"NVIDIA Control Panel"),
          "🔴 R6-3: and the entry is NAMED, with where to set it");
    Check(!has(messages.back(), L"never had their CUDA GPU changed here") &&
              !has(messages.back(), L"was not put back for") &&
              !has(messages[0], L"Nothing is removed at all"),
          "R6-3: it is not counted as one this never touched, not a refusal, and the question blocks nothing");

    // 🔴 R6-3, THE OTHER HALF: THE SESSION WILL NOT OPEN. Behave exactly as before - silent, and Remove
    // proceeds. Setting the whole-run refusal here would block every Remove on a machine with no NVIDIA
    // driver over a change nobody ever made, which is the trap E4 exists to close.
    reset();
    cudaRecord.rows.clear();
    cuda.available = false;
    cuda.openRefusal = cd::CudaRefusal::SessionRefused;
    removeReady(newPath);
    press(cd::IDC_GPU_REMOVE, st->hRemove);
    Check(preferences.count(newPath) == 0,
          "🔴 R6-3: a session that will not open never blocks a Remove with nothing of ours recorded");
    Check(!has(messages[0], L"CUDA") && !has(messages[0], L"Nothing is removed at all") &&
              !has(messages.back(), L"CUDA"),
          "R6-3: and nothing at all is said about it, in the question or the result");

    // 🔴 R6-1: A COUNT IS NOT AN IDENTITY - THE SECOND TIME THIS PROJECT HAS HAD TO LEARN THAT. Round 5
    // read "this entry holds one setting" as "the one setting is mine". Here the entry holds ONE setting
    // that is the user's, and no CUDA setting of its own: NvAPI_DRS_GetSetting still answers with the
    // value this product wrote, because it falls back to NVIDIA's general settings. Deleting the entry on
    // that would take the user's setting - and the application entry - away over a value living elsewhere.
    reset();
    own(newPath, madeProfile, madeEntry, false, 0);
    AddCudaRecord(line(madeProfile, madeEntry, kId5090));
    cuda.settings[madeProfile] = kId5090;         // what the READ answers
    cuda.inherited.insert(madeProfile);           // ... and it is not this entry's own setting
    cuda.otherSettings[madeProfile] = 1;          // the ONE setting it really holds is the user's
    removeReady(newPath);
    press(cd::IDC_GPU_REMOVE, st->hRemove);
    Check(cuda.deletes == 0 && cuda.written.empty() && cuda.profiles.count(madeProfile) == 1 &&
              cuda.otherSettings[madeProfile] == 1 && cudaRecord.rows.size() == 1,
          "🔴 R6-1: one setting that is NOT ours is never deleted, whatever the read answered");
    Check(preferences.count(newPath) == 1,
          "R6-1: and the row is left alone, so Remove assignment can be tried again");
    Check(has(messages.back(), cd::CudaRefusalReason(cd::CudaRefusal::SettingNotOurs)),
          "R6-1: and the result says the two driver answers disagree");

    // 🔴 R6-1 / R6-2: A DRIVER THAT WILL NOT LIST AN ENTRY'S SETTINGS CANNOT BE ASKED TO DELETE IT. Not
    // "it has none" - nobody knows - so nothing is taken away and the line is kept.
    reset();
    own(newPath, madeProfile, madeEntry, false, 0);
    AddCudaRecord(line(madeProfile, madeEntry, kId5090));
    cuda.settings[madeProfile] = kId5090;
    cuda.noEnumSettings = true;                   // R6-2: the entry point did not resolve, so the op is EMPTY
    removeReady(newPath);
    press(cd::IDC_GPU_REMOVE, st->hRemove);
    Check(cuda.deletes == 0 && cuda.profiles.count(madeProfile) == 1 && cudaRecord.rows.size() == 1 &&
              preferences.count(newPath) == 1,
          "🔴 R6-2: with no way to list an entry's settings, nothing is taken away and the line is kept");
    Check(has(messages.back(), cd::CudaRefusalReason(cd::CudaRefusal::SettingsUnknown)),
          "R6-1: and it is said as 'nobody could tell', never as 'it holds none'");

    // 🔴 R6-2: AND THE DELETE CALLS THEMSELVES. MakeCudaOps used to install a delete lambda whatever the
    // driver offered, so PlanCudaForRow's NoWayToUndo could never fire on a real machine. The panel builds
    // its ops through the same gate the product does, so a driver without those entry points refuses to
    // MAKE an entry it could never take away again.
    reset();
    cuda.noDelete = true;
    space(newPath);
    yesNoAnswer = IDYES;
    press(cd::IDC_GPU_APPLY, st->hApply);
    Check(preferences.count(newPath) == 1 && cuda.creates == 0 && cuda.profiles.empty() &&
              cuda.written.empty() && cudaRecord.rows.empty(),
          "🔴 R6-2: a driver with no delete calls makes no entry, and the GPU assignment still happens");
    Check(has(messages.back(), cd::CudaRefusalReason(cd::CudaRefusal::NoWayToUndo)),
          "R6-2: and the result says why");

    // 🔴 PRE-PUBLISH REVIEW: AND THE SETTINGS ENUMERATION. Remove refuses an entry whose settings it cannot list
    // (the SettingsUnknown case above), so a driver without NvAPI_DRS_EnumSettings must not be given one to refuse.
    reset();
    cuda.noEnumSettings = true;
    space(newPath);
    yesNoAnswer = IDYES;
    press(cd::IDC_GPU_APPLY, st->hApply);
    Check(preferences.count(newPath) == 1 && cuda.creates == 0 && cuda.profiles.empty() &&
              cuda.written.empty() && cudaRecord.rows.empty(),
          "🔴 pre-publish: a driver that cannot list an entry's settings makes no entry, and the GPU assignment "
          "still happens");
    Check(has(messages.back(), cd::CudaRefusalReason(cd::CudaRefusal::NoWayToListSettings)),
          "pre-publish: and the result names the missing enumeration");

    // 🔴 AND AN ENTRY OF OURS THAT ALREADY EXISTS: Remove could not undo a write on it either (SettingsUnknown).
    reset();
    cuda.noEnumSettings = true;
    own(newPath, madeProfile, madeEntry, false, 0);
    AddCudaRecord(line(madeProfile, madeEntry, kId4090));
    cuda.settings[madeProfile] = kId4090;
    space(newPath);
    yesNoAnswer = IDYES;
    press(cd::IDC_GPU_APPLY, st->hApply);
    Check(preferences.count(newPath) == 1 && cuda.written.empty() && cuda.saves == 0 &&
              cuda.settings[madeProfile] == kId4090 && cudaRecord.rows.size() == 1 &&
              cudaRecord.rows[0].lastWrote == kId4090,
          "🔴 pre-publish: an existing entry is not written on either, and the GPU assignment still happens");
    Check(has(messages.back(), cd::CudaRefusalReason(cd::CudaRefusal::NoWayToListSettings)),
          "pre-publish: and the result names the missing enumeration for it too");

    // 🔴 R5-7: -167 NAMES THE ENTRY THAT ALREADY HOLDS THAT FILE NAME, when the driver can be asked which.
    reset();
    cuda.createAnswer = cd::CudaCreate::AlreadyInUse;
    own(L"chat.exe", L"Some Other Game", L"chat.exe", false, 2);   // the BASE NAME, which is what -167 means
    space(newPath);
    yesNoAnswer = IDYES;
    press(cd::IDC_GPU_APPLY, st->hApply);
    Check(preferences.count(newPath) == 1 && cuda.settings.empty() && cudaRecord.rows.empty() &&
              cuda.profiles.count(L"Some Other Game") == 1,
          "R5-7: the row is refused, nothing of ours is left and the other entry is untouched");
    Check(has(messages.back(), L"\"Some Other Game\"") &&
              !has(messages.back(), cd::CudaRefusalReason(cd::CudaRefusal::NameAlreadyInUse)),
          "🔴 R5-7: and the result names that entry rather than describing it in general");

    ResetCuda();
    reset();

    // ---- v0.5.9: A RUN HOLDS THE TAB, SAYS WHERE IT IS, AND CANNOT BE STARTED TWICE --------------------------------
    //
    // 🔴 WHAT THESE CANNOT SHOW, said plainly because the gap is structural and not an oversight:
    //   * THE PUMP ITSELF IS NOT COVERED. This file macro-replaces MessageBoxW with a stub that pumps nothing, the
    //     parent window is never made visible, and nothing here observes PeekMessage. ShowRunProgress' bounded
    //     WM_PAINT peeks run, and no assertion below can tell a working pump from one that returned immediately.
    //   * NOTHING HERE PROVES WINDOWS WOULD STOP CALLING THE WINDOW HUNG. That is a judgement Windows makes about a
    //     real message queue on a visible window, and it takes a running application to see.
    //   * The re-entry below arrives by SendMessage from inside the write, which is the shape MessageBoxW's own modal
    //     loop produces - but it is a stand-in for that loop, not the loop itself.
    // What they DO cover is every rule that decides whether a second run may start, what the tab says while one is
    // going, and which controls are dead while it runs.
    {
        const std::wstring alphaPath = L"C:\\GameOptimizerPanelTest\\Alpha\\alpha.exe";
        const std::wstring betaPath = L"C:\\GameOptimizerPanelTest\\Beta\\beta.exe";
        cd::ProcInfo extra;
        extra.pid = 70011; extra.name = L"alpha.exe"; extra.fullPath = alphaPath; processes[extra.pid] = extra;
        extra.pid = 70012; extra.name = L"beta.exe"; extra.fullPath = betaPath; processes[extra.pid] = extra;
        // FOUR ROWS, TICKED BY HAND, so every one of them reaches pass two and the counter has four steps to take.
        const auto fourTicked = [&]() {
            reset();
            space(oldPath); space(newPath); space(alphaPath); space(betaPath);
            yesNoAnswer = IDYES;
        };
        const auto allTicked = [&]() {
            return st->rows[index(oldPath)].r.selected && st->rows[index(newPath)].r.selected &&
                   st->rows[index(alphaPath)].r.selected && st->rows[index(betaPath)].r.selected;
        };
        // Every control of the panel a run must take away, enumerated ONCE here so a button added later is covered by
        // the enabled/disabled checks below without its author remembering to add it.
        HWND panelControls[7] = { st->hTarget, st->hBulk, st->hSelAll, st->hClearSel,
                                  st->hRemove, st->hApply, st->hClose };
        const auto anyEnabled = [&]() {
            for (int i = 0; i < 7; ++i) if (IsWindowEnabled(panelControls[i])) return true;
            return false;
        };

        fourTicked();
        Check(st->rows.size() == 4 && allTicked(), "v0.5.9 setup: four rows, every one ticked by hand");

        // THE COUNTER, READ FROM THE REAL STATUS CONTROL WHILE THE RUN IS INSIDE A WRITE. duringWrite is called with
        // every earlier row already finished, so the n-th call must read the n-th row.
        std::vector<std::wstring> seen;
        duringWrite = [&]() { seen.push_back(statusText()); };
        press(cd::IDC_GPU_APPLY, st->hApply);
        duringWrite = nullptr;
        Check(seen.size() == 4, "v0.5.9: the run reached all four writes");
        {
            bool counted = true, verb = true;
            for (size_t i = 0; i < seen.size(); ++i) {
                if (!has(seen[i], L"Assigning " + std::to_wstring(i + 1) + L" of 4.")) counted = false;
                if (!has(seen[i], L"Assigning")) verb = false;
            }
            Check(counted && seen.size() == 4,
                  "🔴 v0.5.9: the status line counts the rows while the run walks them - 1 of 4 to 4 of 4");
            Check(verb, "v0.5.9: and every step of it names what the run is doing");
        }
        Check(!has(statusText(), L"Assigning") && !has(statusText(), L" of 4"),
              "🔴 v0.5.9: once the result is shown the counting line is gone again");
        Check(has(seen[0], L"NVIDIA's settings are saved once for each application"),
              "v0.5.9: a run with a CUDA half warns that each application costs a driver save");

        // 🔴 FOUNDER DECISION 18, ON THE PROGRESS LINE TOO. The check box off is a run with no CUDA half at all.
        cfg.setCudaGpu = false;
        fourTicked();
        seen.clear();
        duringWrite = [&]() { seen.push_back(statusText()); };
        press(cd::IDC_GPU_APPLY, st->hApply);
        duringWrite = nullptr;
        {
            bool quiet = seen.size() == 4;
            for (size_t i = 0; i < seen.size(); ++i)
                if (has(seen[i], L"CUDA") || has(seen[i], L"NVIDIA")) quiet = false;
            Check(quiet, "🔴 v0.5.9: a run with no CUDA half never says NVIDIA or CUDA while it counts");
            Check(seen.size() == 4 && has(seen[3], L"Assigning 4 of 4."),
                  "v0.5.9: and it still counts its rows");
        }
        cfg.setCudaGpu = true;

        // 🔴 THE DEFECT THIS WHOLE CHANGE EXISTS TO CLOSE: a second Apply DISPATCHED WHILE THE FIRST IS RUNNING used
        // to open a SECOND NVAPI driver session over the first, each with its own copy of gpu-cuda-record.txt, and
        // whichever saved last lost the other's lines. A disabled button does not stop a message already queued.
        fourTicked();
        {
            int openedInside = -1;
            size_t askedInside = 0;
            bool reentered = false;
            duringWrite = [&]() {
                if (reentered) return;
                reentered = true;
                press(cd::IDC_GPU_APPLY, st->hApply);   // the click the modal loop would have dispatched
                openedInside = cuda.opened;
                askedInside = messages.size();
            };
            press(cd::IDC_GPU_APPLY, st->hApply);
            duringWrite = nullptr;
            Check(reentered && openedInside == 1 && cuda.opened == 1,
                  "🔴 v0.5.9: a second Apply pressed during a run opens NO second driver session");
            Check(askedInside == 1 && messages.size() == 2,
                  "🔴 v0.5.9: and it asks no second question - one confirm, one result, for the whole run");
        }

        // THE OTHER FOUR ACTIONS, EACH PRESSED MID-RUN. None of them may touch the selection the run is walking.
        fourTicked();
        {
            bool tickedInside = false, pressedInside = false;
            duringWrite = [&]() {
                if (pressedInside) return;
                pressedInside = true;
                press(cd::IDC_GPU_BULK, st->hBulk);
                press(cd::IDC_GPU_SELALL, st->hSelAll);
                press(cd::IDC_GPU_CLEARSEL, st->hClearSel);
                press(cd::IDC_GPU_REMOVE, st->hRemove);
                tickedInside = allTicked();
            };
            press(cd::IDC_GPU_APPLY, st->hApply);
            duringWrite = nullptr;
            Check(pressedInside && tickedInside,
                  "🔴 v0.5.9: Auto assign, Select all, Deselect all and Remove pressed during a run change nothing");
            Check(messages.size() == 2,
                  "v0.5.9: and none of them asked a question of its own");
        }

        // CANCEL unticks every row, so a run walking them must not see one vanish underneath it.
        fourTicked();
        {
            bool tickedInside = false, pressedInside = false;
            duringWrite = [&]() {
                if (pressedInside) return;
                pressedInside = true;
                press(cd::IDC_GPU_CLOSE, st->hClose);
                tickedInside = allTicked();
            };
            press(cd::IDC_GPU_APPLY, st->hApply);
            duringWrite = nullptr;
            Check(pressedInside && tickedInside,
                  "🔴 v0.5.9: Cancel pressed during a run does not untick the rows the run is walking");
        }

        // EVERY BUTTON AND THE PICKER, BY ENUMERATION - the freeze lives in SyncButtons for exactly this reason.
        fourTicked();
        {
            bool deadInside = false, checkedInside = false;
            duringWrite = [&]() {
                if (checkedInside) return;
                checkedInside = true;
                deadInside = !anyEnabled();
            };
            press(cd::IDC_GPU_APPLY, st->hApply);
            duringWrite = nullptr;
            Check(checkedInside && deadInside,
                  "🔴 v0.5.9: every panel button and the GPU picker are disabled while a run is going");
            Check(IsWindowEnabled(st->hTarget) && IsWindowEnabled(st->hClose),
                  "🔴 v0.5.9: and they come back when it ends - Cancel included, which nothing else re-enables");
        }

        // 🔴 THE CLOSEST THIS SUITE CAN GET TO THE PUMP: a click POSTED during a run is still in the queue when the
        // run returns. It cannot prove the pump ran; it can prove the pump did not EAT input, which is the property
        // that makes the WM_PAINT-only peek safe.
        fourTicked();
        {
            bool posted = false;
            duringWrite = [&]() {
                if (posted) return;
                posted = PostMessageW(panel, WM_COMMAND, MAKEWPARAM(cd::IDC_GPU_APPLY, BN_CLICKED),
                                      reinterpret_cast<LPARAM>(st->hApply)) != 0;
            };
            press(cd::IDC_GPU_APPLY, st->hApply);
            duringWrite = nullptr;
            MSG queued;
            const bool still = PeekMessageW(&queued, panel, WM_COMMAND, WM_COMMAND, PM_REMOVE) != 0;
            Check(posted && still && LOWORD(queued.wParam) == cd::IDC_GPU_APPLY,
                  "🔴 v0.5.9: a click posted during a run is STILL IN THE QUEUE when the run returns");
        }

        // 🔴 B10 / CHAIR DECISION: Select all is off while Windows' GPU preferences could not all be read, exactly as
        // Auto assign is. Its sibling-pin exclusion reads the map that walk builds, and forty rows ticked by one
        // click cannot be judged row by row the way a single hand tick can.
        reset();
        Check(IsWindowEnabled(st->hSelAll), "v0.5.9 control: after a complete walk Select all is lit");
        complete = false;
        cd::ActivateGpuPanel(panel, cfg, snap);
        Check(!IsWindowEnabled(st->hSelAll),
              "🔴 v0.5.9: an incomplete preference walk greys Select all, as it greys Auto assign");
        press(cd::IDC_GPU_SELALL, st->hSelAll);
        Check(!st->rows[index(oldPath)].r.selected && !st->rows[index(newPath)].r.selected &&
                  !st->rows[index(alphaPath)].r.selected && !st->rows[index(betaPath)].r.selected,
              "🔴 v0.5.9: and a click posted to that grey button ticks nothing either");
        complete = true;

        processes.erase(70011); processes.erase(70012);
        reset();
        Check(st->rows.size() == 2, "v0.5.9: the run-progress rows are gone again");
    }

    // ---- THE BUTTON ROW'S TAB ORDER, ASKED OF THE DIALOG MANAGER ITSELF (v0.5.9) ------------------------
    //
    // 🔴 WHY THIS EXISTS. A live screen run posted VK_TAB along the row and read "Tab 1 -> 2102, Tab 2 -> 2103,
    // Tab 3 -> no focus", which was read as Deselect all (2104) having fallen out of the tab chain when Select
    // all was inserted before it. GetNextDlgTabItem IS the call IsDialogMessageW makes to answer Tab - the same
    // call GpuPanelKey makes for the full-path box - and it answers on a HIDDEN window, so the order this
    // product actually offers can be measured here, every gate run, with no screen and no live application.
    // A button added to the row in the wrong Z-order position, or with WS_TABSTOP forgotten, goes red here.
    //
    // 🔴 THE ENABLED STATES ARE SET HERE, NOT INHERITED. The dialog manager SKIPS a disabled control, and
    // "Deselect all is grey while nothing is ticked" is correct behaviour that would legitimately take 2104 out
    // of the walk. A check that inherited whatever the previous test left behind would be measuring the
    // selection, not the order. Every tab stop in the row is lit first; the last check then turns one off on
    // purpose, so a walk that had been comparing against a fixed list rather than reading live state fails too.
    //
    // THE PANEL IS GIVEN WS_VISIBLE FOR THE WALK. The dialog manager steps into a WS_EX_CONTROLPARENT child
    // only while that child carries WS_VISIBLE, and Settings shows this panel with its tab (settings.cpp's
    // ApplyPageVisibility). The top-level parent stays hidden, so nothing appears on screen.
    {
        reset();
        ShowWindow(panel, SW_SHOWNA);
        HWND lit[] = {st->hTarget, st->hList, st->hBulk, st->hSelAll, st->hClearSel, st->hRemove, st->hApply,
                      st->hClose};
        for (HWND h : lit) EnableWindow(h, TRUE);

        HWND intoPanel = GetNextDlgTabItem(parent, st->hList, FALSE);
        Check(intoPanel != nullptr && IsChild(panel, intoPanel),
              "control: the dialog manager steps from the list into the panel's own controls");

        const int wantFwd[] = {cd::IDC_GPU_BULK,   cd::IDC_GPU_SELALL, cd::IDC_GPU_CLEARSEL,
                               cd::IDC_GPU_REMOVE, cd::IDC_GPU_APPLY,  cd::IDC_GPU_CLOSE};
        int gotFwd[6] = {0, 0, 0, 0, 0, 0};
        HWND stop = st->hList;
        for (int i = 0; i < 6; ++i) {
            stop = stop ? GetNextDlgTabItem(parent, stop, FALSE) : nullptr;
            gotFwd[i] = stop ? GetDlgCtrlID(stop) : 0;
        }
        std::printf("TAB WALK forward from the list (2100): %d %d %d %d %d %d\n", gotFwd[0], gotFwd[1], gotFwd[2],
                    gotFwd[3], gotFwd[4], gotFwd[5]);
        bool forward = true;
        for (int i = 0; i < 6; ++i) if (gotFwd[i] != wantFwd[i]) forward = false;
        Check(forward,
              "🔴 v0.5.9: Tab from the list walks Auto assign 2102, Select all 2103, Deselect all 2104, "
              "Remove 2105, Apply 2106, Cancel 2107");

        // BACKWARDS IS NOT THE SAME QUESTION: Shift+Tab is IsDialogMessageW's own handling everywhere except the
        // full-path box, and a row that reads right forwards can still be wrong the other way.
        const int wantBack[] = {cd::IDC_GPU_APPLY,  cd::IDC_GPU_REMOVE, cd::IDC_GPU_CLEARSEL,
                                cd::IDC_GPU_SELALL, cd::IDC_GPU_BULK,   cd::IDC_GPU_LIST};
        int gotBack[6] = {0, 0, 0, 0, 0, 0};
        stop = st->hClose;
        for (int i = 0; i < 6; ++i) {
            stop = stop ? GetNextDlgTabItem(parent, stop, TRUE) : nullptr;
            gotBack[i] = stop ? GetDlgCtrlID(stop) : 0;
        }
        std::printf("TAB WALK backward from Cancel (2107): %d %d %d %d %d %d\n", gotBack[0], gotBack[1],
                    gotBack[2], gotBack[3], gotBack[4], gotBack[5]);
        bool backward = true;
        for (int i = 0; i < 6; ++i) if (gotBack[i] != wantBack[i]) backward = false;
        Check(backward,
              "🔴 v0.5.9: Shift+Tab from Cancel walks Apply 2106, Remove 2105, Deselect all 2104, "
              "Select all 2103, Auto assign 2102, the list 2100");

        // 🔴 AND THE WALK READS LIVE STATE, which is why the states above had to be set rather than inherited:
        // a grey Deselect all is correctly NOT a tab stop, so "Tab did not reach 2104" is only a defect while
        // the button is lit. This also proves the two checks above cannot pass vacuously against a fixed list.
        EnableWindow(st->hClearSel, FALSE);
        HWND past = GetNextDlgTabItem(parent, st->hSelAll, FALSE);
        Check(past != nullptr && GetDlgCtrlID(past) == cd::IDC_GPU_REMOVE,
              "🔴 v0.5.9: a DISABLED Deselect all is skipped by the dialog manager - grey is not a tab stop");
        EnableWindow(st->hClearSel, TRUE);

        ShowWindow(panel, SW_HIDE);
        cd::SyncButtons(st);   // the enabled states go back to what the panel's own rules say
    }

    // ---- capture056 step 12, v0.5.9 final run: a PROFILE GAME RUNNING, and the tab left and entered again ------------
    //
    // [M] v059-final-capture.txt: 9f ended "the tick is cleared - Deselect all and Apply are both off", chose the default
    // GPU again, 10b went to Profiles and back, and 12's first checks read "Deselect all enabled=True", then - after its
    // Space on the test row - "Apply enabled=False". [M] That run's 10-gpu-tab-minimum-size.png, taken between the two,
    // shows "[x] gotest-d3d.exe" on the caret row with the RTX 4090 the target and Apply lit. Two replays of the same
    // sequence through the real message paths, with a profile game running: as the script meant it (must pass), and
    // with ONE stray Space on the caret row after 9f (reproduces every reading of the failure).
    {
        const std::wstring testPath = L"C:\\GameOptimizerPanelTest\\Test\\gotest-d3d.exe";
        const std::wstring gamePath = L"C:\\GameOptimizerPanelTest\\SandFall\\SandFall-WinGDK-Shipping.exe";
        cd::ProcInfo extra;
        extra.pid = 70021; extra.name = L"gotest-d3d.exe"; extra.fullPath = testPath; processes[extra.pid] = extra;
        extra.pid = 70022; extra.name = L"SandFall-WinGDK-Shipping.exe"; extra.fullPath = gamePath; processes[extra.pid] = extra;
        cd::Profile profile;
        profile.name = L"clair33";
        profile.game = L"SandFall-WinGDK-Shipping.exe";
        cfg.profiles.push_back(profile);
        const auto anyTicked = [&]() { for (const auto& r : st->rows) if (r.r.selected) return true; return false; };
        // The script's SelectTarget: CB_SETCURSEL on the item carrying the GPU, then the CBN_SELCHANGE the combo would send.
        const auto chooseBackground = [&]() {
            const int n = static_cast<int>(SendMessageW(st->hTarget, CB_GETCOUNT, 0, 0));
            for (int i = 0; i < n; ++i) {
                const size_t c = static_cast<size_t>(SendMessageW(st->hTarget, CB_GETITEMDATA, i, 0));
                if (c < st->candidates.size() && st->candidates[c] == bgKey) SendMessageW(st->hTarget, CB_SETCURSEL, i, 0);
            }
            SendMessageW(panel, WM_COMMAND, MAKEWPARAM(cd::IDC_GPU_TARGET, CBN_SELCHANGE), reinterpret_cast<LPARAM>(st->hTarget));
        };
        // 9f's end, as the script does it: the test row ticked through the list (which leaves the caret on it), Deselect
        // all, and the default GPU chosen again by label.
        const auto endOf9f = [&]() {
            reset();
            preferences[gamePath] = cd::FormatPreferenceValue(mainKey);   // the game on the main GPU, as the screenshot shows
            cd::ActivateGpuPanel(panel, cfg, snap);
            SendMessageW(st->hList, LB_SETCURSEL, static_cast<WPARAM>(lineOf(testPath)), 0);
            space(testPath);
            press(cd::IDC_GPU_CLEARSEL, st->hClearSel);
            chooseBackground();
        };
        const size_t game = [&]() { endOf9f(); return index(gamePath); }();
        Check(game < st->rows.size() && st->rows[game].r.isProfileGame && index(testPath) < st->rows.size(),
              "step-12 replay setup: the test row and a running profile game are both listed");
        Check(st->targetKey == bgKey && !anyTicked() && !IsWindowEnabled(st->hClearSel) && !IsWindowEnabled(st->hApply),
              "step-12 replay: 9f's end - the background GPU chosen, nothing ticked, Deselect all and Apply off");

        // A: THE SEQUENCE THE SCRIPT MEANT. 10b's Profiles -> GPU Assignment is SwitchPage -> ActivateGpuPage -> ActivateGpuPanel.
        cd::ActivateGpuPanel(panel, cfg, snap);
        Check(st->targetKey == bgKey && !anyTicked() && !IsWindowEnabled(st->hClearSel),
              "step-12 replay A: re-entering the tab with a profile game running keeps the target and ticks nothing");
        space(testPath);
        Check(st->rows[index(testPath)].r.selected && IsWindowEnabled(st->hApply),
              "step-12 replay A: ticking the test program then turns Apply on");

        // B: ONE STRAY SPACE after 9f, by the list box's own route (a key pressed while Settings has the foreground):
        // WM_KEYDOWN to the list, which asks its owner with WM_VKEYTOITEM for the CARET row - the row 9f left it on.
        endOf9f();
        SendMessageW(st->hList, WM_KEYDOWN, VK_SPACE, 0);
        Check(st->rows[index(testPath)].r.selected && st->targetKey == bgKey && IsWindowEnabled(st->hApply),
              "step-12 replay B: one Space on the caret row ticks the test program - the 19:36:25 screenshot");
        cd::ActivateGpuPanel(panel, cfg, snap);
        const bool deselectLit = IsWindowEnabled(st->hClearSel) != FALSE;
        Check(st->rows[index(testPath)].r.selected && st->targetKey == bgKey && deselectLit,
              "step-12 replay B: the tick survives 10b's re-entry (same GPU, same row) - 12 reads Deselect all enabled=True");
        space(testPath);   // the script's own TickRow: Space TOGGLES, so it unticks the row the stray key ticked
        Check(!st->rows[index(testPath)].r.selected && !IsWindowEnabled(st->hApply) && !IsWindowEnabled(st->hClearSel) &&
                  st->targetKey == bgKey,
              "step-12 replay B: the script's Space unticks it - Apply enabled=False WITH the target still set");

        processes.erase(70021); processes.erase(70022);
        cfg.profiles.pop_back();
        reset();
    }

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
