// Game Optimizer - small shared helpers.  Implementation of util.h.
//
// Win32 + C++ standard library only.  No third-party dependency, no throwing
// across the module boundary: every entry point returns bool / a value type and
// swallows its own failures.

#include "util.h"

#include <shlobj.h>     // SHGetKnownFolderPath, KF_FLAG_DEFAULT
#include <shellapi.h>   // ShellExecuteW
#include <objbase.h>    // CoTaskMemFree
#include <winsvc.h>     // OpenSCManagerW, QueryServiceStatusEx

#include <cstdarg>
#include <cstdlib>
#include <cerrno>
#include <cwchar>

namespace cd {

namespace {

// FOLDERID_LocalAppData, spelled out locally.
//
// The KNOWNFOLDERID symbols live in uuid.lib, which tools\build.bat does not
// pass to the linker, and pulling <initguid.h> in here would make the GUID
// definition depend on include ordering across the whole build.  A literal is
// link-clean and ordering-proof.  Value: {F1B32785-6FBA-4FCF-9D55-7B8E7F157091}
const GUID kFolderIdLocalAppData =
    { 0xF1B32785, 0x6FBA, 0x4FCF,
      { 0x9D, 0x55, 0x7B, 0x8E, 0x7F, 0x15, 0x70, 0x91 } };

const wchar_t* const kRunKeyPath =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
// The HKCU\...\Run value name. Deliberately unspaced: it is an identifier other tools match
// on, not display text. kLegacyRunValueName is the name this product shipped under before it
// was renamed, and exists only so MigrateLegacyAutostart can clean it up.
const wchar_t* const kRunValueName       = L"GameOptimizer";
const wchar_t* const kLegacyRunValueName = L"CoreDirector";

// Config-folder names under %LOCALAPPDATA%. Same reasoning: a folder name, not display text.
const wchar_t* const kConfigDirName       = L"GameOptimizer";
const wchar_t* const kLegacyConfigDirName = L"CoreDirector";

const wchar_t* const kGameBarKeyPath  = L"Software\\Microsoft\\GameBar";
const wchar_t* const kGameBarValue    = L"AutoGameModeEnabled";

const wchar_t* const kCpu0KeyPath =
    L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0";
const wchar_t* const kCpuBrandValue = L"ProcessorNameString";

// The Svc name is the user-mode service; the other name is the kernel driver. They start
// and stop independently. Measured failure: the operator disabled the service while the
// driver kept running, and the old single-component UI made that look like both were gone.
const wchar_t* const kVCacheServiceName = L"amd3dvcacheSvc";
const wchar_t* const kVCacheDriverName  = L"amd3dvcache";

// Log file is rotated (truncated) once it passes this size.
const unsigned long long kMaxLogBytes = 1024ull * 1024ull;

// ---- tiny registry helpers -------------------------------------------------

bool RegReadString(HKEY root, const wchar_t* subKey, const wchar_t* valueName,
                   std::wstring& out) {
    out.clear();
    HKEY key = nullptr;
    if (::RegOpenKeyExW(root, subKey, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS)
        return false;

    DWORD type = 0;
    DWORD bytes = 0;
    LONG rc = ::RegQueryValueExW(key, valueName, nullptr, &type, nullptr, &bytes);
    if (rc != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ) || bytes == 0) {
        ::RegCloseKey(key);
        return false;
    }

    std::vector<wchar_t> buf(bytes / sizeof(wchar_t) + 2, L'\0');
    DWORD cap = static_cast<DWORD>((buf.size() - 1) * sizeof(wchar_t));
    rc = ::RegQueryValueExW(key, valueName, nullptr, &type,
                            reinterpret_cast<LPBYTE>(buf.data()), &cap);
    ::RegCloseKey(key);
    if (rc != ERROR_SUCCESS) return false;

    buf[buf.size() - 1] = L'\0';
    out.assign(buf.data());
    return true;
}

bool RegReadDword(HKEY root, const wchar_t* subKey, const wchar_t* valueName,
                  DWORD& out) {
    out = 0;
    HKEY key = nullptr;
    if (::RegOpenKeyExW(root, subKey, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS)
        return false;

    DWORD type = 0;
    DWORD data = 0;
    DWORD bytes = sizeof(data);
    LONG rc = ::RegQueryValueExW(key, valueName, nullptr, &type,
                                 reinterpret_cast<LPBYTE>(&data), &bytes);
    ::RegCloseKey(key);
    if (rc != ERROR_SUCCESS || type != REG_DWORD || bytes != sizeof(DWORD))
        return false;
    out = data;
    return true;
}

bool RegKeyExists(HKEY root, const wchar_t* subKey) {
    HKEY key = nullptr;
    if (::RegOpenKeyExW(root, subKey, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS)
        return false;
    ::RegCloseKey(key);
    return true;
}

// ---- log serialisation -----------------------------------------------------

// Function-local static: C++11 magic statics make construction thread-safe, and
// the watcher thread reaches LogLine through the same guard as the UI thread.
CRITICAL_SECTION& LogLock() {
    struct Guard {
        CRITICAL_SECTION cs;
        Guard()  { ::InitializeCriticalSection(&cs); }
        ~Guard() { ::DeleteCriticalSection(&cs); }
    };
    static Guard g;
    return g.cs;
}

// %LOCALAPPDATA% with any trailing separator stripped, or empty when it cannot be found.
// Factored out of GetConfigDir so the rename migration can name the OLD folder without
// duplicating the known-folder lookup and its fallback.
std::wstring LocalAppDataBase() {
    std::wstring base;
    PWSTR raw = nullptr;
    if (SUCCEEDED(::SHGetKnownFolderPath(kFolderIdLocalAppData,
                                         KF_FLAG_DEFAULT, nullptr, &raw)) &&
        raw != nullptr) {
        base.assign(raw);
    }
    if (raw) ::CoTaskMemFree(raw);

    if (base.empty()) {
        // Last-ditch fallback so the app still has somewhere to write.
        wchar_t env[MAX_PATH] = { 0 };
        DWORD n = ::GetEnvironmentVariableW(L"LOCALAPPDATA", env, MAX_PATH);
        if (n > 0 && n < MAX_PATH) base.assign(env, n);
    }

    while (!base.empty() && (base.back() == L'\\' || base.back() == L'/')) {
        base.pop_back();
    }
    return base;
}

bool FileExists(const std::wstring& path) {
    if (path.empty()) return false;
    const DWORD attrs = ::GetFileAttributesW(path.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES &&
           (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

}  // namespace

// ---- Paths -----------------------------------------------------------------

std::wstring GetConfigDir() {
    static const std::wstring dir = []() -> std::wstring {
        const std::wstring base = LocalAppDataBase();
        if (base.empty()) return std::wstring();

        std::wstring full = base + L"\\" + kConfigDirName;
        DWORD attrs = ::GetFileAttributesW(full.c_str());
        if (attrs == INVALID_FILE_ATTRIBUTES) {
            ::CreateDirectoryW(full.c_str(), nullptr);
        }
        return full;
    }();
    return dir;
}

std::wstring GetConfigPath() {
    std::wstring d = GetConfigDir();
    if (d.empty()) return std::wstring();
    return d + L"\\config.ini";
}

std::wstring GetJournalPath() {
    std::wstring d = GetConfigDir();
    if (d.empty()) return std::wstring();
    return d + L"\\applied.journal";
}

std::wstring GetLogPath() {
    std::wstring d = GetConfigDir();
    if (d.empty()) return std::wstring();
    return d + L"\\GameOptimizer.log";
}

std::wstring GetExePath() {
    std::vector<wchar_t> buf(MAX_PATH);
    for (;;) {
        DWORD n = ::GetModuleFileNameW(nullptr, buf.data(),
                                       static_cast<DWORD>(buf.size()));
        if (n == 0) return std::wstring();
        if (n < buf.size()) return std::wstring(buf.data(), n);
        if (buf.size() >= 32768) return std::wstring();
        buf.resize(buf.size() * 2);
    }
}

// ---- Strings ---------------------------------------------------------------

std::string Narrow(const std::wstring& s) {
    if (s.empty()) return std::string();
    int need = ::WideCharToMultiByte(CP_UTF8, 0, s.c_str(),
                                     static_cast<int>(s.size()),
                                     nullptr, 0, nullptr, nullptr);
    if (need <= 0) return std::string();
    std::string out(static_cast<size_t>(need), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()),
                          &out[0], need, nullptr, nullptr);
    return out;
}

std::wstring Widen(const std::string& s) {
    if (s.empty()) return std::wstring();
    int need = ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(),
                                     static_cast<int>(s.size()), nullptr, 0);
    if (need <= 0) return std::wstring();
    std::wstring out(static_cast<size_t>(need), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()),
                          &out[0], need);
    return out;
}

std::wstring ToLower(std::wstring s) {
    if (!s.empty()) {
        ::CharLowerBuffW(&s[0], static_cast<DWORD>(s.size()));
    }
    return s;
}

std::wstring Trim(const std::wstring& s) {
    size_t b = 0;
    size_t e = s.size();
    while (b < e && (s[b] == L' ' || s[b] == L'\t')) ++b;
    while (e > b && (s[e - 1] == L' ' || s[e - 1] == L'\t')) --e;
    return s.substr(b, e - b);
}

std::vector<std::wstring> Split(const std::wstring& s, wchar_t sep) {
    std::vector<std::wstring> out;
    size_t start = 0;
    for (size_t i = 0; i <= s.size(); ++i) {
        if (i == s.size() || s[i] == sep) {
            if (i > start) out.push_back(s.substr(start, i - start));
            start = i + 1;
        }
    }
    return out;
}

std::wstring Join(const std::vector<std::wstring>& v, wchar_t sep) {
    std::wstring out;
    for (size_t i = 0; i < v.size(); ++i) {
        if (i != 0) out.push_back(sep);
        out += v[i];
    }
    return out;
}

std::wstring BaseName(const std::wstring& path) {
    size_t p = path.find_last_of(L"\\/");
    if (p == std::wstring::npos) return path;
    return path.substr(p + 1);
}

bool IEquals(const std::wstring& a, const std::wstring& b) {
    if (a.size() != b.size()) return false;
    if (a.empty()) return true;
    return ::CompareStringOrdinal(a.c_str(), static_cast<int>(a.size()),
                                  b.c_str(), static_cast<int>(b.size()),
                                  TRUE) == CSTR_EQUAL;
}

bool ParseIntW(const std::wstring& s, int& out) {
    std::wstring t = Trim(s);
    if (t.empty()) return false;

    errno = 0;
    wchar_t* end = nullptr;
    long v = ::wcstol(t.c_str(), &end, 10);
    if (end == t.c_str()) return false;
    if (*end != L'\0') return false;
    if (errno == ERANGE) return false;
    if (v < static_cast<long>(INT_MIN) || v > static_cast<long>(INT_MAX))
        return false;

    out = static_cast<int>(v);
    return true;
}

bool ParseUlongW(const std::wstring& s, unsigned long& out) {
    std::wstring t = Trim(s);
    if (t.empty()) return false;
    if (t[0] == L'-') return false;   // wcstoul silently wraps negatives

    errno = 0;
    wchar_t* end = nullptr;
    unsigned long v = ::wcstoul(t.c_str(), &end, 10);
    if (end == t.c_str()) return false;
    if (*end != L'\0') return false;
    if (errno == ERANGE) return false;

    out = v;
    return true;
}

// ---- Files -----------------------------------------------------------------

bool ReadFileUtf8(const std::wstring& path, std::wstring& out) {
    out.clear();
    if (path.empty()) return false;

    HANDLE h = ::CreateFileW(path.c_str(), GENERIC_READ,
                             FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                             OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;   // absent -> false, per header

    LARGE_INTEGER size;
    size.QuadPart = 0;
    if (!::GetFileSizeEx(h, &size)) {
        ::CloseHandle(h);
        return false;
    }
    if (size.QuadPart > 64ll * 1024ll * 1024ll) {   // refuse absurd config files
        ::CloseHandle(h);
        return false;
    }

    std::string raw(static_cast<size_t>(size.QuadPart), '\0');
    size_t got = 0;
    while (got < raw.size()) {
        DWORD want = static_cast<DWORD>(
            (raw.size() - got) > 0x10000000u ? 0x10000000u : (raw.size() - got));
        DWORD read = 0;
        if (!::ReadFile(h, &raw[got], want, &read, nullptr)) {
            ::CloseHandle(h);
            return false;
        }
        if (read == 0) break;
        got += read;
    }
    ::CloseHandle(h);
    raw.resize(got);

    // Strip a UTF-8 BOM if present.
    if (raw.size() >= 3 &&
        static_cast<unsigned char>(raw[0]) == 0xEF &&
        static_cast<unsigned char>(raw[1]) == 0xBB &&
        static_cast<unsigned char>(raw[2]) == 0xBF) {
        raw.erase(0, 3);
    }

    out = Widen(raw);
    return true;
}

bool WriteFileUtf8Atomic(const std::wstring& path, const std::wstring& text) {
    if (path.empty()) return false;

    const std::wstring tmp = path + L".tmp";

    HANDLE h = ::CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr,
                             CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;

    bool ok = true;

    static const unsigned char kBom[3] = { 0xEF, 0xBB, 0xBF };
    DWORD wrote = 0;
    if (!::WriteFile(h, kBom, 3u, &wrote, nullptr) || wrote != 3u) ok = false;

    if (ok) {
        const std::string body = Narrow(text);
        size_t sent = 0;
        while (ok && sent < body.size()) {
            DWORD want = static_cast<DWORD>(
                (body.size() - sent) > 0x10000000u ? 0x10000000u
                                                   : (body.size() - sent));
            DWORD n = 0;
            if (!::WriteFile(h, body.data() + sent, want, &n, nullptr) || n == 0) {
                ok = false;
                break;
            }
            sent += n;
        }
    }

    if (ok) ::FlushFileBuffers(h);
    ::CloseHandle(h);

    if (!ok) {
        ::DeleteFileW(tmp.c_str());
        return false;
    }

    if (!::MoveFileExW(tmp.c_str(), path.c_str(),
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        ::DeleteFileW(tmp.c_str());
        return false;
    }
    return true;
}

int ReadServiceStartValue(const wchar_t* serviceName) {
    if (serviceName == nullptr || serviceName[0] == L'\0') return -1;
    const std::wstring keyPath =
        std::wstring(L"SYSTEM\\CurrentControlSet\\Services\\") + serviceName;
    HKEY key = nullptr;
    if (::RegOpenKeyExW(HKEY_LOCAL_MACHINE, keyPath.c_str(), 0, KEY_QUERY_VALUE, &key) !=
        ERROR_SUCCESS) {
        return -1;
    }

    DWORD type = 0;
    DWORD value = 0;
    DWORD bytes = sizeof(value);
    const LONG rc = ::RegQueryValueExW(key, L"Start", nullptr, &type,
                                       reinterpret_cast<LPBYTE>(&value), &bytes);
    ::RegCloseKey(key);
    if (rc != ERROR_SUCCESS || type != REG_DWORD || bytes != sizeof(value) ||
        value > 0x7ffffffful) {
        return -1;
    }
    return static_cast<int>(value);
}

// ---- Autostart -------------------------------------------------------------

std::wstring AutostartCommand(const std::wstring& exePath) {
    return L"\"" + exePath + L"\" --tray";
}

bool AutostartNeedsMigration(const std::wstring& existingValue) {
    return !existingValue.empty() && ToLower(existingValue).find(L"--tray") ==
                                         std::wstring::npos;
}

bool GetStartWithWindows() {
    HKEY key = nullptr;
    if (::RegOpenKeyExW(HKEY_CURRENT_USER, kRunKeyPath, 0, KEY_QUERY_VALUE,
                        &key) != ERROR_SUCCESS) {
        return false;
    }
    DWORD type = 0;
    DWORD bytes = 0;
    LONG rc = ::RegQueryValueExW(key, kRunValueName, nullptr, &type, nullptr,
                                 &bytes);
    ::RegCloseKey(key);
    return rc == ERROR_SUCCESS && bytes > 0;
}

bool SetStartWithWindows(bool on) {
    if (on) {
        const std::wstring exe = GetExePath();
        if (exe.empty()) return false;
        const std::wstring command = AutostartCommand(exe);

        HKEY key = nullptr;
        if (::RegCreateKeyExW(HKEY_CURRENT_USER, kRunKeyPath, 0, nullptr,
                              REG_OPTION_NON_VOLATILE, KEY_SET_VALUE, nullptr,
                              &key, nullptr) != ERROR_SUCCESS) {
            return false;
        }
        const DWORD bytes =
            static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t));
        LONG rc = ::RegSetValueExW(
            key, kRunValueName, 0, REG_SZ,
            reinterpret_cast<const BYTE*>(command.c_str()), bytes);
        ::RegCloseKey(key);
        return rc == ERROR_SUCCESS;
    }

    HKEY key = nullptr;
    LONG open = ::RegOpenKeyExW(HKEY_CURRENT_USER, kRunKeyPath, 0, KEY_SET_VALUE,
                                &key);
    if (open == ERROR_FILE_NOT_FOUND) return true;   // nothing to remove
    if (open != ERROR_SUCCESS) return false;

    LONG rc = ::RegDeleteValueW(key, kRunValueName);
    ::RegCloseKey(key);
    return rc == ERROR_SUCCESS || rc == ERROR_FILE_NOT_FOUND;
}

void MigrateAutostartCommand() {
    // Builds before the manual-launch Settings behavior wrote only the quoted exe path. Once
    // manual launches became visible, leaving that measured flagless form in place would
    // turn every existing user's next login into an unwanted Settings popup.
    HKEY key = nullptr;
    if (::RegOpenKeyExW(HKEY_CURRENT_USER, kRunKeyPath, 0, KEY_QUERY_VALUE,
                        &key) != ERROR_SUCCESS) {
        return;
    }

    DWORD type = 0;
    DWORD bytes = 0;
    LONG query = ::RegQueryValueExW(key, kRunValueName, nullptr, &type, nullptr, &bytes);
    if (query != ERROR_SUCCESS || bytes == 0 ||
        (type != REG_SZ && type != REG_EXPAND_SZ)) {
        ::RegCloseKey(key);
        return;
    }

    std::vector<wchar_t> value(static_cast<size_t>(bytes / sizeof(wchar_t)) + 1, L'\0');
    query = ::RegQueryValueExW(key, kRunValueName, nullptr, &type,
                              reinterpret_cast<BYTE*>(value.data()), &bytes);
    ::RegCloseKey(key);
    if (query != ERROR_SUCCESS) return;

    const std::wstring existingValue(value.data());
    if (!AutostartNeedsMigration(existingValue)) return;

    const std::wstring exe = GetExePath();
    if (exe.empty()) return;
    const std::wstring command = AutostartCommand(exe);
    const DWORD commandBytes =
        static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t));

