// cuda_assign_probe - an OPT-IN pre-release check, run by tools\probe-cuda-assign.bat.
//
// 🔴 IT WRITES ONE DUMMY APPLICATION PROFILE TO THE REAL NVIDIA DRIVER DATABASE AND REMOVES IT AGAIN.
// NOT PART OF GATE A, and it must never be added to it: Gate A has to stay safe to run anywhere, any
// number of times, on a machine with no NVIDIA driver at all. This is run by hand, once, before a
// release that touches src\gpu_cuda.cpp. The operator authorised exactly this class of reversible write
// (founder decision 16: "Yes, test it").
//
// WHAT IT PROVES, AND WHY NOTHING ELSE DOES. The unit suite reaches every pure decision in gpu_cuda.h,
// and the panel regressions drive Apply and Remove end to end - but both run against a FAKE CudaOps that
// never opens nvapi64.dll. Not one line of the NVAPI half of src\gpu_cuda.cpp is executed by either:
// the interface ids, the struct layouts, the id-string enumeration, the profile lookup, the wide-string
// setting write, the delete-the-setting branch and NvAPI_DRS_SaveSettings are all unmeasured until
// something calls them against the real driver. This calls them - through the product's OWN functions
// (cd::MakeCudaOps, cd::CudaTargetFor, cd::WriteCudaForRow, cd::RestoreCudaForRow), never a copy.
//
// SECTIONS 4 AND 4b. Section 4 is the straight line: make the entry, read it, ask again, take it away -
// and the RECORD the product keeps while it does so, which since founder decision 22 is one line per
// application carrying the entry, the value last written and when (E2). Section 4b is the cases none of
// which is reachable from a straight line - each needs the driver in a state the run before it arranged.
//
// THE SCENARIO NUMBERS ARE NAMES, NOT AN ORDER. Each sits where the driver state it needs is, so the run
// order is S1..S7, S16, then S17, S8, S14, S9, S12, S13, S18, S19, S20, S21, S22, S23, S24. What each is:
//   S5  E1, READ-ONLY: the very entry S2 made is refused when no record line of ours names it, and the
//       refusal names the entry and counts its other programs. Nothing is written.
//   S6  E3: Remove takes the ENTRY away, setting and all - the state before this product was "NVIDIA has
//       no settings entry for this application", and nothing less than removing the entry reaches it.
//   S7  the record on disk: one line after two Applies (E2), a per-run gpu-cuda-before-*.txt from the
//       earlier rounds making the whole record UNREADABLE rather than empty (E6), and a line being spent.
//   S16 a gpu-cuda-record.txt this version does not understand - round 3's seven-field format, a file of
//       nothing but comments, a header version never written here, a file of nought bytes and a file that
//       is nothing but a byte-order mark. All five read as Unreadable and never as "there was nothing"
//       (E6), and the product refuses to write over any of them.
//   S17 E1's REAL SUBJECT: an entry that is NOT the product's and covers TWO programs. The probe makes
//       one through a session of its own, under a name nothing like the product's, and Apply must refuse
//       it, NAME it, COUNT the other program and change nothing at all. The probe takes it away again.
//   S8  an undo whose value no longer matches what we LAST wrote, so the product must REFUSE and leave the
//       other party's value alone (E3). The change arrives from a session of the probe's own, because the
//       product's compare reads through the product's session, not behind its back.
//   S14 an undo whose ENTRY IDENTITY changed since the line was written - the record names an entry the
//       driver no longer answers with, and then an application entry that entry no longer covers. Both are
//       refused with nothing taken away (E3). It runs BEFORE S9, because S9 removes the entry.
//   S9  an undo that succeeds, and then the record line it spends - a second Remove finds nothing to do.
//   S12 the entry is already gone: the state the line asks for is already true, which is ABSENT and not a
//       failure (E3).
//   S13 OUR OWN ENTRY, MET AGAIN - and the round-2 blocker on the real driver. Apply to GPU A, Apply again
//       to GPU B, then Remove. The second Apply must UPDATE the entry it made and not make a second one,
//       which only enumerating the database can show, and the undo must compare the driver against the
//       SECOND value and take the entry away - no conflict.
//   S18 R5-1, ROUND 4's BLOCKER: NVIDIA keeps EVERY per-application setting in ONE profile, so the probe
//       plays the user and puts a SECOND setting on the entry the product made. Remove must then take away
//       OUR setting and nothing else - the entry, its application and that other setting all still standing.
//   S19 R5-2, ROUND 4's OTHER HIGH: an entry carrying the product's OWN NAME that covers somebody else's
//       application and not the dummy's, with a record line of ours claiming it. The old adopt path went
//       straight to CreateApplication with no GetProfileInfo at all; it must now be refused, NAMED and
//       COUNTED, with nothing written into it.
//   S20 R5-3, the finding the operator would have met first: a row whose Windows GPU pin ALREADY equals
//       what Apply intends - every application v0.5.6 and v0.5.7 pinned - must still get its CUDA entry.
//       It drives cd::RunGpuEdits, where that gate lives; every Windows call there is a function object
//       this file supplies in memory, so no registry value is read or written.
//   S21 R6-1, ROUND 5's DEFECT, AND IT IS ROUND 4's SHAPE ONE LEVEL DOWN. Round 4's blocker was fixed with
//       NVDRS_PROFILE::numOfSettings > 1 - a COUNT standing in for an IDENTITY, which is exactly what
//       round 4 had already proved wrong for APPLICATIONS. The probe arranges an entry of ours holding ONE
//       setting that is the USER'S and none of its own, and a Remove aimed at it must refuse and delete
//       nothing. 🔴 IT IS ALSO THE MEASUREMENT ROUND 6 COULD NOT MAKE: it prints whether
//       NvAPI_DRS_EnumSettings resolved at all (its interface id is [A]), the setting ids the driver
//       lists, and whether NvAPI_DRS_GetSetting still answers with a value the entry does not hold of its
//       own. It also runs R6-7's "nothing of ours" record line against the real driver.
//   S22 R6-3, the commonest way to a leftover: the record file is GONE and an entry this product made is
//       still in the driver. Remove used to never open the session at all in that state. What the probe
//       settles - and nothing else can - is that the real driver still resolves that executable to an
//       entry the product recognises as its own, so there is something for the new sentence to name. The
//       window's own rules (open anyway, never block, never refuse) are driven by the panel regressions.
//   S23 A MEASUREMENT, NOT A RULE, and it passes whatever the driver answers. This product compares
//       profile names WITHOUT case everywhere (R5-10) on an assumption nobody measured. The probe makes
//       its own dummy profile and looks it up exactly, in UPPER CASE and in lower case, and prints what
//       NvAPI_DRS_FindProfileByName says. Every call is a lookup; nothing is written.
//   S24 R6-12, AND IT IS S23's MEASUREMENT TURNED INTO A RULE. The lookup has case; every ownership
//       comparison in the product is without case (R5-10) and stays that way - so a path Windows spells
//       differently than when the entry was made still reads as ours while a RECOMPUTED profile name
//       misses the entry, and a SECOND entry differing only in case used to be made. The probe makes the
//       entry through the product, takes the application out through a session of its own (which is what
//       makes the adopt path reachable at all), and drives a second Apply with the path in UPPER CASE. It
//       must adopt the entry THE RECORD NAMES, and the total number of profiles must not move.
// Every one of them opens the product's session through cd::MakeCudaOps() afresh, because that is how Apply
// and Remove really meet the driver: one session per action, with anything at all able to happen between.
//
// WHAT THIS PROBE CANNOT REACH, AND WHERE THOSE RULES ARE MEASURED INSTEAD. -167, and E7's clean-up of the
// profile made moments before it, need another profile to already claim the dummy's BASE NAME - the one
// thing this probe refuses to arrange, and S17's foreign entry is not it: that claims a full PATH, which
// is the form -167 does NOT answer to. ProfileNameTaken needs an entry carrying the product's own name with
// no application in it, which needs a driver call this product does not make. Both live with the fake
// CudaOps in tests\test_main.cpp and tests\gpu_panel_regression.cpp.
//
// SAFETY, and every one of these is a refusal rather than a best effort:
//   * It backs up nvdrsdb0.bin, nvdrsdb1.bin and nvdrssel.bin out of the driver's own folder into its
//     work folder and hashes them BEFORE anything is written. That is the recovery path if it dies
//     half-way, and the paths are printed.
//   * It dumps every profile that carries CUDA_EXCLUDED_GPUS_ID (0x10354FF8) before and after, in full -
//     name, isPredefined, counts, the value, and every application entry - and FAILS if that set changed.
//   * It refuses to run at all if its dummy profile, or its dummy executable, is already known to the
//     driver. A run that stopped part-way is reported, not written over.
//   * It makes entries under exactly TWO NAMES in the driver database and takes both away again: the dummy
//     profile, which is the only one the PRODUCT is ever pointed at, and S17's foreign entry, which exists
//     to be refused and which only this file's own code ever touches. Both names are judged by
//     ProbeMadeProfile before any mutating call, both are named in the recovery notice, and `cleanup`
//     removes both. S19 makes the DUMMY name itself a second time, around somebody else's application - it
//     is still that one name, still guarded by ProbeMadeProfile, and still removed by `cleanup`.
//     🔴 A THIRD NAME IS KNOWN TO THIS FILE AND IS NEVER MADE BY IT: kDummyProfileUpper, the dummy path in
//     UPPER CASE. S24 exists to prove the PRODUCT does not make it. It is allowed by ProbeMadeProfile,
//     refused by section 3 if it is already there, named in the recovery notice and removed by `cleanup`
//     for one reason only - so the run in which the rule BREAKS still leaves the driver as it found it.
//   * S24 removes ONE application entry of its own, the dummy's, from the dummy profile - through
//     StripDummyApplication, behind the same OwnProfile + FindOwnProfile guards SetOwnSetting uses. The
//     profile it came out of is the probe's own and is deleted whole before the probe ends.
//   * S18 writes ONE setting of its own, OGL_IMPLICIT_GPU_AFFINITY_ID, onto the dummy entry, to play the
//     user setting something else for the same application in NVIDIA Control Panel. It goes through
//     OwnProfile and FindOwnProfile exactly as SetOwnSetting does, it lands on an entry that covers one
//     path which does not exist, and the whole entry is deleted before the probe ends.
//   * Its one and only subject is C:\GameOptimizerProbe\cuda\v1\gocudaassign.exe - a name no NVIDIA
//     profile claims, which matters because [M] NvAPI_DRS_CreateApplication answers -167
//     NVAPI_EXECUTABLE_ALREADY_IN_USE when another profile already owns an executable's BASE NAME, and a
//     full path is no escape.
//   * EVERY mutating call passes through OwnProfile(), which compares the profile name to the dummy
//     character for character. The delete path re-reads NvAPI_DRS_GetProfileInfo immediately before the
//     delete and refuses unless the name still matches AND isPredefined == 0. No other profile - and in
//     particular none of the 8 on this machine that carry 0x10354FF8 - is reachable from any code path
//     in this file.
//   * The probe makes ONE mutating call of its own, SetOwnSetting, which plays the other program in S8.
//     It goes through OwnProfile() and then through FindOwnProfile(), so it reaches the dummy profile and
//     nothing else, and it writes a value NVIDIA itself uses rather than one of this product's.
//
// 🔴 NvAPI_Unload IS NEVER CALLED HERE EITHER. [M] 2026-09-19: after it, ANY further NvAPI call crashes
// the process, NvAPI_GetErrorMessage included - so even formatting the status of the call that noticed is
// fatal. The library is left loaded for the life of the process; only DRS SESSIONS are opened and closed.
//
// THE STRUCT LAYOUTS AND INTERFACE IDS ARE NVIDIA'S, from the MIT-licensed headers at
// github.com/NVIDIA/nvapi (see NOTICE.md). They are re-declared here, as src\gpu_cuda.cpp re-declares
// them, and every size is pinned by a static_assert - a layout typed wrong is a build failure and not a
// corrupt driver database.
//
// WHAT THE BYTE FINGERPRINT OF THE DATABASE IS AND IS NOT. NvAPI_DRS_SaveSettings rewrites the whole
// ~1.8 MB file, so the live .bin files are NOT expected to come back byte-identical and FINGERPRINT_MATCH
// is printed as information only - it is NOT a pass criterion and the exit code ignores it. DUMP_MATCH is
// the criterion: what the driver LOGICALLY holds for 0x10354FF8, which is the thing that must not change.
// CURRENT_DB_CLEAN is the second criterion, and section 7 explains why the dummy name is EXPECTED to
// survive in the other half of the database pair without that being a leak.
//
// USAGE:  cuda_assign_probe.exe            run the probe
//         cuda_assign_probe.exe cleanup    run ONLY the guarded cleanup, for a run that died half-way
//
// EXIT: 0 every scenario as expected, the CUDA-profile dump unchanged, and the current database file free
//       of the dummy name; 1 a scenario was unexpected, or the dump changed, or the current database file
//       still names the dummy; 2 refused because the dummy profile or executable already exists;
//       3 refused because a precondition failed (no NVAPI, no session, no backup, nothing to exclude);
//       4 🔴 CLEANUP FAILED - the dummy profile may still be in the driver database.
#include <windows.h>

#include <algorithm>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwctype>   // S23 only: towupper/towlower, to ask the driver for one name in three cases
#include <string>
#include <vector>

#include "gpu_cuda.h"
// S20 only: cd::RunGpuEdits is where R5-3 lives, and every Windows call it makes is a function object this
// file supplies in memory - so the row loop is driven end to end without reading or writing one registry
// value of the operator's.
#include "gpu_edit.h"
#include "gpu_pref.h"
#include "util.h"

namespace {

// ---- the one subject, and nothing else ---------------------------------------------------------
const wchar_t* kDummyExe = L"C:\\GameOptimizerProbe\\cuda\\v1\\gocudaassign.exe";
// EXACTLY what cd::MakeCudaOps().createProfileForExe builds: L"Game Optimizer - " + exePath. If the
// product ever changes that name this probe stops matching and refuses, which is the wanted failure.
const wchar_t* kDummyProfile = L"Game Optimizer - C:\\GameOptimizerProbe\\cuda\\v1\\gocudaassign.exe";
const wchar_t* kDummyAppKey = L"c:/gameoptimizerprobe/cuda/v1/gocudaassign.exe";
const wchar_t* kDummyBaseName = L"gocudaassign.exe";
// ---- S24's second spelling of the SAME file (R6-12) ---------------------------------------------
//
// 🔴 THE SAME EXECUTABLE, AS WINDOWS IS ENTITLED TO HAND IT BACK. Windows paths are case-insensitive, so
// an application can reach this product spelled differently than when its entry was made - and [M] S23
// measured NvAPI_DRS_FindProfileByName to be CASE-SENSITIVE. cd::CudaAppKeyFor lower-cases, so the
// APPLICATION entry is kDummyAppKey either way and section 3's pre-flight already covers it; the PROFILE
// name is not lower-cased, so this is a second name the driver could be made to hold.
const wchar_t* kDummyExeUpper = L"C:\\GAMEOPTIMIZERPROBE\\CUDA\\V1\\GOCUDAASSIGN.EXE";
// 🔴 EXACTLY cd::CudaProfileNameFor(kDummyExeUpper), and it is named here for ONE reason: the entry this
// scenario must prove is never made is one nothing else in this file could clean up. ProbeMadeProfile
// allows it, CleanUp removes it, section 3 refuses to start if the driver already holds it, and the
// recovery notice names it - so the run in which the rule is BROKEN still leaves the driver as it found it.
const wchar_t* kDummyProfileUpper = L"Game Optimizer - C:\\GAMEOPTIMIZERPROBE\\CUDA\\V1\\GOCUDAASSIGN.EXE";
// 🔴 THE ONE PROFILE NAME THIS PROBE EVER HANDS THE PRODUCT THAT IS NOT THE DUMMY, AND WHY IT CANNOT
// MUTATE ANYTHING (S14). E3 says Remove must refuse when NVIDIA now keeps the application's settings under
// a different entry than the record names - a profile renamed in NVIDIA's own Control Panel arrives at
// Remove in exactly that shape - so measuring it needs a record line naming a profile the driver does NOT
// resolve the dummy application to. This name is deliberately one the driver has never heard of:
//   * S14 asks the driver, read-only, immediately before and immediately after, and fails the row if it is
//     known either time;
//   * cd::RestoreCudaForRow refuses at its identity check, which is before it reads or writes anything at
//     all by that name;
//   * and cd::CudaOps::writeSetting looks a profile up by name and answers false when there is none - it
//     never creates one - so there is no route by which this string becomes a profile.
// It is not put through OwnProfile(): OwnProfile exists to stop a MUTATING call reaching a profile the probe
// does not own, and this is not a profile.
const wchar_t* kPhantomProfile =
    L"Game Optimizer - C:\\GameOptimizerProbe\\cuda\\v1\\renamed-gocudaassign.exe";
// And an application entry the dummy profile does not cover, for the other half of an identity change.
const wchar_t* kPhantomAppKey = L"c:/gameoptimizerprobe/cuda/v1/never-added-gocudaassign.exe";
// ---- S17's entry: the one thing in the driver this probe makes that is NOT the product's ---------
//
// 🔴 E1 IS A RULE ABOUT SOMEBODY ELSE'S ENTRY, AND EVERY OTHER SCENARIO HERE MEETS ONE OF OUR OWN. A
// profile the product made, handed to a plan with no record line, is refused for want of a line - which is
// real, and is S5 - but it is refused on the LAST of E1's tests, so it never reaches the ones that matter
// most: a name that is not ours, and a second member. This is the entry that does: its name is nothing
// like CudaProfileNameFor's, it covers TWO applications, and it carries a CUDA value of NVIDIA's own.
//
// Nothing of the PRODUCT ever touches it. It is made, read and taken away by this file's own NVAPI calls,
// each of them judged by ProbeMadeProfile first, and the product only ever meets it through the driver.
const wchar_t* kForeignProfile = L"GameOptimizerProbe foreign v1 - not Game Optimizer's";
// The second member. A base name of its own, because [M] NvAPI_DRS_CreateApplication answers -167 when
// another profile already claims one - so the run refuses to start if the driver has heard of either.
const wchar_t* kForeignNeighbourExe = L"C:\\GameOptimizerProbe\\cuda\\v1\\neighbourprobe.exe";
const wchar_t* kForeignNeighbourKey = L"c:/gameoptimizerprobe/cuda/v1/neighbourprobe.exe";
const wchar_t* kForeignNeighbourBase = L"neighbourprobe.exe";
// NVIDIA's own word for "nothing is excluded": a value the driver itself offers, and one this product
// would never write for this machine - so "the refusal changed nothing" is a checkable claim rather than
// two strings that happen to match.
const wchar_t* kForeignValue = L"none";
const wchar_t* kDrsDir = L"C:\\ProgramData\\NVIDIA Corporation\\Drs";
const wchar_t* kDbFiles[3] = { L"nvdrsdb0.bin", L"nvdrsdb1.bin", L"nvdrssel.bin" };
// A plausible AMD integrated GPU, used only if this machine has no non-NVIDIA adapter of its own.
const wchar_t* kFallbackNonNvidiaKey = L"1002&164E&00000000";

void Log(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    fflush(stdout);
}

std::string U8(const std::wstring& w) {
    if (w.empty()) return std::string();
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    if (n <= 0) return std::string();
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), &s[0], n, nullptr, nullptr);
    return s;
}

unsigned long long Fnv(const void* p, size_t n, unsigned long long h = 1469598103934665603ULL) {
    const unsigned char* b = static_cast<const unsigned char*>(p);
    for (size_t i = 0; i < n; ++i) {
        h ^= b[i];
        h *= 1099511628211ULL;
    }
    return h;
}

// How many times a wide string appears in a block of bytes. wchar_t is UTF-16LE here, which is how the
// driver stores its names, so the literal's own bytes are the needle.
size_t CountWide(const std::vector<unsigned char>& data, const wchar_t* needle) {
    const size_t n = wcslen(needle) * sizeof(wchar_t);
    if (n == 0 || data.size() < n) return 0;
    const unsigned char* p = reinterpret_cast<const unsigned char*>(needle);
    size_t hits = 0;
    for (size_t i = 0; i + n <= data.size(); ++i)
        if (std::memcmp(&data[i], p, n) == 0) ++hits;
    return hits;
}

bool FileExists(const std::wstring& path) {
    const DWORD a = GetFileAttributesW(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

bool ReadWholeFile(const std::wstring& path, std::vector<unsigned char>& out) {
    out.clear();
    const HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                 nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size;
    if (!GetFileSizeEx(h, &size) || size.QuadPart < 0 || size.QuadPart > (64LL << 20)) {
        CloseHandle(h);
        return false;
    }
    out.resize(static_cast<size_t>(size.QuadPart));
    DWORD got = 0;
    const bool ok = out.empty() || (ReadFile(h, &out[0], static_cast<DWORD>(out.size()), &got, nullptr) &&
                                    got == out.size());
    CloseHandle(h);
    if (!ok) out.clear();
    return ok;
}

void WriteTextFile(const std::wstring& path, const std::string& text) {
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"wb") != 0 || f == nullptr) return;
    if (!text.empty()) fwrite(text.data(), 1, text.size(), f);
    fclose(f);
}

// UTF-16LE with a byte-order mark, which is the ONLY shape cd::ReadRestoreFileText accepts - it refuses an
// odd-sized file and a file whose first unit is not 0xFEFF. S16 needs the record to be READ successfully and
// then fail to PARSE, because the thing it measures is the parser's refusal of an older format, not the
// reader's refusal of a file it cannot open.
void WriteWideTextFile(const std::wstring& path, const std::wstring& text) {
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"wb") != 0 || f == nullptr) return;
    const wchar_t bom = 0xFEFF;
    fwrite(&bom, sizeof bom, 1, f);
    if (!text.empty()) fwrite(text.data(), sizeof(wchar_t), text.size(), f);
    fclose(f);
}

unsigned long long FileFingerprint(const std::wstring& path) {
    std::vector<unsigned char> bytes;
    if (!ReadWholeFile(path, bytes)) return 0;
    return Fnv(bytes.empty() ? "" : reinterpret_cast<const char*>(&bytes[0]), bytes.size());
}

