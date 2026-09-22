// guarded_write_probe - an OPT-IN pre-release check, run by tools\probe-guarded-write.bat. NOT part of Gate A: it writes
// two dummy values to the current user's real Windows GPU preferences and removes them.
//
// WHAT IT PROVES, AND WHY NOTHING ELSE DOES. v0.5.7 asks a row Auto assign ticked once more INSIDE its guarded write:
// cd::GuardedWriteGpuPreference calls StillAutoEligible (gpuwindow.cpp) after the transacted change and before the
// commit, and that walks HKCU\Software\Microsoft\DirectX\UserGpuPreferences again with the real
// cd::EnumerateGpuPreferenceEntries while this process holds an OPEN registry transaction on the same key. Were that
// walk to come back incomplete, or to see the uncommitted value, every Auto-assigned Apply would be refused. The unit
// tests and the panel harness cannot see it: neither opens a transaction on the real key. This runs the REAL enumerator
// and the REAL rule (cd::SelectForAutoAssign) inside the REAL guarded write, on two dummy values.
//
// FIDELITY LIMIT: StillAutoEligible and ChoicesByPath sit in an unnamed namespace in gpuwindow.cpp and cannot be linked,
// so StillAutoEligibleMirror repeats their lines below; every function it calls is the product's own.
// ponytail: the other writer is a second RegOpenKeyExW handle in THIS process on a helper thread, one step at a time
// (as in txprobe3 and writeprobe057); a writer in another process racing at full speed is not measured.
//
// SAFETY: refuses to run if either dummy value exists; fingerprints every value of the key (names, types, data -
// hashed, never printed) before and after; deletes both dummy values at the end; writes no other value.
// EXIT: 0 every scenario as expected AND the fingerprint unchanged; 1 otherwise; 2 refused because a dummy value
// exists; 3 refused because a precondition failed.
#include <windows.h>
#include <cstdio>
#include <cstdarg>
#include <string>
#include <vector>
#include <map>
#include <algorithm>
#include "gpu_rows.h"