    HKEY writeKey = nullptr;
    const LONG open = ::RegOpenKeyExW(HKEY_CURRENT_USER, kRunKeyPath, 0,
                                      KEY_SET_VALUE, &writeKey);
    if (open != ERROR_SUCCESS) {
        LogLine(L"[migrate] flagless autostart command could not be updated, err=%ld", open);
        return;
    }
    const LONG write = ::RegSetValueExW(
        writeKey, kRunValueName, 0, REG_SZ,
        reinterpret_cast<const BYTE*>(command.c_str()), commandBytes);
    ::RegCloseKey(writeKey);

    if (write == ERROR_SUCCESS) {
        LogLine(L"[migrate] autostart command updated from '%s' to '%s'",
                existingValue.c_str(), command.c_str());
    } else {
        LogLine(L"[migrate] flagless autostart command could not be updated, err=%ld", write);
    }
}

// ---- Migration from the previous product name ------------------------------
//
// This product shipped as "CoreDirector" before it was renamed to "Game Optimizer".  Both
// of the following run once at startup, BEFORE anything reads the journal or the config.
// Without them the rename is silently destructive to an existing user, in two ways that
// look like nothing at all went wrong.

void MigrateLegacyConfigDir() {
    const std::wstring base = LocalAppDataBase();
    if (base.empty()) return;

    const std::wstring legacyDir = base + L"\\" + kLegacyConfigDirName;
    const std::wstring legacyCfg = legacyDir + L"\\config.ini";
    if (!FileExists(legacyCfg)) return;          // nothing from the old name to carry over

    // GetConfigPath() creates the new folder as a side effect of GetConfigDir().
    const std::wstring newCfg = GetConfigPath();
    if (newCfg.empty()) return;
    if (FileExists(newCfg)) return;              // already configured under the new name

    // COPY, never move.  This is the user's settings file: a copy is reversible and leaves
    // the old folder intact for anyone who wants to roll back to the previous build.
    // bFailIfExists = TRUE so a race cannot clobber a config written since the check above.
    if (!::CopyFileW(legacyCfg.c_str(), newCfg.c_str(), TRUE)) {
        LogLine(L"[migrate] could not copy config.ini out of %s, err=%lu - starting from "
                L"defaults instead", legacyDir.c_str(), ::GetLastError());
        return;
    }
    LogLine(L"[migrate] config.ini migrated from %s (the folder used before the rename); "
            L"the original was left in place", legacyDir.c_str());

    // The journal only means anything alongside the config it was written with, so it moves
    // with it and only with it.  A stale journal on its own would name pids from another era.
    const std::wstring legacyJournal = legacyDir + L"\\applied.journal";
    const std::wstring newJournal    = GetJournalPath();
    if (FileExists(legacyJournal) && !newJournal.empty() && !FileExists(newJournal)) {
        if (::CopyFileW(legacyJournal.c_str(), newJournal.c_str(), TRUE)) {
            LogLine(L"[migrate] applied.journal migrated from %s", legacyDir.c_str());
        } else {
            LogLine(L"[migrate] could not copy applied.journal out of %s, err=%lu",
                    legacyDir.c_str(), ::GetLastError());
        }
    }
}

void MigrateLegacyAutostart() {
    // The old HKCU\...\Run value named the OLD exe, which no longer exists after the rename.
    // Left alone it fails silently at every login and stays in the registry forever.
    HKEY key = nullptr;
    if (::RegOpenKeyExW(HKEY_CURRENT_USER, kRunKeyPath, 0,
                        KEY_QUERY_VALUE | KEY_SET_VALUE, &key) != ERROR_SUCCESS) {
        return;                                   // no Run key: nothing to repair
    }

    DWORD type = 0;
    DWORD bytes = 0;
    const LONG q = ::RegQueryValueExW(key, kLegacyRunValueName, nullptr, &type, nullptr,
                                      &bytes);
    const bool legacyPresent = (q == ERROR_SUCCESS && bytes > 0);
    if (!legacyPresent) {
        // Nothing under the old name.  In particular this does NOT create the new value:
        // a user who deliberately turned autostart off must not have it switched back on.
        ::RegCloseKey(key);
        return;
    }

    const LONG del = ::RegDeleteValueW(key, kLegacyRunValueName);
    ::RegCloseKey(key);
    if (del != ERROR_SUCCESS && del != ERROR_FILE_NOT_FOUND) {
        LogLine(L"[migrate] the stale '%s' autostart value could not be removed, err=%ld - "
                L"it still points at an exe that no longer exists",
                kLegacyRunValueName, del);
        return;
    }
    LogLine(L"[migrate] removed the stale '%s' autostart value - it pointed at the exe name "
            L"used before the rename", kLegacyRunValueName);

    // Re-created ONLY because the old value was actually there, i.e. only for a user who
    // had autostart switched on.  This is what makes the migration safe to run every start.
    if (SetStartWithWindows(true)) {
        LogLine(L"[migrate] autostart re-registered as '%s' -> %s",
                kRunValueName, GetExePath().c_str());
    } else {
        LogLine(L"[migrate] autostart could NOT be re-registered as '%s'; it is now off",
                kRunValueName);
    }
}

// ---- Environment probes ----------------------------------------------------

void RefreshEnvironmentStatus(EnvironmentInfo& info) {
    // Reset first so a failed refresh can never leave a stale, confident answer on screen.
    info.gameModeKeyPresent = false;
    info.autoGameModeEnabled = 0;
    info.gameModeState = GameModeState::NotDeterminable;

    // One key open and one value query. A missing/unreadable value is not the same fact as
    // OFF, so only a successfully read DWORD produces an On/Off state.
    HKEY gameBar = nullptr;
    if (::RegOpenKeyExW(HKEY_CURRENT_USER, kGameBarKeyPath, 0, KEY_QUERY_VALUE,
                        &gameBar) == ERROR_SUCCESS) {
        info.gameModeKeyPresent = true;
        DWORD type = 0;
        DWORD value = 0;
        DWORD bytes = sizeof(value);
        if (::RegQueryValueExW(gameBar, kGameBarValue, nullptr, &type,
                               reinterpret_cast<LPBYTE>(&value), &bytes) == ERROR_SUCCESS &&
            type == REG_DWORD && bytes == sizeof(value)) {
            info.autoGameModeEnabled = value;
            info.gameModeState = value == 0 ? GameModeState::Off : GameModeState::On;
        }
        ::RegCloseKey(gameBar);
    }

    info.amdVCacheServicePresent = false;
    info.amdVCacheServiceRunning = false;
    info.amdVCacheServiceState = AmdVCacheServiceState::NotDeterminable;
    info.amdVCacheDriverPresent = false;
    info.amdVCacheDriverRunning = false;
    info.amdVCacheDriverState = AmdVCacheServiceState::NotDeterminable;

    // Open exactly the two components we care about. No EnumServicesStatusEx call belongs
    // on a one-second UI timer. Every operation here is query-only.
    SC_HANDLE scm = ::OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (scm == nullptr) return;

    const auto queryComponent = [scm](const wchar_t* name, bool& present, bool& running,
                                      AmdVCacheServiceState& state) {
        ::SetLastError(ERROR_SUCCESS);
        SC_HANDLE component = ::OpenServiceW(scm, name, SERVICE_QUERY_STATUS);
        if (component == nullptr) {
            if (::GetLastError() == ERROR_SERVICE_DOES_NOT_EXIST) {
                state = AmdVCacheServiceState::NotInstalled;
            }
            return;
        }

        present = true;
        SERVICE_STATUS_PROCESS ssp;
        ::ZeroMemory(&ssp, sizeof(ssp));
        DWORD needed = 0;
        if (::QueryServiceStatusEx(component, SC_STATUS_PROCESS_INFO,
                                   reinterpret_cast<LPBYTE>(&ssp), sizeof(ssp), &needed)) {
            if (ssp.dwCurrentState == SERVICE_RUNNING) {
                running = true;
                state = AmdVCacheServiceState::Running;
            } else if (ssp.dwCurrentState == SERVICE_STOPPED) {
                state = AmdVCacheServiceState::InstalledButStopped;
            }
            // Paused and transition states are deliberately NotDeterminable: calling either
            // one "stopped" would be a claim the service manager did not return.
        }
        ::CloseServiceHandle(component);
    };

    queryComponent(kVCacheServiceName, info.amdVCacheServicePresent,
                   info.amdVCacheServiceRunning, info.amdVCacheServiceState);
    queryComponent(kVCacheDriverName, info.amdVCacheDriverPresent,
                   info.amdVCacheDriverRunning, info.amdVCacheDriverState);
    ::CloseServiceHandle(scm);
}

EnvironmentInfo ProbeEnvironment() {
    EnvironmentInfo info;

    // CPU brand string.
    if (RegReadString(HKEY_LOCAL_MACHINE, kCpu0KeyPath, kCpuBrandValue,
                      info.cpuBrand)) {
        info.cpuBrand = Trim(info.cpuBrand);
    }
    if (!info.cpuBrand.empty()) {
        const std::wstring lower = ToLower(info.cpuBrand);
        info.isAmd   = lower.find(L"amd")   != std::wstring::npos;
        info.isIntel = lower.find(L"intel") != std::wstring::npos;
    }

    RefreshEnvironmentStatus(info);

    // Elevation.
    HANDLE token = nullptr;
    if (::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &token)) {
        TOKEN_ELEVATION elev;
        ::ZeroMemory(&elev, sizeof(elev));
        DWORD got = 0;
        if (::GetTokenInformation(token, TokenElevation, &elev, sizeof(elev),
                                  &got) && got == sizeof(elev)) {
            info.isElevated = (elev.TokenIsElevated != 0);
        }
        ::CloseHandle(token);
    }

