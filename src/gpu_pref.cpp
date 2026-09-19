// Game Optimizer - GPU preference and isolation, Win32 implementation.
//
// This TU provides Win32 bindings for GPU enumeration, registry access, and process
// discovery. All pure decision logic is inlined in gpu_pref.h and is unit-testable
// without Win32 dependencies.
#include "gpu_pref.h"

#include <algorithm>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dxgi1_6.h>
#include <ktmw32.h>
#include <cstdint>
#include <dxcore_interface.h>   // the DXCore interfaces only; the factory function is looked up below

#include "util.h"   // LogLine - util.cpp is already linked beside this file in the product, and in every probe build of it

#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "ktmw32.lib")

namespace cd {

// ---- Integrated or discrete: DXCore, resolved late ------------------------------------------------------
//
// DXCoreCreateAdapterFactory is looked up in System32's dxcore.dll at run time, the way theme.cpp resolves
// dwmapi, so the executable gains no import and still starts where the DLL is absent. No dxcore.lib and no
// <initguid.h>: only GetAdapterByLuid and IsIntegrated are used, and their interface ids come from __uuidof.
//
// 🔴 SYSTEM32 ONLY, AND NEVER FREED. A name-only LoadLibrary searches the application's own folder first for a
// DLL that is not a KnownDLL, so a dxcore.dll placed beside the exe would be the one loaded.
typedef HRESULT (WINAPI* PFN_DXCoreCreateAdapterFactory)(REFIID riid, void** factory);

static PFN_DXCoreCreateAdapterFactory DxcoreFactoryFunction() {
    static const PFN_DXCoreCreateAdapterFactory fn = []() -> PFN_DXCoreCreateAdapterFactory {
        const HMODULE m = LoadLibraryExW(L"dxcore.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);   // kept for process life
        if (m == nullptr) return nullptr;
        return reinterpret_cast<PFN_DXCoreCreateAdapterFactory>(
            reinterpret_cast<void*>(GetProcAddress(m, "DXCoreCreateAdapterFactory")));
    }();
    return fn;
}

// Fills each adapter's kind from DXCore's IsIntegrated, matched by LUID: `luids[i]` is `adapters[i]`'s. Whatever
// DXCore cannot answer leaves that entry Unknown, and nothing here can fail the enumeration - the kind decides only
// whether Apply's question carries a warning, never whether the list exists.
//
// [M] igpuprobe, the operator's machine: AMD Radeon(TM) Graphics reads integrated, the RTX 5090 and the RTX 4090
// read discrete, and GetAdapterByLuid refuses the second RTX 5090 entry (E_INVALIDARG), which stays Unknown.
// ponytail: DXCore alone, so an entry it does not know stays Unknown; D3DKMT's HybridIntegrated bit (gdi32, about
// 0.01 ms in the probe) could fill such an entry if a real integrated GPU is ever seen reading Unknown.
//
// 🔴 A GPU KEY LEFT UNKNOWN IS LOGGED, WITH WHY (stage review, v0.5.6). Unknown used to leave no trace, so the log could
// not tell a discrete GPU from "DXCore failed" - and an integrated GPU read as Unknown silently loses Apply's integrated
// warning. One line per enumeration, only when a KEY - what the picker offers - stays Unknown after KindForKey's merge
// (UnknownKindKeyCount): how many keys, and which of the four reasons, with the first HRESULT of each failure kind.
// COUNTED BY KEY SINCE THE COUNCIL REVIEW, v0.5.6: counted by entry, the operator's second RTX 5090 entry - unknown to
// DXCore, its key Discrete through its twin - wrote an "Unknown" line on every visit while no offered GPU was unknown.
static void FillGpuKinds(std::vector<GpuAdapter>& adapters, const std::vector<LUID>& luids) {
    size_t keys = 0;
    const PFN_DXCoreCreateAdapterFactory create = DxcoreFactoryFunction();
    if (create == nullptr) {
        const size_t unknownKeys = UnknownKindKeyCount(adapters, &keys);
        if (unknownKeys > 0)
            LogLine(L"[gpu] integrated or discrete is Unknown for %zu of %zu GPU keys: the DXCore factory is unavailable "
                    L"(dxcore.dll or DXCoreCreateAdapterFactory not found in System32)", unknownKeys, keys);
        return;
    }
    IDXCoreAdapterFactory* dxcore = nullptr;
    const HRESULT made = create(IID_PPV_ARGS(&dxcore));
    if (FAILED(made) || dxcore == nullptr) {
        if (dxcore) dxcore->Release();
        const size_t unknownKeys = UnknownKindKeyCount(adapters, &keys);
        if (unknownKeys > 0)
            LogLine(L"[gpu] integrated or discrete is Unknown for %zu of %zu GPU keys: the DXCore factory is unavailable "
                    L"(DXCoreCreateAdapterFactory hr=0x%08lX)", unknownKeys, keys, static_cast<unsigned long>(made));
        return;
    }
    size_t lookupFailed = 0, unsupported = 0, readFailed = 0;
    HRESULT lookupHr = S_OK, readHr = S_OK;   // the first failure of each kind
    for (size_t i = 0; i < adapters.size() && i < luids.size(); ++i) {
        IDXCoreAdapter* one = nullptr;
        const HRESULT found = dxcore->GetAdapterByLuid(luids[i], &one);
        if (FAILED(found) || one == nullptr) {
            // a success code with no adapter is logged as E_POINTER, never as a failure with hr=0x00000000
            if (lookupFailed++ == 0) lookupHr = FAILED(found) ? found : E_POINTER;
        } else if (!one->IsPropertySupported(DXCoreAdapterProperty::IsIntegrated)) {
            ++unsupported;
        } else {
            bool integrated = false;
            const HRESULT read = one->GetProperty(DXCoreAdapterProperty::IsIntegrated, &integrated);
            if (FAILED(read)) {
                if (readFailed++ == 0) readHr = read;
            } else {
                adapters[i].kind = integrated ? GpuKind::Integrated : GpuKind::Discrete;
            }
        }
        if (one) one->Release();
    }
    dxcore->Release();
    const size_t unknownKeys = UnknownKindKeyCount(adapters, &keys);
    if (unknownKeys > 0) {
        LogLine(L"[gpu] integrated or discrete is Unknown for %zu of %zu GPU keys; of %zu entries, lookup by LUID failed "
                L"%zu (first hr=0x%08lX), property unsupported %zu, read failed %zu (first hr=0x%08lX)",
                unknownKeys, keys, adapters.size(), lookupFailed, static_cast<unsigned long>(lookupHr), unsupported,
                readFailed, static_cast<unsigned long>(readHr));
    }
}

bool EnumerateGpuAdapters(std::vector<GpuAdapter>& out, std::wstring* error) {
    out.clear();
    if (error) error->clear();

    IDXGIFactory1* factory = nullptr;
    HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    if (FAILED(hr)) {
        if (error) *error = L"Failed to create DXGI factory";
        return false;
    }

    // Each listed adapter's LUID, in `out`'s order. DXCore is asked only once the list is complete, so no
    // DXCore object exists on any of the early returns below.
    std::vector<LUID> luids;

    // Enumerate adapters
    UINT index = 0;
    IDXGIAdapter1* adapter = nullptr;
    for (;;) {
        hr = factory->EnumAdapters1(index, &adapter);
        if (hr == DXGI_ERROR_NOT_FOUND) break;   // the normal end of the list
        if (FAILED(hr) || !adapter) {
            // 🔴 ANY OTHER FAILURE IS NOT "NO MORE ADAPTERS". Ending the walk on it returned a list that
            // looked complete and a plan made around the gap (adversarial review, round 3).
            if (adapter) adapter->Release();
            factory->Release();
            out.clear();
            if (error) *error = L"the list of graphics adapters could not be read to the end";
            return false;
        }

        DXGI_ADAPTER_DESC1 desc;
        hr = adapter->GetDesc1(&desc);
        if (FAILED(hr)) {
            // 🔴 AN ADAPTER THAT CANNOT DESCRIBE ITSELF IS NOT SKIPPED. Skipping it returned a list
            // that looked complete while a card was missing from it, and the plan was made around
            // the gap (adversarial review, v0.5.5). No list at all: the window refuses instead.
            adapter->Release();
            factory->Release();
            out.clear();
            if (error) *error = L"a graphics adapter could not be described";
            return false;
        }

        // 🔴 THE MICROSOFT BASIC RENDER DRIVER IS NOT A GPU, AND COUNTING IT IS WORSE THAN
        // COUNTING NOTHING. DXGI enumerates a software rasteriser on Windows 8 and later, so
        // without this test a SINGLE-GPU machine reports two adapters, PlanGpuIsolation's
        // "fewer than 2 GPUs" rule passes, and - because WARP has no output and therefore no
        // display - it is selected as the BACKGROUND GPU on the first pass. Every background
        // app would be pinned to a software renderer and told it succeeded.
        //
        // The whole feature is gated on the adapter COUNT, so this filter is what decides
        // whether the feature offers itself at all on a one-GPU box.
        if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) {
            adapter->Release();
            adapter = nullptr;
            index++;
            continue;
        }

        GpuAdapter gpu;
        gpu.name = desc.Description;
        gpu.vram = desc.DedicatedVideoMemory;

        // Build adapter key from DXGI vendor/device/subsys IDs
        wchar_t keyBuf[64];
        swprintf_s(keyBuf, sizeof(keyBuf) / sizeof(keyBuf[0]), L"%04X&%04X&%08X",
                   desc.VendorId, desc.DeviceId, desc.SubSysId);
        gpu.adapterKey = keyBuf;

        // Does a monitor hang off this adapter? THREE answers, not two: S_OK with an output is
        // yes, DXGI_ERROR_NOT_FOUND is no, and any other result is "could not tell" - which the
        // plan refuses on. The previous line counted every error as a display.
        IDXGIOutput* output = nullptr;
        const HRESULT outHr = adapter->EnumOutputs(0, &output);
        gpu.hasDisplay = SUCCEEDED(outHr) && output != nullptr;
        gpu.displayUnknown = FAILED(outHr) && outHr != DXGI_ERROR_NOT_FOUND;
        if (output) {
            output->Release();
            output = nullptr;
        }

        out.push_back(gpu);
        luids.push_back(desc.AdapterLuid);
        adapter->Release();
        adapter = nullptr;
        index++;
    }

    if (factory) {
        factory->Release();
        factory = nullptr;
    }

    FillGpuKinds(out, luids);
    return true;
}

bool ReadGpuPreference(const std::wstring& exePath, std::wstring& valueOut, bool* unreadable) {
    valueOut.clear();
    if (unreadable) *unreadable = false;

    HKEY hKey;
    LONG result = RegOpenKeyExW(HKEY_CURRENT_USER,
                                L"Software\\Microsoft\\DirectX\\UserGpuPreferences",
                                0, KEY_READ, &hKey);
    if (result != ERROR_SUCCESS) {
        // No key at all is "no preference". Any other failure to open it means a value may be
        // there that this cannot see, and that is reported rather than passed off as absent.
        if (unreadable && result != ERROR_FILE_NOT_FOUND) *unreadable = true;
        return false;
    }

    // THE TYPE IS ASKED FOR AND CHECKED. Passing nullptr here - which this did - accepts a
    // REG_DWORD or a REG_BINARY and copies its raw bytes into a wstring.
    //
    // 🔴 AND THE SIZE IS ASKED FOR, NEVER ASSUMED. A fixed 512-character buffer made any longer
    // value read as "no value". That was harmless while Apply overwrote blindly; now Apply MERGES
    // into what this returns, so a value that failed to read would have had the two GPU fields
    // written over it and every other field in it silently dropped - the exact loss the merge
    // exists to prevent. Found re-reading this function against the merge, v0.5.5.
    // A value that grows between the size query and the read answers ERROR_MORE_DATA. It is asked
    // for again, up to three times; if it is still moving it is reported unreadable below, and the
    // callers leave it alone.
    DWORD type = 0;
    DWORD size = 0;
    std::vector<wchar_t> buffer;
    for (int attempt = 0; attempt < 3; ++attempt) {
        size = 0;
        result = RegQueryValueExW(hKey, exePath.c_str(), nullptr, &type, nullptr, &size);
        if (result != ERROR_SUCCESS) break;
        // +2: room for an odd trailing byte and a terminator the stored data may lack.
        buffer.assign(size / sizeof(wchar_t) + 2, L'\0');
        size = static_cast<DWORD>(buffer.size() * sizeof(wchar_t));
        result = RegQueryValueExW(hKey, exePath.c_str(), nullptr, &type,
                                  reinterpret_cast<LPBYTE>(&buffer[0]), &size);
        if (result != ERROR_MORE_DATA) break;
    }

    RegCloseKey(hKey);

    // 🔴 ABSENT AND UNREADABLE ARE DIFFERENT ANSWERS. Found by adversarial review of v0.5.5, round
    // 2: Apply and Remove took ANY false as "no value" and then wrote over whatever was there. Only
    // ERROR_FILE_NOT_FOUND means absent; every other failure, and a value that is not a string,
    // is a value this must not be used to replace.
    if (result != ERROR_SUCCESS) {
        if (unreadable && result != ERROR_FILE_NOT_FOUND) *unreadable = true;
        return false;
    }
    // ONLY REG_SZ IS EDITABLE. Every write here is REG_SZ, so accepting REG_EXPAND_SZ would change the
    // value's type on the way back; and a string of an odd number of bytes is not text this can hand
    // back unchanged. Both are reported unreadable and left alone (adversarial review, v0.5.5).
    if (type != REG_SZ || (size % sizeof(wchar_t)) != 0) {
        if (unreadable) *unreadable = true;
        return false;
    }

    // 🔴 size IS A BYTE COUNT AND IT CAN BE ZERO. The old form was
    // `size / sizeof(wchar_t) - 1`, which for an empty value computes 0 - 1 as an UNSIGNED
    // DWORD and hands std::wstring a length of 4294967295. An empty REG_SZ is legal and a
    // user can create one by hand, so this was reachable.
    size_t chars = size / sizeof(wchar_t);
    while (chars > 0 && buffer[chars - 1] == L'\0') --chars;   // RegQueryValueEx may or may
                                                                // not include the terminator
    valueOut.assign(buffer.empty() ? L"" : &buffer[0], chars);
    return true;
}

bool WriteGpuPreference(const std::wstring& exePath, const std::wstring& value) {
    HKEY hKey;
    LONG result = RegOpenKeyExW(HKEY_CURRENT_USER,
                                L"Software\\Microsoft\\DirectX\\UserGpuPreferences",
                                0, KEY_WRITE, &hKey);
    if (result != ERROR_SUCCESS) {
        // Create the key if it does not exist
        result = RegCreateKeyExW(HKEY_CURRENT_USER,
                                 L"Software\\Microsoft\\DirectX\\UserGpuPreferences",
                                 0, nullptr, REG_OPTION_NON_VOLATILE, KEY_WRITE, nullptr,
                                 &hKey, nullptr);
        if (result != ERROR_SUCCESS) return false;
    }

    result = RegSetValueExW(hKey, exePath.c_str(), 0, REG_SZ,
                            (const BYTE*)value.c_str(),
                            (DWORD)(value.length() + 1) * sizeof(wchar_t));

    RegCloseKey(hKey);
    return result == ERROR_SUCCESS;
}

// A NEW UTF-16 text file with a byte-order mark - as regedit expects of a .reg - flushed. A partial file is
// deleted if it can be.
static bool WriteNewTextFile(const std::wstring& path, const std::wstring& text) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    const wchar_t bom = 0xFEFF;
    const DWORD bytes = static_cast<DWORD>(text.size() * sizeof(wchar_t));
    DWORD w = 0;
    bool ok = WriteFile(h, &bom, sizeof(bom), &w, nullptr) && w == sizeof(bom);
    ok = ok && WriteFile(h, text.data(), bytes, &w, nullptr) && w == bytes;
    ok = ok && FlushFileBuffers(h);
    CloseHandle(h);
    if (!ok) DeleteFileW(path.c_str());
    return ok;
}