namespace {

const wchar_t* kKey = L"Software\\Microsoft\\DirectX\\UserGpuPreferences";
// Two versions of one application under one install root, so SameInstallRoot holds and the sibling rule applies.
const wchar_t* kNew = L"C:\\GameOptimizerProbe\\gwp\\Chat\\app-2.0\\chat.exe";
const wchar_t* kOld = L"C:\\GameOptimizerProbe\\gwp\\Chat\\app-1.0\\chat.exe";
const wchar_t* kFallbackGameKey = L"10DE&2B85&53021462";
const wchar_t* kFallbackTargetKey = L"10DE&2684&40BF1458";

std::wstring g_gameKey, g_targetKey, g_gamePin, g_targetPin;
double g_qpcFreq = 1.0;
std::vector<double> g_inTx, g_outTx;   // the check's elapsed microseconds: inside the guarded write, and with none open

void Log(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    fflush(stdout);
}

std::string Narrow(const std::wstring& w) {
    std::string s;
    for (wchar_t c : w) s += (c < 128) ? static_cast<char>(c) : '?';
    return s;
}

double MicrosSince(const LARGE_INTEGER& t0) {
    LARGE_INTEGER t1;
    QueryPerformanceCounter(&t1);
    return static_cast<double>(t1.QuadPart - t0.QuadPart) * 1e6 / g_qpcFreq;
}

// ---------- fingerprint of every value in the key (same scheme as writeprobe057) ----------
unsigned long long Fnv(const void* p, size_t n, unsigned long long h = 1469598103934665603ULL) {
    const unsigned char* b = static_cast<const unsigned char*>(p);
    for (size_t i = 0; i < n; ++i) { h ^= b[i]; h *= 1099511628211ULL; }
    return h;
}
struct Val { std::wstring name; DWORD type; std::vector<BYTE> data; };
bool Fingerprint(std::string& text, size_t& valueCount) {
    text.clear();
    valueCount = 0;
    HKEY h = nullptr;
    LONG r = RegOpenKeyExW(HKEY_CURRENT_USER, kKey, 0, KEY_READ, &h);
    if (r != ERROR_SUCCESS) { text = "open failed rc=" + std::to_string(r) + "\n"; return false; }
    DWORD subkeys = 0, nvals = 0, maxName = 0, maxData = 0;
    r = RegQueryInfoKeyW(h, nullptr, nullptr, nullptr, &subkeys, nullptr, nullptr, &nvals, &maxName, &maxData, nullptr, nullptr);
    if (r != ERROR_SUCCESS) { RegCloseKey(h); text = "queryinfo failed\n"; return false; }
    std::vector<Val> vals;
    std::vector<wchar_t> nb(maxName + 64);
    std::vector<BYTE> db(maxData + 256);
    for (DWORD i = 0;;) {
        DWORD nl = static_cast<DWORD>(nb.size()), dl = static_cast<DWORD>(db.size()), type = 0;
        r = RegEnumValueW(h, i, nb.data(), &nl, nullptr, &type, db.data(), &dl);
        if (r == ERROR_NO_MORE_ITEMS) break;
        if (r == ERROR_MORE_DATA) { nb.resize(nb.size() * 2); db.resize(db.size() * 2); continue; }
        if (r != ERROR_SUCCESS) { RegCloseKey(h); text = "enum failed rc=" + std::to_string(r) + "\n"; return false; }
        Val v; v.name.assign(nb.data(), nl); v.type = type; v.data.assign(db.begin(), db.begin() + dl);
        vals.push_back(v);
        ++i;
    }
    RegCloseKey(h);
    std::sort(vals.begin(), vals.end(), [](const Val& a, const Val& b) { return a.name < b.name; });
    unsigned long long all = 1469598103934665603ULL;
    char line[256];
    std::string body;
    for (size_t i = 0; i < vals.size(); ++i) {
        const Val& v = vals[i];
        snprintf(line, sizeof(line), "value %03zu name#%016llx type=%lu len=%zu data#%016llx\n", i,
                 Fnv(v.name.data(), v.name.size() * sizeof(wchar_t)), static_cast<unsigned long>(v.type), v.data.size(),
                 Fnv(v.data.data(), v.data.size()));
        body += line;
        const wchar_t zero = 0;
        const DWORD len = static_cast<DWORD>(v.data.size());
        all = Fnv(v.name.data(), v.name.size() * sizeof(wchar_t), all);
        all = Fnv(&zero, sizeof(zero), all);
        all = Fnv(&v.type, sizeof(v.type), all);
        all = Fnv(&len, sizeof(len), all);
        all = Fnv(v.data.data(), v.data.size(), all);
    }
    snprintf(line, sizeof(line), "subkeys=%lu values=%zu overall#%016llx\n", static_cast<unsigned long>(subkeys),
             vals.size(), all);
    text = line + body;
    valueCount = vals.size();
    return true;
}
void WriteText(const std::wstring& path, const std::string& text) {
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"wb") != 0 || !f) return;
    fwrite(text.data(), 1, text.size(), f);
    fclose(f);
}

// ---------- plain (non-transacted) helpers, each on its own fresh handle ----------
bool ReadPlain(const wchar_t* name, std::wstring& value, LONG* rcOut = nullptr) {
    value.clear();
    HKEY h = nullptr;
    LONG rc = RegOpenKeyExW(HKEY_CURRENT_USER, kKey, 0, KEY_READ, &h);
    if (rc == ERROR_SUCCESS) {
        wchar_t buf[512]; DWORD size = sizeof(buf) - sizeof(wchar_t), type = 0;
        rc = RegQueryValueExW(h, name, nullptr, &type, reinterpret_cast<BYTE*>(buf), &size);
        RegCloseKey(h);
        if (rc == ERROR_SUCCESS) { buf[size / sizeof(wchar_t)] = 0; value = buf; }
    }
    if (rcOut) *rcOut = rc;
    return rc == ERROR_SUCCESS;
}
std::string Show(const wchar_t* name) {
    std::wstring v; LONG rc = 0;
    if (ReadPlain(name, v, &rc)) return "\"" + Narrow(v) + "\"";
    return rc == ERROR_FILE_NOT_FOUND ? "absent" : "unreadable rc=" + std::to_string(rc);
}
// The same, short: absent, or the GPU choice the value names (PreferenceChoiceKey).
std::string ShowShort(const wchar_t* name) {
    std::wstring v; LONG rc = 0;
    if (ReadPlain(name, v, &rc)) return "pin:" + Narrow(cd::PreferenceChoiceKey(v));
    return rc == ERROR_FILE_NOT_FOUND ? "absent" : "rc=" + std::to_string(rc);
}
LONG SetPlain(const wchar_t* name, const wchar_t* value) {
    HKEY h = nullptr;
    LONG rc = RegOpenKeyExW(HKEY_CURRENT_USER, kKey, 0, KEY_SET_VALUE, &h);
    if (rc != ERROR_SUCCESS) return rc;
    rc = RegSetValueExW(h, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value),
                        static_cast<DWORD>((wcslen(value) + 1) * sizeof(wchar_t)));
    RegCloseKey(h);
    return rc;
}
LONG DeletePlain(const wchar_t* name) {
    HKEY h = nullptr;
    LONG rc = RegOpenKeyExW(HKEY_CURRENT_USER, kKey, 0, KEY_SET_VALUE, &h);
    if (rc != ERROR_SUCCESS) return rc;
    rc = RegDeleteValueW(h, name);
    RegCloseKey(h);
    return rc;
}