    return info;
}

void OpenGameModeSettings() {
    ::ShellExecuteW(nullptr, L"open", L"ms-settings:gaming-gamemode", nullptr,
                    nullptr, SW_SHOWNORMAL);
}

void OpenFolder(const std::wstring& path) {
    if (path.empty()) return;
    ::ShellExecuteW(nullptr, L"open", path.c_str(), nullptr, nullptr,
                    SW_SHOWNORMAL);
}

// ---- Logging ---------------------------------------------------------------

void LogLine(const wchar_t* fmt, ...) {
    if (fmt == nullptr) return;

    wchar_t body[2048];
    body[0] = L'\0';

    va_list args;
    va_start(args, fmt);
    int n = ::_vsnwprintf_s(body, _countof(body), _TRUNCATE, fmt, args);
    va_end(args);
    if (n < 0) {
        // _TRUNCATE already handled overflow; a negative result here means the
        // format itself failed, so there is nothing worth writing.
        body[_countof(body) - 1] = L'\0';
    }

    const std::wstring path = GetLogPath();
    if (path.empty()) return;

    SYSTEMTIME st;
    ::GetLocalTime(&st);

    wchar_t stamp[64];
    ::_snwprintf_s(stamp, _countof(stamp), _TRUNCATE,
                   L"[%04u-%02u-%02u %02u:%02u:%02u.%03u] ",
                   static_cast<unsigned>(st.wYear),
                   static_cast<unsigned>(st.wMonth),
                   static_cast<unsigned>(st.wDay),
                   static_cast<unsigned>(st.wHour),
                   static_cast<unsigned>(st.wMinute),
                   static_cast<unsigned>(st.wSecond),
                   static_cast<unsigned>(st.wMilliseconds));

    std::wstring line;
    line.reserve(64 + ::wcslen(body) + 2);
    line.append(stamp);
    line.append(body);
    line.append(L"\r\n");

    const std::string utf8 = Narrow(line);

    ::EnterCriticalSection(&LogLock());

    // Rotate before appending, so the file never grows unbounded on a machine
    // left running for weeks.
    WIN32_FILE_ATTRIBUTE_DATA fad;
    ::ZeroMemory(&fad, sizeof(fad));
    if (::GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad)) {
        const unsigned long long size =
            (static_cast<unsigned long long>(fad.nFileSizeHigh) << 32) |
            static_cast<unsigned long long>(fad.nFileSizeLow);
        if (size > kMaxLogBytes) {
            HANDLE t = ::CreateFileW(path.c_str(), GENERIC_WRITE,
                                     FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                                     FILE_ATTRIBUTE_NORMAL, nullptr);
            if (t != INVALID_HANDLE_VALUE) ::CloseHandle(t);
        }
    }

    HANDLE h = ::CreateFileW(path.c_str(), FILE_APPEND_DATA,
                             FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                             OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        LARGE_INTEGER cur;
        cur.QuadPart = 0;
        DWORD wrote = 0;
        if (::GetFileSizeEx(h, &cur) && cur.QuadPart == 0) {
            static const unsigned char kBom[3] = { 0xEF, 0xBB, 0xBF };
            ::WriteFile(h, kBom, 3u, &wrote, nullptr);
        }
        if (!utf8.empty()) {
            ::WriteFile(h, utf8.data(), static_cast<DWORD>(utf8.size()), &wrote,
                        nullptr);
        }
        ::CloseHandle(h);
    }

    ::LeaveCriticalSection(&LogLock());
}

}  // namespace cd