// 🔴 A RUN STARTS FROM A CLEAN RECORD FOLDER, AND "CLEAN" HAS TO INCLUDE THE LEGACY FILES. The first version
// of this deleted only gpu-cuda-record.txt, so the gpu-cuda-before-*.txt that S7 writes on purpose - and any
// left behind by a run that died before deleting it - survived into the NEXT run and made that folder's
// record Unreadable before a single scenario had touched it. [M] 2026-09-20: that is exactly what happened,
// on the first run of this file, and it failed S7 and S9 for a reason that had nothing to do with the
// product. The product was right both times - an older-format file really does make the record unreadable,
// which is the whole of D7 - and the probe was measuring its own leftovers.
void ClearRecordFolder(const std::wstring& dir) {
    DeleteFileW((dir + L"\\" + cd::CudaRecordFileName()).c_str());
    WIN32_FIND_DATAW fd;
    const HANDLE f = FindFirstFileW((dir + L"\\gpu-cuda-before-*.txt").c_str(), &fd);
    if (f == INVALID_HANDLE_VALUE) return;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
            DeleteFileW((dir + L"\\" + fd.cFileName).c_str());
    } while (FindNextFileW(f, &fd));
    FindClose(f);
}

// ---- NVAPI, for the two things the product deliberately cannot do -------------------------------
//
// The product never enumerates other people's profiles and never deletes a profile, so the dump and the
// cleanup need their own access. Everything the PRODUCT does is still done by the product.
typedef uint32_t NvU32;
typedef uint16_t NvU16;
typedef uint8_t NvU8;
typedef uint64_t NvU64;
typedef int NvAPI_Status;
typedef char NvAPI_ShortString[64];
typedef NvU16 NvAPI_UnicodeString[2048];
typedef void* NvDRSSessionHandle;
typedef void* NvDRSProfileHandle;

const NvAPI_Status NVAPI_OK = 0;

#define MAKE_NVAPI_VERSION(t, v) (NvU32)(sizeof(t) | ((v) << 16))

typedef struct { NvU32 valueLength; NvU8 valueData[4096]; } NVDRS_BINARY_SETTING;
#pragma pack(push, 4)
typedef struct {
    NvU32 version; NvAPI_UnicodeString settingName; NvU32 settingId; int settingType; int settingLocation;
    NvU32 isCurrentPredefined; NvU32 isPredefinedValid;
    union { NvU32 u32PredefinedValue; NVDRS_BINARY_SETTING binaryPredefinedValue;
            NvAPI_UnicodeString wszPredefinedValue; NvU64 u64PredefinedValue; };
    union { NvU32 u32CurrentValue; NVDRS_BINARY_SETTING binaryCurrentValue;
            NvAPI_UnicodeString wszCurrentValue; NvU64 u64CurrentValue; };
} NVDRS_SETTING_V1;
typedef struct {
    NvU32 version; NvAPI_UnicodeString profileName; NvU32 gpuSupport; NvU32 isPredefined;
    NvU32 numOfApps; NvU32 numOfSettings;
} NVDRS_PROFILE_V1;
typedef struct {
    NvU32 version; NvU32 isPredefined; NvAPI_UnicodeString appName; NvAPI_UnicodeString userFriendlyName;
    NvAPI_UnicodeString launcher; NvAPI_UnicodeString fileInFolder;
    NvU32 isMetro : 1; NvU32 isCommandLine : 1; NvU32 reserved : 30; NvAPI_UnicodeString commandLine;
} NVDRS_APPLICATION_V4;
#pragma pack(pop)
static_assert(sizeof(NVDRS_APPLICATION_V4) == 20492, "NVDRS_APPLICATION_V4 size");
static_assert(sizeof(NVDRS_SETTING_V1) == 12320, "NVDRS_SETTING_V1 size");
static_assert(sizeof(NVDRS_PROFILE_V1) == 4116, "NVDRS_PROFILE_V1 size");

// Every id below resolved on this machine [M] 2026-09-19 (ledger, "Measured ground truth").
enum : NvU32 {
    ID_Initialize = 0x0150e828,
    ID_GetErrorMessage = 0x6c2d048c,
    ID_DRS_CreateSession = 0x0694d52e,
    ID_DRS_DestroySession = 0xdad9cff8,
    ID_DRS_LoadSettings = 0x375dbd6b,
    ID_DRS_SaveSettings = 0xfcbc7e14,
    ID_DRS_GetProfileInfo = 0x61cd6fd6,
    ID_DRS_EnumProfiles = 0xbc371ee0,
    ID_DRS_GetNumProfiles = 0x1dae4fbc,
    ID_DRS_GetSetting = 0x73bf8338,
    ID_DRS_SetSetting = 0x577dd202,
    ID_DRS_EnumApplications = 0x7fa2173a,
    ID_DRS_FindProfileByName = 0x7e4a9a0b,
    ID_DRS_FindApplicationByName = 0xeee566b2,
    ID_DRS_DeleteProfile = 0x17093206,
    ID_DRS_DeleteApplicationEx = 0xc5ea85a1,
    // Only S17 uses these two, and only on its own foreign entry. The product makes its own profiles
    // through src\gpu_cuda.cpp; a profile that is NOT the product's is the one thing it cannot make.
    ID_DRS_CreateProfile = 0xcc176068,
    ID_DRS_CreateApplication = 0x4347a9de,
};

const NvU32 kCudaExcludedGpus = 0x10354FF8;
// 🔴 S18's STAND-IN FOR THE USER'S OWN SETTING (R5-1). NVIDIA keeps EVERY per-application setting in ONE
// profile, so an entry the product made is also where NVIDIA Control Panel puts that application's other
// settings - and Remove used to delete the whole profile and take them with it. This is a second, real
// setting on the dummy entry: OGL_IMPLICIT_GPU_AFFINITY_ID, the wide string whose value list the product
// ENUMERATES for id strings and never writes, set to the driver's own default word. The entry covers one
// path that does not exist and is deleted before this probe ends, so it governs nothing at any point.
const NvU32 kOglImplicitAffinity = 0x20D0F3E6;
const wchar_t* kOtherSettingValue = L"autoselect";

typedef void*(__cdecl* QueryInterface_t)(NvU32);
typedef NvAPI_Status(__cdecl* F_Void)();
typedef NvAPI_Status(__cdecl* F_ErrMsg)(NvAPI_Status, NvAPI_ShortString);
typedef NvAPI_Status(__cdecl* F_CreateSess)(NvDRSSessionHandle*);
typedef NvAPI_Status(__cdecl* F_Sess)(NvDRSSessionHandle);
typedef NvAPI_Status(__cdecl* F_ProfInfo)(NvDRSSessionHandle, NvDRSProfileHandle, NVDRS_PROFILE_V1*);
typedef NvAPI_Status(__cdecl* F_EnumProf)(NvDRSSessionHandle, NvU32, NvDRSProfileHandle*);
typedef NvAPI_Status(__cdecl* F_NumProf)(NvDRSSessionHandle, NvU32*);
typedef NvAPI_Status(__cdecl* F_GetSetting)(NvDRSSessionHandle, NvDRSProfileHandle, NvU32, NVDRS_SETTING_V1*);
typedef NvAPI_Status(__cdecl* F_SetSetting)(NvDRSSessionHandle, NvDRSProfileHandle, NVDRS_SETTING_V1*);
typedef NvAPI_Status(__cdecl* F_EnumApps)(NvDRSSessionHandle, NvDRSProfileHandle, NvU32, NvU32*, NVDRS_APPLICATION_V4*);
typedef NvAPI_Status(__cdecl* F_FindProf)(NvDRSSessionHandle, NvU16*, NvDRSProfileHandle*);
typedef NvAPI_Status(__cdecl* F_FindApp)(NvDRSSessionHandle, NvU16*, NvDRSProfileHandle*, NVDRS_APPLICATION_V4*);
typedef NvAPI_Status(__cdecl* F_DelProf)(NvDRSSessionHandle, NvDRSProfileHandle);
typedef NvAPI_Status(__cdecl* F_DelAppEx)(NvDRSSessionHandle, NvDRSProfileHandle, NVDRS_APPLICATION_V4*);
typedef NvAPI_Status(__cdecl* F_CreateProf)(NvDRSSessionHandle, NVDRS_PROFILE_V1*, NvDRSProfileHandle*);
typedef NvAPI_Status(__cdecl* F_CreateApp)(NvDRSSessionHandle, NvDRSProfileHandle, NVDRS_APPLICATION_V4*);

struct Nv {
    bool ready = false;
    F_ErrMsg errMsg = nullptr;
    F_CreateSess createSession = nullptr;
    F_Sess destroySession = nullptr;
    F_Sess loadSettings = nullptr;
    F_Sess saveSettings = nullptr;
    F_ProfInfo profileInfo = nullptr;
    F_EnumProf enumProfiles = nullptr;
    F_NumProf numProfiles = nullptr;
    F_GetSetting getSetting = nullptr;
    F_SetSetting setSetting = nullptr;
    F_EnumApps enumApps = nullptr;
    F_FindProf findProfile = nullptr;
    F_FindApp findApp = nullptr;
    F_DelProf deleteProfile = nullptr;
    F_DelAppEx deleteApp = nullptr;
    F_CreateProf createProfile = nullptr;
    F_CreateApp createApp = nullptr;
};
Nv g_nv;
NvDRSSessionHandle g_sess = nullptr;

std::string Msg(NvAPI_Status s) {
    NvAPI_ShortString b = { 0 };
    if (g_nv.errMsg) g_nv.errMsg(s, b);
    char out[160];
    snprintf(out, sizeof out, "%d (%s)", s, b);
    return out;
}

// SYSTEM32 ONLY, for the same reason src\gpu_cuda.cpp does it: a name-only LoadLibrary searches the
// application's own folder first for a DLL that is not a KnownDLL.
bool LoadNvapi() {
    const HMODULE m = LoadLibraryExW(L"nvapi64.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (m == nullptr) {
        Log("nvapi64.dll is not present in System32 (gle=%lu)\n", GetLastError());
        return false;
    }
    const QueryInterface_t qi =
        reinterpret_cast<QueryInterface_t>(reinterpret_cast<void*>(GetProcAddress(m, "nvapi_QueryInterface")));
    if (qi == nullptr) {
        Log("nvapi64.dll has no nvapi_QueryInterface\n");
        return false;
    }
    const F_Void init = reinterpret_cast<F_Void>(qi(ID_Initialize));
    g_nv.errMsg = reinterpret_cast<F_ErrMsg>(qi(ID_GetErrorMessage));
    g_nv.createSession = reinterpret_cast<F_CreateSess>(qi(ID_DRS_CreateSession));
    g_nv.destroySession = reinterpret_cast<F_Sess>(qi(ID_DRS_DestroySession));
    g_nv.loadSettings = reinterpret_cast<F_Sess>(qi(ID_DRS_LoadSettings));
    g_nv.saveSettings = reinterpret_cast<F_Sess>(qi(ID_DRS_SaveSettings));
    g_nv.profileInfo = reinterpret_cast<F_ProfInfo>(qi(ID_DRS_GetProfileInfo));
    g_nv.enumProfiles = reinterpret_cast<F_EnumProf>(qi(ID_DRS_EnumProfiles));
    g_nv.numProfiles = reinterpret_cast<F_NumProf>(qi(ID_DRS_GetNumProfiles));
    g_nv.getSetting = reinterpret_cast<F_GetSetting>(qi(ID_DRS_GetSetting));
    g_nv.setSetting = reinterpret_cast<F_SetSetting>(qi(ID_DRS_SetSetting));
    g_nv.enumApps = reinterpret_cast<F_EnumApps>(qi(ID_DRS_EnumApplications));
    g_nv.findProfile = reinterpret_cast<F_FindProf>(qi(ID_DRS_FindProfileByName));
    g_nv.findApp = reinterpret_cast<F_FindApp>(qi(ID_DRS_FindApplicationByName));
    g_nv.deleteProfile = reinterpret_cast<F_DelProf>(qi(ID_DRS_DeleteProfile));
    g_nv.deleteApp = reinterpret_cast<F_DelAppEx>(qi(ID_DRS_DeleteApplicationEx));
    g_nv.createProfile = reinterpret_cast<F_CreateProf>(qi(ID_DRS_CreateProfile));
    g_nv.createApp = reinterpret_cast<F_CreateApp>(qi(ID_DRS_CreateApplication));
    const bool all = init && g_nv.errMsg && g_nv.createSession && g_nv.destroySession && g_nv.loadSettings &&
                     g_nv.saveSettings && g_nv.profileInfo && g_nv.enumProfiles && g_nv.numProfiles &&
                     g_nv.getSetting && g_nv.setSetting && g_nv.enumApps && g_nv.findProfile &&
                     g_nv.findApp && g_nv.deleteProfile && g_nv.deleteApp && g_nv.createProfile &&
                     g_nv.createApp;
    if (!all) {
        Log("this NVIDIA driver does not offer every settings function the probe needs\n");
        return false;
    }
    const NvAPI_Status s = init();
    Log("NvAPI_Initialize -> %s\n", Msg(s).c_str());
    if (s != NVAPI_OK) return false;
    g_nv.ready = true;
    return true;
}

// One probe session at a time, never overlapping the product's own.
bool OpenSession(const char* why) {
    if (g_sess != nullptr) return true;
    NvAPI_Status s = g_nv.createSession(&g_sess);
    Log("probe session (%s): NvAPI_DRS_CreateSession -> %s\n", why, Msg(s).c_str());
    if (s != NVAPI_OK || g_sess == nullptr) {
        g_sess = nullptr;
        return false;
    }
    s = g_nv.loadSettings(g_sess);
    Log("probe session (%s): NvAPI_DRS_LoadSettings -> %s\n", why, Msg(s).c_str());
    if (s != NVAPI_OK) {
        g_nv.destroySession(g_sess);
        g_sess = nullptr;
        return false;
    }
    return true;
}

void CloseSession() {
    // ONLY THE SESSION. NvAPI_Unload is never called - see the top of this file.
    if (g_sess == nullptr) return;
    g_nv.destroySession(g_sess);
    g_sess = nullptr;
}

std::wstring FromU(const NvU16* w) {
    const wchar_t* p = reinterpret_cast<const wchar_t*>(w);
    size_t n = 0;
    while (n < 2048 && p[n] != L'\0') ++n;
    return std::wstring(p, n);
}

void ToU(NvU16* dst, const std::wstring& src) {
    std::memset(dst, 0, 2048 * sizeof(NvU16));
    const size_t n = src.size() < 2046 ? src.size() : 2046;
    if (n) std::memcpy(dst, src.c_str(), n * sizeof(wchar_t));
}

// Every application entry a profile covers, in the driver's own order, read in pages of eight.
std::vector<std::wstring> AppsOf(NvDRSProfileHandle profile, bool& truncated) {
    std::vector<std::wstring> out;
    truncated = false;
    std::vector<NVDRS_APPLICATION_V4> page(8);
    for (NvU32 start = 0; start < 256;) {
        std::memset(&page[0], 0, page.size() * sizeof(NVDRS_APPLICATION_V4));
        for (size_t k = 0; k < page.size(); ++k) page[k].version = MAKE_NVAPI_VERSION(NVDRS_APPLICATION_V4, 4);
        NvU32 count = static_cast<NvU32>(page.size());
        if (g_nv.enumApps(g_sess, profile, start, &count, &page[0]) != NVAPI_OK) break;
        if (count == 0) break;
        for (NvU32 k = 0; k < count && k < page.size(); ++k) out.push_back(FromU(page[k].appName));
        if (count < page.size()) break;
        start += count;
        if (start >= 256) truncated = true;
    }
    return out;
}

// 🔴 THE THING THAT MUST NOT CHANGE. Every profile that carries 0x10354FF8 at PROFILE level, in the
// driver's enumeration order, with everything about it this probe can see. Read-only.
bool DumpCudaProfiles(std::string& text, int& found) {
    text.clear();
    found = 0;
    NvU32 total = 0;
    const NvAPI_Status sn = g_nv.numProfiles(g_sess, &total);
    if (sn != NVAPI_OK) {
        text = "NvAPI_DRS_GetNumProfiles failed " + Msg(sn) + "\n";
        return false;
    }
    char line[1024];
    snprintf(line, sizeof line, "TOTAL_PROFILES=%lu\n", static_cast<unsigned long>(total));
    text = line;
    std::vector<NVDRS_SETTING_V1> setting(1);
    for (NvU32 i = 0; i < total; ++i) {
        NvDRSProfileHandle ph = nullptr;
        if (g_nv.enumProfiles(g_sess, i, &ph) != NVAPI_OK || ph == nullptr) break;
        std::memset(&setting[0], 0, sizeof(NVDRS_SETTING_V1));
        setting[0].version = MAKE_NVAPI_VERSION(NVDRS_SETTING_V1, 1);
        if (g_nv.getSetting(g_sess, ph, kCudaExcludedGpus, &setting[0]) != NVAPI_OK) continue;
        if (setting[0].settingLocation != 0) continue;   // profile-level only, as the earlier probe read it
        NVDRS_PROFILE_V1 info;
        std::memset(&info, 0, sizeof info);
        info.version = MAKE_NVAPI_VERSION(NVDRS_PROFILE_V1, 1);
        if (g_nv.profileInfo(g_sess, ph, &info) != NVAPI_OK) continue;
        ++found;
        snprintf(line, sizeof line, "PROFILE '%s' isPredefined=%lu numOfApps=%lu numOfSettings=%lu type=%d\n",
                 U8(FromU(info.profileName)).c_str(), static_cast<unsigned long>(info.isPredefined),
                 static_cast<unsigned long>(info.numOfApps), static_cast<unsigned long>(info.numOfSettings),
                 setting[0].settingType);
        text += line;
        snprintf(line, sizeof line, "  0x10354FF8 = \"%s\"\n", U8(FromU(setting[0].wszCurrentValue)).c_str());
        text += line;
        bool truncated = false;
        const std::vector<std::wstring> apps = AppsOf(ph, truncated);
        for (size_t k = 0; k < apps.size(); ++k) {
            snprintf(line, sizeof line, "  app[%zu] = '%s'\n", k, U8(apps[k]).c_str());
            text += line;
        }
        if (truncated) text += "  (application list truncated at 256)\n";
    }
    snprintf(line, sizeof line, "PROFILES_WITH_0x10354FF8=%d\n", found);
    text += line;
    return true;
}

// How many profiles the whole database holds. Read-only, one call.
//
// 🔴 THIS IS HOW "THE SECOND APPLY UPDATED ITS ENTRY AND DID NOT MAKE A SECOND ONE" IS MEASURED, and
// nothing else in this file can say it. The RECORD cannot: one line per application is what the record
// IS, so it reads the same either way. NvAPI_DRS_FindProfileByName cannot: it hands back one handle
// however many profiles match, so a duplicate is invisible to it. And a count of profiles carrying our
// own name would miss a new entry made under any OTHER name, which is just as wrong. The total moving at
// all, while our entry is standing before and after, is the whole question - and it costs one call
// instead of a sweep of the 13,000 profiles this machine holds.
bool ProfileCount(NvU32& total) {
    total = 0;
    return g_nv.numProfiles(g_sess, &total) == NVAPI_OK;
}

// ---- the guards --------------------------------------------------------------------------------
//
// 🔴 NOTHING IN THIS FILE MUTATES A PROFILE WITHOUT PASSING THROUGH ProbeMadeProfile FIRST, and nothing
// in it hands the PRODUCT a profile name without passing through OwnProfile. They are two different jobs
// and the second is deliberately the stricter of the two.
// 🔴 THREE NAMES SINCE R6-12, AND THE THIRD IS THE ONE THE PRODUCT MUST NEVER MAKE. S24 asserts that a
// second Apply under a differently-cased path does NOT create kDummyProfileUpper - and a guard that
// refused to touch it would leave it in the operator's driver on exactly the run where the rule broke.
bool ProbeMadeProfile(const std::wstring& name) {
    if (name == kDummyProfile || name == kForeignProfile || name == kDummyProfileUpper) return true;
    Log("*** GUARD: refusing to touch profile '%s' - the probe makes only '%s', '%s' and '%s'\n",
        U8(name).c_str(), U8(kDummyProfile).c_str(), U8(kForeignProfile).c_str(),
        U8(kDummyProfileUpper).c_str());
    return false;
}

// The product's writeSetting takes a profile NAME, so the only way to point it at someone else's profile
// is to hand it a different name - and this is the one place a name given to the product is judged.
// 🔴 IT REFUSES S17's FOREIGN ENTRY TOO, and that is the point of having two guards rather than one: that
// entry exists to be refused by the product, so the product reaching it at all is the failure S17 measures.
bool OwnProfile(const std::wstring& name) {
    if (name == kDummyProfile) return true;
    Log("*** GUARD: refusing to let the product touch profile '%s' - it owns only '%s'\n", U8(name).c_str(),
        U8(kDummyProfile).c_str());
    return false;
}

// A handle to one of the two profiles this probe makes, or nullptr. Re-reads GetProfileInfo and refuses
// unless the name still matches exactly AND the driver did not ship the profile itself.
NvDRSProfileHandle FindProbeProfile(const wchar_t* want, bool quiet) {
    if (!ProbeMadeProfile(want)) return nullptr;
    NvAPI_UnicodeString nm;
    ToU(nm, want);
    NvDRSProfileHandle ph = nullptr;
    const NvAPI_Status s = g_nv.findProfile(g_sess, nm, &ph);
    if (!quiet) Log("NvAPI_DRS_FindProfileByName('%s') -> %s\n", U8(want).c_str(), Msg(s).c_str());
    if (s != NVAPI_OK || ph == nullptr) return nullptr;
    NVDRS_PROFILE_V1 info;
    std::memset(&info, 0, sizeof info);
    info.version = MAKE_NVAPI_VERSION(NVDRS_PROFILE_V1, 1);
    if (g_nv.profileInfo(g_sess, ph, &info) != NVAPI_OK) {
        Log("*** GUARD: GetProfileInfo failed - refusing to treat this handle as the probe's own\n");
        return nullptr;
    }
    // The name the driver answered with, character for character - never the name that was asked for.
    if (FromU(info.profileName) != want) {
        Log("*** GUARD: asked for '%s' and the driver answered with '%s' - refusing\n", U8(want).c_str(),
            U8(FromU(info.profileName)).c_str());
        return nullptr;
    }
    if (info.isPredefined != 0) {
        Log("*** GUARD: isPredefined=1 - refusing\n");
        return nullptr;
    }
    return ph;
}