// The other writer: a separate RegOpenKeyExW handle on a helper thread, waited on for 5 s - a block is reported, not hung on.
struct OtherWrite { const wchar_t* name; const wchar_t* value; LONG rc; };
DWORD WINAPI OtherWriteThread(void* p) {
    OtherWrite* w = static_cast<OtherWrite*>(p);
    w->rc = SetPlain(w->name, w->value);
    return 0;
}
void RunOtherWrite(OtherWrite& w, bool& blocked) {
    w.rc = -1;
    blocked = false;
    HANDLE t = CreateThread(nullptr, 0, OtherWriteThread, &w, 0, nullptr);
    if (!t) return;
    blocked = WaitForSingleObject(t, 5000) != WAIT_OBJECT_0;
    if (!blocked) CloseHandle(t);   // a blocked thread's handle is left open on purpose; the run is reported as blocked
}

// ---------- the check, as the product makes it ----------
// NEW as the GPU Assignment tab holds it after Auto assign ticked it: an application that is not a profile's game, with no
// GPU value of its own and no lost assignment.
cd::GpuRow ProbeRow() {
    cd::GpuRow row;
    row.exeName = L"chat.exe";
    row.exePath = kNew;
    row.selected = true;
    return row;
}

std::string SeenAs(const std::map<std::wstring, std::wstring>& choices, const wchar_t* path) {
    const std::map<std::wstring, std::wstring>::const_iterator f = choices.find(cd::ToLower(path));
    if (f == choices.end()) return "absent";
    return f->second.empty() ? "no-choice" : Narrow(f->second);
}

struct CheckResult {
    bool answer = false;
    bool complete = false;
    double micros = 0;
    size_t entries = 0;
    std::string seen;   // what the walk found for NEW and OLD
};

// gpuwindow.cpp StillAutoEligible, line for line (the timed part); only the row comes from ProbeRow instead of st->rows.
CheckResult StillAutoEligibleMirror() {
    CheckResult out;
    LARGE_INTEGER t0;
    QueryPerformanceCounter(&t0);
    bool complete = true;
    const std::vector<cd::GpuPreferenceEntry> entries = cd::EnumerateGpuPreferenceEntries(&complete);
    std::map<std::wstring, std::wstring> choices;   // ChoicesByPath
    for (const auto& entry : entries) choices[cd::ToLower(entry.path)] = entry.choiceKey;
    cd::GpuRow row = ProbeRow();
    const std::map<std::wstring, std::wstring>::const_iterator found = choices.find(cd::ToLower(row.exePath));
    row.assignedKey = found != choices.end() ? found->second : std::wstring();
    out.answer = cd::SelectForAutoAssign(std::vector<cd::GpuRow>(1, row), g_targetKey, g_gameKey, complete,
                                         cd::GpuChoicePairs(entries))[0].selected;
    out.micros = MicrosSince(t0);
    out.complete = complete;
    out.entries = entries.size();
    out.seen = "NEW=" + SeenAs(choices, kNew) + " OLD=" + SeenAs(choices, kOld);
    return out;
}