bool BeginGpuRestore(const std::wstring& dir, const std::vector<GpuPreferenceBefore>& planned,
                     GpuRestoreJournal& journal) {
    journal = GpuRestoreJournal();
    if (dir.empty()) return false;
    SYSTEMTIME t;
    GetLocalTime(&t);
    wchar_t name[128];
    swprintf_s(name, 128, L"gpu-preferences-before-%04u%02u%02u-%02u%02u%02u-%03u",
               t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    const std::wstring stem = dir + L"\\" + name;
    // Rows are added to whatever file has the .reg name, so one that already exists is never used.
    if (GetFileAttributesW((stem + L".reg").c_str()) != INVALID_FILE_ATTRIBUTES) return false;
    if (!WriteNewTextFile(stem + L".pending", FormatPendingRestoreFile(planned))) return false;
    journal.pendingPath = stem + L".pending";
    journal.regPath = stem + L".reg";
    return true;
}

bool RecordGpuRestoreRow(GpuRestoreJournal& journal, const GpuPreferenceBefore& row) {
    if (journal.regPath.empty()) return false;
    std::vector<GpuPreferenceBefore> next = journal.recorded;
    next.push_back(row);
    const std::wstring tmp = journal.regPath + L".tmp";
    DeleteFileW(tmp.c_str());   // a leftover from a stopped run is never reused; if it cannot go, the write below fails
    if (!WriteNewTextFile(tmp, FormatRegRestoreFile(next))) return false;
    if (!MoveFileExW(tmp.c_str(), journal.regPath.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(tmp.c_str());
        return false;
    }
    journal.recorded = next;
    journal.regStarted = true;
    return true;
}

bool FinishGpuRestore(const GpuRestoreJournal& journal) {
    if (journal.pendingPath.empty()) return true;
    return DeleteFileW(journal.pendingPath.c_str()) != 0 || GetLastError() == ERROR_FILE_NOT_FOUND;
}

std::vector<std::wstring> UnfinishedGpuRestores(const std::wstring& dir) {
    std::vector<std::wstring> out;
    if (dir.empty()) return out;
    WIN32_FIND_DATAW fd;
    HANDLE f = FindFirstFileW((dir + L"\\gpu-preferences-before-*.pending").c_str(), &fd);
    if (f == INVALID_HANDLE_VALUE) return out;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) out.push_back(dir + L"\\" + fd.cFileName);
    } while (FindNextFileW(f, &fd));
    FindClose(f);
    std::sort(out.begin(), out.end());   // every name carries its time, so name order is time order
    return out;
}