NvDRSProfileHandle FindOwnProfile(bool quiet) { return FindProbeProfile(kDummyProfile, quiet); }

// True when the driver resolves `name` to some profile - used only to refuse before anything is written.
bool AppIsKnown(const wchar_t* name, std::wstring& owner) {
    owner.clear();
    NvAPI_UnicodeString nm;
    ToU(nm, name);
    std::vector<NVDRS_APPLICATION_V4> app(1);
    std::memset(&app[0], 0, sizeof(NVDRS_APPLICATION_V4));
    app[0].version = MAKE_NVAPI_VERSION(NVDRS_APPLICATION_V4, 4);
    NvDRSProfileHandle ph = nullptr;
    if (g_nv.findApp(g_sess, nm, &ph, &app[0]) != NVAPI_OK || ph == nullptr) return false;
    NVDRS_PROFILE_V1 info;
    std::memset(&info, 0, sizeof info);
    info.version = MAKE_NVAPI_VERSION(NVDRS_PROFILE_V1, 1);
    if (g_nv.profileInfo(g_sess, ph, &info) == NVAPI_OK) owner = FromU(info.profileName);
    return true;
}

// Read-only, and used for one thing: proving both before and after S14 that the profile NAME it hands the
// product is a name the driver does not resolve to anything. A probe session must be open.
bool ProfileKnownByName(const wchar_t* name) {
    NvAPI_UnicodeString nm;
    ToU(nm, name);
    NvDRSProfileHandle ph = nullptr;
    return g_nv.findProfile(g_sess, nm, &ph) == NVAPI_OK && ph != nullptr;
}

// 🔴 THE ONE THING THIS PROBE WRITES ITSELF, AND IT EXISTS TO PLAY SOMEBODY ELSE. S8 needs the CUDA
// setting to change BEHIND THE PRODUCT'S BACK - which is what NVIDIA's own Control Panel, or any other
// program, really does between an Apply and a Remove. The product cannot stand in for that: its
// compare-before-write reads through its own session, so a change made there is not behind anything's
// back. This writes through the PROBE's session and saves it, so the product's NEXT session loads it
// from disk exactly as it would load a stranger's change.
//
// Two guards before it writes, the same two the delete path uses: the name is judged by OwnProfile, and
// FindOwnProfile re-reads NvAPI_DRS_GetProfileInfo and refuses unless the name still matches and NVIDIA
// did not ship the profile.
bool SetOwnSetting(const std::wstring& value) {
    if (!OwnProfile(kDummyProfile)) return false;
    const NvDRSProfileHandle ph = FindOwnProfile(true);
    if (ph == nullptr) {
        Log("*** the other session: the probe's own profile is not there to change\n");
        return false;
    }
    std::vector<NVDRS_SETTING_V1> setting(1);
    std::memset(&setting[0], 0, sizeof(NVDRS_SETTING_V1));
    setting[0].version = MAKE_NVAPI_VERSION(NVDRS_SETTING_V1, 1);
    setting[0].settingId = kCudaExcludedGpus;
    setting[0].settingType = 3;   // NVDRS_WSTRING_TYPE, the type src\gpu_cuda.cpp writes and reads
    ToU(setting[0].wszCurrentValue, value);
    const NvAPI_Status ss = g_nv.setSetting(g_sess, ph, &setting[0]);
    const NvAPI_Status sv = g_nv.saveSettings(g_sess);
    Log("the other session: NvAPI_DRS_SetSetting(\"%s\") -> %s; NvAPI_DRS_SaveSettings -> %s\n",
        U8(value).c_str(), Msg(ss).c_str(), Msg(sv).c_str());
    return ss == NVAPI_OK && sv == NVAPI_OK;
}

// 🔴 THE OTHER THING THIS PROBE WRITES ITSELF, AND IT EXISTS TO BE REFUSED (S17, E1). Founder decision 22
// narrowed v0.5.8 to entries the product owns by itself, so the rule that now carries the whole feature is
// what it does when an entry is SOMEBODY ELSE'S - and there is no way to measure that against a profile the
// product made, because the name is half of what ownership is decided on.
//
// TWO application entries, not one, and that is the whole reason this call exists rather than a one-liner:
// a single-member entry under a foreign name is refused on the name alone, and the count E1's sentence has
// to state would be zero. With a second member the refusal must both name the entry AND say it covers one
// other program, which is the sentence the user actually reads.
//
// The value is NVIDIA's own "none": a legal driver value the product would never write for this machine,
// so "nothing moved" is a real comparison. Everything here goes through ProbeMadeProfile first, and this
// profile is never handed to the product - OwnProfile refuses it.
bool MakeForeignProfile() {
    if (!ProbeMadeProfile(kForeignProfile)) return false;
    NVDRS_PROFILE_V1 info;
    std::memset(&info, 0, sizeof info);
    info.version = MAKE_NVAPI_VERSION(NVDRS_PROFILE_V1, 1);
    ToU(info.profileName, kForeignProfile);
    NvDRSProfileHandle ph = nullptr;
    const NvAPI_Status cp = g_nv.createProfile(g_sess, &info, &ph);
    Log("S17 setup: NvAPI_DRS_CreateProfile('%s') -> %s\n", U8(kForeignProfile).c_str(), Msg(cp).c_str());
    if (cp != NVAPI_OK || ph == nullptr) return false;
    const wchar_t* keys[2] = { kDummyAppKey, kForeignNeighbourKey };
    const wchar_t* friendly[2] = { kDummyBaseName, kForeignNeighbourBase };
    for (int i = 0; i < 2; ++i) {
        std::vector<NVDRS_APPLICATION_V4> app(1);
        std::memset(&app[0], 0, sizeof(NVDRS_APPLICATION_V4));
        app[0].version = MAKE_NVAPI_VERSION(NVDRS_APPLICATION_V4, 4);
        app[0].isPredefined = 0;
        ToU(app[0].appName, keys[i]);
        ToU(app[0].userFriendlyName, friendly[i]);
        const NvAPI_Status ca = g_nv.createApp(g_sess, ph, &app[0]);
        Log("S17 setup: NvAPI_DRS_CreateApplication('%s') -> %s\n", U8(keys[i]).c_str(), Msg(ca).c_str());
        // A half-made entry is still an entry: the caller takes it away either way, and so does CleanUp.
        if (ca != NVAPI_OK) return false;
    }
    std::vector<NVDRS_SETTING_V1> setting(1);
    std::memset(&setting[0], 0, sizeof(NVDRS_SETTING_V1));
    setting[0].version = MAKE_NVAPI_VERSION(NVDRS_SETTING_V1, 1);
    setting[0].settingId = kCudaExcludedGpus;
    setting[0].settingType = 3;   // NVDRS_WSTRING_TYPE, the type src\gpu_cuda.cpp writes and reads
    ToU(setting[0].wszCurrentValue, kForeignValue);
    const NvAPI_Status ss = g_nv.setSetting(g_sess, ph, &setting[0]);
    const NvAPI_Status sv = g_nv.saveSettings(g_sess);
    Log("S17 setup: NvAPI_DRS_SetSetting(\"%s\") -> %s; NvAPI_DRS_SaveSettings -> %s\n",
        U8(kForeignValue).c_str(), Msg(ss).c_str(), Msg(sv).c_str());
    return ss == NVAPI_OK && sv == NVAPI_OK;
}

// Read-only: how many application entries S17's foreign entry covers, and what 0x10354FF8 holds in it.
// 🔴 THIS IS THE CHECK THAT THE REFUSAL REALLY REFUSED. A row that reports NvidiaManagesIt and had already
// moved the value would pass every other assertion in this file, because every other one reads the
// product's own answer rather than the driver.
bool ForeignProfileState(size_t& apps, std::wstring& value) {
    apps = 0;
    value.clear();
    const NvDRSProfileHandle ph = FindProbeProfile(kForeignProfile, true);
    if (ph == nullptr) return false;
    bool truncated = false;
    const std::vector<std::wstring> list = AppsOf(ph, truncated);
    apps = list.size();
    for (size_t k = 0; k < list.size(); ++k)
        Log("S17: the foreign entry still covers '%s'\n", U8(list[k]).c_str());
    std::vector<NVDRS_SETTING_V1> setting(1);
    std::memset(&setting[0], 0, sizeof(NVDRS_SETTING_V1));
    setting[0].version = MAKE_NVAPI_VERSION(NVDRS_SETTING_V1, 1);
    if (g_nv.getSetting(g_sess, ph, kCudaExcludedGpus, &setting[0]) != NVAPI_OK) return false;
    value = FromU(setting[0].wszCurrentValue);
    return !truncated;
}

// 🔴 S18's OTHER SETTING, WRITTEN THROUGH THE PROBE'S OWN SESSION (R5-1). It plays the user opening NVIDIA
// Control Panel and setting something else for the same application, which is the state that makes deleting
// the whole profile destructive. Two guards first, the same two SetOwnSetting uses: the name is judged by
// OwnProfile, and FindOwnProfile re-reads GetProfileInfo and refuses unless the name still matches and
// NVIDIA did not ship the profile.
bool SetOwnOtherSetting() {
    if (!OwnProfile(kDummyProfile)) return false;
    const NvDRSProfileHandle ph = FindOwnProfile(true);
    if (ph == nullptr) {
        Log("S18 setup: the probe's own profile is not there to add a second setting to\n");
        return false;
    }
    std::vector<NVDRS_SETTING_V1> setting(1);
    std::memset(&setting[0], 0, sizeof(NVDRS_SETTING_V1));
    setting[0].version = MAKE_NVAPI_VERSION(NVDRS_SETTING_V1, 1);
    setting[0].settingId = kOglImplicitAffinity;
    setting[0].settingType = 3;   // NVDRS_WSTRING_TYPE
    ToU(setting[0].wszCurrentValue, kOtherSettingValue);
    const NvAPI_Status ss = g_nv.setSetting(g_sess, ph, &setting[0]);
    const NvAPI_Status sv = g_nv.saveSettings(g_sess);
    Log("S18 setup: NvAPI_DRS_SetSetting(0x%08lX, \"%s\") -> %s; NvAPI_DRS_SaveSettings -> %s\n",
        static_cast<unsigned long>(kOglImplicitAffinity), U8(kOtherSettingValue).c_str(), Msg(ss).c_str(),
        Msg(sv).c_str());
    return ss == NVAPI_OK && sv == NVAPI_OK;
}

// 🔴 S24's SETUP (R6-12): THE APPLICATION COMES OUT AND THE ENTRY STAYS STANDING. That is the one state
// the adopt path exists for, and nothing else in this file arranges it - an entry this product made whose
// application a user has since removed in NVIDIA Control Panel, with our record line still naming it. It
// is the only state in which the driver's lookup for the EXECUTABLE answers Absent while a line of ours
// claims an entry that really is there, which is the branch where the profile name gets looked up by name.
//
// Two guards first, the same two SetOwnSetting uses: the name is judged by OwnProfile, and FindOwnProfile
// re-reads GetProfileInfo and refuses unless the name still matches and NVIDIA did not ship the profile.
// Exactly ONE application entry is removed - the one this probe put there - and the profile is left alone.
bool StripDummyApplication() {
    if (!OwnProfile(kDummyProfile)) return false;
    const NvDRSProfileHandle ph = FindOwnProfile(true);
    if (ph == nullptr) {
        Log("S24 setup: the probe's own profile is not there to take the application out of\n");
        return false;
    }
    std::vector<NVDRS_APPLICATION_V4> one(1);
    std::memset(&one[0], 0, sizeof(NVDRS_APPLICATION_V4));
    one[0].version = MAKE_NVAPI_VERSION(NVDRS_APPLICATION_V4, 4);
    ToU(one[0].appName, kDummyAppKey);
    const NvAPI_Status sd = g_nv.deleteApp(g_sess, ph, &one[0]);
    const NvAPI_Status sv = g_nv.saveSettings(g_sess);
    Log("S24 setup: NvAPI_DRS_DeleteApplicationEx('%s') -> %s; NvAPI_DRS_SaveSettings -> %s\n",
        U8(kDummyAppKey).c_str(), Msg(sd).c_str(), Msg(sv).c_str());
    return sd == NVAPI_OK && sv == NVAPI_OK;
}

// Read-only: what the dummy entry holds RIGHT NOW. 🔴 THIS IS THE CHECK THAT R5-1 REALLY KEPT THEM. Every
// other assertion reads the product's own answer; this reads the driver.
bool DummyProfileState(size_t& apps, NvU32& settings, bool& hasCuda, bool& hasOther) {
    apps = 0;
    settings = 0;
    hasCuda = false;
    hasOther = false;
    const NvDRSProfileHandle ph = FindProbeProfile(kDummyProfile, true);
    if (ph == nullptr) return false;
    NVDRS_PROFILE_V1 info;
    std::memset(&info, 0, sizeof info);
    info.version = MAKE_NVAPI_VERSION(NVDRS_PROFILE_V1, 1);
    if (g_nv.profileInfo(g_sess, ph, &info) != NVAPI_OK) return false;
    settings = info.numOfSettings;
    bool truncated = false;
    apps = AppsOf(ph, truncated).size();
    std::vector<NVDRS_SETTING_V1> one(1);
    std::memset(&one[0], 0, sizeof(NVDRS_SETTING_V1));
    one[0].version = MAKE_NVAPI_VERSION(NVDRS_SETTING_V1, 1);
    hasCuda = g_nv.getSetting(g_sess, ph, kCudaExcludedGpus, &one[0]) == NVAPI_OK;
    std::memset(&one[0], 0, sizeof(NVDRS_SETTING_V1));
    one[0].version = MAKE_NVAPI_VERSION(NVDRS_SETTING_V1, 1);
    hasOther = g_nv.getSetting(g_sess, ph, kOglImplicitAffinity, &one[0]) == NVAPI_OK;
    return !truncated;
}

// 🔴 S19's SETUP, AND IT IS THE ROUND-4 HIGH ON THE REAL DRIVER (R5-2). An entry carrying THE PRODUCT'S OWN
// NAME that covers somebody ELSE'S application and not the dummy's. The driver's lookup for the dummy
// executable therefore answers Absent - which is exactly the branch that used to go straight to
// CreateApplication with no GetProfileInfo at all - while a record line of ours names that very entry. The
// product must ask the driver who is in it and refuse.
//
// It goes through ProbeMadeProfile, which allows this name, and the probe takes it away again inside the
// scenario; CleanUp removes it too if the run dies here.
bool MakeNamedProfileWithNeighbour() {
    if (!ProbeMadeProfile(kDummyProfile)) return false;
    NVDRS_PROFILE_V1 info;
    std::memset(&info, 0, sizeof info);
    info.version = MAKE_NVAPI_VERSION(NVDRS_PROFILE_V1, 1);
    ToU(info.profileName, kDummyProfile);
    NvDRSProfileHandle ph = nullptr;
    const NvAPI_Status cp = g_nv.createProfile(g_sess, &info, &ph);
    Log("S19 setup: NvAPI_DRS_CreateProfile('%s') -> %s\n", U8(kDummyProfile).c_str(), Msg(cp).c_str());
    if (cp != NVAPI_OK || ph == nullptr) return false;
    std::vector<NVDRS_APPLICATION_V4> app(1);
    std::memset(&app[0], 0, sizeof(NVDRS_APPLICATION_V4));
    app[0].version = MAKE_NVAPI_VERSION(NVDRS_APPLICATION_V4, 4);
    app[0].isPredefined = 0;
    // SOMEBODY ELSE'S application, and deliberately NOT the dummy's: the whole point is an entry of our
    // name that the dummy executable is not in.
    ToU(app[0].appName, kForeignNeighbourKey);
    ToU(app[0].userFriendlyName, kForeignNeighbourBase);
    const NvAPI_Status ca = g_nv.createApp(g_sess, ph, &app[0]);
    const NvAPI_Status sv = g_nv.saveSettings(g_sess);
    Log("S19 setup: NvAPI_DRS_CreateApplication('%s') -> %s; NvAPI_DRS_SaveSettings -> %s\n",
        U8(kForeignNeighbourKey).c_str(), Msg(ca).c_str(), Msg(sv).c_str());
    return ca == NVAPI_OK && sv == NVAPI_OK;
}

// ---- the scenario table ------------------------------------------------------------------------
struct Scn {
    std::string name;
    std::string expected;
    std::string observed;
    bool pass = false;
};
std::vector<Scn> g_scn;

void Record(const char* name, const char* expected, const std::string& observed, bool pass) {
    Scn s;
    s.name = name;
    s.expected = expected;
    s.observed = observed;
    s.pass = pass;
    g_scn.push_back(s);
    // ONE TOKEN, NOT TWO WORDS: a verdict anyone can grep for, and the exit code counts the same rows.
    Log("  %-16s expected: %s\n  %-16s observed: %s\n  %-16s -> %s\n", name, expected, "", observed.c_str(), "",
        pass ? "AS_EXPECTED" : "UNEXPECTED");
}

const char* RefusalName(cd::CudaRefusal r) {
    switch (r) {
        case cd::CudaRefusal::None: return "None";
        case cd::CudaRefusal::NoNvidiaDriver: return "NoNvidiaDriver";
        case cd::CudaRefusal::NoNvidiaGpuForKey: return "NoNvidiaGpuForKey";
        case cd::CudaRefusal::AmbiguousIdenticalCards: return "AmbiguousIdenticalCards";
        case cd::CudaRefusal::NoIdForGpu: return "NoIdForGpu";
        case cd::CudaRefusal::SeveralIdsForGpu: return "SeveralIdsForGpu";
        case cd::CudaRefusal::SeveralToExclude: return "SeveralToExclude";
        case cd::CudaRefusal::SessionRefused: return "SessionRefused";
        case cd::CudaRefusal::NvidiaManagesIt: return "NvidiaManagesIt";
        case cd::CudaRefusal::NameAlreadyInUse: return "NameAlreadyInUse";
        case cd::CudaRefusal::ProfileNameTaken: return "ProfileNameTaken";
        case cd::CudaRefusal::ProfileNotCreated: return "ProfileNotCreated";
        case cd::CudaRefusal::ProfileNotRemoved: return "ProfileNotRemoved";
        case cd::CudaRefusal::NotSaved: return "NotSaved";
        case cd::CudaRefusal::CouldNotRead: return "CouldNotRead";
        case cd::CudaRefusal::MembershipUnknown: return "MembershipUnknown";
        case cd::CudaRefusal::RecordUnreadable: return "RecordUnreadable";
        case cd::CudaRefusal::RecordUnusable: return "RecordUnusable";
        case cd::CudaRefusal::RecordedProfileChanged: return "RecordedProfileChanged";
        case cd::CudaRefusal::ChangedSinceWritten: return "ChangedSinceWritten";
        case cd::CudaRefusal::NotRecorded: return "NotRecorded";
        case cd::CudaRefusal::NotConfirmed: return "NotConfirmed";
        case cd::CudaRefusal::NotUndone: return "NotUndone";
        case cd::CudaRefusal::CouldNotLookUp: return "CouldNotLookUp";
        case cd::CudaRefusal::NoWayToUndo: return "NoWayToUndo";           // R5-9
        case cd::CudaRefusal::NoWayToListSettings: return "NoWayToListSettings";
        case cd::CudaRefusal::SettingNotRemoved: return "SettingNotRemoved";   // R5-1
        default: return "SettingNotWritten";
    }
}

const char* ReadName(cd::CudaRead r) {
    switch (r) {
        case cd::CudaRead::Value: return "Value";
        case cd::CudaRead::Absent: return "Absent";
        default: return "Failed";
    }
}

// Council round 2, F13: a lookup answers three ways as a read does, and a probe that printed only
// "found / not found" could not tell the driver refusing to look from the driver having no entry.
const char* LookupName(cd::CudaLookup l) {
    switch (l) {
        case cd::CudaLookup::Found: return "Found";
        case cd::CudaLookup::Absent: return "Absent";
        default: return "Failed";
    }
}

const char* OutcomeName(cd::CudaOutcome o) {
    switch (o) {
        case cd::CudaOutcome::NotAsked: return "NotAsked";
        case cd::CudaOutcome::AlreadySet: return "AlreadySet";
        case cd::CudaOutcome::Written: return "Written";
        // E3: NVIDIA has no settings entry for the application at all any more, so the state the record
        // asks for is already true. That is not a failure.
        case cd::CudaOutcome::Absent: return "Absent";
        default: return "Refused";
    }
}

const char* RecordStateName(cd::CudaRecordState s) {
    switch (s) {
        case cd::CudaRecordState::Ok: return "Ok";
        case cd::CudaRecordState::Missing: return "Missing";
        default: return "Unreadable";
    }
}

// The record this product keeps (gpu_cuda.h, E2): one line per application, in one file in a folder.
// The probe gives each scenario a folder of its own so a run always starts from the same state.
cd::CudaApplyInputs RecordInto(cd::CudaRecord& record) {
    cd::CudaApplyInputs in;
    in.record = [&record](const cd::CudaRecordRow& row) { return cd::SaveCudaRecordRow(record, row); };
    return in;
}