// ---------- per-run context and the callbacks handed to the guarded write ----------
struct Ctx {
    int calls = 0;              // check calls inside the guarded write
    bool allComplete = true;    // every one of them reported complete=true
    bool answer = false;        // the last one's answer
    double micros = -1;         // the last one's elapsed time
    size_t entries = 0;
    std::string seen;
    int hook1 = 0, hook2 = 0;
    LONG otherRc = -1;
    bool otherBlocked = false;
};

void Record(Ctx* x, const CheckResult& c) {
    ++x->calls;
    x->allComplete = x->allComplete && c.complete;
    x->answer = c.answer;
    x->micros = c.micros;
    x->entries = c.entries;
    x->seen = c.seen;
    g_inTx.push_back(c.micros);
}
bool CheckPlain(void* p) {
    Ctx* x = static_cast<Ctx*>(p);
    const CheckResult c = StillAutoEligibleMirror();
    Record(x, c);
    return c.answer;
}
// P4: the check reads and answers as the product's does; OLD is pinned to the main GPU only AFTER its walk.
bool CheckThenPinOld(void* p) {
    Ctx* x = static_cast<Ctx*>(p);
    const CheckResult c = StillAutoEligibleMirror();
    Record(x, c);
    OtherWrite w = { kOld, g_gamePin.c_str(), -1 };
    RunOtherWrite(w, x->otherBlocked);
    x->otherRc = w.rc;
    return c.answer;
}
// P3: OLD is pinned to the main GPU after the transacted change and before the comparison and the check.
void HookPinOld(int stage, void* p) {
    Ctx* x = static_cast<Ctx*>(p);
    if (stage == 1) {
        ++x->hook1;
        OtherWrite w = { kOld, g_gamePin.c_str(), -1 };
        RunOtherWrite(w, x->otherBlocked);
        x->otherRc = w.rc;
    } else if (stage == 2) {
        ++x->hook2;
    }
}

const char* ResultName(cd::GuardedWriteResult g) {
    switch (g) {
        case cd::GuardedWriteResult::Written: return "Written";
        case cd::GuardedWriteResult::Changed: return "Changed";
        case cd::GuardedWriteResult::CheckUnreadable: return "CheckUnreadable";
        case cd::GuardedWriteResult::NotCommitted: return "NotCommitted";
        case cd::GuardedWriteResult::Unavailable: return "Unavailable";
        case cd::GuardedWriteResult::Failed: return "Failed";
        case cd::GuardedWriteResult::NoLongerAllowed: return "NoLongerAllowed";
    }
    return "?";
}

bool ValidKey(const std::wstring& k) {
    return k.size() == 18 && cd::PreferenceChoiceKey(cd::FormatPreferenceValue(k)) == k;
}

long long FileBytes(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA fad;
    if (path.empty() || !GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad)) return -1;
    return (static_cast<long long>(fad.nFileSizeHigh) << 32) | fad.nFileSizeLow;
}

void Stats(const char* label, std::vector<double> v) {
    if (v.empty()) { Log("%s: no calls\n", label); return; }
    std::sort(v.begin(), v.end());
    const size_t n = v.size();
    const double median = (n % 2) ? v[n / 2] : (v[n / 2 - 1] + v[n / 2]) / 2;
    Log("%s: calls=%zu min=%.1f us median=%.1f us max=%.1f us\n", label, n, v.front(), median, v.back());
}

struct Row {
    int scenario; int rep; std::string result; unsigned long err; int calls; std::string complete; double micros;
    bool ruleBefore; std::string finalNew, finalOld; bool pass;
};

}  // namespace