bool ReadRestoreFileText(const std::wstring& path, std::wstring& textOut) {
    textOut.clear();
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                           nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size;
    bool ok = GetFileSizeEx(h, &size) != 0 && size.QuadPart >= 2 && size.QuadPart <= (16LL << 20) &&
              (size.QuadPart % 2) == 0;
    std::vector<wchar_t> buf;
    if (ok) {
        buf.resize(static_cast<size_t>(size.QuadPart / 2));
        DWORD got = 0;
        ok = ReadFile(h, &buf[0], static_cast<DWORD>(size.QuadPart), &got, nullptr) != 0 &&
             static_cast<LONGLONG>(got) == size.QuadPart;
    }
    CloseHandle(h);
    if (!ok || buf.empty() || buf[0] != 0xFEFF) return false;
    textOut.assign(buf.begin() + 1, buf.end());
    return true;
}

GuardedWriteResult GuardedWriteGpuPreference(const std::wstring& exePath, bool expectPresent,
                                             const std::wstring& expectValue, bool deleteValue,
                                             const std::wstring& newValue, unsigned long* error,
                                             GuardedWriteHook hook, void* hookContext) {
    if (error) *error = 0;
    wchar_t description[] = L"Game Optimizer GPU preference";
    HANDLE tx = CreateTransaction(nullptr, nullptr, 0, 0, 0, 10000, description);
    if (tx == INVALID_HANDLE_VALUE) {
        if (error) *error = GetLastError();
        return GuardedWriteResult::Unavailable;
    }
    HKEY key = nullptr;
    LONG r = RegCreateKeyTransactedW(HKEY_CURRENT_USER, L"Software\\Microsoft\\DirectX\\UserGpuPreferences",
                                     0, nullptr, REG_OPTION_NON_VOLATILE, KEY_SET_VALUE | KEY_QUERY_VALUE,
                                     nullptr, &key, nullptr, tx, nullptr);
    if (r != ERROR_SUCCESS) {
        RollbackTransaction(tx);
        CloseHandle(tx);
        if (error) *error = static_cast<unsigned long>(r);
        return GuardedWriteResult::Failed;
    }
    // 1. the change, inside the transaction
    r = deleteValue ? RegDeleteValueW(key, exePath.c_str())
                    : RegSetValueExW(key, exePath.c_str(), 0, REG_SZ,
                                     reinterpret_cast<const BYTE*>(newValue.c_str()),
                                     static_cast<DWORD>((newValue.size() + 1) * sizeof(wchar_t)));
    if (r != ERROR_SUCCESS) {
        RegCloseKey(key);
        RollbackTransaction(tx);
        CloseHandle(tx);
        // A value to delete that is already gone was changed by someone else - said as that, not as a refusal
        // (adversarial review, round 5).
        if (deleteValue && expectPresent && r == ERROR_FILE_NOT_FOUND) return GuardedWriteResult::Changed;
        if (error) *error = static_cast<unsigned long>(r);
        return GuardedWriteResult::Failed;
    }
    if (hook) hook(1, hookContext);
    // 2. the committed value, read OUTSIDE the transaction, must still be what the caller read. A value that
    //    cannot be read is a different answer from a changed one, and is reported as that (round 5).
    std::wstring now;
    bool unreadable = false;
    const bool present = ReadGpuPreference(exePath, now, &unreadable);
    if (unreadable || present != expectPresent || (present && now != expectValue)) {
        RegCloseKey(key);
        RollbackTransaction(tx);
        CloseHandle(tx);
        return unreadable ? GuardedWriteResult::CheckUnreadable : GuardedWriteResult::Changed;
    }
    if (hook) hook(2, hookContext);
    // 3. only then the commit - which fails if anyone wrote the value after step 1. Its error is kept: a
    //    conflict and any other failure are not the same thing to tell a user.
    const BOOL committed = CommitTransaction(tx);
    const DWORD commitError = committed ? 0 : GetLastError();
    RegCloseKey(key);
    CloseHandle(tx);
    if (!committed) {
        if (error) *error = commitError;
        return GuardedWriteResult::NotCommitted;
    }
    return GuardedWriteResult::Written;
}