// ---- the guarded cleanup -----------------------------------------------------------------------
//
// Application entries first, then the profile, then a save, then a read-back. Returns true only when the
// profile is gone - or was never there. A probe session must already be open.
bool CleanUpProfile(const wchar_t* name) {
    NvDRSProfileHandle ph = FindProbeProfile(name, false);
    if (ph == nullptr) {
        Log("cleanup: '%s' is absent - nothing to remove\n", U8(name).c_str());
        return true;
    }
    bool truncated = false;
    const std::vector<std::wstring> apps = AppsOf(ph, truncated);
    Log("cleanup: '%s' covers %zu application entr%s\n", U8(name).c_str(), apps.size(),
        apps.size() == 1 ? "y" : "ies");
    std::vector<NVDRS_APPLICATION_V4> one(1);
    for (size_t k = 0; k < apps.size(); ++k) {
        std::memset(&one[0], 0, sizeof(NVDRS_APPLICATION_V4));
        one[0].version = MAKE_NVAPI_VERSION(NVDRS_APPLICATION_V4, 4);
        ToU(one[0].appName, apps[k]);
        const NvAPI_Status sd = g_nv.deleteApp(g_sess, ph, &one[0]);
        Log("cleanup: NvAPI_DRS_DeleteApplicationEx('%s') -> %s\n", U8(apps[k]).c_str(), Msg(sd).c_str());
    }
    // The guard is asked AGAIN, immediately before the delete, on a freshly read handle.
    NvDRSProfileHandle again = FindProbeProfile(name, true);
    if (again == nullptr) {
        Log("*** cleanup GUARD: '%s' could not be re-confirmed - NOT deleting anything\n", U8(name).c_str());
        return false;
    }
    const NvAPI_Status sp = g_nv.deleteProfile(g_sess, again);
    Log("cleanup: NvAPI_DRS_DeleteProfile('%s') -> %s\n", U8(name).c_str(), Msg(sp).c_str());
    const NvAPI_Status sv = g_nv.saveSettings(g_sess);
    Log("cleanup: NvAPI_DRS_SaveSettings -> %s\n", Msg(sv).c_str());
    if (sp != NVAPI_OK || sv != NVAPI_OK) return false;
    if (FindProbeProfile(name, true) != nullptr) {
        Log("*** cleanup: '%s' is STILL PRESENT after delete + save\n", U8(name).c_str());
        return false;
    }
    Log("cleanup: '%s' is gone\n", U8(name).c_str());
    return true;
}

// 🔴 BOTH NAMES, EVERY TIME, AND NEITHER SHORT-CIRCUITS THE OTHER. S17 takes its own entry away inside the
// scenario, so the second call is normally a no-op that says so - but a run that died between making that
// entry and removing it leaves one behind, and that is exactly the run whose cleanup has to find it.
bool CleanUp() {
    Log("\n=== cleanup (guarded) ===\n");
    if (!OpenSession("cleanup")) return false;
    const bool dummy = CleanUpProfile(kDummyProfile);
    const bool foreign = CleanUpProfile(kForeignProfile);
    // 🔴 R6-12: the name S24 proves is never made. On a correct product this is always "absent - nothing
    // to remove", and it is here for the run where it is NOT - which is the run that must not leave an
    // entry behind in the operator's driver database.
    const bool upper = CleanUpProfile(kDummyProfileUpper);
    return dummy && foreign && upper;
}