int main() {
    LARGE_INTEGER freq;
    QueryPerformanceFrequency(&freq);
    g_qpcFreq = static_cast<double>(freq.QuadPart);

    Log("guarded_write_probe - the product's cd::GuardedWriteGpuPreference with Auto assign's in-write check, against the\n"
        "real registry, on two dummy values only. OPT-IN pre-release check; not part of Gate A.\n");
    {
        typedef LONG(WINAPI * RtlGetVersionFn)(PRTL_OSVERSIONINFOW);
        RTL_OSVERSIONINFOW vi = {};
        vi.dwOSVersionInfoSize = sizeof(vi);
        const RtlGetVersionFn fn =
            reinterpret_cast<RtlGetVersionFn>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion"));
        if (fn && fn(&vi) == 0)
            Log("windows: %lu.%lu build %lu\n", vi.dwMajorVersion, vi.dwMinorVersion, vi.dwBuildNumber);
    }
    Log("key: HKCU\\%s\nNEW = %s\nOLD = %s\n", Narrow(kKey).c_str(), Narrow(kNew).c_str(), Narrow(kOld).c_str());
    Log("LIMIT: the other writer is a separate RegOpenKeyExW handle in this process, on a helper thread, one step at a time\n");

    // ---- the keys: this machine's main GPU as the product plans it, and another card as the target ----
    const std::wstring logPath = cd::GetLogPath();
    const long long logBefore = FileBytes(logPath);
    std::vector<cd::GpuAdapter> adapters;
    std::wstring adapterError;
    const bool adaptersOk = cd::EnumerateGpuAdapters(adapters, &adapterError);
    const cd::GpuPlan plan = cd::PlanGpuIsolation(adapters);
    Log("adapters: ok=%d count=%zu plan.gameKey=%s plan.backgroundKey=%s\n", adaptersOk ? 1 : 0, adapters.size(),
        Narrow(plan.gameKey).c_str(), Narrow(plan.backgroundKey).c_str());
    const bool gameFromPlan = ValidKey(plan.gameKey);
    g_gameKey = gameFromPlan ? plan.gameKey : kFallbackGameKey;
    for (size_t i = 0; i < adapters.size() && g_targetKey.empty(); ++i)
        if (adapters[i].adapterKey != g_gameKey && ValidKey(adapters[i].adapterKey)) g_targetKey = adapters[i].adapterKey;
    const bool targetFromAdapters = !g_targetKey.empty();
    if (!targetFromAdapters) g_targetKey = kFallbackTargetKey;
    g_gamePin = cd::FormatPreferenceValue(g_gameKey);
    g_targetPin = cd::FormatPreferenceValue(g_targetKey);
    Log("gameKey   = %s (%s)\ntargetKey = %s (%s)\ngame pin   = \"%s\"\ntarget pin = \"%s\"\n", Narrow(g_gameKey).c_str(),
        gameFromPlan ? "this machine's main GPU, PlanGpuIsolation" : "fallback", Narrow(g_targetKey).c_str(),
        targetFromAdapters ? "another adapter on this machine" : "fallback", Narrow(g_gamePin).c_str(),
        Narrow(g_targetPin).c_str());

    // ---- preconditions: without these the scenarios below would test nothing ----
    if (!ValidKey(g_gameKey) || !ValidKey(g_targetKey) || g_gameKey == g_targetKey) {
        Log("REFUSED: the keys are not two different 18-character keys the product's parser reads back\n");
        return 3;
    }
    if (cd::PreferenceChoiceKey(g_gamePin) != g_gameKey) {
        Log("REFUSED: PreferenceChoiceKey(game pin) != gameKey\n");
        return 3;
    }
    if (!cd::SameInstallRoot(kOld, kNew)) {
        Log("REFUSED: SameInstallRoot(OLD, NEW) is false, so the sibling rule would not apply\n");
        return 3;
    }
    Log("preconditions: PreferenceChoiceKey(game pin) == gameKey, PreferenceChoiceKey(target pin) == targetKey, "
        "SameInstallRoot(OLD, NEW) - all hold\n");

    std::wstring dir(MAX_PATH, L'\0');
    dir.resize(GetModuleFileNameW(nullptr, &dir[0], MAX_PATH));
    dir.resize(dir.find_last_of(L'\\') + 1);

    std::string before;
    size_t valuesBefore = 0;
    if (!Fingerprint(before, valuesBefore)) { Log("REFUSED: fingerprint-before failed: %s", before.c_str()); return 3; }
    WriteText(dir + L"guarded_write_probe-fingerprint-before.txt", before);
    Log("fingerprint-before: %s", before.substr(0, before.find('\n') + 1).c_str());

    {
        std::wstring v;
        const bool newExists = ReadPlain(kNew, v), oldExists = ReadPlain(kOld, v);
        if (newExists || oldExists) {
            Log("REFUSED: NEW exists=%d OLD exists=%d - touching nothing. A run that stopped part-way leaves them; remove\n"
                "them with: reg delete \"HKCU\\%s\" /v \"<path>\" /f\n", newExists ? 1 : 0, oldExists ? 1 : 0,
                Narrow(kKey).c_str());
            return 2;
        }
    }

    const char* expectText[] = {
        "",
        "Written, complete=true inside the transaction, NEW = target pin, OLD absent",
        "NoLongerAllowed (the rule refuses before the call too), NEW absent, OLD = game pin",
        "NoLongerAllowed or NotCommitted 6704, NEW absent, OLD = game pin",
        "NotCommitted 6704 (check said yes, complete=true), NEW absent, OLD = game pin",
        "Written with no check (a hand tick overrides), NEW = target pin, OLD = game pin",
    };

    std::vector<Row> rows;
    for (int s = 1; s <= 5; ++s) {
        for (int rep = 1; rep <= 3; ++rep) {
            // known state, plain writes: NEW absent; OLD pinned to the main GPU before the call in P2 and P5, else absent
            const LONG rn = DeletePlain(kNew);
            const LONG ro = (s == 2 || s == 5) ? SetPlain(kOld, g_gamePin.c_str()) : DeletePlain(kOld);
            Log("\n=== P%d rep %d ===\n  setup: delete NEW rc=%ld; %s OLD rc=%ld (2 = already absent)\n  pre: NEW=%s | OLD=%s\n",
                s, rep, rn, (s == 2 || s == 5) ? "pin" : "delete", ro, Show(kNew).c_str(), Show(kOld).c_str());
            std::wstring v;
            const bool preOk = !ReadPlain(kNew, v) && ((s == 2 || s == 5) ? (ReadPlain(kOld, v) && v == g_gamePin)
                                                                          : !ReadPlain(kOld, v));
            // What Auto assign's rule answers with no transaction open: PrepareEdits asks the same rule before the write.
            const CheckResult pre = StillAutoEligibleMirror();
            g_outTx.push_back(pre.micros);
            Log("  rule before the call (no transaction open): %s, complete=%s, %.1f us, walk saw %s\n",
                pre.answer ? "tick" : "no tick", pre.complete ? "true" : "false", pre.micros, pre.seen.c_str());

            Ctx ctx;
            unsigned long err = 12345;
            cd::GuardedWriteResult g = cd::GuardedWriteResult::Failed;
            switch (s) {
                case 1: case 2:   // exactly as RunEdits calls it for an automatic row: no hook, the check
                    g = cd::GuardedWriteGpuPreference(kNew, false, L"", false, g_targetPin, &err, nullptr, nullptr,
                                                      CheckPlain, &ctx);
                    break;
                case 3:
                    g = cd::GuardedWriteGpuPreference(kNew, false, L"", false, g_targetPin, &err, HookPinOld, &ctx,
                                                      CheckPlain, &ctx);
                    break;
                case 4:
                    g = cd::GuardedWriteGpuPreference(kNew, false, L"", false, g_targetPin, &err, nullptr, nullptr,
                                                      CheckThenPinOld, &ctx);
                    break;
                case 5:           // exactly as RunEdits calls it for a hand tick: no check
                    g = cd::GuardedWriteGpuPreference(kNew, false, L"", false, g_targetPin, &err);
                    break;
            }

            std::wstring nv, ov;
            const bool newPresent = ReadPlain(kNew, nv), oldPresent = ReadPlain(kOld, ov);
            const bool newTarget = newPresent && nv == g_targetPin;
            const bool oldGame = oldPresent && ov == g_gamePin;
            const bool other = ctx.otherRc == 0 && !ctx.otherBlocked;
            const bool checked = ctx.calls == 1 && ctx.allComplete;
            using R = cd::GuardedWriteResult;
            bool pass = false;
            switch (s) {
                case 1: pass = g == R::Written && err == 0 && checked && ctx.answer && newTarget && !oldPresent && pre.answer; break;
                case 2: pass = g == R::NoLongerAllowed && err == 0 && checked && !ctx.answer && !newPresent && oldGame && !pre.answer; break;
                case 3: pass = ((g == R::NoLongerAllowed && err == 0 && checked && !ctx.answer) ||
                                (g == R::NotCommitted && err == 6704 && ctx.allComplete)) &&
                               ctx.hook1 == 1 && other && !newPresent && oldGame && pre.answer; break;
                case 4: pass = g == R::NotCommitted && err == 6704 && checked && ctx.answer && other && !newPresent && oldGame &&
                               pre.answer; break;
                case 5: pass = g == R::Written && err == 0 && ctx.calls == 0 && newTarget && oldGame && !pre.answer; break;
            }
            pass = pass && preOk;

            const std::string complete = ctx.calls == 0 ? "-" : (ctx.allComplete ? "true" : "FALSE");
            Log("  result: %s error=%lu | check calls=%d complete=%s answer=%s elapsed=%.1f us entries=%zu walk saw %s\n",
                ResultName(g), err, ctx.calls, complete.c_str(), ctx.calls ? (ctx.answer ? "yes" : "no") : "-",
                ctx.micros, ctx.entries, ctx.calls ? ctx.seen.c_str() : "-");
            if (s == 3 || s == 4)
                Log("  other writer (pin OLD): rc=%ld blocked=%s hook1=%d hook2=%d\n", ctx.otherRc,
                    ctx.otherBlocked ? "YES" : "no", ctx.hook1, ctx.hook2);
            Log("  final (plain read): NEW=%s | OLD=%s\n  expect: %s -> %s\n", Show(kNew).c_str(), Show(kOld).c_str(),
                expectText[s], pass ? "AS EXPECTED" : "UNEXPECTED");
            const Row r = { s, rep, ResultName(g), err, ctx.calls, complete, ctx.micros, pre.answer,
                            ShowShort(kNew), ShowShort(kOld), pass };
            rows.push_back(r);
        }
    }

    const LONG dn = DeletePlain(kNew), dold = DeletePlain(kOld);
    Log("\ncleanup: delete NEW rc=%ld, delete OLD rc=%ld (2 = already absent)\nafter cleanup: NEW=%s | OLD=%s\n", dn, dold,
        Show(kNew).c_str(), Show(kOld).c_str());

    Log("\n=== SUMMARY ===\n");
    Log("%-4s %-4s %-16s %-6s %-6s %-9s %-11s %-11s %-28s %-28s %s\n", "scn", "rep", "result", "error", "checks", "complete",
        "check_us", "rule_before", "final NEW", "final OLD", "verdict");
    int unexpected = 0;
    for (const Row& r : rows) {
        char us[32];
        if (r.calls) snprintf(us, sizeof(us), "%.1f", r.micros); else snprintf(us, sizeof(us), "-");
        Log("P%-3d %-4d %-16s %-6lu %-6d %-9s %-11s %-11s %-28s %-28s %s\n", r.scenario, r.rep, r.result.c_str(), r.err,
            r.calls, r.complete.c_str(), us, r.ruleBefore ? "tick" : "no tick", r.finalNew.c_str(), r.finalOld.c_str(),
            r.pass ? "AS_EXPECTED" : "UNEXPECTED");
        if (!r.pass) ++unexpected;
    }
    Log("\n");
    Stats("TIMING check inside the guarded write (transaction open)", g_inTx);
    Stats("TIMING same check with no transaction open", g_outTx);
    Log("KEY_VALUES=%zu (values in the key before the run)\n", valuesBefore);

    std::string after;
    size_t valuesAfter = 0;
    const bool okAfter = Fingerprint(after, valuesAfter);
    WriteText(dir + L"guarded_write_probe-fingerprint-after.txt", after);
    Log("fingerprint-after: %s", after.substr(0, after.find('\n') + 1).c_str());
    const long long logAfter = FileBytes(logPath);
    Log("product log %s: %lld bytes before, %lld after (EnumerateGpuAdapters may log an Unknown GPU kind there)\n",
        Narrow(logPath).c_str(), logBefore, logAfter);
    const bool match = okAfter && after == before;
    Log("FINGERPRINT_MATCH=%s\nUNEXPECTED_ROWS=%d of %zu\n", match ? "True" : "False", unexpected, rows.size());
    return (match && unexpected == 0) ? 0 : 1;
}