bool ClearGpuPreference(const std::wstring& exePath) {
    HKEY hKey;
    LONG result = RegOpenKeyExW(HKEY_CURRENT_USER,
                                L"Software\\Microsoft\\DirectX\\UserGpuPreferences",
                                0, KEY_WRITE, &hKey);
    if (result != ERROR_SUCCESS) return false;

    result = RegDeleteValueW(hKey, exePath.c_str());
    RegCloseKey(hKey);

    return result == ERROR_SUCCESS || result == ERROR_FILE_NOT_FOUND;
}

// Every preference in the registry, as (full exe path, adapter key) pairs.
//
// IT REPLACES TWO FUNCTIONS THAT COULD NOT DO THEIR JOBS. RunningExePathsForBasename was a
// stub that returned an empty vector with a comment saying so - the caller now passes a
// process snapshot instead, which the Settings window already keeps. And
// FindOrphanedPreferencePaths read only value NAMES, so it could not supply the adapter keys
// FindOrphanedAssignments needs, and it pre-filtered to non-existent paths, so it could not
// supply the live side of that comparison either.
std::vector<std::pair<std::wstring, std::wstring> > EnumerateGpuPreferences(bool* complete) {
    // Every entry, those with no choice included - this function's contract since v0.5.5, and the write probe's baseline.
    const std::vector<GpuPreferenceEntry> entries = EnumerateGpuPreferenceEntries(complete);
    std::vector<std::pair<std::wstring, std::wstring> > out;
    for (size_t i = 0; i < entries.size(); ++i) out.push_back(std::make_pair(entries[i].path, entries[i].choiceKey));
    return out;
}