void PrintRecovery(const std::wstring& workDir) {
    Log("\nRECOVERY, if either profile this probe makes is still in the driver database:\n"
        "  1. run this probe again with the single argument   cleanup\n"
        "  2. or remove the profiles named below in NVIDIA Control Panel\n"
        "  3. or, as a last resort with the display driver stopped, put the backed-up\n"
        "     nvdrsdb0.bin / nvdrsdb1.bin / nvdrssel.bin from the work folder back into\n"
        "     %s\n"
        "  the product's profile: %s\n"
        "  S17's foreign entry:   %s\n"
        "  S24's cased name:      %s  (this one should never have been made)\n"
        "  work folder: %s\n",
        U8(kDrsDir).c_str(), U8(kDummyProfile).c_str(), U8(kForeignProfile).c_str(),
        U8(kDummyProfileUpper).c_str(), U8(workDir).c_str());
}

}  // namespace

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    const bool cleanupOnly = argc > 1 && std::string(argv[1]) == "cleanup";

    Log("cuda_assign_probe - the product's own CUDA assignment (src\\gpu_cuda.cpp) against the REAL NVIDIA\n"
        "driver, on ONE dummy application. OPT-IN pre-release check; NOT part of Gate A.\n"
        "dummy executable: %s\ndummy profile:    %s\n",
        U8(kDummyExe).c_str(), U8(kDummyProfile).c_str());
    {
        typedef LONG(WINAPI * RtlGetVersionFn)(PRTL_OSVERSIONINFOW);
        RTL_OSVERSIONINFOW vi = {};
        vi.dwOSVersionInfoSize = sizeof(vi);
        const RtlGetVersionFn fn =
            reinterpret_cast<RtlGetVersionFn>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion"));
        if (fn && fn(&vi) == 0)
            Log("windows: %lu.%lu build %lu\n", vi.dwMajorVersion, vi.dwMinorVersion, vi.dwBuildNumber);
        HANDLE tok = nullptr;
        if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) {
            TOKEN_ELEVATION el = {};
            DWORD rl = 0;
            GetTokenInformation(tok, TokenElevation, &el, sizeof el, &rl);
            Log("token: elevated=%lu (the write is expected to need no elevation)\n",
                static_cast<unsigned long>(el.TokenIsElevated));
            CloseHandle(tok);
        }
    }

    // ---- the work folder, beside the executable ------------------------------------------------
    std::wstring exeDir(MAX_PATH, L'\0');
    exeDir.resize(GetModuleFileNameW(nullptr, &exeDir[0], MAX_PATH));
    exeDir.resize(exeDir.find_last_of(L'\\') + 1);
    const std::wstring workDir = exeDir + L"cuda-probe";
    CreateDirectoryW(workDir.c_str(), nullptr);
    const std::wstring recordDir = workDir + L"\\record";
    CreateDirectoryW(recordDir.c_str(), nullptr);
    // A FOLDER OF ITS OWN PER SCENARIO, AND THE REASON IS THAT THIS PROBE IS RUN AGAIN. S7 asserts exactly
    // what its own folder holds, so a record written into it by another scenario is still there on the
    // next run and fails S7 for a reason that has nothing to do with the product.
    const std::wstring applyDir = workDir + L"\\record-apply";
    CreateDirectoryW(applyDir.c_str(), nullptr);
    const std::wstring removeDir = workDir + L"\\record-remove";
    CreateDirectoryW(removeDir.c_str(), nullptr);
    const std::wstring abDir = workDir + L"\\record-ab";
    CreateDirectoryW(abDir.c_str(), nullptr);
    const std::wstring oldDir = workDir + L"\\record-oldformat";
    CreateDirectoryW(oldDir.c_str(), nullptr);
    const std::wstring foreignDir = workDir + L"\\record-foreign";
    CreateDirectoryW(foreignDir.c_str(), nullptr);
    // Round 5: one folder each, for the same reason every other scenario has one.
    const std::wstring otherDir = workDir + L"\\record-othersetting";   // S18 (R5-1)
    CreateDirectoryW(otherDir.c_str(), nullptr);
    const std::wstring adoptDir = workDir + L"\\record-adopt";          // S19 (R5-2)
    CreateDirectoryW(adoptDir.c_str(), nullptr);
    const std::wstring gateDir = workDir + L"\\record-alreadypinned";   // S20 (R5-3)
    CreateDirectoryW(gateDir.c_str(), nullptr);
    // Round 6: one folder each, as every other scenario has.
    const std::wstring notOursDir = workDir + L"\\record-settingnotours"; // S21 (R6-1, R6-7)
    CreateDirectoryW(notOursDir.c_str(), nullptr);
    const std::wstring orphanDir = workDir + L"\\record-orphan";          // S22 (R6-3)
    CreateDirectoryW(orphanDir.c_str(), nullptr);
    const std::wstring caseDir = workDir + L"\\record-namecase";          // S24 (R6-12)
    CreateDirectoryW(caseDir.c_str(), nullptr);
    // Every run starts from a clean record, so S7's counts are about this run and not the last one - and
    // that means the legacy gpu-cuda-before-*.txt files as well, see ClearRecordFolder.
    ClearRecordFolder(recordDir);
    ClearRecordFolder(applyDir);
    ClearRecordFolder(removeDir);
    ClearRecordFolder(abDir);
    ClearRecordFolder(oldDir);
    ClearRecordFolder(foreignDir);
    ClearRecordFolder(otherDir);
    ClearRecordFolder(adoptDir);
    ClearRecordFolder(gateDir);
    ClearRecordFolder(notOursDir);
    ClearRecordFolder(orphanDir);
    ClearRecordFolder(caseDir);
    Log("work folder: %s\n  record folders: %s (section 4), %s (S7), %s (S9),\n"
        "    %s (S13), %s (S16), %s (S17), %s (S18), %s (S19), %s (S20), %s (S21), %s (S22)\n"
        "    and %s (S24)\n",
        U8(workDir).c_str(), U8(applyDir).c_str(), U8(recordDir).c_str(), U8(removeDir).c_str(),
        U8(abDir).c_str(), U8(oldDir).c_str(), U8(foreignDir).c_str(), U8(otherDir).c_str(),
        U8(adoptDir).c_str(), U8(gateDir).c_str(), U8(notOursDir).c_str(), U8(orphanDir).c_str(),
        U8(caseDir).c_str());

    if (!LoadNvapi()) {
        Log("REFUSED: NVAPI could not be prepared - there is nothing to probe\nPROBE_EXIT=3\n");
        return 3;
    }

    if (cleanupOnly) {
        const bool ok = CleanUp();
        CloseSession();
        if (!ok) PrintRecovery(workDir);
        Log("CLEANUP_ONLY=%s\nPROBE_EXIT=%d\n", ok ? "OK" : "FAILED", ok ? 0 : 4);
        return ok ? 0 : 4;
    }

    // ---- 1. back the driver database up, and hash it -------------------------------------------
    Log("\n=== 1. backup and fingerprint of the driver database ===\n");
    unsigned long long liveBefore = 1469598103934665603ULL;
    for (int i = 0; i < 3; ++i) {
        const std::wstring src = std::wstring(kDrsDir) + L"\\" + kDbFiles[i];
        const std::wstring dst = workDir + L"\\" + kDbFiles[i] + L".bak";
        std::vector<unsigned char> bytes;
        if (!ReadWholeFile(src, bytes)) {
            Log("REFUSED: could not read %s (gle=%lu)\nPROBE_EXIT=3\n", U8(src).c_str(), GetLastError());
            CloseSession();
            return 3;
        }
        if (!CopyFileW(src.c_str(), dst.c_str(), FALSE)) {
            Log("REFUSED: could not back %s up to %s (gle=%lu)\nPROBE_EXIT=3\n", U8(src).c_str(), U8(dst).c_str(),
                GetLastError());
            CloseSession();
            return 3;
        }
        const unsigned long long h = Fnv(bytes.empty() ? "" : reinterpret_cast<const char*>(&bytes[0]), bytes.size());
        liveBefore = Fnv(&h, sizeof h, liveBefore);
        Log("BACKUP %-14s %9zu bytes  #%016llx  ->  %s\n", U8(kDbFiles[i]).c_str(), bytes.size(), h, U8(dst).c_str());
    }
    Log("FINGERPRINT_BEFORE=#%016llx (the three live files; information only, see the header)\n", liveBefore);

    // ---- 2. the CUDA-profile dump, before -------------------------------------------------------
    Log("\n=== 2. every profile carrying 0x10354FF8, before ===\n");
    if (!OpenSession("dump-before")) {
        Log("REFUSED: NVIDIA's settings could not be opened\nPROBE_EXIT=3\n");
        return 3;
    }
    std::string dumpBefore;
    int cudaBefore = 0;
    if (!DumpCudaProfiles(dumpBefore, cudaBefore)) {
        Log("REFUSED: the dump failed: %s\nPROBE_EXIT=3\n", dumpBefore.c_str());
        CloseSession();
        return 3;
    }
    WriteTextFile(workDir + L"\\cuda-profiles-before.txt", dumpBefore);
    const unsigned long long dumpBeforeHash = Fnv(dumpBefore.data(), dumpBefore.size());
    Log("%s", dumpBefore.c_str());
    Log("DUMP_BEFORE_FINGERPRINT=#%016llx  (%s)\n", dumpBeforeHash,
        U8(workDir + L"\\cuda-profiles-before.txt").c_str());

    // ---- 3. refuse if the driver already knows this profile or this executable ------------------
    // 🔴 EVERY NAME THIS RUN WILL MAKE, NOT JUST THE DUMMY. S17's foreign entry and its neighbour are made
    // by this probe too, so a run that died with either still in the driver must be reported and not
    // written over - and its neighbour's BASE NAME matters for the same reason the dummy's does: [M]
    // NvAPI_DRS_CreateApplication answers -167 when another profile already claims one.
    Log("\n=== 3. the dummy AND S17's foreign names must be unknown to the driver ===\n");
    {
        const bool profileThere = FindOwnProfile(false) != nullptr;
        const bool foreignThere = FindProbeProfile(kForeignProfile, false) != nullptr;
        // 🔴 R6-12: S24's differently-cased name too. A run that died with the product's old recompute
        // still in it leaves exactly this entry behind, and writing over it would hide the very defect.
        const bool upperThere = FindProbeProfile(kDummyProfileUpper, false) != nullptr;
        std::wstring ownerFull, ownerBase, ownerNbrFull, ownerNbrBase;
        const bool fullThere = AppIsKnown(kDummyAppKey, ownerFull);
        const bool baseThere = AppIsKnown(kDummyBaseName, ownerBase);
        const bool nbrFullThere = AppIsKnown(kForeignNeighbourKey, ownerNbrFull);
        const bool nbrBaseThere = AppIsKnown(kForeignNeighbourBase, ownerNbrBase);
        Log("dummy profile present: %s\n", profileThere ? "YES" : "no");
        Log("S17's foreign entry present: %s\n", foreignThere ? "YES" : "no");
        Log("S24's differently-cased name present: %s\n", upperThere ? "YES" : "no");
        Log("application '%s' known: %s%s%s\n", U8(kDummyAppKey).c_str(), fullThere ? "YES" : "no",
            fullThere ? " - owned by " : "", fullThere ? U8(ownerFull).c_str() : "");
        Log("application '%s' known: %s%s%s\n", U8(kDummyBaseName).c_str(), baseThere ? "YES" : "no",
            baseThere ? " - owned by " : "", baseThere ? U8(ownerBase).c_str() : "");
        Log("application '%s' known: %s%s%s\n", U8(kForeignNeighbourKey).c_str(), nbrFullThere ? "YES" : "no",
            nbrFullThere ? " - owned by " : "", nbrFullThere ? U8(ownerNbrFull).c_str() : "");
        Log("application '%s' known: %s%s%s\n", U8(kForeignNeighbourBase).c_str(), nbrBaseThere ? "YES" : "no",
            nbrBaseThere ? " - owned by " : "", nbrBaseThere ? U8(ownerNbrBase).c_str() : "");
        Log("a file exists at the dummy path: %s (it never has to - the driver stores a string)\n",
            FileExists(kDummyExe) ? "yes" : "no");
        Log("a file exists at %s: %s\n", U8(kForeignNeighbourExe).c_str(),
            FileExists(kForeignNeighbourExe) ? "yes" : "no");
        if (profileThere || foreignThere || upperThere || fullThere || baseThere || nbrFullThere ||
            nbrBaseThere) {
            Log("\nREFUSED: the driver already knows one of the probe's own names. Nothing was written.\n");
            PrintRecovery(workDir);
            CloseSession();
            Log("PROBE_EXIT=2\n");
            return 2;
        }
    }
    // The probe's session is closed before the product opens its own: one DRS session at a time.
    CloseSession();

    // ---- 4. the product's own code path ---------------------------------------------------------
    Log("\n=== 4. the product: cd::MakeCudaOps() and the real Apply / Remove functions ===\n");
    int unexpected = 0;
    bool cleanupOk = true;
    // Section 4b needs the same target section 4 used, and it opens its own sessions, so the one thing
    // that outlives the block below is the value every scenario writes and compares against.
    cd::CudaTarget target;
    // The Windows adapter key that target was built from. S20 needs it as well, because R5-3 is a rule in
    // the ROW LOOP and the row loop's own subject is Windows' per-application GPU preference.
    std::wstring nvTargetKey;
    // GPU B, for S13. [M] on this machine there are two NVIDIA cards, so this is the product's own target
    // for the OTHER one and S13 is literally "assign it to card A, then to card B".
    cd::CudaTarget targetB;
    // The record section 4's Apply writes into, read back from disk before anything is written.
    cd::CudaRecord applyRecord = cd::ReadCudaRecord(applyDir);
    {
        cd::CudaOps ops = cd::MakeCudaOps();
        Log("cd::MakeCudaOps(): available=%d openRefusal=%s\n", ops.available ? 1 : 0, RefusalName(ops.openRefusal));
        if (!ops.available) {
            Log("REFUSED: the product could not open NVIDIA's settings\nPROBE_EXIT=3\n");
            return 3;
        }
        const std::vector<cd::NvidiaGpu> gpus = ops.listGpus();
        const std::vector<std::wstring> ids = ops.listIds();
        Log("ops.listGpus() -> %zu NVIDIA GPU(s):\n", gpus.size());
        for (size_t i = 0; i < gpus.size(); ++i)
            Log("   [%zu] adapterKey=%s busId=%lu\n", i, U8(gpus[i].adapterKey).c_str(), gpus[i].busId);
        Log("ops.listIds() -> %zu id string(s) from 0x20D0F3E6:\n", ids.size());
        for (size_t i = 0; i < ids.size(); ++i) Log("   [%zu] \"%s\"\n", i, U8(ids[i]).c_str());
        if (gpus.empty()) {
            ops.close();
            Log("REFUSED: the driver enumerated no NVIDIA GPU\nPROBE_EXIT=3\n");
            return 3;
        }

        // The NVIDIA target: this machine's first NVIDIA card. On a two-card machine the plan excludes the
        // other one by a real id string; on a one-card machine the value is "none", which is the same code
        // path with a different value and is said plainly rather than silently skipped.
        const std::wstring nvKey = gpus[0].adapterKey;
        nvTargetKey = nvKey;
        target = cd::CudaTargetFor(nvKey, gpus, ids);
        Log("\ncd::CudaTargetFor(NVIDIA key %s) -> act=%d refusal=%s value=\"%s\"\n", U8(nvKey).c_str(),
            target.act ? 1 : 0, RefusalName(target.refusal), U8(target.value).c_str());
        if (!target.act) {
            ops.close();
            Log("REFUSED: this machine's GPUs give the product nothing to write\nPROBE_EXIT=3\n");
            return 3;
        }

        // A SECOND, DIFFERENT VALUE - the one S13 assigns to after the first. With two NVIDIA cards it is
        // the product's own target for the other one. With one card CudaTargetFor has only a single answer,
        // so the value is taken VERBATIM from the driver's own enumeration instead - still a string the
        // driver itself offered, never a synthesised one (gpu_cuda.h forbids synthesis outright) - because
        // what S13 measures is the record telling two written values apart, not how many cards are fitted.
        for (size_t i = 1; i < gpus.size() && !targetB.act; ++i) {
            const cd::CudaTarget t = cd::CudaTargetFor(gpus[i].adapterKey, gpus, ids);
            if (t.act && t.value != target.value) targetB = t;
        }
        for (size_t i = 0; i < ids.size() && !targetB.act; ++i) {
            if (!cd::CudaValueIsLegal(ids[i]) || ids[i] == target.value) continue;
            targetB.act = true;
            targetB.value = ids[i];
            Log("second target: no other NVIDIA card, so a value copied verbatim from the driver's own "
                "enumeration\n");
        }
        Log("S13's second target: act=%d value=\"%s\"\n", targetB.act ? 1 : 0, U8(targetB.value).c_str());

        // A non-NVIDIA target: a real adapter from this machine if there is one, else a plausible AMD key.
        std::wstring nonNvKey;
        {
            std::vector<cd::GpuAdapter> adapters;
            std::wstring err;
            cd::EnumerateGpuAdapters(adapters, &err);
            for (size_t i = 0; i < adapters.size() && nonNvKey.empty(); ++i)
                if (!adapters[i].adapterKey.empty() && !cd::IsNvidiaAdapterKey(adapters[i].adapterKey))
                    nonNvKey = adapters[i].adapterKey;
            Log("non-NVIDIA target: %s (%s)\n", nonNvKey.empty() ? U8(kFallbackNonNvidiaKey).c_str() : U8(nonNvKey).c_str(),
                nonNvKey.empty() ? "no non-NVIDIA adapter on this machine, so a fallback key"
                                 : "a real adapter on this machine");
            if (nonNvKey.empty()) nonNvKey = kFallbackNonNvidiaKey;
        }

        // 🔴 BIG ENOUGH FOR THE LONGEST ROW, AND THE LONGEST ROW IS A SENTENCE. S17 prints the refusal the
        // USER reads, which names a profile and counts its other programs; a truncated observation is a
        // scenario line nobody can check.
        char buf[2048];

        // S1 - a target that is not an NVIDIA GPU: nothing planned, nothing said, NOTHING WRITTEN.
        Log("\n--- S1: the target is not an NVIDIA GPU (founder decision 18) ---\n");
        {
            const cd::CudaTarget t = cd::CudaTargetFor(nonNvKey, gpus, ids);
            const cd::CudaRowResult r =
                cd::WriteCudaForRow(kDummyExe, t, ops, applyRecord, RecordInto(applyRecord));
            cd::CudaProfile after;
            const cd::CudaLookup look = ops.findProfileForExe(kDummyExe, after);
            snprintf(buf, sizeof buf, "target.act=%d target.refusal=%s outcome=%s refusal=%s; profile now=%s",
                     t.act ? 1 : 0, RefusalName(t.refusal), OutcomeName(r.outcome), RefusalName(r.refusal),
                     LookupName(look));
            // 🔴 Absent, NOT "anything but Found". A lookup that FAILED would also pass that test while
            // meaning the opposite - nobody knows what NVIDIA holds - which is the one confusion
            // cd::CudaLookup exists to prevent, so this file may not make it either.
            Record("S1 non-NVIDIA", "target.act=0 refusal=None, outcome=NotAsked, no profile created", buf,
                   !t.act && t.refusal == cd::CudaRefusal::None && r.outcome == cd::CudaOutcome::NotAsked &&
                       r.refusal == cd::CudaRefusal::None && look == cd::CudaLookup::Absent);
        }

        // S2 - the exclusion is written for an NVIDIA target.
        Log("\n--- S2: set the CUDA exclusion for an NVIDIA target ---\n");
        {
            // 🔴 READ-ONLY PRE-FLIGHT. If the driver already resolved the dummy executable to somebody
            // else's profile, the product would write THERE - so the run stops instead.
            const cd::CudaRowPlan plan = cd::PlanCudaForRow(kDummyExe, target, ops, applyRecord);
            Log("cd::PlanCudaForRow: act=%d profileExists=%d profileName='%s'\n", plan.act ? 1 : 0,
                plan.profileExists ? 1 : 0, U8(plan.profileName).c_str());
            if (plan.profileExists) {
                ops.close();
                Log("REFUSED: the driver resolves the dummy executable to an existing profile\n");
                PrintRecovery(workDir);
                Log("PROBE_EXIT=2\n");
                return 2;
            }
            const cd::CudaRowResult r =
                cd::WriteCudaForRow(kDummyExe, target, ops, applyRecord, RecordInto(applyRecord));
            // 🔴 THE LINE IS IN THE RECORD BEFORE THE SAVE, so a change on the driver is always one Remove
            // assignment can see. E2: the line IS the entry, the value and the time - there is no original to
            // keep, because before this row NVIDIA had no settings entry for the application at all.
            const cd::CudaRecordRow* wrote = cd::CudaLineFor(applyRecord, kDummyExe);
            snprintf(buf, sizeof buf,
                     "outcome=%s refusal=%s profile='%s' appEntry='%s' hadPrevious=%d recorded=%d; the "
                     "record now holds %zu line(s), lastWrote=\"%s\"",
                     OutcomeName(r.outcome), RefusalName(r.refusal), U8(r.profileName).c_str(),
                     U8(r.appEntry).c_str(), r.hadPrevious ? 1 : 0, r.recorded ? 1 : 0,
                     applyRecord.rows.size(), wrote ? U8(wrote->lastWrote).c_str() : "");
            const bool pass = r.outcome == cd::CudaOutcome::Written && r.profileName == kDummyProfile &&
                              r.appEntry == kDummyAppKey && !r.hadPrevious && r.recorded && wrote != nullptr &&
                              applyRecord.rows.size() == 1 && wrote->lastWrote == target.value;
            Record("S2 write",
                   "outcome=Written into an entry the product made for the dummy, nothing previous, ONE "
                   "record line naming it", buf, pass);
            if (!pass && r.outcome == cd::CudaOutcome::Written && !OwnProfile(r.profileName)) {
                // The product wrote somewhere this probe may not clean up. Say so as loudly as possible.
                Log("*** THE PRODUCT WROTE TO A PROFILE THE PROBE DOES NOT OWN. It will not be deleted here.\n");
                ops.close();
                PrintRecovery(workDir);
                Log("PROBE_EXIT=1\n");
                return 1;
            }
        }

        // S3 - read it back through the product's own reader.
        Log("\n--- S3: read the value back ---\n");
        {
            std::wstring value;
            const cd::CudaRead read = OwnProfile(kDummyProfile) ? ops.readSetting(kDummyProfile, value)
                                                                : cd::CudaRead::Failed;
            const cd::CudaRowPlan plan = cd::PlanCudaForRow(kDummyExe, target, ops, applyRecord);
            snprintf(buf, sizeof buf,
                     "readSetting=%s value=\"%s\"; re-plan: profileExists=%d hadPrevious=%d "
                     "alreadySet=%d previous=\"%s\"",
                     ReadName(read), U8(value).c_str(), plan.profileExists ? 1 : 0,
                     plan.hadPrevious ? 1 : 0, plan.alreadySet ? 1 : 0, U8(plan.previousValue).c_str());
            Record("S3 read back", "the driver holds exactly the value the plan asked for", buf,
                   read == cd::CudaRead::Value && value == target.value && plan.profileExists &&
                       plan.hadPrevious && plan.alreadySet && plan.previousValue == target.value);
        }

        // S4 - asking again writes nothing.
        Log("\n--- S4: the same Apply again ---\n");
        {
            const size_t before = applyRecord.rows.size();
            const cd::CudaRowResult r =
                cd::WriteCudaForRow(kDummyExe, target, ops, applyRecord, RecordInto(applyRecord));
            snprintf(buf, sizeof buf, "outcome=%s previous=\"%s\"; record lines %zu -> %zu",
                     OutcomeName(r.outcome), U8(r.previousValue).c_str(), before, applyRecord.rows.size());
            Record("S4 already set", "outcome=AlreadySet, nothing written and nothing recorded", buf,
                   r.outcome == cd::CudaOutcome::AlreadySet && r.previousValue == target.value &&
                       applyRecord.rows.size() == before);
        }

        // S5 - 🔴 E1 ON THE REAL DRIVER, AND READ-ONLY. The entry S2 made is ours ONLY because a line of
        // ours names it. Hand the very same driver state to a plan with an EMPTY record and the product
        // must refuse it as an entry NVIDIA manages - naming it and counting its other programs - rather
        // than writing on it. Nothing is created, written or saved here.
        Log("\n--- S5: an entry no record line of ours claims is not ours to write on (E1) ---\n");
        {
            const cd::CudaRecord nothingRecorded;
            const cd::CudaRowPlan plan = cd::PlanCudaForRow(kDummyExe, target, ops, nothingRecorded);
            cd::CudaRowResult asRow;
            asRow.refusal = plan.refusal;
            asRow.profileName = plan.profileName;
            asRow.otherApps = plan.otherApps;
            const std::wstring says = cd::CudaRowRefusalText(asRow);
            snprintf(buf, sizeof buf, "act=%d refusal=%s profile='%s' otherApps=%zu; it says: %s",
                     plan.act ? 1 : 0, RefusalName(plan.refusal), U8(plan.profileName).c_str(),
                     plan.otherApps, U8(says).c_str());
            Record("S5 not ours",
                   "act=0 refusal=NvidiaManagesIt, naming the entry and counting no other programs", buf,
                   !plan.act && plan.refusal == cd::CudaRefusal::NvidiaManagesIt &&
                       plan.profileName == kDummyProfile && plan.otherApps == 0 &&
                       says.find(kDummyProfile) != std::wstring::npos);
        }

        // S6 - 🔴 E3: REMOVE TAKES THE ENTRY AWAY, SETTING AND ALL. The state before this product ever
        // touched the application was "NVIDIA has no settings entry for it", and the only way back to that
        // is to remove the entry the product made. So the checks below expect the PROFILE to be gone, not
        // the setting to be absent from a profile that is still standing.
        Log("\n--- S6: Remove assignment takes away the entry the product made ---\n");
        {
            cd::CudaRecordRow rec;
            rec.profileName = kDummyProfile;
            rec.appEntry = kDummyAppKey;
            // What S2 wrote, and what the restore compares the driver with before it touches anything.
            rec.lastWrote = target.value;
            rec.when = L"probe";
            const bool guard = OwnProfile(rec.profileName);
            const cd::CudaRowResult r =
                guard ? cd::RestoreCudaForRow(kDummyExe, rec, ops) : cd::CudaRowResult();
            std::wstring value;
            const cd::CudaRead read = guard ? ops.readSetting(kDummyProfile, value) : cd::CudaRead::Failed;
            cd::CudaProfile after;
            const cd::CudaLookup look = ops.findProfileForExe(kDummyExe, after);
            snprintf(buf, sizeof buf, "outcome=%s refusal=%s profileDeleted=%d; read back=%s value=\"%s\" lookup=%s",
                     OutcomeName(r.outcome), RefusalName(r.refusal), r.profileDeleted ? 1 : 0, ReadName(read),
                     U8(value).c_str(), LookupName(look));
            Record("S6 entry gone",
                   "outcome=Written, the setting is GONE, the entry went with it and the driver answers "
                   "Absent for the dummy again",
                   buf,
                   guard && r.outcome == cd::CudaOutcome::Written && r.profileDeleted &&
                       look == cd::CudaLookup::Absent && read == cd::CudaRead::Failed && value.empty());
        }
        // S7 - the other half of gpu_cuda.cpp: the record file on disk, which no other harness links.
        //
        // 🔴 E2 AND E6 ON A REAL FILE. One line per application however many times Apply runs, and a per-run
        // file from an older format making the whole record UNREADABLE rather than empty.
        Log("\n--- S7: the record on disk (gpu_cuda.cpp, no driver involved) ---\n");
        {
            const cd::CudaRecord fresh = cd::ReadCudaRecord(recordDir);
            cd::CudaRecord record = fresh;
            cd::CudaRecordRow first;
            first.profileName = kDummyProfile;
            first.appEntry = kDummyAppKey;
            first.lastWrote = target.value;
            // A SECOND APPLY, exactly as WriteCudaForRow builds it: the same entry, and only the value this
            // product last wrote moves.
            cd::CudaRecordRow second = first;
            second.lastWrote = cd::CudaNoneValue();
            const bool w1 = cd::SaveCudaRecordRow(record, first);
            const bool w2 = cd::SaveCudaRecordRow(record, second);
            const cd::CudaRecord read = cd::ReadCudaRecord(recordDir);
            const cd::CudaRecordRow* got = cd::CudaLineFor(read, kDummyExe);
            // E6: a per-run file from rounds 1 and 2 makes the record unreadable, and is never parsed.
            const std::wstring legacy = recordDir + L"\\gpu-cuda-before-20260919-120000.txt";
            WriteTextFile(legacy, "this is the format v0.5.8 wrote before round 3\n");
            const cd::CudaRecord poisoned = cd::ReadCudaRecord(recordDir);
            DeleteFileW(legacy.c_str());
            // A line is only spent once the restore that used it was read back - here, by hand.
            cd::CudaRecord spending = cd::ReadCudaRecord(recordDir);
            const bool forgot = cd::ForgetCudaRecordRow(spending, kDummyAppKey);
            const cd::CudaRecord after = cd::ReadCudaRecord(recordDir);
            snprintf(buf, sizeof buf,
                     "path=%s fresh=%s written=%d/%d; read=%s rows=%zu lastWrote=\"%s\"; with a legacy file: "
                     "%s; forgot=%d -> %s rows=%zu",
                     U8(read.path).c_str(), RecordStateName(fresh.state), (w1 ? 1 : 0) + (w2 ? 1 : 0), 2,
                     RecordStateName(read.state), read.rows.size(), got ? U8(got->lastWrote).c_str() : "",
                     RecordStateName(poisoned.state), forgot ? 1 : 0, RecordStateName(after.state),
                     after.rows.size());
            Record("S7 record",
                   "ONE line for the application after two Applies, a legacy per-run file makes the record "
                   "Unreadable rather than empty, and the line can be spent",
                   buf,
                   fresh.state == cd::CudaRecordState::Missing && w1 && w2 &&
                       read.state == cd::CudaRecordState::Ok && read.rows.size() == 1 && got != nullptr &&
                       got->lastWrote == cd::CudaNoneValue() &&
                       poisoned.state == cd::CudaRecordState::Unreadable && poisoned.rows.empty() && forgot &&
                       after.state == cd::CudaRecordState::Ok && after.rows.empty());
        }

        // S16 - 🔴 E6: A gpu-cuda-record.txt THIS VERSION DOES NOT UNDERSTAND IS UNREADABLE, NEVER EMPTY.
        // A file this code cannot parse is still a record of changes the product really made, written by a
        // version that wrote them differently, and reading it as "there was nothing" would leave an
        // application pinned to a card in silence. It must read as UNREADABLE - and the product must also
        // refuse to WRITE OVER it, because rewriting it destroys the only note of those changes.
        //
        // The file is written UTF-16LE with a byte-order mark on purpose: that is the shape the reader
        // accepts, so the read SUCCEEDS and the parse is what refuses. A file the reader could not open
        // would come back Unreadable too and would have measured the wrong half.
        Log("\n--- S16: a gpu-cuda-record.txt this version does not understand (no driver involved) ---\n");
        {
            const std::wstring path = oldDir + L"\\" + cd::CudaRecordFileName();
            // The shape a record had in round 3: a per-row version, an ownership flag and an ORIGINAL field,
            // and no machine-readable header at all. Seven fields, not four, and nothing on the first line.
            const std::wstring roundThree = L"; Game Optimizer - an older gpu-cuda-record.txt\r\n"
                                            L"1\t" + std::wstring(kDummyProfile) + L"\t" + kDummyAppKey +
                                            L"\t1\t(none)\t" + target.value + L"\t2026-09-19 12:00:00\r\n";
            WriteWideTextFile(path, roundThree);
            const unsigned long long beforeSave = FileFingerprint(path);
            cd::CudaRecord oldRec = cd::ReadCudaRecord(oldDir);
            const cd::CudaRecordRow* none = cd::CudaLineFor(oldRec, kDummyExe);
            cd::CudaRecordRow row;
            row.profileName = kDummyProfile;
            row.appEntry = kDummyAppKey;
            row.lastWrote = target.value;
            row.when = L"probe";
            const bool refusedWrite = !cd::SaveCudaRecordRow(oldRec, row);
            const unsigned long long afterSave = FileFingerprint(path);
            // And the two shapes E6 names beside an older format: a file of nothing but comments, and one
            // carrying a header version this code never wrote. A parser that skipped what it did not
            // understand would answer Ok with no rows for both.
            WriteWideTextFile(path, L"; nothing but a comment\r\n");
            const cd::CudaRecord comments = cd::ReadCudaRecord(oldDir);
            WriteWideTextFile(path, cd::CudaRecordMagic() + L"\t99\r\n" + std::wstring(kDummyProfile) +
                                        L"\t" + kDummyAppKey + L"\t" + target.value + L"\tt\r\n");
            const cd::CudaRecord versioned = cd::ReadCudaRecord(oldDir);
            // 🔴 AND THE TWO EMPTIEST SHAPES OF ALL, WHICH E6 NAMES BESIDE THE OTHERS AND WHICH FAIL AT
            // DIFFERENT LAYERS. A file of nought bytes never reaches the parser at all - the reader wants a
            // byte-order mark and there is not even room for one. A file that is NOTHING BUT a byte-order
            // mark reads perfectly and has no header line. Both have to be Unreadable: a record file that
            // EXISTS and says nothing is not the same fact as no record file, and reading it as the second
            // would tell a user their application never had its CUDA GPU changed here.
            WriteTextFile(path, std::string());
            const cd::CudaRecord emptyFile = cd::ReadCudaRecord(oldDir);
            WriteWideTextFile(path, std::wstring());
            const cd::CudaRecord bomOnly = cd::ReadCudaRecord(oldDir);
            DeleteFileW(path.c_str());
            snprintf(buf, sizeof buf,
                     "round-3 line: %s rows=%zu lineForTheExe=%d; a save into it was refused=%d and the "
                     "file on disk is unchanged=%d; comments only: %s rows=%zu; a header version this code "
                     "never wrote: %s rows=%zu; an empty file: %s rows=%zu; a byte-order mark and nothing "
                     "else: %s rows=%zu",
                     RecordStateName(oldRec.state), oldRec.rows.size(), none ? 1 : 0, refusedWrite ? 1 : 0,
                     beforeSave == afterSave ? 1 : 0, RecordStateName(comments.state), comments.rows.size(),
                     RecordStateName(versioned.state), versioned.rows.size(),
                     RecordStateName(emptyFile.state), emptyFile.rows.size(), RecordStateName(bomOnly.state),
                     bomOnly.rows.size());
            Record("S16 old format",
                   "Unreadable every way - an older format, comments only, an unknown header version, an "
                   "empty file and a bare byte-order mark - never Missing, no rows, and the product refuses "
                   "to write over it",
                   buf,
                   oldRec.state == cd::CudaRecordState::Unreadable && oldRec.rows.empty() && none == nullptr &&
                       refusedWrite && beforeSave != 0 && beforeSave == afterSave &&
                       comments.state == cd::CudaRecordState::Unreadable && comments.rows.empty() &&
                       versioned.state == cd::CudaRecordState::Unreadable && versioned.rows.empty() &&
                       emptyFile.state == cd::CudaRecordState::Unreadable && emptyFile.rows.empty() &&
                       bomOnly.state == cd::CudaRecordState::Unreadable && bomOnly.rows.empty());
        }
        ops.close();
        Log("\nops.close(): the product's DRS session is ended (NvAPI_Unload is never called)\n");
    }

    // ---- 4b. the cases no straight line reaches --------------------------------------------------
    //
    // Same dummy application, same guards, same backup and same before/after dump - see the header. Each
    // scenario leaves the driver in the state the next one needs, so a scenario that goes wrong stops the
    // ones after it rather than measuring a state nobody arranged. The last of them leaves the dummy
    // profile gone again, which is the state section 6 compares.
    {
        char buf[2048];
        bool go = target.act;
        // NVIDIA's own word for "nothing is excluded" - a legal driver value, and deliberately NOT one this
        // product would have written for this machine, so S8's compare has something real to notice.
        const std::wstring otherValue = cd::CudaNoneValue();

        // S17 - 🔴 E1'S REAL SUBJECT, ON THE REAL DRIVER: AN ENTRY THAT IS NOT OURS AND COVERS MORE THAN
        // ONE PROGRAM. Founder decision 22 narrowed v0.5.8 to exactly this - the product writes CUDA only
        // where it owns the entry by itself, and every other entry is named and left alone - because [M]
        // an NVIDIA profile can cover many applications while a GPU assignment is per application, and
        // this machine's own Microsoft Edge Beta profile carries six entries under one 0x10354FF8 value.
        //
        // Every other scenario in this file meets an entry the PRODUCT made, so not one of them reaches
        // the refusal the whole feature now rests on. So the probe makes the entry itself, through a
        // session of its own (MakeForeignProfile, and the note above it says why two members and not one),
        // and then Apply is RUN - not planned, run - and must refuse, name the entry, count the other
        // program, write nothing to the driver and put nothing in the record.
        //
        // 🔴 IT RUNS FIRST IN THIS SECTION BECAUSE S6 LEFT THE DRIVER KNOWING NOTHING ABOUT THE DUMMY, and
        // that is the only state in which the foreign entry is the one the driver answers with. It leaves
        // the driver exactly as it found it, which is where S8 starts.
        Log("\n--- S17: an entry that is NOT ours, covering two programs (E1) ---\n");
        if (go) {
            bool clear = false, made = false, ran = false, cleaned = false;
            if (OpenSession("S17: make an entry that is not ours")) {
                std::wstring owner;
                // The BASE names too, and not only the full paths: [M] NvAPI_DRS_CreateApplication
                // answers -167 when another profile claims a base name, and a full path is no escape.
                clear = !ProfileKnownByName(kForeignProfile) && !AppIsKnown(kDummyAppKey, owner) &&
                        !AppIsKnown(kDummyBaseName, owner) && !AppIsKnown(kForeignNeighbourKey, owner) &&
                        !AppIsKnown(kForeignNeighbourBase, owner);
                if (clear) made = MakeForeignProfile();
                else Log("S17 setup: REFUSED, the driver already knows one of these names\n");
                CloseSession();
            }
            cd::CudaRowPlan plan;
            cd::CudaRowResult r;
            std::wstring says;
            // A record folder of its own, cleared at the start of the run, so "nothing was recorded" is a
            // statement about THIS run and not about a file an earlier one left behind.
            cd::CudaRecord noLine = cd::ReadCudaRecord(foreignDir);
            const cd::CudaRecordState noLineState = noLine.state;
            {
                cd::CudaOps ops = cd::MakeCudaOps();
                if (made && ops.available) {
                    ran = true;
                    // NO LINE OF OURS NAMES THIS ENTRY, because no run of this product ever made it - and
                    // under E1 that is exactly what makes an entry somebody else's.
                    plan = cd::PlanCudaForRow(kDummyExe, target, ops, noLine);
                    r = cd::WriteCudaForRow(kDummyExe, target, ops, noLine, RecordInto(noLine));
                    says = cd::CudaRowRefusalText(r);
                }
                ops.close();
            }
            size_t appsAfter = 0;
            std::wstring valueAfter;
            bool readBack = false;
            if (OpenSession("S17: nothing of it moved, and then take it away")) {
                readBack = ForeignProfileState(appsAfter, valueAfter);
                cleaned = CleanUpProfile(kForeignProfile);
                CloseSession();
            }
            bool dummyKnown = true, foreignKnown = true;
            if (OpenSession("S17: the driver must know neither name any more")) {
                std::wstring owner;
                dummyKnown = AppIsKnown(kDummyAppKey, owner);
                foreignKnown = ProfileKnownByName(kForeignProfile);
                CloseSession();
            }
            snprintf(buf, sizeof buf,
                     "made=%d; plan: act=%d refusal=%s profile='%s' otherApps=%zu; Apply: outcome=%s "
                     "refusal=%s recorded=%d; it says: %s; the entry still covers %zu program(s) and holds "
                     "\"%s\"; the record stayed %s with %zu line(s); taken away=%d, and the driver still "
                     "knows the dummy=%d, the entry=%d",
                     made ? 1 : 0, plan.act ? 1 : 0, RefusalName(plan.refusal), U8(plan.profileName).c_str(),
                     plan.otherApps, OutcomeName(r.outcome), RefusalName(r.refusal), r.recorded ? 1 : 0,
                     U8(says).c_str(), appsAfter, U8(valueAfter).c_str(), RecordStateName(noLine.state),
                     noLine.rows.size(), cleaned ? 1 : 0, dummyKnown ? 1 : 0, foreignKnown ? 1 : 0);
            const bool pass =
                clear && made && ran && readBack && !plan.act &&
                plan.refusal == cd::CudaRefusal::NvidiaManagesIt && plan.profileName == kForeignProfile &&
                plan.otherApps == 1 && r.outcome == cd::CudaOutcome::Refused &&
                r.refusal == cd::CudaRefusal::NvidiaManagesIt && r.profileName == kForeignProfile &&
                r.otherApps == 1 && !r.recorded && says.find(kForeignProfile) != std::wstring::npos &&
                says.find(L"1 other program") != std::wstring::npos && appsAfter == 2 &&
                valueAfter == kForeignValue && noLine.state == noLineState && noLine.rows.empty() &&
                cleaned && !dummyKnown && !foreignKnown;
            Record("S17 not ours",
                   "outcome=Refused refusal=NvidiaManagesIt, the sentence NAMES the entry and COUNTS 1 "
                   "other program, its value and both its application entries are untouched, and nothing "
                   "is recorded",
                   buf, pass);
            go = pass;
        } else {
            Record("S17 not ours",
                   "outcome=Refused refusal=NvidiaManagesIt, the sentence NAMES the entry and COUNTS 1 "
                   "other program, its value and both its application entries are untouched, and nothing "
                   "is recorded",
                   "not run: this machine's GPUs gave the product nothing to write", false);
        }

        // S8 - THE UNDO THAT MUST REFUSE (E3, second case). The compare is against the value this product
        // LAST wrote; somebody else's value is not it, so the entry is left standing and the line is kept.
        Log("\n--- S8: somebody else changed the value, so Remove must leave it alone ---\n");
        if (go) {
            bool pass = false;
            bool made = false, changed = false, guard = false;
            cd::CudaRowResult r;
            cd::CudaRead read = cd::CudaRead::Failed;
            std::wstring now;
            {
                cd::CudaOps ops = cd::MakeCudaOps();
                if (ops.available) {
                    // The same read-only pre-flight S2 does: if the driver resolves the dummy to a profile
                    // already, nothing is written and the row is reported unexpected rather than guessed at.
                    const cd::CudaRowPlan pre = cd::PlanCudaForRow(kDummyExe, target, ops, applyRecord);
                    if (!pre.profileExists) {
                        const cd::CudaRowResult w =
                            cd::WriteCudaForRow(kDummyExe, target, ops, applyRecord, RecordInto(applyRecord));
                        made = w.outcome == cd::CudaOutcome::Written && OwnProfile(w.profileName);
                        Log("S8 setup: WriteCudaForRow -> outcome=%s profile='%s' recorded=%d\n",
                            OutcomeName(w.outcome), U8(w.profileName).c_str(), w.recorded ? 1 : 0);
                    } else {
                        Log("S8 setup: REFUSED, the driver already resolves the dummy to '%s'\n",
                            U8(pre.profileName).c_str());
                    }
                    ops.close();   // the product's session ends BEFORE the other one begins
                }
            }
            if (made && OpenSession("the other session")) {
                changed = SetOwnSetting(otherValue);
                CloseSession();
            }
            {
                // A NEW product session: this is Remove, minutes or days later, loading from disk.
                cd::CudaOps ops = cd::MakeCudaOps();
                cd::CudaRecordRow rec;
                rec.profileName = kDummyProfile;
                rec.appEntry = kDummyAppKey;
                rec.lastWrote = target.value;      // what Apply wrote - no longer what is there
                rec.when = L"probe";
                guard = OwnProfile(rec.profileName) && ops.available;
                if (guard) {
                    r = cd::RestoreCudaForRow(kDummyExe, rec, ops);
                    read = ops.readSetting(kDummyProfile, now);
                }
                ops.close();
            }
            snprintf(buf, sizeof buf,
                     "setup written=%d, changed by the other session=%d; outcome=%s refusal=%s; it now reads "
                     "%s \"%s\"",
                     made ? 1 : 0, changed ? 1 : 0, OutcomeName(r.outcome), RefusalName(r.refusal),
                     ReadName(read), U8(now).c_str());
            pass = made && changed && guard && r.outcome == cd::CudaOutcome::Refused &&
                   r.refusal == cd::CudaRefusal::ChangedSinceWritten && read == cd::CudaRead::Value &&
                   now == otherValue;
            Record("S8 conflict",
                   "outcome=Refused refusal=ChangedSinceWritten, and the other value is still there", buf, pass);
            go = pass;
        } else {
            Record("S8 conflict", "outcome=Refused refusal=ChangedSinceWritten, and the other value is still there",
                   "not run: this machine's GPUs gave the product nothing to write", false);
        }

        // S14 - THE ENTRY THE LINE NAMES IS NOT THE ONE THE DRIVER ANSWERS WITH (E3, second case, at the
        // identity level). Two shapes, and a user reaches both through NVIDIA's own Control Panel: the
        // settings entry was RENAMED, so the driver answers with an entry the note does not name; and the
        // entry lost this application, so the entry the note names no longer covers it. Remove used to ask
        // neither question - it wrote on a profile NAME out of a file - and both must be refused with
        // nothing taken away.
        //
        // 🔴 IT RUNS BEFORE S9 BECAUSE S9 TAKES THE ENTRY AWAY. Once the entry is gone the resolver answers
        // Absent and every row below would measure that instead of the identity check.
        //
        // The driver is holding the other session's value from S8, and this scenario must leave it holding
        // exactly that: both lines carry that value, so the compare would pass and only the identity check
        // can be what refuses. The phantom profile name is the one string in this file that does not go
        // through OwnProfile, and the note beside its declaration says why that cannot mutate anything; it
        // is asked of the driver read-only before AND after, and the row fails if it is ever known.
        Log("\n--- S14: the record names an entry identity the driver no longer agrees with ---\n");
        if (go) {
            bool phantomBefore = true, phantomAfter = true, checked = false;
            if (OpenSession("S14: the phantom name must be unknown")) {
                phantomBefore = ProfileKnownByName(kPhantomProfile);
                checked = true;
                CloseSession();
            }
            cd::CudaRowResult renamed, lostApp;
            cd::CudaRead read = cd::CudaRead::Failed;
            std::wstring now;
            bool ran = false;
            if (checked && !phantomBefore) {
                cd::CudaRecordRow base;
                base.appEntry = kDummyAppKey;
                base.lastWrote = otherValue;   // EXACTLY what the driver really holds
                base.when = L"probe";
                cd::CudaRecordRow rowRenamed = base;
                rowRenamed.profileName = kPhantomProfile;
                cd::CudaRecordRow rowLostApp = base;
                rowLostApp.profileName = kDummyProfile;
                rowLostApp.appEntry = kPhantomAppKey;
                cd::CudaOps ops = cd::MakeCudaOps();
                if (ops.available && OwnProfile(rowLostApp.profileName)) {
                    ran = true;
                    renamed = cd::RestoreCudaForRow(kDummyExe, rowRenamed, ops);
                    lostApp = cd::RestoreCudaForRow(kDummyExe, rowLostApp, ops);
                    read = ops.readSetting(kDummyProfile, now);
                }
                ops.close();
            }
            if (OpenSession("S14: the phantom name must still be unknown")) {
                phantomAfter = ProfileKnownByName(kPhantomProfile);
                CloseSession();
            }
            snprintf(buf, sizeof buf,
                     "the phantom profile was known before=%d and after=%d; renamed: outcome=%s refusal=%s; "
                     "the entry lost the application: outcome=%s refusal=%s; the dummy still reads %s \"%s\"",
                     phantomBefore ? 1 : 0, phantomAfter ? 1 : 0, OutcomeName(renamed.outcome),
                     RefusalName(renamed.refusal), OutcomeName(lostApp.outcome), RefusalName(lostApp.refusal),
                     ReadName(read), U8(now).c_str());
            const bool pass = checked && ran && !phantomBefore && !phantomAfter &&
                              renamed.outcome == cd::CudaOutcome::Refused &&
                              renamed.refusal == cd::CudaRefusal::RecordedProfileChanged &&
                              lostApp.outcome == cd::CudaOutcome::Refused &&
                              lostApp.refusal == cd::CudaRefusal::RecordedProfileChanged &&
                              read == cd::CudaRead::Value && now == otherValue;
            Record("S14 identity",
                   "both Refused with RecordedProfileChanged, nothing taken away, and no profile made under "
                   "the name the record gave",
                   buf, pass);
            go = pass;
        } else {
            Record("S14 identity",
                   "both Refused with RecordedProfileChanged, nothing taken away, and no profile made under "
                   "the name the record gave",
                   "not run: the scenario before it did not leave the driver where this one starts", false);
        }

        // S9 - AN UNDO THAT SUCCEEDS, AND THE LINE IT SPENDS (E2, E3). The driver is holding the other
        // session's value from S8, so a note saying "we wrote THAT" is one the compare accepts - and the
        // point is what is left in the record file afterwards.
        Log("\n--- S9: an undo that succeeds, and the record line it spends ---\n");
        if (go) {
            cd::CudaRecord record = cd::ReadCudaRecord(removeDir);
            cd::CudaRecordRow row;
            row.profileName = kDummyProfile;
            row.appEntry = kDummyAppKey;
            row.lastWrote = otherValue;      // what the driver holds now, so the compare passes
            const bool wrote = cd::SaveCudaRecordRow(record, row);
            const cd::CudaRecord onDisk = cd::ReadCudaRecord(removeDir);
            const cd::CudaRecordRow* got = cd::CudaLineFor(onDisk, kDummyExe);
            const bool guard = got != nullptr && OwnProfile(got->profileName);
            cd::CudaRowResult r;
            cd::CudaRead read = cd::CudaRead::Failed;
            std::wstring now;
            cd::CudaProfile leftover;
            cd::CudaLookup look = cd::CudaLookup::Failed;
            {
                cd::CudaOps ops = cd::MakeCudaOps();
                if (guard && ops.available) {
                    r = cd::RestoreCudaForRow(kDummyExe, *got, ops);
                    read = ops.readSetting(kDummyProfile, now);
                    look = ops.findProfileForExe(kDummyExe, leftover);
                }
                ops.close();
            }
            // ONLY AN UNDO THAT REALLY HAPPENED SPENDS ITS LINE, which is what the product does too.
            cd::CudaRecord spending = onDisk;
            const bool spent =
                r.outcome == cd::CudaOutcome::Written && cd::ForgetCudaRecordRow(spending, kDummyAppKey);
            const cd::CudaRecord after = cd::ReadCudaRecord(removeDir);
            const bool secondFinds = cd::CudaLineFor(after, kDummyExe) != nullptr;
            snprintf(buf, sizeof buf,
                     "recorded=%d read=%s rows=%zu found=%d; outcome=%s refusal=%s profileDeleted=%d; it "
                     "reads back %s \"%s\" and the driver answers %s; spent=%d; a second Remove finds a "
                     "line=%d record now %s",
                     wrote ? 1 : 0, RecordStateName(onDisk.state), onDisk.rows.size(), got ? 1 : 0,
                     OutcomeName(r.outcome), RefusalName(r.refusal), r.profileDeleted ? 1 : 0, ReadName(read),
                     U8(now).c_str(), LookupName(look), spent ? 1 : 0, secondFinds ? 1 : 0,
                     RecordStateName(after.state));
            const bool pass = wrote && onDisk.state == cd::CudaRecordState::Ok && onDisk.rows.size() == 1 &&
                              guard && r.outcome == cd::CudaOutcome::Written && r.profileDeleted &&
                              read == cd::CudaRead::Failed && now.empty() &&
                              look == cd::CudaLookup::Absent && spent && !secondFinds &&
                              after.state == cd::CudaRecordState::Ok;
            Record("S9 spent",
                   "outcome=Written, the entry is gone with its setting, and a second Remove finds nothing "
                   "to do",
                   buf, pass);
            go = pass;
        } else {
            Record("S9 spent",
                   "outcome=Written, the entry is gone with its setting, and a second Remove finds nothing "
                   "to do",
                   "not run: the scenario before it did not leave the driver where this one starts", false);
        }

        // S12 - THE ENTRY IS ALREADY GONE (E3, third case). S9 took it away, so a line still naming it is
        // exactly the shape a real Remove meets after somebody deleted the entry in NVIDIA's Control Panel.
        // The state the line asks for is already true, so it is ABSENT and not a failure.
        //
        // 🔴 IT ALSO MEASURES readSetting'S OWN ANSWER ON A PROFILE THE DRIVER WILL NOT RESOLVE, which is
        // the only one of readSetting's three Failed routes a probe can provoke: the other two - a
        // NvAPI_DRS_GetSetting that answers something other than OK or SETTING_NOT_FOUND, and a CUDA
        // setting stored with a type that is not a wide string - CANNOT be produced from outside the
        // driver, so they stay with the fake CudaOps in tests\gpu_panel_regression.cpp.
        Log("\n--- S12: the settings entry is already gone ---\n");
        if (go) {
            cd::CudaRecordRow rec;
            rec.profileName = kDummyProfile;   // a name this probe owns, and one the driver no longer has
            rec.appEntry = kDummyAppKey;
            rec.lastWrote = target.value;
            rec.when = L"probe";
            const bool guard = OwnProfile(rec.profileName);
            cd::CudaRead direct = cd::CudaRead::Failed;
            std::wstring readValue;
            cd::CudaRowResult r;
            cd::CudaResolved resolved;
            bool ran = false;
            {
                cd::CudaOps ops = cd::MakeCudaOps();
                if (guard && ops.available) {
                    ran = true;
                    direct = ops.readSetting(kDummyProfile, readValue);
                    resolved = cd::ResolveCudaProfile(kDummyExe, ops);
                    r = cd::RestoreCudaForRow(kDummyExe, rec, ops);
                }
                ops.close();
            }
            snprintf(buf, sizeof buf,
                     "readSetting=%s value=\"%s\"; the resolver answers %s; outcome=%s refusal=%s",
                     ReadName(direct), U8(readValue).c_str(), LookupName(resolved.state),
                     OutcomeName(r.outcome), RefusalName(r.refusal));
            const bool pass = ran && direct == cd::CudaRead::Failed && readValue.empty() &&
                              resolved.state == cd::CudaLookup::Absent &&
                              r.outcome == cd::CudaOutcome::Absent && r.refusal == cd::CudaRefusal::None;
            Record("S12 already gone",
                   "readSetting=Failed, the resolver says Absent, outcome=Absent refusal=None - nothing to "
                   "take away, and not a failure",
                   buf, pass);
            go = pass;
        } else {
            Record("S12 already gone",
                   "readSetting=Failed, the resolver says Absent, outcome=Absent refusal=None - nothing to "
                   "take away, and not a failure",
                   "not run: the scenario before it did not leave the driver where this one starts", false);
        }

        // S13 - 🔴 THE ROUND-2 BLOCKER, ON THE REAL DRIVER. Assign the application to GPU A, assign it again
        // to GPU B, then Remove. The per-run journal wrote one file per Apply and Remove compared the driver
        // against the OLDEST of them, so the SECOND Apply's own value read as somebody else's change: a
        // conflict that never happened, the CUDA setting left pinned, and - the GPU assignment being gone by
        // then - a Remove button greyed out, so the undo the founder asked for could not even be retried.
        // One line per application, carrying the value this product LAST wrote, is what makes the compare
        // see B and succeed.
        //
        // Each Apply RE-READS THE RECORD FROM DISK first, because that is what a second run of the product
        // does. Keeping it in memory between the two would measure something no user ever meets.
        //
        // The state before the product was "NVIDIA had no settings entry for this application at all", so
        // putting it back means the entry is gone again - which is the state the cleanup and section 6
        // compare against.
        Log("\n--- S13: Apply to GPU A, Apply to GPU B, then Remove ---\n");
        if (go && targetB.act) {
            cd::CudaRecord rec = cd::ReadCudaRecord(abDir);
            const cd::CudaRecordState freshState = rec.state;
            cd::CudaRowResult a, b;
            bool preOk = false;
            {
                cd::CudaOps ops = cd::MakeCudaOps();
                if (ops.available) {
                    // The same read-only pre-flight S2 does: if the driver already resolves the dummy to a
                    // profile, nothing is written and the row is reported rather than guessed at.
                    const cd::CudaRowPlan pre = cd::PlanCudaForRow(kDummyExe, target, ops, rec);
                    preOk = !pre.profileExists;
                    if (preOk) {
                        a = cd::WriteCudaForRow(kDummyExe, target, ops, rec, RecordInto(rec));
                    } else {
                        Log("S13 setup: REFUSED, the driver already resolves the dummy to '%s'\n",
                            U8(pre.profileName).c_str());
                    }
                }
                ops.close();
            }
            const cd::CudaRecordRow* afterA = cd::CudaLineFor(rec, kDummyExe);
            const bool aOk = a.outcome == cd::CudaOutcome::Written && OwnProfile(a.profileName) &&
                             afterA != nullptr && rec.rows.size() == 1;
            const std::wstring wroteA = afterA != nullptr ? afterA->lastWrote : std::wstring();
            // 🔴 THE SECOND APPLY MUST UPDATE THE ENTRY IT MADE, NOT MAKE ANOTHER ONE. The count of
            // profiles in the database is taken with our entry standing, before and after the second
            // Apply, and our entry is confirmed to be ours at both points - see ProfileCount for why that
            // pair of facts is the question and the record is not able to answer it.
            NvU32 totalA = 0, totalB = 0;
            bool oursA = false, oursB = false, counted = false;
            if (aOk && OpenSession("S13: count the entries after the first Apply")) {
                counted = ProfileCount(totalA);
                oursA = FindOwnProfile(true) != nullptr;
                CloseSession();
            }
            cd::CudaRecord second = cd::ReadCudaRecord(abDir);
            {
                cd::CudaOps ops = cd::MakeCudaOps();
                if (aOk && ops.available)
                    b = cd::WriteCudaForRow(kDummyExe, targetB, ops, second, RecordInto(second));
                ops.close();
            }
            if (counted && OpenSession("S13: count them again after the second")) {
                counted = ProfileCount(totalB);
                oursB = FindOwnProfile(true) != nullptr;
                CloseSession();
            }
            const cd::CudaRecord onDisk = cd::ReadCudaRecord(abDir);
            const cd::CudaRecordRow* line = cd::CudaLineFor(onDisk, kDummyExe);
            cd::CudaRowResult r;
            cd::CudaRead read = cd::CudaRead::Failed;
            std::wstring now;
            cd::CudaProfile leftover;
            cd::CudaLookup look = cd::CudaLookup::Failed;
            const bool guard = line != nullptr && OwnProfile(line->profileName);
            {
                // A THIRD SESSION: this is Remove, and the only thing it is given is the line off the disk.
                cd::CudaOps ops = cd::MakeCudaOps();
                if (guard && ops.available) {
                    r = cd::RestoreCudaForRow(kDummyExe, *line, ops);
                    read = ops.readSetting(kDummyProfile, now);
                    look = ops.findProfileForExe(kDummyExe, leftover);
                }
                ops.close();
            }
            cd::CudaRecord spending = onDisk;
            const bool spent =
                r.outcome == cd::CudaOutcome::Written && cd::ForgetCudaRecordRow(spending, kDummyAppKey);
            snprintf(buf, sizeof buf,
                     "fresh=%s; A: outcome=%s lastWrote=\"%s\"; B: outcome=%s rows=%zu lastWrote=\"%s\"; "
                     "profiles in the database %lu -> %lu with our entry standing %d -> %d (counted=%d); "
                     "restore: outcome=%s refusal=%s profileDeleted=%d, it reads back %s \"%s\", the "
                     "driver answers %s; spent=%d",
                     RecordStateName(freshState), OutcomeName(a.outcome), U8(wroteA).c_str(),
                     OutcomeName(b.outcome), onDisk.rows.size(),
                     line != nullptr ? U8(line->lastWrote).c_str() : "",
                     static_cast<unsigned long>(totalA), static_cast<unsigned long>(totalB), oursA ? 1 : 0,
                     oursB ? 1 : 0, counted ? 1 : 0, OutcomeName(r.outcome), RefusalName(r.refusal),
                     r.profileDeleted ? 1 : 0, ReadName(read), U8(now).c_str(), LookupName(look),
                     spent ? 1 : 0);
            const bool pass = freshState == cd::CudaRecordState::Missing && preOk && aOk &&
                              wroteA == target.value && b.outcome == cd::CudaOutcome::Written &&
                              onDisk.rows.size() == 1 && line != nullptr &&
                              line->lastWrote == targetB.value && counted && oursA && oursB &&
                              totalA == totalB && guard &&
                              r.outcome == cd::CudaOutcome::Written && r.refusal == cd::CudaRefusal::None &&
                              r.profileDeleted && read == cd::CudaRead::Failed && now.empty() &&
                              look == cd::CudaLookup::Absent && spent;
            Record("S13 two applies",
                   "ONE line and ONE entry, the second Apply moves the value the entry holds without making "
                   "a second entry, and the undo takes the entry away with NO conflict",
                   buf, pass);
            go = pass;
        } else {
            Record("S13 two applies",
                   "ONE line and ONE entry, the second Apply moves the value the entry holds without making "
                   "a second entry, and the undo takes the entry away with NO conflict",
                   go ? "not run: this machine's driver offered no second value to assign to"
                      : "not run: the scenario before it did not leave the driver where this one starts",
                   false);
            go = false;
        }

        // S18 - 🔴 R5-1 ON THE REAL DRIVER, AND IT IS ROUND 4's BLOCKER. NVIDIA keeps EVERY per-application
        // setting in ONE profile, so the entry the product makes is also where NVIDIA Control Panel puts
        // that application's other settings. Remove deleted the whole profile on four guards - predefined,
        // one application, that application ours, membership readable - and NVDRS_PROFILE::numOfSettings sat
        // in the struct unread, so those settings went with it. Here the probe plays the user: Apply, then a
        // SECOND setting written through the probe's own session, then Remove. The CUDA setting must go and
        // NOTHING ELSE - the entry, its application and the other setting all still standing.
        Log("\n--- S18: an entry holding a setting this product did not write is never deleted (R5-1) ---\n");
        if (go) {
            cd::CudaRecord rec = cd::ReadCudaRecord(otherDir);
            cd::CudaRowResult made;
            bool preOk = false;
            {
                cd::CudaOps ops = cd::MakeCudaOps();
                if (ops.available) {
                    const cd::CudaRowPlan pre = cd::PlanCudaForRow(kDummyExe, target, ops, rec);
                    preOk = !pre.profileExists;
                    if (preOk) made = cd::WriteCudaForRow(kDummyExe, target, ops, rec, RecordInto(rec));
                    else Log("S18 setup: REFUSED, the driver already resolves the dummy to '%s'\n",
                             U8(pre.profileName).c_str());
                }
                ops.close();
            }
            const bool madeOk = made.outcome == cd::CudaOutcome::Written && OwnProfile(made.profileName);
            // The user, in NVIDIA's own Control Panel: a second setting on the same entry.
            bool added = false;
            size_t appsBefore = 0;
            NvU32 settingsBefore = 0;
            bool cudaBeforeClear = false, otherBefore = false;
            if (madeOk && OpenSession("S18: the user's own second setting")) {
                added = SetOwnOtherSetting();
                DummyProfileState(appsBefore, settingsBefore, cudaBeforeClear, otherBefore);
                CloseSession();
            }
            // Remove, through the product's own restore, off the line the product itself wrote.
            const cd::CudaRecord onDisk = cd::ReadCudaRecord(otherDir);
            const cd::CudaRecordRow* line = cd::CudaLineFor(onDisk, kDummyExe);
            const bool guard = line != nullptr && OwnProfile(line->profileName);
            cd::CudaRowResult r;
            {
                cd::CudaOps ops = cd::MakeCudaOps();
                if (added && guard && ops.available) r = cd::RestoreCudaForRow(kDummyExe, *line, ops);
                ops.close();
            }
            size_t appsAfter = 0;
            NvU32 settingsAfter = 0;
            bool cudaAfterClear = true, otherAfter = false;
            bool readAfter = false;
            if (OpenSession("S18: what the entry holds after the restore")) {
                readAfter = DummyProfileState(appsAfter, settingsAfter, cudaAfterClear, otherAfter);
                CloseSession();
            }
            // The probe takes its own leftover away: the entry is still there ON PURPOSE, which is the whole
            // finding, so the scenario that made it removes it rather than leaving it for section 5.
            bool tidied = false;
            if (OpenSession("S18: take the probe's own entry away again")) {
                tidied = CleanUpProfile(kDummyProfile);
                CloseSession();
            }
            snprintf(buf, sizeof buf,
                     "Apply: outcome=%s; the user's second setting added=%d, the entry then held %lu "
                     "setting(s) over %zu application(s); Remove: outcome=%s refusal=%s settingCleared=%d "
                     "profileDeleted=%d; the entry NOW holds %lu setting(s) over %zu application(s), CUDA=%d "
                     "other=%d (read=%d); probe tidied=%d",
                     OutcomeName(made.outcome), added ? 1 : 0,
                     static_cast<unsigned long>(settingsBefore), appsBefore, OutcomeName(r.outcome),
                     RefusalName(r.refusal), r.settingCleared ? 1 : 0, r.profileDeleted ? 1 : 0,
                     static_cast<unsigned long>(settingsAfter), appsAfter, cudaAfterClear ? 1 : 0,
                     otherAfter ? 1 : 0, readAfter ? 1 : 0, tidied ? 1 : 0);
            const bool pass = preOk && madeOk && added && settingsBefore == 2 && appsBefore == 1 &&
                              cudaBeforeClear && otherBefore && guard &&
                              r.outcome == cd::CudaOutcome::Written && r.refusal == cd::CudaRefusal::None &&
                              r.settingCleared && !r.profileDeleted && readAfter && settingsAfter == 1 &&
                              appsAfter == 1 && !cudaAfterClear && otherAfter && tidied;
            Record("S18 other setting",
                   "outcome=Written with settingCleared=1 and profileDeleted=0 - the CUDA setting is gone "
                   "and the entry, its application and the user's own setting are all still there",
                   buf, pass);
            go = pass;
        } else {
            Record("S18 other setting",
                   "outcome=Written with settingCleared=1 and profileDeleted=0 - the CUDA setting is gone "
                   "and the entry, its application and the user's own setting are all still there",
                   "not run: the scenario before it did not leave the driver where this one starts", false);
        }

        // S19 - 🔴 R5-2 ON THE REAL DRIVER, AND IT IS ROUND 4's OTHER HIGH. An entry carrying the product's
        // OWN NAME that covers somebody else's application and not the dummy's: the driver's lookup for the
        // dummy therefore answers Absent, which is the branch that used to go straight to CreateApplication
        // with no GetProfileInfo at all and then report otherApps = 0 as a constant. A record line of ours
        // names that entry, so the old code would have adopted it and written CUDA for the OTHER program.
        Log("\n--- S19: an entry of our own name that another program joined is refused (R5-2) ---\n");
        if (go) {
            bool setUp = false;
            if (OpenSession("S19: make an entry of our name around somebody else's application")) {
                setUp = MakeNamedProfileWithNeighbour();
                CloseSession();
            }
            // The line of ours that claims that entry - which is the ONLY thing that used to be asked.
            cd::CudaRecord rec = cd::ReadCudaRecord(adoptDir);
            cd::CudaRecordRow row;
            row.profileName = kDummyProfile;
            row.appEntry = kDummyAppKey;
            row.lastWrote = target.value;
            const bool recorded = cd::SaveCudaRecordRow(rec, row);
            cd::CudaRecord claim = cd::ReadCudaRecord(adoptDir);
            cd::CudaRowResult r;
            cd::CudaLookup look = cd::CudaLookup::Failed;
            bool ran = false;
            {
                cd::CudaOps ops = cd::MakeCudaOps();
                if (setUp && recorded && ops.available) {
                    ran = true;
                    cd::CudaProfile before;
                    look = ops.findProfileForExe(kDummyExe, before);
                    r = cd::WriteCudaForRow(kDummyExe, target, ops, claim, RecordInto(claim));
                }
                ops.close();
            }
            const std::wstring says = cd::CudaRowRefusalText(r);
            size_t apps = 0;
            NvU32 settings = 0;
            bool hasCuda = true, hasOther = false, readBack = false;
            if (OpenSession("S19: what that entry holds after the refusal")) {
                readBack = DummyProfileState(apps, settings, hasCuda, hasOther);
                CloseSession();
            }
            bool tidied = false;
            if (OpenSession("S19: take the probe's own entry away again")) {
                tidied = CleanUpProfile(kDummyProfile);
                CloseSession();
            }
            snprintf(buf, sizeof buf,
                     "set up=%d; the lookup for the dummy answers %s; outcome=%s refusal=%s profile='%s' "
                     "otherApps=%zu; it says: %s; the entry still covers %zu application(s) and holds %lu "
                     "setting(s), CUDA=%d (read=%d); probe tidied=%d",
                     setUp ? 1 : 0, LookupName(look), OutcomeName(r.outcome), RefusalName(r.refusal),
                     U8(r.profileName).c_str(), r.otherApps, U8(says).c_str(), apps,
                     static_cast<unsigned long>(settings), hasCuda ? 1 : 0, readBack ? 1 : 0, tidied ? 1 : 0);
            const bool pass = setUp && recorded && ran && look == cd::CudaLookup::Absent &&
                              r.outcome == cd::CudaOutcome::Refused &&
                              r.refusal == cd::CudaRefusal::NvidiaManagesIt && r.profileName == kDummyProfile &&
                              r.otherApps == 1 && says.find(kDummyProfile) != std::wstring::npos &&
                              says.find(L"1 other program as well") != std::wstring::npos && readBack &&
                              apps == 1 && settings == 0 && !hasCuda && tidied;
            Record("S19 adopt refused",
                   "outcome=Refused refusal=NvidiaManagesIt naming the entry and counting ONE other program, "
                   "with nothing written into it and the dummy still not in it",
                   buf, pass);
            go = pass;
        } else {
            Record("S19 adopt refused",
                   "outcome=Refused refusal=NvidiaManagesIt naming the entry and counting ONE other program, "
                   "with nothing written into it and the dummy still not in it",
                   "not run: the scenario before it did not leave the driver where this one starts", false);
        }

        // S20 - 🔴 R5-3 ON THE REAL DRIVER, AND IT IS THE FINDING THE OPERATOR WOULD HAVE MET FIRST. A row
        // whose Windows GPU pin ALREADY equals what Apply intends is AlreadyDone - which is every application
        // v0.5.6 and v0.5.7 pinned - and the CUDA half was gated on Done alone, so the whole feature did
        // nothing at all for them, in silence. This drives cd::RunGpuEdits, where that gate lives.
        //
        // 🔴 NOT ONE REGISTRY VALUE IS READ OR WRITTEN. GpuEditOps takes every Windows call as a function
        // object; the three below are in memory, and `wrote` proves the write was never even asked for.
        Log("\n--- S20: a row already pinned to the target still gets its CUDA entry (R5-3) ---\n");
        if (go) {
            const std::wstring pinned = cd::MergeGpuPreferenceValue(std::wstring(), nvTargetKey);
            std::vector<cd::GpuEditItem> items;
            cd::GpuEditItem item;
            item.exePath = kDummyExe;
            item.present = true;
            item.existing = pinned;            // exactly what this Apply would write: nothing to do
            items.push_back(item);
            cd::CudaRecord rec = cd::ReadCudaRecord(gateDir);
            cd::CudaRowResult cudaRow;
            int wrote = 0, asked = 0;
            bool preOk = false;
            std::vector<cd::GpuEditResult> out;
            {
                cd::CudaOps ops = cd::MakeCudaOps();
                if (ops.available) {
                    const cd::CudaRowPlan pre = cd::PlanCudaForRow(kDummyExe, target, ops, rec);
                    preOk = !pre.profileExists;
                }
                if (preOk) {
                    cd::GpuEditOps edit;
                    edit.write = [&wrote](const std::wstring&, bool, const std::wstring&, bool,
                                          const std::wstring&, unsigned long&, bool) {
                        ++wrote;   // 🔴 must never happen: an AlreadyDone row writes nothing
                        return cd::GuardedWriteResult::Written;
                    };
                    edit.read = [&pinned](const std::wstring&, std::wstring& value, bool& unreadable) {
                        unreadable = false;
                        value = pinned;
                        return true;
                    };
                    edit.record = [](const cd::GpuPreferenceBefore&) { return true; };
                    edit.cudaRow = [&](size_t, std::wstring&) {
                        ++asked;
                        if (ops.available)
                            cudaRow = cd::WriteCudaForRow(kDummyExe, target, ops, rec, RecordInto(rec));
                        return cd::GpuRowGate::Go;
                    };
                    out = cd::RunGpuEdits(items, false, nvTargetKey, edit);
                }
                ops.close();
            }
            // What the DRIVER holds, not what the row answered.
            std::wstring live;
            cd::CudaRead read = cd::CudaRead::Failed;
            {
                cd::CudaOps ops = cd::MakeCudaOps();
                if (ops.available && OwnProfile(kDummyProfile)) read = ops.readSetting(kDummyProfile, live);
                ops.close();
            }
            // Put it back the way every other scenario does: through the product's own restore.
            const cd::CudaRecord onDisk = cd::ReadCudaRecord(gateDir);
            const cd::CudaRecordRow* line = cd::CudaLineFor(onDisk, kDummyExe);
            cd::CudaRowResult back;
            cd::CudaLookup gone = cd::CudaLookup::Failed;
            {
                cd::CudaOps ops = cd::MakeCudaOps();
                if (line != nullptr && OwnProfile(line->profileName) && ops.available) {
                    back = cd::RestoreCudaForRow(kDummyExe, *line, ops);
                    cd::CudaProfile leftover;
                    gone = ops.findProfileForExe(kDummyExe, leftover);
                }
                ops.close();
            }
            snprintf(buf, sizeof buf,
                     "the row's Windows outcome=%s, the registry write was asked for %d time(s), the CUDA "
                     "half was asked %d time(s); CUDA: outcome=%s refusal=%s; the driver reads back %s "
                     "\"%s\"; put back: outcome=%s profileDeleted=%d and the driver then answers %s",
                     out.empty() ? "none"
                                 : (out[0].outcome == cd::GpuEditOutcome::AlreadyDone ? "AlreadyDone"
                                                                                      : "something else"),
                     wrote, asked, OutcomeName(cudaRow.outcome), RefusalName(cudaRow.refusal), ReadName(read),
                     U8(live).c_str(), OutcomeName(back.outcome), back.profileDeleted ? 1 : 0,
                     LookupName(gone));
            const bool pass = preOk && out.size() == 1 &&
                              out[0].outcome == cd::GpuEditOutcome::AlreadyDone && wrote == 0 && asked == 1 &&
                              cudaRow.outcome == cd::CudaOutcome::Written && read == cd::CudaRead::Value &&
                              live == target.value && back.outcome == cd::CudaOutcome::Written &&
                              back.profileDeleted && gone == cd::CudaLookup::Absent;
            Record("S20 already pinned",
                   "the row is AlreadyDone, nothing is written to Windows, and the CUDA entry is made all "
                   "the same - then taken away again",
                   buf, pass);
            go = pass;
        } else {
            Record("S20 already pinned",
                   "the row is AlreadyDone, nothing is written to Windows, and the CUDA entry is made all "
                   "the same - then taken away again",
                   "not run: the scenario before it did not leave the driver where this one starts", false);
        }

        // S21 - 🔴 R6-1 ON THE REAL DRIVER, AND IT IS THE ROUND-5 DEFECT BOTH COUNCIL SEATS FOUND.
        // Round 4's blocker was fixed with NVDRS_PROFILE::numOfSettings > 1 - a COUNT standing in for an
        // IDENTITY, which is precisely the mistake round 4 had already proved for APPLICATIONS. An entry of
        // ours carrying ONE setting that is the USER'S counts one, and went whole.
        //
        // This arranges exactly that state on the real driver: Apply, the probe adds a second setting as
        // the user would, Remove takes only ours out (R5-1) - and the entry is then left holding ONE
        // setting that is not ours at all.
        //
        // 🔴 AND IT IS ALSO THE ONE MEASUREMENT THIS ROUND COULD NOT MAKE. NvAPI_DRS_EnumSettings'
        // interface id is [A] - taken from NVIDIA's headers and never resolved on this machine - and
        // whether NvAPI_DRS_GetSetting answers for a profile that has no such setting of its own is
        // untested. Both are PRINTED here whatever they turn out to be: `enumOffered`, the ids the driver
        // lists, and what the read answers.
        Log("\n--- S21: an entry holding ONE setting that is NOT ours is never deleted (R6-1) ---\n");
        if (go) {
            cd::CudaRecord rec = cd::ReadCudaRecord(notOursDir);
            cd::CudaRowResult made;
            bool preOk = false, enumOffered = false;
            {
                cd::CudaOps ops = cd::MakeCudaOps();
                enumOffered = ops.listSettingIds != nullptr;   // 🔴 R6-2: EMPTY when the driver has no such call
                if (ops.available) {
                    const cd::CudaRowPlan pre = cd::PlanCudaForRow(kDummyExe, target, ops, rec);
                    preOk = !pre.profileExists;
                    if (preOk) made = cd::WriteCudaForRow(kDummyExe, target, ops, rec, RecordInto(rec));
                    else Log("S21 setup: REFUSED, the driver already resolves the dummy to '%s'\n",
                             U8(pre.profileName).c_str());
                }
                ops.close();
            }
            const bool madeOk = made.outcome == cd::CudaOutcome::Written && OwnProfile(made.profileName);
            bool added = false;
            if (madeOk && OpenSession("S21: the user's own second setting")) {
                added = SetOwnOtherSetting();
                CloseSession();
            }
            // Remove, which must take OURS out and leave the entry standing (R5-1) - and only then is the
            // entry in the state this scenario is about.
            const cd::CudaRecord afterApply = cd::ReadCudaRecord(notOursDir);
            const cd::CudaRecordRow* line = cd::CudaLineFor(afterApply, kDummyExe);
            const bool guard = line != nullptr && OwnProfile(line->profileName);
            cd::CudaRecordRow realLine;
            cd::CudaRowResult cleared;
            {
                cd::CudaOps ops = cd::MakeCudaOps();
                if (added && guard && ops.available) {
                    realLine = *line;
                    cleared = cd::RestoreCudaForRow(kDummyExe, realLine, ops);
                }
                ops.close();
            }
            // 🔴 WHAT THE DRIVER NOW SAYS ABOUT THAT ENTRY, BOTH WAYS. This is the measurement: the ids it
            // lists, and whether a CUDA read still answers with a value it does not hold of its own.
            std::vector<unsigned long> ids;
            bool listed = false, ourIdThere = false;
            cd::CudaRead read = cd::CudaRead::Failed;
            std::wstring live;
            {
                cd::CudaOps ops = cd::MakeCudaOps();
                if (ops.available && OwnProfile(kDummyProfile)) {
                    if (ops.listSettingIds) listed = ops.listSettingIds(kDummyProfile, ids);
                    for (size_t i = 0; i < ids.size(); ++i)
                        if (ids[i] == cd::CudaSettingId()) ourIdThere = true;
                    read = ops.readSetting(kDummyProfile, live);
                }
                ops.close();
            }
            std::string idText;
            for (size_t i = 0; i < ids.size() && i < 8; ++i) {
                char one[24];
                snprintf(one, sizeof one, "%s0x%08lX", i ? "," : "", ids[i]);
                idText += one;
            }
            if (idText.empty()) idText = "(none)";
            // 🔴 THE RULE: A REMOVE AIMED AT THAT ENTRY WITH A LINE CLAIMING OUR OLD VALUE MUST REFUSE AND
            // MUST NOT DELETE IT. Whichever way the read answers, the entry and the user's setting stand.
            cd::CudaRowResult again;
            {
                cd::CudaOps ops = cd::MakeCudaOps();
                if (guard && ops.available) again = cd::RestoreCudaForRow(kDummyExe, realLine, ops);
                ops.close();
            }
            // 🔴 R6-7: AND THE LINE THE CLEAR-ONLY RESTORE LEAVES BEHIND - "we made this entry and there is
            // nothing of ours in it now" - takes nothing away and says so.
            cd::CudaRecord kept = cd::ReadCudaRecord(notOursDir);
            cd::CudaRecordRow nothingRow = realLine;
            nothingRow.lastWrote = cd::CudaNothingWritten();
            nothingRow.when.clear();
            const bool keptOk = cd::SaveCudaRecordRow(kept, nothingRow);
            const cd::CudaRecord reread = cd::ReadCudaRecord(notOursDir);
            const cd::CudaRecordRow* keptLine = cd::CudaLineFor(reread, kDummyExe);
            const bool keptRead = keptLine != nullptr && cd::CudaRowHoldsNothingOfOurs(*keptLine);
            cd::CudaRowResult nothing;
            {
                cd::CudaOps ops = cd::MakeCudaOps();
                if (keptRead && OwnProfile(keptLine->profileName) && ops.available)
                    nothing = cd::RestoreCudaForRow(kDummyExe, *keptLine, ops);
                ops.close();
            }
            size_t apps = 0;
            NvU32 settings = 0;
            bool hasCuda = true, hasOther = false, readBack = false;
            if (OpenSession("S21: what the entry holds at the end")) {
                readBack = DummyProfileState(apps, settings, hasCuda, hasOther);
                CloseSession();
            }
            bool tidied = false;
            if (OpenSession("S21: take the probe's own entry away again")) {
                tidied = CleanUpProfile(kDummyProfile);
                CloseSession();
            }
            snprintf(buf, sizeof buf,
                     "enumSettings offered=%d; Apply=%s, the user's setting added=%d, the clear-only "
                     "Remove=%s settingCleared=%d profileDeleted=%d; the entry's OWN setting ids are now "
                     "%s (listed=%d, ours among them=%d) and a CUDA read answers %s \"%s\"; a Remove "
                     "claiming the old value: outcome=%s refusal=%s profileDeleted=%d; the \"nothing of "
                     "ours\" line: written=%d read back=%d outcome=%s refusal=%s; the entry NOW holds %lu "
                     "setting(s) over %zu application(s), CUDA=%d other=%d (read=%d); probe tidied=%d",
                     enumOffered ? 1 : 0, OutcomeName(made.outcome), added ? 1 : 0,
                     OutcomeName(cleared.outcome), cleared.settingCleared ? 1 : 0,
                     cleared.profileDeleted ? 1 : 0, idText.c_str(), listed ? 1 : 0, ourIdThere ? 1 : 0,
                     ReadName(read), U8(live).c_str(), OutcomeName(again.outcome), RefusalName(again.refusal),
                     again.profileDeleted ? 1 : 0, keptOk ? 1 : 0, keptRead ? 1 : 0,
                     OutcomeName(nothing.outcome), RefusalName(nothing.refusal),
                     static_cast<unsigned long>(settings), apps, hasCuda ? 1 : 0, hasOther ? 1 : 0,
                     readBack ? 1 : 0, tidied ? 1 : 0);
            // The pass criterion is the RULE, not the measurement: whichever way the read and the
            // enumeration answer, that entry is not deleted and the user's setting is still in it.
            const bool refusedRight = again.outcome == cd::CudaOutcome::Refused &&
                                      (again.refusal == cd::CudaRefusal::SettingNotOurs ||
                                       again.refusal == cd::CudaRefusal::ChangedSinceWritten);
            const bool pass = preOk && madeOk && added && enumOffered && guard &&
                              cleared.outcome == cd::CudaOutcome::Written && cleared.settingCleared &&
                              !cleared.profileDeleted && listed && !ourIdThere && refusedRight &&
                              !again.profileDeleted && keptOk && keptRead &&
                              nothing.outcome == cd::CudaOutcome::NothingOfOurs &&
                              nothing.refusal == cd::CudaRefusal::None && !nothing.profileDeleted &&
                              readBack && settings == 1 && apps == 1 && !hasCuda && hasOther && tidied;
            Record("S21 not ours",
                   "the entry is left holding ONE setting that is not ours, a Remove aimed at it is "
                   "REFUSED and deletes nothing, and a \"nothing of ours\" line takes nothing away",
                   buf, pass);
            go = pass;
        } else {
            Record("S21 not ours",
                   "the entry is left holding ONE setting that is not ours, a Remove aimed at it is "
                   "REFUSED and deletes nothing, and a \"nothing of ours\" line takes nothing away",
                   "not run: the scenario before it did not leave the driver where this one starts", false);
        }

        // S22 - 🔴 R6-3 ON THE REAL DRIVER. The commonest way to a leftover: the record file is GONE - the
        // user deleted a text file whose own header says it is only a note - and an entry this product made
        // is still in the driver. Until round 6 Remove never opened the session at all in that state, so
        // the Windows pin went and the NVIDIA entry stayed for good, in silence.
        //
        // 🔴 WHAT THIS PROBE CAN AND CANNOT REACH. The decision "open the session anyway, name it, never
        // block the removal" lives in OpenCudaRun and CudaAfterRow in src\gpuwindow.cpp, which needs a
        // panel and a row list - tests\gpu_panel_regression.cpp drives those end to end. What the probe can
        // settle, and nothing else can, is that against the REAL driver a record of NO lines plus an entry
        // of ours really does resolve to an entry the product recognises as its own, so there is something
        // for that sentence to name. Nothing is written here at all.
        Log("\n--- S22: an entry of ours with NO record line at all is found and named (R6-3) ---\n");
        if (go) {
            cd::CudaRecord rec = cd::ReadCudaRecord(orphanDir);
            cd::CudaRowResult made;
            bool preOk = false;
            {
                cd::CudaOps ops = cd::MakeCudaOps();
                if (ops.available) {
                    const cd::CudaRowPlan pre = cd::PlanCudaForRow(kDummyExe, target, ops, rec);
                    preOk = !pre.profileExists;
                    if (preOk) made = cd::WriteCudaForRow(kDummyExe, target, ops, rec, RecordInto(rec));
                }
                ops.close();
            }
            const bool madeOk = made.outcome == cd::CudaOutcome::Written && OwnProfile(made.profileName);
            // The user deletes the note. The entry stays exactly where it is.
            ClearRecordFolder(orphanDir);
            const cd::CudaRecord gone = cd::ReadCudaRecord(orphanDir);
            const bool noLine = gone.state == cd::CudaRecordState::Missing && gone.rows.empty() &&
                                cd::CudaLineFor(gone, kDummyExe) == nullptr;
            // What Remove's third way does: open the session and LOOK. Read-only, start to finish.
            cd::CudaResolved found;
            bool opened = false, ours = false;
            {
                cd::CudaOps ops = cd::MakeCudaOps();
                opened = ops.available;
                if (opened) found = cd::ResolveCudaProfile(kDummyExe, ops);
                ops.close();
            }
            ours = found.state == cd::CudaLookup::Found && cd::IsCudaProfileWeMade(found.profileName);
            const std::wstring says = ours ? cd::FormatCudaOrphanEntryLine(found.profileName) : std::wstring();
            size_t apps = 0;
            NvU32 settings = 0;
            bool hasCuda = false, hasOther = false, readBack = false;
            if (OpenSession("S22: the entry is untouched by the look")) {
                readBack = DummyProfileState(apps, settings, hasCuda, hasOther);
                CloseSession();
            }
            bool tidied = false;
            if (OpenSession("S22: take the probe's own entry away again")) {
                tidied = CleanUpProfile(kDummyProfile);
                CloseSession();
            }
            snprintf(buf, sizeof buf,
                     "Apply=%s; the note was deleted and reads %s with %zu line(s) and no line for the "
                     "dummy=%d; the look: session opened=%d lookup=%s profile='%s' ours=%d; it says: %s; "
                     "the entry is UNTOUCHED - %lu setting(s) over %zu application(s), CUDA=%d (read=%d); "
                     "probe tidied=%d",
                     OutcomeName(made.outcome),
                     gone.state == cd::CudaRecordState::Missing ? "Missing" : "something else",
                     gone.rows.size(), noLine ? 1 : 0, opened ? 1 : 0, LookupName(found.state),
                     U8(found.profileName).c_str(), ours ? 1 : 0, U8(says).c_str(),
                     static_cast<unsigned long>(settings), apps, hasCuda ? 1 : 0, readBack ? 1 : 0,
                     tidied ? 1 : 0);
            const bool pass = preOk && madeOk && noLine && opened && ours &&
                              found.profileName == kDummyProfile &&
                              says.find(kDummyProfile) != std::wstring::npos &&
                              says.find(L"NVIDIA Control Panel") != std::wstring::npos && readBack &&
                              settings == 1 && apps == 1 && hasCuda && tidied;
            Record("S22 no record",
                   "with the note gone the entry is still resolved, recognised as one Game Optimizer made, "
                   "NAMED in a sentence that sends the user to NVIDIA Control Panel, and left untouched",
                   buf, pass);
            go = pass;
        } else {
            Record("S22 no record",
                   "with the note gone the entry is still resolved, recognised as one Game Optimizer made, "
                   "NAMED in a sentence that sends the user to NVIDIA Control Panel, and left untouched",
                   "not run: the scenario before it did not leave the driver where this one starts", false);
        }

        // S23 - 🔴 A MEASUREMENT, NOT A RULE. Nobody has ever measured whether NvAPI's profile lookup is
        // case-sensitive, and this product compares profile names WITHOUT case everywhere (R5-10) on the
        // strength of an assumption: that a user who renames a profile in NVIDIA Control Panel, or a driver
        // that answers in another case, would otherwise make an entry of ours unrecognisable.
        //
        // 🔴 IT PASSES WHATEVER THE DRIVER ANSWERS, and it must: an answer that surprises us is the point
        // of running it, not a failure of the product. What it may NOT do is change anything, so every call
        // here is a lookup and the profile it makes is the probe's own dummy, removed at the end.
        Log("\n--- S23: is NvAPI's profile lookup case-sensitive? A measurement (round 6) ---\n");
        if (go) {
            cd::CudaRecord rec = cd::ReadCudaRecord(orphanDir);
            cd::CudaRowResult made;
            bool preOk = false;
            {
                cd::CudaOps ops = cd::MakeCudaOps();
                if (ops.available) {
                    const cd::CudaRowPlan pre = cd::PlanCudaForRow(kDummyExe, target, ops, rec);
                    preOk = !pre.profileExists;
                    if (preOk) made = cd::WriteCudaForRow(kDummyExe, target, ops, rec, RecordInto(rec));
                }
                ops.close();
            }
            const bool madeOk = made.outcome == cd::CudaOutcome::Written && OwnProfile(made.profileName);
            std::wstring upper = kDummyProfile, lower = kDummyProfile;
            for (size_t i = 0; i < upper.size(); ++i) upper[i] = static_cast<wchar_t>(towupper(upper[i]));
            for (size_t i = 0; i < lower.size(); ++i) lower[i] = static_cast<wchar_t>(towlower(lower[i]));
            cd::CudaLookup exact = cd::CudaLookup::Failed;
            cd::CudaLookup asUpper = cd::CudaLookup::Failed;
            cd::CudaLookup asLower = cd::CudaLookup::Failed;
            std::wstring upperAnswered, lowerAnswered;
            {
                cd::CudaOps ops = cd::MakeCudaOps();
                if (madeOk && ops.available && ops.findProfileByName) {
                    cd::CudaProfile p;
                    exact = ops.findProfileByName(kDummyProfile, p);
                    cd::CudaProfile u;
                    asUpper = ops.findProfileByName(upper, u);
                    upperAnswered = u.profileName;
                    cd::CudaProfile l;
                    asLower = ops.findProfileByName(lower, l);
                    lowerAnswered = l.profileName;
                }
                ops.close();
            }
            bool tidied = false;
            if (OpenSession("S23: take the probe's own entry away again")) {
                tidied = CleanUpProfile(kDummyProfile);
                CloseSession();
            }
            const char* verdict = (asUpper == cd::CudaLookup::Found || asLower == cd::CudaLookup::Found)
                                      ? "CASE-INSENSITIVE (a differently-cased name found it)"
                                      : "CASE-SENSITIVE (only the exact name found it)";
            snprintf(buf, sizeof buf,
                     "Apply=%s; the exact name answers %s; UPPER CASE answers %s (driver said '%s'); lower "
                     "case answers %s (driver said '%s'); so NvAPI_DRS_FindProfileByName is %s on this "
                     "driver; probe tidied=%d",
                     OutcomeName(made.outcome), LookupName(exact), LookupName(asUpper),
                     U8(upperAnswered).c_str(), LookupName(asLower), U8(lowerAnswered).c_str(), verdict,
                     tidied ? 1 : 0);
            // 🔴 THE ONLY THING THAT COULD FAIL HERE IS THE MEASUREMENT NOT BEING MADE. The driver's answer
            // is recorded, never judged: this scenario exists to put a number where an assumption was.
            const bool pass = preOk && madeOk && exact == cd::CudaLookup::Found &&
                              asUpper != cd::CudaLookup::Failed && asLower != cd::CudaLookup::Failed &&
                              tidied;
            Record("S23 name case",
                   "the lookup is made three ways - exact, upper and lower - and whatever the driver "
                   "answers is recorded; nothing is written and the dummy is removed",
                   buf, pass);
            go = pass;
        } else {
            Record("S23 name case",
                   "the lookup is made three ways - exact, upper and lower - and whatever the driver "
                   "answers is recorded; nothing is written and the dummy is removed",
                   "not run: the scenario before it did not leave the driver where this one starts", false);
        }

        // S24 - 🔴 R6-12 ON THE REAL DRIVER, AND IT IS S23's MEASUREMENT TURNED INTO A RULE. S23 measured
        // that NvAPI_DRS_FindProfileByName is CASE-SENSITIVE. Every ownership comparison in this product is
        // IEquals and stays that way (R5-10), so an executable path Windows spells differently than when the
        // entry was made still reads as OURS - while a profile name RECOMPUTED from that path misses the
        // entry in the driver. The adopt path used to recompute it, so it made a SECOND entry differing from
        // the first only in case: one no record line claims, which CudaOwnershipRefusal then answers "NVIDIA
        // manages this one" to forever. That is the trap R6-7 was built to close, through another door.
        //
        // 🔴 THE STATE THIS NEEDS, AND WHY IT TAKES THREE STEPS. The defect is only reachable when the
        // driver's lookup for the EXECUTABLE answers Absent while a line of ours names an entry that is
        // really there - because cd::CudaAppKeyFor lower-cases, so while the application entry is still in
        // the profile the driver finds it under either spelling and the adopt path is never taken at all.
        // So: make the entry through the PRODUCT, take the application out through a session of the probe's
        // own (what a user removing it in NVIDIA Control Panel leaves behind), then drive a second Apply
        // with the path in UPPER CASE.
        //
        // WHAT IT MEASURES, and the profile COUNT is the half nothing else can say: the record reads the
        // same either way (one line per application is what the record IS), and FindProfileByName hands back
        // one handle however many profiles match. The total standing still, while our entry is there before
        // and after, is the whole question.
        Log("\n--- S24: a second Apply with the path in UPPER CASE adopts the SAME entry (R6-12) ---\n");
        if (go) {
            cd::CudaRecord rec = cd::ReadCudaRecord(caseDir);
            cd::CudaRowResult made;
            bool preOk = false;
            {
                cd::CudaOps ops = cd::MakeCudaOps();
                if (ops.available) {
                    const cd::CudaRowPlan pre = cd::PlanCudaForRow(kDummyExe, target, ops, rec);
                    preOk = !pre.profileExists;
                    if (preOk) made = cd::WriteCudaForRow(kDummyExe, target, ops, rec, RecordInto(rec));
                }
                ops.close();
            }
            const bool madeOk = made.outcome == cd::CudaOutcome::Written && OwnProfile(made.profileName) &&
                                made.profileName == kDummyProfile;
            // The application comes out. The entry - and the CUDA value in it - stay exactly where they are.
            bool stripped = false, counted = false, stillThere = false, upperBefore = true;
            NvU32 totalBefore = 0;
            if (OpenSession("S24: take the application out and leave the entry standing")) {
                stripped = madeOk && StripDummyApplication();
                stillThere = ProfileKnownByName(kDummyProfile);
                upperBefore = ProfileKnownByName(kDummyProfileUpper);
                counted = ProfileCount(totalBefore);
                CloseSession();
            }
            // The line of ours still names the entry, spelled as it was MADE - and it is still the line for
            // this application, because the application entry is the lower-case path either way.
            const cd::CudaRecordRow* claim = cd::CudaLineFor(rec, kDummyExeUpper);
            const bool lineNamesIt = claim != nullptr && claim->profileName == kDummyProfile;
            cd::CudaRowResult again;
            cd::CudaLookup look = cd::CudaLookup::Failed;
            bool ran = false;
            {
                cd::CudaOps ops = cd::MakeCudaOps();
                if (madeOk && stripped && stillThere && lineNamesIt && ops.available) {
                    ran = true;
                    cd::CudaProfile before;
                    look = ops.findProfileForExe(kDummyExeUpper, before);
                    again = cd::WriteCudaForRow(kDummyExeUpper, target, ops, rec, RecordInto(rec));
                }
                ops.close();
            }
            NvU32 totalAfter = 0;
            bool countedAfter = false, upperAfter = true, readBack = false;
            size_t apps = 0;
            NvU32 settings = 0;
            bool hasCuda = false, hasOther = false;
            if (OpenSession("S24: exactly ONE entry, and it is the one that was already there")) {
                countedAfter = ProfileCount(totalAfter);
                upperAfter = ProfileKnownByName(kDummyProfileUpper);
                readBack = DummyProfileState(apps, settings, hasCuda, hasOther);
                CloseSession();
            }
            bool tidied = false, tidiedUpper = false;
            if (OpenSession("S24: take the probe's own entry away again")) {
                tidied = CleanUpProfile(kDummyProfile);
                // 🔴 AND THE DIFFERENTLY-CASED NAME TOO. With the rule holding this is always "absent -
                // nothing to remove"; it is here for the run where it is NOT, which is exactly the run that
                // must not leave a second entry behind in the operator's driver.
                tidiedUpper = CleanUpProfile(kDummyProfileUpper);
                CloseSession();
            }
            snprintf(buf, sizeof buf,
                     "Apply='%s' outcome=%s; the application was taken out=%d and the entry is still "
                     "there=%d; the driver then held %lu profile(s) and '%s'=%d; the line still names "
                     "'%s'=%d; the UPPER CASE Apply: lookup=%s outcome=%s refusal=%s profile='%s' "
                     "hadPrevious=%d; the driver NOW holds %lu profile(s) and '%s'=%d; our entry covers "
                     "%zu application(s) and holds %lu setting(s), CUDA=%d (read=%d); probe tidied=%d/%d",
                     U8(made.profileName).c_str(), OutcomeName(made.outcome), stripped ? 1 : 0,
                     stillThere ? 1 : 0, static_cast<unsigned long>(totalBefore),
                     U8(kDummyProfileUpper).c_str(), upperBefore ? 1 : 0, U8(kDummyProfile).c_str(),
                     lineNamesIt ? 1 : 0, LookupName(look), OutcomeName(again.outcome),
                     RefusalName(again.refusal), U8(again.profileName).c_str(), again.hadPrevious ? 1 : 0,
                     static_cast<unsigned long>(totalAfter), U8(kDummyProfileUpper).c_str(),
                     upperAfter ? 1 : 0, apps, static_cast<unsigned long>(settings), hasCuda ? 1 : 0,
                     readBack ? 1 : 0, tidied ? 1 : 0, tidiedUpper ? 1 : 0);
            const bool pass = preOk && madeOk && stripped && stillThere && !upperBefore && counted &&
                              lineNamesIt && ran && look == cd::CudaLookup::Absent &&
                              again.outcome == cd::CudaOutcome::Written &&
                              again.refusal == cd::CudaRefusal::None &&
                              again.profileName == kDummyProfile && again.hadPrevious &&
                              // 🔴 THE TWO THAT ARE THE RULE: no second entry under the cased name, and the
                              // database holds exactly as many profiles as it did before the second Apply.
                              !upperAfter && countedAfter && totalAfter == totalBefore && readBack &&
                              apps == 1 && settings == 1 && hasCuda && !hasOther && tidied && tidiedUpper;
            Record("S24 cased path",
                   "the UPPER CASE Apply ADOPTS the entry the record names - outcome=Written naming that "
                   "same entry - and the driver still holds exactly ONE entry for the application, with no "
                   "second one differing only in case",
                   buf, pass);
            go = pass;
        } else {
            Record("S24 cased path",
                   "the UPPER CASE Apply ADOPTS the entry the record names - outcome=Written naming that "
                   "same entry - and the driver still holds exactly ONE entry for the application, with no "
                   "second one differing only in case",
                   "not run: the scenario before it did not leave the driver where this one starts", false);
        }
    }

    for (size_t i = 0; i < g_scn.size(); ++i)
        if (!g_scn[i].pass) ++unexpected;

    // ---- 5. remove the dummy profile ------------------------------------------------------------
    cleanupOk = CleanUp();

    // ---- 6. the CUDA-profile dump, after --------------------------------------------------------
    Log("\n=== 6. every profile carrying 0x10354FF8, after ===\n");
    std::string dumpAfter;
    int cudaAfter = -1;
    bool dumpOk = false;
    if (g_sess != nullptr || OpenSession("dump-after")) dumpOk = DumpCudaProfiles(dumpAfter, cudaAfter);
    if (!dumpOk) Log("*** the dump after the run FAILED: %s\n", dumpAfter.c_str());
    WriteTextFile(workDir + L"\\cuda-profiles-after.txt", dumpAfter);
    const unsigned long long dumpAfterHash = Fnv(dumpAfter.data(), dumpAfter.size());
    Log("%s", dumpAfter.c_str());
    Log("DUMP_AFTER_FINGERPRINT=#%016llx  (%s)\n", dumpAfterHash, U8(workDir + L"\\cuda-profiles-after.txt").c_str());
    CloseSession();

    const bool dumpMatch = dumpOk && dumpAfter == dumpBefore;
    if (!dumpMatch) {
        // Fail loudly, and say WHERE: the first line that differs, from both sides.
        size_t a = 0, b = 0;
        int lineNo = 1;
        while (a < dumpBefore.size() && b < dumpAfter.size()) {
            const size_t ea = dumpBefore.find('\n', a), eb = dumpAfter.find('\n', b);
            const std::string la = dumpBefore.substr(a, (ea == std::string::npos ? dumpBefore.size() : ea) - a);
            const std::string lb = dumpAfter.substr(b, (eb == std::string::npos ? dumpAfter.size() : eb) - b);
            if (la != lb) {
                Log("*** FIRST DIFFERENCE at line %d:\n  before: %s\n  after : %s\n", lineNo, la.c_str(), lb.c_str());
                break;
            }
            if (ea == std::string::npos || eb == std::string::npos) break;
            a = ea + 1;
            b = eb + 1;
            ++lineNo;
        }
    }

    // ---- 7. the database files on disk, for information only -------------------------------------
    //
    // 🔴 THE DUMMY NAME IS EXPECTED TO SURVIVE IN ONE OF THESE FILES, AND THAT IS NOT A LEAK. [M]
    // 2026-09-19: nvdrsdb0.bin and nvdrsdb1.bin are a PAIR and nvdrssel.bin's single byte says which one
    // is current (00 = nvdrsdb0). Each NvAPI_DRS_SaveSettings writes the slot that is not in use, so the
    // other slot is simply the PREVIOUS generation of the database - and one generation back, the probe's
    // profile was still there. It is not read while nvdrssel.bin points elsewhere, and the next settings
    // change anyone makes overwrites it.
    // This prints which slot is current and which slot still carries the name, so nobody has to rediscover
    // that from scratch and mistake it for something the cleanup failed to do. The CURRENT slot carrying
    // the name WOULD be a real finding; the other slot carrying it is housekeeping.
    unsigned long long liveAfter = 1469598103934665603ULL;
    std::vector<size_t> nameHits(3, 0);
    // S17's neighbour, counted separately: it can only be in the file if S17's foreign entry is, so it is
    // the one string that proves that entry went away without depending on the dummy's own name at all.
    std::vector<size_t> nbrHits(3, 0);
    std::vector<size_t> fileSizes(3, 0);
    int selects = -1;
    for (int i = 0; i < 3; ++i) {
        std::vector<unsigned char> bytes;
        if (!ReadWholeFile(std::wstring(kDrsDir) + L"\\" + kDbFiles[i], bytes)) continue;
        const unsigned long long h = Fnv(bytes.empty() ? "" : reinterpret_cast<const char*>(&bytes[0]), bytes.size());
        liveAfter = Fnv(&h, sizeof h, liveAfter);
        fileSizes[i] = bytes.size();
        nameHits[i] = CountWide(bytes, kDummyBaseName);
        nbrHits[i] = CountWide(bytes, kForeignNeighbourBase);
        if (i == 2 && bytes.size() == 1) selects = bytes[0];
    }
    Log("\n=== 7. the driver's database files on disk, after the run (information only) ===\n");
    Log("nvdrssel.bin selects: %s\n",
        selects == 0 ? "nvdrsdb0.bin" : (selects == 1 ? "nvdrsdb1.bin" : "unreadable"));
    for (int i = 0; i < 3; ++i) {
        const char* what = (i == 2) ? "   (the selector itself)"
                                    : (selects == i ? "   <- CURRENT"
                                                    : "   (the previous generation; not read while "
                                                      "nvdrssel.bin points elsewhere)");
        Log("  %-14s %9zu bytes  holds '%s' %zu time(s) and '%s' %zu time(s)%s\n", U8(kDbFiles[i]).c_str(),
            fileSizes[i], U8(kDummyBaseName).c_str(), nameHits[i], U8(kForeignNeighbourBase).c_str(),
            nbrHits[i], what);
    }
    const bool currentClean =
        selects < 0 || selects > 1 || (nameHits[selects] == 0 && nbrHits[selects] == 0);
    Log("CURRENT_DB_CLEAN=%s   (the name left in the OTHER slot is the previous generation, not a leak -\n"
        "    see the note above section 7 in tests\\cuda_assign_probe.cpp)\n", currentClean ? "True" : "False");

    // ---- the summary ----------------------------------------------------------------------------
    Log("\n=== SUMMARY ===\n");
    Log("%-16s %-12s %s\n", "scenario", "verdict", "observed");
    for (size_t i = 0; i < g_scn.size(); ++i)
        Log("%-16s %-12s %s\n", g_scn[i].name.c_str(), g_scn[i].pass ? "AS_EXPECTED" : "UNEXPECTED",
            g_scn[i].observed.c_str());
    Log("\nPROFILES_WITH_CUDA_BEFORE=%d\nPROFILES_WITH_CUDA_AFTER=%d\n", cudaBefore, cudaAfter);
    Log("FINGERPRINT_BEFORE=#%016llx\nFINGERPRINT_AFTER=#%016llx\nFINGERPRINT_MATCH=%s"
        "   (NvAPI_DRS_SaveSettings rewrites the whole file, so this is NOT expected to match and is NOT\n"
        "    a pass criterion - DUMP_MATCH is)\n",
        liveBefore, liveAfter, liveBefore == liveAfter ? "True" : "False");
    Log("DUMP_MATCH=%s\nCLEANUP=%s\nCURRENT_DB_CLEAN=%s\nUNEXPECTED_SCENARIOS=%d of %zu\n",
        dumpMatch ? "True" : "False", cleanupOk ? "OK" : "FAILED", currentClean ? "True" : "False", unexpected,
        g_scn.size());

    int rc = 0;
    if (!cleanupOk) rc = 4;
    else if (!dumpMatch || unexpected > 0 || !currentClean) rc = 1;
    if (rc != 0) PrintRecovery(workDir);
    Log("PROBE_EXIT=%d\n", rc);
    return rc;
}