std::vector<GpuPreferenceEntry> EnumerateGpuPreferenceEntries(bool* complete) {
    std::vector<GpuPreferenceEntry> out;
    if (complete) *complete = true;

    HKEY hKey = nullptr;
    LONG res = RegOpenKeyExW(HKEY_CURRENT_USER,
                             L"Software\\Microsoft\\DirectX\\UserGpuPreferences",
                             0, KEY_READ, &hKey);
    if (res != ERROR_SUCCESS) {
        // No key at all is "none set". Any other failure means preferences may exist unseen.
        if (complete && res != ERROR_FILE_NOT_FOUND) *complete = false;
        return out;
    }

    // 🔴 THE NAME IS A FULL PATH AND MAX_PATH IS NOT ITS LIMIT. A registry value name may be
    // up to 16,383 characters, and the paths this feature cares about are the deep versioned
    // folders an auto-updating app creates. The previous loop used a MAX_PATH+1 buffer and
    // BROKE on the first ERROR_MORE_DATA, silently dropping every entry after it - so on the
    // exact machine the feature was written for it would have stopped early and reported a
    // clean result. Grow and retry instead; never break on a recoverable error.
    std::vector<wchar_t> name(1024);
    std::vector<wchar_t> data(1024);

    for (DWORD index = 0; ; ++index) {
        DWORD nameLen = static_cast<DWORD>(name.size());
        DWORD dataBytes = static_cast<DWORD>(data.size() * sizeof(wchar_t));
        DWORD type = 0;
        res = RegEnumValueW(hKey, index, &name[0], &nameLen, nullptr, &type,
                            reinterpret_cast<LPBYTE>(&data[0]), &dataBytes);

        if (res == ERROR_MORE_DATA) {
            // Either buffer was short. RegEnumValueW does NOT reliably report the size it wanted
            // for the NAME, so both grow and the same index is retried.
            //
            // 🔴 GROW, THEN ALWAYS RETRY - AND WHEN NOTHING CAN GROW, SKIP THIS ONE ENTRY, NEVER THE
            // REST. Found by adversarial review of v0.5.5: the previous guard resized first and then
            // BROKE OUT once both buffers reached the cap, so the very resize that would have made
            // an entry fit also ended the walk before that entry was retried - and every preference
            // after it went unread, with the function still returning a normal-looking list.
            const size_t kMaxName = 32768;          // a value name is at most 16,383 characters
            const size_t kMaxData = 1 << 20;        // far beyond any real preference string
            const size_t nameBefore = name.size();
            const size_t dataBefore = data.size();
            if (name.size() < kMaxName)
                name.resize(name.size() * 2 < kMaxName ? name.size() * 2 : kMaxName);
            size_t wantData = dataBytes / sizeof(wchar_t) + 1;
            if (wantData <= data.size()) wantData = data.size() * 2;
            if (wantData > kMaxData) wantData = kMaxData;
            if (wantData > data.size()) data.resize(wantData);
            if (name.size() == nameBefore && data.size() == dataBefore) {
                if (complete) *complete = false;    // said, not hidden
                continue;                           // cannot grow further: skip THIS entry only
            }
            --index;                                // retry the same index with larger buffers
            continue;
        }
        if (res != ERROR_SUCCESS) {
            // ERROR_NO_MORE_ITEMS is the normal end; anything else is a partial list, and says so.
            if (complete && res != ERROR_NO_MORE_ITEMS) *complete = false;
            break;
        }

        if (nameLen == 0) continue;
        std::wstring path(&name[0], nameLen);
        if (path.empty()) continue;

        // A card, a Windows setting, or no choice at all - see PreferenceChoiceKey. An entry with no choice is
        // still returned. A value that is not a string, or a string of an odd number of bytes, is "could not
        // be read" - never "not assigned", which is what it showed until adversarial review, round 5.
        // The value's GPU fields are kept beside its key (MakeGpuPreferenceEntry): Apply and Remove compare those fields,
        // and the key alone cannot give them - "GpuPreference=0" and a value with only Windows' own fields both have no
        // key (PrepareVerdict, GpuChoicePairs).
        if ((type == REG_SZ || type == REG_EXPAND_SZ) && (dataBytes % sizeof(wchar_t)) == 0) {
            size_t chars = dataBytes / sizeof(wchar_t);
            while (chars > 0 && data[chars - 1] == L'\0') --chars;
            out.push_back(MakeGpuPreferenceEntry(path, std::wstring(chars ? &data[0] : L"", chars), false));
        } else {
            out.push_back(MakeGpuPreferenceEntry(path, std::wstring(), true));
        }
    }

    RegCloseKey(hKey);
    return out;
}

}  // namespace cd
